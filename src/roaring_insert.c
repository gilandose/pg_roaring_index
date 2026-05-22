#include "pg_roaring_index.h"

#include <math.h>

#include "access/generic_xlog.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/pg_type_d.h"
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
	bool				 merged_unproductively = false;

retry:
	/* Step 1: lock metapage exclusively — held throughout. */
	metabuf  = ReadBuffer(index, ROARING_METAPAGE_BLKNO);
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
	metapage = BufferGetPage(metabuf);
	meta	 = RoaringPageGetMeta(metapage);
	roaring_validate_metapage(index, meta);
	tail_blkno = meta->pending_insert_tail;

	if (!merged_unproductively &&
		meta->pending_insert_count >= meta->pending_merge_threshold)
	{
		uint32 count_before = meta->pending_insert_count;

		UnlockReleaseBuffer(metabuf);
		roaring_merge_pending(index);

		/*
		 * Re-read to detect an unproductive merge (all pending entries
		 * belonged to in-progress transactions; none were flushed).  If so,
		 * skip the threshold check on the next attempt to avoid an infinite
		 * retry loop.
		 */
		{
			Buffer tmp = ReadBuffer(index, ROARING_METAPAGE_BLKNO);

			LockBuffer(tmp, BUFFER_LOCK_SHARE);
			if (RoaringPageGetMeta(BufferGetPage(tmp))->pending_insert_count
				>= count_before)
				merged_unproductively = true;
			UnlockReleaseBuffer(tmp);
		}
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
		newspc->_pad		= 0;
		newspc->value_min	= newentry->value;
		newspc->value_max	= newentry->value;

		/* Write the new entry directly into the new page's data area. */
		{
			RoaringPendingEntry *slot =
				(RoaringPendingEntry *) PageGetContents(newpage);
			*slot = *newentry;
			((PageHeader) newpage)->pd_lower =
				(LocationIndex)(SizeOfPageHeaderData +
								sizeof(RoaringPendingEntry));
		}

		{
		GenericXLogState * volatile xstate = GenericXLogStart(index);
		PG_TRY();
		{
			old_img = GenericXLogRegisterBuffer((GenericXLogState *) xstate,
												tailbuf, 0);
			((RoaringPendingSpecial *)
			 PageGetSpecialPointer(old_img))->next_page = newblkno;

			new_img = GenericXLogRegisterBuffer((GenericXLogState *) xstate,
												newbuf, GENERIC_XLOG_FULL_IMAGE);
			memcpy(new_img, newpage, BLCKSZ);

			meta_img = GenericXLogRegisterBuffer((GenericXLogState *) xstate,
												 metabuf, 0);
			meta	 = RoaringPageGetMeta(meta_img);
			meta->pending_insert_tail  = newblkno;
			meta->pending_insert_count++;

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

		UnlockReleaseBuffer(newbuf);
		UnlockReleaseBuffer(tailbuf);
		UnlockReleaseBuffer(metabuf);
		return;
	}

	/* Step 4: append entry to existing tail page and bump meta count. */
	{
		GenericXLogState * volatile xstate = GenericXLogStart(index);
		Page				 meta_img;
		RoaringPendingEntry *slot;

		PG_TRY();
		{
			tailimg  = GenericXLogRegisterBuffer((GenericXLogState *) xstate,
												 tailbuf, 0);
			tailspc  = (RoaringPendingSpecial *) PageGetSpecialPointer(tailimg);
			slot	 = (RoaringPendingEntry *) PageGetContents(tailimg)
					   + tailspc->entry_count;
			*slot = *newentry;
			if (!TransactionIdIsValid(tailspc->xmin_low) ||
				TransactionIdPrecedes(newentry->xmin, tailspc->xmin_low))
				tailspc->xmin_low = newentry->xmin;
			if (newentry->value < tailspc->value_min)
				tailspc->value_min = newentry->value;
			if (newentry->value > tailspc->value_max)
				tailspc->value_max = newentry->value;
			tailspc->entry_count++;
			((PageHeader) tailimg)->pd_lower += sizeof(RoaringPendingEntry);

			meta_img = GenericXLogRegisterBuffer((GenericXLogState *) xstate,
												 metabuf, 0);
			RoaringPageGetMeta(meta_img)->pending_insert_count++;

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

	/* HOT update: indexed column unchanged, no new index entry needed. */
	if (indexUnchanged)
		return false;

	if (index->rd_att->natts > 1)
	{
		/* Multi-column: one pending entry per non-null column. */
		uint32		linear_tid = ((uint32) ItemPointerGetBlockNumber(ht_ctid) << 9) |
							   (uint32)(ItemPointerGetOffsetNumber(ht_ctid) - 1);
		TransactionId xmin = GetCurrentTransactionId();
		int			  i;

		for (i = 0; i < index->rd_att->natts; i++)
		{
			Oid typid = TupleDescAttr(index->rd_att, i)->atttypid;

			if (isnull[i])
				continue;
			if (typid == FLOAT4OID && isnan(DatumGetFloat4(values[i])))
				continue;
			entry.value      = ROARING_COL_KEY(i + 1,
											   roaring_datum_to_key32(values[i], typid));
			entry.linear_tid = linear_tid;
			entry.xmin       = xmin;
			roaring_pending_append(index, &entry);
		}
		return false;
	}

	{
		Oid typid = TupleDescAttr(index->rd_att, 0)->atttypid;

		if (typid == FLOAT4OID && isnan(DatumGetFloat4(values[0])))
			return false;
		entry.value = roaring_datum_to_key64(values[0], typid);
	}
	entry.linear_tid = ((uint32) ItemPointerGetBlockNumber(ht_ctid) << 9) |
					   (uint32)(ItemPointerGetOffsetNumber(ht_ctid) - 1);
	entry.xmin       = GetCurrentTransactionId();

	roaring_pending_append(index, &entry);
	return false;	/* this AM does not support unique indexes */
}

/* ----------------------------------------------------------------
 * roaring_insert_lossy
 *
 * Lossy variant: stores the heap block number in linear_tid instead of
 * the (blkno<<9|offset-1) linearization.  The pending list, metapage,
 * and locking protocol are identical to the exact path.
 * ---------------------------------------------------------------- */
bool
roaring_insert_lossy(Relation index, Datum *values, bool *isnull,
					 ItemPointer ht_ctid, Relation heapRel,
					 IndexUniqueCheck checkUnique,
					 bool indexUnchanged,
					 struct IndexInfo *indexInfo)
{
	RoaringPendingEntry entry;

	if (isnull[0])
		return false;

	if (indexUnchanged)
		return false;

	if (index->rd_att->natts > 1)
	{
		/* Multi-column lossy: one pending entry per non-null column. */
		uint32		  blkno = (uint32) ItemPointerGetBlockNumber(ht_ctid);
		TransactionId xmin  = GetCurrentTransactionId();
		int			  i;

		for (i = 0; i < index->rd_att->natts; i++)
		{
			Oid typid = TupleDescAttr(index->rd_att, i)->atttypid;

			if (isnull[i])
				continue;
			if (typid == FLOAT4OID && isnan(DatumGetFloat4(values[i])))
				continue;
			entry.value      = ROARING_COL_KEY(i + 1,
											   roaring_datum_to_key32(values[i], typid));
			entry.linear_tid = blkno;
			entry.xmin       = xmin;
			roaring_pending_append(index, &entry);
		}
		return false;
	}

	{
		Oid typid = TupleDescAttr(index->rd_att, 0)->atttypid;

		if (typid == FLOAT4OID && isnan(DatumGetFloat4(values[0])))
			return false;
		entry.value = roaring_datum_to_key64(values[0], typid);
	}
	entry.linear_tid = (uint32) ItemPointerGetBlockNumber(ht_ctid);
	entry.xmin       = GetCurrentTransactionId();

	roaring_pending_append(index, &entry);
	return false;
}
