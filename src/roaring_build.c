#include "pg_roaring_index.h"

#include "access/tableam.h"
#include "access/xloginsert.h"
#include "catalog/pg_type_d.h"
#include "commands/progress.h"
#include "pgstat.h"
#include "storage/checksum.h"
#include "storage/smgr.h"
#include "utils/hsearch.h"
#include "utils/rel.h"

/* ----------------------------------------------------------------
 * Internal types
 * ---------------------------------------------------------------- */

typedef struct RoaringBuildEntry
{
	int64				value;		/* hash key — must be first */
	roaring_bitmap_t   *bitmap;
} RoaringBuildEntry;

typedef struct RoaringBuildState
{
	double	heap_tuples;
	double	index_tuples;
	HTAB   *bitmaps;
} RoaringBuildState;

/* ----------------------------------------------------------------
 * roaring_build_callback
 * ---------------------------------------------------------------- */
static void
roaring_build_callback(Relation index, ItemPointer tid, Datum *values,
					   bool *isnull, bool tupleIsAlive, void *state)
{
	RoaringBuildState  *bstate = (RoaringBuildState *) state;
	int64				value;
	RoaringBuildEntry  *entry;
	bool				found;

	bstate->heap_tuples++;

	if (isnull[0])
		return;

	value = (TupleDescAttr(index->rd_att, 0)->atttypid == INT4OID)
			? (int64) DatumGetInt32(values[0])
			: DatumGetInt64(values[0]);

	entry = (RoaringBuildEntry *) hash_search(bstate->bitmaps, &value,
											   HASH_ENTER, &found);
	if (!found)
	{
		entry->bitmap = roaring_bitmap_create();
		bstate->index_tuples++;
	}

	roaring_bitmap_add(entry->bitmap,
					   ((uint32) ItemPointerGetBlockNumber(tid) << 9) |
					   (uint32)(ItemPointerGetOffsetNumber(tid) - 1));
}

static int
cmp_build_entry(const void *a, const void *b)
{
	int64 va = (*(const RoaringBuildEntry *const *) a)->value;
	int64 vb = (*(const RoaringBuildEntry *const *) b)->value;
	return (va > vb) - (va < vb);
}


/* ----------------------------------------------------------------
 * write_metapage
 *
 * Rewrites block 0 (previously zeroed-out) with real metapage data.
 * ---------------------------------------------------------------- */
static void
write_metapage(Relation index,
			   BlockNumber root_dir,
			   BlockNumber leftmost_leaf,
			   BlockNumber rightmost_leaf,
			   BlockNumber pending_insert,
			   BlockNumber pending_delete,
			   uint32 total_entries)
{
	Buffer				buf;
	Page				page;
	RoaringMetaPageData *meta;

	buf  = ReadBuffer(index, ROARING_METAPAGE_BLKNO);
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	page = BufferGetPage(buf);

	PageInit(page, BLCKSZ, 0);
	meta = RoaringPageGetMeta(page);
	memset(meta, 0, sizeof(*meta));

	meta->magic					 = ROARING_MAGIC;
	meta->version				 = ROARING_INDEX_VERSION;
	meta->flags					 = ROARING_FLAG_EXACT;
	meta->root_directory_page	 = root_dir;
	meta->leftmost_leaf_page	 = leftmost_leaf;
	meta->rightmost_leaf_page	 = rightmost_leaf;
	meta->pending_insert_head	 = pending_insert;
	meta->pending_insert_tail	 = pending_insert;
	meta->pending_insert_count	 = 0;
	meta->pending_merging_head	 = InvalidBlockNumber;
	meta->pending_delete_head	 = pending_delete;
	meta->pending_delete_tail	 = pending_delete;
	meta->pending_delete_count	 = 0;
	meta->tombstone_root_page	 = InvalidBlockNumber;
	meta->total_entries			 = total_entries;
	meta->pending_merge_threshold = ROARING_PENDING_MERGE_THRESHOLD;

	/*
	 * Set pd_lower past the metapage data so GenericXLog's mask_unused_space
	 * does not zero our fields when computing WAL deltas.
	 */
	((PageHeader) page)->pd_lower =
		(LocationIndex)(SizeOfPageHeaderData + sizeof(RoaringMetaPageData));

	roaring_wal_and_release(index, buf);
}


/* ----------------------------------------------------------------
 * write_leaf_and_dir_pages
 *
 * Writes sorted entries as leaf pages, then builds the directory
 * (1-level if ≤ max_dir leaf pages, 2-level otherwise).
 * Sets *root_dir_out = InvalidBlockNumber if nentries == 0.
 * ---------------------------------------------------------------- */
static void
write_leaf_and_dir_pages(Relation index,
						  RoaringBuildEntry **sorted, long nentries,
						  BlockNumber *root_dir_out,
						  BlockNumber *leftmost_out,
						  BlockNumber *rightmost_out)
{
	/*
	 * Max bitmap payload that fits inline on a fresh leaf page.
	 * PageGetFreeSpace on a fresh page = (BLCKSZ - special) - header - ItemIdData
	 * Then subtract the fixed RoaringLeafEntry header.
	 */
	const int max_inline = (int)(BLCKSZ
								 - MAXALIGN(sizeof(RoaringLeafSpecial))
								 - SizeOfPageHeaderData
								 - sizeof(ItemIdData)
								 - MAXALIGN(sizeof(RoaringLeafEntry)));

	/* Flat-array dir capacity per page (no line pointers). */
	const uint32 max_dir = (uint32)((BLCKSZ
									 - MAXALIGN(sizeof(RoaringDirSpecial))
									 - SizeOfPageHeaderData)
									/ sizeof(RoaringDirEntry));

	/* leaf_entries: one entry per leaf page written */
	RoaringDirEntry	   *leaf_entries;
	uint32				leaf_count = 0;

	Buffer				leaf_buf  = InvalidBuffer;
	Page				leaf_page = NULL;
	RoaringLeafSpecial *leaf_spc  = NULL;
	BlockNumber			leftmost  = InvalidBlockNumber;
	BlockNumber			rightmost = InvalidBlockNumber;

	long i;

	if (nentries == 0)
	{
		*root_dir_out  = InvalidBlockNumber;
		*leftmost_out  = InvalidBlockNumber;
		*rightmost_out = InvalidBlockNumber;
		return;
	}

	/* Worst case: one leaf page per value. */
	leaf_entries = palloc(nentries * sizeof(RoaringDirEntry));

	/* ---- Phase A: write leaf pages ---- */
	for (i = 0; i < nentries; i++)
	{
		RoaringBuildEntry *be = sorted[i];
		size_t			   bitmap_size;
		Size			   entry_size;
		RoaringLeafEntry  *le;

		roaring_bitmap_run_optimize(be->bitmap);
		bitmap_size = roaring_bitmap_portable_size_in_bytes(be->bitmap);

		if ((int) bitmap_size > max_inline)
			entry_size = MAXALIGN(sizeof(RoaringOverflowEntry));
		else
			entry_size = MAXALIGN(sizeof(RoaringLeafEntry) + bitmap_size);

		/* ---- transition to new leaf page if needed ---- */
		if (leaf_buf == InvalidBuffer ||
			PageGetFreeSpace(leaf_page) < entry_size)
		{
			if (leaf_buf != InvalidBuffer)
			{
				OffsetNumber	 maxoff;
				RoaringLeafEntry *last;
				BlockNumber		 old_blkno;
				Buffer			 new_buf;
				BlockNumber		 new_blkno;

				maxoff	  = PageGetMaxOffsetNumber(leaf_page);
				last	  = (RoaringLeafEntry *)
							PageGetItem(leaf_page,
										PageGetItemId(leaf_page, maxoff));
				old_blkno = BufferGetBlockNumber(leaf_buf);

				leaf_entries[leaf_count].high_key   = last->value;
				leaf_entries[leaf_count].child_page = old_blkno;
				leaf_count++;

				new_buf	  = roaring_extend_page(index);
				new_blkno = BufferGetBlockNumber(new_buf);

				leaf_spc->right_page = new_blkno;
				roaring_wal_and_release(index, leaf_buf);

				leaf_buf  = new_buf;
				leaf_page = BufferGetPage(leaf_buf);
				PageInit(leaf_page, BLCKSZ, sizeof(RoaringLeafSpecial));
				leaf_spc  = (RoaringLeafSpecial *)
							PageGetSpecialPointer(leaf_page);
				leaf_spc->page_type	  = ROARING_PAGE_LEAF;
				leaf_spc->flags		  = 0;
				leaf_spc->entry_count = 0;
				leaf_spc->left_page	  = old_blkno;
				leaf_spc->right_page  = InvalidBlockNumber;
			}
			else
			{
				leaf_buf  = roaring_extend_page(index);
				leaf_page = BufferGetPage(leaf_buf);
				PageInit(leaf_page, BLCKSZ, sizeof(RoaringLeafSpecial));
				leaf_spc  = (RoaringLeafSpecial *)
							PageGetSpecialPointer(leaf_page);
				leaf_spc->page_type	  = ROARING_PAGE_LEAF;
				leaf_spc->flags		  = 0;
				leaf_spc->entry_count = 0;
				leaf_spc->left_page	  = InvalidBlockNumber;
				leaf_spc->right_page  = InvalidBlockNumber;
				leftmost = BufferGetBlockNumber(leaf_buf);
			}
		}

		if ((int) bitmap_size > max_inline)
		{
			char				 *bm_data;
			size_t				  pfx_len;
			RoaringOverflowEntry *oe;

			bm_data	   = (char *) palloc(bitmap_size);
			roaring_bitmap_portable_serialize(be->bitmap, bm_data);
			pfx_len	   = Min(ROARING_OVERFLOW_INLINE_BYTES, bitmap_size);
			oe		   = (RoaringOverflowEntry *) palloc(sizeof(RoaringOverflowEntry));
			oe->value		   = be->value;
			oe->cardinality	   = (uint32) roaring_bitmap_get_cardinality(be->bitmap);
			oe->flags		   = ROARING_ENTRY_OVERFLOW;
			oe->total_len	   = (uint32) bitmap_size;
			oe->overflow_blkno = roaring_write_overflow_chain(index, bm_data,
															   bitmap_size, pfx_len);
			memcpy(oe->inline_prefix, bm_data, pfx_len);
			pfree(bm_data);

			if (PageAddItem(leaf_page, (Item) oe,
							sizeof(RoaringOverflowEntry),
							InvalidOffsetNumber, false, false) == InvalidOffsetNumber)
				elog(ERROR, "roaring_build: PageAddItem failed unexpectedly");
			pfree(oe);
		}
		else
		{
			le = (RoaringLeafEntry *) palloc(sizeof(RoaringLeafEntry) + bitmap_size);
			le->value		= be->value;
			le->cardinality = (uint32) roaring_bitmap_get_cardinality(be->bitmap);
			le->flags		= ROARING_ENTRY_INLINE;
			roaring_bitmap_portable_serialize(be->bitmap, (char *)(le + 1));

			if (PageAddItem(leaf_page, (Item) le,
							sizeof(RoaringLeafEntry) + bitmap_size,
							InvalidOffsetNumber, false, false) == InvalidOffsetNumber)
				elog(ERROR, "roaring_build: PageAddItem failed unexpectedly");
			pfree(le);
		}

		leaf_spc->entry_count++;

		roaring_bitmap_free(be->bitmap);
		be->bitmap = NULL;
	}

	/* Finalize last leaf page. */
	if (leaf_buf != InvalidBuffer)
	{
		OffsetNumber	 maxoff;
		RoaringLeafEntry *last;
		BlockNumber		 blkno;

		blkno  = BufferGetBlockNumber(leaf_buf);
		maxoff = PageGetMaxOffsetNumber(leaf_page);
		last   = (RoaringLeafEntry *)
				 PageGetItem(leaf_page, PageGetItemId(leaf_page, maxoff));

		leaf_entries[leaf_count].high_key   = last->value;
		leaf_entries[leaf_count].child_page = blkno;
		leaf_count++;

		rightmost = blkno;
		roaring_wal_and_release(index, leaf_buf);
	}

	/* ---- Phase B: build directory ---- */
	if (leaf_count <= max_dir)
	{
		/* Single-level: root points directly to leaf pages. */
		*root_dir_out = roaring_write_dir_page(index, leaf_entries, leaf_count, 0);
	}
	else
	{
		/*
		 * Two-level: batch leaf_entries into level-1 dir pages, then
		 * write a root (level 0) page pointing to the level-1 pages.
		 *
		 * For 10K distinct values this is ~2 level-1 pages; the root
		 * comfortably fits in one page (max_dir ≈ 679 entries).
		 */
		uint32			 l1_count;
		RoaringDirEntry *l1_entries;
		uint32			 j;

		l1_count  = (leaf_count + max_dir - 1) / max_dir; /* ceil division */
		l1_entries = palloc(l1_count * sizeof(RoaringDirEntry));

		for (j = 0; j < l1_count; j++)
		{
			uint32		start = j * max_dir;
			uint32		count = Min(max_dir, leaf_count - start);
			BlockNumber blkno;

			/*
			 * Level-1 pages (level=0): their children are leaf pages.
			 * Convention: level=0 means "children are leaf pages";
			 *             level=N>0 means "children are dir pages at level N-1".
			 */
			blkno = roaring_write_dir_page(index, leaf_entries + start, count, 0);
			l1_entries[j].high_key   = leaf_entries[start + count - 1].high_key;
			l1_entries[j].child_page = blkno;
		}

		if (l1_count > max_dir)
			elog(ERROR,
				 "roaring_build: index too large for two-level directory "
				 "(%u leaf pages); three-level directory not yet implemented",
				 leaf_count);

		/* Root (level=1): children are dir pages at level 0. */
		*root_dir_out = roaring_write_dir_page(index, l1_entries, l1_count, 1);
		pfree(l1_entries);
	}

	pfree(leaf_entries);
	*leftmost_out  = leftmost;
	*rightmost_out = rightmost;
}

/* ================================================================
 * roaring_build
 * ================================================================ */
IndexBuildResult *
roaring_build(Relation heap, Relation index, struct IndexInfo *indexInfo)
{
	IndexBuildResult   *result;
	RoaringBuildState	bstate;
	HASHCTL				hashctl;
	double				reltuples;

	RoaringBuildEntry **sorted;
	long				nentries;
	HASH_SEQ_STATUS		seq;
	RoaringBuildEntry  *entry;
	long				i;

	BlockNumber			root_dir		= InvalidBlockNumber;
	BlockNumber			leftmost_leaf	= InvalidBlockNumber;
	BlockNumber			rightmost_leaf	= InvalidBlockNumber;
	BlockNumber			pending_insert;
	BlockNumber			pending_delete;

	result = (IndexBuildResult *) palloc0(sizeof(IndexBuildResult));

	/* Block 0: placeholder — filled in by write_metapage at the end. */
	{
		Buffer buf = roaring_extend_page(index);

		Assert(BufferGetBlockNumber(buf) == ROARING_METAPAGE_BLKNO);
		PageInit(BufferGetPage(buf), BLCKSZ, 0);
		roaring_wal_and_release(index, buf);
	}

	/* Heap scan. */
	memset(&hashctl, 0, sizeof(hashctl));
	hashctl.keysize   = sizeof(int64);
	hashctl.entrysize = sizeof(RoaringBuildEntry);
	bstate.bitmaps		= hash_create("roaring build bitmaps", 1024,
									  &hashctl, HASH_ELEM | HASH_BLOBS);
	bstate.heap_tuples  = 0;
	bstate.index_tuples = 0;

	reltuples = table_index_build_scan(heap, index, indexInfo,
									   true, true,
									   roaring_build_callback,
									   &bstate, NULL);

	/* Sort by value. */
	nentries = hash_get_num_entries(bstate.bitmaps);
	sorted   = (RoaringBuildEntry **)
			   palloc(nentries * sizeof(RoaringBuildEntry *));
	i = 0;
	hash_seq_init(&seq, bstate.bitmaps);
	while ((entry = (RoaringBuildEntry *) hash_seq_search(&seq)) != NULL)
		sorted[i++] = entry;
	qsort(sorted, nentries, sizeof(RoaringBuildEntry *), cmp_build_entry);

	/* Write leaf pages + directory. */
	write_leaf_and_dir_pages(index, sorted, nentries,
							 &root_dir, &leftmost_leaf, &rightmost_leaf);

	pfree(sorted);
	hash_destroy(bstate.bitmaps);

	/* Pending-list pages. */
	pending_insert = roaring_init_pending_page(index, ROARING_PAGE_PENDING_INSERT);
	pending_delete = roaring_init_pending_page(index, ROARING_PAGE_PENDING_DELETE);

	/* Write metapage into block 0. */
	write_metapage(index, root_dir, leftmost_leaf, rightmost_leaf,
				   pending_insert, pending_delete, (uint32) nentries);

	result->heap_tuples  = reltuples;
	result->index_tuples = bstate.index_tuples;
	return result;
}

/* ================================================================
 * roaring_buildempty
 *
 * Called by PostgreSQL to initialise the INIT fork of an UNLOGGED index.
 * The INIT fork is copied to the MAIN fork on crash recovery, so it must
 * contain a complete, valid empty index state.
 *
 * We cannot use the normal buffer-manager path (ReadBuffer, etc.) because
 * that always targets MAIN_FORKNUM.  Instead, write all three pages
 * directly to INIT_FORKNUM via smgrextend, WAL-log each with log_newpage,
 * then sync.  Pattern: contrib/bloom/blutils.c:blbuildempty.
 * ================================================================ */
void
roaring_buildempty(Relation index)
{
	SMgrRelation		 smgr				 = RelationGetSmgr(index);
	char				*buf				 = (char *) palloc(BLCKSZ);
	Page				 page				 = (Page) buf;
	RoaringMetaPageData *meta;
	RoaringPendingSpecial *spc;
	const BlockNumber	 pending_insert_blkno = 1;
	const BlockNumber	 pending_delete_blkno = 2;

	smgrcreate(smgr, INIT_FORKNUM, false);

	/* Page 0: metapage */
	PageInit(page, BLCKSZ, 0);
	meta = RoaringPageGetMeta(page);
	memset(meta, 0, sizeof(*meta));
	meta->magic					  = ROARING_MAGIC;
	meta->version				  = ROARING_INDEX_VERSION;
	meta->flags					  = ROARING_FLAG_EXACT;
	meta->root_directory_page	  = InvalidBlockNumber;
	meta->leftmost_leaf_page	  = InvalidBlockNumber;
	meta->rightmost_leaf_page	  = InvalidBlockNumber;
	meta->pending_insert_head	  = pending_insert_blkno;
	meta->pending_insert_tail	  = pending_insert_blkno;
	meta->pending_insert_count	  = 0;
	meta->pending_merging_head	  = InvalidBlockNumber;
	meta->pending_delete_head	  = pending_delete_blkno;
	meta->pending_delete_tail	  = pending_delete_blkno;
	meta->pending_delete_count	  = 0;
	meta->tombstone_root_page	  = InvalidBlockNumber;
	meta->total_entries			  = 0;
	meta->pending_merge_threshold = ROARING_PENDING_MERGE_THRESHOLD;
	((PageHeader) page)->pd_lower =
		(LocationIndex)(SizeOfPageHeaderData + sizeof(RoaringMetaPageData));
	PageSetChecksumInplace(page, ROARING_METAPAGE_BLKNO);
	smgrextend(smgr, INIT_FORKNUM, ROARING_METAPAGE_BLKNO, page, true);
	log_newpage(&smgr->smgr_rlocator.locator, INIT_FORKNUM,
				ROARING_METAPAGE_BLKNO, page, true);

	/* Page 1: pending insert */
	PageInit(page, BLCKSZ, sizeof(RoaringPendingSpecial));
	spc			   = (RoaringPendingSpecial *) PageGetSpecialPointer(page);
	spc->page_type	= ROARING_PAGE_PENDING_INSERT;
	spc->flags		= 0;
	spc->entry_count = 0;
	spc->next_page	= InvalidBlockNumber;
	spc->xmin_low	= InvalidTransactionId;
	PageSetChecksumInplace(page, pending_insert_blkno);
	smgrextend(smgr, INIT_FORKNUM, pending_insert_blkno, page, true);
	log_newpage(&smgr->smgr_rlocator.locator, INIT_FORKNUM,
				pending_insert_blkno, page, true);

	/* Page 2: pending delete */
	PageInit(page, BLCKSZ, sizeof(RoaringPendingSpecial));
	spc			   = (RoaringPendingSpecial *) PageGetSpecialPointer(page);
	spc->page_type	= ROARING_PAGE_PENDING_DELETE;
	spc->flags		= 0;
	spc->entry_count = 0;
	spc->next_page	= InvalidBlockNumber;
	spc->xmin_low	= InvalidTransactionId;
	PageSetChecksumInplace(page, pending_delete_blkno);
	smgrextend(smgr, INIT_FORKNUM, pending_delete_blkno, page, true);
	log_newpage(&smgr->smgr_rlocator.locator, INIT_FORKNUM,
				pending_delete_blkno, page, true);

	smgrimmedsync(smgr, INIT_FORKNUM);
	pfree(buf);
}
