/*
 * roaring_payload.c
 *
 * T65: Dense int64 payload store for INCLUDE column projection.
 *
 * Maps linear_tid → int64 PK value using a 2-level directory tree:
 *
 *   metapage.payload_dir_head
 *       → root dir page  (array of BlockNumber, one per leaf dir page)
 *           → leaf dir page  (array of BlockNumber, one per payload page)
 *               → payload page  (array of int64, one per TID slot)
 *
 * Addressing:
 *   payload_idx    = linear_tid / ROARING_PAYLOAD_PK_PER_PAGE
 *   payload_offset = linear_tid % ROARING_PAYLOAD_PK_PER_PAGE
 *   dir_root_idx   = payload_idx / ROARING_PAYLOAD_DIR_ENTRIES_PER_PAGE
 *   dir_leaf_idx   = payload_idx % ROARING_PAYLOAD_DIR_ENTRIES_PER_PAGE
 *
 * All modifications are WAL-logged via generic_xlog.  Lookup is lock-free
 * on the hot path (share locks only).  Page creation uses the standard
 * "check under share, create under exclusive, re-check" pattern.
 */

#include "pg_roaring_index.h"

#include "access/generic_xlog.h"
#include "access/relscan.h"
#include "storage/bufmgr.h"
#include "utils/memutils.h"
#include "utils/rel.h"

/* ----------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------- */

/*
 * payload_get_root_dir
 *
 * Return the root directory block number, creating it if necessary.
 */
static BlockNumber
payload_get_root_dir(Relation index)
{
	Buffer				 metabuf;
	Page				 metapage;
	RoaringMetaPageData *meta;
	BlockNumber			 root_blkno;

	/* Fast path: share lock */
	metabuf  = ReadBuffer(index, ROARING_METAPAGE_BLKNO);
	LockBuffer(metabuf, BUFFER_LOCK_SHARE);
	metapage = BufferGetPage(metabuf);
	meta     = RoaringPageGetMeta(metapage);
	root_blkno = meta->payload_dir_head;
	UnlockReleaseBuffer(metabuf);

	if (root_blkno != InvalidBlockNumber)
		return root_blkno;

	/* Slow path: create under exclusive lock */
	{
		GenericXLogState	 *state;
		Buffer				  root_buf;
		Page				  root_page;
		RoaringPayloadDirSpecial *root_spc;

		state   = GenericXLogStart(index);
		metabuf = ReadBuffer(index, ROARING_METAPAGE_BLKNO);
		LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
		metapage = GenericXLogRegisterBuffer(state, metabuf, 0);
		meta     = RoaringPageGetMeta(metapage);

		if (meta->payload_dir_head != InvalidBlockNumber)
		{
			/* Lost the race; another session created it. */
			root_blkno = meta->payload_dir_head;
			GenericXLogAbort(state);
			UnlockReleaseBuffer(metabuf);
			return root_blkno;
		}

		root_buf  = ReadBuffer(index, P_NEW);
		LockBuffer(root_buf, BUFFER_LOCK_EXCLUSIVE);
		root_page = GenericXLogRegisterBuffer(state, root_buf, GENERIC_XLOG_FULL_IMAGE);
		PageInit(root_page, BLCKSZ, sizeof(RoaringPayloadDirSpecial));
		root_spc  = (RoaringPayloadDirSpecial *) PageGetSpecialPointer(root_page);
		root_spc->page_type  = ROARING_PAGE_PAYLOAD_DIR;
		root_spc->level      = 1;
		root_spc->entry_count = 0;

		root_blkno = BufferGetBlockNumber(root_buf);
		meta->payload_dir_head = root_blkno;

		GenericXLogFinish(state);
		UnlockReleaseBuffer(root_buf);
		UnlockReleaseBuffer(metabuf);

		return root_blkno;
	}
}

/*
 * payload_get_leaf_dir
 *
 * Return the leaf directory block number for dir_root_idx, creating it if
 * necessary.
 */
static BlockNumber
payload_get_leaf_dir(Relation index, BlockNumber root_blkno, uint32 dir_root_idx)
{
	Buffer				  root_buf;
	Page				  root_page;
	BlockNumber			 *root_entries;
	RoaringPayloadDirSpecial *root_spc;
	BlockNumber			  leaf_blkno;

	/* Fast path: share lock */
	root_buf     = ReadBuffer(index, root_blkno);
	LockBuffer(root_buf, BUFFER_LOCK_SHARE);
	root_page    = BufferGetPage(root_buf);
	root_entries = (BlockNumber *) PageGetContents(root_page);
	root_spc     = (RoaringPayloadDirSpecial *) PageGetSpecialPointer(root_page);

	leaf_blkno = (dir_root_idx < root_spc->entry_count)
		? root_entries[dir_root_idx]
		: InvalidBlockNumber;
	UnlockReleaseBuffer(root_buf);

	if (leaf_blkno != InvalidBlockNumber)
		return leaf_blkno;

	/* Slow path: create under exclusive lock */
	{
		GenericXLogState	 *state;
		Buffer				  leaf_buf;
		Page				  leaf_page;
		RoaringPayloadDirSpecial *leaf_spc;
		uint32				  i;

		state    = GenericXLogStart(index);
		root_buf = ReadBuffer(index, root_blkno);
		LockBuffer(root_buf, BUFFER_LOCK_EXCLUSIVE);
		root_page    = GenericXLogRegisterBuffer(state, root_buf, 0);
		root_entries = (BlockNumber *) PageGetContents(root_page);
		root_spc     = (RoaringPayloadDirSpecial *) PageGetSpecialPointer(root_page);

		if (dir_root_idx < root_spc->entry_count)
		{
			/* Lost the race. */
			leaf_blkno = root_entries[dir_root_idx];
			GenericXLogAbort(state);
			UnlockReleaseBuffer(root_buf);
			return leaf_blkno;
		}

		leaf_buf  = ReadBuffer(index, P_NEW);
		LockBuffer(leaf_buf, BUFFER_LOCK_EXCLUSIVE);
		leaf_page = GenericXLogRegisterBuffer(state, leaf_buf, GENERIC_XLOG_FULL_IMAGE);
		PageInit(leaf_page, BLCKSZ, sizeof(RoaringPayloadDirSpecial));
		leaf_spc  = (RoaringPayloadDirSpecial *) PageGetSpecialPointer(leaf_page);
		leaf_spc->page_type   = ROARING_PAGE_PAYLOAD_DIR;
		leaf_spc->level       = 0;
		leaf_spc->entry_count = 0;

		leaf_blkno = BufferGetBlockNumber(leaf_buf);

		/* Fill any gap entries with InvalidBlockNumber */
		for (i = root_spc->entry_count; i < dir_root_idx; i++)
			root_entries[i] = InvalidBlockNumber;

		root_entries[dir_root_idx] = leaf_blkno;
		root_spc->entry_count      = dir_root_idx + 1;
		((PageHeader) root_page)->pd_lower =
			(LocationIndex)(SizeOfPageHeaderData +
							root_spc->entry_count * sizeof(BlockNumber));

		GenericXLogFinish(state);
		UnlockReleaseBuffer(leaf_buf);
		UnlockReleaseBuffer(root_buf);

		return leaf_blkno;
	}
}

/*
 * payload_get_page
 *
 * Return the payload page block number for dir_leaf_idx within leaf_dir_blkno,
 * creating it if necessary.
 */
static BlockNumber
payload_get_page(Relation index, BlockNumber leaf_dir_blkno, uint32 dir_leaf_idx)
{
	Buffer				  leaf_buf;
	Page				  leaf_page;
	BlockNumber			 *leaf_entries;
	RoaringPayloadDirSpecial *leaf_spc;
	BlockNumber			  payload_blkno;

	/* Fast path: share lock */
	leaf_buf     = ReadBuffer(index, leaf_dir_blkno);
	LockBuffer(leaf_buf, BUFFER_LOCK_SHARE);
	leaf_page    = BufferGetPage(leaf_buf);
	leaf_entries = (BlockNumber *) PageGetContents(leaf_page);
	leaf_spc     = (RoaringPayloadDirSpecial *) PageGetSpecialPointer(leaf_page);

	payload_blkno = (dir_leaf_idx < leaf_spc->entry_count)
		? leaf_entries[dir_leaf_idx]
		: InvalidBlockNumber;
	UnlockReleaseBuffer(leaf_buf);

	if (payload_blkno != InvalidBlockNumber)
		return payload_blkno;

	/* Slow path: create under exclusive lock */
	{
		GenericXLogState	 *state;
		Buffer				  pay_buf;
		Page				  pay_page;
		RoaringPayloadSpecial *pay_spc;
		uint32				   i;

		state    = GenericXLogStart(index);
		leaf_buf = ReadBuffer(index, leaf_dir_blkno);
		LockBuffer(leaf_buf, BUFFER_LOCK_EXCLUSIVE);
		leaf_page    = GenericXLogRegisterBuffer(state, leaf_buf, 0);
		leaf_entries = (BlockNumber *) PageGetContents(leaf_page);
		leaf_spc     = (RoaringPayloadDirSpecial *) PageGetSpecialPointer(leaf_page);

		if (dir_leaf_idx < leaf_spc->entry_count)
		{
			/* Lost the race. */
			payload_blkno = leaf_entries[dir_leaf_idx];
			GenericXLogAbort(state);
			UnlockReleaseBuffer(leaf_buf);
			return payload_blkno;
		}

		pay_buf  = ReadBuffer(index, P_NEW);
		LockBuffer(pay_buf, BUFFER_LOCK_EXCLUSIVE);
		pay_page = GenericXLogRegisterBuffer(state, pay_buf, GENERIC_XLOG_FULL_IMAGE);
		PageInit(pay_page, BLCKSZ, sizeof(RoaringPayloadSpecial));
		pay_spc  = (RoaringPayloadSpecial *) PageGetSpecialPointer(pay_page);
		pay_spc->page_type   = ROARING_PAGE_PAYLOAD;
		pay_spc->flags       = 0;
		pay_spc->entry_count = 0;
		pay_spc->next_page   = InvalidBlockNumber;

		payload_blkno = BufferGetBlockNumber(pay_buf);

		for (i = leaf_spc->entry_count; i < dir_leaf_idx; i++)
			leaf_entries[i] = InvalidBlockNumber;

		leaf_entries[dir_leaf_idx] = payload_blkno;
		leaf_spc->entry_count      = dir_leaf_idx + 1;
		((PageHeader) leaf_page)->pd_lower =
			(LocationIndex)(SizeOfPageHeaderData +
							leaf_spc->entry_count * sizeof(BlockNumber));

		GenericXLogFinish(state);
		UnlockReleaseBuffer(pay_buf);
		UnlockReleaseBuffer(leaf_buf);

		return payload_blkno;
	}
}

/* ----------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------- */

/*
 * roaring_payload_insert
 *
 * Store pk at the slot corresponding to linear_tid.  Called from
 * roaring_insert (aminsert) and roaring_build_callback, always before
 * the pending entry is written so that any concurrent scan reading the
 * pending entry will find the payload already populated.
 */
void
roaring_payload_insert(Relation index, uint64 linear_tid, int64 pk)
{
	uint32				  payload_idx    = (uint32)(linear_tid / ROARING_PAYLOAD_PK_PER_PAGE);
	uint16				  chunk_offset   = (uint16)(linear_tid % ROARING_PAYLOAD_PK_PER_PAGE);
	uint32				  dir_root_idx   = payload_idx / ROARING_PAYLOAD_DIR_ENTRIES_PER_PAGE;
	uint32				  dir_leaf_idx   = payload_idx % ROARING_PAYLOAD_DIR_ENTRIES_PER_PAGE;
	BlockNumber			  root_blkno;
	BlockNumber			  leaf_blkno;
	BlockNumber			  payload_blkno;
	GenericXLogState	 *state;
	Buffer				  buf;
	Page				  page;
	RoaringPayloadEntry	 *entries;
	RoaringPayloadSpecial *spc;
	int					  i, insert_idx;
	bool				  found = false;

	root_blkno    = payload_get_root_dir(index);
	leaf_blkno    = payload_get_leaf_dir(index, root_blkno, dir_root_idx);
	payload_blkno = payload_get_page(index, leaf_blkno, dir_leaf_idx);

	/* Traverse overflow chain to find space or update existing */
	for (;;)
	{
		int low = 0;
		int high;
		int mid;

		buf     = ReadBuffer(index, payload_blkno);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		page    = BufferGetPage(buf);
		spc     = (RoaringPayloadSpecial *) PageGetSpecialPointer(page);
		entries = (RoaringPayloadEntry *) PageGetContents(page);

		high = spc->entry_count - 1;

		/* Binary search within the page */
		while (low <= high)
		{
			mid = low + (high - low) / 2;
			if (entries[mid].chunk_offset == chunk_offset)
			{
				found = true;
				insert_idx = mid;
				break;
			}
			else if (entries[mid].chunk_offset < chunk_offset)
				low = mid + 1;
			else
				high = mid - 1;
		}

		if (found)
		{
			/* Update existing */
			state = GenericXLogStart(index);
			page = GenericXLogRegisterBuffer(state, buf, 0);
			entries = (RoaringPayloadEntry *) PageGetContents(page);
			entries[insert_idx].pk = pk;
			GenericXLogFinish(state);
			UnlockReleaseBuffer(buf);
			return;
		}

		insert_idx = low;

		if (spc->entry_count < ROARING_PAYLOAD_MAX_ENTRIES)
		{
			/* Insert into this page */
			state = GenericXLogStart(index);
			page = GenericXLogRegisterBuffer(state, buf, 0);
			spc = (RoaringPayloadSpecial *) PageGetSpecialPointer(page);
			entries = (RoaringPayloadEntry *) PageGetContents(page);

			/* Shift elements to keep sorted */
			for (i = spc->entry_count; i > insert_idx; i--)
				entries[i] = entries[i - 1];

			entries[insert_idx].chunk_offset = chunk_offset;
			entries[insert_idx].pk = pk;
			spc->entry_count++;

			((PageHeader) page)->pd_lower =
				(LocationIndex)(SizeOfPageHeaderData +
								spc->entry_count * sizeof(RoaringPayloadEntry));

			GenericXLogFinish(state);
			UnlockReleaseBuffer(buf);
			return;
		}

		/* Page is full. If there is a next page, go there. */
		if (spc->next_page != InvalidBlockNumber)
		{
			BlockNumber next = spc->next_page;
			UnlockReleaseBuffer(buf);
			payload_blkno = next;
			continue;
		}

		/* Allocate new overflow page */
		{
			Buffer next_buf;
			Page next_page;
			RoaringPayloadSpecial *next_spc;
			RoaringPayloadEntry *next_entries;

			next_buf = ReadBuffer(index, P_NEW);
			LockBuffer(next_buf, BUFFER_LOCK_EXCLUSIVE);
			state = GenericXLogStart(index);
			
			/* Register current page to update next_page pointer */
			page = GenericXLogRegisterBuffer(state, buf, 0);
			spc = (RoaringPayloadSpecial *) PageGetSpecialPointer(page);
			spc->next_page = BufferGetBlockNumber(next_buf);

			/* Init new page */
			next_page = GenericXLogRegisterBuffer(state, next_buf, GENERIC_XLOG_FULL_IMAGE);
			PageInit(next_page, BLCKSZ, sizeof(RoaringPayloadSpecial));
			next_spc = (RoaringPayloadSpecial *) PageGetSpecialPointer(next_page);
			next_spc->page_type = ROARING_PAGE_PAYLOAD;
			next_spc->flags = 0;
			next_spc->entry_count = 1;
			next_spc->next_page = InvalidBlockNumber;

			next_entries = (RoaringPayloadEntry *) PageGetContents(next_page);
			next_entries[0].chunk_offset = chunk_offset;
			next_entries[0].pk = pk;

			((PageHeader) next_page)->pd_lower =
				(LocationIndex)(SizeOfPageHeaderData + sizeof(RoaringPayloadEntry));

			GenericXLogFinish(state);
			UnlockReleaseBuffer(next_buf);
			UnlockReleaseBuffer(buf);
			return;
		}
	}
}

/*
 * roaring_payload_fetch
 *
 * Retrieve the int64 PK stored at linear_tid.  Returns 0 if the slot
 * has never been written (deleted row reused before payload was populated).
 * Called only for TIDs that passed the bitmap's MVCC gating, so a return
 * value of 0 indicates a gap caused by a deleted+reused TID — the executor
 * will heap-fetch via xs_recheck in that case.
 */
int64
roaring_payload_fetch(Relation index, uint64 linear_tid)
{
	uint32				  payload_idx    = (uint32)(linear_tid / ROARING_PAYLOAD_PK_PER_PAGE);
	uint16				  chunk_offset   = (uint16)(linear_tid % ROARING_PAYLOAD_PK_PER_PAGE);
	uint32				  dir_root_idx   = payload_idx / ROARING_PAYLOAD_DIR_ENTRIES_PER_PAGE;
	uint32				  dir_leaf_idx   = payload_idx % ROARING_PAYLOAD_DIR_ENTRIES_PER_PAGE;
	Buffer				  buf;
	Page				  page;
	BlockNumber			 *entries_blkno;
	BlockNumber			  root_blkno;
	BlockNumber			  leaf_blkno;
	BlockNumber			  payload_blkno;
	int64				  pk = 0;
	RoaringPayloadDirSpecial *dir_spc;
	RoaringPayloadSpecial *pay_spc;
	RoaringPayloadEntry	 *pay_entries;

	/* Read metapage for root dir pointer */
	buf      = ReadBuffer(index, ROARING_METAPAGE_BLKNO);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	root_blkno = RoaringPageGetMeta(BufferGetPage(buf))->payload_dir_head;
	UnlockReleaseBuffer(buf);

	if (root_blkno == InvalidBlockNumber)
		return 0;

	/* Root dir */
	buf          = ReadBuffer(index, root_blkno);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page         = BufferGetPage(buf);
	entries_blkno = (BlockNumber *) PageGetContents(page);
	dir_spc      = (RoaringPayloadDirSpecial *) PageGetSpecialPointer(page);

	if (dir_root_idx >= dir_spc->entry_count)
	{
		UnlockReleaseBuffer(buf);
		return 0;
	}
	leaf_blkno = entries_blkno[dir_root_idx];
	UnlockReleaseBuffer(buf);

	if (leaf_blkno == InvalidBlockNumber)
		return 0;

	/* Leaf dir */
	buf          = ReadBuffer(index, leaf_blkno);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page         = BufferGetPage(buf);
	entries_blkno = (BlockNumber *) PageGetContents(page);
	dir_spc      = (RoaringPayloadDirSpecial *) PageGetSpecialPointer(page);

	if (dir_leaf_idx >= dir_spc->entry_count)
	{
		UnlockReleaseBuffer(buf);
		return 0;
	}
	payload_blkno = entries_blkno[dir_leaf_idx];
	UnlockReleaseBuffer(buf);

	if (payload_blkno == InvalidBlockNumber)
		return 0;

	/* Payload page overflow chain */
	while (payload_blkno != InvalidBlockNumber)
	{
		int low, high, mid;

		buf         = ReadBuffer(index, payload_blkno);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page        = BufferGetPage(buf);
		pay_entries = (RoaringPayloadEntry *) PageGetContents(page);
		pay_spc     = (RoaringPayloadSpecial *) PageGetSpecialPointer(page);

		/* Binary search */
		low = 0;
		high = pay_spc->entry_count - 1;
		while (low <= high)
		{
			mid = low + (high - low) / 2;
			if (pay_entries[mid].chunk_offset == chunk_offset)
			{
				pk = pay_entries[mid].pk;
				UnlockReleaseBuffer(buf);
				return pk;
			}
			else if (pay_entries[mid].chunk_offset < chunk_offset)
				low = mid + 1;
			else
				high = mid - 1;
		}

		payload_blkno = pay_spc->next_page;
		UnlockReleaseBuffer(buf);
	}

	return 0;
}

/* ----------------------------------------------------------------
 * Streaming payload cursor
 *
 * roaring_payload_fetch() re-reads the metapage, both directory levels and
 * re-walks the payload chain on every call.  Under an IndexOnlyScan the TIDs
 * arrive in ascending order, so (payload_idx, chunk_offset) is monotonically
 * non-decreasing.  The cursor caches each resolved level and a local copy of
 * the current chain page, advancing forward only — so every dir/payload page
 * is read once per scan rather than once per tuple.
 * ---------------------------------------------------------------- */

/* Copy one payload chain page into the cursor's local buffer. */
static void
payload_cursor_load_page(RoaringScanOpaque *so, Relation index,
						 BlockNumber blkno)
{
	Buffer					buf  = ReadBuffer(index, blkno);
	Page					page;
	RoaringPayloadSpecial  *spc;
	RoaringPayloadEntry	   *ents;
	int						count;

	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	spc  = (RoaringPayloadSpecial *) PageGetSpecialPointer(page);
	ents = (RoaringPayloadEntry *) PageGetContents(page);

	count = spc->entry_count;
	if (count > ROARING_PAYLOAD_MAX_ENTRIES)	/* defensive against corruption */
		count = ROARING_PAYLOAD_MAX_ENTRIES;

	if (so->pay_page_entries == NULL)
		so->pay_page_entries = (RoaringPayloadEntry *)
			MemoryContextAlloc(GetMemoryChunkContext(so),
							   ROARING_PAYLOAD_MAX_ENTRIES * sizeof(RoaringPayloadEntry));

	if (count > 0)
		memcpy(so->pay_page_entries, ents,
			   count * sizeof(RoaringPayloadEntry));

	so->pay_page_blkno  = blkno;
	so->pay_page_next   = spc->next_page;
	so->pay_page_count  = count;
	so->pay_page_maxoff = (count > 0) ? so->pay_page_entries[count - 1].chunk_offset : 0;

	UnlockReleaseBuffer(buf);
}

int64
roaring_payload_fetch_scan(IndexScanDesc scan, uint64 linear_tid)
{
	RoaringScanOpaque *so		   = (RoaringScanOpaque *) scan->opaque;
	Relation		   index	   = scan->indexRelation;
	uint32			   payload_idx  = (uint32)(linear_tid / ROARING_PAYLOAD_PK_PER_PAGE);
	uint16			   chunk_offset = (uint16)(linear_tid % ROARING_PAYLOAD_PK_PER_PAGE);
	uint32			   dir_root_idx = payload_idx / ROARING_PAYLOAD_DIR_ENTRIES_PER_PAGE;
	uint32			   dir_leaf_idx = payload_idx % ROARING_PAYLOAD_DIR_ENTRIES_PER_PAGE;
	int				   lo, hi;

	/* 1. Resolve the payload root dir once per scan. */
	if (!so->pay_init)
	{
		Buffer b = ReadBuffer(index, ROARING_METAPAGE_BLKNO);

		LockBuffer(b, BUFFER_LOCK_SHARE);
		so->pay_root_dir = RoaringPageGetMeta(BufferGetPage(b))->payload_dir_head;
		UnlockReleaseBuffer(b);
		so->pay_init = true;
	}
	if (so->pay_root_dir == InvalidBlockNumber)
		return 0;

	/* 2. Resolve the leaf dir for dir_root_idx (cached across calls). */
	if (!so->pay_dir_valid || dir_root_idx != so->pay_dir_root_idx)
	{
		Buffer					 b = ReadBuffer(index, so->pay_root_dir);
		Page					 p;
		RoaringPayloadDirSpecial *ds;
		BlockNumber				*ents;

		LockBuffer(b, BUFFER_LOCK_SHARE);
		p	 = BufferGetPage(b);
		ents = (BlockNumber *) PageGetContents(p);
		ds	 = (RoaringPayloadDirSpecial *) PageGetSpecialPointer(p);
		so->pay_leaf_dir = (dir_root_idx < ds->entry_count)
			? ents[dir_root_idx] : InvalidBlockNumber;
		UnlockReleaseBuffer(b);

		so->pay_dir_root_idx = dir_root_idx;
		so->pay_dir_valid	 = true;
		so->pay_chain_valid	 = false;	/* leaf dir changed → re-resolve chain */
	}
	if (so->pay_leaf_dir == InvalidBlockNumber)
		return 0;

	/* 3. Resolve the chain head for payload_idx (cached across calls). */
	if (!so->pay_chain_valid || payload_idx != so->pay_payload_idx)
	{
		Buffer					 b = ReadBuffer(index, so->pay_leaf_dir);
		Page					 p;
		RoaringPayloadDirSpecial *ds;
		BlockNumber				*ents;

		LockBuffer(b, BUFFER_LOCK_SHARE);
		p	 = BufferGetPage(b);
		ents = (BlockNumber *) PageGetContents(p);
		ds	 = (RoaringPayloadDirSpecial *) PageGetSpecialPointer(p);
		so->pay_chain_head = (dir_leaf_idx < ds->entry_count)
			? ents[dir_leaf_idx] : InvalidBlockNumber;
		UnlockReleaseBuffer(b);

		so->pay_payload_idx = payload_idx;
		so->pay_chain_valid = true;
		so->pay_page_blkno	= InvalidBlockNumber;	/* force reload from chain head */
	}
	if (so->pay_chain_head == InvalidBlockNumber)
		return 0;

	/* 4. Position on the chain page covering chunk_offset (forward-only). */
	if (so->pay_page_blkno == InvalidBlockNumber)
		payload_cursor_load_page(so, index, so->pay_chain_head);

	while (chunk_offset > so->pay_page_maxoff &&
		   so->pay_page_next != InvalidBlockNumber)
		payload_cursor_load_page(so, index, so->pay_page_next);

	/* 5. Binary search the cached page for chunk_offset. */
	lo = 0;
	hi = so->pay_page_count - 1;
	while (lo <= hi)
	{
		int mid = lo + (hi - lo) / 2;
		uint16 off = so->pay_page_entries[mid].chunk_offset;

		if (off == chunk_offset)
			return so->pay_page_entries[mid].pk;
		else if (off < chunk_offset)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return 0;	/* slot never written (deleted+reused TID) */
}

/* Reset the cursor so the next fetch re-resolves from the metapage.  Called
 * from amrescan; the page-copy buffer is retained for reuse. */
void
roaring_payload_cursor_reset(RoaringScanOpaque *so)
{
	so->pay_init		 = false;
	so->pay_dir_valid	 = false;
	so->pay_chain_valid	 = false;
	so->pay_page_blkno	 = InvalidBlockNumber;
}

/* Free the cursor's page-copy buffer.  Called from amendscan. */
void
roaring_payload_cursor_release(RoaringScanOpaque *so)
{
	if (so->pay_page_entries != NULL)
	{
		pfree(so->pay_page_entries);
		so->pay_page_entries = NULL;
	}
}
