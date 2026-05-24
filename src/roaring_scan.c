#include "pg_roaring_index.h"

#include <math.h>

#include "access/relscan.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/pg_type_d.h"
#include "nodes/tidbitmap.h"
#include "storage/bufmgr.h"
#include "storage/procarray.h"
#include "utils/array.h"
#include "utils/lsyscache.h"
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

	/* Detect hash-keyed columns (text, uuid): their bitmaps can collide, so
	 * the executor must recheck every matched tuple. */
	so->needs_recheck = false;
	{
		int i;

		for (i = 0; i < rel->rd_index->indnkeyatts; i++)
		{
			Oid opcintype = rel->rd_opcintype[i];

			if (opcintype == TEXTOID || opcintype == UUIDOID)
			{
				so->needs_recheck = true;
				break;
			}
		}
	}

	scan->opaque = so;
	return scan;
}

void
roaring_rescan(IndexScanDesc scan, ScanKey keys, int nkeys,
			   ScanKey orderbys, int norderbys)
{
	RoaringScanOpaque *so = (RoaringScanOpaque *) scan->opaque;

	Assert(norderbys == 0);

	so->bitmap_loaded = false;

	if (keys && nkeys > 0)
		memmove(scan->keyData, keys, nkeys * sizeof(ScanKeyData));

	scan->numberOfKeys = nkeys;
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
 * Caller must free the result with roaring64_bitmap_free().
 * ---------------------------------------------------------------- */
static roaring64_bitmap_t *
lookup_value_as_bitmap(Relation index, BlockNumber root_blkno, int64 value)
{
	BlockNumber		 leaf_blkno;
	Buffer			 leafbuf;
	Page			 leafpage;
	OffsetNumber	 found_off;
	RoaringLeafEntry *le;
	roaring64_bitmap_t *bm;

	leaf_blkno = roaring_dir_lookup(index, root_blkno, value);
	if (leaf_blkno == InvalidBlockNumber)
		return roaring64_bitmap_create();

	for (;;)
	{
		OffsetNumber		lo,
							hi;
		RoaringLeafSpecial *lspc;

		leafbuf   = ReadBuffer(index, leaf_blkno);
		LockBuffer(leafbuf, BUFFER_LOCK_SHARE);
		leafpage  = BufferGetPage(leafbuf);
		found_off = InvalidOffsetNumber;
		le		  = NULL;

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
				le		  = e;
				found_off = mid;
				break;
			}
			else if (e->value < value)
				lo = mid + 1;
			else
				hi = mid - 1;
		}

		if (le != NULL)
			break;

		/*
		 * Binary search miss.  A concurrent merge may have split this leaf
		 * after our directory lookup, moving the target value to the right
		 * sibling.  Follow the right link if the target is beyond this
		 * leaf's max key (_bt_moveright pattern).
		 */
		lspc = (RoaringLeafSpecial *) PageGetSpecialPointer(leafpage);
		if (lspc->right_page != InvalidBlockNumber)
		{
			OffsetNumber maxoff = PageGetMaxOffsetNumber(leafpage);

			if (maxoff >= 1)
			{
				RoaringLeafEntry *last = (RoaringLeafEntry *)
					PageGetItem(leafpage, PageGetItemId(leafpage, maxoff));

				if (last->value < value)
				{
					BlockNumber right = lspc->right_page;

					UnlockReleaseBuffer(leafbuf);
					leaf_blkno = right;
					continue;
				}
			}
		}

		UnlockReleaseBuffer(leafbuf);
		return roaring64_bitmap_create();
	}

	if (le->cardinality == 0)
	{
		UnlockReleaseBuffer(leafbuf);
		return roaring64_bitmap_create();
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
		Size item_len = ItemIdGetLength(PageGetItemId(leafpage, found_off));
		Size bitmap_len;

		if (item_len < sizeof(RoaringLeafEntry))
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("pg_roaring_index: leaf entry at offset %u is too short",
							found_off)));
		bitmap_len = item_len - sizeof(RoaringLeafEntry);
		bm = roaring64_bitmap_portable_deserialize_safe(
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
 * lookup_values_as_bitmaps_leaf_walk
 *
 * Bulk variant of lookup_value_as_bitmap for SK_SEARCHARRAY (IN-list) scans.
 * vals[0..nvals-1] must be sorted ascending and deduplicated.
 * bitmaps[i] is set to a newly allocated roaring bitmap for vals[i];
 * values not found in the index get an empty (non-NULL) bitmap.
 * Caller must free all nvals bitmaps.
 *
 * Uses one roaring_dir_lookup for vals[0] then walks the leaf chain forward,
 * avoiding N-1 repeated directory lookups for clustered IN-list values.
 * ---------------------------------------------------------------- */
static void
lookup_values_as_bitmaps_leaf_walk(Relation index, BlockNumber root_blkno,
								   const int64 *vals, roaring64_bitmap_t **bitmaps,
								   int nvals)
{
	BlockNumber	cur;
	int			vi = 0;	/* index of next vals[] entry to resolve */
	int			j;

	/*
	 * Deferred overflow entries: the leaf buffer must be released before
	 * calling roaring_read_overflow_bitmap (which reads further buffers).
	 * Allocated lazily — overflow entries are rare; most callers pay zero.
	 *
	 * Struct declared at top of block to satisfy -Wdeclaration-after-statement.
	 */
	typedef struct { int vi; RoaringOverflowEntry oe; } DeferredOE;
	DeferredOE *deferred     = NULL;
	int			deferred_cap = 0;
	int			ndeferred    = 0;

	/*
	 * Initialize all output slots to NULL.  Hits are filled inline; misses
	 * are back-filled with empty bitmaps at the end.  This avoids an
	 * up-front create+free cycle for every hit value.
	 */
	memset(bitmaps, 0, (size_t) nvals * sizeof(roaring64_bitmap_t *));

	cur = roaring_dir_lookup(index, root_blkno, vals[0]);
	if (cur == InvalidBlockNumber)
		goto fill_empty;

	while (cur != InvalidBlockNumber && vi < nvals)
	{
		Buffer				 leafbuf;
		Page				 leafpage;
		RoaringLeafSpecial	*lspc;
		OffsetNumber		 maxoff;
		RoaringLeafEntry	*last_e;
		BlockNumber			 right;

		leafbuf  = ReadBuffer(index, cur);
		LockBuffer(leafbuf, BUFFER_LOCK_SHARE);
		leafpage = BufferGetPage(leafbuf);
		lspc     = (RoaringLeafSpecial *) PageGetSpecialPointer(leafpage);
		maxoff   = PageGetMaxOffsetNumber(leafpage);
		right    = lspc->right_page;

		if (maxoff == 0)
		{
			UnlockReleaseBuffer(leafbuf);
			cur = right;
			continue;
		}

		last_e = (RoaringLeafEntry *)
			PageGetItem(leafpage, PageGetItemId(leafpage, maxoff));

		/* Skip page if all remaining targets lie beyond its max key. */
		if (vals[vi] > last_e->value)
		{
			UnlockReleaseBuffer(leafbuf);
			cur = right;
			continue;
		}

		/*
		 * Binary-search for each val that could live on this page.
		 * Since vals[] is sorted and the leaf is sorted, once we pass
		 * last_e->value we stop and move to the next page.
		 */
		while (vi < nvals && vals[vi] <= last_e->value)
		{
			OffsetNumber lo = 1, hi = maxoff;
			OffsetNumber found_off = InvalidOffsetNumber;

			while (lo <= hi)
			{
				OffsetNumber	  mid = (lo + hi) / 2;
				RoaringLeafEntry *e   = (RoaringLeafEntry *)
					PageGetItem(leafpage, PageGetItemId(leafpage, mid));

				if (e->value == vals[vi])
				{
					found_off = mid;
					break;
				}
				else if (e->value < vals[vi])
					lo = mid + 1;
				else
					hi = mid - 1;
			}

			if (found_off != InvalidOffsetNumber)
			{
				RoaringLeafEntry *le = (RoaringLeafEntry *)
					PageGetItem(leafpage, PageGetItemId(leafpage, found_off));

				if (le->cardinality > 0)
				{
					if (le->flags & ROARING_ENTRY_OVERFLOW)
					{
						/* Must release leaf buffer before reading overflow chain. */
						if (ndeferred == deferred_cap)
						{
							int newcap = (deferred_cap == 0) ? 4 : deferred_cap * 2;

							deferred = (deferred == NULL)
								? palloc(newcap * sizeof(DeferredOE))
								: repalloc(deferred, newcap * sizeof(DeferredOE));
							deferred_cap = newcap;
						}
						deferred[ndeferred].vi = vi;
						memcpy(&deferred[ndeferred].oe, le,
							   sizeof(RoaringOverflowEntry));
						ndeferred++;
					}
					else
					{
						Size item_len  = ItemIdGetLength(
							PageGetItemId(leafpage, found_off));
						Size bitmap_len;

						if (item_len < sizeof(RoaringLeafEntry))
							ereport(ERROR,
									(errcode(ERRCODE_INDEX_CORRUPTED),
									 errmsg("pg_roaring_index: leaf entry at "
											"offset %u is too short",
											found_off)));
						bitmap_len = item_len - sizeof(RoaringLeafEntry);
						bitmaps[vi] = roaring64_bitmap_portable_deserialize_safe(
								(const char *)(le + 1), bitmap_len);
						if (bitmaps[vi] == NULL)
							elog(ERROR,
								 "pg_roaring_index: failed to deserialize "
								 "bitmap for value " INT64_FORMAT, vals[vi]);
					}
				}
			}
			vi++;
		}

		UnlockReleaseBuffer(leafbuf);
		cur = right;
	}

	/* Resolve deferred overflow entries now that no leaf buffer is held. */
	for (j = 0; j < ndeferred; j++)
		bitmaps[deferred[j].vi] =
			roaring_read_overflow_bitmap(index, &deferred[j].oe);

	if (deferred != NULL)
		pfree(deferred);

fill_empty:
	/* Back-fill any remaining NULL slots (values not found) with empty bitmaps. */
	for (j = 0; j < nvals; j++)
		if (bitmaps[j] == NULL)
			bitmaps[j] = roaring64_bitmap_create();
}

/* ----------------------------------------------------------------
 * pending_chain_as_bitmap / pending_chain_as_bitmap_lossy
 *
 * Walk one pending list chain, collecting visible TIDs for scan_value.
 * The two variants differ only in bitmap type (roaring64 vs roaring32)
 * and the add call.  If you fix a bug in one, fix the other too.
 * ---------------------------------------------------------------- */
static roaring64_bitmap_t *
pending_chain_as_bitmap(Relation index, BlockNumber start_blkno,
						int64 scan_value, Snapshot snapshot)
{
	roaring64_bitmap_t *bm  = roaring64_bitmap_create();
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

		/*
		 * Guard against vacuum recycling a pending page between when we read
		 * next_page from the previous page and when we re-acquire SHARE here.
		 * A recycled page has a different page_type; stop the walk on mismatch.
		 */
		if (spc->page_type != ROARING_PAGE_PENDING_INSERT)
		{
			UnlockReleaseBuffer(buf);
			break;
		}

		if (spc->entry_count > ROARING_PENDING_PER_PAGE)
		{
			UnlockReleaseBuffer(buf);
			roaring64_bitmap_free(bm);
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
				roaring64_bitmap_add(bm, raw[k].linear_tid);
			}
		}

		cur = spc->next_page;
		UnlockReleaseBuffer(buf);
	}
	return bm;
}

/* ----------------------------------------------------------------
 * Lossy variants: bitmaps contain heap block numbers (uint32), not
 * linearized TIDs.  Block numbers always fit in uint32, so roaring32
 * is the correct (and more compact) representation.
 * ---------------------------------------------------------------- */
static roaring_bitmap_t *
lookup_value_as_bitmap_lossy(Relation index, BlockNumber root_blkno, int64 value)
{
	BlockNumber		 leaf_blkno;
	Buffer			 leafbuf;
	Page			 leafpage;
	OffsetNumber	 found_off;
	RoaringLeafEntry *le;
	roaring_bitmap_t *bm;

	leaf_blkno = roaring_dir_lookup(index, root_blkno, value);
	if (leaf_blkno == InvalidBlockNumber)
		return roaring_bitmap_create();

	for (;;)
	{
		OffsetNumber		lo,
							hi;
		RoaringLeafSpecial *lspc;

		leafbuf   = ReadBuffer(index, leaf_blkno);
		LockBuffer(leafbuf, BUFFER_LOCK_SHARE);
		leafpage  = BufferGetPage(leafbuf);
		found_off = InvalidOffsetNumber;
		le		  = NULL;

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
				le		  = e;
				found_off = mid;
				break;
			}
			else if (e->value < value)
				lo = mid + 1;
			else
				hi = mid - 1;
		}

		if (le != NULL)
			break;

		lspc = (RoaringLeafSpecial *) PageGetSpecialPointer(leafpage);
		if (lspc->right_page != InvalidBlockNumber)
		{
			OffsetNumber maxoff = PageGetMaxOffsetNumber(leafpage);

			if (maxoff >= 1)
			{
				RoaringLeafEntry *last = (RoaringLeafEntry *)
					PageGetItem(leafpage, PageGetItemId(leafpage, maxoff));

				if (last->value < value)
				{
					BlockNumber right = lspc->right_page;

					UnlockReleaseBuffer(leafbuf);
					leaf_blkno = right;
					continue;
				}
			}
		}

		UnlockReleaseBuffer(leafbuf);
		return roaring_bitmap_create();
	}

	if (le->cardinality == 0)
	{
		UnlockReleaseBuffer(leafbuf);
		return roaring_bitmap_create();
	}

	if (le->flags & ROARING_ENTRY_OVERFLOW)
	{
		RoaringOverflowEntry oe_copy;

		memcpy(&oe_copy, le, sizeof(RoaringOverflowEntry));
		UnlockReleaseBuffer(leafbuf);
		bm = roaring_read_overflow_bitmap_lossy(index, &oe_copy);
	}
	else
	{
		Size item_len = ItemIdGetLength(PageGetItemId(leafpage, found_off));
		Size bitmap_len;

		if (item_len < sizeof(RoaringLeafEntry))
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("pg_roaring_index: leaf entry at offset %u is too short",
							found_off)));
		bitmap_len = item_len - sizeof(RoaringLeafEntry);
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

		if (spc->page_type != ROARING_PAGE_PENDING_INSERT)
		{
			UnlockReleaseBuffer(buf);
			break;
		}

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
				roaring_bitmap_add(bm, (uint32) raw[k].linear_tid);
			}
		}

		cur = spc->next_page;
		UnlockReleaseBuffer(buf);
	}
	return bm;
}

static void
lookup_values_as_bitmaps_leaf_walk_lossy(Relation index, BlockNumber root_blkno,
										  const int64 *vals, roaring_bitmap_t **bitmaps,
										  int nvals)
{
	BlockNumber cur;
	int			vi = 0;
	int			j;

	typedef struct { int vi; RoaringOverflowEntry oe; } DeferredOEL;
	DeferredOEL *deferred     = NULL;
	int			 deferred_cap = 0;
	int			 ndeferred    = 0;

	memset(bitmaps, 0, (size_t) nvals * sizeof(roaring_bitmap_t *));

	cur = roaring_dir_lookup(index, root_blkno, vals[0]);
	if (cur == InvalidBlockNumber)
		goto fill_empty_lossy;

	while (cur != InvalidBlockNumber && vi < nvals)
	{
		Buffer				 leafbuf;
		Page				 leafpage;
		RoaringLeafSpecial	*lspc;
		OffsetNumber		 maxoff;
		RoaringLeafEntry	*last_e;
		BlockNumber			 right;

		leafbuf  = ReadBuffer(index, cur);
		LockBuffer(leafbuf, BUFFER_LOCK_SHARE);
		leafpage = BufferGetPage(leafbuf);
		lspc     = (RoaringLeafSpecial *) PageGetSpecialPointer(leafpage);
		maxoff   = PageGetMaxOffsetNumber(leafpage);
		right    = lspc->right_page;

		if (maxoff == 0)
		{
			UnlockReleaseBuffer(leafbuf);
			cur = right;
			continue;
		}

		last_e = (RoaringLeafEntry *)
			PageGetItem(leafpage, PageGetItemId(leafpage, maxoff));

		if (vals[vi] > last_e->value)
		{
			UnlockReleaseBuffer(leafbuf);
			cur = right;
			continue;
		}

		while (vi < nvals && vals[vi] <= last_e->value)
		{
			OffsetNumber lo = 1, hi = maxoff;
			OffsetNumber found_off = InvalidOffsetNumber;

			while (lo <= hi)
			{
				OffsetNumber	  mid = (lo + hi) / 2;
				RoaringLeafEntry *e   = (RoaringLeafEntry *)
					PageGetItem(leafpage, PageGetItemId(leafpage, mid));

				if (e->value == vals[vi])
				{
					found_off = mid;
					break;
				}
				else if (e->value < vals[vi])
					lo = mid + 1;
				else
					hi = mid - 1;
			}

			if (found_off != InvalidOffsetNumber)
			{
				RoaringLeafEntry *le = (RoaringLeafEntry *)
					PageGetItem(leafpage, PageGetItemId(leafpage, found_off));

				if (le->cardinality > 0)
				{
					if (le->flags & ROARING_ENTRY_OVERFLOW)
					{
						if (ndeferred == deferred_cap)
						{
							int newcap = (deferred_cap == 0) ? 4 : deferred_cap * 2;

							deferred = (deferred == NULL)
								? palloc(newcap * sizeof(DeferredOEL))
								: repalloc(deferred, newcap * sizeof(DeferredOEL));
							deferred_cap = newcap;
						}
						deferred[ndeferred].vi = vi;
						memcpy(&deferred[ndeferred].oe, le,
							   sizeof(RoaringOverflowEntry));
						ndeferred++;
					}
					else
					{
						Size item_len  = ItemIdGetLength(
							PageGetItemId(leafpage, found_off));
						Size bitmap_len;

						if (item_len < sizeof(RoaringLeafEntry))
							ereport(ERROR,
									(errcode(ERRCODE_INDEX_CORRUPTED),
									 errmsg("pg_roaring_index: leaf entry at "
											"offset %u is too short",
											found_off)));
						bitmap_len = item_len - sizeof(RoaringLeafEntry);
						bitmaps[vi] = roaring_bitmap_portable_deserialize_safe(
								(const char *)(le + 1), bitmap_len);
						if (bitmaps[vi] == NULL)
							elog(ERROR,
								 "pg_roaring_index: failed to deserialize "
								 "bitmap for value " INT64_FORMAT, vals[vi]);
					}
				}
			}
			vi++;
		}

		UnlockReleaseBuffer(leafbuf);
		cur = right;
	}

	for (j = 0; j < ndeferred; j++)
		bitmaps[deferred[j].vi] =
			roaring_read_overflow_bitmap_lossy(index, &deferred[j].oe);

	if (deferred != NULL)
		pfree(deferred);

fill_empty_lossy:
	for (j = 0; j < nvals; j++)
		if (bitmaps[j] == NULL)
			bitmaps[j] = roaring_bitmap_create();
}

static void
pending_chain_fill_values_lossy(Relation index, BlockNumber start_blkno,
								const int64 *values, roaring_bitmap_t **bitmaps,
								int nvalues, Snapshot snapshot)
{
	BlockNumber cur = start_blkno;

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

		if (values[0] <= spc->value_max && values[nvalues - 1] >= spc->value_min)
		{
			for (k = 0; k < spc->entry_count; k++)
			{
				int64 v  = raw[k].value;
				int   lo = 0;
				int   hi = nvalues - 1;

				while (lo <= hi)
				{
					int mid = (lo + hi) / 2;

					if (values[mid] == v)
					{
						if (roaring_pending_visible(raw[k].xmin, snapshot))
							roaring_bitmap_add(bitmaps[mid], (uint32) raw[k].linear_tid);
						break;
					}
					else if (values[mid] < v)
						lo = mid + 1;
					else
						hi = mid - 1;
				}
			}
		}

		cur = spc->next_page;
		UnlockReleaseBuffer(buf);
	}
}

/* ----------------------------------------------------------------
 * int64_cmp_for_sort — qsort comparator for int64 arrays
 * ---------------------------------------------------------------- */
static int
int64_cmp_for_sort(const void *a, const void *b)
{
	int64 x = *(const int64 *) a;
	int64 y = *(const int64 *) b;

	if (x < y) return -1;
	if (x > y) return 1;
	return 0;
}

/* ----------------------------------------------------------------
 * pending_chain_fill_values
 *
 * Walk one pending list chain once, adding visible TIDs into bitmaps[i]
 * for every pending entry whose value equals values[i].
 * values[] must be sorted ascending with no duplicates; bitmaps[] must
 * be pre-allocated.  Caller frees each bitmap.
 *
 * Two variants: exact (roaring64, linearized TIDs) and lossy (roaring32,
 * block numbers).  Both share identical chain-walk logic.
 * ---------------------------------------------------------------- */
static void
pending_chain_fill_values_exact(Relation index, BlockNumber start_blkno,
								const int64 *values, roaring64_bitmap_t **bitmaps,
								int nvalues, Snapshot snapshot)
{
	BlockNumber cur = start_blkno;

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

		if (values[0] <= spc->value_max && values[nvalues - 1] >= spc->value_min)
		{
			for (k = 0; k < spc->entry_count; k++)
			{
				int64 v  = raw[k].value;
				int   lo = 0;
				int   hi = nvalues - 1;

				while (lo <= hi)
				{
					int mid = (lo + hi) / 2;

					if (values[mid] == v)
					{
						if (roaring_pending_visible(raw[k].xmin, snapshot))
							roaring64_bitmap_add(bitmaps[mid], raw[k].linear_tid);
						break;
					}
					else if (values[mid] < v)
						lo = mid + 1;
					else
						hi = mid - 1;
				}
			}
		}

		cur = spc->next_page;
		UnlockReleaseBuffer(buf);
	}
}

/* ----------------------------------------------------------------
 * peek_value_cardinality
 *
 * Return the stored leaf cardinality for value without deserializing
 * the bitmap.  Returns 0 if value is absent from the main index.
 * Used to order multi-column AND keys smallest-first.
 * ---------------------------------------------------------------- */
static uint32
peek_value_cardinality(Relation index, BlockNumber root_blkno, int64 value)
{
	BlockNumber		  leaf_blkno;

	leaf_blkno = roaring_dir_lookup(index, root_blkno, value);
	if (leaf_blkno == InvalidBlockNumber)
		return 0;

	for (;;)
	{
		Buffer			   leafbuf;
		Page			   leafpage;
		RoaringLeafSpecial *lspc;
		OffsetNumber	   lo,
						   hi;

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
				uint32 card = e->cardinality;

				UnlockReleaseBuffer(leafbuf);
				return card;
			}
			else if (e->value < value)
				lo = mid + 1;
			else
				hi = mid - 1;
		}

		/*
		 * Binary search miss.  A concurrent merge may have split this leaf
		 * after our directory lookup, moving the target value to the right
		 * sibling.  Follow the right link if the target is beyond this
		 * leaf's max key (_bt_moveright pattern).
		 */
		lspc = (RoaringLeafSpecial *) PageGetSpecialPointer(leafpage);
		if (lspc->right_page != InvalidBlockNumber)
		{
			OffsetNumber maxoff = PageGetMaxOffsetNumber(leafpage);

			if (maxoff >= 1)
			{
				RoaringLeafEntry *last = (RoaringLeafEntry *)
					PageGetItem(leafpage, PageGetItemId(leafpage, maxoff));

				if (last->value < value)
				{
					BlockNumber right = lspc->right_page;

					UnlockReleaseBuffer(leafbuf);
					leaf_blkno = right;
					continue;
				}
			}
		}

		UnlockReleaseBuffer(leafbuf);
		return 0;
	}
}

typedef struct
{
	int    ki;
	uint64 card_est;
} ScanKeyOrder;

static int
scankey_order_cmp(const void *a, const void *b)
{
	const ScanKeyOrder *x = (const ScanKeyOrder *) a;
	const ScanKeyOrder *y = (const ScanKeyOrder *) b;

	if (x->card_est < y->card_est) return -1;
	if (x->card_est > y->card_est) return 1;
	return 0;
}

/* ----------------------------------------------------------------
 * emit_exact_bitmap_to_tbm
 *
 * Decode linearized TIDs from bm and add to tbm via tid_buf batching.
 * Returns TID count.
 * ---------------------------------------------------------------- */
static int64
emit_exact_bitmap_to_tbm(roaring64_bitmap_t *bm, TIDBitmap *tbm,
						  ItemPointerData *tid_buf, int *tid_count_p,
						  bool recheck)
{
	roaring64_iterator_t *it;
	int64				  ntids = 0;

	it = roaring64_iterator_create(bm);
	while (roaring64_iterator_has_value(it))
	{
		uint64		 linear = roaring64_iterator_value(it);
		BlockNumber  block  = (BlockNumber)(linear >> 9);
		OffsetNumber off    = (OffsetNumber)((linear & 0x1FF) + 1);

		if (*tid_count_p == ROARING_TID_BATCH)
		{
			tbm_add_tuples(tbm, tid_buf, *tid_count_p, recheck);
			*tid_count_p = 0;
		}
		ItemPointerSet(&tid_buf[(*tid_count_p)++], block, off);
		ntids++;
		roaring64_iterator_advance(it);
	}
	roaring64_iterator_free(it);
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
 * roaring_getbitmap / roaring_getbitmap_lossy
 *
 * Near-verbatim counterparts differing in bitmap type (roaring64 vs
 * roaring32), TBM addition (tbm_add_tuples vs tbm_add_page), and
 * linear_tid encoding (TID vs block number).
 *
 * IMPORTANT: if you fix a bug in one function, apply the same fix to
 * the other.  See also pending_chain_as_bitmap / _lossy and
 * lookup_value_as_bitmap / _lossy which share the same duality.
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
	BlockNumber			insert_heads[ROARING_PENDING_SHARDS];
	BlockNumber			merging_heads[ROARING_PENDING_SHARDS];
	uint32				pending_count_approx;
	bool				any_pending;
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

		if (cache != NULL && cache->pending_count_approx == 0)
		{
			bool	   any_merging = false;
			int		   si;

			for (si = 0; si < ROARING_PENDING_SHARDS; si++)
				if (cache->merging_head[si] != InvalidBlockNumber)
				{ any_merging = true; break; }

			if (!any_merging)
			{
				Buffer     tmp     = ReadBuffer(index, ROARING_METAPAGE_BLKNO);
				XLogRecPtr cur_lsn = BufferGetLSNAtomic(tmp);

				ReleaseBuffer(tmp);

				if (cur_lsn == cache->meta_lsn)
				{
					int k;

					root_blkno    = cache->root_blkno;
					pending_count_approx = 0;
					for (k = 0; k < ROARING_PENDING_SHARDS; k++)
					{
						insert_heads[k]  = cache->insert_head[k];
						merging_heads[k] = InvalidBlockNumber;
					}
					/*
					 * insert_count is updated only on page extension, not on
					 * every hot-path insert.  pending_count_approx == 0 does NOT mean
					 * the chains are empty.  Always walk the chains for
					 * correctness; head pages are typically in shared_buffers.
					 */
					any_pending = true;
					goto after_meta;
				}
			}
		}

		metabuf = ReadBuffer(index, ROARING_METAPAGE_BLKNO);
		LockBuffer(metabuf, BUFFER_LOCK_SHARE);
		meta = RoaringPageGetMeta(BufferGetPage(metabuf));
		roaring_validate_metapage(index, meta);
		root_blkno    = meta->root_directory_page;
		{
			uint32 total = 0;
			int    si;

			for (si = 0; si < ROARING_PENDING_SHARDS; si++)
			{
				total               += meta->shards[si].insert_count;
				insert_heads[si]     = meta->shards[si].insert_head;
				merging_heads[si]    = meta->shards[si].merging_head;
			}
			pending_count_approx = total;
		}

		if (cache == NULL)
		{
			cache = (RoaringAmCache *)
				MemoryContextAllocZero(CacheMemoryContext,
									   sizeof(RoaringAmCache));
			index->rd_amcache = cache;
		}
		{
			int si;

			for (si = 0; si < ROARING_PENDING_SHARDS; si++)
			{
				cache->insert_head[si]  = insert_heads[si];
				cache->merging_head[si] = merging_heads[si];
			}
		}
		cache->root_blkno    = root_blkno;
		cache->total_entries = meta->total_entries;
		cache->meta_lsn      = PageGetLSN(BufferGetPage(metabuf));
		cache->pending_count_approx = pending_count_approx;
		UnlockReleaseBuffer(metabuf);

		{
			bool   any_merging = false;
			int    si;

			for (si = 0; si < ROARING_PENDING_SHARDS; si++)
				if (merging_heads[si] != InvalidBlockNumber)
				{ any_merging = true; break; }
			any_pending = (pending_count_approx > 0 || any_merging);
		}
	}
	after_meta:;

	snapshot = scan->xs_snapshot;

	/*
	 * Multi-column path: look up per-column bitmaps, AND them together, emit
	 * to tbm.  Keys are processed in cardinality order (smallest first) so
	 * each AND step narrows the running result as quickly as possible.
	 * SK_SEARCHARRAY keys use a single pending-chain walk for all IN values.
	 */
	if (index->rd_att->natts > 1)
	{
		roaring64_bitmap_t * volatile result = NULL;
		roaring64_bitmap_t * volatile col_bm = NULL;
		ScanKeyOrder	   *order;
		int ki;

		/* Build cardinality estimates, sort keys ascending. */
		order = palloc(scan->numberOfKeys * sizeof(ScanKeyOrder));
		for (ki = 0; ki < scan->numberOfKeys; ki++)
		{
			ScanKey k     = &scan->keyData[ki];
			Oid		typid = TupleDescAttr(index->rd_att, k->sk_attno - 1)->atttypid;

			order[ki].ki = ki;
			if (k->sk_flags & (SK_ISNULL | SK_SEARCHARRAY))
				order[ki].card_est = UINT64_MAX;
			else if (typid == FLOAT4OID && isnan(DatumGetFloat4(k->sk_argument)))
				order[ki].card_est = 0;		/* NaN matches nothing; process first */
			else
			{
				int64 col_key = ROARING_COL_KEY(k->sk_attno,
												 roaring_datum_to_key32(k->sk_argument, typid));

				order[ki].card_est = peek_value_cardinality(index, root_blkno, col_key);
			}

			/*
			 * Absent key with no pending list: the AND result is provably
			 * empty.  Stop peeking immediately; skip sort and bitmap work.
			 * Not safe when pending_count_approx > 0 — that column might have
			 * freshly inserted rows not yet merged into the main index.
			 */
			if (order[ki].card_est == 0 && !any_pending)
			{
				pfree(order);
				return 0;
			}
		}
		qsort(order, scan->numberOfKeys, sizeof(ScanKeyOrder), scankey_order_cmp);

		PG_TRY();
		{
			for (ki = 0; ki < scan->numberOfKeys; ki++)
			{
				ScanKey k = &scan->keyData[order[ki].ki];

				if (k->sk_flags & SK_ISNULL)
					continue;

				if (k->sk_flags & SK_SEARCHARRAY)
				{
					Oid		   col_typid = TupleDescAttr(index->rd_att,
														 k->sk_attno - 1)->atttypid;
					ArrayType *arr		 = DatumGetArrayTypeP(k->sk_argument);
					Datum	  *elems;
					bool	  *nulls;
					int		   nelems, i;
					int16	   typlen;
					bool	   typbyval;
					char	   typalign;
					int64	  *col_keys;
					int		   nkeys	 = 0;

					get_typlenbyvalalign(col_typid, &typlen, &typbyval, &typalign);
					deconstruct_array(arr, col_typid, typlen, typbyval, typalign,
									  &elems, &nulls, &nelems);
					col_bm	 = roaring64_bitmap_create();
					col_keys = palloc(nelems * sizeof(int64));

					for (i = 0; i < nelems; i++)
					{
						if (nulls[i])
							continue;
						if (col_typid == FLOAT4OID &&
							isnan(DatumGetFloat4(elems[i])))
							continue;
						col_keys[nkeys++] = ROARING_COL_KEY(
							k->sk_attno,
							roaring_datum_to_key32(elems[i], col_typid));
					}
					pfree(elems);
					pfree(nulls);

					if (nkeys > 0)
					{
						/*
						 * pbms and vbms are declared volatile so the nested PG_CATCH
						 * can free them safely on error (T15).  palloc0 ensures
						 * uninitialized slots are NULL so catch-side iteration is safe
						 * even if roaring64_bitmap_create() throws mid-loop (T16).
						 */
						roaring64_bitmap_t ** volatile pbms_v = NULL;
						roaring64_bitmap_t ** volatile vbms_v = NULL;
						int j, m;

						qsort(col_keys, nkeys, sizeof(int64), int64_cmp_for_sort);
						for (j = 1, m = 1; j < nkeys; j++)
							if (col_keys[j] != col_keys[j - 1])
								col_keys[m++] = col_keys[j];
						nkeys = m;

						PG_TRY();
						{
							if (any_pending)
							{
								int s;

								pbms_v = palloc0(nkeys * sizeof(roaring64_bitmap_t *));
								for (j = 0; j < nkeys; j++)
									pbms_v[j] = roaring64_bitmap_create();
								for (s = 0; s < ROARING_PENDING_SHARDS; s++)
								{
									pending_chain_fill_values_exact(
										index, insert_heads[s],
										col_keys, (roaring64_bitmap_t **) pbms_v,
										nkeys, snapshot);
									if (merging_heads[s] != InvalidBlockNumber)
										pending_chain_fill_values_exact(
											index, merging_heads[s],
											col_keys, (roaring64_bitmap_t **) pbms_v,
											nkeys, snapshot);
								}
							}

							vbms_v = palloc0(nkeys * sizeof(roaring64_bitmap_t *));
							lookup_values_as_bitmaps_leaf_walk(
								index, root_blkno, col_keys,
								(roaring64_bitmap_t **) vbms_v, nkeys);
							for (j = 0; j < nkeys; j++)
							{
								if (pbms_v != NULL)
								{
									roaring64_bitmap_or_inplace(vbms_v[j], pbms_v[j]);
									roaring64_bitmap_free(pbms_v[j]);
								}
								roaring64_bitmap_or_inplace(col_bm, vbms_v[j]);
								roaring64_bitmap_free(vbms_v[j]);
							}
							pfree((void *) vbms_v);
							vbms_v = NULL;
							if (pbms_v != NULL)
							{
								pfree((void *) pbms_v);
								pbms_v = NULL;
							}
						}
						PG_CATCH();
						{
							if (vbms_v)
							{
								for (j = 0; j < nkeys; j++)
									if (vbms_v[j])
										roaring64_bitmap_free(vbms_v[j]);
								pfree((void *) vbms_v);
							}
							if (pbms_v)
							{
								for (j = 0; j < nkeys; j++)
									if (pbms_v[j])
										roaring64_bitmap_free(pbms_v[j]);
								pfree((void *) pbms_v);
							}
							pfree(col_keys);
							PG_RE_THROW();
						}
						PG_END_TRY();
					}
					pfree(col_keys);
				}
				else
				{
					Oid		col_typid = TupleDescAttr(index->rd_att,
													  k->sk_attno - 1)->atttypid;
					int64	col_key;

					if (col_typid == FLOAT4OID &&
						isnan(DatumGetFloat4(k->sk_argument)))
					{
						col_bm = roaring64_bitmap_create();
					}
					else
					{
						int s;

						col_key = ROARING_COL_KEY(k->sk_attno,
												   roaring_datum_to_key32(k->sk_argument,
																		   col_typid));
						col_bm = lookup_value_as_bitmap(index, root_blkno, col_key);
						if (any_pending)
						{
							for (s = 0; s < ROARING_PENDING_SHARDS; s++)
							{
								roaring64_bitmap_t *pbm =
									pending_chain_as_bitmap(index, insert_heads[s],
															 col_key, snapshot);
								roaring64_bitmap_or_inplace(col_bm, pbm);
								roaring64_bitmap_free(pbm);
								if (merging_heads[s] != InvalidBlockNumber)
								{
									pbm = pending_chain_as_bitmap(index, merging_heads[s],
																   col_key, snapshot);
									roaring64_bitmap_or_inplace(col_bm, pbm);
									roaring64_bitmap_free(pbm);
								}
							}
						}
					}
				}

				if (result == NULL)
					result = col_bm;
				else
				{
					roaring64_bitmap_and_inplace(result, col_bm);
					roaring64_bitmap_free(col_bm);
					/* Early exit: no rows can match. */
					if (roaring64_bitmap_is_empty(result))
					{
						col_bm = NULL;
						break;
					}
				}
				col_bm = NULL;
			}

			if (result != NULL)
			{
				ntids = emit_exact_bitmap_to_tbm(result, tbm, tid_buf, &tid_count,
												  so->needs_recheck);
				roaring64_bitmap_free(result);
				result = NULL;
			}
		}
		PG_CATCH();
		{
			pfree(order);
			if (col_bm)
				roaring64_bitmap_free(col_bm);
			if (result)
				roaring64_bitmap_free(result);
			PG_RE_THROW();
		}
		PG_END_TRY();

		pfree(order);

		if (tid_count > 0)
			tbm_add_tuples(tbm, tid_buf, tid_count, so->needs_recheck);

		return ntids;
	}

	/*
	 * Single-column path.
	 */
	{
		ScanKey key      = &scan->keyData[0];
		Oid		atttypid = TupleDescAttr(index->rd_att, 0)->atttypid;

		if (key->sk_flags & SK_SEARCHARRAY)
		{
			ArrayType *arr = DatumGetArrayTypeP(key->sk_argument);
			Datum	  *elems;
			bool	  *nulls;
			int		   nelems, i;
			int16	   typlen;
			bool	   typbyval;
			char	   typalign;
			int64	  *vals;
			int		   nvals = 0;
			roaring64_bitmap_t ** volatile bms  = NULL;
			roaring64_bitmap_t ** volatile pbms = NULL;

			get_typlenbyvalalign(atttypid, &typlen, &typbyval, &typalign);
			deconstruct_array(arr, atttypid, typlen, typbyval, typalign,
							  &elems, &nulls, &nelems);

			vals = palloc(nelems * sizeof(int64));
			for (i = 0; i < nelems; i++)
			{
				if (nulls[i])
					continue;
				if (atttypid == FLOAT4OID && isnan(DatumGetFloat4(elems[i])))
					continue;
				vals[nvals++] = roaring_datum_to_key64(elems[i], atttypid);
			}
			pfree(elems);
			pfree(nulls);

			if (nvals > 0)
			{
				int j, m;

				qsort(vals, nvals, sizeof(int64), int64_cmp_for_sort);
				for (j = 1, m = 1; j < nvals; j++)
					if (vals[j] != vals[j - 1])
						vals[m++] = vals[j];
				nvals = m;

				PG_TRY();
				{
					if (any_pending)
					{
						int s;

						pbms = palloc0(nvals * sizeof(roaring64_bitmap_t *));
						for (j = 0; j < nvals; j++)
							pbms[j] = roaring64_bitmap_create();
						for (s = 0; s < ROARING_PENDING_SHARDS; s++)
						{
							pending_chain_fill_values_exact(index, insert_heads[s],
															vals, (roaring64_bitmap_t **) pbms,
															nvals, snapshot);
							if (merging_heads[s] != InvalidBlockNumber)
								pending_chain_fill_values_exact(index, merging_heads[s],
																vals, (roaring64_bitmap_t **) pbms,
																nvals, snapshot);
						}
					}

					bms = palloc0(nvals * sizeof(roaring64_bitmap_t *));
					lookup_values_as_bitmaps_leaf_walk(index, root_blkno,
													   vals, (roaring64_bitmap_t **) bms,
													   nvals);
					for (j = 0; j < nvals; j++)
					{
						if (pbms != NULL)
						{
							roaring64_bitmap_or_inplace(bms[j], pbms[j]);
							roaring64_bitmap_free(pbms[j]);
							pbms[j] = NULL;
						}
						ntids += emit_exact_bitmap_to_tbm(bms[j], tbm, tid_buf,
														  &tid_count,
														  so->needs_recheck);
						roaring64_bitmap_free(bms[j]);
						bms[j] = NULL;
					}
					pfree((void *) bms);
					bms = NULL;
					if (pbms != NULL)
					{
						pfree((void *) pbms);
						pbms = NULL;
					}
				}
				PG_CATCH();
				{
					if (bms)
					{
						for (j = 0; j < nvals; j++)
							if (bms[j])
								roaring64_bitmap_free(bms[j]);
						pfree((void *) bms);
					}
					if (pbms)
					{
						for (j = 0; j < nvals; j++)
							if (pbms[j])
								roaring64_bitmap_free(pbms[j]);
						pfree((void *) pbms);
					}
					PG_RE_THROW();
				}
				PG_END_TRY();
			}

			pfree(vals);
		}
		else
		{
			roaring64_bitmap_t * volatile bm = NULL;
			int64 scan_value;

			if (atttypid == FLOAT4OID && isnan(DatumGetFloat4(key->sk_argument)))
				goto single_exact_done;

			scan_value = roaring_datum_to_key64(key->sk_argument, atttypid);

			PG_TRY();
			{
				bm = lookup_value_as_bitmap(index, root_blkno, scan_value);
				if (any_pending)
				{
					int s;

					for (s = 0; s < ROARING_PENDING_SHARDS; s++)
					{
						roaring64_bitmap_t *pbm =
							pending_chain_as_bitmap(index, insert_heads[s],
													scan_value, snapshot);
						roaring64_bitmap_or_inplace(bm, pbm);
						roaring64_bitmap_free(pbm);
						if (merging_heads[s] != InvalidBlockNumber)
						{
							pbm = pending_chain_as_bitmap(index, merging_heads[s],
														  scan_value, snapshot);
							roaring64_bitmap_or_inplace(bm, pbm);
							roaring64_bitmap_free(pbm);
						}
					}
				}
				ntids = emit_exact_bitmap_to_tbm(bm, tbm, tid_buf, &tid_count,
												 so->needs_recheck);
				roaring64_bitmap_free(bm);
				bm = NULL;
			}
			PG_CATCH();
			{
				if (bm)
					roaring64_bitmap_free(bm);
				PG_RE_THROW();
			}
			PG_END_TRY();
		}
		single_exact_done:;
	}

	if (tid_count > 0)
		tbm_add_tuples(tbm, tid_buf, tid_count, so->needs_recheck);

	return ntids;
}

/* ----------------------------------------------------------------
 * roaring_getbitmap_lossy
 *
 * Lossy (page-level) scan.  Identical structure to roaring_getbitmap
 * (see its comment above) but uses roaring32 bitmaps and tbm_add_page.
 * IMPORTANT: if you fix a bug here, apply the same fix to
 * roaring_getbitmap and the _lossy helper functions above.
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
	BlockNumber			insert_heads[ROARING_PENDING_SHARDS];
	BlockNumber			merging_heads[ROARING_PENDING_SHARDS];
	uint32				pending_count_approx;
	bool				any_pending;
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

		if (cache != NULL && cache->pending_count_approx == 0)
		{
			bool	   any_merging = false;
			int		   si;

			for (si = 0; si < ROARING_PENDING_SHARDS; si++)
				if (cache->merging_head[si] != InvalidBlockNumber)
				{ any_merging = true; break; }

			if (!any_merging)
			{
				Buffer     tmp     = ReadBuffer(index, ROARING_METAPAGE_BLKNO);
				XLogRecPtr cur_lsn = BufferGetLSNAtomic(tmp);

				ReleaseBuffer(tmp);

				if (cur_lsn == cache->meta_lsn)
				{
					int k;

					root_blkno    = cache->root_blkno;
					pending_count_approx = 0;
					for (k = 0; k < ROARING_PENDING_SHARDS; k++)
					{
						insert_heads[k]  = cache->insert_head[k];
						merging_heads[k] = InvalidBlockNumber;
					}
					/*
					 * insert_count is updated only on page extension, not on
					 * every hot-path insert. pending_count_approx == 0 does NOT mean
					 * the chains are empty. Always walk the chains for
					 * correctness; head pages are typically in shared_buffers.
					 */
					any_pending = true;
					goto after_meta_lossy;
				}
			}
		}

		metabuf = ReadBuffer(index, ROARING_METAPAGE_BLKNO);
		LockBuffer(metabuf, BUFFER_LOCK_SHARE);
		meta = RoaringPageGetMeta(BufferGetPage(metabuf));
		roaring_validate_metapage(index, meta);
		root_blkno    = meta->root_directory_page;
		{
			uint32 total = 0;
			int    si;

			for (si = 0; si < ROARING_PENDING_SHARDS; si++)
			{
				total               += meta->shards[si].insert_count;
				insert_heads[si]     = meta->shards[si].insert_head;
				merging_heads[si]    = meta->shards[si].merging_head;
			}
			pending_count_approx = total;
		}

		if (cache == NULL)
		{
			cache = (RoaringAmCache *)
				MemoryContextAllocZero(CacheMemoryContext,
									   sizeof(RoaringAmCache));
			index->rd_amcache = cache;
		}
		{
			int si;

			for (si = 0; si < ROARING_PENDING_SHARDS; si++)
			{
				cache->insert_head[si]  = insert_heads[si];
				cache->merging_head[si] = merging_heads[si];
			}
		}
		cache->root_blkno    = root_blkno;
		cache->total_entries = meta->total_entries;
		cache->meta_lsn      = PageGetLSN(BufferGetPage(metabuf));
		cache->pending_count_approx = pending_count_approx;
		UnlockReleaseBuffer(metabuf);

		{
			bool   any_merging = false;
			int    si;

			for (si = 0; si < ROARING_PENDING_SHARDS; si++)
				if (merging_heads[si] != InvalidBlockNumber)
				{ any_merging = true; break; }
			any_pending = (pending_count_approx > 0 || any_merging);
		}
	}
	after_meta_lossy:;

	snapshot = scan->xs_snapshot;

	/* Multi-column: AND bitmaps across scan keys (block-number bitmaps). */
	if (index->rd_att->natts > 1)
	{
		roaring_bitmap_t * volatile result = NULL;
		roaring_bitmap_t * volatile col_bm = NULL;
		ScanKeyOrder	   *order;
		int ki;

		/* Build cardinality estimates, sort keys ascending. */
		order = palloc(scan->numberOfKeys * sizeof(ScanKeyOrder));
		for (ki = 0; ki < scan->numberOfKeys; ki++)
		{
			ScanKey k     = &scan->keyData[ki];
			Oid		typid = TupleDescAttr(index->rd_att, k->sk_attno - 1)->atttypid;

			order[ki].ki = ki;
			if (k->sk_flags & (SK_ISNULL | SK_SEARCHARRAY))
				order[ki].card_est = UINT64_MAX;
			else if (typid == FLOAT4OID && isnan(DatumGetFloat4(k->sk_argument)))
				order[ki].card_est = 0;
			else
			{
				int64 col_key = ROARING_COL_KEY(k->sk_attno,
												 roaring_datum_to_key32(k->sk_argument, typid));

				order[ki].card_est = peek_value_cardinality(index, root_blkno, col_key);
			}

			/* Absent key, no pending list → AND result is empty. */
			if (order[ki].card_est == 0 && !any_pending)
			{
				pfree(order);
				return 0;
			}
		}
		qsort(order, scan->numberOfKeys, sizeof(ScanKeyOrder), scankey_order_cmp);

		PG_TRY();
		{
			for (ki = 0; ki < scan->numberOfKeys; ki++)
			{
				ScanKey k = &scan->keyData[order[ki].ki];

				if (k->sk_flags & SK_ISNULL)
					continue;

				if (k->sk_flags & SK_SEARCHARRAY)
				{
					Oid		   col_typid = TupleDescAttr(index->rd_att,
														 k->sk_attno - 1)->atttypid;
					ArrayType *arr		 = DatumGetArrayTypeP(k->sk_argument);
					Datum	  *elems;
					bool	  *nulls;
					int		   nelems, i;
					int16	   typlen;
					bool	   typbyval;
					char	   typalign;
					int64	  *col_keys;
					int		   nkeys	 = 0;

					get_typlenbyvalalign(col_typid, &typlen, &typbyval, &typalign);
					deconstruct_array(arr, col_typid, typlen, typbyval, typalign,
									  &elems, &nulls, &nelems);
					col_bm	 = roaring_bitmap_create();
					col_keys = palloc(nelems * sizeof(int64));

					for (i = 0; i < nelems; i++)
					{
						if (nulls[i])
							continue;
						if (col_typid == FLOAT4OID &&
							isnan(DatumGetFloat4(elems[i])))
							continue;
						col_keys[nkeys++] = ROARING_COL_KEY(
							k->sk_attno,
							roaring_datum_to_key32(elems[i], col_typid));
					}
					pfree(elems);
					pfree(nulls);

					if (nkeys > 0)
					{
						roaring_bitmap_t ** volatile pbms_v = NULL;
						roaring_bitmap_t ** volatile vbms_v = NULL;
						int j, m;

						qsort(col_keys, nkeys, sizeof(int64), int64_cmp_for_sort);
						for (j = 1, m = 1; j < nkeys; j++)
							if (col_keys[j] != col_keys[j - 1])
								col_keys[m++] = col_keys[j];
						nkeys = m;

						PG_TRY();
						{
							if (any_pending)
							{
								int s;

								pbms_v = palloc0(nkeys * sizeof(roaring_bitmap_t *));
								for (j = 0; j < nkeys; j++)
									pbms_v[j] = roaring_bitmap_create();
								for (s = 0; s < ROARING_PENDING_SHARDS; s++)
								{
									pending_chain_fill_values_lossy(
										index, insert_heads[s],
										col_keys, (roaring_bitmap_t **) pbms_v,
										nkeys, snapshot);
									if (merging_heads[s] != InvalidBlockNumber)
										pending_chain_fill_values_lossy(
											index, merging_heads[s],
											col_keys, (roaring_bitmap_t **) pbms_v,
											nkeys, snapshot);
								}
							}

							vbms_v = palloc0(nkeys * sizeof(roaring_bitmap_t *));
							lookup_values_as_bitmaps_leaf_walk_lossy(
								index, root_blkno, col_keys,
								(roaring_bitmap_t **) vbms_v, nkeys);
							for (j = 0; j < nkeys; j++)
							{
								if (pbms_v != NULL)
								{
									roaring_bitmap_or_inplace(vbms_v[j], pbms_v[j]);
									roaring_bitmap_free(pbms_v[j]);
								}
								roaring_bitmap_or_inplace(
									(roaring_bitmap_t *) col_bm, vbms_v[j]);
								roaring_bitmap_free(vbms_v[j]);
							}
							pfree((void *) vbms_v);
							vbms_v = NULL;
							if (pbms_v != NULL)
							{
								pfree((void *) pbms_v);
								pbms_v = NULL;
							}
						}
						PG_CATCH();
						{
							if (vbms_v)
							{
								for (j = 0; j < nkeys; j++)
									if (vbms_v[j])
										roaring_bitmap_free(vbms_v[j]);
								pfree((void *) vbms_v);
							}
							if (pbms_v)
							{
								for (j = 0; j < nkeys; j++)
									if (pbms_v[j])
										roaring_bitmap_free(pbms_v[j]);
								pfree((void *) pbms_v);
							}
							pfree(col_keys);
							PG_RE_THROW();
						}
						PG_END_TRY();
					}
					pfree(col_keys);
				}
				else
				{
					Oid		col_typid = TupleDescAttr(index->rd_att,
													  k->sk_attno - 1)->atttypid;
					int64	col_key;

					if (col_typid == FLOAT4OID &&
						isnan(DatumGetFloat4(k->sk_argument)))
					{
						col_bm = roaring_bitmap_create();
					}
					else
					{
						col_key = ROARING_COL_KEY(k->sk_attno,
												   roaring_datum_to_key32(k->sk_argument,
																		   col_typid));
						col_bm = lookup_value_as_bitmap_lossy(index, root_blkno, col_key);
						if (any_pending)
						{
							int s;

							for (s = 0; s < ROARING_PENDING_SHARDS; s++)
							{
								roaring_bitmap_t *pbm =
									pending_chain_as_bitmap_lossy(index, insert_heads[s],
																   col_key, snapshot);
								roaring_bitmap_or_inplace((roaring_bitmap_t *) col_bm, pbm);
								roaring_bitmap_free(pbm);
								if (merging_heads[s] != InvalidBlockNumber)
								{
									pbm = pending_chain_as_bitmap_lossy(
										index, merging_heads[s], col_key, snapshot);
									roaring_bitmap_or_inplace((roaring_bitmap_t *) col_bm, pbm);
									roaring_bitmap_free(pbm);
								}
							}
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
					/* Early exit: no pages can match. */
					if (roaring_bitmap_is_empty((roaring_bitmap_t *) result))
					{
						col_bm = NULL;
						break;
					}
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
			pfree(order);
			if (col_bm)
				roaring_bitmap_free((roaring_bitmap_t *) col_bm);
			if (result)
				roaring_bitmap_free((roaring_bitmap_t *) result);
			PG_RE_THROW();
		}
		PG_END_TRY();

		pfree(order);

		return ntids;
	}

	/* Single-column path. */
	{
		ScanKey key      = &scan->keyData[0];
		Oid		atttypid = TupleDescAttr(index->rd_att, 0)->atttypid;

		if (key->sk_flags & SK_SEARCHARRAY)
		{
			ArrayType *arr = DatumGetArrayTypeP(key->sk_argument);
			Datum	  *elems;
			bool	  *nulls;
			int		   nelems, i;
			int16	   typlen;
			bool	   typbyval;
			char	   typalign;
			int64	  *vals;
			int		   nvals = 0;
			roaring_bitmap_t ** volatile bms  = NULL;
			roaring_bitmap_t ** volatile pbms = NULL;

			get_typlenbyvalalign(atttypid, &typlen, &typbyval, &typalign);
			deconstruct_array(arr, atttypid, typlen, typbyval, typalign,
							  &elems, &nulls, &nelems);

			vals = palloc(nelems * sizeof(int64));
			for (i = 0; i < nelems; i++)
			{
				if (nulls[i])
					continue;
				if (atttypid == FLOAT4OID && isnan(DatumGetFloat4(elems[i])))
					continue;
				vals[nvals++] = roaring_datum_to_key64(elems[i], atttypid);
			}
			pfree(elems);
			pfree(nulls);

			if (nvals > 0)
			{
				int j, m;

				qsort(vals, nvals, sizeof(int64), int64_cmp_for_sort);
				for (j = 1, m = 1; j < nvals; j++)
					if (vals[j] != vals[j - 1])
						vals[m++] = vals[j];
				nvals = m;

				PG_TRY();
				{
					if (any_pending)
					{
						int s;

						pbms = palloc0(nvals * sizeof(roaring_bitmap_t *));
						for (j = 0; j < nvals; j++)
							pbms[j] = roaring_bitmap_create();
						for (s = 0; s < ROARING_PENDING_SHARDS; s++)
						{
							pending_chain_fill_values_lossy(index, insert_heads[s],
															vals, (roaring_bitmap_t **) pbms,
															nvals, snapshot);
							if (merging_heads[s] != InvalidBlockNumber)
								pending_chain_fill_values_lossy(index, merging_heads[s],
																vals, (roaring_bitmap_t **) pbms,
																nvals, snapshot);
						}
					}

					bms = palloc0(nvals * sizeof(roaring_bitmap_t *));
					lookup_values_as_bitmaps_leaf_walk_lossy(index, root_blkno,
															  vals, bms, nvals);
					for (j = 0; j < nvals; j++)
					{
						if (pbms != NULL)
						{
							roaring_bitmap_or_inplace(bms[j], pbms[j]);
							roaring_bitmap_free(pbms[j]);
							pbms[j] = NULL;
						}
						ntids += emit_lossy_bitmap_to_tbm(bms[j], tbm);
						roaring_bitmap_free(bms[j]);
						bms[j] = NULL;
					}
					pfree((void *) bms);
					bms = NULL;
					if (pbms != NULL)
					{
						pfree((void *) pbms);
						pbms = NULL;
					}
				}
				PG_CATCH();
				{
					if (bms)
					{
						for (j = 0; j < nvals; j++)
							if (bms[j])
								roaring_bitmap_free(bms[j]);
						pfree((void *) bms);
					}
					if (pbms)
					{
						for (j = 0; j < nvals; j++)
							if (pbms[j])
								roaring_bitmap_free(pbms[j]);
						pfree((void *) pbms);
					}
					PG_RE_THROW();
				}
				PG_END_TRY();
			}

			pfree(vals);
		}
		else
		{
			roaring_bitmap_t * volatile bm = NULL;
			int64 scan_value;

			if (atttypid == FLOAT4OID && isnan(DatumGetFloat4(key->sk_argument)))
				goto single_lossy_done;

			scan_value = roaring_datum_to_key64(key->sk_argument, atttypid);

			PG_TRY();
			{
				bm = lookup_value_as_bitmap_lossy(index, root_blkno, scan_value);
				if (any_pending)
				{
					int s;

					for (s = 0; s < ROARING_PENDING_SHARDS; s++)
					{
						roaring_bitmap_t *pbm =
							pending_chain_as_bitmap_lossy(index, insert_heads[s],
														  scan_value, snapshot);
						roaring_bitmap_or_inplace(bm, pbm);
						roaring_bitmap_free(pbm);
						if (merging_heads[s] != InvalidBlockNumber)
						{
							pbm = pending_chain_as_bitmap_lossy(index, merging_heads[s],
																scan_value, snapshot);
							roaring_bitmap_or_inplace(bm, pbm);
							roaring_bitmap_free(pbm);
						}
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
		single_lossy_done:;
	}

	return ntids;
}
