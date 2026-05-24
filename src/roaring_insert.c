#include "pg_roaring_index.h"

#include <math.h>

#include "access/generic_xlog.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/pg_type_d.h"
#include "postmaster/bgworker.h"
#include "storage/bufmgr.h"
#include "utils/rel.h"

/* ----------------------------------------------------------------
 * roaring_pending_append
 *
 * Append one entry to the pending insert list for the shard determined
 * by ROARING_VALUE_SHARD(newentry->value).
 *
 * Locking protocol (two-phase):
 *
 *   Hot path (tail has space):
 *     1. Lock metapage SHARE: read shards[shard].insert_tail + total count.
 *     2. Release metapage SHARE.
 *     3. Lock tail page EXCLUSIVE.
 *     4. If tail has space: write entry, WAL covers tail page only.
 *        insert_count is NOT updated on the hot path — only on extension.
 *        Max undercount per shard: ROARING_PENDING_PER_PAGE entries.
 *     5. Release tail.
 *
 *   Extension path (tail full after step 3):
 *     4b. Release tail EXCLUSIVE.
 *     4c. Lock metapage EXCLUSIVE; re-read shards[shard].insert_tail.
 *         If changed (another writer extended first), release and goto retry.
 *     4d. Re-acquire old tail EXCLUSIVE under metapage EX
 *         (lock order: blk 0 before tail_blkno).
 *     4e. Allocate new page, write entry there.
 *         Fused WAL: old_tail.next_page + new page (FULL_IMAGE) +
 *         metapage shard update (insert_tail = newblkno,
 *                                insert_count += ROARING_PENDING_PER_PAGE).
 *     5. Release new page, old tail, metapage.
 *
 * Back-pressure: total = sum(shards[i].insert_count) under SHARE.
 * Counts are updated only on page extension, so max undercount is
 * ROARING_PENDING_SHARDS * ROARING_PENDING_PER_PAGE = 4064 entries.
 *
 * pd_lower note: pending pages store data starting at SizeOfPageHeaderData
 * but do not advance pd_lower via PageAddItem.  We set pd_lower explicitly
 * so that GenericXLog's mask_unused_space does not zero the data region.
 * ---------------------------------------------------------------- */
static void
roaring_pending_append(Relation index, RoaringPendingEntry *newentry)
{
	int					 shard_id;
	BlockNumber			 tail_blkno;
	Buffer				 metabuf;
	RoaringMetaPageData *meta;
	Buffer				 tailbuf;
	RoaringPendingSpecial *tailspc;
	bool				 merged_unproductively = false;

	shard_id = ROARING_VALUE_SHARD(newentry->value);

retry:
	/* Phase 1: read metapage under SHARE to get tail blkno and back-pressure. */
	metabuf = ReadBuffer(index, ROARING_METAPAGE_BLKNO);
	LockBuffer(metabuf, BUFFER_LOCK_SHARE);
	meta = RoaringPageGetMeta(BufferGetPage(metabuf));
	roaring_validate_metapage(index, meta);
	tail_blkno = meta->shards[shard_id].insert_tail;

	if (!merged_unproductively)
	{
		uint32 total = 0;
		int    i;

		for (i = 0; i < ROARING_PENDING_SHARDS; i++)
			total += meta->shards[i].insert_count;

		if (total >= meta->pending_merge_threshold)
		{
			bool bgworker_running = (meta->flags & ROARING_FLAG_BGMERGE_SPAWNED) != 0;
			bool over_hard_cap    = (total >= 2 * meta->pending_merge_threshold);

			if (!bgworker_running || over_hard_cap)
			{
				/*
				 * No background worker running (flag not set), OR pending list
				 * has grown past the hard cap (2× threshold) — fall back to
				 * synchronous merge to bound list growth.
				 */
				uint32 count_before = total;

				UnlockReleaseBuffer(metabuf);
				roaring_merge_pending(index);

				/*
				 * Re-read to detect an unproductive merge (all pending entries
				 * belong to in-progress transactions).  If so, skip the
				 * threshold check on the next pass to avoid an infinite retry.
				 */
				{
					Buffer tmp;
					uint32 total_after = 0;
					int    j;

					tmp = ReadBuffer(index, ROARING_METAPAGE_BLKNO);
					LockBuffer(tmp, BUFFER_LOCK_SHARE);
					meta = RoaringPageGetMeta(BufferGetPage(tmp));
					for (j = 0; j < ROARING_PENDING_SHARDS; j++)
						total_after += meta->shards[j].insert_count;
					UnlockReleaseBuffer(tmp);

					if (total_after >= count_before)
						merged_unproductively = true;
				}
				goto retry;
			}
			/*
			 * Background worker is running (flag set) and below the hard cap:
			 * let the worker drain the list asynchronously.  Continue the
			 * insert normally — the tail read below is still valid.
			 */
		}
	}

	/* Phase 2: release metapage SHARE, then lock shard tail EX. */
	UnlockReleaseBuffer(metabuf);

	tailbuf = ReadBuffer(index, tail_blkno);
	LockBuffer(tailbuf, BUFFER_LOCK_EXCLUSIVE);
	tailspc = (RoaringPendingSpecial *)
			  PageGetSpecialPointer(BufferGetPage(tailbuf));

	if (tailspc->entry_count < ROARING_PENDING_PER_PAGE)
	{
		/* Hot path: write entry, WAL covers tail page only (no metapage). */
		GenericXLogState * volatile xstate = GenericXLogStart(index);
		Page				 tailimg;
		RoaringPendingEntry *slot;

		PG_TRY();
		{
			tailimg = GenericXLogRegisterBuffer((GenericXLogState *) xstate,
												tailbuf, 0);
			tailspc = (RoaringPendingSpecial *) PageGetSpecialPointer(tailimg);
			slot = (RoaringPendingEntry *) PageGetContents(tailimg)
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

		UnlockReleaseBuffer(tailbuf);
		return;
	}

	/* Extension: tail full. Release tail, acquire metapage EX. */
	UnlockReleaseBuffer(tailbuf);

	metabuf = ReadBuffer(index, ROARING_METAPAGE_BLKNO);
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
	meta = RoaringPageGetMeta(BufferGetPage(metabuf));

	if (meta->shards[shard_id].insert_tail != tail_blkno)
	{
		/* Another writer extended this shard first; retry with updated tail. */
		UnlockReleaseBuffer(metabuf);
		goto retry;
	}

	/* Re-acquire old tail under metapage EX (lock order: blk0 < tail_blkno). */
	tailbuf = ReadBuffer(index, tail_blkno);
	LockBuffer(tailbuf, BUFFER_LOCK_EXCLUSIVE);

	{
		Buffer				  newbuf;
		Page				  newpage;
		Page				  old_img, new_img, meta_img;
		RoaringPendingSpecial *newspc;
		BlockNumber			  newblkno;
		BlockNumber			  new_free_list_head;
		bool				  spawn_worker = false;

		newbuf   = roaring_alloc_page(index, metabuf, &new_free_list_head);
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
			meta = RoaringPageGetMeta(meta_img);
			meta->shards[shard_id].insert_tail  = newblkno;
			meta->shards[shard_id].insert_count += ROARING_PENDING_PER_PAGE;
			if (new_free_list_head != meta->free_list_head)
				meta->free_list_head = new_free_list_head;

			/*
			 * If the total pending count (after updating this shard) now
			 * crosses the merge threshold AND no bgworker flag is set yet,
			 * set the flag and arrange to spawn a worker after releasing locks.
			 * The flag and the insert_count update are in the same WAL record.
			 */
			{
				uint32 total = 0;
				int    k;

				for (k = 0; k < ROARING_PENDING_SHARDS; k++)
					total += meta->shards[k].insert_count;

				if (total >= meta->pending_merge_threshold &&
					!(meta->flags & ROARING_FLAG_BGMERGE_SPAWNED))
				{
					meta->flags |= ROARING_FLAG_BGMERGE_SPAWNED;
					spawn_worker = true;
				}
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
		}

		UnlockReleaseBuffer(newbuf);
		UnlockReleaseBuffer(tailbuf);
		UnlockReleaseBuffer(metabuf);

		/*
		 * Spawn the background merge worker AFTER releasing all locks.
		 * If the spawn fails (max_worker_processes exhausted), the hard-cap
		 * check in the soft-threshold path will trigger a sync merge when
		 * pending grows past 2 × pending_merge_threshold.
		 */
		if (spawn_worker)
			roaring_try_spawn_merge_worker(index);
	}
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
		uint64		linear_tid = ((uint64) ItemPointerGetBlockNumber(ht_ctid) << 9) |
							   (uint64)(ItemPointerGetOffsetNumber(ht_ctid) - 1);
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
	entry.linear_tid = ((uint64) ItemPointerGetBlockNumber(ht_ctid) << 9) |
					   (uint64)(ItemPointerGetOffsetNumber(ht_ctid) - 1);
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
