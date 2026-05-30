#include "pg_roaring_index.h"

#include <math.h>

#include "access/htup_details.h"
#include "access/tableam.h"
#include "access/xloginsert.h"
#include "catalog/pg_operator_d.h"
#include "catalog/pg_type_d.h"
#include "commands/progress.h"
#include "executor/tuptable.h"
#include "pgstat.h"
#include "storage/checksum.h"
#include "storage/smgr.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/tuplesort.h"

extern int maintenance_work_mem;

/* ----------------------------------------------------------------
 * Internal types
 * ---------------------------------------------------------------- */

/*
 * (value, linearized_tid) pairs are fed into a Tuplesortstate sorted by
 * (value ASC, tid ASC) so value groups come back contiguous with each group's
 * tids in ascending order.  Tuplesort spills to temp files when input exceeds
 * maintenance_work_mem, so the build is no longer bounded by RAM (T46/T61).
 *
 * Each entry is a synthetic two-column heap tuple (int8 value, int8 tid).
 * tid is ≤ 41 bits ((blkno<<9)|offset), always positive in an int8, so signed
 * int8 comparison matches the desired unsigned tid order.
 */
typedef struct RoaringBuildState
{
	double				heap_tuples;
	Oid					atttypid;   /* column 0 type: used only for single-column indexes */
	long				ntuples;    /* count emitted into the sort (progress only) */
	TupleDesc			tupdesc;    /* (int8, int8) descriptor for sort tuples */
	TupleTableSlot	   *slot;       /* virtual slot reused for each put */
	Tuplesortstate	   *sortstate;
} RoaringBuildState;

/*
 * Emit one (value, tid) pair into the sort via the reusable virtual slot.
 */
static inline void
build_emit(RoaringBuildState *bstate, int64 value, uint64 tid)
{
	TupleTableSlot *slot = bstate->slot;

	ExecClearTuple(slot);
	slot->tts_values[0] = Int64GetDatum(value);
	slot->tts_values[1] = Int64GetDatum((int64) tid);
	slot->tts_isnull[0] = false;
	slot->tts_isnull[1] = false;
	ExecStoreVirtualTuple(slot);

	tuplesort_puttupleslot(bstate->sortstate, slot);
	bstate->ntuples++;
}

/* ----------------------------------------------------------------
 * roaring_build_callback
 * ---------------------------------------------------------------- */
static void
roaring_build_callback(Relation index, ItemPointer tid, Datum *values,
					   bool *isnull, bool tupleIsAlive, void *state)
{
	RoaringBuildState  *bstate = (RoaringBuildState *) state;
	int					natts  = index->rd_att->natts;
	int					nkeys  = index->rd_index->indnkeyatts;

	bstate->heap_tuples++;

	if (nkeys > 1)
	{
		/* Multi-column key: emit one (attno-namespaced key, tid) entry per KEY column. */
		int    i;
		uint64 linear_tid =
			((uint64) ItemPointerGetBlockNumber(tid) << 9) |
			(uint64)(ItemPointerGetOffsetNumber(tid) - 1);

		for (i = 0; i < nkeys; i++)
		{
			Oid		typid = TupleDescAttr(index->rd_att, i)->atttypid;
			int64	value;

			if (isnull[i])
				continue;

			if (typid == FLOAT4OID && isnan(DatumGetFloat4(values[i])))
				continue;			/* NaN is not equality-indexable */

			value = ROARING_COL_KEY(i + 1, roaring_datum_to_key32(values[i], typid));

			build_emit(bstate, value, linear_tid);
		}

		/* T65: write INCLUDE column payload before the pending entry. */
		if (natts > nkeys && !isnull[nkeys])
			roaring_payload_insert(index, linear_tid, DatumGetInt64(values[nkeys]));

		return;
	}

	/* Single key column path (with optional INCLUDE columns). */
	{
		uint64 linear_tid =
			((uint64) ItemPointerGetBlockNumber(tid) << 9) |
			(uint64)(ItemPointerGetOffsetNumber(tid) - 1);
		int64 value;

		if (isnull[0])
			return;

		if (bstate->atttypid == FLOAT4OID && isnan(DatumGetFloat4(values[0])))
			return;					/* NaN is not equality-indexable */

		value = roaring_datum_to_key64(values[0], bstate->atttypid);

		build_emit(bstate, value, linear_tid);

		/* T65: write INCLUDE column payload before the pending entry. */
		if (natts > nkeys && !isnull[nkeys])
			roaring_payload_insert(index, linear_tid, DatumGetInt64(values[nkeys]));
	}
}

/* ----------------------------------------------------------------
 * write_metapage
 *
 * Rewrites block 0 (previously zeroed-out) with real metapage data.
 * ---------------------------------------------------------------- */
static void
write_metapage(Relation index,
			   BlockNumber root_dir,
			   BlockNumber leftmost_leaf,
			   BlockNumber rightmost_leaf,
			   BlockNumber pending_blknos[ROARING_PENDING_SHARDS],
			   uint32 total_entries,
			   uint16 flags)
{
	Buffer				buf;
	Page				page;
	RoaringMetaPageData *meta;
	int					 i;

	buf  = ReadBuffer(index, ROARING_METAPAGE_BLKNO);
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	page = BufferGetPage(buf);

	/*
	 * Save payload_dir_head before PageInit wipes the header.  The content
	 * area (where meta fields live) is untouched by PageInit, so reading it
	 * here captures whatever payload_get_root_dir wrote during the build scan.
	 */
	{
		RoaringMetaPageData *old_meta = RoaringPageGetMeta(page);
		BlockNumber saved_payload_dir = old_meta->payload_dir_head;

		PageInit(page, BLCKSZ, 0);
		meta = RoaringPageGetMeta(page);
		memset(meta, 0, sizeof(*meta));
		meta->payload_dir_head = saved_payload_dir;
	}

	meta->magic					   = ROARING_MAGIC;
	meta->version				   = ROARING_INDEX_VERSION;
	meta->flags					   = flags;
	meta->croaring_format_version  = ROARING_EXPECTED_FORMAT_VERSION;
	meta->num_shards			   = ROARING_PENDING_SHARDS;
	meta->root_directory_page	   = root_dir;
	meta->leftmost_leaf_page	   = leftmost_leaf;
	meta->rightmost_leaf_page	   = rightmost_leaf;
	meta->free_list_head		   = InvalidBlockNumber;

	for (i = 0; i < ROARING_PENDING_SHARDS; i++)
	{
		meta->shards[i].insert_head  = pending_blknos[i];
		meta->shards[i].insert_tail  = pending_blknos[i];
		meta->shards[i].insert_count = 0;
		meta->shards[i].merging_head = InvalidBlockNumber;
		meta->shards[i].carry_head   = InvalidBlockNumber;
	}

	meta->total_entries			 = total_entries;
	meta->pending_merge_threshold = (uint32) roaring_pending_merge_threshold_guc;

	/*
	 * Set pd_lower past the metapage data so GenericXLog's mask_unused_space
	 * does not zero our fields when computing WAL deltas.
	 */
	((PageHeader) page)->pd_lower =
		(LocationIndex)(SizeOfPageHeaderData + sizeof(RoaringMetaPageData));

	roaring_wal_and_release(index, buf);
}


/*
 * Leaf pages are WAL-logged in batches of ROARING_BUILD_WAL_BATCH rather
 * than one log_newpage_buffer per page.  One log_newpages record covers the
 * whole batch, giving ~32× WAL reduction on large builds.
 */
#define ROARING_BUILD_WAL_BATCH 32

static void
flush_leaf_wal_batch(Relation index, Buffer *bufs, BlockNumber *blknos, int n)
{
	int i;

	if (n == 0)
		return;

	if (RelationNeedsWAL(index))
	{
		Page *pages = (Page *) palloc(n * sizeof(Page));

		for (i = 0; i < n; i++)
			pages[i] = BufferGetPage(bufs[i]);
		log_newpages(&index->rd_locator, MAIN_FORKNUM, n, blknos, pages, true);
		pfree(pages);
	}

	for (i = 0; i < n; i++)
		UnlockReleaseBuffer(bufs[i]);
}

/*
 * LeafWriter — mutable state for streaming leaf-page construction.
 *
 * leaf_writer_emit() is called once per value group (in ascending value
 * order); it run-optimizes and serializes the group's bitmap, transitions to
 * a new leaf page when the current one is full, and records one directory
 * entry per completed leaf page.  leaf_writer_finish() finalizes the last
 * partially filled leaf page after the stream is exhausted.
 */
typedef struct LeafWriter
{
	Relation			index;
	int					max_inline;
	RoaringDirEntry	   *leaf_entries;	/* one per completed leaf page */
	uint32				leaf_count;
	uint32				leaf_cap;		/* allocated slots in leaf_entries */
	long				nentries;		/* distinct values emitted */
	Buffer				leaf_buf;
	Page				leaf_page;
	RoaringLeafSpecial *leaf_spc;
	BlockNumber			leftmost;
	BlockNumber			rightmost;
	Buffer				batch_bufs[ROARING_BUILD_WAL_BATCH];
	BlockNumber			batch_blknos[ROARING_BUILD_WAL_BATCH];
	int					batch_n;
} LeafWriter;

/*
 * Ensure leaf_entries has room for one more directory entry, doubling the
 * allocation on demand.  The number of leaf pages is not known until the
 * sorted stream is fully consumed; for a multi-column index it is far smaller
 * than the total emitted-tuple count (which is nkeys * nrows), so we grow the
 * array to the actual leaf-page count rather than pre-sizing by tuple count.
 */
static void
leaf_writer_reserve(LeafWriter *w)
{
	if (w->leaf_count < w->leaf_cap)
		return;

	if (w->leaf_cap == 0)
	{
		w->leaf_cap = 1024;
		w->leaf_entries = (RoaringDirEntry *)
			palloc_extended((Size) w->leaf_cap * sizeof(RoaringDirEntry),
							MCXT_ALLOC_HUGE);
	}
	else
	{
		w->leaf_cap *= 2;
		w->leaf_entries = (RoaringDirEntry *)
			repalloc_huge(w->leaf_entries,
						  (Size) w->leaf_cap * sizeof(RoaringDirEntry));
	}
}

/*
 * Write value's bitmap as an overflow entry on the current leaf page: the
 * serialized bytes go to an overflow chain and only the small fixed-size
 * RoaringOverflowEntry header lands on the leaf page, so it always fits.
 */
static void
leaf_emit_overflow(LeafWriter *w, int64 value, roaring64_bitmap_t *bm64,
				   size_t bitmap_size)
{
	char				 *bm_data;
	RoaringOverflowEntry *oe;

	bm_data = (char *) palloc(bitmap_size);
	roaring64_bitmap_portable_serialize(bm64, bm_data);
	oe		= (RoaringOverflowEntry *) palloc(sizeof(RoaringOverflowEntry));
	oe->value		   = value;
	oe->cardinality	   = roaring64_cardinality32(bm64);
	oe->flags		   = ROARING_ENTRY_OVERFLOW;
	oe->total_len	   = (uint32) bitmap_size;
	oe->overflow_blkno = roaring_write_overflow_chain(w->index, bm_data,
													   bitmap_size);
	pfree(bm_data);

	if (PageAddItem(w->leaf_page, (Item) oe, sizeof(RoaringOverflowEntry),
					InvalidOffsetNumber, false, false) == InvalidOffsetNumber)
		elog(ERROR, "roaring_build: overflow PageAddItem failed unexpectedly");
	pfree(oe);
}

static void
leaf_writer_emit(LeafWriter *w, int64 value, roaring64_bitmap_t *bm64)
{
	size_t	bitmap_size;
	Size	entry_size;

	roaring64_bitmap_run_optimize(bm64);
	bitmap_size = roaring64_bitmap_portable_size_in_bytes(bm64);

	w->nentries++;

	if (bitmap_size > (size_t) w->max_inline)
		entry_size = MAXALIGN(sizeof(RoaringOverflowEntry));
	else
		entry_size = MAXALIGN(sizeof(RoaringLeafEntry) + bitmap_size);

	/* ---- transition to new leaf page if needed ---- */
	if (w->leaf_buf == InvalidBuffer ||
		PageGetFreeSpace(w->leaf_page) < entry_size)
	{
		if (w->leaf_buf != InvalidBuffer)
		{
			OffsetNumber	  maxoff;
			RoaringLeafEntry *last;
			BlockNumber		  old_blkno;
			Buffer			  new_buf;
			BlockNumber		  new_blkno;

			maxoff	  = PageGetMaxOffsetNumber(w->leaf_page);
			last	  = (RoaringLeafEntry *)
						PageGetItem(w->leaf_page,
									PageGetItemId(w->leaf_page, maxoff));
			old_blkno = BufferGetBlockNumber(w->leaf_buf);

			leaf_writer_reserve(w);
			w->leaf_entries[w->leaf_count].high_key   = last->value;
			w->leaf_entries[w->leaf_count].child_page = old_blkno;
			w->leaf_count++;

			new_buf	  = roaring_extend_page(w->index);
			new_blkno = BufferGetBlockNumber(new_buf);

			w->leaf_spc->right_page = new_blkno;
			MarkBufferDirty(w->leaf_buf);
			w->batch_bufs[w->batch_n]   = w->leaf_buf;
			w->batch_blknos[w->batch_n] = old_blkno;
			w->batch_n++;
			if (w->batch_n == ROARING_BUILD_WAL_BATCH)
			{
				flush_leaf_wal_batch(w->index, w->batch_bufs,
									 w->batch_blknos, w->batch_n);
				w->batch_n = 0;
			}

			w->leaf_buf  = new_buf;
			w->leaf_page = BufferGetPage(w->leaf_buf);
			PageInit(w->leaf_page, BLCKSZ, sizeof(RoaringLeafSpecial));
			w->leaf_spc  = (RoaringLeafSpecial *)
						PageGetSpecialPointer(w->leaf_page);
			w->leaf_spc->page_type	 = ROARING_PAGE_LEAF;
			w->leaf_spc->flags		 = 0;
			w->leaf_spc->entry_count = 0;
			w->leaf_spc->left_page	 = old_blkno;
			w->leaf_spc->right_page  = InvalidBlockNumber;
		}
		else
		{
			w->leaf_buf  = roaring_extend_page(w->index);
			w->leaf_page = BufferGetPage(w->leaf_buf);
			PageInit(w->leaf_page, BLCKSZ, sizeof(RoaringLeafSpecial));
			w->leaf_spc  = (RoaringLeafSpecial *)
						PageGetSpecialPointer(w->leaf_page);
			w->leaf_spc->page_type	 = ROARING_PAGE_LEAF;
			w->leaf_spc->flags		 = 0;
			w->leaf_spc->entry_count = 0;
			w->leaf_spc->left_page	 = InvalidBlockNumber;
			w->leaf_spc->right_page  = InvalidBlockNumber;
			w->leftmost = BufferGetBlockNumber(w->leaf_buf);
		}
	}

	if (bitmap_size > (size_t) w->max_inline)
	{
		leaf_emit_overflow(w, value, bm64, bitmap_size);
	}
	else
	{
		RoaringLeafEntry *le;

		le = (RoaringLeafEntry *) palloc(sizeof(RoaringLeafEntry) + bitmap_size);
		le->value       = value;
		le->flags       = ROARING_ENTRY_INLINE;
		le->cardinality = roaring64_cardinality32(bm64);
		roaring64_bitmap_portable_serialize(bm64, (char *)(le + 1));

		if (PageAddItem(w->leaf_page, (Item) le,
						sizeof(RoaringLeafEntry) + bitmap_size,
						InvalidOffsetNumber, false, false) == InvalidOffsetNumber)
		{
			/*
			 * Defensive fallback: the corrected max_inline guarantees an inline
			 * entry fits a fresh leaf page, so this should be unreachable.  But
			 * rather than abort a multi-hour build on a boundary miscalculation,
			 * fall back to overflow — its header is far smaller and fits the
			 * same page.
			 */
			pfree(le);
			leaf_emit_overflow(w, value, bm64, bitmap_size);
		}
		else
			pfree(le);
	}

	Assert(w->leaf_spc->entry_count < PG_UINT16_MAX);
	w->leaf_spc->entry_count++;
}

static void
leaf_writer_finish(LeafWriter *w)
{
	OffsetNumber	  maxoff;
	RoaringLeafEntry *last;
	BlockNumber		  blkno;

	if (w->leaf_buf == InvalidBuffer)
		return;

	blkno  = BufferGetBlockNumber(w->leaf_buf);
	maxoff = PageGetMaxOffsetNumber(w->leaf_page);
	last   = (RoaringLeafEntry *)
			 PageGetItem(w->leaf_page, PageGetItemId(w->leaf_page, maxoff));

	leaf_writer_reserve(w);
	w->leaf_entries[w->leaf_count].high_key   = last->value;
	w->leaf_entries[w->leaf_count].child_page = blkno;
	w->leaf_count++;

	w->rightmost = blkno;
	MarkBufferDirty(w->leaf_buf);
	w->batch_bufs[w->batch_n]   = w->leaf_buf;
	w->batch_blknos[w->batch_n] = blkno;
	w->batch_n++;
	flush_leaf_wal_batch(w->index, w->batch_bufs, w->batch_blknos, w->batch_n);
	w->batch_n = 0;
}

/* ----------------------------------------------------------------
 * write_leaf_and_dir_pages
 *
 * Streams sorted (value, tid) tuples out of the Tuplesortstate, builds one
 * roaring bitmap per value group, writes them as leaf pages, then builds the
 * directory (1-level if ≤ max_dir leaf pages, 2/3-level otherwise).
 * Sets *root_dir_out = InvalidBlockNumber if nentries == 0.
 *
 * ntuples is the post-scan count of emitted (value, tid) pairs — an upper
 * bound on distinct values, used to size leaf_entries and to pre-check the
 * directory capacity.
 * ---------------------------------------------------------------- */
static void
write_leaf_and_dir_pages(Relation index,
						  Tuplesortstate *sortstate, TupleDesc tupdesc,
						  long ntuples,
						  long *nentries_out,
						  BlockNumber *root_dir_out,
						  BlockNumber *leftmost_out,
						  BlockNumber *rightmost_out)
{
	/*
	 * Max bitmap payload that fits inline on a fresh leaf page.  An item costs
	 * MAXALIGN(sizeof(RoaringLeafEntry) + bitmap) of data space plus a 4-byte
	 * line pointer, so we MAXALIGN_DOWN the page's usable space *after*
	 * reserving the line pointer, then subtract the entry header.  Subtracting
	 * the (unaligned) line pointer before rounding — as a naive computation
	 * does — leaves up to MAXIMUM_ALIGNOF-1 bytes of slack, which lets a
	 * boundary-sized bitmap be classified inline yet fail PageAddItem.
	 */
	const int max_inline = (int)(MAXALIGN_DOWN(BLCKSZ
									 - SizeOfPageHeaderData
									 - MAXALIGN(sizeof(RoaringLeafSpecial))
									 - sizeof(ItemIdData))
								 - sizeof(RoaringLeafEntry));

	/* Flat-array dir capacity per page (no line pointers). */
	const uint32 max_dir = (uint32)((BLCKSZ
									 - MAXALIGN(sizeof(RoaringDirSpecial))
									 - SizeOfPageHeaderData)
									/ sizeof(RoaringDirEntry));

	RoaringDirEntry	   *leaf_entries;
	uint32				leaf_count;
	LeafWriter			w;

	Assert(max_dir < PG_UINT16_MAX); /* entry_count is uint16 */

	if (ntuples == 0)
	{
		*nentries_out  = 0;
		*root_dir_out  = InvalidBlockNumber;
		*leftmost_out  = InvalidBlockNumber;
		*rightmost_out = InvalidBlockNumber;
		return;
	}

	/*
	 * leaf_entries grows on demand to the actual number of leaf pages (see
	 * leaf_writer_reserve).  It cannot be pre-sized from the emitted-tuple
	 * count: for a multi-column index that count is nkeys * nrows, whereas the
	 * number of distinct column-namespaced values — and hence leaf pages — is
	 * far smaller.  A genuinely oversized index is caught after streaming, when
	 * the level-2 directory would overflow max_dir (see Phase B below).
	 */
	memset(&w, 0, sizeof(w));
	w.index        = index;
	w.max_inline   = max_inline;
	w.leaf_buf     = InvalidBuffer;
	w.leftmost     = InvalidBlockNumber;
	w.rightmost    = InvalidBlockNumber;

	/*
	 * ---- Phase A: stream sorted tuples, one value group at a time ----
	 *
	 * tuplesort returns tuples in (value ASC, tid ASC) order, so equal values
	 * arrive contiguously with ascending tids.  We accumulate each value
	 * group's tids into a single roaring bitmap and hand it to the leaf writer
	 * on the value transition (and once more at end-of-stream).  Peak roaring
	 * memory is one in-progress bitmap (bounded by a single value's
	 * cardinality), not the whole sorted set.
	 *
	 * bm64 is volatile so PG_FINALLY frees the correct pointer after a longjmp
	 * (an ERROR may fire mid-emit before the bitmap is freed and NULLed).
	 */
	{
		roaring64_bitmap_t * volatile bm64 = NULL;
		TupleTableSlot * volatile slot = NULL;
		bool	have_group = false;
		int64	cur_value  = 0;

		PG_TRY();
		{
			slot = MakeSingleTupleTableSlot(tupdesc, &TTSOpsMinimalTuple);

			while (tuplesort_gettupleslot(sortstate, true, false, slot, NULL))
			{
				int64	value;
				uint64	tid;
				bool	isn;

				CHECK_FOR_INTERRUPTS();

				value = DatumGetInt64(slot_getattr(slot, 1, &isn));
				tid   = (uint64) DatumGetInt64(slot_getattr(slot, 2, &isn));

				if (have_group && value != cur_value)
				{
					leaf_writer_emit(&w, cur_value, bm64);
					roaring64_bitmap_free(bm64);
					bm64 = NULL;
					have_group = false;
				}

				if (!have_group)
				{
					bm64       = roaring64_bitmap_create();
					cur_value  = value;
					have_group = true;
				}

				roaring64_bitmap_add(bm64, tid);
			}

			if (have_group)
			{
				leaf_writer_emit(&w, cur_value, bm64);
				roaring64_bitmap_free(bm64);
				bm64 = NULL;
				have_group = false;
			}
		}
		PG_FINALLY();
		{
			if (bm64)
				roaring64_bitmap_free(bm64);
			if (slot)
				ExecDropSingleTupleTableSlot(slot);
		}
		PG_END_TRY();
	}

	/* Finalize the last partially filled leaf page. */
	leaf_writer_finish(&w);

	leaf_count   = w.leaf_count;
	leaf_entries = w.leaf_entries;	/* refresh: array may have been repalloc'd */

	/* ---- Phase B: build directory ---- */
	if (leaf_count <= max_dir)
	{
		/* Single-level: root points directly to leaf pages. */
		*root_dir_out = roaring_write_dir_page(index, leaf_entries, leaf_count, 0);
	}
	else
	{
		/*
		 * Two-level: batch leaf_entries into level-1 dir pages, then
		 * write a root (level 0) page pointing to the level-1 pages.
		 *
		 * For 10K distinct values this is ~2 level-1 pages; the root
		 * comfortably fits in one page (max_dir ≈ 679 entries).
		 */
		uint32			 l1_count;
		RoaringDirEntry *l1_entries;
		uint32			 j;

		l1_count  = (leaf_count + max_dir - 1) / max_dir; /* ceil division */
		l1_entries = palloc(l1_count * sizeof(RoaringDirEntry));

		for (j = 0; j < l1_count; j++)
		{
			uint32		start = j * max_dir;
			uint32		count = Min(max_dir, leaf_count - start);
			BlockNumber blkno;

			/*
			 * Level-1 pages (level=0): their children are leaf pages.
			 * Convention: level=0 means "children are leaf pages";
			 *             level=N>0 means "children are dir pages at level N-1".
			 */
			blkno = roaring_write_dir_page(index, leaf_entries + start, count, 0);
			l1_entries[j].high_key   = leaf_entries[start + count - 1].high_key;
			l1_entries[j].child_page = blkno;
		}

		if (l1_count > max_dir)
		{
			uint32           l2_count  = (l1_count + max_dir - 1) / max_dir;
			RoaringDirEntry *l2_entries;
			uint32           k;

			if (l2_count > max_dir)
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("roaring_build: index too large for three-level directory"),
						 errdetail("%u leaf pages exceed capacity %ld.",
								   leaf_count,
								   (long) max_dir * (long) max_dir * (long) max_dir),
						 errhint("Reduce the number of indexed distinct values, "
								 "or use REINDEX after reducing cardinality.")));

			l2_entries = palloc(l2_count * sizeof(RoaringDirEntry));

			for (k = 0; k < l2_count; k++)
			{
				uint32      start = k * max_dir;
				uint32      count = Min(max_dir, l1_count - start);
				BlockNumber blkno;

				blkno = roaring_write_dir_page(index, l1_entries + start, count, 1);
				l2_entries[k].high_key   = l1_entries[start + count - 1].high_key;
				l2_entries[k].child_page = blkno;
			}

			*root_dir_out = roaring_write_dir_page(index, l2_entries, l2_count, 2);
			pfree(l2_entries);
		}
		else
			*root_dir_out = roaring_write_dir_page(index, l1_entries, l1_count, 1);

		pfree(l1_entries);
	}

	pfree(leaf_entries);
	*nentries_out  = w.nentries;
	*leftmost_out  = w.leftmost;
	*rightmost_out = w.rightmost;
}

/* ================================================================
 * roaring_build
 * ================================================================ */
IndexBuildResult *
roaring_build(Relation heap, Relation index, struct IndexInfo *indexInfo)
{
	IndexBuildResult   *result;
	RoaringBuildState	bstate;
	double				reltuples;
	long				nentries    = 0;

	BlockNumber			root_dir		= InvalidBlockNumber;
	BlockNumber			leftmost_leaf	= InvalidBlockNumber;
	BlockNumber			rightmost_leaf	= InvalidBlockNumber;


	result = (IndexBuildResult *) palloc0(sizeof(IndexBuildResult));

	/* Block 0: placeholder — filled in by write_metapage at the end. */
	{
		Buffer				 buf = roaring_extend_page(index);
		Page				 pg;
		RoaringMetaPageData *m;

		Assert(BufferGetBlockNumber(buf) == ROARING_METAPAGE_BLKNO);
		pg = BufferGetPage(buf);
		PageInit(pg, BLCKSZ, 0);
		m = RoaringPageGetMeta(pg);
		memset(m, 0, sizeof(*m));
		m->payload_dir_head = InvalidBlockNumber;
		((PageHeader) pg)->pd_lower =
			(LocationIndex)(SizeOfPageHeaderData + sizeof(RoaringMetaPageData));
		roaring_wal_and_release(index, buf);
	}

	/*
	 * Sort (value, tid) pairs through a Tuplesortstate so the build spills to
	 * temp files rather than holding every pair in RAM (T46/T61).  The synthetic
	 * sort tuple is two int8 columns: (value, tid), sorted ascending on both,
	 * which matches the (value ASC, tid ASC) order the leaf writer expects.
	 */
	{
		AttrNumber	attNums[2]     = {1, 2};
		Oid			sortOps[2]     = {Int8LessOperator, Int8LessOperator};
		Oid			sortColls[2]   = {InvalidOid, InvalidOid};
		bool		nullsFirst[2]  = {false, false};

		bstate.tupdesc = CreateTemplateTupleDesc(2);
		TupleDescInitEntry(bstate.tupdesc, (AttrNumber) 1, "value", INT8OID, -1, 0);
		TupleDescInitEntry(bstate.tupdesc, (AttrNumber) 2, "tid",   INT8OID, -1, 0);

		bstate.sortstate = tuplesort_begin_heap(bstate.tupdesc, 2,
												attNums, sortOps, sortColls,
												nullsFirst,
												maintenance_work_mem, NULL,
												TUPLESORT_NONE);
		bstate.slot = MakeSingleTupleTableSlot(bstate.tupdesc, &TTSOpsVirtual);
	}

	bstate.heap_tuples = 0;
	bstate.atttypid    = TupleDescAttr(index->rd_att, 0)->atttypid;
	bstate.ntuples     = 0;

	pgstat_progress_update_param(PROGRESS_CREATEIDX_TUPLES_TOTAL,
								 (int64) heap->rd_rel->reltuples);

	reltuples = table_index_build_scan(heap, index, indexInfo,
									   true, true,
									   roaring_build_callback,
									   &bstate, NULL);

	pgstat_progress_update_param(PROGRESS_CREATEIDX_TUPLES_DONE,
								 (int64) bstate.ntuples);

	/* Sort (value, tid); tuplesort spills to disk under maintenance_work_mem. */
	pgstat_progress_update_param(PROGRESS_CREATEIDX_PHASE, 2); /* sorting */
	tuplesort_performsort(bstate.sortstate);

	/* Write leaf pages + directory; counts distinct values into nentries. */
	pgstat_progress_update_param(PROGRESS_CREATEIDX_PHASE, 3); /* writing pages */
	write_leaf_and_dir_pages(index, bstate.sortstate, bstate.tupdesc,
							 bstate.ntuples,
							 &nentries, &root_dir, &leftmost_leaf, &rightmost_leaf);

	tuplesort_end(bstate.sortstate);
	ExecDropSingleTupleTableSlot(bstate.slot);
	FreeTupleDesc(bstate.tupdesc);

	{
		BlockNumber pending_blknos[ROARING_PENDING_SHARDS];
		BlockNumber dummy_head;
		int			i;

		for (i = 0; i < ROARING_PENDING_SHARDS; i++)
			pending_blknos[i] =
				roaring_init_pending_page(index, InvalidBuffer, &dummy_head,
										 ROARING_PAGE_PENDING_INSERT);

		write_metapage(index, root_dir, leftmost_leaf, rightmost_leaf,
					   pending_blknos, (uint32) nentries,
					   ROARING_FLAG_EXACT);
	}

	result->heap_tuples  = reltuples;
	result->index_tuples = (double) nentries;
	return result;
}

/* ================================================================
 * roaring_buildempty
 *
 * Called by PostgreSQL to initialise the INIT fork of an UNLOGGED index.
 * The INIT fork is copied to the MAIN fork on crash recovery, so it must
 * contain a complete, valid empty index state.
 *
 * We cannot use the normal buffer-manager path (ReadBuffer, etc.) because
 * that always targets MAIN_FORKNUM.  Instead, write all three pages
 * directly to INIT_FORKNUM via smgrextend, WAL-log each with log_newpage,
 * then sync.  Pattern: contrib/bloom/blutils.c:blbuildempty.
 * ================================================================ */
void
roaring_buildempty(Relation index)
{
	SMgrRelation		  smgr = RelationGetSmgr(index);
	char				 *buf  = (char *) palloc(BLCKSZ);
	Page				  page = (Page) buf;
	RoaringMetaPageData  *meta;
	RoaringPendingSpecial *spc;
	int					  i;

	smgrcreate(smgr, INIT_FORKNUM, false);

	/* Page 0: metapage */
	PageInit(page, BLCKSZ, 0);
	meta = RoaringPageGetMeta(page);
	memset(meta, 0, sizeof(*meta));
	meta->magic					   = ROARING_MAGIC;
	meta->version				   = ROARING_INDEX_VERSION;
	meta->flags					   = ROARING_FLAG_EXACT;
	meta->croaring_format_version  = ROARING_EXPECTED_FORMAT_VERSION;
	meta->num_shards			   = ROARING_PENDING_SHARDS;
	meta->root_directory_page	   = InvalidBlockNumber;
	meta->leftmost_leaf_page	   = InvalidBlockNumber;
	meta->rightmost_leaf_page	   = InvalidBlockNumber;
	meta->free_list_head		   = InvalidBlockNumber;

	/* Shards occupy pages 1..ROARING_PENDING_SHARDS */
	for (i = 0; i < ROARING_PENDING_SHARDS; i++)
	{
		BlockNumber blkno = (BlockNumber)(i + 1);

		meta->shards[i].insert_head  = blkno;
		meta->shards[i].insert_tail  = blkno;
		meta->shards[i].insert_count = 0;
		meta->shards[i].merging_head = InvalidBlockNumber;
		meta->shards[i].carry_head   = InvalidBlockNumber;
	}
	meta->total_entries			  = 0;
	meta->pending_merge_threshold = (uint32) roaring_pending_merge_threshold_guc;
	meta->payload_dir_head		  = InvalidBlockNumber;
	((PageHeader) page)->pd_lower =
		(LocationIndex)(SizeOfPageHeaderData + sizeof(RoaringMetaPageData));
	PageSetChecksumInplace(page, ROARING_METAPAGE_BLKNO);
	smgrextend(smgr, INIT_FORKNUM, ROARING_METAPAGE_BLKNO, page, true);
	log_newpage(&smgr->smgr_rlocator.locator, INIT_FORKNUM,
				ROARING_METAPAGE_BLKNO, page, true);

	/* Pages 1..ROARING_PENDING_SHARDS: one empty pending page per shard */
	PageInit(page, BLCKSZ, sizeof(RoaringPendingSpecial));
	spc = (RoaringPendingSpecial *) PageGetSpecialPointer(page);
	spc->page_type	 = ROARING_PAGE_PENDING_INSERT;
	spc->flags		 = 0;
	spc->entry_count = 0;
	spc->next_page	 = InvalidBlockNumber;
	spc->xmin_low	 = InvalidTransactionId;
	spc->_pad		 = 0;
	spc->value_min	 = PG_INT64_MAX;
	spc->value_max	 = PG_INT64_MIN;

	for (i = 0; i < ROARING_PENDING_SHARDS; i++)
	{
		BlockNumber blkno = (BlockNumber)(i + 1);

		PageSetChecksumInplace(page, blkno);
		smgrextend(smgr, INIT_FORKNUM, blkno, page, true);
		log_newpage(&smgr->smgr_rlocator.locator, INIT_FORKNUM, blkno, page, true);
	}

	smgrimmedsync(smgr, INIT_FORKNUM);
	pfree(buf);
}

