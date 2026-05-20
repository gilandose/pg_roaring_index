#include "pg_roaring_index.h"

#include "access/relscan.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/pg_type_d.h"
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
 * scan_pending_chain
 *
 * Walk one pending list chain from start_blkno, appending any visible
 * TIDs for scan_value to tbm.  Returns count of TIDs added.
 * ---------------------------------------------------------------- */
static int64
scan_pending_chain(Relation index, BlockNumber start_blkno,
				   int64 scan_value, Snapshot snapshot,
				   TIDBitmap *tbm,
				   ItemPointerData *tid_buf, int *tid_count_p)
{
	BlockNumber  cur   = start_blkno;
	int64		 ntids = 0;

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
			elog(ERROR,
				 "pg_roaring_index: corrupt pending page %u: entry_count %u > max %d",
				 cur, spc->entry_count, ROARING_PENDING_PER_PAGE);
		}

		/* Skip page if scan_value is outside this page's value range. */
		if (scan_value < spc->value_min || scan_value > spc->value_max)
		{
			cur = spc->next_page;
			UnlockReleaseBuffer(buf);
			continue;
		}

		for (k = 0; k < spc->entry_count; k++)
		{
			if (raw[k].value != scan_value)
				continue;
			if (!roaring_pending_visible(raw[k].xmin, snapshot))
				continue;

			if (*tid_count_p == ROARING_TID_BATCH)
			{
				tbm_add_tuples(tbm, tid_buf, *tid_count_p, false);
				*tid_count_p = 0;
			}
			{
				BlockNumber  block = (BlockNumber)(raw[k].linear_tid >> 9);
				OffsetNumber off   = (OffsetNumber)((raw[k].linear_tid & 0x1FF) + 1);
				ItemPointerSet(&tid_buf[(*tid_count_p)++], block, off);
				ntids++;
			}
		}

		cur = spc->next_page;
		UnlockReleaseBuffer(buf);
	}
	return ntids;
}

/* ----------------------------------------------------------------
 * lookup_value_in_index
 *
 * Look up one value in the main index (directory → leaf → bitmap) and
 * add matching TIDs to tbm via tid_buf.  Returns count of TIDs added.
 * ---------------------------------------------------------------- */
static int64
lookup_value_in_index(Relation index, BlockNumber root_blkno, int64 value,
					  TIDBitmap *tbm,
					  ItemPointerData *tid_buf, int *tid_count_p)
{
	BlockNumber		 leaf_blkno;
	Buffer			 leafbuf;
	Page			 leafpage;
	OffsetNumber	 lo, hi;
	OffsetNumber	 found_off = InvalidOffsetNumber;
	RoaringLeafEntry *le	   = NULL;
	int64			 ntids	   = 0;

	leaf_blkno = roaring_dir_lookup(index, root_blkno, value);
	if (leaf_blkno == InvalidBlockNumber)
		return 0;

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

	if (le != NULL)
	{
		roaring_bitmap_t *bm;

		if (le->flags == ROARING_ENTRY_OVERFLOW)
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

		PG_TRY();
		{
			uint64_t  card = roaring_bitmap_get_cardinality(bm);
			uint32_t *arr  = (uint32_t *) palloc(card * sizeof(uint32_t));
			uint64_t  k;

			roaring_bitmap_to_uint32_array(bm, arr);

			for (k = 0; k < card; k++)
			{
				uint32		 linear = arr[k];
				BlockNumber  block  = (BlockNumber)(linear >> 9);
				OffsetNumber off    = (OffsetNumber)((linear & 0x1FF) + 1);

				if (*tid_count_p == ROARING_TID_BATCH)
				{
					tbm_add_tuples(tbm, tid_buf, *tid_count_p, false);
					*tid_count_p = 0;
				}
				ItemPointerSet(&tid_buf[(*tid_count_p)++], block, off);
			}
			ntids = (int64) card;
			pfree(arr);
		}
		PG_FINALLY();
		{
			roaring_bitmap_free(bm);
		}
		PG_END_TRY();
	}
	else
		UnlockReleaseBuffer(leafbuf);

	return ntids;
}

/* ----------------------------------------------------------------
 * roaring_getbitmap
 *
 * Lookup equality scan key(s) in the roaring index and populate
 * the TIDBitmap with matching TIDs.
 *
 * Two sources are consulted:
 *   (a) Main index: metapage → directory → leaf → roaring32 bitmap.
 *   (b) Pending insert list: linear scan for matching value/xmin.
 *
 * Handles both scalar equality (SK_SEARCHARRAY unset) and array-based
 * IN() queries (SK_SEARCHARRAY set, sk_argument is an int8[] array).
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
	ScanKey				key;

	ItemPointerData		tid_buf[ROARING_TID_BATCH];
	int					tid_count = 0;

	/* getbitmap is called once per scan; subsequent calls return 0. */
	if (so->bitmap_loaded)
		return 0;
	so->bitmap_loaded = true;

	if (scan->numberOfKeys < 1)
		return 0;

	key = &scan->keyData[0];
	if (key->sk_flags & SK_ISNULL)
		return 0;

	/* ---- 1. Read metapage (or use rd_amcache). ---- */
	{
		RoaringAmCache *cache = (RoaringAmCache *) index->rd_amcache;

		/*
		 * Fast path: if the cache says pending is empty, check whether the
		 * metapage LSN is still current via an atomic read (no content lock).
		 * If so, skip the full metapage read entirely.
		 */
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

		/* Full metapage read. */
		metabuf = ReadBuffer(index, ROARING_METAPAGE_BLKNO);
		LockBuffer(metabuf, BUFFER_LOCK_SHARE);
		meta = RoaringPageGetMeta(BufferGetPage(metabuf));
		roaring_validate_metapage(index, meta);
		root_blkno    = meta->root_directory_page;
		pending_count = meta->pending_insert_count;
		pending_head  = meta->pending_insert_head;
		merging_head  = meta->pending_merging_head;

		/* Populate or refresh the cache. */
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
	 * Determine the indexed column's base type so we extract datums correctly.
	 * For int4 columns (roaring_int4_tid_ops), use DatumGetInt32 then promote;
	 * for int8 columns (roaring_int8_tid_ops), use DatumGetInt64 directly.
	 */
	{
		Oid atttypid = TupleDescAttr(index->rd_att, 0)->atttypid;

#define ROARING_DATUM_TO_INT64(d) \
	((atttypid == INT4OID) ? (int64) DatumGetInt32(d) : DatumGetInt64(d))

	if (key->sk_flags & SK_SEARCHARRAY)
	{
		/*
		 * IN() / = ANY() query.  Deconstruct using the array element type
		 * matching the opclass: int4[] for roaring_int4_tid_ops, int8[] for
		 * roaring_int8_tid_ops.
		 */
		ArrayType  *arr  = DatumGetArrayTypeP(key->sk_argument);
		Datum	   *elems;
		bool	   *nulls;
		int			nelems;
		int			i;

		if (atttypid == INT4OID)
			deconstruct_array(arr, INT4OID, 4, true, 'i',
							  &elems, &nulls, &nelems);
		else
			deconstruct_array(arr, INT8OID, 8, true, 'd',
							  &elems, &nulls, &nelems);

		for (i = 0; i < nelems; i++)
		{
			int64 v;

			if (nulls[i])
				continue;

			v = ROARING_DATUM_TO_INT64(elems[i]);

			if (root_blkno != InvalidBlockNumber)
				ntids += lookup_value_in_index(index, root_blkno, v,
											   tbm, tid_buf, &tid_count);

			if (pending_count > 0 || merging_head != InvalidBlockNumber)
			{
				ntids += scan_pending_chain(index, pending_head, v, snapshot,
										   tbm, tid_buf, &tid_count);
				if (merging_head != InvalidBlockNumber)
					ntids += scan_pending_chain(index, merging_head, v, snapshot,
												tbm, tid_buf, &tid_count);
			}
		}

		pfree(elems);
		pfree(nulls);
	}
	else
	{
		/* ---- Scalar equality lookup. ---- */
		int64 scan_value = ROARING_DATUM_TO_INT64(key->sk_argument);

		if (root_blkno != InvalidBlockNumber)
			ntids += lookup_value_in_index(index, root_blkno, scan_value,
										   tbm, tid_buf, &tid_count);

		/*
		 * Pending list scan — both chains for concurrent-merge safety.
		 * merging_head is InvalidBlockNumber when no merge is active.
		 */
		if (pending_count > 0 || merging_head != InvalidBlockNumber)
		{
			ntids += scan_pending_chain(index, pending_head, scan_value, snapshot,
										tbm, tid_buf, &tid_count);
			if (merging_head != InvalidBlockNumber)
				ntids += scan_pending_chain(index, merging_head, scan_value, snapshot,
											tbm, tid_buf, &tid_count);
		}
	}

#undef ROARING_DATUM_TO_INT64
	} /* end atttypid dispatch block */

	/* Flush any remaining TIDs. */
	if (tid_count > 0)
		tbm_add_tuples(tbm, tid_buf, tid_count, false);

	return ntids;
}
