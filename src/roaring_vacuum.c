#include "pg_roaring_index.h"

#include "access/generic_xlog.h"
#include "miscadmin.h"
#include "access/htup_details.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "catalog/pg_type_d.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "storage/procarray.h"
#include "access/visibilitymap.h"
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
 * horizon is GetOldestNonRemovableTransactionId() — when a page's xmin_low
 * is older than this, no entry on it can be in-progress or the current
 * transaction, so the per-entry check simplifies to DidCommit only.
 *
 * Called while holding no locks; reads each pending page with a share lock.
 * ---------------------------------------------------------------- */
static CollectedEntry *
collect_pending(Relation index, BlockNumber head_blkno, int *nout,
				TransactionId horizon)
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

		/*
		 * If every xmin on this page is older than the oldest non-removable
		 * xid, no entry can be the current transaction or still in-progress.
		 * Skip the IsCurrentTransactionId call and treat non-committed as gone.
		 */
		{
			bool all_old = (TransactionIdIsValid(spc->xmin_low) &&
							TransactionIdPrecedes(spc->xmin_low, horizon));

			for (i = 0; i < spc->entry_count; i++)
			{
				TransactionId xmin = raw[i].xmin;

				if (!TransactionIdIsValid(xmin))
					continue;
				if (all_old
					? !TransactionIdDidCommit(xmin)
					: (!TransactionIdIsCurrentTransactionId(xmin) &&
					   !TransactionIdDidCommit(xmin)))
					continue;	/* in-progress/aborted (discarded) */

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
		}  /* all_old scope */

		cur = spc->next_page;
		UnlockReleaseBuffer(buf);
	}

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

		if (l1_count > max_dir)
		{
			uint32           l2_count  = (l1_count + max_dir - 1) / max_dir;
			RoaringDirEntry *l2_entries;
			uint32           k;

			if (l2_count > max_dir)
				elog(ERROR,
					 "pg_roaring_index: rebuild_directory: index too large for "
					 "three-level directory (%u leaf pages)", leaf_count);

			l2_entries = (RoaringDirEntry *)
						 palloc(l2_count * sizeof(RoaringDirEntry));

			for (k = 0; k < l2_count; k++)
			{
				uint32      start = k * max_dir;
				uint32      count = Min(max_dir, l1_count - start);
				BlockNumber blkno;

				blkno = roaring_write_dir_page(index, l1_entries + start, count, 1);
				l2_entries[k].high_key   = l1_entries[start + count - 1].high_key;
				l2_entries[k].child_page = blkno;
			}

			root_dir = roaring_write_dir_page(index, l2_entries, l2_count, 2);
			pfree(l2_entries);
		}
		else
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
 * value: the index key whose entry is being replaced (used to re-locate
 * the entry under the exclusive lock, avoiding the TOCTOU race with
 * concurrent ambulkdelete).
 * new_item / new_item_size: the replacement item to substitute.
 * On return *rightmost_leaf_io is updated if a new rightmost page was
 * created; *leaf_structure_changed is set to true.
 * ---------------------------------------------------------------- */
static void
leaf_split_and_update(Relation index,
					   BlockNumber   leaf_blkno,
					   int64		   value,
					   const char   *new_item,
					   Size		     new_item_size,
					   BlockNumber  *rightmost_leaf_io,
					   bool		    *leaf_structure_changed)
{
	Buffer				 leafbuf;
	Page				 leafpage;
	RoaringLeafSpecial	*lspc;
	OffsetNumber		 off, maxoff;
	OffsetNumber		 found_off;
	BlockNumber			 next_blkno;
	int					 nentries, i, split_at;
	char			   **items;
	Size			    *item_sizes;
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

	/*
	 * Re-locate the entry under EX to avoid TOCTOU with concurrent
	 * ambulkdelete, which can shift offsets by calling PageIndexTupleDelete.
	 */
	{
		bool		 found;
		OffsetNumber fresh_off;

		find_insert_offset(leafpage, value, &found, &fresh_off);
		if (!found)
			elog(ERROR,
				 "pg_roaring_index: split: entry for value " INT64_FORMAT
				 " not found on leaf page %u",
				 value, leaf_blkno);
		found_off = fresh_off;
	}

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

	{
	GenericXLogState * volatile xstate = GenericXLogStart(index);
	PG_TRY();
	{
		cur_img = GenericXLogRegisterBuffer((GenericXLogState *) xstate, leafbuf, 0);
		new_img = GenericXLogRegisterBuffer((GenericXLogState *) xstate, newleaf_buf,
											GENERIC_XLOG_FULL_IMAGE);
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

		GenericXLogFinish((GenericXLogState *) xstate);
		xstate = NULL;
	}
	PG_CATCH();
	{
		if (xstate)
			GenericXLogAbort((GenericXLogState *) xstate);
		PG_RE_THROW();
	}
	PG_END_TRY();
	} /* xstate scope */
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
 * recycle_chain
 *
 * Walk the pending chain starting at head_blkno and push every page onto
 * the index free list.  The caller must hold metabuf EXCLUSIVE; this
 * function issues one 2-buffer GenericXLog record per page (freed page +
 * metapage), updating free_list_head after each push.
 *
 * The frozen chain is no longer reachable from the metapage (merging_head
 * has already been cleared), so no concurrent reader can be traversing it.
 * ---------------------------------------------------------------- */
static void
recycle_chain(Relation index, Buffer metabuf, BlockNumber head_blkno)
{
	BlockNumber cur = head_blkno;

	while (cur != InvalidBlockNumber)
	{
		Buffer				  fbuf;
		Page				  fpage;
		BlockNumber			  nxt;

		fbuf  = ReadBuffer(index, cur);
		LockBuffer(fbuf, BUFFER_LOCK_EXCLUSIVE);
		fpage = BufferGetPage(fbuf);

		/* Save next pointer before we overwrite the page content. */
		nxt = ((RoaringPendingSpecial *) PageGetSpecialPointer(fpage))->next_page;

		{
			GenericXLogState * volatile xstate = GenericXLogStart(index);
			PG_TRY();
			{
				Page			  fimg = GenericXLogRegisterBuffer(
					(GenericXLogState *) xstate, fbuf, 0);
				Page			  mimg = GenericXLogRegisterBuffer(
					(GenericXLogState *) xstate, metabuf, 0);
				RoaringMetaPageData *m   = RoaringPageGetMeta(mimg);
				RoaringFreeSpecial  *fs  = (RoaringFreeSpecial *)
										   PageGetSpecialPointer(fimg);

				fs->page_type = ROARING_PAGE_FREE;
				fs->_pad[0]   = 0;
				fs->_pad[1]   = 0;
				fs->_pad[2]   = 0;
				fs->next_free = m->free_list_head;
				m->free_list_head = cur;

				GenericXLogFinish((GenericXLogState *) xstate);
				xstate = NULL;
			}
			PG_CATCH();
			{
				if (xstate)
					GenericXLogAbort((GenericXLogState *) xstate);
				PG_RE_THROW();
			}
			PG_END_TRY();
		}

		UnlockReleaseBuffer(fbuf);
		cur = nxt;
	}
}

/* ----------------------------------------------------------------
 * OvfRecycleCtx
 *
 * Deferred list of old overflow chain heads to recycle after leaf writes
 * are complete.  Recycle happens under a single metapage EX acquisition so
 * we don't interleave with ongoing leaf updates.  Append via ovf_recycle_add;
 * drain via recycle_chain (caller holds metabuf EX) or ovf_recycle_flush.
 * ---------------------------------------------------------------- */
typedef struct
{
	BlockNumber *heads;
	int			 n;
	int			 cap;
} OvfRecycleCtx;

static void
ovf_recycle_add(OvfRecycleCtx *ctx, BlockNumber blkno)
{
	if (blkno == InvalidBlockNumber)
		return;
	if (ctx->n == ctx->cap)
	{
		ctx->cap = (ctx->cap == 0) ? 16 : ctx->cap * 2;
		if (ctx->heads == NULL)
			ctx->heads = (BlockNumber *) palloc(ctx->cap * sizeof(BlockNumber));
		else
			ctx->heads = (BlockNumber *) repalloc(ctx->heads,
												   ctx->cap * sizeof(BlockNumber));
	}
	ctx->heads[ctx->n++] = blkno;
}

/* Drain ctx under a freshly acquired metapage EX lock, then free the list. */
static void
ovf_recycle_flush(Relation index, OvfRecycleCtx *ctx)
{
	int    i;
	Buffer metabuf;

	if (ctx->n == 0)
		return;

	metabuf = ReadBuffer(index, ROARING_METAPAGE_BLKNO);
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
	for (i = 0; i < ctx->n; i++)
		recycle_chain(index, metabuf, ctx->heads[i]);
	UnlockReleaseBuffer(metabuf);

	pfree(ctx->heads);
	ctx->heads = NULL;
	ctx->n     = 0;
	ctx->cap   = 0;
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
 *   6. Step E: recycle frozen chain pages onto the free list.
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
	BlockNumber			 frozen_heads[ROARING_PENDING_SHARDS];
	bool				 shard_active[ROARING_PENDING_SHARDS];
	int					 nactive;
	BlockNumber			 root_dir;
	BlockNumber			 leftmost_leaf;
	BlockNumber			 rightmost_leaf;
	uint32				 old_total_entries;
	int					 ncarry			 = 0;
	int					 carry_shard	 = -1; /* shard that will host carry chain */
	BlockNumber			 carry_head		 = InvalidBlockNumber;
	BlockNumber			 carry_tail_blkno = InvalidBlockNumber;
	CollectedEntry		*entries;
	int					 nentries;
	int					 i;
	bool				 leaf_structure_changed = false;
	uint32				 new_total_entries;
	bool				 is_lossy;

	/*
	 * Step 1: crash-safe merge setup for all shards.
	 *
	 * Ordering (critical for correctness):
	 *   A. WAL: for each active shard set shards[i].merging_head = insert_head.
	 *      Crash-recovery anchor and concurrent-merger mutex.
	 *   B. While still holding metapage exclusive, scan each frozen shard
	 *      chain for in-progress-xid entries; write them to new carry pages.
	 *   C. For each active shard: WAL: swap insert_head/tail to carry chain
	 *      (shard 0) or fresh empty page (shards 1..N-1).
	 *   D. After ALL leaf writes: WAL: clear all merging_heads, update
	 *      root_directory_page and total_entries atomically.
	 */
	metabuf  = ReadBuffer(index, ROARING_METAPAGE_BLKNO);
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
	metapage = BufferGetPage(metabuf);
	meta     = RoaringPageGetMeta(metapage);
	roaring_validate_metapage(index, meta);

	nactive = 0;
	for (i = 0; i < ROARING_PENDING_SHARDS; i++)
	{
		shard_active[i]  = false;
		frozen_heads[i]  = InvalidBlockNumber;

		if (meta->shards[i].merging_head != InvalidBlockNumber)
		{
			/* Another merger is active (or crash recovery needed) — bail. */
			UnlockReleaseBuffer(metabuf);
			return;
		}
		if (meta->shards[i].insert_count == 0)
			continue;

		shard_active[i] = true;
		frozen_heads[i] = meta->shards[i].insert_head;
		nactive++;
	}

	if (nactive == 0)
	{
		UnlockReleaseBuffer(metabuf);
		return;
	}

	root_dir		  = meta->root_directory_page;
	leftmost_leaf	  = meta->leftmost_leaf_page;
	rightmost_leaf	  = meta->rightmost_leaf_page;
	old_total_entries = meta->total_entries;
	is_lossy		  = (meta->flags & ROARING_FLAG_LOSSY) != 0;

	/* Determine which shard will host the carry chain (lowest-numbered active). */
	for (i = 0; i < ROARING_PENDING_SHARDS; i++)
	{
		if (shard_active[i])
		{
			carry_shard = i;
			break;
		}
	}

	/* Step A: atomically set merging_head for all active shards. */
	{
		GenericXLogState *state    = GenericXLogStart(index);
		Page			  meta_img = GenericXLogRegisterBuffer(state, metabuf, 0);
		RoaringMetaPageData *m     = RoaringPageGetMeta(meta_img);

		for (i = 0; i < ROARING_PENDING_SHARDS; i++)
			if (shard_active[i])
				m->shards[i].merging_head = frozen_heads[i];
		GenericXLogFinish(state);
	}

	/*
	 * Step B: scan all frozen shard chains for in-progress entries.
	 * We hold metapage EX throughout; new inserts wanting SHARE will wait.
	 * Reading pending pages SHARE under metapage EX is safe: the new insert
	 * hot path only holds tail EX (no metapage), so it can interleave but
	 * cannot deadlock — it finishes and releases tail before merge can block
	 * on the same page.
	 */
	{
		RoaringPendingEntry *carry     = NULL;
		int					 carry_cap = 0;
		TransactionId		 horizon   = GetOldestNonRemovableTransactionId(index);
		int					 s;

		for (s = 0; s < ROARING_PENDING_SHARDS; s++)
		{
			BlockNumber cscan;

			if (!shard_active[s])
				continue;

			for (cscan = frozen_heads[s]; cscan != InvalidBlockNumber; )
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

				/*
				 * xmin_low page-skip: if every xmin on this page predates the
				 * oldest non-removable xid, no entry can be in-progress.
				 * Nothing to carry forward — skip the per-entry loop entirely.
				 */
				if (TransactionIdIsValid(pspc->xmin_low) &&
					TransactionIdPrecedes(pspc->xmin_low, horizon))
				{
					cscan = pspc->next_page;
					UnlockReleaseBuffer(pb);
					continue;
				}

				for (k = 0; k < pspc->entry_count; k++)
				{
					TransactionId xmin = praw[k].xmin;

					if (!TransactionIdIsValid(xmin))				continue;
					if (TransactionIdDidAbort(xmin))				continue;
					if (TransactionIdIsCurrentTransactionId(xmin) ||
						TransactionIdDidCommit(xmin))				continue;

					/* In-progress: carry forward. */
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

			/* Phase 1: initialise all carry pages in memory. */
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

				/* WAL and release pages 1..N-1 now (before page 0). */
				if (p > 0)
					roaring_wal_and_release(index, cbufs[p]);
			}

			/*
			 * Phase 2: WAL page 0 together with the metapage carry_head update.
			 *
			 * Pages 1..N-1 were WAL'd first so that if we crash between this
			 * record and Step C, crash recovery can walk the full chain starting
			 * at carry_head without hitting uninitialised blocks.
			 */
			{
				GenericXLogState *cstate = GenericXLogStart(index);
				Page carry_img = GenericXLogRegisterBuffer(cstate, cbufs[0],
														   GENERIC_XLOG_FULL_IMAGE);
				Page meta_img  = GenericXLogRegisterBuffer(cstate, metabuf, 0);

				memcpy(carry_img, BufferGetPage(cbufs[0]), BLCKSZ);
				RoaringPageGetMeta(meta_img)->shards[carry_shard].carry_head = cblknos[0];

				GenericXLogFinish(cstate);
				UnlockReleaseBuffer(cbufs[0]);
			}

			carry_head		 = cblknos[0];
			carry_tail_blkno = cblknos[npages - 1];

			pfree(cblknos);
			pfree(cbufs);
			pfree(carry);
		}
	}

	/*
	 * Step C: for each active shard, swap its insert chain to the carry chain
	 * (carry_shard only, if carry entries exist) or a fresh empty page.
	 * Each shard gets its own WAL record (2 buffers: new page + metapage)
	 * to stay within MAX_GENERIC_XLOG_PAGES = 4.
	 */
	{
		int s;

		for (s = 0; s < ROARING_PENDING_SHARDS; s++)
		{
			GenericXLogState *state;
			Page			  meta_img;
			RoaringMetaPageData *m;

			if (!shard_active[s])
				continue;

			if (s == carry_shard && ncarry > 0)
			{
				/* carry_shard gets the carry chain (already WAL'd in Step B). */
				state    = GenericXLogStart(index);
				meta_img = GenericXLogRegisterBuffer(state, metabuf, 0);
				m        = RoaringPageGetMeta(meta_img);
				m->shards[s].insert_head  = carry_head;
				m->shards[s].insert_tail  = carry_tail_blkno;
				m->shards[s].insert_count = (uint32) ncarry;
				m->shards[s].carry_head   = InvalidBlockNumber;
				GenericXLogFinish(state);
			}
			else
			{
				/* Fresh empty pending page (from free list or file extension). */
				Buffer				  npbuf;
				BlockNumber			  npblkno;
				BlockNumber			  new_free_list_head;
				Page				  nppage;
				Page				  np_img;
				RoaringPendingSpecial *npspc;

				npbuf   = roaring_alloc_page(index, metabuf, &new_free_list_head);
				npblkno = BufferGetBlockNumber(npbuf);
				nppage  = BufferGetPage(npbuf);
				PageInit(nppage, BLCKSZ, sizeof(RoaringPendingSpecial));
				npspc = (RoaringPendingSpecial *) PageGetSpecialPointer(nppage);
				npspc->page_type   = ROARING_PAGE_PENDING_INSERT;
				npspc->flags       = 0;
				npspc->entry_count = 0;
				npspc->next_page   = InvalidBlockNumber;
				npspc->xmin_low    = InvalidTransactionId;
				npspc->_pad        = 0;
				npspc->value_min   = PG_INT64_MAX;
				npspc->value_max   = PG_INT64_MIN;
				((PageHeader) nppage)->pd_lower = SizeOfPageHeaderData;

				state    = GenericXLogStart(index);
				np_img   = GenericXLogRegisterBuffer(state, npbuf,
													 GENERIC_XLOG_FULL_IMAGE);
				memcpy(np_img, nppage, BLCKSZ);
				meta_img = GenericXLogRegisterBuffer(state, metabuf, 0);
				m        = RoaringPageGetMeta(meta_img);
				m->shards[s].insert_head  = npblkno;
				m->shards[s].insert_tail  = npblkno;
				m->shards[s].insert_count = 0;
				if (new_free_list_head != m->free_list_head)
					m->free_list_head = new_free_list_head;
				GenericXLogFinish(state);
				UnlockReleaseBuffer(npbuf);
			}
		}
	}

	UnlockReleaseBuffer(metabuf);

	/* ---- Step 2: collect entries from all frozen shard chains, sort. ---- */
	{
		int				 s;
		Size			 cap		= 256;
		int				 ntotal		= 0;
		TransactionId	 horizon	= GetOldestNonRemovableTransactionId(index);
		CollectedEntry *all_entries = (CollectedEntry *)
									  palloc(cap * sizeof(CollectedEntry));

		for (s = 0; s < ROARING_PENDING_SHARDS; s++)
		{
			int			 shard_n;
			CollectedEntry *shard_e;

			if (!shard_active[s])
				continue;

			shard_e = collect_pending(index, frozen_heads[s], &shard_n, horizon);
			if (ntotal + shard_n > (int) cap)
			{
				while (ntotal + shard_n > (int) cap)
					cap *= 2;
				all_entries = (CollectedEntry *)
							  repalloc(all_entries, cap * sizeof(CollectedEntry));
			}
			memcpy(all_entries + ntotal, shard_e, shard_n * sizeof(CollectedEntry));
			ntotal += shard_n;
			pfree(shard_e);
		}

		if (ntotal > 1)
			qsort(all_entries, ntotal, sizeof(CollectedEntry), cmp_collected);

		entries  = all_entries;
		nentries = ntotal;
	}

	if (nentries == 0)
	{
		/*
		 * All pending entries were in-progress or aborted — nothing to merge.
		 * Clear all merging_heads so subsequent merges can proceed normally,
		 * then recycle the frozen chain pages.
		 */
		int s;

		pfree(entries);
		metabuf  = ReadBuffer(index, ROARING_METAPAGE_BLKNO);
		LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
		{
			GenericXLogState * volatile xstate = GenericXLogStart(index);

			PG_TRY();
			{
				Page			  img = GenericXLogRegisterBuffer(
					(GenericXLogState *) xstate, metabuf, 0);
				RoaringMetaPageData *m = RoaringPageGetMeta(img);
				int					  si;

				for (si = 0; si < ROARING_PENDING_SHARDS; si++)
					if (shard_active[si])
						m->shards[si].merging_head = InvalidBlockNumber;
				m->flags &= ~ROARING_FLAG_BGMERGE_SPAWNED;
				GenericXLogFinish((GenericXLogState *) xstate);
				xstate = NULL;
			}
			PG_CATCH();
			{
				if (xstate)
					GenericXLogAbort((GenericXLogState *) xstate);
				PG_RE_THROW();
			}
			PG_END_TRY();
		}

		/* Recycle frozen chain pages (metabuf still held EX). */
		for (s = 0; s < ROARING_PENDING_SHARDS; s++)
		{
			if (!shard_active[s] || frozen_heads[s] == InvalidBlockNumber)
				continue;
			recycle_chain(index, metabuf, frozen_heads[s]);
		}

		UnlockReleaseBuffer(metabuf);
		return;
	}

	new_total_entries = old_total_entries;

	/* Deferred list of old overflow chain heads replaced during Step 3. */
	{
		OvfRecycleCtx old_ovf_ctx;

		old_ovf_ctx.heads = NULL;
		old_ovf_ctx.n     = 0;
		old_ovf_ctx.cap   = 0;

	/* ---- Step 3: process each value group. ---- */
	i = 0;
	while (i < nentries)
	{
		int64		 cur_value  = entries[i].value;
		int			 group_end  = i + 1;
		int			 group_count = 0;
		uint64		*group_tids  = NULL;
		BlockNumber	 leaf_blkno;
		bool		 found_in_index;

		CHECK_FOR_INTERRUPTS();

		while (group_end < nentries && entries[group_end].value == cur_value)
			group_end++;

		/*
		 * Extract linear_tids for this group into a flat array so we can
		 * call roaring64_bitmap_add_many (fast path for sorted input) instead
		 * of looping roaring64_bitmap_add.
		 */
		{
			int		 gc  = group_end - i;
			uint64	*gtp = (uint64 *) palloc(gc * sizeof(uint64));
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
				roaring64_bitmap_t *bm64 = NULL;
				roaring_bitmap_t   *bm32 = NULL;
				size_t				new_size;
				uint32				bm_card;
				bool				was_overflow;
				bool				write_ovf;
				bool				needs_split;
				char			   *bm_data;
				BlockNumber			new_ovf_blkno;
				BlockNumber			old_ovf_blkno;

				le		  = (RoaringLeafEntry *)
							PageGetItem(leafpage, PageGetItemId(leafpage, found_off));
				item_len  = ItemIdGetLength(PageGetItemId(leafpage, found_off));
				was_overflow = (le->flags & ROARING_ENTRY_OVERFLOW) != 0;
				old_ovf_blkno = InvalidBlockNumber;
				/* Save free space before releasing the buffer — pointer goes stale. */
				leaf_free = PageGetFreeSpace(leafpage);

				if (is_lossy)
				{
					if (was_overflow)
					{
						RoaringOverflowEntry oe_copy;

						memcpy(&oe_copy, le, sizeof(RoaringOverflowEntry));
						old_ovf_blkno = oe_copy.overflow_blkno;
						UnlockReleaseBuffer(leafbuf);
						bm32 = roaring_read_overflow_bitmap_lossy(index, &oe_copy);
					}
					else
					{
						bitmap_len = item_len - sizeof(RoaringLeafEntry);
						bm32 = roaring_bitmap_portable_deserialize_safe(
								(const char *)(le + 1), bitmap_len);
						UnlockReleaseBuffer(leafbuf);
					}

					if (bm32 == NULL)
						elog(ERROR,
							 "pg_roaring_index: merge: failed to deserialise bitmap "
							 "for value " INT64_FORMAT, cur_value);

					{
						int		 gc	 = group_count;
						uint32	*gtp = (uint32 *) palloc(gc * sizeof(uint32));
						int		 gi;

						for (gi = 0; gi < gc; gi++)
							gtp[gi] = (uint32) group_tids[gi];
						roaring_bitmap_add_many(bm32, (size_t) gc, gtp);
						pfree(gtp);
					}
					new_size = roaring_bitmap_portable_size_in_bytes(bm32);
					bm_card  = roaring_cardinality32(bm32);
				}
				else
				{
					if (was_overflow)
					{
						RoaringOverflowEntry oe_copy;

						memcpy(&oe_copy, le, sizeof(RoaringOverflowEntry));
						old_ovf_blkno = oe_copy.overflow_blkno;
						UnlockReleaseBuffer(leafbuf);
						bm64 = roaring_read_overflow_bitmap(index, &oe_copy);
					}
					else
					{
						bitmap_len = item_len - sizeof(RoaringLeafEntry);
						bm64 = roaring64_bitmap_portable_deserialize_safe(
								(const char *)(le + 1), bitmap_len);
						UnlockReleaseBuffer(leafbuf);
					}

					if (bm64 == NULL)
						elog(ERROR,
							 "pg_roaring_index: merge: failed to deserialise bitmap "
							 "for value " INT64_FORMAT, cur_value);

					roaring64_bitmap_add_many(bm64, group_count, group_tids);
					new_size = roaring64_bitmap_portable_size_in_bytes(bm64);
					bm_card  = roaring64_cardinality32(bm64);
				}

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
				PG_TRY();
				{
					bm_data		  = (char *) palloc(new_size);
					new_ovf_blkno = InvalidBlockNumber;
					if (is_lossy)
					{
						roaring_bitmap_portable_serialize(bm32, bm_data);
						roaring_bitmap_free(bm32);
						bm32 = NULL;
					}
					else
					{
						roaring64_bitmap_portable_serialize(bm64, bm_data);
						roaring64_bitmap_free(bm64);
						bm64 = NULL;
					}
				}
				PG_FINALLY();
				{
					if (bm32 != NULL) { roaring_bitmap_free(bm32);   bm32 = NULL; }
					if (bm64 != NULL) { roaring64_bitmap_free(bm64); bm64 = NULL; }
				}
				PG_END_TRY();

				if (write_ovf)
				{
					new_ovf_blkno = roaring_write_overflow_chain(index, bm_data,
																  new_size);
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

					leaf_split_and_update(index, leaf_blkno, cur_value,
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
						GenericXLogState * volatile xstate = GenericXLogStart(index);
						PG_TRY();
						{
							bool fresh_found;
							Page img = GenericXLogRegisterBuffer(
								(GenericXLogState *) xstate, leafbuf, 0);

							/*
							 * Re-locate entry under EX: concurrent ambulkdelete may
							 * have called PageIndexTupleDelete in the gap between the
							 * SHARE read and this EX acquisition, shifting offsets.
							 */
							find_insert_offset(img, cur_value,
											   &fresh_found, &found_off);
							if (!fresh_found)
								elog(ERROR,
									 "pg_roaring_index: merge: entry for value "
									 INT64_FORMAT " not found on leaf page %u",
									 cur_value, leaf_blkno);

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

							GenericXLogFinish((GenericXLogState *) xstate);
							xstate = NULL;
						}
						PG_CATCH();
						{
							if (xstate)
								GenericXLogAbort((GenericXLogState *) xstate);
							PG_RE_THROW();
						}
						PG_END_TRY();
					} /* xstate scope */
					UnlockReleaseBuffer(leafbuf);
				}

				pfree(bm_data);
				found_in_index = true;

				/* Queue old overflow chain for deferred recycle in Step E. */
				if (was_overflow)
					ovf_recycle_add(&old_ovf_ctx, old_ovf_blkno);
			}
			else
				UnlockReleaseBuffer(leafbuf);
		}

		if (!found_in_index)
		{
			/* ---- 3b: new value — build bitmap and insert into leaves. ---- */
			roaring64_bitmap_t * volatile bm64 = NULL;
			roaring_bitmap_t   * volatile bm32 = NULL;
			size_t				bm_size;
			Size				entry_size;
			RoaringLeafEntry   *new_le;
			bool				inserted = false;

			if (is_lossy)
			{
				uint32 *gtp = (uint32 *) palloc(group_count * sizeof(uint32));
				int		gi;

				for (gi = 0; gi < group_count; gi++)
					gtp[gi] = (uint32) group_tids[gi];
				bm32 = roaring_bitmap_create();
				roaring_bitmap_add_many(bm32, (size_t) group_count, gtp);
				pfree(gtp);
				bm_size = roaring_bitmap_portable_size_in_bytes(bm32);
			}
			else
			{
				bm64 = roaring64_bitmap_create();
				roaring64_bitmap_add_many(bm64, group_count, group_tids);
				bm_size = roaring64_bitmap_portable_size_in_bytes(bm64);
			}
			entry_size = MAXALIGN(sizeof(RoaringLeafEntry) + bm_size);

			PG_TRY();
			{
				new_le			= (RoaringLeafEntry *)
								  palloc(sizeof(RoaringLeafEntry) + bm_size);
				new_le->value	= cur_value;
				new_le->flags	= ROARING_ENTRY_INLINE;
				if (is_lossy)
				{
					new_le->cardinality = roaring_cardinality32(bm32);
					roaring_bitmap_portable_serialize(bm32, (char *)(new_le + 1));
					roaring_bitmap_free(bm32);
					bm32 = NULL;
				}
				else
				{
					new_le->cardinality = roaring64_cardinality32(bm64);
					roaring64_bitmap_portable_serialize(bm64, (char *)(new_le + 1));
					roaring64_bitmap_free(bm64);
					bm64 = NULL;
				}
			}
			PG_FINALLY();
			{
				if (bm32 != NULL) { roaring_bitmap_free(bm32);   bm32 = NULL; }
				if (bm64 != NULL) { roaring64_bitmap_free(bm64); bm64 = NULL; }
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
					GenericXLogState * volatile xstate = GenericXLogStart(index);
					PG_TRY();
					{
						Page img = GenericXLogRegisterBuffer(
							(GenericXLogState *) xstate, leafbuf, 0);

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

						GenericXLogFinish((GenericXLogState *) xstate);
						xstate = NULL;
					}
					PG_CATCH();
					{
						if (xstate)
							GenericXLogAbort((GenericXLogState *) xstate);
						PG_RE_THROW();
					}
					PG_END_TRY();
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

					{
					GenericXLogState * volatile xstate = GenericXLogStart(index);
					PG_TRY();
					{
						cur_img = GenericXLogRegisterBuffer(
							(GenericXLogState *) xstate, leafbuf, 0);
						new_img = GenericXLogRegisterBuffer(
							(GenericXLogState *) xstate, newleaf_buf,
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

						GenericXLogFinish((GenericXLogState *) xstate);
						xstate = NULL;
					}
					PG_CATCH();
					{
						if (xstate)
							GenericXLogAbort((GenericXLogState *) xstate);
						PG_RE_THROW();
					}
					PG_END_TRY();
					} /* xstate scope */
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
				{
					Buffer				 newleaf_buf;
					BlockNumber			 newleaf_blkno;
					GenericXLogState	* volatile xstate;
					Page				 new_img;
					Buffer				 prev = InvalidBuffer;

					/*
					 * Lock ordering: acquire prev (lower block) before extending
					 * the relation (new page always has a higher block number).
					 * This matches the nbtree convention and prevents deadlock
					 * with concurrent left-to-right leaf chain walkers.
					 */
					if (rightmost_leaf != InvalidBlockNumber)
					{
						prev = ReadBuffer(index, rightmost_leaf);
						LockBuffer(prev, BUFFER_LOCK_EXCLUSIVE);
					}

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

					xstate = GenericXLogStart(index);
					PG_TRY();
					{
						new_img = GenericXLogRegisterBuffer((GenericXLogState *) xstate,
															newleaf_buf,
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
							Page prev_img = GenericXLogRegisterBuffer(
								(GenericXLogState *) xstate, prev, 0);

							((RoaringLeafSpecial *)
							 PageGetSpecialPointer(prev_img))->right_page = newleaf_blkno;
							GenericXLogFinish((GenericXLogState *) xstate);
							xstate = NULL;
							UnlockReleaseBuffer(prev);
						}
						else
						{
							GenericXLogFinish((GenericXLogState *) xstate);
							xstate = NULL;
							leftmost_leaf = newleaf_blkno;
						}
					}
					PG_CATCH();
					{
						if (xstate)
							GenericXLogAbort((GenericXLogState *) xstate);
						PG_RE_THROW();
					}
					PG_END_TRY();

					UnlockReleaseBuffer(newleaf_buf);
					rightmost_leaf	 = newleaf_blkno;
					inserted		 = true;
					leaf_structure_changed = true;
				}
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
	 * Step D: atomically commit the merge — clear all active merging_heads and
	 * update the directory pointer and entry count in a single WAL record.
	 */
	metabuf  = ReadBuffer(index, ROARING_METAPAGE_BLKNO);
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
	{
		GenericXLogState * volatile xstate = GenericXLogStart(index);
		PG_TRY();
		{
			Page			  img = GenericXLogRegisterBuffer(
				(GenericXLogState *) xstate, metabuf, 0);
			RoaringMetaPageData *m  = RoaringPageGetMeta(img);
			int					  si;

			m->root_directory_page = root_dir;
			m->leftmost_leaf_page  = leftmost_leaf;
			m->rightmost_leaf_page = rightmost_leaf;
			m->total_entries	   = new_total_entries;
			for (si = 0; si < ROARING_PENDING_SHARDS; si++)
				if (shard_active[si])
					m->shards[si].merging_head = InvalidBlockNumber;
			m->flags &= ~ROARING_FLAG_BGMERGE_SPAWNED;

			GenericXLogFinish((GenericXLogState *) xstate);
			xstate = NULL;
		}
		PG_CATCH();
		{
			if (xstate)
				GenericXLogAbort((GenericXLogState *) xstate);
			PG_RE_THROW();
		}
		PG_END_TRY();
	}
	UnlockReleaseBuffer(metabuf);

	/*
	 * Step E: recycle the frozen pending chain pages.
	 *
	 * All active merging_heads were cleared in Step D, so the frozen chains
	 * are no longer reachable from the metapage.  Re-acquire metapage EX
	 * and push every page from each frozen chain onto the free list.
	 *
	 * Lock ordering: metapage (blk 0) before any frozen page — same as
	 * every other path that locks metapage + another buffer.
	 */
	{
		int s;
		int i;

		metabuf = ReadBuffer(index, ROARING_METAPAGE_BLKNO);
		LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);

		for (s = 0; s < ROARING_PENDING_SHARDS; s++)
		{
			if (!shard_active[s] || frozen_heads[s] == InvalidBlockNumber)
				continue;
			recycle_chain(index, metabuf, frozen_heads[s]);
		}

		/* Recycle old overflow chains replaced during Step 3. */
		for (i = 0; i < old_ovf_ctx.n; i++)
			recycle_chain(index, metabuf, old_ovf_ctx.heads[i]);

		UnlockReleaseBuffer(metabuf);
	}

	if (old_ovf_ctx.heads != NULL)
		pfree(old_ovf_ctx.heads);

	} /* end old_ovf_ctx scope */
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
						void *callback_state,
						Relation heaprel,
						Buffer *vmbuf,
						OvfRecycleCtx *recycle_ctx)
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
		roaring64_bitmap_t *bm;
		roaring64_bitmap_t *dead_bm = NULL;
		bool				has_dead;
		bool				was_overflow;
		BlockNumber			old_ovf_blkno;

		iid = PageGetItemId(img, off);
		if (!ItemIdIsUsed(iid))
			continue;

		le = (RoaringLeafEntry *) PageGetItem(img, iid);
		was_overflow = (le->flags & ROARING_ENTRY_OVERFLOW) != 0;
		old_ovf_blkno = InvalidBlockNumber;
		bm = NULL;

		if (was_overflow)
		{
			RoaringOverflowEntry oe_copy;

			memcpy(&oe_copy, le, sizeof(RoaringOverflowEntry));
			old_ovf_blkno = oe_copy.overflow_blkno;
			bm = roaring_read_overflow_bitmap(index, &oe_copy);
		}
		else
		{
			bm_bytes = ItemIdGetLength(iid) - sizeof(RoaringLeafEntry);
			bm = roaring64_bitmap_portable_deserialize_safe((char *)(le + 1),
															bm_bytes);
		}
		if (!bm)
			continue;					/* corrupted — leave alone */

		/*
		 * bm uses libc malloc, not palloc, so PG error recovery will not free
		 * it.  PG_FINALLY ensures roaring64_bitmap_free runs even if
		 * PageIndexTupleOverwrite or palloc below throws.
		 */
		PG_TRY();
		{
			/*
			 * Bulk-extract all TIDs then scan the flat array.
			 * roaring64_bitmap_to_uint64_array is 3-5x faster than the
			 * iterator protocol for the sizes we see in practice.
			 */
			{
				uint64	 bm_count = roaring64_bitmap_get_cardinality(bm);
				uint64	*all_tids = (uint64 *) palloc_extended((size_t) bm_count * sizeof(uint64),
																	MCXT_ALLOC_HUGE);
				uint64	*dead_arr = (uint64 *) palloc_extended((size_t) bm_count * sizeof(uint64),
																	MCXT_ALLOC_HUGE);
				uint64	 ndead    = 0;
				uint64	 j;

				roaring64_bitmap_to_uint64_array(bm, all_tids);

				{
					BlockNumber prev_hblk      = InvalidBlockNumber;
					bool		blk_all_visible = false;

					for (j = 0; j < bm_count; j++)
					{
						uint64		ltid   = all_tids[j];
						BlockNumber hblkno = (BlockNumber)(ltid >> 9);

						/*
						 * roaring64 stores values in sorted order, so TIDs from
						 * the same heap block are contiguous.  Check the VM once
						 * per block transition; skip callback on all-visible pages
						 * (no dead TIDs possible).
						 */
						if (hblkno != prev_hblk)
						{
							if (heaprel)
							{
								uint8 vm_status =
									visibilitymap_get_status(heaprel, hblkno, vmbuf);
								blk_all_visible =
									(vm_status & VISIBILITYMAP_ALL_VISIBLE) != 0;
							}
							else
								blk_all_visible = false;
							prev_hblk = hblkno;
						}
						if (blk_all_visible)
							continue;

						{
							ItemPointerData tid;

							ItemPointerSetBlockNumber(&tid, hblkno);
							ItemPointerSetOffsetNumber(&tid,
								(OffsetNumber)((ltid & 0x1FF) + 1));
							if (callback(&tid, callback_state))
							{
								dead_arr[ndead++] = ltid;
								stats->tuples_removed++;
							}
						}
					}
				}
				pfree(all_tids);

				has_dead = (ndead > 0);
				if (has_dead)
				{
					dead_bm = roaring64_bitmap_create();
					roaring64_bitmap_add_many(dead_bm, (size_t) ndead, dead_arr);
				}
				pfree(dead_arr);
			}

			if (has_dead)
			{
				int64    value	     = le->value;
				uint32   bm_card;
				size_t   new_bm_bytes;

				roaring64_bitmap_andnot_inplace(bm, dead_bm);
				roaring64_bitmap_free(dead_bm);
				dead_bm = NULL;
				changed = true;

				bm_card = roaring64_cardinality32(bm);

				if (bm_card == 0)
				{
					/* All TIDs were dead — delete the entry entirely. */
					roaring64_bitmap_free(bm);
					bm = NULL;
					PageIndexTupleDelete(img, off);
					((RoaringLeafSpecial *) PageGetSpecialPointer(img))->entry_count--;
					off--;
					maxoff--;
				}
				else
				{
					new_bm_bytes = roaring64_bitmap_portable_size_in_bytes(bm);

					if (was_overflow)
					{
						char				 *bm_data  = (char *) palloc(new_bm_bytes);
						RoaringOverflowEntry  new_oe;

						roaring64_bitmap_portable_serialize(bm, bm_data);
						new_oe.value		  = value;
						new_oe.cardinality	  = bm_card;
						new_oe.flags		  = ROARING_ENTRY_OVERFLOW;
						new_oe.total_len	  = (uint32) new_bm_bytes;
						new_oe.overflow_blkno = roaring_write_overflow_chain(index,
																			  bm_data,
																			  new_bm_bytes);
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
						new_le->cardinality	= bm_card;
						new_le->flags		= ROARING_ENTRY_INLINE;
						roaring64_bitmap_portable_serialize(bm, (char *)(new_le + 1));

						if (!PageIndexTupleOverwrite(img, off, (Item) new_le, new_size))
							elog(ERROR,
								 "pg_roaring_index: vacuum: PageIndexTupleOverwrite failed "
								 "for value " INT64_FORMAT, value);
						pfree(new_le);
					}

					stats->num_index_tuples++;
					roaring64_bitmap_free(bm);
					bm = NULL;
				}

				/*
				 * Old overflow chain is now unreachable (leaf updated to point
				 * to new chain or inline data, or entry deleted entirely).
				 * Queue for recycle after all leaf writes are committed.
				 */
				if (was_overflow)
					ovf_recycle_add(recycle_ctx, old_ovf_blkno);
			}
			else
			{
				roaring64_bitmap_free(dead_bm);
				dead_bm = NULL;

				/* Entry survives unchanged — count it. */
				stats->num_index_tuples++;
				roaring64_bitmap_free(bm);
				bm = NULL;
			}
		}
		PG_FINALLY();
		{
			if (bm != NULL)
				roaring64_bitmap_free(bm);
			if (dead_bm != NULL)
			{
				roaring64_bitmap_free(dead_bm);
				dead_bm = NULL;
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
 * roaring_bulkdelete
 *
 * Walk all leaf pages; remove dead TIDs from bitmaps via the VACUUM callback.
 * Parallel vacuum is enabled (VACUUM_OPTION_PARALLEL_BULKDEL): workers have
 * disjoint dead-TID sets (different heap pages), and each leaf page is
 * modified under an exclusive buffer lock so concurrent writes are serialised.
 *
 * Pending list dead entries are NOT compacted here — they are merged into
 * leaf bitmaps during vacuumcleanup and removed from there on the next
 * ambulkdelete pass.  This is correct (BitmapHeapScan always rechecks) and
 * avoids the concurrency hazard of multiple workers writing the same pending
 * pages simultaneously.
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
	BlockNumber			 cur;
	Buffer				 vmbuf = InvalidBuffer;
	OvfRecycleCtx		 recycle_ctx;

	recycle_ctx.heads = NULL;
	recycle_ctx.n     = 0;
	recycle_ctx.cap   = 0;

	if (stats == NULL)
		stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));

	/* Read metapage under share lock for leftmost leaf. */
	metabuf  = ReadBuffer(index, ROARING_METAPAGE_BLKNO);
	LockBuffer(metabuf, BUFFER_LOCK_SHARE);
	metapage = BufferGetPage(metabuf);
	meta     = RoaringPageGetMeta(metapage);
	leftmost = meta->leftmost_leaf_page;
	UnlockReleaseBuffer(metabuf);

	/* Walk leaf pages left to right, removing dead TIDs in-place. */
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

		roaring_vacuum_one_leaf(index, buf, stats, callback, callback_state,
								info->heaprel, &vmbuf, &recycle_ctx);

		UnlockReleaseBuffer(buf);
		cur = next;
	}

	if (BufferIsValid(vmbuf))
		ReleaseBuffer(vmbuf);

	/* Recycle old overflow chains replaced during this VACUUM pass. */
	ovf_recycle_flush(index, &recycle_ctx);

	return stats;
}

/* ----------------------------------------------------------------
 * roaring_bulkdelete_lossy
 *
 * Lossy mode does not store individual TIDs, so we cannot remove dead
 * TIDs from bitmaps at ambulkdelete time.  Stale block numbers cause
 * extra rechecks (which find no matching rows) but are not incorrect.
 * We walk the leaf chain only to populate num_index_tuples for stats.
 *
 * Dead block entries are cleaned up implicitly: after vacuumcleanup
 * merges fresh pending inserts, re-inserted values re-add any live
 * blocks.  A future pass could rebuild bitmaps from scratch to reclaim
 * space from fully-dead blocks, but that is not implemented yet.
 * ---------------------------------------------------------------- */
IndexBulkDeleteResult *
roaring_bulkdelete_lossy(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
						 IndexBulkDeleteCallback callback, void *callback_state)
{
	Relation			 index = info->index;
	Buffer				 metabuf;
	RoaringMetaPageData *meta;
	BlockNumber			 leftmost;
	BlockNumber			 cur;

	if (stats == NULL)
		stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));

	metabuf  = ReadBuffer(index, ROARING_METAPAGE_BLKNO);
	LockBuffer(metabuf, BUFFER_LOCK_SHARE);
	meta     = RoaringPageGetMeta(BufferGetPage(metabuf));
	leftmost = meta->leftmost_leaf_page;
	UnlockReleaseBuffer(metabuf);

	/* Walk leaf pages to count distinct values (entries). */
	cur = leftmost;
	while (cur != InvalidBlockNumber)
	{
		Buffer				buf;
		Page				page;
		RoaringLeafSpecial *spc;

		buf  = ReadBuffer(index, cur);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		spc  = (RoaringLeafSpecial *) PageGetSpecialPointer(page);

		stats->num_index_tuples += PageGetMaxOffsetNumber(page);
		cur = spc->right_page;
		UnlockReleaseBuffer(buf);
	}

	return stats;
}

/* ----------------------------------------------------------------
 * roaring_resummarize_lossy
 *
 * Remove stale block numbers from lossy bitmaps.  For each value V, read
 * only the heap pages listed in V's bitmap and check for live tuples with
 * the indexed value.  Block numbers with no live V-tuples are removed.
 *
 * This is the BRIN-style dead-block cleanup: O(bitmap_pages) not O(heap).
 *
 * Holding index leaf EXCLUSIVE while reading heap pages is safe: concurrent
 * scans that have already returned their bitmap hold no index locks, so the
 * lock sets are disjoint and no deadlock is possible.
 *
 * Liveness check: any LP_NORMAL heap tuple with a valid xmin and the indexed
 * value counts as "live enough" to retain the block.  Conservative (we may
 * keep blocks where the only V-tuple has a committed xmax), but never drops
 * a block that could still be needed.  A subsequent VACUUM round will prune
 * such tuples to LP_DEAD and the next vacuumcleanup will drop the block.
 * ---------------------------------------------------------------- */
static void
roaring_resummarize_lossy(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	const int			 max_inline = (int)(BLCKSZ
										   - MAXALIGN(sizeof(RoaringLeafSpecial))
										   - SizeOfPageHeaderData
										   - sizeof(ItemIdData)
										   - MAXALIGN(sizeof(RoaringLeafEntry)));
	Relation			 index        = info->index;
	Relation			 heap         = info->heaprel;
	TupleDesc			 tupdesc;
	Oid					 col_typid;
	AttrNumber			 heap_attnum;
	BlockNumber			 heap_nblocks;
	BlockNumber			 leftmost, cur;
	Buffer				 metabuf;

	if (!heap)
		return;

	tupdesc      = RelationGetDescr(heap);
	col_typid    = TupleDescAttr(index->rd_att, 0)->atttypid;
	{
		IndexInfo *ii = BuildIndexInfo(index);
		heap_attnum  = ii->ii_IndexAttrNumbers[0];  /* 1-based heap column */
	}
	heap_nblocks = RelationGetNumberOfBlocks(heap);

	metabuf  = ReadBuffer(index, ROARING_METAPAGE_BLKNO);
	LockBuffer(metabuf, BUFFER_LOCK_SHARE);
	leftmost = RoaringPageGetMeta(BufferGetPage(metabuf))->leftmost_leaf_page;
	UnlockReleaseBuffer(metabuf);

	{
	OvfRecycleCtx recycle_ctx;

	recycle_ctx.heads = NULL;
	recycle_ctx.n     = 0;
	recycle_ctx.cap   = 0;

	for (cur = leftmost; cur != InvalidBlockNumber; )
	{
		Buffer				 leafbuf;
		Page				 leafpage;
		RoaringLeafSpecial	*lspc;
		OffsetNumber		 off, maxoff;
		BlockNumber			 next;
		GenericXLogState	* volatile xstate;
		Page				 img;
		bool				 page_changed = false;

		leafbuf  = ReadBuffer(index, cur);
		LockBuffer(leafbuf, BUFFER_LOCK_EXCLUSIVE);
		leafpage = BufferGetPage(leafbuf);
		lspc     = (RoaringLeafSpecial *) PageGetSpecialPointer(leafpage);
		maxoff   = PageGetMaxOffsetNumber(leafpage);
		next     = lspc->right_page;

		xstate = GenericXLogStart(index);
		img    = GenericXLogRegisterBuffer((GenericXLogState *) xstate, leafbuf, 0);

		PG_TRY();
		{
			for (off = FirstOffsetNumber; off <= maxoff; off++)
			{
				ItemId            iid;
				RoaringLeafEntry *le;
				int64             value;
				bool              was_overflow;
				BlockNumber       old_ovf_blkno;
				roaring_bitmap_t * volatile bm    = NULL;
				roaring_bitmap_t * volatile new_bm = NULL;
				uint32            old_card, new_card;

				iid = PageGetItemId(img, off);
				if (!ItemIdIsUsed(iid))
					continue;

				le            = (RoaringLeafEntry *) PageGetItem(img, iid);
				value         = le->value;
				was_overflow  = (le->flags & ROARING_ENTRY_OVERFLOW) != 0;
				old_ovf_blkno = InvalidBlockNumber;

				PG_TRY();
				{
					if (was_overflow)
					{
						RoaringOverflowEntry oe_copy;

						memcpy(&oe_copy, le, sizeof(RoaringOverflowEntry));
						old_ovf_blkno = oe_copy.overflow_blkno;
						/*
						 * roaring_read_overflow_bitmap_lossy takes share locks on
						 * overflow pages.  We hold leafbuf exclusive — safe
						 * because overflow pages are always higher-numbered
						 * than the leaf and no other process holds overflow
						 * exclusive while we own this leaf.
						 */
						bm = roaring_read_overflow_bitmap_lossy(index, &oe_copy);
					}
					else
					{
						size_t bm_bytes = ItemIdGetLength(iid)
										  - sizeof(RoaringLeafEntry);
						bm = roaring_bitmap_portable_deserialize_safe(
								(const char *)(le + 1), bm_bytes);
					}

					if (bm != NULL)
					{
						roaring_uint32_iterator_t it;

						old_card = roaring_cardinality32(bm);
						new_bm   = roaring_bitmap_create();

						roaring_iterator_init(bm, &it);
						while (it.has_value)
						{
							BlockNumber hblkno  = (BlockNumber) it.current_value;
							bool        has_live = false;

							if (hblkno < heap_nblocks)
							{
								Buffer       hbuf;
								Page         hpage;
								OffsetNumber ho, hmaxoff;

								hbuf = ReadBufferExtended(heap, MAIN_FORKNUM,
														  hblkno, RBM_NORMAL,
														  info->strategy);
								LockBuffer(hbuf, BUFFER_LOCK_SHARE);
								hpage   = BufferGetPage(hbuf);
								hmaxoff = PageGetMaxOffsetNumber(hpage);

								for (ho = FirstOffsetNumber;
									 ho <= hmaxoff && !has_live;
									 ho++)
								{
									ItemId        hiid = PageGetItemId(hpage, ho);
									HeapTupleData htup;
									bool          isnull;
									Datum         val;
									int64         col_val;

									if (!ItemIdIsNormal(hiid))
										continue;

									htup.t_data     = (HeapTupleHeader)
													  PageGetItem(hpage, hiid);
									htup.t_len      = ItemIdGetLength(hiid);
									htup.t_tableOid = RelationGetRelid(heap);
									ItemPointerSet(&htup.t_self, hblkno, ho);

									/*
									 * Skip tuples whose xmin was never committed
									 * (aborted insert).  Tuples with valid xmin
									 * but committed xmax are conservatively kept;
									 * they will be pruned on a future VACUUM.
									 */
									if (HeapTupleHeaderXminInvalid(htup.t_data))
										continue;

									val = heap_getattr(&htup, heap_attnum,
													   tupdesc, &isnull);
									if (isnull)
										continue;

									col_val = roaring_datum_to_key64(val, col_typid);
									if (col_val == value)
										has_live = true;
								}

								UnlockReleaseBuffer(hbuf);
							}
							/* hblkno >= heap_nblocks: truncated — omit from new_bm */

							if (has_live)
								roaring_bitmap_add(new_bm, (uint32) hblkno);

							roaring_uint32_iterator_advance(&it);
						}

						new_card = roaring_cardinality32(new_bm);

						if (new_card != old_card)
						{
							page_changed = true;

							if (new_card == 0)
							{
								PageIndexTupleDelete(img, off);
								((RoaringLeafSpecial *)
								 PageGetSpecialPointer(img))->entry_count--;
								off--;
								maxoff--;
							}
							else
							{
								size_t new_bytes =
									roaring_bitmap_portable_size_in_bytes(new_bm);

								if (was_overflow &&
									new_bytes > (size_t) max_inline)
								{
									/* Still overflow: write a new chain. */
									char  *bm_data = (char *) palloc(new_bytes);
									RoaringOverflowEntry new_oe;

									roaring_bitmap_portable_serialize(new_bm,
																	  bm_data);
									new_oe.value        = value;
									new_oe.cardinality  = new_card;
									new_oe.flags        = ROARING_ENTRY_OVERFLOW;
									new_oe.total_len    = (uint32) new_bytes;
									new_oe.overflow_blkno =
										roaring_write_overflow_chain(
											index, bm_data, new_bytes);
									pfree(bm_data);

									if (!PageIndexTupleOverwrite(
											img, off,
											(Item) &new_oe,
											sizeof(RoaringOverflowEntry)))
										elog(ERROR,
											 "pg_roaring_index: resummarize: "
											 "PageIndexTupleOverwrite (overflow) "
											 "failed for value " INT64_FORMAT,
											 value);
								}
								else
								{
									/* Inline (or overflow shrunk to inline). */
									Size new_sz = sizeof(RoaringLeafEntry)
												  + new_bytes;
									RoaringLeafEntry *new_le =
										(RoaringLeafEntry *) palloc(new_sz);

									new_le->value       = value;
									new_le->cardinality = new_card;
									new_le->flags       = ROARING_ENTRY_INLINE;
									roaring_bitmap_portable_serialize(
										new_bm, (char *)(new_le + 1));

									if (!PageIndexTupleOverwrite(img, off,
																  (Item) new_le,
																  new_sz))
										elog(ERROR,
											 "pg_roaring_index: resummarize: "
											 "PageIndexTupleOverwrite (inline) "
											 "failed for value " INT64_FORMAT,
											 value);
									pfree(new_le);
								}
							}
						}
						/*
						 * Queue old overflow chain for deferred recycle.  Done
						 * inside bm != NULL so new_card is guaranteed to be set.
						 */
						if (was_overflow && new_card != old_card)
							ovf_recycle_add(&recycle_ctx, old_ovf_blkno);
					} /* bm != NULL */
				}
				PG_FINALLY();
				{
					if (bm)    { roaring_bitmap_free(bm);    bm    = NULL; }
					if (new_bm){ roaring_bitmap_free(new_bm);new_bm = NULL; }
				}
				PG_END_TRY();
			} /* for off */

			if (page_changed)
				GenericXLogFinish((GenericXLogState *) xstate);
			else
				GenericXLogAbort((GenericXLogState *) xstate);
			xstate = NULL;
		}
		PG_CATCH();
		{
			if (xstate)
				GenericXLogAbort((GenericXLogState *) xstate);
			PG_RE_THROW();
		}
		PG_END_TRY();

		UnlockReleaseBuffer(leafbuf);
		cur = next;
	}

	/* Recycle old overflow chains replaced during the resummarize pass. */
	ovf_recycle_flush(index, &recycle_ctx);

	} /* end recycle_ctx scope */
}

/* ----------------------------------------------------------------
 * roaring_vacuumcleanup
 *
 * Called after VACUUM completes its heap pass.  Handles crash recovery
 * first, then merges the pending insert list into the main index.
 *
 * Crash recovery: if a prior merge was interrupted, pending_merging_head
 * remains set.  roaring_merge_pending treats a set pending_merging_head as
 * "another merger is active" and bails out immediately — correct for the
 * concurrent-merger case, but wrong after a crash where no other backend
 * holds the merge lock.  vacuumcleanup is the authoritative single-writer
 * merge context (PostgreSQL serializes VACUUM per relation), so any set
 * pending_merging_head seen here is always crash recovery, never a live
 * concurrent merger.
 *
 * Two crash cases:
 *   Case 1 — crash before Step C (pending_insert_head == pending_merging_head):
 *     The old chain is still in place as the active insert head.  Just clear
 *     the anchor; roaring_merge_pending picks it up on the normal path.
 *
 *   Case 2 — crash after Step C (pending_insert_head != pending_merging_head):
 *     Step C had already swapped in a carry chain as pending_insert_head.
 *     The old merging chain is stranded at pending_merging_head.  Walk to
 *     its tail, link the carry chain onto it, re-route pending_insert_head
 *     to the old chain head, then clear pending_merging_head.  OR-in is
 *     idempotent, so re-merging entries that were already partially merged
 *     before the crash is safe.
 * ---------------------------------------------------------------- */
IndexBulkDeleteResult *
roaring_vacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	Relation			 index = info->index;
	Buffer				 metabuf;
	RoaringMetaPageData *meta;
	BlockNumber			 merging_heads[ROARING_PENDING_SHARDS];
	BlockNumber			 insert_heads[ROARING_PENDING_SHARDS];
	BlockNumber			 carry_heads[ROARING_PENDING_SHARDS];
	uint32				 insert_counts[ROARING_PENDING_SHARDS];
	uint16				 flags;
	bool				 any_stuck = false;
	int					 s;

	if (stats == NULL)
		stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));

	/* Read per-shard state; check for crash-left merging_heads. */
	metabuf = ReadBuffer(index, ROARING_METAPAGE_BLKNO);
	LockBuffer(metabuf, BUFFER_LOCK_SHARE);
	meta    = RoaringPageGetMeta(BufferGetPage(metabuf));
	roaring_validate_metapage(index, meta);
	for (s = 0; s < ROARING_PENDING_SHARDS; s++)
	{
		merging_heads[s]  = meta->shards[s].merging_head;
		insert_heads[s]   = meta->shards[s].insert_head;
		carry_heads[s]    = meta->shards[s].carry_head;
		insert_counts[s]  = meta->shards[s].insert_count;
		if (merging_heads[s] != InvalidBlockNumber)
			any_stuck = true;
	}
	flags = meta->flags;
	UnlockReleaseBuffer(metabuf);

	/*
	 * Per-shard crash recovery.  PostgreSQL serializes VACUUM per relation,
	 * so any set merging_head here is always crash recovery, never a live
	 * concurrent merger.
	 *
	 * Case 1 (per shard): crash before Step C — shards[s].insert_head still
	 *   equals merging_head.  Clear both anchors for this shard.
	 *
	 * Case 2 (per shard): crash after Step C — insert_head was already swapped
	 *   to carry/fresh chain; old merging chain is stranded.  Fuse the two
	 *   chains in one WAL record.
	 */
	if (any_stuck)
	{
		/*
		 * Case 1 shards: clear merging_head and carry_head in one WAL.
		 * Case 2 shards: each gets its own 2-buffer WAL (tail page + metapage).
		 */
		bool case1_any = false;

		for (s = 0; s < ROARING_PENDING_SHARDS; s++)
		{
			if (merging_heads[s] == InvalidBlockNumber)
				continue;
			if (insert_heads[s] == merging_heads[s])
				case1_any = true;
		}

		if (case1_any)
		{
			metabuf = ReadBuffer(index, ROARING_METAPAGE_BLKNO);
			LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
			{
				GenericXLogState *state = GenericXLogStart(index);
				Page			  img   = GenericXLogRegisterBuffer(state, metabuf, 0);
				RoaringMetaPageData *m  = RoaringPageGetMeta(img);

				for (s = 0; s < ROARING_PENDING_SHARDS; s++)
				{
					if (merging_heads[s] == InvalidBlockNumber)
						continue;
					if (insert_heads[s] == merging_heads[s])
					{
						m->shards[s].merging_head = InvalidBlockNumber;
						m->shards[s].carry_head   = InvalidBlockNumber;
					}
				}
				GenericXLogFinish(state);
			}

			/*
			 * Recycle any stranded carry-chain pages from the aborted merge.
			 * The carry chain was allocated in Step B but never installed as
			 * the insert head (crash happened before Step C).  The metapage
			 * entry was cleared above, so the pages are now unreachable.
			 */
			for (s = 0; s < ROARING_PENDING_SHARDS; s++)
			{
				if (merging_heads[s] != InvalidBlockNumber &&
					insert_heads[s] == merging_heads[s] &&
					carry_heads[s] != InvalidBlockNumber)
					recycle_chain(index, metabuf, carry_heads[s]);
			}
			UnlockReleaseBuffer(metabuf);
		}

		for (s = 0; s < ROARING_PENDING_SHARDS; s++)
		{
			BlockNumber tail_blkno;
			uint32		old_count;
			Buffer		pb;
			GenericXLogState *state;
			Page		pb_img, meta_img;
			RoaringMetaPageData *m;

			if (merging_heads[s] == InvalidBlockNumber)
				continue;
			if (insert_heads[s] == merging_heads[s])
				continue;	/* handled as Case 1 above */

			/*
			 * Case 2: carry/fresh chain at insert_head; old merging chain
			 * stranded at merging_heads[s].  Walk to its tail and fuse.
			 */
			tail_blkno = merging_heads[s];
			old_count  = 0;

			for (;;)
			{
				RoaringPendingSpecial *pspc;
				BlockNumber			   next;

				pb   = ReadBuffer(index, tail_blkno);
				LockBuffer(pb, BUFFER_LOCK_SHARE);
				pspc = (RoaringPendingSpecial *)
						PageGetSpecialPointer(BufferGetPage(pb));
				next      = pspc->next_page;
				old_count += pspc->entry_count;
				UnlockReleaseBuffer(pb);

				if (next == InvalidBlockNumber)
					break;
				tail_blkno = next;
			}

			metabuf = ReadBuffer(index, ROARING_METAPAGE_BLKNO);
			pb      = ReadBuffer(index, tail_blkno);
			LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
			LockBuffer(pb,      BUFFER_LOCK_EXCLUSIVE);

			state    = GenericXLogStart(index);
			pb_img   = GenericXLogRegisterBuffer(state, pb,      0);
			meta_img = GenericXLogRegisterBuffer(state, metabuf, 0);

			((RoaringPendingSpecial *)
			 PageGetSpecialPointer(pb_img))->next_page = insert_heads[s];

			m = RoaringPageGetMeta(meta_img);
			m->shards[s].insert_head  = merging_heads[s];
			m->shards[s].insert_count = old_count + insert_counts[s];
			m->shards[s].merging_head = InvalidBlockNumber;

			GenericXLogFinish(state);
			UnlockReleaseBuffer(pb);
			UnlockReleaseBuffer(metabuf);
		}
	}

	roaring_merge_pending(info->index);

	/* Dead-block cleanup only for single-column lossy indexes. */
	if ((flags & ROARING_FLAG_LOSSY) &&
		info->index->rd_att->natts == 1)
		roaring_resummarize_lossy(info, stats);

	return stats;
}
