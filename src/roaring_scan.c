#include "pg_roaring_index.h"

#include "access/relscan.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/pg_type_d.h"
#include "nodes/tidbitmap.h"
#include "storage/bufmgr.h"
#include "storage/procarray.h"
#include "utils/array.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#define ROARING_TID_BATCH 512

/* ----------------------------------------------------------------
 * roaring_dir_lookup
 *
 * Walk the directory tree starting at dir_blkno, looking for the
 * leaf page that would contain 'value'.
 *
 * Directory level convention:
 *   level=0  → children are leaf pages (binary-search within the leaf)
 *   level=N  → children are dir pages at level N-1 (recurse)
 *
 * Returns InvalidBlockNumber if value is beyond all high_keys.
 * ---------------------------------------------------------------- */
BlockNumber
roaring_dir_lookup(Relation index, BlockNumber dir_blkno, int64 value)
{
	BlockNumber cur = dir_blkno;
	int			depth;

	/* Two-level directory cap: at most 3 iterations (root + 1 level + leaf). */
	for (depth = 0; depth < 4; depth++)
	{
		Buffer			   buf;
		Page			   page;
		RoaringDirSpecial *spc;
		RoaringDirEntry   *entries;
		uint32			   count;
		uint8			   level;
		BlockNumber		   child;
		uint32			   lo, hi;

		buf     = ReadBuffer(index, cur);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page    = BufferGetPage(buf);
		spc     = (RoaringDirSpecial *) PageGetSpecialPointer(page);
		entries = (RoaringDirEntry *) PageGetContents(page);
		count   = spc->entry_count;
		level   = spc->level;

		{
			uint32 max_dir = (uint32)
				((BLCKSZ - SizeOfPageHeaderData
				  - MAXALIGN(sizeof(RoaringDirSpecial)))
				 / ROARING_DIR_ENTRY_SIZE);

			if (count > max_dir)
			{
				UnlockReleaseBuffer(buf);
				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("pg_roaring_index: directory page %u has corrupt "
								"entry_count %u (max %u)",
								cur, count, max_dir)));
			}
		}

		/* Binary search: find leftmost entry with high_key >= value. */
		lo = 0;
		hi = count;
		while (lo < hi)
		{
			uint32 mid = (lo + hi) / 2;
			if (entries[mid].high_key < value)
				lo = mid + 1;
			else
				hi = mid;
		}

		if (lo >= count)
		{
			UnlockReleaseBuffer(buf);
			return InvalidBlockNumber;
		}

		child = entries[lo].child_page;
		UnlockReleaseBuffer(buf);

		if (level == 0)
			return child;	/* child is a leaf page */

		cur = child;		/* descend into sub-directory */
	}

	elog(ERROR, "pg_roaring_index: directory depth exceeded at block %u "
				"(possible corruption)", dir_blkno);
	return InvalidBlockNumber;	/* unreachable */
}

/* ----------------------------------------------------------------
 * roaring_beginscan / roaring_rescan / roaring_endscan
 * ---------------------------------------------------------------- */
IndexScanDesc
roaring_beginscan(Relation rel, int nkeys, int norderbys)
{
	IndexScanDesc	   scan;
	RoaringScanOpaque *so;

	scan = RelationGetIndexScan(rel, nkeys, norderbys);

	so				  = (RoaringScanOpaque *) palloc0(sizeof(RoaringScanOpaque));
	so->bitmap_loaded = false;

	scan->opaque = so;
	return scan;
}

void
roaring_rescan(IndexScanDesc scan, ScanKey keys, int nkeys,
			   ScanKey orderbys, int norderbys)
{
	RoaringScanOpaque *so = (RoaringScanOpaque *) scan->opaque;

	so->bitmap_loaded = false;

	if (keys && nkeys > 0)
		memmove(scan->keyData, keys, nkeys * sizeof(ScanKeyData));
}

void
roaring_endscan(IndexScanDesc scan)
{
	pfree(scan->opaque);
	scan->opaque = NULL;
}

/* ----------------------------------------------------------------
 * roaring_pending_visible
 *
 * GIN-style four-state MVCC visibility check for a pending entry's xmin.
 * ---------------------------------------------------------------- */
static bool
roaring_pending_visible(TransactionId xmin, Snapshot snapshot)
{
	if (!TransactionIdIsValid(xmin))
		return false;
	/* Frozen / bootstrap xids are always visible. */
	if (!TransactionIdIsNormal(xmin))
		return true;
	if (TransactionIdIsCurrentTransactionId(xmin))
		return true;
	if (XidInMVCCSnapshot(xmin, snapshot))
		return false;
	if (TransactionIdDidAbort(xmin))
		return false;
	if (TransactionIdIsInProgress(xmin))
		return false;
	return TransactionIdDidCommit(xmin);
}

/* ----------------------------------------------------------------
 * lookup_value_as_bitmap
 *
 * Look up one value in the main index and return its deserialized roaring
 * bitmap.  Returns an empty (but valid) bitmap if the value is not found.
 * Caller must free the result with roaring_bitmap_free().
 * ---------------------------------------------------------------- */
static roaring_bitmap_t *
lookup_value_as_bitmap(Relation index, BlockNumber root_blkno, int64 value)
{
	BlockNumber		 leaf_blkno;
	Buffer			 leafbuf;
	Page			 leafpage;
	OffsetNumber	 lo, hi;
	OffsetNumber	 found_off = InvalidOffsetNumber;
	RoaringLeafEntry *le	   = NULL;
	roaring_bitmap_t *bm;

	leaf_blkno = roaring_dir_lookup(index, root_blkno, value);
	if (leaf_blkno == InvalidBlockNumber)
		return roaring_bitmap_create();

	leafbuf  = ReadBuffer(index, leaf_blkno);
	LockBuffer(leafbuf, BUFFER_LOCK_SHARE);
	leafpage = BufferGetPage(leafbuf);

	lo = 1;
	hi = PageGetMaxOffsetNumber(leafpage);
	while (lo <= hi)
	{
		OffsetNumber	  mid = (lo + hi) / 2;
		RoaringLeafEntry *e   = (RoaringLeafEntry *)
								PageGetItem(leafpage,
											PageGetItemId(leafpage, mid));
		if (e->value == value)
		{
			le        = e;
			found_off = mid;
			break;
		}
		else if (e->value < value)
			lo = mid + 1;
		else
			hi = mid - 1;
	}

	if (le == NULL)
	{
		UnlockReleaseBuffer(leafbuf);
		return roaring_bitmap_create();
	}

	if (le->flags & ROARING_ENTRY_OVERFLOW)
	{
		RoaringOverflowEntry oe_copy;

		memcpy(&oe_copy, le, sizeof(RoaringOverflowEntry));
		UnlockReleaseBuffer(leafbuf);
		bm = roaring_read_overflow_bitmap(index, &oe_copy);
	}
	else
	{
		Size item_len   = ItemIdGetLength(PageGetItemId(leafpage, found_off));
		Size bitmap_len = item_len - sizeof(RoaringLeafEntry);

		bm = roaring_bitmap_portable_deserialize_safe(
				(const char *)(le + 1), bitmap_len);
		UnlockReleaseBuffer(leafbuf);
	}

	if (bm == NULL)
		elog(ERROR,
			 "pg_roaring_index: failed to deserialize bitmap "
			 "for value " INT64_FORMAT, value);

	return bm;
}

/* ----------------------------------------------------------------
 * pending_chain_as_bitmap
 *
 * Walk one pending list chain, collecting visible TIDs for scan_value
 * into a roaring bitmap of linearized TIDs.  Returns an empty bitmap
 * (not NULL) if nothing matches.  Caller frees with roaring_bitmap_free().
 * ---------------------------------------------------------------- */
static roaring_bitmap_t *
pending_chain_as_bitmap(Relation index, BlockNumber start_blkno,
						int64 scan_value, Snapshot snapshot)
{
	roaring_bitmap_t *bm  = roaring_bitmap_create();
	BlockNumber		  cur = start_blkno;

	while (cur != InvalidBlockNumber)
	{
		Buffer				  buf;
		Page				  page;
		RoaringPendingSpecial *spc;
		RoaringPendingEntry	  *raw;
		uint16				  k;

		buf  = ReadBuffer(index, cur);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		spc  = (RoaringPendingSpecial *) PageGetSpecialPointer(page);
		raw  = (RoaringPendingEntry *) PageGetContents(page);

		if (spc->entry_count > ROARING_PENDING_PER_PAGE)
		{
			UnlockReleaseBuffer(buf);
			roaring_bitmap_free(bm);
			elog(ERROR,
				 "pg_roaring_index: corrupt pending page %u: entry_count %u > max %d",
				 cur, spc->entry_count, ROARING_PENDING_PER_PAGE);
		}

		if (scan_value >= spc->value_min && scan_value <= spc->value_max)
		{
			for (k = 0; k < spc->entry_count; k++)
			{
				if (raw[k].value != scan_value)
					continue;
				if (!roaring_pending_visible(raw[k].xmin, snapshot))
					continue;
				roaring_bitmap_add(bm, raw[k].linear_tid);
			}
		}

		cur = spc->next_page;
		UnlockReleaseBuffer(buf);
	}
	return bm;
}

/* ----------------------------------------------------------------
 * Lossy variants: bitmaps contain heap block numbers, not TID linearizations.
 * ---------------------------------------------------------------- */
static roaring_bitmap_t *
lookup_value_as_bitmap_lossy(Relation index, BlockNumber root_blkno, int64 value)
{
	BlockNumber		 leaf_blkno;
	Buffer			 leafbuf;
	Page			 leafpage;
	OffsetNumber	 lo, hi;
	OffsetNumber	 found_off = InvalidOffsetNumber;
	RoaringLeafEntry *le	   = NULL;
	roaring_bitmap_t *bm;

	leaf_blkno = roaring_dir_lookup(index, root_blkno, value);
	if (leaf_blkno == InvalidBlockNumber)
		return roaring_bitmap_create();

	leafbuf  = ReadBuffer(index, leaf_blkno);
	LockBuffer(leafbuf, BUFFER_LOCK_SHARE);
	leafpage = BufferGetPage(leafbuf);

	lo = 1;
	hi = PageGetMaxOffsetNumber(leafpage);
	while (lo <= hi)
	{
		OffsetNumber	  mid = (lo + hi) / 2;
		RoaringLeafEntry *e   = (RoaringLeafEntry *)
								PageGetItem(leafpage,
											PageGetItemId(leafpage, mid));
		if (e->value == value)
		{
			le        = e;
			found_off = mid;
			break;
		}
		else if (e->value < value)
			lo = mid + 1;
		else
			hi = mid - 1;
	}

	if (le == NULL)
	{
		UnlockReleaseBuffer(leafbuf);
		return roaring_bitmap_create();
	}

	if (le->flags & ROARING_ENTRY_OVERFLOW)
	{
		RoaringOverflowEntry oe_copy;

		memcpy(&oe_copy, le, sizeof(RoaringOverflowEntry));
		UnlockReleaseBuffer(leafbuf);
		bm = roaring_read_overflow_bitmap(index, &oe_copy);
	}
	else
	{
		Size item_len   = ItemIdGetLength(PageGetItemId(leafpage, found_off));
		Size bitmap_len = item_len - sizeof(RoaringLeafEntry);

		bm = roaring_bitmap_portable_deserialize_safe(
				(const char *)(le + 1), bitmap_len);
		UnlockReleaseBuffer(leafbuf);
	}

	if (bm == NULL)
		elog(ERROR,
			 "pg_roaring_index: failed to deserialize bitmap "
			 "for value " INT64_FORMAT, value);

	return bm;
}

static roaring_bitmap_t *
pending_chain_as_bitmap_lossy(Relation index, BlockNumber start_blkno,
							   int64 scan_value, Snapshot snapshot)
{
	roaring_bitmap_t *bm  = roaring_bitmap_create();
	BlockNumber		  cur = start_blkno;

	while (cur != InvalidBlockNumber)
	{
		Buffer				  buf;
		Page				  page;
		RoaringPendingSpecial *spc;
		RoaringPendingEntry	  *raw;
		uint16				  k;

		buf  = ReadBuffer(index, cur);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		spc  = (RoaringPendingSpecial *) PageGetSpecialPointer(page);
		raw  = (RoaringPendingEntry *) PageGetContents(page);

		if (spc->entry_count > ROARING_PENDING_PER_PAGE)
		{
			UnlockReleaseBuffer(buf);
			roaring_bitmap_free(bm);
			elog(ERROR,
				 "pg_roaring_index: corrupt pending page %u: entry_count %u > max %d",
				 cur, spc->entry_count, ROARING_PENDING_PER_PAGE);
		}

		if (scan_value >= spc->value_min && scan_value <= spc->value_max)
		{
			for (k = 0; k < spc->entry_count; k++)
			{
				if (raw[k].value != scan_value)
					continue;
				if (!roaring_pending_visible(raw[k].xmin, snapshot))
					continue;
				roaring_bitmap_add(bm, raw[k].linear_tid);
			}
		}

		cur = spc->next_page;
		UnlockReleaseBuffer(buf);
	}
	return bm;
}

/* ----------------------------------------------------------------
 * emit_exact_bitmap_to_tbm
 *
 * Decode linearized TIDs from bm and add to tbm via tid_buf batching.
 * Returns TID count.
 * ---------------------------------------------------------------- */
static int64
emit_exact_bitmap_to_tbm(roaring_bitmap_t *bm, TIDBitmap *tbm,
						  ItemPointerData *tid_buf, int *tid_count_p)
{
	roaring_uint32_iterator_t it;
	int64				ntids = 0;

	roaring_iterator_init(bm, &it);
	while (it.has_value)
	{
		uint32		 linear = it.current_value;
		BlockNumber  block  = (BlockNumber)(linear >> 9);
		OffsetNumber off    = (OffsetNumber)((linear & 0x1FF) + 1);

		if (*tid_count_p == ROARING_TID_BATCH)
		{
			tbm_add_tuples(tbm, tid_buf, *tid_count_p, false);
			*tid_count_p = 0;
		}
		ItemPointerSet(&tid_buf[(*tid_count_p)++], block, off);
		ntids++;
		roaring_uint32_iterator_advance(&it);
	}
	return ntids;
}

/* emit block numbers from bm to tbm via tbm_add_page */
static int64
emit_lossy_bitmap_to_tbm(roaring_bitmap_t *bm, TIDBitmap *tbm)
{
	roaring_uint32_iterator_t it;
	int64 npages = 0;

	roaring_iterator_init(bm, &it);
	while (it.has_value)
	{
		tbm_add_page(tbm, (BlockNumber) it.current_value);
		npages++;
		roaring_uint32_iterator_advance(&it);
	}
	return npages;
}

/* ----------------------------------------------------------------
 * roaring_getbitmap
 *
 * Equality scan using the per-column namespace encoding.
 *
 * Single-column (natts==1): raw int8/int4 value lookup.
 * Multi-column (natts>1): one bitmap per scan key (index + pending),
 * AND across all keys, then emit the intersection to tbm.
 * ---------------------------------------------------------------- */

int64
roaring_getbitmap(IndexScanDesc scan, TIDBitmap *tbm)
{
	Relation			index = scan->indexRelation;
	RoaringScanOpaque  *so    = (RoaringScanOpaque *) scan->opaque;
	int64				ntids = 0;

	Buffer				metabuf;
	RoaringMetaPageData *meta;
	BlockNumber			root_blkno;
	BlockNumber			pending_head;
	BlockNumber			merging_head;
	uint32				pending_count;
	Snapshot			snapshot;

	ItemPointerData		tid_buf[ROARING_TID_BATCH];
	int					tid_count = 0;

	if (so->bitmap_loaded)
		return 0;
	so->bitmap_loaded = true;

	if (scan->numberOfKeys < 1)
		return 0;

	if (scan->keyData[0].sk_flags & SK_ISNULL)
		return 0;

	/* ---- Read metapage (or use rd_amcache). ---- */
	{
		RoaringAmCache *cache = (RoaringAmCache *) index->rd_amcache;

		if (cache != NULL &&
			cache->pending_count == 0 &&
			cache->merging_head == InvalidBlockNumber)
		{
			Buffer     tmp     = ReadBuffer(index, ROARING_METAPAGE_BLKNO);
			XLogRecPtr cur_lsn = BufferGetLSNAtomic(tmp);

			ReleaseBuffer(tmp);

			if (cur_lsn == cache->meta_lsn)
			{
				root_blkno    = cache->root_blkno;
				pending_count = 0;
				pending_head  = InvalidBlockNumber;
				merging_head  = InvalidBlockNumber;
				goto after_meta;
			}
		}

		metabuf = ReadBuffer(index, ROARING_METAPAGE_BLKNO);
		LockBuffer(metabuf, BUFFER_LOCK_SHARE);
		meta = RoaringPageGetMeta(BufferGetPage(metabuf));
		roaring_validate_metapage(index, meta);
		root_blkno    = meta->root_directory_page;
		pending_count = meta->pending_insert_count;
		pending_head  = meta->pending_insert_head;
		merging_head  = meta->pending_merging_head;

		if (cache == NULL)
		{
			cache = (RoaringAmCache *)
				MemoryContextAllocZero(CacheMemoryContext,
									   sizeof(RoaringAmCache));
			index->rd_amcache = cache;
		}
		cache->root_blkno    = root_blkno;
		cache->total_entries = meta->total_entries;
		cache->meta_lsn      = PageGetLSN(BufferGetPage(metabuf));
		cache->pending_count = pending_count;
		cache->merging_head  = merging_head;
		UnlockReleaseBuffer(metabuf);
	}
	after_meta:;

	snapshot = scan->xs_snapshot;

	/*
	 * Multi-column path: for each scan key look up its per-column bitmap
	 * (main index OR pending), then AND all per-key bitmaps together and
	 * emit the intersection to tbm.
	 */
	if (index->rd_att->natts > 1)
	{
		roaring_bitmap_t * volatile result = NULL;
		roaring_bitmap_t * volatile col_bm = NULL;
		int ki;

		PG_TRY();
		{
			for (ki = 0; ki < scan->numberOfKeys; ki++)
			{
				ScanKey k = &scan->keyData[ki];

				if (k->sk_flags & SK_ISNULL)
					continue;

				if (k->sk_flags & SK_SEARCHARRAY)
				{
					ArrayType *arr = DatumGetArrayTypeP(k->sk_argument);
					Datum	  *elems;
					bool	  *nulls;
					int		   nelems, i;

					deconstruct_array(arr, INT4OID, 4, true, 'i',
									  &elems, &nulls, &nelems);
					col_bm = roaring_bitmap_create();

					for (i = 0; i < nelems; i++)
					{
						roaring_bitmap_t *vbm;
						int64			  col_key;

						if (nulls[i])
							continue;
						col_key = ROARING_COL_KEY(k->sk_attno,
												   DatumGetInt32(elems[i]));
						vbm = lookup_value_as_bitmap(index, root_blkno, col_key);
						if (pending_count > 0 || merging_head != InvalidBlockNumber)
						{
							roaring_bitmap_t *pbm =
								pending_chain_as_bitmap(index, pending_head,
														 col_key, snapshot);
							roaring_bitmap_or_inplace(vbm, pbm);
							roaring_bitmap_free(pbm);
							if (merging_head != InvalidBlockNumber)
							{
								pbm = pending_chain_as_bitmap(index, merging_head,
															   col_key, snapshot);
								roaring_bitmap_or_inplace(vbm, pbm);
								roaring_bitmap_free(pbm);
							}
						}
						roaring_bitmap_or_inplace((roaring_bitmap_t *) col_bm, vbm);
						roaring_bitmap_free(vbm);
					}
					pfree(elems);
					pfree(nulls);
				}
				else
				{
					int64 col_key = ROARING_COL_KEY(k->sk_attno,
													 DatumGetInt32(k->sk_argument));

					col_bm = lookup_value_as_bitmap(index, root_blkno, col_key);
					if (pending_count > 0 || merging_head != InvalidBlockNumber)
					{
						roaring_bitmap_t *pbm =
							pending_chain_as_bitmap(index, pending_head,
													 col_key, snapshot);
						roaring_bitmap_or_inplace((roaring_bitmap_t *) col_bm, pbm);
						roaring_bitmap_free(pbm);
						if (merging_head != InvalidBlockNumber)
						{
							pbm = pending_chain_as_bitmap(index, merging_head,
														   col_key, snapshot);
							roaring_bitmap_or_inplace((roaring_bitmap_t *) col_bm, pbm);
							roaring_bitmap_free(pbm);
						}
					}
				}

				if (result == NULL)
					result = col_bm;
				else
				{
					roaring_bitmap_and_inplace((roaring_bitmap_t *) result,
											   (roaring_bitmap_t *) col_bm);
					roaring_bitmap_free((roaring_bitmap_t *) col_bm);
				}
				col_bm = NULL;
			}

			if (result != NULL)
			{
				ntids = emit_exact_bitmap_to_tbm((roaring_bitmap_t *) result,
												  tbm, tid_buf, &tid_count);
				roaring_bitmap_free((roaring_bitmap_t *) result);
				result = NULL;
			}
		}
		PG_CATCH();
		{
			if (col_bm)
				roaring_bitmap_free((roaring_bitmap_t *) col_bm);
			if (result)
				roaring_bitmap_free((roaring_bitmap_t *) result);
			PG_RE_THROW();
		}
		PG_END_TRY();

		if (tid_count > 0)
			tbm_add_tuples(tbm, tid_buf, tid_count, false);

		return ntids;
	}

	/*
	 * Single-column path.
	 */
	{
		ScanKey key      = &scan->keyData[0];
		Oid		atttypid = TupleDescAttr(index->rd_att, 0)->atttypid;

#define ROARING_DATUM_TO_INT64(d) \
	((atttypid == INT4OID) ? (int64) DatumGetInt32(d) : DatumGetInt64(d))

		if (key->sk_flags & SK_SEARCHARRAY)
		{
			ArrayType *arr = DatumGetArrayTypeP(key->sk_argument);
			Datum	  *elems;
			bool	  *nulls;
			int		   nelems, i;
			roaring_bitmap_t * volatile bm = NULL;

			if (atttypid == INT4OID)
				deconstruct_array(arr, INT4OID, 4, true, 'i',
								  &elems, &nulls, &nelems);
			else
				deconstruct_array(arr, INT8OID, 8, true, 'd',
								  &elems, &nulls, &nelems);

			PG_TRY();
			{
				for (i = 0; i < nelems; i++)
				{
					int64 v;

					if (nulls[i])
						continue;
					v = ROARING_DATUM_TO_INT64(elems[i]);

					bm = lookup_value_as_bitmap(index, root_blkno, v);
					if (pending_count > 0 || merging_head != InvalidBlockNumber)
					{
						roaring_bitmap_t *pbm =
							pending_chain_as_bitmap(index, pending_head, v, snapshot);
						roaring_bitmap_or_inplace(bm, pbm);
						roaring_bitmap_free(pbm);
						if (merging_head != InvalidBlockNumber)
						{
							pbm = pending_chain_as_bitmap(index, merging_head, v, snapshot);
							roaring_bitmap_or_inplace(bm, pbm);
							roaring_bitmap_free(pbm);
						}
					}
					ntids += emit_exact_bitmap_to_tbm(bm, tbm, tid_buf, &tid_count);
					roaring_bitmap_free(bm);
					bm = NULL;
				}
			}
			PG_CATCH();
			{
				if (bm)
					roaring_bitmap_free(bm);
				PG_RE_THROW();
			}
			PG_END_TRY();

			pfree(elems);
			pfree(nulls);
		}
		else
		{
			roaring_bitmap_t * volatile bm = NULL;
			int64 scan_value = ROARING_DATUM_TO_INT64(key->sk_argument);

			PG_TRY();
			{
				bm = lookup_value_as_bitmap(index, root_blkno, scan_value);
				if (pending_count > 0 || merging_head != InvalidBlockNumber)
				{
					roaring_bitmap_t *pbm =
						pending_chain_as_bitmap(index, pending_head, scan_value, snapshot);
					roaring_bitmap_or_inplace(bm, pbm);
					roaring_bitmap_free(pbm);
					if (merging_head != InvalidBlockNumber)
					{
						pbm = pending_chain_as_bitmap(index, merging_head, scan_value, snapshot);
						roaring_bitmap_or_inplace(bm, pbm);
						roaring_bitmap_free(pbm);
					}
				}
				ntids = emit_exact_bitmap_to_tbm(bm, tbm, tid_buf, &tid_count);
				roaring_bitmap_free(bm);
				bm = NULL;
			}
			PG_CATCH();
			{
				if (bm)
					roaring_bitmap_free(bm);
				PG_RE_THROW();
			}
			PG_END_TRY();
		}

#undef ROARING_DATUM_TO_INT64
	}

	if (tid_count > 0)
		tbm_add_tuples(tbm, tid_buf, tid_count, false);

	return ntids;
}

/* ----------------------------------------------------------------
 * roaring_getbitmap_lossy
 *
 * Lossy (page-level) scan.  Identical structure to roaring_getbitmap
 * but uses block-number bitmaps and tbm_add_page.
 * ---------------------------------------------------------------- */
int64
roaring_getbitmap_lossy(IndexScanDesc scan, TIDBitmap *tbm)
{
	Relation			index = scan->indexRelation;
	RoaringScanOpaque  *so    = (RoaringScanOpaque *) scan->opaque;
	int64				ntids = 0;

	Buffer				metabuf;
	RoaringMetaPageData *meta;
	BlockNumber			root_blkno;
	BlockNumber			pending_head;
	BlockNumber			merging_head;
	uint32				pending_count;
	Snapshot			snapshot;

	if (so->bitmap_loaded)
		return 0;
	so->bitmap_loaded = true;

	if (scan->numberOfKeys < 1)
		return 0;

	if (scan->keyData[0].sk_flags & SK_ISNULL)
		return 0;

	/* ---- Read metapage (or use rd_amcache). ---- */
	{
		RoaringAmCache *cache = (RoaringAmCache *) index->rd_amcache;

		if (cache != NULL &&
			cache->pending_count == 0 &&
			cache->merging_head == InvalidBlockNumber)
		{
			Buffer     tmp     = ReadBuffer(index, ROARING_METAPAGE_BLKNO);
			XLogRecPtr cur_lsn = BufferGetLSNAtomic(tmp);

			ReleaseBuffer(tmp);

			if (cur_lsn == cache->meta_lsn)
			{
				root_blkno    = cache->root_blkno;
				pending_count = 0;
				pending_head  = InvalidBlockNumber;
				merging_head  = InvalidBlockNumber;
				goto after_meta_lossy;
			}
		}

		metabuf = ReadBuffer(index, ROARING_METAPAGE_BLKNO);
		LockBuffer(metabuf, BUFFER_LOCK_SHARE);
		meta = RoaringPageGetMeta(BufferGetPage(metabuf));
		roaring_validate_metapage(index, meta);
		root_blkno    = meta->root_directory_page;
		pending_count = meta->pending_insert_count;
		pending_head  = meta->pending_insert_head;
		merging_head  = meta->pending_merging_head;

		if (cache == NULL)
		{
			cache = (RoaringAmCache *)
				MemoryContextAllocZero(CacheMemoryContext,
									   sizeof(RoaringAmCache));
			index->rd_amcache = cache;
		}
		cache->root_blkno    = root_blkno;
		cache->total_entries = meta->total_entries;
		cache->meta_lsn      = PageGetLSN(BufferGetPage(metabuf));
		cache->pending_count = pending_count;
		cache->merging_head  = merging_head;
		UnlockReleaseBuffer(metabuf);
	}
	after_meta_lossy:;

	snapshot = scan->xs_snapshot;

	/* Multi-column: AND bitmaps across scan keys (block-number bitmaps). */
	if (index->rd_att->natts > 1)
	{
		roaring_bitmap_t * volatile result = NULL;
		roaring_bitmap_t * volatile col_bm = NULL;
		int ki;

		PG_TRY();
		{
			for (ki = 0; ki < scan->numberOfKeys; ki++)
			{
				ScanKey k = &scan->keyData[ki];

				if (k->sk_flags & SK_ISNULL)
					continue;

				if (k->sk_flags & SK_SEARCHARRAY)
				{
					ArrayType *arr = DatumGetArrayTypeP(k->sk_argument);
					Datum	  *elems;
					bool	  *nulls;
					int		   nelems, i;

					deconstruct_array(arr, INT4OID, 4, true, 'i',
									  &elems, &nulls, &nelems);
					col_bm = roaring_bitmap_create();

					for (i = 0; i < nelems; i++)
					{
						roaring_bitmap_t *vbm;
						int64			  col_key;

						if (nulls[i])
							continue;
						col_key = ROARING_COL_KEY(k->sk_attno,
												   DatumGetInt32(elems[i]));
						vbm = lookup_value_as_bitmap_lossy(index, root_blkno, col_key);
						if (pending_count > 0 || merging_head != InvalidBlockNumber)
						{
							roaring_bitmap_t *pbm =
								pending_chain_as_bitmap_lossy(index, pending_head,
															   col_key, snapshot);
							roaring_bitmap_or_inplace(vbm, pbm);
							roaring_bitmap_free(pbm);
							if (merging_head != InvalidBlockNumber)
							{
								pbm = pending_chain_as_bitmap_lossy(index, merging_head,
																	 col_key, snapshot);
								roaring_bitmap_or_inplace(vbm, pbm);
								roaring_bitmap_free(pbm);
							}
						}
						roaring_bitmap_or_inplace((roaring_bitmap_t *) col_bm, vbm);
						roaring_bitmap_free(vbm);
					}
					pfree(elems);
					pfree(nulls);
				}
				else
				{
					int64 col_key = ROARING_COL_KEY(k->sk_attno,
													 DatumGetInt32(k->sk_argument));

					col_bm = lookup_value_as_bitmap_lossy(index, root_blkno, col_key);
					if (pending_count > 0 || merging_head != InvalidBlockNumber)
					{
						roaring_bitmap_t *pbm =
							pending_chain_as_bitmap_lossy(index, pending_head,
														   col_key, snapshot);
						roaring_bitmap_or_inplace((roaring_bitmap_t *) col_bm, pbm);
						roaring_bitmap_free(pbm);
						if (merging_head != InvalidBlockNumber)
						{
							pbm = pending_chain_as_bitmap_lossy(index, merging_head,
																 col_key, snapshot);
							roaring_bitmap_or_inplace((roaring_bitmap_t *) col_bm, pbm);
							roaring_bitmap_free(pbm);
						}
					}
				}

				if (result == NULL)
					result = col_bm;
				else
				{
					roaring_bitmap_and_inplace((roaring_bitmap_t *) result,
											   (roaring_bitmap_t *) col_bm);
					roaring_bitmap_free((roaring_bitmap_t *) col_bm);
				}
				col_bm = NULL;
			}

			if (result != NULL)
			{
				ntids = emit_lossy_bitmap_to_tbm((roaring_bitmap_t *) result, tbm);
				roaring_bitmap_free((roaring_bitmap_t *) result);
				result = NULL;
			}
		}
		PG_CATCH();
		{
			if (col_bm)
				roaring_bitmap_free((roaring_bitmap_t *) col_bm);
			if (result)
				roaring_bitmap_free((roaring_bitmap_t *) result);
			PG_RE_THROW();
		}
		PG_END_TRY();

		return ntids;
	}

	/* Single-column path. */
	{
		ScanKey key      = &scan->keyData[0];
		Oid		atttypid = TupleDescAttr(index->rd_att, 0)->atttypid;

#define ROARING_DATUM_TO_INT64_LOSSY(d) \
	((atttypid == INT4OID) ? (int64) DatumGetInt32(d) : DatumGetInt64(d))

		if (key->sk_flags & SK_SEARCHARRAY)
		{
			ArrayType *arr = DatumGetArrayTypeP(key->sk_argument);
			Datum	  *elems;
			bool	  *nulls;
			int		   nelems, i;
			roaring_bitmap_t * volatile bm = NULL;

			if (atttypid == INT4OID)
				deconstruct_array(arr, INT4OID, 4, true, 'i',
								  &elems, &nulls, &nelems);
			else
				deconstruct_array(arr, INT8OID, 8, true, 'd',
								  &elems, &nulls, &nelems);

			PG_TRY();
			{
				for (i = 0; i < nelems; i++)
				{
					int64 v;

					if (nulls[i])
						continue;
					v = ROARING_DATUM_TO_INT64_LOSSY(elems[i]);

					bm = lookup_value_as_bitmap_lossy(index, root_blkno, v);
					if (pending_count > 0 || merging_head != InvalidBlockNumber)
					{
						roaring_bitmap_t *pbm =
							pending_chain_as_bitmap_lossy(index, pending_head, v, snapshot);
						roaring_bitmap_or_inplace(bm, pbm);
						roaring_bitmap_free(pbm);
						if (merging_head != InvalidBlockNumber)
						{
							pbm = pending_chain_as_bitmap_lossy(index, merging_head, v, snapshot);
							roaring_bitmap_or_inplace(bm, pbm);
							roaring_bitmap_free(pbm);
						}
					}
					ntids += emit_lossy_bitmap_to_tbm(bm, tbm);
					roaring_bitmap_free(bm);
					bm = NULL;
				}
			}
			PG_CATCH();
			{
				if (bm)
					roaring_bitmap_free(bm);
				PG_RE_THROW();
			}
			PG_END_TRY();

			pfree(elems);
			pfree(nulls);
		}
		else
		{
			roaring_bitmap_t * volatile bm = NULL;
			int64 scan_value = ROARING_DATUM_TO_INT64_LOSSY(key->sk_argument);

			PG_TRY();
			{
				bm = lookup_value_as_bitmap_lossy(index, root_blkno, scan_value);
				if (pending_count > 0 || merging_head != InvalidBlockNumber)
				{
					roaring_bitmap_t *pbm =
						pending_chain_as_bitmap_lossy(index, pending_head, scan_value, snapshot);
					roaring_bitmap_or_inplace(bm, pbm);
					roaring_bitmap_free(pbm);
					if (merging_head != InvalidBlockNumber)
					{
						pbm = pending_chain_as_bitmap_lossy(index, merging_head, scan_value, snapshot);
						roaring_bitmap_or_inplace(bm, pbm);
						roaring_bitmap_free(pbm);
					}
				}
				ntids = emit_lossy_bitmap_to_tbm(bm, tbm);
				roaring_bitmap_free(bm);
				bm = NULL;
			}
			PG_CATCH();
			{
				if (bm)
					roaring_bitmap_free(bm);
				PG_RE_THROW();
			}
			PG_END_TRY();
		}

#undef ROARING_DATUM_TO_INT64_LOSSY
	}

	return ntids;
}
