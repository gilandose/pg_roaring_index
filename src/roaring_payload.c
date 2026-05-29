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
#include "storage/bufmgr.h"
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
		pay_spc->entry_count = 0;

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
	uint32				  payload_offset = (uint32)(linear_tid % ROARING_PAYLOAD_PK_PER_PAGE);
	uint32				  dir_root_idx   = payload_idx / ROARING_PAYLOAD_DIR_ENTRIES_PER_PAGE;
	uint32				  dir_leaf_idx   = payload_idx % ROARING_PAYLOAD_DIR_ENTRIES_PER_PAGE;
	BlockNumber			  root_blkno;
	BlockNumber			  leaf_blkno;
	BlockNumber			  payload_blkno;
	GenericXLogState	 *state;
	Buffer				  buf;
	Page				  page;
	int64				 *entries;
	RoaringPayloadSpecial *spc;
	uint32				  i;

	root_blkno    = payload_get_root_dir(index);
	leaf_blkno    = payload_get_leaf_dir(index, root_blkno, dir_root_idx);
	payload_blkno = payload_get_page(index, leaf_blkno, dir_leaf_idx);

	state   = GenericXLogStart(index);
	buf     = ReadBuffer(index, payload_blkno);
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	page    = GenericXLogRegisterBuffer(state, buf, 0);
	entries = (int64 *) PageGetContents(page);
	spc     = (RoaringPayloadSpecial *) PageGetSpecialPointer(page);

	entries[payload_offset] = pk;

	if (payload_offset >= spc->entry_count)
	{
		/* Fill any gap slots with 0 (never-inserted / deleted slots). */
		for (i = spc->entry_count; i < payload_offset; i++)
			entries[i] = 0;
		spc->entry_count = payload_offset + 1;
	}

	((PageHeader) page)->pd_lower =
		(LocationIndex)(SizeOfPageHeaderData +
						spc->entry_count * sizeof(int64));

	GenericXLogFinish(state);
	UnlockReleaseBuffer(buf);
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
	uint32				  payload_offset = (uint32)(linear_tid % ROARING_PAYLOAD_PK_PER_PAGE);
	uint32				  dir_root_idx   = payload_idx / ROARING_PAYLOAD_DIR_ENTRIES_PER_PAGE;
	uint32				  dir_leaf_idx   = payload_idx % ROARING_PAYLOAD_DIR_ENTRIES_PER_PAGE;
	Buffer				  buf;
	Page				  page;
	BlockNumber			 *entries_blkno;
	BlockNumber			  root_blkno;
	BlockNumber			  leaf_blkno;
	BlockNumber			  payload_blkno;
	int64				  pk;
	RoaringPayloadDirSpecial *dir_spc;
	RoaringPayloadSpecial *pay_spc;
	int64				 *pay_entries;

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

	/* Payload page */
	buf         = ReadBuffer(index, payload_blkno);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page        = BufferGetPage(buf);
	pay_entries = (int64 *) PageGetContents(page);
	pay_spc     = (RoaringPayloadSpecial *) PageGetSpecialPointer(page);

	pk = (payload_offset < pay_spc->entry_count) ? pay_entries[payload_offset] : 0;
	UnlockReleaseBuffer(buf);

	return pk;
}
