#include "pg_roaring_index.h"

#include "access/generic_xlog.h"
#include "access/transam.h"
#include "access/xact.h"
#include "storage/bufmgr.h"
#include "utils/rel.h"

/* ----------------------------------------------------------------
 * roaring_pending_append
 *
 * Append one entry to the pending insert list.
 *
 * Locking protocol:
 *   1. Lock metapage EXCLUSIVE — held for the entire operation.
 *      This serialises concurrent inserts and merges and eliminates
 *      any TOCTOU races on the tail pointer.
 *   2. While holding metapage: lock tail page EXCLUSIVE.
 *   3. If tail full: extend new page and link it in one WAL record.
 *   4. Write entry to tail page and bump meta count in one WAL record.
 *   5. Release tail, release metapage.
 *
 * pd_lower note: our metapage and pending pages store data starting at
 * SizeOfPageHeaderData but do not advance pd_lower via PageAddItem.
 * We set pd_lower explicitly so that GenericXLog's mask_unused_space
 * does not zero the data region when computing WAL deltas.
 * ---------------------------------------------------------------- */
static void
roaring_pending_append(Relation index, RoaringPendingEntry *newentry)
{
	Buffer				 metabuf;
	Page				 metapage;
	RoaringMetaPageData *meta;
	BlockNumber			 tail_blkno;
	Buffer				 tailbuf;
	Page				 tailimg;
	RoaringPendingSpecial *tailspc;
	GenericXLogState	*state;

retry:
	/* Step 1: lock metapage exclusively — held throughout. */
	metabuf  = ReadBuffer(index, ROARING_METAPAGE_BLKNO);
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
	metapage = BufferGetPage(metabuf);
	meta	 = RoaringPageGetMeta(metapage);
	tail_blkno = meta->pending_insert_tail;

	if (meta->pending_insert_count >= meta->pending_merge_threshold)
	{
		/* Release metapage before merge (merge also acquires it). */
		UnlockReleaseBuffer(metabuf);
		roaring_merge_pending(index);
		goto retry;
	}

	/* Step 2: lock tail page while metapage is still held exclusively. */
	tailbuf = ReadBuffer(index, tail_blkno);
	LockBuffer(tailbuf, BUFFER_LOCK_EXCLUSIVE);
	tailspc = (RoaringPendingSpecial *)
			  PageGetSpecialPointer(BufferGetPage(tailbuf));

	if (tailspc->entry_count >= ROARING_PENDING_PER_PAGE)
	{
		/* Step 3: tail full — extend new page and link in one WAL record. */
		Buffer				  newbuf;
		Page				  newpage;
		Page				  old_img, new_img, meta_img;
		RoaringPendingSpecial *newspc;
		BlockNumber			  newblkno;

		newbuf   = roaring_extend_page(index);	/* EB_LOCK_FIRST */
		newblkno = BufferGetBlockNumber(newbuf);
		newpage  = BufferGetPage(newbuf);
		PageInit(newpage, BLCKSZ, sizeof(RoaringPendingSpecial));
		newspc				= (RoaringPendingSpecial *) PageGetSpecialPointer(newpage);
		newspc->page_type	= ROARING_PAGE_PENDING_INSERT;
		newspc->flags		= 0;
		newspc->entry_count	= 1;
		newspc->next_page	= InvalidBlockNumber;
		newspc->xmin_low	= newentry->xmin;

		/* Write the new entry directly into the new page's data area. */
		{
			RoaringPendingEntry *slot =
				(RoaringPendingEntry *) PageGetContents(newpage);
			*slot = *newentry;
			((PageHeader) newpage)->pd_lower =
				(LocationIndex)(SizeOfPageHeaderData +
								sizeof(RoaringPendingEntry));
		}

		state   = GenericXLogStart(index);

		old_img = GenericXLogRegisterBuffer(state, tailbuf, 0);
		((RoaringPendingSpecial *)
		 PageGetSpecialPointer(old_img))->next_page = newblkno;

		new_img = GenericXLogRegisterBuffer(state, newbuf,
											GENERIC_XLOG_FULL_IMAGE);
		memcpy(new_img, newpage, BLCKSZ);

		meta_img = GenericXLogRegisterBuffer(state, metabuf, 0);
		meta	 = RoaringPageGetMeta(meta_img);
		meta->pending_insert_tail  = newblkno;
		meta->pending_insert_count++;

		GenericXLogFinish(state);

		UnlockReleaseBuffer(newbuf);
		UnlockReleaseBuffer(tailbuf);
		UnlockReleaseBuffer(metabuf);
		return;
	}

	/* Step 4: append entry to existing tail page and bump meta count. */
	{
		Page				 meta_img;
		RoaringPendingEntry *slot;

		state	 = GenericXLogStart(index);

		tailimg  = GenericXLogRegisterBuffer(state, tailbuf, 0);
		tailspc  = (RoaringPendingSpecial *) PageGetSpecialPointer(tailimg);
		slot	 = (RoaringPendingEntry *) PageGetContents(tailimg)
				   + tailspc->entry_count;
		*slot = *newentry;
		if (!TransactionIdIsValid(tailspc->xmin_low) ||
			TransactionIdPrecedes(newentry->xmin, tailspc->xmin_low))
			tailspc->xmin_low = newentry->xmin;
		tailspc->entry_count++;
		((PageHeader) tailimg)->pd_lower += sizeof(RoaringPendingEntry);

		meta_img = GenericXLogRegisterBuffer(state, metabuf, 0);
		RoaringPageGetMeta(meta_img)->pending_insert_count++;

		GenericXLogFinish(state);
	}

	UnlockReleaseBuffer(tailbuf);
	UnlockReleaseBuffer(metabuf);
}

/* ----------------------------------------------------------------
 * roaring_insert
 * ---------------------------------------------------------------- */
bool
roaring_insert(Relation index, Datum *values, bool *isnull,
			   ItemPointer ht_ctid, Relation heapRel,
			   IndexUniqueCheck checkUnique,
			   bool indexUnchanged,
			   struct IndexInfo *indexInfo)
{
	RoaringPendingEntry entry;

	if (isnull[0])
		return false;

	entry.value      = DatumGetInt64(values[0]);
	entry.linear_tid = ((uint64) ItemPointerGetBlockNumber(ht_ctid) << 16) |
					   (uint64) ItemPointerGetOffsetNumber(ht_ctid);
	entry.xmin       = GetCurrentTransactionId();
	entry.reserved   = 0;

	roaring_pending_append(index, &entry);
	return false;	/* this AM does not support unique indexes */
}
