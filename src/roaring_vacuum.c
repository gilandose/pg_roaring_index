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
	uint64 linear_tid;
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
	int				capacity = 256;
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

		for (i = 0; i < spc->entry_count; i++)
		{
			TransactionId xmin = raw[i].xmin;

			if (!TransactionIdIsValid(xmin))
				continue;
			if (!TransactionIdIsCurrentTransactionId(xmin) &&
				!TransactionIdDidCommit(xmin))
				continue;	/* in-progress or aborted */

			if (n == capacity)
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
		RoaringDirEntry *l1_entries = (RoaringDirEntry *)
									  palloc(l1_count * sizeof(RoaringDirEntry));
		uint32 j;

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
	CollectedEntry		*entries;
	int					 nentries;
	int					 i;
	bool				 leaf_structure_changed = false;
	uint32				 new_total_entries;

	/* ---- Step 1: atomically swap in a fresh pending page. ---- */
	metabuf  = ReadBuffer(index, ROARING_METAPAGE_BLKNO);
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
	metapage = BufferGetPage(metabuf);
	meta     = RoaringPageGetMeta(metapage);

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

	/* Extend a new empty pending page. */
	new_pending_buf   = roaring_extend_page(index);
	new_pending_blkno = BufferGetBlockNumber(new_pending_buf);
	new_pending_page  = BufferGetPage(new_pending_buf);
	PageInit(new_pending_page, BLCKSZ, sizeof(RoaringPendingSpecial));
	{
		RoaringPendingSpecial *spc =
			(RoaringPendingSpecial *) PageGetSpecialPointer(new_pending_page);
		spc->page_type	= ROARING_PAGE_PENDING_INSERT;
		spc->flags		= 0;
		spc->entry_count = 0;
		spc->next_page	= InvalidBlockNumber;
		spc->xmin_low	= InvalidTransactionId;
	}

	/* Atomically: WAL new pending page + update metapage to point to it. */
	{
		GenericXLogState *state = GenericXLogStart(index);
		Page			  np_img, meta_img;

		np_img = GenericXLogRegisterBuffer(state, new_pending_buf,
										   GENERIC_XLOG_FULL_IMAGE);
		memcpy(np_img, new_pending_page, BLCKSZ);

		meta_img = GenericXLogRegisterBuffer(state, metabuf, 0);
		meta	 = RoaringPageGetMeta(meta_img);
		meta->pending_insert_head  = new_pending_blkno;
		meta->pending_insert_tail  = new_pending_blkno;
		meta->pending_insert_count = 0;

		GenericXLogFinish(state);
	}

	UnlockReleaseBuffer(new_pending_buf);
	UnlockReleaseBuffer(metabuf);

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
		int64		 cur_value = entries[i].value;
		int			 group_end = i + 1;
		BlockNumber	 leaf_blkno;
		bool		 found_in_index;

		while (group_end < nentries && entries[group_end].value == cur_value)
			group_end++;

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
				roaring64_bitmap_t *bm;
				int					j;
				size_t				new_size;
				Size				new_item_size;
				RoaringLeafEntry   *new_le;

				le		   = (RoaringLeafEntry *)
							 PageGetItem(leafpage, PageGetItemId(leafpage, found_off));
				item_len   = ItemIdGetLength(PageGetItemId(leafpage, found_off));
				bitmap_len = item_len - sizeof(RoaringLeafEntry);

				bm = roaring64_bitmap_portable_deserialize_safe(
						(const char *)(le + 1), bitmap_len);
				UnlockReleaseBuffer(leafbuf);

				if (bm == NULL)
					elog(ERROR,
						 "pg_roaring_index: merge: failed to deserialise bitmap "
						 "for value " INT64_FORMAT, cur_value);

				for (j = i; j < group_end; j++)
					roaring64_bitmap_add(bm, entries[j].linear_tid);

				roaring64_bitmap_run_optimize(bm);
				new_size      = roaring64_bitmap_portable_size_in_bytes(bm);
				new_item_size = MAXALIGN(sizeof(RoaringLeafEntry) + new_size);

				if (PageGetFreeSpace(leafpage) + item_len < new_item_size)
				{
					roaring64_bitmap_free(bm);
					elog(ERROR,
						 "pg_roaring_index: merge: updated bitmap for value "
						 INT64_FORMAT " (%zu bytes) does not fit on leaf page; "
						 "run REINDEX to rebuild", cur_value, new_size);
				}

				/* Re-read with exclusive lock to modify. */
				leafbuf  = ReadBuffer(index, leaf_blkno);
				LockBuffer(leafbuf, BUFFER_LOCK_EXCLUSIVE);
				{
					GenericXLogState *state = GenericXLogStart(index);
					Page			  img   = GenericXLogRegisterBuffer(state,
																 leafbuf, 0);

					/* Remove old item, insert updated one at the same offset. */
					PageIndexTupleDeleteNoCompact(img, found_off);

					new_le			 = (RoaringLeafEntry *)
									   palloc(sizeof(RoaringLeafEntry) + new_size);
					new_le->value		 = cur_value;
					new_le->cardinality  =
						(uint32) roaring64_bitmap_get_cardinality(bm);
					new_le->flags		 = ROARING_ENTRY_INLINE;
					roaring64_bitmap_portable_serialize(bm, (char *)(new_le + 1));

					if (PageAddItem(img, (Item) new_le,
									sizeof(RoaringLeafEntry) + new_size,
									found_off, false, false)
						== InvalidOffsetNumber)
						elog(ERROR,
							 "pg_roaring_index: merge: PageAddItem failed "
							 "for value " INT64_FORMAT, cur_value);

					pfree(new_le);
					GenericXLogFinish(state);
				}
				UnlockReleaseBuffer(leafbuf);
				roaring64_bitmap_free(bm);
				found_in_index = true;
			}
			else
				UnlockReleaseBuffer(leafbuf);
		}

		if (!found_in_index)
		{
			/* ---- 3b: new value — build bitmap and insert into leaves. ---- */
			roaring64_bitmap_t *bm;
			size_t				bm_size;
			Size				entry_size;
			RoaringLeafEntry   *new_le;
			int					j;
			bool				inserted = false;

			bm = roaring64_bitmap_create();
			for (j = i; j < group_end; j++)
				roaring64_bitmap_add(bm, entries[j].linear_tid);
			roaring64_bitmap_run_optimize(bm);
			bm_size    = roaring64_bitmap_portable_size_in_bytes(bm);
			entry_size = MAXALIGN(sizeof(RoaringLeafEntry) + bm_size);

			new_le				= (RoaringLeafEntry *)
								  palloc(sizeof(RoaringLeafEntry) + bm_size);
			new_le->value		= cur_value;
			new_le->cardinality	= (uint32) roaring64_bitmap_get_cardinality(bm);
			new_le->flags		= ROARING_ENTRY_INLINE;
			roaring64_bitmap_portable_serialize(bm, (char *)(new_le + 1));
			roaring64_bitmap_free(bm);

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

				state   = GenericXLogStart(index);
				new_img = GenericXLogRegisterBuffer(state, newleaf_buf,
													GENERIC_XLOG_FULL_IMAGE);
				memcpy(new_img, BufferGetPage(newleaf_buf), BLCKSZ);
				if (PageAddItem(new_img, (Item) new_le,
								sizeof(RoaringLeafEntry) + bm_size,
								1, false, false)
					== InvalidOffsetNumber)
					elog(ERROR, "pg_roaring_index: merge: PageAddItem on appended leaf failed");
				((RoaringLeafSpecial *)
				 PageGetSpecialPointer(new_img))->entry_count = 1;

				/* Link previous rightmost to new page. */
				if (rightmost_leaf != InvalidBlockNumber)
				{
					Buffer prev = ReadBuffer(index, rightmost_leaf);
					LockBuffer(prev, BUFFER_LOCK_EXCLUSIVE);
					{
						Page prev_img = GenericXLogRegisterBuffer(state, prev, 0);
						((RoaringLeafSpecial *)
						 PageGetSpecialPointer(prev_img))->right_page =
							newleaf_blkno;
					}
					GenericXLogFinish(state);
					UnlockReleaseBuffer(prev);
				}
				else
				{
					GenericXLogFinish(state);
					leftmost_leaf = newleaf_blkno;
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

		i = group_end;
	}

	pfree(entries);

	/* ---- Step 4: rebuild directory if leaf structure changed. ---- */
	if (leaf_structure_changed || root_dir == InvalidBlockNumber)
		root_dir = rebuild_directory(index, leftmost_leaf);

	/* ---- Step 5: update metapage. ---- */
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

		GenericXLogFinish(state);
	}
	UnlockReleaseBuffer(metabuf);
}

/* ----------------------------------------------------------------
 * roaring_bulkdelete
 *
 * Phase 1: Walk the pending insert list, purge entries whose xmins are
 * aborted (they will never be visible, so we can discard them now).
 * Dead TIDs from actual heap deletes are handled by vacuumcleanup via
 * the tombstone path — not yet implemented (Phase 1.3).
 * ---------------------------------------------------------------- */
IndexBulkDeleteResult *
roaring_bulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
				   IndexBulkDeleteCallback callback, void *callback_state)
{
	if (stats == NULL)
		stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));

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
