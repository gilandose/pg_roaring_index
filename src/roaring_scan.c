#include "pg_roaring_index.h"

#include "access/relscan.h"
#include "access/transam.h"
#include "access/xact.h"
#include "storage/bufmgr.h"
#include "storage/procarray.h"
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
 * roaring_getbitmap
 *
 * Lookup the single equality scan key in the roaring index and
 * populate the TIDBitmap with matching TIDs.
 *
 * Two sources are consulted:
 *   (a) Main index: metapage → directory → leaf → roaring32 bitmap.
 *   (b) Pending insert list: linear scan for matching value/xmin.
 * ---------------------------------------------------------------- */
int64
roaring_getbitmap(IndexScanDesc scan, TIDBitmap *tbm)
{
	Relation			index = scan->indexRelation;
	RoaringScanOpaque  *so    = (RoaringScanOpaque *) scan->opaque;
	int64				ntids = 0;
	int64				scan_value;

	Buffer				metabuf;
	RoaringMetaPageData *meta;
	BlockNumber			root_blkno;
	BlockNumber			pending_head;
	BlockNumber			merging_head;
	uint32				pending_count;

	ItemPointerData		tid_buf[ROARING_TID_BATCH];
	int					tid_count = 0;

	/* getbitmap is called once per scan; subsequent calls return 0. */
	if (so->bitmap_loaded)
		return 0;
	so->bitmap_loaded = true;

	if (scan->numberOfKeys < 1)
		return 0;

	if (scan->keyData[0].sk_flags & SK_ISNULL)
		return 0;

	scan_value = DatumGetInt64(scan->keyData[0].sk_argument);

	/* ---- 1. Read metapage ---- */
	metabuf = ReadBuffer(index, ROARING_METAPAGE_BLKNO);
	LockBuffer(metabuf, BUFFER_LOCK_SHARE);
	meta = RoaringPageGetMeta(BufferGetPage(metabuf));

	if (meta->magic != ROARING_MAGIC)
		elog(ERROR, "pg_roaring_index: bad magic in metapage of index \"%s\"",
			 RelationGetRelationName(index));

	root_blkno    = meta->root_directory_page;
	pending_head  = meta->pending_insert_head;
	pending_count = meta->pending_insert_count;
	merging_head  = meta->pending_merging_head;	/* set during a concurrent merge */
	UnlockReleaseBuffer(metabuf);

	/* ---- 2–5. Main index lookup (directory → leaf → bitmap). ---- */
	if (root_blkno != InvalidBlockNumber)
	{
		BlockNumber			 leaf_blkno;
		Buffer				 leafbuf;
		Page				 leafpage;
		OffsetNumber		 lo, hi;
		OffsetNumber		 found_off = InvalidOffsetNumber;
		RoaringLeafEntry	*le		   = NULL;

		leaf_blkno = roaring_dir_lookup(index, root_blkno, scan_value);

		if (leaf_blkno != InvalidBlockNumber)
		{
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
				if (e->value == scan_value)
				{
					le        = e;
					found_off = mid;
					break;
				}
				else if (e->value < scan_value)
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
						 "for value " INT64_FORMAT, scan_value);

				PG_TRY();
				{
					roaring_uint32_iterator_t *it = roaring_iterator_create(bm);

					while (it->has_value)
					{
						uint32		 linear = it->current_value;
						BlockNumber  block  = (BlockNumber)(linear >> 9);
						OffsetNumber off    = (OffsetNumber)((linear & 0x1FF) + 1);

						if (tid_count == ROARING_TID_BATCH)
						{
							tbm_add_tuples(tbm, tid_buf, tid_count, false);
							tid_count = 0;
						}
						ItemPointerSet(&tid_buf[tid_count++], block, off);
						ntids++;
						roaring_uint32_iterator_advance(it);
					}
					roaring_uint32_iterator_free(it);
				}
				PG_FINALLY();
				{
					roaring_bitmap_free(bm);
				}
				PG_END_TRY();
			}
			else
				UnlockReleaseBuffer(leafbuf);
		}
	}

	/*
	 * ---- 6. Pending insert list scan. ----
	 *
	 * Walk both chains: pending_head (current inserts) and merging_head
	 * (entries being merged into leaves by a concurrent roaring_merge_pending).
	 * When no merge is active, merging_head == InvalidBlockNumber and only the
	 * first scan_pending_chain call does any work.
	 */
	{
		Snapshot snapshot = scan->xs_snapshot;

		if (pending_count > 0 || merging_head != InvalidBlockNumber)
		{
			ntids += scan_pending_chain(index, pending_head, scan_value, snapshot,
										tbm, tid_buf, &tid_count);
			if (merging_head != InvalidBlockNumber)
				ntids += scan_pending_chain(index, merging_head, scan_value, snapshot,
											tbm, tid_buf, &tid_count);
		}
	}

	/* Flush any remaining TIDs. */
	if (tid_count > 0)
		tbm_add_tuples(tbm, tid_buf, tid_count, false);

	return ntids;
}
