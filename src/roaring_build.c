#include "pg_roaring_index.h"

#include <math.h>

#include "access/tableam.h"
#include "access/xloginsert.h"
#include "catalog/pg_type_d.h"
#include "commands/progress.h"
#include "pgstat.h"
#include "storage/checksum.h"
#include "storage/smgr.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/rel.h"

extern int maintenance_work_mem;

/* ----------------------------------------------------------------
 * Internal types
 * ---------------------------------------------------------------- */

/*
 * One (value, linearized_tid) pair collected during the heap scan.
 * After the scan the array is sorted by (value, tid) so value groups
 * are contiguous; each group's tids are in ascending order.
 */
typedef struct RoaringBuildTuple
{
	int64	value;
	uint64	tid;
} RoaringBuildTuple;

typedef struct RoaringBuildState
{
	double				heap_tuples;
	Oid					atttypid;   /* column 0 type: used only for single-column indexes */
	long				nalloc;
	long				ntuples;
	RoaringBuildTuple  *tuples;
} RoaringBuildState;

/*
 * Abort the build if doubling the tuple array would exceed maintenance_work_mem.
 * maintenance_work_mem is in kB; sizeof(RoaringBuildTuple) == 16 bytes.
 */
static void
check_build_mem_limit(long new_nalloc)
{
	long max_nalloc = ((long) maintenance_work_mem * 1024L) /
					  (long) sizeof(RoaringBuildTuple);

	if (new_nalloc > max_nalloc)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("roaring_build: index build exceeds maintenance_work_mem"),
				 errdetail("Tuple array requires %ld entries (%zu bytes each) but "
						   "maintenance_work_mem limits the build to %ld entries.",
						   new_nalloc, sizeof(RoaringBuildTuple), max_nalloc),
				 errhint("Increase maintenance_work_mem and retry.")));
}

/* ----------------------------------------------------------------
 * roaring_build_callback
 * ---------------------------------------------------------------- */
static void
roaring_build_callback(Relation index, ItemPointer tid, Datum *values,
					   bool *isnull, bool tupleIsAlive, void *state)
{
	RoaringBuildState  *bstate = (RoaringBuildState *) state;
	int					natts   = index->rd_att->natts;

	bstate->heap_tuples++;

	if (natts > 1)
	{
		/* Multi-column: emit one (attno-namespaced key, tid) entry per column. */
		int i;

		for (i = 0; i < natts; i++)
		{
			Oid		typid = TupleDescAttr(index->rd_att, i)->atttypid;
			int64	value;

			if (isnull[i])
				continue;

			if (typid == FLOAT4OID && isnan(DatumGetFloat4(values[i])))
				continue;			/* NaN is not equality-indexable */

			value = ROARING_COL_KEY(i + 1, roaring_datum_to_key32(values[i], typid));

			if (bstate->ntuples == bstate->nalloc)
			{
				long new_nalloc = bstate->nalloc * 2;
				check_build_mem_limit(new_nalloc);
				bstate->nalloc = new_nalloc;
				bstate->tuples  = (RoaringBuildTuple *)
								  repalloc_extended(bstate->tuples,
													bstate->nalloc * sizeof(RoaringBuildTuple),
													MCXT_ALLOC_HUGE);
			}

			bstate->tuples[bstate->ntuples].value = value;
			bstate->tuples[bstate->ntuples].tid   =
				((uint64) ItemPointerGetBlockNumber(tid) << 9) |
				(uint64)(ItemPointerGetOffsetNumber(tid) - 1);
			bstate->ntuples++;
		}
		return;
	}

	/* Single-column path. */
	{
		int64 value;

		if (isnull[0])
			return;

		if (bstate->atttypid == FLOAT4OID && isnan(DatumGetFloat4(values[0])))
			return;					/* NaN is not equality-indexable */

		value = roaring_datum_to_key64(values[0], bstate->atttypid);

		if (bstate->ntuples == bstate->nalloc)
		{
			long new_nalloc = bstate->nalloc * 2;
			check_build_mem_limit(new_nalloc);
			bstate->nalloc = new_nalloc;
			bstate->tuples  = (RoaringBuildTuple *)
							  repalloc_extended(bstate->tuples,
												bstate->nalloc * sizeof(RoaringBuildTuple),
												MCXT_ALLOC_HUGE);
		}

		bstate->tuples[bstate->ntuples].value = value;
		bstate->tuples[bstate->ntuples].tid   =
			((uint32) ItemPointerGetBlockNumber(tid) << 9) |
			(uint32)(ItemPointerGetOffsetNumber(tid) - 1);
		bstate->ntuples++;
	}
}

static int
cmp_build_tuple(const void *a, const void *b)
{
	const RoaringBuildTuple *ta = (const RoaringBuildTuple *) a;
	const RoaringBuildTuple *tb = (const RoaringBuildTuple *) b;

	if (ta->value != tb->value)
		return (ta->value > tb->value) - (ta->value < tb->value);
	return (ta->tid > tb->tid) - (ta->tid < tb->tid);
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
			   BlockNumber pending_blknos[ROARING_PENDING_SHARDS],
			   uint32 total_entries,
			   uint16 flags)
{
	Buffer				buf;
	Page				page;
	RoaringMetaPageData *meta;
	int					 i;

	buf  = ReadBuffer(index, ROARING_METAPAGE_BLKNO);
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	page = BufferGetPage(buf);

	PageInit(page, BLCKSZ, 0);
	meta = RoaringPageGetMeta(page);
	memset(meta, 0, sizeof(*meta));

	meta->magic					   = ROARING_MAGIC;
	meta->version				   = ROARING_INDEX_VERSION;
	meta->flags					   = flags;
	meta->croaring_format_version  = ROARING_EXPECTED_FORMAT_VERSION;
	meta->num_shards			   = ROARING_PENDING_SHARDS;
	meta->root_directory_page	   = root_dir;
	meta->leftmost_leaf_page	   = leftmost_leaf;
	meta->rightmost_leaf_page	   = rightmost_leaf;
	meta->free_list_head		   = InvalidBlockNumber;

	for (i = 0; i < ROARING_PENDING_SHARDS; i++)
	{
		meta->shards[i].insert_head  = pending_blknos[i];
		meta->shards[i].insert_tail  = pending_blknos[i];
		meta->shards[i].insert_count = 0;
		meta->shards[i].merging_head = InvalidBlockNumber;
		meta->shards[i].carry_head   = InvalidBlockNumber;
	}

	meta->total_entries			 = total_entries;
	meta->pending_merge_threshold = (uint32) roaring_pending_merge_threshold_guc;

	/*
	 * Set pd_lower past the metapage data so GenericXLog's mask_unused_space
	 * does not zero our fields when computing WAL deltas.
	 */
	((PageHeader) page)->pd_lower =
		(LocationIndex)(SizeOfPageHeaderData + sizeof(RoaringMetaPageData));

	roaring_wal_and_release(index, buf);
}


/*
 * Leaf pages are WAL-logged in batches of ROARING_BUILD_WAL_BATCH rather
 * than one log_newpage_buffer per page.  One log_newpages record covers the
 * whole batch, giving ~32× WAL reduction on large builds.
 */
#define ROARING_BUILD_WAL_BATCH 32

static void
flush_leaf_wal_batch(Relation index, Buffer *bufs, BlockNumber *blknos, int n)
{
	int i;

	if (n == 0)
		return;

	if (RelationNeedsWAL(index))
	{
		Page *pages = (Page *) palloc(n * sizeof(Page));

		for (i = 0; i < n; i++)
			pages[i] = BufferGetPage(bufs[i]);
		log_newpages(&index->rd_locator, MAIN_FORKNUM, n, blknos, pages, true);
		pfree(pages);
	}

	for (i = 0; i < n; i++)
		UnlockReleaseBuffer(bufs[i]);
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
						  RoaringBuildTuple *tuples, long ntuples,
						  bool is_lossy,
						  long *nentries_out,
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
	long				nentries   = 0;

	Buffer				leaf_buf  = InvalidBuffer;
	Page				leaf_page = NULL;
	RoaringLeafSpecial *leaf_spc  = NULL;
	BlockNumber			leftmost  = InvalidBlockNumber;
	BlockNumber			rightmost = InvalidBlockNumber;

	/* WAL batch for completed leaf pages */
	Buffer		leaf_batch_bufs[ROARING_BUILD_WAL_BATCH];
	BlockNumber	leaf_batch_blknos[ROARING_BUILD_WAL_BATCH];
	int			leaf_batch_n = 0;

	long i;

	Assert(max_dir < PG_UINT16_MAX); /* entry_count is uint16 */

	if (ntuples == 0)
	{
		*nentries_out  = 0;
		*root_dir_out  = InvalidBlockNumber;
		*leftmost_out  = InvalidBlockNumber;
		*rightmost_out = InvalidBlockNumber;
		return;
	}

	/*
	 * Pre-check: worst case is one leaf page per distinct value.  If ntuples
	 * (an upper bound on distinct values) already exceeds the three-level
	 * directory capacity, fail now before paying the full build cost.
	 */
	if (ntuples > (long) max_dir * (long) max_dir * (long) max_dir)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("roaring_build: index too large for three-level directory"),
				 errdetail("Distinct-value estimate %ld exceeds capacity %ld.",
						   ntuples, (long) max_dir * (long) max_dir * (long) max_dir),
				 errhint("Reduce the number of indexed distinct values, "
						 "or use REINDEX after reducing cardinality.")));

	/* Worst case: one leaf page per distinct value (≤ ntuples). */
	leaf_entries = palloc_extended(ntuples * sizeof(RoaringDirEntry),
								   MCXT_ALLOC_HUGE);

	/* ---- Phase A: write leaf pages ---- */
	i = 0;
	while (i < ntuples)
	{
		int64			   cur_value  = tuples[i].value;
		long			   group_end  = i + 1;
		long			   gc;
		long			   gi;
		size_t			   bitmap_size;
		Size			   entry_size;
		RoaringLeafEntry  *le;
		/*
		 * volatile ensures PG_FINALLY sees the correct pointer after a
		 * longjmp: the bitmaps are constructed before PG_TRY but may not
		 * yet be freed (NULL'd) when an ERROR fires inside the try block.
		 * Without volatile, the compiler may cache the NULL assignment in a
		 * register that the longjmp clobbers, leaving PG_FINALLY with a
		 * stale non-NULL or NULL value.
		 */
		roaring64_bitmap_t * volatile bm64 = NULL;
		roaring_bitmap_t   * volatile bm32 = NULL;

		CHECK_FOR_INTERRUPTS();

		while (group_end < ntuples && tuples[group_end].value == cur_value)
			group_end++;

		gc = group_end - i;

		if (is_lossy)
		{
			/* Lossy: tids are block numbers (fit in uint32). */
			uint32 *gtids32 = (uint32 *) palloc(gc * sizeof(uint32));

			for (gi = 0; gi < gc; gi++)
				gtids32[gi] = (uint32) tuples[i + gi].tid;
			bm32 = roaring_bitmap_of_ptr((size_t) gc, gtids32);
			pfree(gtids32);
			bitmap_size = roaring_bitmap_portable_size_in_bytes(bm32);
		}
		else
		{
			uint64 *gtids64 = (uint64 *) palloc(gc * sizeof(uint64));

			for (gi = 0; gi < gc; gi++)
				gtids64[gi] = tuples[i + gi].tid;
			bm64 = roaring64_bitmap_of_ptr((size_t) gc, gtids64);
			pfree(gtids64);
			bitmap_size = roaring64_bitmap_portable_size_in_bytes(bm64);
		}

		PG_TRY();
		{
		nentries++;

		if (bitmap_size > (size_t) max_inline)
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
				MarkBufferDirty(leaf_buf);
				leaf_batch_bufs[leaf_batch_n]   = leaf_buf;
				leaf_batch_blknos[leaf_batch_n] = old_blkno;
				leaf_batch_n++;
				if (leaf_batch_n == ROARING_BUILD_WAL_BATCH)
				{
					flush_leaf_wal_batch(index, leaf_batch_bufs,
										 leaf_batch_blknos, leaf_batch_n);
					leaf_batch_n = 0;
				}

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

		if (bitmap_size > (size_t) max_inline)
		{
			char				 *bm_data;
			RoaringOverflowEntry *oe;

			bm_data = (char *) palloc(bitmap_size);
			if (is_lossy)
				roaring_bitmap_portable_serialize(bm32, bm_data);
			else
				roaring64_bitmap_portable_serialize(bm64, bm_data);
			oe		= (RoaringOverflowEntry *) palloc(sizeof(RoaringOverflowEntry));
			oe->value		   = cur_value;
			oe->cardinality	   = is_lossy ? roaring_cardinality32(bm32)
										  : roaring64_cardinality32(bm64);
			oe->flags		   = ROARING_ENTRY_OVERFLOW;
			oe->total_len	   = (uint32) bitmap_size;
			oe->overflow_blkno = roaring_write_overflow_chain(index, bm_data,
															   bitmap_size);
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
			le->value   = cur_value;
			le->flags   = ROARING_ENTRY_INLINE;
			if (is_lossy)
			{
				le->cardinality = roaring_cardinality32(bm32);
				roaring_bitmap_portable_serialize(bm32, (char *)(le + 1));
			}
			else
			{
				le->cardinality = roaring64_cardinality32(bm64);
				roaring64_bitmap_portable_serialize(bm64, (char *)(le + 1));
			}

			if (PageAddItem(leaf_page, (Item) le,
							sizeof(RoaringLeafEntry) + bitmap_size,
							InvalidOffsetNumber, false, false) == InvalidOffsetNumber)
				elog(ERROR, "roaring_build: PageAddItem failed unexpectedly");
			pfree(le);
		}

		Assert(leaf_spc->entry_count < PG_UINT16_MAX);
		leaf_spc->entry_count++;
		if (is_lossy) { roaring_bitmap_free(bm32); bm32 = NULL; }
		else		  { roaring64_bitmap_free(bm64); bm64 = NULL; }
		}
		PG_FINALLY();
		{
			if (bm32) { roaring_bitmap_free(bm32); bm32 = NULL; }
			if (bm64) { roaring64_bitmap_free(bm64); bm64 = NULL; }
		}
		PG_END_TRY();

		i = group_end;
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
		MarkBufferDirty(leaf_buf);
		leaf_batch_bufs[leaf_batch_n]   = leaf_buf;
		leaf_batch_blknos[leaf_batch_n] = blkno;
		leaf_batch_n++;
		flush_leaf_wal_batch(index, leaf_batch_bufs,
							 leaf_batch_blknos, leaf_batch_n);
		leaf_batch_n = 0;
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
		{
			uint32           l2_count  = (l1_count + max_dir - 1) / max_dir;
			RoaringDirEntry *l2_entries;
			uint32           k;

			if (l2_count > max_dir)
				elog(ERROR,
					 "roaring_build: index too large for three-level directory "
					 "(%u leaf pages)", leaf_count);

			l2_entries = palloc(l2_count * sizeof(RoaringDirEntry));

			for (k = 0; k < l2_count; k++)
			{
				uint32      start = k * max_dir;
				uint32      count = Min(max_dir, l1_count - start);
				BlockNumber blkno;

				blkno = roaring_write_dir_page(index, l1_entries + start, count, 1);
				l2_entries[k].high_key   = l1_entries[start + count - 1].high_key;
				l2_entries[k].child_page = blkno;
			}

			*root_dir_out = roaring_write_dir_page(index, l2_entries, l2_count, 2);
			pfree(l2_entries);
		}
		else
			*root_dir_out = roaring_write_dir_page(index, l1_entries, l1_count, 1);

		pfree(l1_entries);
	}

	pfree(leaf_entries);
	*nentries_out  = nentries;
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
	double				reltuples;
	long				nentries    = 0;
	long				init_nalloc;

	BlockNumber			root_dir		= InvalidBlockNumber;
	BlockNumber			leftmost_leaf	= InvalidBlockNumber;
	BlockNumber			rightmost_leaf	= InvalidBlockNumber;


	result = (IndexBuildResult *) palloc0(sizeof(IndexBuildResult));

	/* Block 0: placeholder — filled in by write_metapage at the end. */
	{
		Buffer buf = roaring_extend_page(index);

		Assert(BufferGetBlockNumber(buf) == ROARING_METAPAGE_BLKNO);
		PageInit(BufferGetPage(buf), BLCKSZ, 0);
		roaring_wal_and_release(index, buf);
	}

	/*
	 * Size the initial tuple array from pg_class.reltuples so the array
	 * doesn't have to be repalloc'd on most builds.  Fall back to 1024 for
	 * empty/unanalyzed tables.
	 */
	{
		/* Clamp initial allocation to maintenance_work_mem budget. */
		long mem_nalloc = ((long) maintenance_work_mem * 1024L) /
						  (long) sizeof(RoaringBuildTuple) /
						  Max(index->rd_att->natts, 1);

		/* Guard against implementation-defined cast when reltuples > LONG_MAX. */
		init_nalloc = Min((long) heap->rd_rel->reltuples,
						  (long) (MaxAllocHugeSize / sizeof(RoaringBuildTuple) /
								  (Size) Max(index->rd_att->natts, 1)));
		if (init_nalloc < 1024)
			init_nalloc = 1024;
		if (mem_nalloc > 1024 && init_nalloc > mem_nalloc)
			init_nalloc = mem_nalloc;
	}

	bstate.heap_tuples = 0;
	bstate.atttypid    = TupleDescAttr(index->rd_att, 0)->atttypid;
	bstate.nalloc      = init_nalloc * index->rd_att->natts; /* natts entries per row */
	bstate.ntuples     = 0;
	bstate.tuples      = (RoaringBuildTuple *)
						 palloc_extended(bstate.nalloc * sizeof(RoaringBuildTuple),
										 MCXT_ALLOC_HUGE);

	pgstat_progress_update_param(PROGRESS_CREATEIDX_TUPLES_TOTAL,
								 (int64) heap->rd_rel->reltuples);

	reltuples = table_index_build_scan(heap, index, indexInfo,
									   true, true,
									   roaring_build_callback,
									   &bstate, NULL);

	pgstat_progress_update_param(PROGRESS_CREATEIDX_TUPLES_DONE,
								 (int64) bstate.ntuples);

	/* Sort flat array by (value, tid). */
	pgstat_progress_update_param(PROGRESS_CREATEIDX_PHASE, 2); /* sorting */
	qsort(bstate.tuples, bstate.ntuples, sizeof(RoaringBuildTuple),
		  cmp_build_tuple);

	/* Write leaf pages + directory; counts distinct values into nentries. */
	pgstat_progress_update_param(PROGRESS_CREATEIDX_PHASE, 3); /* writing pages */
	write_leaf_and_dir_pages(index, bstate.tuples, bstate.ntuples, false,
							 &nentries, &root_dir, &leftmost_leaf, &rightmost_leaf);

	pfree(bstate.tuples);

	{
		BlockNumber pending_blknos[ROARING_PENDING_SHARDS];
		BlockNumber dummy_head;
		int			i;

		for (i = 0; i < ROARING_PENDING_SHARDS; i++)
			pending_blknos[i] =
				roaring_init_pending_page(index, InvalidBuffer, &dummy_head,
										 ROARING_PAGE_PENDING_INSERT);

		write_metapage(index, root_dir, leftmost_leaf, rightmost_leaf,
					   pending_blknos, (uint32) nentries,
					   ROARING_FLAG_EXACT);
	}

	result->heap_tuples  = reltuples;
	result->index_tuples = (double) nentries;
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
	SMgrRelation		  smgr = RelationGetSmgr(index);
	char				 *buf  = (char *) palloc(BLCKSZ);
	Page				  page = (Page) buf;
	RoaringMetaPageData  *meta;
	RoaringPendingSpecial *spc;
	int					  i;

	smgrcreate(smgr, INIT_FORKNUM, false);

	/* Page 0: metapage */
	PageInit(page, BLCKSZ, 0);
	meta = RoaringPageGetMeta(page);
	memset(meta, 0, sizeof(*meta));
	meta->magic					   = ROARING_MAGIC;
	meta->version				   = ROARING_INDEX_VERSION;
	meta->flags					   = ROARING_FLAG_EXACT;
	meta->croaring_format_version  = ROARING_EXPECTED_FORMAT_VERSION;
	meta->num_shards			   = ROARING_PENDING_SHARDS;
	meta->root_directory_page	   = InvalidBlockNumber;
	meta->leftmost_leaf_page	   = InvalidBlockNumber;
	meta->rightmost_leaf_page	   = InvalidBlockNumber;
	meta->free_list_head		   = InvalidBlockNumber;

	/* Shards occupy pages 1..ROARING_PENDING_SHARDS */
	for (i = 0; i < ROARING_PENDING_SHARDS; i++)
	{
		BlockNumber blkno = (BlockNumber)(i + 1);

		meta->shards[i].insert_head  = blkno;
		meta->shards[i].insert_tail  = blkno;
		meta->shards[i].insert_count = 0;
		meta->shards[i].merging_head = InvalidBlockNumber;
		meta->shards[i].carry_head   = InvalidBlockNumber;
	}
	meta->total_entries			  = 0;
	meta->pending_merge_threshold = (uint32) roaring_pending_merge_threshold_guc;
	((PageHeader) page)->pd_lower =
		(LocationIndex)(SizeOfPageHeaderData + sizeof(RoaringMetaPageData));
	PageSetChecksumInplace(page, ROARING_METAPAGE_BLKNO);
	smgrextend(smgr, INIT_FORKNUM, ROARING_METAPAGE_BLKNO, page, true);
	log_newpage(&smgr->smgr_rlocator.locator, INIT_FORKNUM,
				ROARING_METAPAGE_BLKNO, page, true);

	/* Pages 1..ROARING_PENDING_SHARDS: one empty pending page per shard */
	PageInit(page, BLCKSZ, sizeof(RoaringPendingSpecial));
	spc = (RoaringPendingSpecial *) PageGetSpecialPointer(page);
	spc->page_type	 = ROARING_PAGE_PENDING_INSERT;
	spc->flags		 = 0;
	spc->entry_count = 0;
	spc->next_page	 = InvalidBlockNumber;
	spc->xmin_low	 = InvalidTransactionId;
	spc->_pad		 = 0;
	spc->value_min	 = PG_INT64_MAX;
	spc->value_max	 = PG_INT64_MIN;

	for (i = 0; i < ROARING_PENDING_SHARDS; i++)
	{
		BlockNumber blkno = (BlockNumber)(i + 1);

		PageSetChecksumInplace(page, blkno);
		smgrextend(smgr, INIT_FORKNUM, blkno, page, true);
		log_newpage(&smgr->smgr_rlocator.locator, INIT_FORKNUM, blkno, page, true);
	}

	smgrimmedsync(smgr, INIT_FORKNUM);
	pfree(buf);
}

/* ================================================================
 * roaring_build_lossy
 *
 * Lossy (page-level) variant of roaring_build.  The build callback
 * stores the heap block number instead of the linearized TID, so each
 * bitmap entry represents a page rather than an individual tuple.
 * Everything else — leaf/directory page layout, pending list, metapage —
 * is identical to the exact path.
 * ================================================================ */
static void
roaring_build_callback_lossy(Relation index, ItemPointer tid, Datum *values,
							  bool *isnull, bool tupleIsAlive, void *state)
{
	RoaringBuildState  *bstate = (RoaringBuildState *) state;
	int					natts   = index->rd_att->natts;

	bstate->heap_tuples++;

	if (natts > 1)
	{
		int i;

		for (i = 0; i < natts; i++)
		{
			Oid		typid = TupleDescAttr(index->rd_att, i)->atttypid;
			int64	value;

			if (isnull[i])
				continue;

			if (typid == FLOAT4OID && isnan(DatumGetFloat4(values[i])))
				continue;

			value = ROARING_COL_KEY(i + 1, roaring_datum_to_key32(values[i], typid));

			if (bstate->ntuples == bstate->nalloc)
			{
				long new_nalloc = bstate->nalloc * 2;
				check_build_mem_limit(new_nalloc);
				bstate->nalloc = new_nalloc;
				bstate->tuples  = (RoaringBuildTuple *)
								  repalloc_extended(bstate->tuples,
													bstate->nalloc * sizeof(RoaringBuildTuple),
													MCXT_ALLOC_HUGE);
			}

			bstate->tuples[bstate->ntuples].value = value;
			bstate->tuples[bstate->ntuples].tid   =
				(uint32) ItemPointerGetBlockNumber(tid);
			bstate->ntuples++;
		}
		return;
	}

	/* Single-column path. */
	{
		int64 value;

		if (isnull[0])
			return;

		if (bstate->atttypid == FLOAT4OID && isnan(DatumGetFloat4(values[0])))
			return;

		value = roaring_datum_to_key64(values[0], bstate->atttypid);

		if (bstate->ntuples == bstate->nalloc)
		{
			long new_nalloc = bstate->nalloc * 2;
			check_build_mem_limit(new_nalloc);
			bstate->nalloc = new_nalloc;
			bstate->tuples  = (RoaringBuildTuple *)
							  repalloc_extended(bstate->tuples,
												bstate->nalloc * sizeof(RoaringBuildTuple),
												MCXT_ALLOC_HUGE);
		}

		bstate->tuples[bstate->ntuples].value = value;
		/* Lossy: store block number only — many TIDs map to the same blkno. */
		bstate->tuples[bstate->ntuples].tid   =
			(uint32) ItemPointerGetBlockNumber(tid);
		bstate->ntuples++;
	}
}

IndexBuildResult *
roaring_build_lossy(Relation heap, Relation index, struct IndexInfo *indexInfo)
{
	IndexBuildResult   *result;
	RoaringBuildState	bstate;
	double				reltuples;
	long				nentries    = 0;
	long				init_nalloc;

	BlockNumber			root_dir		= InvalidBlockNumber;
	BlockNumber			leftmost_leaf	= InvalidBlockNumber;
	BlockNumber			rightmost_leaf	= InvalidBlockNumber;


	result = (IndexBuildResult *) palloc0(sizeof(IndexBuildResult));

	{
		Buffer buf = roaring_extend_page(index);

		Assert(BufferGetBlockNumber(buf) == ROARING_METAPAGE_BLKNO);
		PageInit(BufferGetPage(buf), BLCKSZ, 0);
		roaring_wal_and_release(index, buf);
	}

	{
		long mem_nalloc = ((long) maintenance_work_mem * 1024L) /
						  (long) sizeof(RoaringBuildTuple) /
						  Max(index->rd_att->natts, 1);

		init_nalloc = Min((long) heap->rd_rel->reltuples,
						  (long) (MaxAllocHugeSize / sizeof(RoaringBuildTuple) /
								  (Size) Max(index->rd_att->natts, 1)));
		if (init_nalloc < 1024)
			init_nalloc = 1024;
		if (mem_nalloc > 1024 && init_nalloc > mem_nalloc)
			init_nalloc = mem_nalloc;
	}

	bstate.heap_tuples = 0;
	bstate.atttypid    = TupleDescAttr(index->rd_att, 0)->atttypid;
	bstate.nalloc      = init_nalloc * index->rd_att->natts;
	bstate.ntuples     = 0;
	bstate.tuples      = (RoaringBuildTuple *)
						 palloc_extended(bstate.nalloc * sizeof(RoaringBuildTuple),
										 MCXT_ALLOC_HUGE);

	pgstat_progress_update_param(PROGRESS_CREATEIDX_TUPLES_TOTAL,
								 (int64) heap->rd_rel->reltuples);

	reltuples = table_index_build_scan(heap, index, indexInfo,
									   true, true,
									   roaring_build_callback_lossy,
									   &bstate, NULL);

	pgstat_progress_update_param(PROGRESS_CREATEIDX_TUPLES_DONE,
								 (int64) bstate.ntuples);

	/* Sort by (value, blkno). */
	pgstat_progress_update_param(PROGRESS_CREATEIDX_PHASE, 2); /* sorting */
	qsort(bstate.tuples, bstate.ntuples, sizeof(RoaringBuildTuple),
		  cmp_build_tuple);

	/* Remove duplicate (value, blkno) pairs — many heap TIDs share the same
	 * block.  Dedup here so write_leaf_and_dir_pages sees unique entries only,
	 * cutting bitmap work 10-100x at low ndistinct. */
	{
		long out = 0;
		long in;

		for (in = 0; in < bstate.ntuples; in++)
		{
			if (out == 0 ||
				bstate.tuples[in].value != bstate.tuples[out - 1].value ||
				bstate.tuples[in].tid   != bstate.tuples[out - 1].tid)
				bstate.tuples[out++] = bstate.tuples[in];
		}
		bstate.ntuples = out;
	}

	pgstat_progress_update_param(PROGRESS_CREATEIDX_PHASE, 3); /* writing pages */
	write_leaf_and_dir_pages(index, bstate.tuples, bstate.ntuples, true,
							 &nentries, &root_dir, &leftmost_leaf, &rightmost_leaf);

	pfree(bstate.tuples);

	{
		BlockNumber pending_blknos[ROARING_PENDING_SHARDS];
		BlockNumber dummy_head;
		int			i;

		for (i = 0; i < ROARING_PENDING_SHARDS; i++)
			pending_blknos[i] =
				roaring_init_pending_page(index, InvalidBuffer, &dummy_head,
										 ROARING_PAGE_PENDING_INSERT);

		write_metapage(index, root_dir, leftmost_leaf, rightmost_leaf,
					   pending_blknos, (uint32) nentries,
					   ROARING_FLAG_LOSSY);
	}

	result->heap_tuples  = reltuples;
	result->index_tuples = (double) nentries;
	return result;
}
