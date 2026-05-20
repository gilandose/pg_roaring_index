#include "pg_roaring_index.h"

#include "access/generic_xlog.h"
#include "access/transam.h"
#include "access/xact.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "utils/rel.h"

/* ----------------------------------------------------------------
 * Internal types for merge
 * ---------------------------------------------------------------- */

typedef struct CollectedEntry
{
	int64  value;
	uint32 linear_tid;
} CollectedEntry;

static int
cmp_collected(const void *a, const void *b)
{
	const CollectedEntry *ea = (const CollectedEntry *) a;
	const CollectedEntry *eb = (const CollectedEntry *) b;

	if (ea->value != eb->value)
		return (ea->value > eb->value) - (ea->value < eb->value);
	return (ea->linear_tid > eb->linear_tid) - (ea->linear_tid < eb->linear_tid);
}

/* ----------------------------------------------------------------
 * collect_pending
 *
 * Walk the pending insert chain and collect all entries whose xmin is
 * committed or belongs to the current transaction.  Returns a sorted
 * palloc'd array; sets *nout to the element count.
 *
 * Called while holding no locks; reads each pending page with a share lock.
 * ---------------------------------------------------------------- */
static CollectedEntry *
collect_pending(Relation index, BlockNumber head_blkno, int *nout)
{
	CollectedEntry *entries;
	int				n		 = 0;
	Size			capacity = 256;
	BlockNumber		cur		 = head_blkno;

	entries = (CollectedEntry *) palloc(capacity * sizeof(CollectedEntry));

	while (cur != InvalidBlockNumber)
	{
		Buffer				  buf;
		Page				  page;
		RoaringPendingSpecial *spc;
		RoaringPendingEntry	  *raw;
		uint16				  i;

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

		for (i = 0; i < spc->entry_count; i++)
		{
			TransactionId xmin = raw[i].xmin;

			if (!TransactionIdIsValid(xmin))
				continue;
			if (!TransactionIdIsCurrentTransactionId(xmin) &&
				!TransactionIdDidCommit(xmin))
				continue;	/* in-progress (carried forward) or aborted (discarded) */

			if ((Size) n == capacity)
			{
				capacity *= 2;
				entries = (CollectedEntry *)
						  repalloc(entries,
								   capacity * sizeof(CollectedEntry));
			}
			entries[n].value      = raw[i].value;
			entries[n].linear_tid = raw[i].linear_tid;
			n++;
		}

		cur = spc->next_page;
		UnlockReleaseBuffer(buf);
	}

	qsort(entries, n, sizeof(CollectedEntry), cmp_collected);
	*nout = n;
	return entries;
}

/* ----------------------------------------------------------------
 * find_insert_offset
 *
 * Binary-search a leaf page for the correct insertion offset for `value`.
 * Returns the OffsetNumber where PageAddItem should place the new entry
 * (1-based; = MaxOffsetNumber+1 if it goes after all existing entries).
 * Sets *found to true and *found_off to the offset if value is already present.
 * ---------------------------------------------------------------- */
static OffsetNumber
find_insert_offset(Page page, int64 value, bool *found, OffsetNumber *found_off)
{
	OffsetNumber lo = 1;
	OffsetNumber hi = PageGetMaxOffsetNumber(page);

	*found     = false;
	*found_off = InvalidOffsetNumber;

	while (lo <= hi)
	{
		OffsetNumber	  mid = (lo + hi) / 2;
		RoaringLeafEntry *e   = (RoaringLeafEntry *)
								PageGetItem(page, PageGetItemId(page, mid));
		if (e->value == value)
		{
			*found     = true;
			*found_off = mid;
			return mid;
		}
		else if (e->value < value)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return lo;	/* insertion point */
}

/* ----------------------------------------------------------------
 * rebuild_directory
 *
 * Walk the leaf chain starting at leftmost_leaf and write a fresh
 * directory structure (same algorithm as roaring_build Phase B).
 * Returns the new root directory block number.
 * ---------------------------------------------------------------- */
static BlockNumber
rebuild_directory(Relation index, BlockNumber leftmost_leaf)
{
	const uint32 max_dir =
		(uint32)((BLCKSZ - MAXALIGN(sizeof(RoaringDirSpecial))
				  - SizeOfPageHeaderData)
				 / sizeof(RoaringDirEntry));

	RoaringDirEntry *leaf_entries;
	uint32			 leaf_count = 0;
	uint32			 capacity   = 256;
	BlockNumber		 cur		= leftmost_leaf;
	BlockNumber		 root_dir;

	leaf_entries = (RoaringDirEntry *) palloc(capacity * sizeof(RoaringDirEntry));

	/* Collect (high_key, blkno) for every leaf page. */
	while (cur != InvalidBlockNumber)
	{
		Buffer				buf;
		Page				page;
		RoaringLeafSpecial *lspc;
		OffsetNumber		maxoff;
		RoaringLeafEntry   *last;

		buf   = ReadBuffer(index, cur);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page  = BufferGetPage(buf);
		lspc  = (RoaringLeafSpecial *) PageGetSpecialPointer(page);
		maxoff = PageGetMaxOffsetNumber(page);

		if (maxoff < 1)
		{
			/* Empty leaf (shouldn't happen after merge, but be defensive). */
			cur = lspc->right_page;
			UnlockReleaseBuffer(buf);
			continue;
		}

		last = (RoaringLeafEntry *)
			   PageGetItem(page, PageGetItemId(page, maxoff));

		if (leaf_count == capacity)
		{
			capacity *= 2;
			leaf_entries = (RoaringDirEntry *)
						   repalloc(leaf_entries,
									capacity * sizeof(RoaringDirEntry));
		}
		leaf_entries[leaf_count].high_key   = last->value;
		leaf_entries[leaf_count].child_page = cur;
		leaf_count++;

		cur = lspc->right_page;
		UnlockReleaseBuffer(buf);
	}

	if (leaf_count == 0)
	{
		pfree(leaf_entries);
		return InvalidBlockNumber;
	}

	/* Build directory (same as roaring_build Phase B). */
	if (leaf_count <= max_dir)
	{
		root_dir = roaring_write_dir_page(index, leaf_entries, leaf_count, 0);
	}
	else
	{
		uint32			 l1_count   = (leaf_count + max_dir - 1) / max_dir;
		RoaringDirEntry *l1_entries;
		uint32 j;

		if (l1_count > max_dir)
			elog(ERROR,
				 "pg_roaring_index: rebuild_directory: index too large for "
				 "two-level directory (%u leaf pages)", leaf_count);

		l1_entries = (RoaringDirEntry *)
					 palloc(l1_count * sizeof(RoaringDirEntry));

		for (j = 0; j < l1_count; j++)
		{
			uint32		start = j * max_dir;
			uint32		count = Min(max_dir, leaf_count - start);
			BlockNumber blkno;

			blkno = roaring_write_dir_page(index, leaf_entries + start,
										   count, 0);
			l1_entries[j].high_key   = leaf_entries[start + count - 1].high_key;
			l1_entries[j].child_page = blkno;
		}

		root_dir = roaring_write_dir_page(index, l1_entries, l1_count, 1);
		pfree(l1_entries);
	}

	pfree(leaf_entries);
	return root_dir;
}

/* ----------------------------------------------------------------
 * leaf_split_and_update
 *
 * Called when an updated inline leaf entry no longer fits on its page
 * (the page is too full to absorb the bitmap growth).  Splits the page
 * roughly in half, places the updated entry on the correct half, and
 * wires both halves into the existing doubly-linked leaf chain.
 *
 * new_item / new_item_size: the replacement item to substitute at found_off.
 * On return *rightmost_leaf_io is updated if a new rightmost page was
 * created; *leaf_structure_changed is set to true.
 * ---------------------------------------------------------------- */
static void
leaf_split_and_update(Relation index,
					   BlockNumber   leaf_blkno,
					   OffsetNumber  found_off,
					   const char   *new_item,
					   Size		     new_item_size,
					   BlockNumber  *rightmost_leaf_io,
					   bool		    *leaf_structure_changed)
{
	Buffer				 leafbuf;
	Page				 leafpage;
	RoaringLeafSpecial	*lspc;
	OffsetNumber		 off, maxoff;
	BlockNumber			 next_blkno;
	int					 nentries, i, split_at;
	char			   **items;
	Size			    *item_sizes;
	GenericXLogState	*state;
	Page				 cur_img, new_img;
	Buffer				 newleaf_buf;
	BlockNumber			 newleaf_blkno;

	leafbuf    = ReadBuffer(index, leaf_blkno);
	LockBuffer(leafbuf, BUFFER_LOCK_EXCLUSIVE);
	leafpage   = BufferGetPage(leafbuf);
	lspc	   = (RoaringLeafSpecial *) PageGetSpecialPointer(leafpage);
	maxoff	   = PageGetMaxOffsetNumber(leafpage);
	next_blkno = lspc->right_page;
	nentries   = (int) maxoff;

	items	   = (char **) palloc(nentries * sizeof(char *));
	item_sizes = (Size *)  palloc(nentries * sizeof(Size));

	/* Collect all entries; substitute the updated one at found_off. */
	for (off = FirstOffsetNumber; off <= maxoff; off++)
	{
		if (off == found_off)
		{
			items[off - 1]		= (char *) new_item; /* caller-owned */
			item_sizes[off - 1] = new_item_size;
		}
		else
		{
			ItemId iid = PageGetItemId(leafpage, off);
			Size   sz  = ItemIdGetLength(iid);
			char  *src = (char *) PageGetItem(leafpage, iid);

			items[off - 1]		= (char *) palloc(sz);
			item_sizes[off - 1] = sz;
			memcpy(items[off - 1], src, sz);
		}
	}

	/* Split at midpoint by entry count. */
	split_at = nentries / 2;
	if (split_at < 1)
		split_at = 1;

	/* Allocate and initialize new right leaf page. */
	newleaf_buf   = roaring_extend_page(index);
	newleaf_blkno = BufferGetBlockNumber(newleaf_buf);
	{
		Page				np = BufferGetPage(newleaf_buf);
		RoaringLeafSpecial *ns;

		PageInit(np, BLCKSZ, sizeof(RoaringLeafSpecial));
		ns				= (RoaringLeafSpecial *) PageGetSpecialPointer(np);
		ns->page_type	= ROARING_PAGE_LEAF;
		ns->flags		= 0;
		ns->entry_count = 0;
		ns->left_page	= leaf_blkno;
		ns->right_page	= next_blkno;
	}

	state   = GenericXLogStart(index);
	cur_img = GenericXLogRegisterBuffer(state, leafbuf, 0);
	new_img = GenericXLogRegisterBuffer(state, newleaf_buf, GENERIC_XLOG_FULL_IMAGE);
	memcpy(new_img, BufferGetPage(newleaf_buf), BLCKSZ);

	/*
	 * Rewrite the current page with the left half.  PageInit clears it;
	 * GenericXLog records the full delta from the registered snapshot.
	 */
	PageInit(cur_img, BLCKSZ, sizeof(RoaringLeafSpecial));
	{
		RoaringLeafSpecial *cs = (RoaringLeafSpecial *) PageGetSpecialPointer(cur_img);

		cs->page_type	= ROARING_PAGE_LEAF;
		cs->flags		= lspc->flags;
		cs->entry_count = 0;
		cs->left_page	= lspc->left_page;
		cs->right_page	= newleaf_blkno;
	}
	for (i = 0; i < split_at; i++)
	{
		if (PageAddItem(cur_img, (Item) items[i], item_sizes[i],
						InvalidOffsetNumber, false, false) == InvalidOffsetNumber)
			elog(ERROR, "pg_roaring_index: split: PageAddItem left-half failed");
		((RoaringLeafSpecial *) PageGetSpecialPointer(cur_img))->entry_count++;
	}

	/* Write right half to new page. */
	for (i = split_at; i < nentries; i++)
	{
		if (PageAddItem(new_img, (Item) items[i], item_sizes[i],
						InvalidOffsetNumber, false, false) == InvalidOffsetNumber)
			elog(ERROR, "pg_roaring_index: split: PageAddItem right-half failed");
		((RoaringLeafSpecial *) PageGetSpecialPointer(new_img))->entry_count++;
	}

	GenericXLogFinish(state);
	UnlockReleaseBuffer(leafbuf);
	UnlockReleaseBuffer(newleaf_buf);

	/* Update next page's left_page link (if any). */
	if (next_blkno != InvalidBlockNumber)
	{
		Buffer next_buf = ReadBuffer(index, next_blkno);

		LockBuffer(next_buf, BUFFER_LOCK_EXCLUSIVE);
		{
			GenericXLogState *st2 = GenericXLogStart(index);
			Page			  ni   = GenericXLogRegisterBuffer(st2, next_buf, 0);

			((RoaringLeafSpecial *) PageGetSpecialPointer(ni))->left_page =
				newleaf_blkno;
			GenericXLogFinish(st2);
		}
		UnlockReleaseBuffer(next_buf);
	}
	else
	{
		*rightmost_leaf_io = newleaf_blkno;
	}

	/* Free per-entry palloc'd copies (skip found_off-1, which is caller-owned). */
	for (i = 0; i < nentries; i++)
	{
		if (i != found_off - 1)
			pfree(items[i]);
	}
	pfree(items);
	pfree(item_sizes);

	*leaf_structure_changed = true;
}

/* ----------------------------------------------------------------
 * roaring_merge_pending
 *
 * Merge all pending inserts into the main leaf pages.
 *
 * Algorithm:
 *   1. Snapshot the pending list head/tail under a shared metapage lock.
 *      Extend a fresh pending page and atomically make it the new head/tail
 *      (so new inserts go there while we work on the old chain).
 *   2. Collect and sort the old chain's entries.
 *   3. For each distinct value:
 *      a. Find its leaf page via the directory.
 *      b. Existing value: deserialise bitmap, OR in new TIDs, re-serialise,
 *         write back via GenericXLog.
 *      c. New value: insert a new entry at the sorted position in the leaf
 *         page; create a new leaf page if the existing one is full.
 *   4. Rebuild the directory if the leaf structure changed.
 *   5. Update the metapage: new root_directory_page, leftmost/rightmost,
 *      updated total_entries.
 * ---------------------------------------------------------------- */
void
roaring_merge_pending(Relation index)
{
	const int			 max_inline = (int)(BLCKSZ
									   - MAXALIGN(sizeof(RoaringLeafSpecial))
									   - SizeOfPageHeaderData
									   - sizeof(ItemIdData)
									   - MAXALIGN(sizeof(RoaringLeafEntry)));
	Buffer				 metabuf;
	Page				 metapage;
	RoaringMetaPageData *meta;
	BlockNumber			 old_head;
	BlockNumber			 root_dir;
	BlockNumber			 leftmost_leaf;
	BlockNumber			 rightmost_leaf;
	uint32				 old_total_entries;
	BlockNumber			 new_pending_blkno;
	Buffer				 new_pending_buf;
	Page				 new_pending_page;
	int					 ncarry			 = 0;
	BlockNumber			 carry_head		 = InvalidBlockNumber;
	BlockNumber			 carry_tail_blkno = InvalidBlockNumber;
	CollectedEntry		*entries;
	int					 nentries;
	int					 i;
	bool				 leaf_structure_changed = false;
	uint32				 new_total_entries;

	/*
	 * Step 1: crash-safe merge setup.
	 *
	 * Ordering (critical for correctness):
	 *   A. WAL: set pending_merging_head = old pending_insert_head.
	 *      Crash-recovery anchor and concurrent-merger mutex.
	 *   B. While still holding metapage exclusive, scan the frozen old chain
	 *      for in-progress-xid entries; write them to new carry pages.
	 *      These entries are not ready to merge (their transactions may commit
	 *      or abort later) but must not be discarded — if dropped here and the
	 *      owning transaction later commits, those TIDs would be permanently lost.
	 *   C. WAL: swap pending_insert_head/tail to carry chain (or a fresh empty
	 *      page if no in-progress entries).  New inserts now land here.
	 *      pending_insert_count = ncarry.
	 *   D. After ALL leaf writes, WAL: clear pending_merging_head, update
	 *      root_directory_page and total_entries atomically.
	 */
	metabuf  = ReadBuffer(index, ROARING_METAPAGE_BLKNO);
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
	metapage = BufferGetPage(metabuf);
	meta     = RoaringPageGetMeta(metapage);

	/* Bail if another merger is already active (pending_merging_head is set). */
	if (meta->pending_merging_head != InvalidBlockNumber)
	{
		UnlockReleaseBuffer(metabuf);
		return;
	}

	if (meta->pending_insert_count == 0)
	{
		UnlockReleaseBuffer(metabuf);
		return;
	}

	old_head		  = meta->pending_insert_head;
	root_dir		  = meta->root_directory_page;
	leftmost_leaf	  = meta->leftmost_leaf_page;
	rightmost_leaf	  = meta->rightmost_leaf_page;
	old_total_entries = meta->total_entries;

	/* Step A: record pending_merging_head — this is the crash-recovery anchor. */
	{
		GenericXLogState *state = GenericXLogStart(index);
		Page			  meta_img = GenericXLogRegisterBuffer(state, metabuf, 0);

		RoaringPageGetMeta(meta_img)->pending_merging_head = old_head;
		GenericXLogFinish(state);
	}

	/*
	 * Step B: scan the frozen old chain for in-progress entries and carry them
	 * forward.  We hold the metapage exclusive throughout, so no new inserts
	 * can reach the old chain and it is effectively frozen.
	 *
	 * Reading pending pages with a share lock while holding the metapage
	 * exclusive is safe: no other backend holds pending-page locks while
	 * acquiring the metapage (roaring_pending_append takes metapage first,
	 * then pending page).
	 */
	{
		RoaringPendingEntry *carry     = NULL;
		int					 carry_cap = 0;
		BlockNumber			 cscan	   = old_head;

		while (cscan != InvalidBlockNumber)
		{
			Buffer				  pb;
			Page				  pp;
			RoaringPendingSpecial *pspc;
			RoaringPendingEntry   *praw;
			uint16				  k;

			pb   = ReadBuffer(index, cscan);
			LockBuffer(pb, BUFFER_LOCK_SHARE);
			pp   = BufferGetPage(pb);
			pspc = (RoaringPendingSpecial *) PageGetSpecialPointer(pp);
			praw = (RoaringPendingEntry *)   PageGetContents(pp);

			for (k = 0; k < pspc->entry_count; k++)
			{
				TransactionId xmin = praw[k].xmin;

				if (!TransactionIdIsValid(xmin))					continue;
				if (TransactionIdDidAbort(xmin))					continue;
				if (TransactionIdIsCurrentTransactionId(xmin) ||
					TransactionIdDidCommit(xmin))					continue;

				/* In-progress: carry forward to new pending chain. */
				if (ncarry == carry_cap)
				{
					carry_cap = (carry_cap == 0) ? 64 : carry_cap * 2;
					carry	  = (RoaringPendingEntry *)
								(carry == NULL
								 ? palloc(carry_cap * sizeof(RoaringPendingEntry))
								 : repalloc(carry,
											carry_cap * sizeof(RoaringPendingEntry)));
				}
				carry[ncarry++] = praw[k];
			}

			cscan = pspc->next_page;
			UnlockReleaseBuffer(pb);
		}

		if (ncarry > 0)
		{
			int			 npages  = (ncarry + ROARING_PENDING_PER_PAGE - 1)
								   / ROARING_PENDING_PER_PAGE;
			Buffer		*cbufs   = (Buffer *)      palloc(npages * sizeof(Buffer));
			BlockNumber *cblknos = (BlockNumber *) palloc(npages * sizeof(BlockNumber));
			int			 p;

			for (p = 0; p < npages; p++)
			{
				cbufs[p]   = roaring_extend_page(index);
				cblknos[p] = BufferGetBlockNumber(cbufs[p]);
			}

			for (p = 0; p < npages; p++)
			{
				Page				   cp	   = BufferGetPage(cbufs[p]);
				RoaringPendingSpecial *cspc;
				RoaringPendingEntry	  *cslots;
				int					   pstart  = p * ROARING_PENDING_PER_PAGE;
				int					   pcount  = Min(ROARING_PENDING_PER_PAGE,
													 ncarry - pstart);

				PageInit(cp, BLCKSZ, sizeof(RoaringPendingSpecial));
				cspc			  = (RoaringPendingSpecial *) PageGetSpecialPointer(cp);
				cspc->page_type   = ROARING_PAGE_PENDING_INSERT;
				cspc->flags		  = 0;
				cspc->entry_count = (uint16) pcount;
				cspc->next_page   = (p + 1 < npages)
									? cblknos[p + 1] : InvalidBlockNumber;
				cspc->xmin_low	  = carry[pstart].xmin;
				cspc->_pad		  = 0;
				cspc->value_min	  = carry[pstart].value;
				cspc->value_max	  = carry[pstart + pcount - 1].value;

				cslots = (RoaringPendingEntry *) PageGetContents(cp);
				memcpy(cslots, carry + pstart, pcount * sizeof(RoaringPendingEntry));
				((PageHeader) cp)->pd_lower =
					(LocationIndex)(SizeOfPageHeaderData +
									pcount * sizeof(RoaringPendingEntry));

				roaring_wal_and_release(index, cbufs[p]);
			}

			carry_head		= cblknos[0];
			carry_tail_blkno = cblknos[npages - 1];

			pfree(cblknos);
			pfree(cbufs);
			pfree(carry);
		}
	}

	/*
	 * Step C: atomically swap pending_insert_head to the carry chain (if any
	 * in-progress entries were found) or to a fresh empty page otherwise.
	 */
	if (ncarry > 0)
	{
		/* Carry pages are already WAL'd; only the metapage needs updating. */
		GenericXLogState *state	   = GenericXLogStart(index);
		Page			  meta_img = GenericXLogRegisterBuffer(state, metabuf, 0);

		meta					  = RoaringPageGetMeta(meta_img);
		meta->pending_insert_head  = carry_head;
		meta->pending_insert_tail  = carry_tail_blkno;
		meta->pending_insert_count = (uint32) ncarry;

		GenericXLogFinish(state);
		UnlockReleaseBuffer(metabuf);
	}
	else
	{
		/* No in-progress entries: extend a fresh empty pending page. */
		GenericXLogState *state = GenericXLogStart(index);
		Page			  np_img, meta_img;

		new_pending_buf   = roaring_extend_page(index);
		new_pending_blkno = BufferGetBlockNumber(new_pending_buf);
		new_pending_page  = BufferGetPage(new_pending_buf);
		PageInit(new_pending_page, BLCKSZ, sizeof(RoaringPendingSpecial));
		{
			RoaringPendingSpecial *spc =
				(RoaringPendingSpecial *) PageGetSpecialPointer(new_pending_page);
			spc->page_type	 = ROARING_PAGE_PENDING_INSERT;
			spc->flags		 = 0;
			spc->entry_count = 0;
			spc->next_page	 = InvalidBlockNumber;
			spc->xmin_low	 = InvalidTransactionId;
			spc->_pad		 = 0;
			spc->value_min	 = PG_INT64_MAX;
			spc->value_max	 = PG_INT64_MIN;
		}

		np_img = GenericXLogRegisterBuffer(state, new_pending_buf,
										   GENERIC_XLOG_FULL_IMAGE);
		memcpy(np_img, new_pending_page, BLCKSZ);

		meta_img = GenericXLogRegisterBuffer(state, metabuf, 0);
		meta	 = RoaringPageGetMeta(meta_img);
		meta->pending_insert_head  = new_pending_blkno;
		meta->pending_insert_tail  = new_pending_blkno;
		meta->pending_insert_count = 0;

		GenericXLogFinish(state);
		UnlockReleaseBuffer(new_pending_buf);
		UnlockReleaseBuffer(metabuf);
	}

	/* ---- Step 2: collect and sort the old pending chain. ---- */
	entries = collect_pending(index, old_head, &nentries);
	if (nentries == 0)
	{
		pfree(entries);
		return;
	}

	new_total_entries = old_total_entries;

	/* ---- Step 3: process each value group. ---- */
	i = 0;
	while (i < nentries)
	{
		int64		 cur_value  = entries[i].value;
		int			 group_end  = i + 1;
		int			 group_count = 0;
		uint32		*group_tids  = NULL;
		BlockNumber	 leaf_blkno;
		bool		 found_in_index;

		while (group_end < nentries && entries[group_end].value == cur_value)
			group_end++;

		/*
		 * Extract linear_tids for this group into a flat array so we can
		 * call roaring_bitmap_add_many (fast path for sorted input) instead
		 * of looping roaring_bitmap_add.
		 */
		{
			int		 gc  = group_end - i;
			uint32	*gtp = (uint32 *) palloc(gc * sizeof(uint32));
			int		 gi;

			for (gi = 0; gi < gc; gi++)
				gtp[gi] = entries[i + gi].linear_tid;
			group_count = gc;
			group_tids  = gtp;
		}

		/* Find the leaf page for this value. */
		found_in_index = false;
		leaf_blkno     = InvalidBlockNumber;

		if (root_dir != InvalidBlockNumber)
			leaf_blkno = roaring_dir_lookup(index, root_dir, cur_value);

		if (leaf_blkno != InvalidBlockNumber)
		{
			/* Binary-search within the leaf page to find the entry. */
			Buffer				 leafbuf;
			Page				 leafpage;
			bool				 found_on_page;
			OffsetNumber		 found_off;

			leafbuf  = ReadBuffer(index, leaf_blkno);
			LockBuffer(leafbuf, BUFFER_LOCK_SHARE);
			leafpage = BufferGetPage(leafbuf);

			find_insert_offset(leafpage, cur_value, &found_on_page, &found_off);

			if (found_on_page)
			{
				/* ---- 3a: existing value — OR in new TIDs. ---- */
				RoaringLeafEntry   *le;
				Size				item_len;
				Size				bitmap_len;
				Size				leaf_free;
				roaring_bitmap_t   *bm = NULL;
				size_t				new_size;
				uint32				bm_card;
				bool				was_overflow;
				bool				write_ovf;
				bool				needs_split;
				char			   *bm_data;
				BlockNumber			new_ovf_blkno;
				char				pfx_buf[ROARING_OVERFLOW_INLINE_BYTES];
				size_t				pfx_len;

				le		  = (RoaringLeafEntry *)
							PageGetItem(leafpage, PageGetItemId(leafpage, found_off));
				item_len  = ItemIdGetLength(PageGetItemId(leafpage, found_off));
				was_overflow = (le->flags == ROARING_ENTRY_OVERFLOW);
				/* Save free space before releasing the buffer — pointer goes stale. */
				leaf_free = PageGetFreeSpace(leafpage);

				if (was_overflow)
				{
					RoaringOverflowEntry oe_copy;

					memcpy(&oe_copy, le, sizeof(RoaringOverflowEntry));
					UnlockReleaseBuffer(leafbuf);
					bm = roaring_read_overflow_bitmap(index, &oe_copy);
				}
				else
				{
					bitmap_len = item_len - sizeof(RoaringLeafEntry);
					bm = roaring_bitmap_portable_deserialize_safe(
							(const char *)(le + 1), bitmap_len);
					UnlockReleaseBuffer(leafbuf);
				}

				if (bm == NULL)
					elog(ERROR,
						 "pg_roaring_index: merge: failed to deserialise bitmap "
						 "for value " INT64_FORMAT, cur_value);

				roaring_bitmap_add_many(bm, group_count, group_tids);

				new_size  = roaring_bitmap_portable_size_in_bytes(bm);
				bm_card   = roaring_cardinality32(bm);
				write_ovf = (new_size > (size_t) max_inline);

				/*
				 * Does the grown inline entry still fit on the current page?
				 * (Overflow always fits: old and new entries are the same size.)
				 * If not, a page split is needed.
				 */
				needs_split = (!write_ovf && !was_overflow &&
							   leaf_free + item_len <
							   MAXALIGN(sizeof(RoaringLeafEntry) + new_size));

				/*
				 * Serialize the bitmap.  For overflow, write the chain before
				 * re-acquiring the leaf lock so overflow WAL precedes leaf WAL.
				 */
				/*
				 * palloc can throw OOM; bm uses libc malloc so PG_FINALLY
				 * ensures it is freed even if palloc (or serialize) throws.
				 * On the success path, bm = NULL inside the try block so
				 * PG_FINALLY becomes a no-op.
				 */
				PG_TRY();
				{
					bm_data		  = (char *) palloc(new_size);
					new_ovf_blkno = InvalidBlockNumber;
					pfx_len		  = 0;
					roaring_bitmap_portable_serialize(bm, bm_data);
					roaring_bitmap_free(bm);
					bm = NULL;
				}
				PG_FINALLY();
				{
					if (bm != NULL)
					{
						roaring_bitmap_free(bm);
						bm = NULL;
					}
				}
				PG_END_TRY();

				if (write_ovf)
				{
					pfx_len		  = Min(ROARING_OVERFLOW_INLINE_BYTES, new_size);
					memcpy(pfx_buf, bm_data, pfx_len);
					new_ovf_blkno = roaring_write_overflow_chain(index, bm_data,
																  new_size, pfx_len);
				}

				if (needs_split)
				{
					/*
					 * Inline bitmap grew beyond the page's capacity.
					 * Split the leaf page and place the updated entry on the
					 * correct half.  Directory will be rebuilt at step 4.
					 */
					RoaringLeafEntry *new_le =
						(RoaringLeafEntry *) palloc(sizeof(RoaringLeafEntry) + new_size);

					new_le->value		= cur_value;
					new_le->cardinality = bm_card;
					new_le->flags		= ROARING_ENTRY_INLINE;
					memcpy((char *)(new_le + 1), bm_data, new_size);

					leaf_split_and_update(index, leaf_blkno, found_off,
										  (const char *) new_le,
										  sizeof(RoaringLeafEntry) + new_size,
										  &rightmost_leaf,
										  &leaf_structure_changed);
					pfree(new_le);
				}
				else
				{
					/* Normal path: delete old entry and re-insert updated one. */
					leafbuf = ReadBuffer(index, leaf_blkno);
					LockBuffer(leafbuf, BUFFER_LOCK_EXCLUSIVE);
					{
						GenericXLogState *state = GenericXLogStart(index);
						Page			  img   = GenericXLogRegisterBuffer(state,
																	 leafbuf, 0);

						/*
						 * Compacting delete so no LP_UNUSED hole breaks binary search.
						 */
						PageIndexTupleDelete(img, found_off);

						if (write_ovf)
						{
							RoaringOverflowEntry new_oe;

							new_oe.value		  = cur_value;
							new_oe.cardinality	  = bm_card;
							new_oe.flags		  = ROARING_ENTRY_OVERFLOW;
							new_oe.total_len	  = (uint32) new_size;
							new_oe.overflow_blkno = new_ovf_blkno;
							memcpy(new_oe.inline_prefix, pfx_buf, pfx_len);

							if (PageAddItem(img, (Item) &new_oe,
											sizeof(RoaringOverflowEntry),
											found_off, false, false)
								== InvalidOffsetNumber)
								elog(ERROR,
									 "pg_roaring_index: merge: PageAddItem failed "
									 "for value " INT64_FORMAT, cur_value);
						}
						else
						{
							RoaringLeafEntry *new_le =
								(RoaringLeafEntry *)
								palloc(sizeof(RoaringLeafEntry) + new_size);

							new_le->value		= cur_value;
							new_le->cardinality	= bm_card;
							new_le->flags		= ROARING_ENTRY_INLINE;
							memcpy((char *)(new_le + 1), bm_data, new_size);

							if (PageAddItem(img, (Item) new_le,
											sizeof(RoaringLeafEntry) + new_size,
											found_off, false, false)
								== InvalidOffsetNumber)
								elog(ERROR,
									 "pg_roaring_index: merge: PageAddItem failed "
									 "for value " INT64_FORMAT, cur_value);
							pfree(new_le);
						}

						GenericXLogFinish(state);
					}
					UnlockReleaseBuffer(leafbuf);
				}

				pfree(bm_data);
				found_in_index = true;
			}
			else
				UnlockReleaseBuffer(leafbuf);
		}

		if (!found_in_index)
		{
			/* ---- 3b: new value — build bitmap and insert into leaves. ---- */
			roaring_bitmap_t * volatile bm = NULL;
			size_t				bm_size;
			Size				entry_size;
			RoaringLeafEntry   *new_le;
			bool				inserted = false;

			bm = roaring_bitmap_create();
			roaring_bitmap_add_many(bm, group_count, group_tids);
			bm_size    = roaring_bitmap_portable_size_in_bytes(bm);
			entry_size = MAXALIGN(sizeof(RoaringLeafEntry) + bm_size);

			PG_TRY();
			{
				new_le				= (RoaringLeafEntry *)
									  palloc(sizeof(RoaringLeafEntry) + bm_size);
				new_le->value		= cur_value;
				new_le->cardinality	= roaring_cardinality32(bm);
				new_le->flags		= ROARING_ENTRY_INLINE;
				roaring_bitmap_portable_serialize(bm, (char *)(new_le + 1));
				roaring_bitmap_free(bm);
				bm = NULL;
			}
			PG_FINALLY();
			{
				if (bm != NULL)
				{
					roaring_bitmap_free(bm);
					bm = NULL;
				}
			}
			PG_END_TRY();

			/*
			 * Find the leaf page to insert into.  We need the page whose
			 * high_key >= cur_value with the smallest high_key — that is,
			 * the page the directory routes to for cur_value.  If the page
			 * has space and cur_value fits in its sorted order, insert there.
			 * Otherwise create a new leaf page.
			 */
			if (leaf_blkno != InvalidBlockNumber)
			{
				/* The dir returned a page; try to fit on it. */
				Buffer				 leafbuf;
				Page				 leafpage;
				OffsetNumber		 ins_off;
				bool				 dummy_found;
				OffsetNumber		 dummy_off;

				leafbuf  = ReadBuffer(index, leaf_blkno);
				LockBuffer(leafbuf, BUFFER_LOCK_EXCLUSIVE);
				leafpage = BufferGetPage(leafbuf);

				ins_off = find_insert_offset(leafpage, cur_value,
											 &dummy_found, &dummy_off);

				if (PageGetFreeSpace(leafpage) >= entry_size)
				{
					GenericXLogState *state = GenericXLogStart(index);
					Page			  img   = GenericXLogRegisterBuffer(state,
																 leafbuf, 0);

					if (PageAddItem(img, (Item) new_le,
									sizeof(RoaringLeafEntry) + bm_size,
									ins_off, false, false)
						== InvalidOffsetNumber)
						elog(ERROR,
							 "pg_roaring_index: merge: PageAddItem (new) "
							 "failed for value " INT64_FORMAT, cur_value);

					/* Update leaf special entry count. */
					((RoaringLeafSpecial *) PageGetSpecialPointer(img))
						->entry_count++;

					GenericXLogFinish(state);
					UnlockReleaseBuffer(leafbuf);
					inserted = true;
				}
				else
				{
					/*
					 * Leaf is full.  Create a new leaf page and splice it
					 * in after the current page.  The new page gets a single
					 * entry, and we link: cur_page ↔ new_page ↔ cur_page.right.
					 */
					Buffer				 newleaf_buf;
					BlockNumber			 newleaf_blkno;
					BlockNumber			 next_blkno;
					RoaringLeafSpecial	*cur_spc;
					GenericXLogState	*state;
					Page				 cur_img, new_img;

					cur_spc    = (RoaringLeafSpecial *)
								 PageGetSpecialPointer(leafpage);
					next_blkno = cur_spc->right_page;

					newleaf_buf   = roaring_extend_page(index);
					newleaf_blkno = BufferGetBlockNumber(newleaf_buf);
					{
						Page				np = BufferGetPage(newleaf_buf);
						RoaringLeafSpecial *ns;
						PageInit(np, BLCKSZ, sizeof(RoaringLeafSpecial));
						ns				= (RoaringLeafSpecial *)
										  PageGetSpecialPointer(np);
						ns->page_type	= ROARING_PAGE_LEAF;
						ns->flags		= 0;
						ns->entry_count = 0;
						ns->left_page	= leaf_blkno;
						ns->right_page	= next_blkno;
					}

					state   = GenericXLogStart(index);
					cur_img = GenericXLogRegisterBuffer(state, leafbuf, 0);
					new_img = GenericXLogRegisterBuffer(state, newleaf_buf,
														GENERIC_XLOG_FULL_IMAGE);
					memcpy(new_img, BufferGetPage(newleaf_buf), BLCKSZ);

					/* Insert entry into new page. */
					if (PageAddItem(new_img, (Item) new_le,
									sizeof(RoaringLeafEntry) + bm_size,
									1, false, false)
						== InvalidOffsetNumber)
						elog(ERROR, "pg_roaring_index: merge: PageAddItem on new leaf failed");
					((RoaringLeafSpecial *)
					 PageGetSpecialPointer(new_img))->entry_count = 1;

					/* Update old leaf: right_page → new page. */
					((RoaringLeafSpecial *)
					 PageGetSpecialPointer(cur_img))->right_page = newleaf_blkno;

					GenericXLogFinish(state);
					UnlockReleaseBuffer(leafbuf);
					UnlockReleaseBuffer(newleaf_buf);

					/* If next page exists, update its left_page link. */
					if (next_blkno != InvalidBlockNumber)
					{
						Buffer next_buf = ReadBuffer(index, next_blkno);
						LockBuffer(next_buf, BUFFER_LOCK_EXCLUSIVE);
						{
							GenericXLogState *st2 = GenericXLogStart(index);
							Page			  ni   = GenericXLogRegisterBuffer(
								st2, next_buf, 0);
							((RoaringLeafSpecial *)
							 PageGetSpecialPointer(ni))->left_page = newleaf_blkno;
							GenericXLogFinish(st2);
						}
						UnlockReleaseBuffer(next_buf);
					}
					else
					{
						/* New page is the new rightmost leaf. */
						rightmost_leaf = newleaf_blkno;
					}

					inserted = true;
					leaf_structure_changed = true;
				}
			}
			else
			{
				/*
				 * value is beyond all existing leaf high_keys (or index is
				 * empty).  Append a new leaf page at the rightmost position.
				 */
				Buffer				 newleaf_buf;
				BlockNumber			 newleaf_blkno;
				GenericXLogState	*state;
				Page				 new_img;

				newleaf_buf   = roaring_extend_page(index);
				newleaf_blkno = BufferGetBlockNumber(newleaf_buf);
				{
					Page				np = BufferGetPage(newleaf_buf);
					RoaringLeafSpecial *ns;
					PageInit(np, BLCKSZ, sizeof(RoaringLeafSpecial));
					ns				= (RoaringLeafSpecial *)
									  PageGetSpecialPointer(np);
					ns->page_type	= ROARING_PAGE_LEAF;
					ns->flags		= 0;
					ns->entry_count = 0;
					ns->left_page	= rightmost_leaf;
					ns->right_page	= InvalidBlockNumber;
				}

				/*
				 * Lock ordering: acquire prev (lower block) before newleaf_buf
				 * (just extended, always a higher block number).  This matches
				 * the convention used everywhere else in the leaf chain and
				 * prevents deadlocks with left-to-right chain walkers.
				 */
				{
					Buffer prev = InvalidBuffer;

					if (rightmost_leaf != InvalidBlockNumber)
					{
						prev = ReadBuffer(index, rightmost_leaf);
						LockBuffer(prev, BUFFER_LOCK_EXCLUSIVE);
					}

					state   = GenericXLogStart(index);
					new_img = GenericXLogRegisterBuffer(state, newleaf_buf,
														GENERIC_XLOG_FULL_IMAGE);
					memcpy(new_img, BufferGetPage(newleaf_buf), BLCKSZ);
					if (PageAddItem(new_img, (Item) new_le,
									sizeof(RoaringLeafEntry) + bm_size,
									1, false, false)
						== InvalidOffsetNumber)
						elog(ERROR,
							 "pg_roaring_index: merge: PageAddItem on appended leaf failed");
					((RoaringLeafSpecial *)
					 PageGetSpecialPointer(new_img))->entry_count = 1;

					if (prev != InvalidBuffer)
					{
						Page prev_img = GenericXLogRegisterBuffer(state, prev, 0);

						((RoaringLeafSpecial *)
						 PageGetSpecialPointer(prev_img))->right_page = newleaf_blkno;
						GenericXLogFinish(state);
						UnlockReleaseBuffer(prev);
					}
					else
					{
						GenericXLogFinish(state);
						leftmost_leaf = newleaf_blkno;
					}
				}

				UnlockReleaseBuffer(newleaf_buf);
				rightmost_leaf	 = newleaf_blkno;
				inserted		 = true;
				leaf_structure_changed = true;
			}

			if (inserted)
				new_total_entries++;

			pfree(new_le);
		}

		pfree(group_tids);
		i = group_end;
	}

	pfree(entries);

	/* ---- Step 4: rebuild directory if leaf structure changed. ---- */
	if (leaf_structure_changed || root_dir == InvalidBlockNumber)
		root_dir = rebuild_directory(index, leftmost_leaf);

	/*
	 * Step D: atomically commit the merge — clear pending_merging_head and
	 * update the directory pointer and entry count in a single WAL record.
	 * After this WAL record is durable the merge is complete.  The old chain
	 * starting at old_head is now unreachable and will be reclaimed on the
	 * next REINDEX / amvacuumcleanup free-space pass.
	 */
	metabuf  = ReadBuffer(index, ROARING_METAPAGE_BLKNO);
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
	{
		GenericXLogState *state = GenericXLogStart(index);
		Page			  img   = GenericXLogRegisterBuffer(state, metabuf, 0);
		RoaringMetaPageData *m  = RoaringPageGetMeta(img);

		m->root_directory_page = root_dir;
		m->leftmost_leaf_page  = leftmost_leaf;
		m->rightmost_leaf_page = rightmost_leaf;
		m->total_entries	   = new_total_entries;
		m->pending_merging_head = InvalidBlockNumber;	/* merge committed */

		GenericXLogFinish(state);
	}
	UnlockReleaseBuffer(metabuf);
}

/* ----------------------------------------------------------------
 * roaring_vacuum_one_leaf
 *
 * For every bitmap entry on a leaf page, call callback for each TID.
 * Remove dead TIDs by reserialing the bitmap in place using
 * PageIndexTupleOverwrite (which handles size shrinkage).  Empty bitmaps
 * are written back with cardinality=0 rather than deleted, so the sorted
 * order of line pointers stays intact for binary search in scan/merge.
 *
 * The caller must hold buf EXCLUSIVE.  GenericXLog is started lazily;
 * GenericXLogAbort is called if the page turns out to be clean.
 * ---------------------------------------------------------------- */
static bool
roaring_vacuum_one_leaf(Relation index, Buffer buf,
						IndexBulkDeleteResult *stats,
						IndexBulkDeleteCallback callback,
						void *callback_state)
{
	GenericXLogState * volatile state = NULL;
	Page			  img;
	OffsetNumber	  off, maxoff;
	bool			  changed = false;

	state  = GenericXLogStart(index);
	img    = GenericXLogRegisterBuffer(state, buf, 0);
	maxoff = PageGetMaxOffsetNumber(img);

	PG_TRY();
	{
	for (off = FirstOffsetNumber; off <= maxoff; off++)
	{
		ItemId				iid;
		RoaringLeafEntry   *le;
		size_t				bm_bytes;
		roaring_bitmap_t *bm;
		roaring_bitmap_t *dead_bm = NULL;
		roaring_uint32_iterator_t *it = NULL;
		bool				has_dead;
		bool				was_overflow;

		iid = PageGetItemId(img, off);
		if (!ItemIdIsUsed(iid))
			continue;

		le = (RoaringLeafEntry *) PageGetItem(img, iid);
		was_overflow = (le->flags == ROARING_ENTRY_OVERFLOW);
		bm = NULL;

		if (was_overflow)
		{
			RoaringOverflowEntry oe_copy;

			memcpy(&oe_copy, le, sizeof(RoaringOverflowEntry));
			bm = roaring_read_overflow_bitmap(index, &oe_copy);
		}
		else
		{
			bm_bytes = ItemIdGetLength(iid) - sizeof(RoaringLeafEntry);
			bm = roaring_bitmap_portable_deserialize_safe((char *)(le + 1),
														  bm_bytes);
		}
		if (!bm)
			continue;					/* corrupted — leave alone */

		/*
		 * bm uses libc malloc, not palloc, so PG error recovery will not free
		 * it.  PG_FINALLY ensures roaring_bitmap_free runs even if
		 * PageIndexTupleOverwrite or palloc below throws.
		 */
		PG_TRY();
		{
			dead_bm  = roaring_bitmap_create();
			has_dead = false;

			it = roaring_iterator_create(bm);
			while (it->has_value)
			{
				uint32			ltid = it->current_value;
				ItemPointerData	tid;

				ItemPointerSetBlockNumber(&tid, (BlockNumber)(ltid >> 9));
				ItemPointerSetOffsetNumber(&tid, (OffsetNumber)((ltid & 0x1FF) + 1));

				if (callback(&tid, callback_state))
				{
					roaring_bitmap_add(dead_bm, ltid);
					has_dead = true;
					stats->tuples_removed++;
				}
				else
					stats->num_index_tuples++;

				roaring_uint32_iterator_advance(it);
			}
			roaring_uint32_iterator_free(it);
			it = NULL;

			if (has_dead)
			{
				int64  value	   = le->value;
				size_t new_bm_bytes;

				/* Remove dead TIDs and reserialize. */
				roaring_bitmap_andnot_inplace(bm, dead_bm);
				roaring_bitmap_free(dead_bm);
				dead_bm = NULL;
				changed = true;

				new_bm_bytes = roaring_bitmap_portable_size_in_bytes(bm);

				if (was_overflow)
				{
					char				 *bm_data  = (char *) palloc(new_bm_bytes);
					size_t				  pfx_len  = Min(ROARING_OVERFLOW_INLINE_BYTES,
														 new_bm_bytes);
					RoaringOverflowEntry  new_oe;

					roaring_bitmap_portable_serialize(bm, bm_data);
					new_oe.value		  = value;
					new_oe.cardinality	  = roaring_cardinality32(bm);
					new_oe.flags		  = ROARING_ENTRY_OVERFLOW;
					new_oe.total_len	  = (uint32) new_bm_bytes;
					new_oe.overflow_blkno = roaring_write_overflow_chain(index,
																		  bm_data,
																		  new_bm_bytes,
																		  pfx_len);
					memcpy(new_oe.inline_prefix, bm_data, pfx_len);
					pfree(bm_data);

					if (!PageIndexTupleOverwrite(img, off, (Item) &new_oe,
												 sizeof(RoaringOverflowEntry)))
						elog(ERROR,
							 "pg_roaring_index: vacuum: PageIndexTupleOverwrite failed "
							 "for value " INT64_FORMAT, value);
				}
				else
				{
					Size			  new_size = sizeof(RoaringLeafEntry) + new_bm_bytes;
					RoaringLeafEntry *new_le   = (RoaringLeafEntry *) palloc(new_size);

					new_le->value		= value;
					new_le->cardinality	= roaring_cardinality32(bm);
					new_le->flags		= ROARING_ENTRY_INLINE;
					roaring_bitmap_portable_serialize(bm, (char *)(new_le + 1));

					if (!PageIndexTupleOverwrite(img, off, (Item) new_le, new_size))
						elog(ERROR,
							 "pg_roaring_index: vacuum: PageIndexTupleOverwrite failed "
							 "for value " INT64_FORMAT, value);
					pfree(new_le);
				}
			}
			else
			{
				roaring_bitmap_free(dead_bm);
				dead_bm = NULL;
			}

			roaring_bitmap_free(bm);
			bm = NULL;
		}
		PG_FINALLY();
		{
			if (bm != NULL)
				roaring_bitmap_free(bm);
			if (dead_bm != NULL)
			{
				roaring_bitmap_free(dead_bm);
				dead_bm = NULL;
			}
			if (it != NULL)
			{
				roaring_uint32_iterator_free(it);
				it = NULL;
			}
		}
		PG_END_TRY();
	}

	if (changed)
		GenericXLogFinish(state);
	else
		GenericXLogAbort(state);
	state = NULL;
	}
	PG_FINALLY();
	{
		if (state != NULL)
			GenericXLogAbort(state);
	}
	PG_END_TRY();

	return changed;
}

/* ----------------------------------------------------------------
 * roaring_vacuum_pending_list
 *
 * Walk a pending list chain (insert or delete) starting at head_blkno.
 * For each entry, call callback; compact out dead entries via GenericXLog.
 * Each page is locked EXCLUSIVE during the update.
 *
 * After processing all pages, subtracts the total number of removed entries
 * from pending_insert_count in the metapage so back-pressure stays accurate.
 * ---------------------------------------------------------------- */
static void
roaring_vacuum_pending_list(Relation index, BlockNumber head_blkno,
							IndexBulkDeleteResult *stats,
							IndexBulkDeleteCallback callback,
							void *callback_state)
{
	BlockNumber			cur		   = head_blkno;
	uint32				total_removed = 0;
	RoaringPendingEntry *live_buf  =
		(RoaringPendingEntry *) palloc(ROARING_PENDING_PER_PAGE *
									   sizeof(RoaringPendingEntry));

	while (cur != InvalidBlockNumber)
	{
		Buffer				  buf;
		Page				  page;
		RoaringPendingSpecial *spc;
		RoaringPendingEntry	  *raw;
		BlockNumber			  next;
		int					  n, nlive, i;
		bool				  changed = false;

		buf  = ReadBuffer(index, cur);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		page = BufferGetPage(buf);
		spc  = (RoaringPendingSpecial *) PageGetSpecialPointer(page);
		raw  = (RoaringPendingEntry *) PageGetContents(page);
		n    = spc->entry_count;
		next = spc->next_page;

		if (n > ROARING_PENDING_PER_PAGE)
			elog(ERROR,
				 "pg_roaring_index: corrupt pending page %u: entry_count %d > max %d",
				 cur, n, ROARING_PENDING_PER_PAGE);

		nlive = 0;
		for (i = 0; i < n; i++)
		{
			ItemPointerData tid;

			ItemPointerSetBlockNumber(&tid,
									 (BlockNumber)(raw[i].linear_tid >> 9));
			ItemPointerSetOffsetNumber(&tid,
									  (OffsetNumber)((raw[i].linear_tid & 0x1FF) + 1));

			if (callback(&tid, callback_state))
			{
				changed = true;
				stats->tuples_removed++;
				total_removed++;
			}
			else
				live_buf[nlive++] = raw[i];
		}

		if (changed)
		{
			GenericXLogState	*state = GenericXLogStart(index);
			Page				 img   = GenericXLogRegisterBuffer(state, buf, 0);
			RoaringPendingSpecial *ispc = (RoaringPendingSpecial *)
										  PageGetSpecialPointer(img);
			RoaringPendingEntry	 *ient  = (RoaringPendingEntry *)
										  PageGetContents(img);

			memcpy(ient, live_buf, nlive * sizeof(RoaringPendingEntry));
			ispc->entry_count = (uint16) nlive;
			((PageHeader) img)->pd_lower =
				(LocationIndex)(SizeOfPageHeaderData +
								nlive * sizeof(RoaringPendingEntry));

			GenericXLogFinish(state);
		}

		UnlockReleaseBuffer(buf);
		cur = next;
	}
	pfree(live_buf);

	/* Keep pending_insert_count in sync so back-pressure stays accurate. */
	if (total_removed > 0)
	{
		Buffer				 metabuf;
		GenericXLogState	*state;
		Page				 img;
		RoaringMetaPageData *meta;

		metabuf = ReadBuffer(index, ROARING_METAPAGE_BLKNO);
		LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
		state = GenericXLogStart(index);
		img   = GenericXLogRegisterBuffer(state, metabuf, 0);
		meta  = RoaringPageGetMeta(img);
		meta->pending_insert_count =
			(meta->pending_insert_count >= total_removed)
			? meta->pending_insert_count - total_removed
			: 0;
		GenericXLogFinish(state);
		UnlockReleaseBuffer(metabuf);
	}
}

/* ----------------------------------------------------------------
 * roaring_bulkdelete
 *
 * Walk all leaf pages and the pending insert list.  For each TID found,
 * call the VACUUM callback; remove dead TIDs from bitmaps / compact them
 * out of pending pages.
 * ---------------------------------------------------------------- */
IndexBulkDeleteResult *
roaring_bulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
				   IndexBulkDeleteCallback callback, void *callback_state)
{
	Relation			 index = info->index;
	Buffer				 metabuf;
	Page				 metapage;
	RoaringMetaPageData *meta;
	BlockNumber			 leftmost;
	BlockNumber			 pending_head;
	BlockNumber			 cur;

	if (stats == NULL)
		stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));

	/* Read metapage under share lock for page references. */
	metabuf  = ReadBuffer(index, ROARING_METAPAGE_BLKNO);
	LockBuffer(metabuf, BUFFER_LOCK_SHARE);
	metapage = BufferGetPage(metabuf);
	meta     = RoaringPageGetMeta(metapage);
	leftmost     = meta->leftmost_leaf_page;
	pending_head = meta->pending_insert_head;
	UnlockReleaseBuffer(metabuf);

	/* Walk leaf pages left to right. */
	cur = leftmost;
	while (cur != InvalidBlockNumber)
	{
		Buffer				buf;
		Page				page;
		RoaringLeafSpecial *spc;
		BlockNumber			next;

		buf  = ReadBuffer(index, cur);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		page = BufferGetPage(buf);
		spc  = (RoaringLeafSpecial *) PageGetSpecialPointer(page);
		next = spc->right_page;

		roaring_vacuum_one_leaf(index, buf, stats, callback, callback_state);

		UnlockReleaseBuffer(buf);
		cur = next;
	}

	/* Compact dead entries out of the pending insert list. */
	if (pending_head != InvalidBlockNumber)
		roaring_vacuum_pending_list(index, pending_head, stats,
									callback, callback_state);

	return stats;
}

/* ----------------------------------------------------------------
 * roaring_vacuumcleanup
 *
 * Called after VACUUM completes its heap pass.  Merge the pending insert
 * list into the main index so that subsequent scans don't have to walk
 * the pending list at all.
 * ---------------------------------------------------------------- */
IndexBulkDeleteResult *
roaring_vacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	if (stats == NULL)
		stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));

	roaring_merge_pending(info->index);

	return stats;
}
