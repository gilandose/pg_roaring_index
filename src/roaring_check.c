#include "pg_roaring_index.h"

#include "access/relation.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/rel.h"

PG_FUNCTION_INFO_V1(roaring_index_check);

/* ----------------------------------------------------------------
 * Internal types
 * ---------------------------------------------------------------- */

/*
 * Overflow chains are verified in a second pass after the leaf walk so we
 * never hold a leaf buffer and an overflow buffer simultaneously.
 */
typedef struct DeferredOvf
{
	BlockNumber  leaf_blkno;
	OffsetNumber leaf_off;
	BlockNumber  chain_start;
	uint32       total_len;
} DeferredOvf;

/* ----------------------------------------------------------------
 * Error macro — includes index name in every message.
 * ---------------------------------------------------------------- */
#define ROARING_CHECK_ERROR(fmt, ...) \
	ereport(ERROR, \
			(errcode(ERRCODE_INDEX_CORRUPTED), \
			 errmsg("pg_roaring_index: corruption detected in index \"%s\": " fmt, \
					RelationGetRelationName(index), ##__VA_ARGS__)))

/* ----------------------------------------------------------------
 * check_metapage
 * ---------------------------------------------------------------- */
static void
check_metapage(Relation index, const RoaringMetaPageData *meta,
			   BlockNumber nblocks)
{
	int s;

	if (meta->magic != ROARING_MAGIC)
		ROARING_CHECK_ERROR("bad magic 0x%08X in metapage (expected 0x%08X)",
							meta->magic, ROARING_MAGIC);
	if (meta->version != ROARING_INDEX_VERSION)
		ROARING_CHECK_ERROR("version mismatch: on-disk %u, compiled-in %u",
							meta->version, ROARING_INDEX_VERSION);
	if (meta->num_shards != ROARING_PENDING_SHARDS)
		ROARING_CHECK_ERROR("num_shards %u != compiled-in %d",
							meta->num_shards, ROARING_PENDING_SHARDS);
	if (meta->croaring_format_version != ROARING_EXPECTED_FORMAT_VERSION)
		ROARING_CHECK_ERROR("croaring_format_version %u != expected %u",
							meta->croaring_format_version,
							ROARING_EXPECTED_FORMAT_VERSION);
	if (meta->root_directory_page == InvalidBlockNumber ||
		meta->root_directory_page >= nblocks)
		ROARING_CHECK_ERROR("root_directory_page %u is out of range (nblocks %u)",
							meta->root_directory_page, nblocks);
	if (meta->leftmost_leaf_page == InvalidBlockNumber ||
		meta->leftmost_leaf_page >= nblocks)
		ROARING_CHECK_ERROR("leftmost_leaf_page %u is out of range",
							meta->leftmost_leaf_page);
	if (meta->rightmost_leaf_page == InvalidBlockNumber ||
		meta->rightmost_leaf_page >= nblocks)
		ROARING_CHECK_ERROR("rightmost_leaf_page %u is out of range",
							meta->rightmost_leaf_page);
	if (meta->free_list_head != InvalidBlockNumber &&
		meta->free_list_head >= nblocks)
		ROARING_CHECK_ERROR("free_list_head %u is out of range",
							meta->free_list_head);

	for (s = 0; s < ROARING_PENDING_SHARDS; s++)
	{
		const RoaringPendingShard *sh = &meta->shards[s];

		if (sh->insert_head != InvalidBlockNumber &&
			sh->insert_head >= nblocks)
			ROARING_CHECK_ERROR("shard %d: insert_head %u out of range",
								s, sh->insert_head);
		if (sh->insert_tail != InvalidBlockNumber &&
			sh->insert_tail >= nblocks)
			ROARING_CHECK_ERROR("shard %d: insert_tail %u out of range",
								s, sh->insert_tail);
		if (sh->merging_head != InvalidBlockNumber &&
			sh->merging_head >= nblocks)
			ROARING_CHECK_ERROR("shard %d: merging_head %u out of range",
								s, sh->merging_head);
	}
}

/* ----------------------------------------------------------------
 * check_directory
 *
 * Walk every directory page.  Verify:
 *   - page_type == ROARING_PAGE_DIRECTORY
 *   - entry_count within page bounds
 *   - high_keys strictly ascending within each page and across pages
 *   - each child_page is a valid block containing a leaf page
 * ---------------------------------------------------------------- */
static void
check_directory(Relation index, BlockNumber root_dir, BlockNumber nblocks)
{
	BlockNumber cur       = root_dir;
	int64       prev_key  = PG_INT64_MIN;
	bool        have_prev = false;
	uint32      steps     = 0;

	while (cur != InvalidBlockNumber)
	{
		Buffer			  buf;
		Page			  page;
		RoaringDirSpecial *spc;
		RoaringDirEntry   *raw_entries;
		BlockNumber        right;
		int                n, max_entries, i;
		uint8              level;

		/* Palloc'd copies of entries so we can read them after buffer release. */
		int64       *high_keys;
		BlockNumber *child_pages;

		if (++steps > nblocks)
			ROARING_CHECK_ERROR("directory chain has more pages than the relation "
								"(%u); possible cycle", nblocks);

		CHECK_FOR_INTERRUPTS();

		if (cur >= nblocks)
			ROARING_CHECK_ERROR("directory right_page %u is out of range", cur);

		buf         = ReadBuffer(index, cur);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page        = BufferGetPage(buf);
		spc         = (RoaringDirSpecial *) PageGetSpecialPointer(page);
		raw_entries = (RoaringDirEntry *) PageGetContents(page);
		right       = spc->right_page;
		n           = (int) spc->entry_count;
		level       = spc->level;

		if (spc->page_type != ROARING_PAGE_DIRECTORY)
		{
			UnlockReleaseBuffer(buf);
			ROARING_CHECK_ERROR("block %u: expected page_type DIRECTORY (0x%02X), got 0x%02X",
								cur, ROARING_PAGE_DIRECTORY, spc->page_type);
		}
		/* T37: level must be ≤ 2 (max three-level directory); reserved must be 0. */
		if (spc->level > 2)
		{
			uint8 bad_level = spc->level;

			UnlockReleaseBuffer(buf);
			ROARING_CHECK_ERROR("directory page %u: level %u > 2", cur, bad_level);
		}
		if (spc->reserved != 0)
		{
			uint32 bad_reserved = spc->reserved;

			UnlockReleaseBuffer(buf);
			ROARING_CHECK_ERROR("directory page %u: reserved field is %u (expected 0)",
								cur, bad_reserved);
		}

		max_entries = (int) ((PageGetPageSize(page)
							  - SizeOfPageHeaderData
							  - MAXALIGN(sizeof(RoaringDirSpecial)))
							 / ROARING_DIR_ENTRY_SIZE);
		if (n < 0 || n > max_entries)
		{
			UnlockReleaseBuffer(buf);
			ROARING_CHECK_ERROR("directory page %u: entry_count %d out of range [0, %d]",
								cur, n, max_entries);
		}

		/*
		 * Copy (high_key, child_page) pairs into palloc'd arrays before
		 * releasing the buffer — raw_entries points into the shared buffer
		 * page and becomes invalid after UnlockReleaseBuffer.
		 */
		high_keys   = (int64 *)       palloc(n * sizeof(int64));
		child_pages = (BlockNumber *) palloc(n * sizeof(BlockNumber));
		for (i = 0; i < n; i++)
		{
			int64       hk    = raw_entries[i].high_key;
			BlockNumber child = raw_entries[i].child_page;

			if (have_prev && hk <= prev_key)
			{
				UnlockReleaseBuffer(buf);
				pfree(high_keys);
				pfree(child_pages);
				ROARING_CHECK_ERROR("directory page %u: entry %d high_key " INT64_FORMAT
									" not strictly above previous " INT64_FORMAT,
									cur, i, hk, prev_key);
			}
			if (child == InvalidBlockNumber || child >= nblocks)
			{
				UnlockReleaseBuffer(buf);
				pfree(high_keys);
				pfree(child_pages);
				ROARING_CHECK_ERROR("directory page %u: entry %d child_page %u out of range",
									cur, i, child);
			}

			high_keys[i]   = hk;
			child_pages[i] = child;
			prev_key  = hk;
			have_prev = true;
		}

		UnlockReleaseBuffer(buf);

		/*
		 * Verify each child has the expected page type.  If level > 0, children
		 * are inner directory pages (recurse); if level == 0, children are leaves.
		 * Done after releasing the directory buffer so we never hold two buffers
		 * simultaneously.
		 */
		for (i = 0; i < n; i++)
		{
			BlockNumber child = child_pages[i];
			Buffer      cbuf;
			uint8       child_page_type;

			cbuf            = ReadBuffer(index, child);
			LockBuffer(cbuf, BUFFER_LOCK_SHARE);
			child_page_type = ((RoaringLeafSpecial *)
								PageGetSpecialPointer(BufferGetPage(cbuf)))->page_type;
			UnlockReleaseBuffer(cbuf);

			if (level > 0)
			{
				if (child_page_type != ROARING_PAGE_DIRECTORY)
				{
					pfree(high_keys);
					pfree(child_pages);
					ROARING_CHECK_ERROR("directory page %u (level %u): entry %d child %u "
										"has page_type 0x%02X, expected DIRECTORY",
										cur, level, i, child, child_page_type);
				}
				/* Recurse into sub-directory. */
				check_directory(index, child, nblocks);
			}
			else
			{
				if (child_page_type != ROARING_PAGE_LEAF)
				{
					pfree(high_keys);
					pfree(child_pages);
					ROARING_CHECK_ERROR("directory page %u: entry %d child %u has "
										"page_type 0x%02X, expected LEAF",
										cur, i, child, child_page_type);
				}
			}
		}

		pfree(high_keys);
		pfree(child_pages);

		cur = right;
	}
}

/* ----------------------------------------------------------------
 * check_leaf_chain
 *
 * Walk the leaf doubly-linked list from leftmost to rightmost.  Verify:
 *   - page_type == ROARING_PAGE_LEAF
 *   - left_page back-link matches the previous page
 *   - entry_count matches actual line-pointer count
 *   - values strictly ascending within each page and across pages
 *   - per-entry flags and minimum item sizes
 *   - overflow entries are deferred for second-pass chain check
 *
 * Returns the last leaf block seen (should equal rightmost_leaf_page).
 * ---------------------------------------------------------------- */
static BlockNumber
check_leaf_chain(Relation index,
				 BlockNumber leftmost,
				 BlockNumber nblocks,
				 DeferredOvf **ovf_out,
				 int *novf_out)
{
	BlockNumber  cur        = leftmost;
	BlockNumber  prev_blkno = InvalidBlockNumber;
	int64        prev_max   = PG_INT64_MIN;
	bool         first_page = true;
	int          novf       = 0;
	int          ovf_cap    = 32;
	DeferredOvf *ovf        = (DeferredOvf *) palloc(ovf_cap * sizeof(DeferredOvf));
	uint32       steps      = 0;

	while (cur != InvalidBlockNumber)
	{
		Buffer			   buf;
		Page			   page;
		RoaringLeafSpecial *spc;
		OffsetNumber       noff, i;
		int64              page_min = PG_INT64_MAX;
		int64              page_max = PG_INT64_MIN;
		int64              prev_val = PG_INT64_MIN;
		bool               have_prev_val = false;
		BlockNumber        right;

		if (++steps > nblocks)
			ROARING_CHECK_ERROR("leaf chain has more pages than the relation "
								"(%u); possible cycle", nblocks);

		CHECK_FOR_INTERRUPTS();

		if (cur >= nblocks)
			ROARING_CHECK_ERROR("leaf right_page %u is out of range", cur);

		buf  = ReadBuffer(index, cur);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		spc  = (RoaringLeafSpecial *) PageGetSpecialPointer(page);

		if (spc->page_type != ROARING_PAGE_LEAF)
		{
			UnlockReleaseBuffer(buf);
			ROARING_CHECK_ERROR("block %u: expected LEAF page type, got 0x%02X",
								cur, spc->page_type);
		}

		if (spc->left_page != prev_blkno)
		{
			UnlockReleaseBuffer(buf);
			ROARING_CHECK_ERROR("leaf page %u: left_page %u != expected %u",
								cur, spc->left_page, prev_blkno);
		}

		right = spc->right_page;
		noff  = PageGetMaxOffsetNumber(page);

		if ((OffsetNumber) spc->entry_count != noff)
		{
			UnlockReleaseBuffer(buf);
			ROARING_CHECK_ERROR("leaf page %u: entry_count %u != actual item count %u",
								cur, spc->entry_count, noff);
		}

		/* Overlap next page's I/O with processing this one.
		 * T40: guard against corrupt right_page before passing to prefetch. */
		if (right != InvalidBlockNumber && right < nblocks)
			PrefetchBuffer(index, MAIN_FORKNUM, right);

		for (i = 1; i <= noff; i++)
		{
			ItemId        iid  = PageGetItemId(page, i);
			RoaringLeafEntry *le;
			Size          item_len;
			int64         val;

			if (!ItemIdIsNormal(iid))
			{
				UnlockReleaseBuffer(buf);
				ROARING_CHECK_ERROR("leaf page %u: item %d has non-normal item ID",
									cur, i);
			}

			item_len = ItemIdGetLength(iid);
			if (item_len < sizeof(RoaringLeafEntry))
			{
				UnlockReleaseBuffer(buf);
				ROARING_CHECK_ERROR("leaf page %u: item %d length %zu < minimum %zu",
									cur, i, item_len, sizeof(RoaringLeafEntry));
			}

			le  = (RoaringLeafEntry *) PageGetItem(page, iid);
			val = le->value;

			if (have_prev_val && val <= prev_val)
			{
				UnlockReleaseBuffer(buf);
				ROARING_CHECK_ERROR("leaf page %u: item %d value " INT64_FORMAT
									" not strictly above previous " INT64_FORMAT,
									cur, i, val, prev_val);
			}

			if (le->flags == ROARING_ENTRY_OVERFLOW)
			{
				RoaringOverflowEntry *oe = (RoaringOverflowEntry *) le;

				if (item_len < sizeof(RoaringOverflowEntry))
				{
					UnlockReleaseBuffer(buf);
					ROARING_CHECK_ERROR("leaf page %u: overflow item %d length %zu < %zu",
										cur, i, item_len, sizeof(RoaringOverflowEntry));
				}
				if (oe->overflow_blkno == InvalidBlockNumber ||
					oe->overflow_blkno >= nblocks)
				{
					UnlockReleaseBuffer(buf);
					ROARING_CHECK_ERROR("leaf page %u: overflow item %d: "
										"overflow_blkno %u out of range",
										cur, i, oe->overflow_blkno);
				}
				if (oe->total_len == 0)
				{
					UnlockReleaseBuffer(buf);
					ROARING_CHECK_ERROR("leaf page %u: overflow item %d: total_len is zero",
										cur, i);
				}
				if ((Size) oe->total_len > MaxAllocSize)
				{
					UnlockReleaseBuffer(buf);
					ROARING_CHECK_ERROR("leaf page %u: overflow item %d: total_len %u "
										"exceeds MaxAllocSize", cur, i, oe->total_len);
				}

				if (novf == ovf_cap)
				{
					ovf_cap *= 2;
					ovf = (DeferredOvf *) repalloc(ovf, ovf_cap * sizeof(DeferredOvf));
				}
				ovf[novf].leaf_blkno  = cur;
				ovf[novf].leaf_off    = i;
				ovf[novf].chain_start = oe->overflow_blkno;
				ovf[novf].total_len   = oe->total_len;
				novf++;
			}
			else if (le->flags == ROARING_ENTRY_INLINE)
			{
				/* T41: inline entries must have non-zero cardinality. */
				if (le->cardinality == 0)
				{
					UnlockReleaseBuffer(buf);
					ROARING_CHECK_ERROR("leaf page %u: inline item %d has cardinality 0",
										cur, i);
				}
			}
			else
			{
				UnlockReleaseBuffer(buf);
				ROARING_CHECK_ERROR("leaf page %u: item %d has unknown flags 0x%02X",
									cur, i, le->flags);
			}

			if (i == 1)
				page_min = val;
			page_max        = val;
			prev_val        = val;
			have_prev_val   = true;
		}

		/* Cross-page sort order. */
		if (!first_page && noff > 0 && page_min <= prev_max)
		{
			UnlockReleaseBuffer(buf);
			ROARING_CHECK_ERROR("leaf page %u: min value " INT64_FORMAT
								" not strictly above previous page's max " INT64_FORMAT,
								cur, page_min, prev_max);
		}

		if (noff > 0)
			prev_max = page_max;

		prev_blkno = cur;
		cur        = right;
		first_page = false;
		UnlockReleaseBuffer(buf);
	}

	/* F4: rightmost leaf must terminate the chain with right_page == Invalid.
	 * The loop exits only when cur (= right_page of last leaf) is Invalid, so
	 * this is a loop invariant — assert it explicitly for clarity. */
	if (prev_blkno != InvalidBlockNumber)
	{
		Buffer				  rbuf;
		RoaringLeafSpecial	 *rspc;

		rbuf = ReadBuffer(index, prev_blkno);
		LockBuffer(rbuf, BUFFER_LOCK_SHARE);
		rspc = (RoaringLeafSpecial *)
			   PageGetSpecialPointer(BufferGetPage(rbuf));
		if (rspc->right_page != InvalidBlockNumber)
		{
			BlockNumber bad = rspc->right_page;
			UnlockReleaseBuffer(rbuf);
			ROARING_CHECK_ERROR("rightmost leaf page %u: right_page %u is not "
								"InvalidBlockNumber", prev_blkno, bad);
		}
		UnlockReleaseBuffer(rbuf);
	}

	*ovf_out  = ovf;
	*novf_out = novf;
	return prev_blkno;
}

/* ----------------------------------------------------------------
 * check_overflow_chains
 *
 * Second pass: verify each deferred overflow chain.  For each entry:
 *   - every page has page_type == ROARING_PAGE_OVERFLOW
 *   - sequence numbers are 0, 1, 2, … (contiguous, no gaps)
 *   - total bytes across the chain equals oe->total_len
 *   - next_page == InvalidBlockNumber on the last page
 * ---------------------------------------------------------------- */
static void
check_overflow_chains(Relation index, const DeferredOvf *ovf, int novf,
					  BlockNumber nblocks)
{
	Size cap = ROARING_OVERFLOW_PAGE_CAP;
	int  oi;

	for (oi = 0; oi < novf; oi++)
	{
		BlockNumber chain    = ovf[oi].chain_start;
		uint32      expected = ovf[oi].total_len;
		size_t      seen     = 0;
		uint16      seq      = 0;
		uint32      osteps   = 0;
		uint32      max_ovf_steps = (uint32)((expected + cap - 1) / cap) + 1;

		CHECK_FOR_INTERRUPTS();

		while (chain != InvalidBlockNumber)
		{
			Buffer				   ovbuf;
			RoaringOverflowSpecial *ospc;
			size_t				   cl;

			if (++osteps > max_ovf_steps)
				ROARING_CHECK_ERROR("leaf page %u item %d: overflow chain length "
									"exceeds expected page count (%u); possible cycle",
									ovf[oi].leaf_blkno, ovf[oi].leaf_off,
									max_ovf_steps);

			if (chain >= nblocks)
				ROARING_CHECK_ERROR("leaf page %u item %d: overflow chain page %u out of range",
									ovf[oi].leaf_blkno, ovf[oi].leaf_off, chain);

			ovbuf = ReadBuffer(index, chain);
			LockBuffer(ovbuf, BUFFER_LOCK_SHARE);
			ospc  = (RoaringOverflowSpecial *)
					PageGetSpecialPointer(BufferGetPage(ovbuf));

			if (ospc->page_type != ROARING_PAGE_OVERFLOW)
			{
				UnlockReleaseBuffer(ovbuf);
				ROARING_CHECK_ERROR("leaf page %u item %d: overflow block %u has "
									"page_type 0x%02X, expected OVERFLOW",
									ovf[oi].leaf_blkno, ovf[oi].leaf_off,
									chain, ospc->page_type);
			}
			if (ospc->sequence != seq)
			{
				UnlockReleaseBuffer(ovbuf);
				ROARING_CHECK_ERROR("leaf page %u item %d: overflow block %u has "
									"sequence %u, expected %u",
									ovf[oi].leaf_blkno, ovf[oi].leaf_off,
									chain, ospc->sequence, seq);
			}
			/* T38: owner_page is always written as InvalidBlockNumber. */
			if (ospc->owner_page != InvalidBlockNumber)
			{
				BlockNumber bad_owner = ospc->owner_page;

				UnlockReleaseBuffer(ovbuf);
				ROARING_CHECK_ERROR("leaf page %u item %d: overflow block %u has "
									"owner_page %u (expected InvalidBlockNumber)",
									ovf[oi].leaf_blkno, ovf[oi].leaf_off,
									chain, bad_owner);
			}

			cl    = Min(cap, (size_t) expected - seen);
			seen += cl;
			seq++;
			chain = ospc->next_page;
			UnlockReleaseBuffer(ovbuf);
		}

		if (seen != (size_t) expected)
			ROARING_CHECK_ERROR("leaf page %u item %d: overflow chain covers %zu bytes, "
								"entry declares total_len %u",
								ovf[oi].leaf_blkno, ovf[oi].leaf_off, seen, expected);
	}
}

/* ----------------------------------------------------------------
 * check_pending_chain
 *
 * Walk one pending insert chain.  Verify:
 *   - page_type == ROARING_PAGE_PENDING_INSERT
 *   - entry_count <= ROARING_PENDING_PER_PAGE
 *   - value_min / value_max bracket every entry's value
 *   - xmin_low <= every valid xmin on the page
 * ---------------------------------------------------------------- */
static void
check_pending_chain(Relation index, BlockNumber head,
					const char *label, BlockNumber nblocks)
{
	BlockNumber cur   = head;
	uint32      steps = 0;

	while (cur != InvalidBlockNumber)
	{
		Buffer				  buf;
		Page				  page;
		RoaringPendingSpecial *spc;
		RoaringPendingEntry   *raw;
		uint16                k;

		if (++steps > nblocks)
			ROARING_CHECK_ERROR("pending chain \"%s\" has more pages than the "
								"relation (%u); possible cycle", label, nblocks);

		CHECK_FOR_INTERRUPTS();

		if (cur >= nblocks)
			ROARING_CHECK_ERROR("pending chain \"%s\": page %u out of range",
								label, cur);

		buf  = ReadBuffer(index, cur);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		spc  = (RoaringPendingSpecial *) PageGetSpecialPointer(page);
		raw  = (RoaringPendingEntry *)   PageGetContents(page);

		if (spc->page_type != ROARING_PAGE_PENDING_INSERT)
		{
			UnlockReleaseBuffer(buf);
			ROARING_CHECK_ERROR("pending chain \"%s\" page %u: page_type 0x%02X, "
								"expected PENDING_INSERT",
								label, cur, spc->page_type);
		}
		if (spc->entry_count > ROARING_PENDING_PER_PAGE)
		{
			UnlockReleaseBuffer(buf);
			ROARING_CHECK_ERROR("pending chain \"%s\" page %u: entry_count %u > max %d",
								label, cur, spc->entry_count, ROARING_PENDING_PER_PAGE);
		}

		/* F8: empty page must have sentinel min/max values. */
		if (spc->entry_count == 0 &&
			(spc->value_min != PG_INT64_MAX || spc->value_max != PG_INT64_MIN))
		{
			UnlockReleaseBuffer(buf);
			ROARING_CHECK_ERROR("pending chain \"%s\" page %u: entry_count 0 but "
								"value_min/value_max are not sentinels "
								"(" INT64_FORMAT " / " INT64_FORMAT ")",
								label, cur, spc->value_min, spc->value_max);
		}

		for (k = 0; k < spc->entry_count; k++)
		{
			int64         val  = raw[k].value;
			TransactionId xmin = raw[k].xmin;

			if (val < spc->value_min || val > spc->value_max)
			{
				UnlockReleaseBuffer(buf);
				ROARING_CHECK_ERROR("pending chain \"%s\" page %u: entry %d value "
									INT64_FORMAT " outside page range ["
									INT64_FORMAT ", " INT64_FORMAT "]",
									label, cur, k, val,
									spc->value_min, spc->value_max);
			}
			if (TransactionIdIsValid(xmin) &&
				TransactionIdIsValid(spc->xmin_low) &&
				TransactionIdPrecedes(xmin, spc->xmin_low))
			{
				UnlockReleaseBuffer(buf);
				ROARING_CHECK_ERROR("pending chain \"%s\" page %u: entry %d xmin %u "
									"is older than page xmin_low %u",
									label, cur, k, xmin, spc->xmin_low);
			}
		}

		cur = spc->next_page;
		UnlockReleaseBuffer(buf);
	}
}

/* ----------------------------------------------------------------
 * check_free_list
 *
 * Walk the free-page list.  Verify:
 *   - every page within range
 *   - page_type == ROARING_PAGE_FREE on each page
 *   - no cycle (capped at nblocks steps)
 * ---------------------------------------------------------------- */
static void
check_free_list(Relation index, BlockNumber head, BlockNumber nblocks)
{
	BlockNumber cur   = head;
	uint32      steps = 0;

	while (cur != InvalidBlockNumber)
	{
		Buffer				buf;
		RoaringLeafSpecial *spc;
		BlockNumber			next;

		if (++steps > nblocks)
			ROARING_CHECK_ERROR("free list has more pages than the relation "
								"(%u); possible cycle", nblocks);

		CHECK_FOR_INTERRUPTS();

		if (cur >= nblocks)
			ROARING_CHECK_ERROR("free list: page %u is out of range", cur);

		buf  = ReadBuffer(index, cur);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		spc  = (RoaringLeafSpecial *) PageGetSpecialPointer(BufferGetPage(buf));

		if (spc->page_type != ROARING_PAGE_FREE)
		{
			uint8 got = spc->page_type;
			UnlockReleaseBuffer(buf);
			ROARING_CHECK_ERROR("free list page %u: page_type 0x%02X, expected FREE",
								cur, got);
		}

		next = ((RoaringFreeSpecial *) spc)->next_free;
		UnlockReleaseBuffer(buf);
		cur = next;
	}
}

/* ----------------------------------------------------------------
 * roaring_index_check
 *
 * SQL-callable entry point.  Accepts a regclass and an optional
 * heapallindexed boolean (accepted for API compatibility; heap
 * cross-checking is not yet implemented).
 * ---------------------------------------------------------------- */
Datum
roaring_index_check(PG_FUNCTION_ARGS)
{
	Oid					 indexoid       = PG_GETARG_OID(0);
	bool				 heapallindexed = PG_GETARG_BOOL(1);
	Relation			 index;
	Buffer				 metabuf;
	RoaringMetaPageData *meta;
	RoaringMetaPageData  meta_copy;
	BlockNumber			 nblocks;
	DeferredOvf			*ovf;
	int					  novf;
	BlockNumber			  last_leaf;
	int					  s;

	if (heapallindexed)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("pg_roaring_index: heapallindexed is not yet implemented"),
				 errhint("Call roaring_index_check(index) without heapallindexed, "
						 "or pass heapallindexed => false.")));

	index   = relation_open(indexoid, AccessShareLock);
	nblocks = RelationGetNumberOfBlocks(index);

	if (nblocks == 0)
	{
		relation_close(index, AccessShareLock);
		PG_RETURN_VOID();
	}

	/* ---- Metapage ---- */
	metabuf = ReadBuffer(index, ROARING_METAPAGE_BLKNO);
	LockBuffer(metabuf, BUFFER_LOCK_SHARE);
	meta    = RoaringPageGetMeta(BufferGetPage(metabuf));

	check_metapage(index, meta, nblocks);

	/* Copy metapage fields we need after releasing the buffer. */
	meta_copy = *meta;
	UnlockReleaseBuffer(metabuf);

	/* ---- Directory ---- */
	check_directory(index, meta_copy.root_directory_page, nblocks);

	/* ---- Leaf chain ---- */
	last_leaf = check_leaf_chain(index, meta_copy.leftmost_leaf_page,
								 nblocks, &ovf, &novf);

	if (last_leaf != meta_copy.rightmost_leaf_page)
		ROARING_CHECK_ERROR("leaf chain ends at block %u, but metapage says "
							"rightmost_leaf_page is %u",
							last_leaf, meta_copy.rightmost_leaf_page);

	/* ---- Overflow chains (deferred second pass) ---- */
	check_overflow_chains(index, ovf, novf, nblocks);
	pfree(ovf);

	/* ---- Pending chains (all shards, insert and merging heads) ---- */
	for (s = 0; s < ROARING_PENDING_SHARDS; s++)
	{
		char label[32];

		if (meta_copy.shards[s].insert_head != InvalidBlockNumber)
		{
			snprintf(label, sizeof(label), "shard%d/insert", s);
			check_pending_chain(index, meta_copy.shards[s].insert_head,
								label, nblocks);
		}
		if (meta_copy.shards[s].merging_head != InvalidBlockNumber)
		{
			snprintf(label, sizeof(label), "shard%d/merging", s);
			check_pending_chain(index, meta_copy.shards[s].merging_head,
								label, nblocks);
		}
	}

	/* ---- Free list (F3) ---- */
	if (meta_copy.free_list_head != InvalidBlockNumber)
		check_free_list(index, meta_copy.free_list_head, nblocks);

	relation_close(index, AccessShareLock);
	PG_RETURN_VOID();
}
