#include "pg_roaring_index.h"

#include "access/relscan.h"
#include "access/transam.h"
#include "access/xact.h"
#include "storage/bufmgr.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

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
	Buffer			   buf;
	Page			   page;
	RoaringDirSpecial *spc;
	RoaringDirEntry   *entries;
	uint32			   count;
	uint8			   level;
	BlockNumber		   child;
	uint32			   lo, hi;

	buf     = ReadBuffer(index, dir_blkno);
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
		/* value exceeds all high_keys in this directory. */
		UnlockReleaseBuffer(buf);
		return InvalidBlockNumber;
	}

	child = entries[lo].child_page;
	UnlockReleaseBuffer(buf);

	if (level == 0)
		return child;		/* child is a leaf page */

	return roaring_dir_lookup(index, child, value);	/* recurse into sub-dir */
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

	if (keys && scan->numberOfKeys > 0)
		memmove(scan->keyData, keys, scan->numberOfKeys * sizeof(ScanKeyData));
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
 * MVCC visibility check for a pending-list entry's xmin against the
 * current scan snapshot.
 * ---------------------------------------------------------------- */
static bool
roaring_pending_visible(TransactionId xmin, Snapshot snapshot)
{
	if (!TransactionIdIsValid(xmin))
		return false;
	if (TransactionIdIsCurrentTransactionId(xmin))
		return true;					/* own insert */
	if (XidInMVCCSnapshot(xmin, snapshot))
		return false;					/* was in-progress when snapshot taken */
	return TransactionIdDidCommit(xmin);
}

/* ----------------------------------------------------------------
 * roaring_getbitmap
 *
 * Lookup the single equality scan key in the roaring index and
 * populate the TIDBitmap with matching TIDs.
 *
 * Two sources are consulted:
 *   (a) Main index: metapage → directory → leaf → roaring64 bitmap.
 *   (b) Pending insert list: linear scan for matching value/xmin.
 * ---------------------------------------------------------------- */
int64
roaring_getbitmap(IndexScanDesc scan, TIDBitmap *tbm)
{
#define ROARING_TID_BATCH 512
	Relation			index = scan->indexRelation;
	RoaringScanOpaque  *so    = (RoaringScanOpaque *) scan->opaque;
	int64				ntids = 0;
	int64				scan_value;

	Buffer				metabuf;
	RoaringMetaPageData *meta;
	BlockNumber			root_blkno;
	BlockNumber			pending_head;
	uint32				pending_count;

	ItemPointerData		tid_buf[ROARING_TID_BATCH];
	int					tid_count = 0;

	/* getbitmap is called once per scan; subsequent calls return 0. */
	if (so->bitmap_loaded)
		return 0;
	so->bitmap_loaded = true;

	if (scan->numberOfKeys < 1)
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
				if (le->flags != ROARING_ENTRY_INLINE)
				{
					UnlockReleaseBuffer(leafbuf);
					elog(ERROR,
						 "pg_roaring_index: overflow entries not yet implemented");
				}

				/* Deserialize bitmap and drain into tid_buf. */
				{
					Size				item_len   = ItemIdGetLength(
						PageGetItemId(leafpage, found_off));
					Size				bitmap_len = item_len - sizeof(RoaringLeafEntry);
					roaring64_bitmap_t *bm;
					roaring64_iterator_t *it;

					bm = roaring64_bitmap_portable_deserialize_safe(
							(const char *)(le + 1), bitmap_len);
					if (bm == NULL)
					{
						UnlockReleaseBuffer(leafbuf);
						elog(ERROR,
							 "pg_roaring_index: failed to deserialize bitmap "
							 "for value " INT64_FORMAT, scan_value);
					}

					UnlockReleaseBuffer(leafbuf);

					it = roaring64_iterator_create(bm);
					while (roaring64_iterator_has_value(it))
					{
						uint64		 linear = roaring64_iterator_value(it);
						BlockNumber  block  = (BlockNumber)(linear >> 16);
						OffsetNumber off    = (OffsetNumber)(linear & 0xFFFF);

						if (tid_count == ROARING_TID_BATCH)
						{
							tbm_add_tuples(tbm, tid_buf, tid_count, false);
							tid_count = 0;
						}
						ItemPointerSet(&tid_buf[tid_count++], block, off);
						ntids++;
						roaring64_iterator_advance(it);
					}
					roaring64_iterator_free(it);
					roaring64_bitmap_free(bm);
				}
			}
			else
				UnlockReleaseBuffer(leafbuf);
		}
	}

	/* ---- 6. Pending insert list scan. ---- */
	if (pending_count > 0)
	{
		BlockNumber		cur      = pending_head;
		Snapshot		snapshot = scan->xs_snapshot;

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

			for (k = 0; k < spc->entry_count; k++)
			{
				if (raw[k].value != scan_value)
					continue;
				if (!roaring_pending_visible(raw[k].xmin, snapshot))
					continue;

				if (tid_count == ROARING_TID_BATCH)
				{
					tbm_add_tuples(tbm, tid_buf, tid_count, false);
					tid_count = 0;
				}
				{
					BlockNumber  block = (BlockNumber)(raw[k].linear_tid >> 16);
					OffsetNumber off   = (OffsetNumber)(raw[k].linear_tid & 0xFFFF);
					ItemPointerSet(&tid_buf[tid_count++], block, off);
					ntids++;
				}
			}

			cur = spc->next_page;
			UnlockReleaseBuffer(buf);
		}
	}

	/* Flush any remaining TIDs. */
	if (tid_count > 0)
		tbm_add_tuples(tbm, tid_buf, tid_count, false);

	return ntids;
#undef ROARING_TID_BATCH
}
