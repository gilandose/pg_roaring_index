#ifndef PG_ROARING_INDEX_H
#define PG_ROARING_INDEX_H

#include "postgres.h"
#include "access/amapi.h"       /* file-scope struct PlannerInfo/IndexPath forward decls */
#include "access/genam.h"
#include "access/generic_xlog.h"
#include "storage/bufmgr.h"
#include "storage/itemptr.h"
#include "utils/relcache.h"

#ifdef USE_CROARING
/* CRoaring uses C99 declarations-after-statements in inline functions. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeclaration-after-statement"
#include "roaring.h"
#pragma GCC diagnostic pop
#endif

/* ----------
 * On-disk constants
 * ---------- */
#define ROARING_MAGIC               UINT32_C(0x524F4152)  /* "ROAR" */
#define ROARING_INDEX_VERSION       3   /* T65: added payload directory for INCLUDE column storage */

/*
 * Expected CRoaring major version stored in the metapage.  If the index was
 * built with a different major version — which may have a different portable
 * serialisation format — the index must be rebuilt with REINDEX.
 */
#define ROARING_EXPECTED_FORMAT_VERSION  ((uint16) ROARING_VERSION_MAJOR)

/* Page type tags (stored in special->page_type) */
#define ROARING_PAGE_META           0x01
#define ROARING_PAGE_DIRECTORY      0x02
#define ROARING_PAGE_LEAF           0x03
#define ROARING_PAGE_OVERFLOW       0x04
#define ROARING_PAGE_PENDING_INSERT 0x05
#define ROARING_PAGE_FREE           0x06    /* recycled page on the free list */
#define ROARING_PAGE_PAYLOAD        0x07    /* dense int64 array for INCLUDE column values */
#define ROARING_PAGE_PAYLOAD_DIR    0x08    /* directory of payload pages (2-level) */

/* Metapage flags */
#define ROARING_FLAG_EXACT              0x01
/*
 * ROARING_FLAG_BGMERGE_SPAWNED: set (under metapage EX, in the same WAL record
 * as the page extension that crossed pending_merge_threshold) when a background
 * merge worker has been spawned for this index.  Cleared by roaring_merge_pending
 * Step D when the merge commits.  Allows roaring_pending_append to skip the
 * inline back-pressure merge while the worker is running, eliminating
 * multi-second INSERT latency spikes.  Hard cap (2× threshold) forces an
 * inline fallback if the worker is too slow or crashed.
 */
#define ROARING_FLAG_BGMERGE_SPAWNED    0x04

/* Leaf entry flags */
#define ROARING_ENTRY_INLINE        0x00
#define ROARING_ENTRY_OVERFLOW      0x01

/* Capacities derived from 8KB page size */
#define ROARING_PAGE_SIZE           BLCKSZ
#define ROARING_DIR_ENTRY_SIZE      16      /* int64 high_key + BlockNumber + 4-byte pad */
#define ROARING_PENDING_ENTRY_SIZE  24      /* int64 + uint64 + TransactionId + pad */
#define ROARING_PENDING_PER_PAGE    339     /* (8192-24-32)/24; special is 32 bytes */

/*
 * Overflow pages: bitmap bytes that don't fit on a single leaf page are
 * chained via ROARING_PAGE_OVERFLOW pages.  All serialized bytes go to the
 * chain; the leaf-side RoaringOverflowEntry holds only the header metadata.
 */
/* Usable data area per overflow page */
#define ROARING_OVERFLOW_PAGE_CAP \
    ((Size)(BLCKSZ - SizeOfPageHeaderData \
            - MAXALIGN(sizeof(RoaringOverflowSpecial))))

/* Pending list merge threshold (entries); override via storage param later */
#define ROARING_PENDING_MERGE_THRESHOLD 10000

/*
 * Number of independent pending-list shards.  Writers pick a shard by
 * ROARING_VALUE_SHARD(value) and compete only with other writers that hash
 * to the same shard.  Must be a power-of-two so that modulo is a bitmask.
 * Changing this constant requires a REINDEX (on-disk version bump covers it).
 */
#define ROARING_PENDING_SHARDS      8
#define ROARING_SHARD_MASK          (ROARING_PENDING_SHARDS - 1)

/*
 * Shard selection: XOR the high and low 32 bits of the int64 key so that
 * multi-column keys (attno packed in the high 32 bits) and sequential
 * values both distribute across shards.
 */
#define ROARING_VALUE_SHARD(value) \
    ((int) (((uint32)((uint64)(value) >> 32) ^ (uint32)(uint64)(value)) \
            & ROARING_SHARD_MASK))

/*
 * Per-column key encoding for multi-column indexes (natts > 1).
 *
 * Key layout: high 32 bits = attno (1-indexed column number),
 *             low  32 bits = (uint32)(int32)column_value.
 *
 * Each column is independently stored with its own set of bitmaps, namespaced
 * by attno.  A multi-column AND query collects one bitmap per scan key and
 * intersects them inside the AM before returning a TIDBitmap to the executor.
 *
 * This encoding supports up to INT32_MAX columns and any int4 value per column.
 * Single-column indexes (natts == 1) do NOT use this encoding — they store the
 * raw int8/int4 value directly, preserving backward compatibility.
 *
 * Multi-type extension note: for non-integer column types, the low 32 bits
 * would carry a type-specific integer representation (float4 bit pattern,
 * hash, etc.).  The attno prefix keeps columns independent regardless of type.
 */
#define ROARING_COL_KEY(attno, val32) \
    (((int64)(attno) << 32) | (uint64)(uint32)(int32)(val32))
#define ROARING_COL_KEY_RANGE_LO(attno) \
    ((int64)(attno) << 32)
#define ROARING_COL_KEY_RANGE_HI(attno) \
    (((int64)(attno) << 32) | INT64_C(0x00000000FFFFFFFF))

/* Fixed block number for the metapage */
#define ROARING_METAPAGE_BLKNO          0

/* ----------
 * On-disk page structures
 * ---------- */

/*
 * Per-shard state stored in the metapage.  Each shard is an independent
 * pending-insert chain; writers pick a shard via ROARING_VALUE_SHARD() and
 * compete only with other writers mapping to the same shard.
 *
 * insert_head / insert_tail / insert_count: the live append chain.
 * merging_head: set at merge start; acts as a mutex (second merger bails out).
 *               Concurrent scans must walk both insert_head and merging_head.
 * carry_head:   Case-2 crash-recovery anchor (set in Step B, cleared in Step C).
 */
typedef struct RoaringPendingShard
{
    BlockNumber insert_head;
    BlockNumber insert_tail;
    uint32      insert_count;   /* updated on page extension; approximate between */
    BlockNumber merging_head;
    BlockNumber carry_head;
} RoaringPendingShard;          /* 20 bytes */

typedef struct RoaringMetaPageData
{
    uint32      magic;
    uint16      version;
    uint16      flags;
    uint16      croaring_format_version;  /* ROARING_EXPECTED_FORMAT_VERSION */
    uint8       num_shards;              /* = ROARING_PENDING_SHARDS; validated on open */
    uint8       _reserved;

    BlockNumber root_directory_page;
    BlockNumber leftmost_leaf_page;
    BlockNumber rightmost_leaf_page;
    BlockNumber free_list_head;         /* head of recycled-page free list */

    /* Per-shard pending-list state (ROARING_PENDING_SHARDS entries) */
    RoaringPendingShard shards[ROARING_PENDING_SHARDS];

    /* Statistics (updated on merge) */
    uint32      total_entries;
    uint32      pending_merge_threshold;

    /* T65: head of the 2-level payload directory (InvalidBlockNumber if no INCLUDE columns) */
    BlockNumber payload_dir_head;
} RoaringMetaPageData;

#define RoaringPageGetMeta(page) \
    ((RoaringMetaPageData *) PageGetContents(page))

typedef struct RoaringDirEntry
{
    int64       high_key;
    BlockNumber child_page;
} RoaringDirEntry;  /* must be ROARING_DIR_ENTRY_SIZE bytes */
StaticAssertDecl(sizeof(RoaringDirEntry) == ROARING_DIR_ENTRY_SIZE,
				 "RoaringDirEntry size mismatch — update ROARING_DIR_ENTRY_SIZE");

typedef struct RoaringDirSpecial
{
    uint8       page_type;  /* ROARING_PAGE_DIRECTORY */
    uint8       level;      /* 0 = root directory, 1 = level 1, ... */
    uint16      entry_count;
    BlockNumber right_page;
    uint32      reserved;
} RoaringDirSpecial;    /* 16 bytes, fits in pd_special */

/*
 * Inline leaf entry: header immediately followed by bitmap_data bytes.
 * Total size varies; accessed via line pointer.
 */
typedef struct RoaringLeafEntry
{
    int64       value;
    uint32      cardinality;
    uint8       flags;      /* ROARING_ENTRY_INLINE or ROARING_ENTRY_OVERFLOW */
    /* bitmap_data[bitmap_len] follows for INLINE entries */
} RoaringLeafEntry;

/*
 * Overflow leaf entry: same header layout, different tail.
 * flags == ROARING_ENTRY_OVERFLOW.
 */
typedef struct RoaringOverflowEntry
{
    int64       value;
    uint32      cardinality;
    uint8       flags;          /* ROARING_ENTRY_OVERFLOW */
    BlockNumber overflow_blkno;
    uint32      total_len;
} RoaringOverflowEntry;

#define RoaringLeafEntryDataOffset  \
    (offsetof(RoaringLeafEntry, flags) + sizeof(uint8))

typedef struct RoaringLeafSpecial
{
    uint8       page_type;  /* ROARING_PAGE_LEAF */
    uint8       flags;
    uint16      entry_count;
    BlockNumber left_page;
    BlockNumber right_page;
} RoaringLeafSpecial;   /* 16 bytes */

/*
 * Fixed-size pending entry — 24 bytes, 339 per page.
 * Linearization: ((uint64)blkno << 9) | (offset - 1)
 *   9 bits for offset: up to 511 tuples/page (safe for default 8KB pages)
 *   55 bits for blkno: full uint32 block space + 23 spare bits (32 TiB table)
 *   roaring64 shard key = blkno >> 23 (one shard per 64 GiB range)
 *   roaring32 container key = (blkno & 0x7FFFFF) >> 7: 128 blocks per container
 * Reverse: blkno = (BlockNumber)(linear_tid >> 9);
 *          offset = (OffsetNumber)((linear_tid & 0x1FF) + 1)
 *
 * Exact mode uses roaring64_bitmap_t; lossy mode keeps roaring_bitmap_t (uint32
 * block numbers fit comfortably in roaring32).
 */
typedef struct RoaringPendingEntry
{
    int64           value;
    uint64          linear_tid;     /* ((uint64)blkno << 9) | (offset - 1) */
    TransactionId   xmin;
    uint32          _pad;           /* keep sizeof == 24 and array alignment happy */
} RoaringPendingEntry;

StaticAssertDecl(sizeof(RoaringPendingEntry) == ROARING_PENDING_ENTRY_SIZE,
                 "RoaringPendingEntry must be 24 bytes");
StaticAssertDecl(MaxHeapTuplesPerPage <= 511,
                 "offset field too narrow — rebuild with larger BLCKSZ or extend linearization");

typedef struct RoaringPendingSpecial
{
    uint8           page_type;  /* ROARING_PAGE_PENDING_INSERT/DELETE */
    uint8           flags;
    uint16          entry_count;
    BlockNumber     next_page;
    TransactionId   xmin_low;   /* earliest xmin on this page */
    uint32          _pad;       /* align int64 fields */
    int64           value_min;  /* PG_INT64_MAX when empty */
    int64           value_max;  /* PG_INT64_MIN when empty */
} RoaringPendingSpecial; /* 32 bytes */

typedef struct RoaringOverflowSpecial
{
    uint8       page_type;  /* ROARING_PAGE_OVERFLOW */
    uint8       flags;
    uint16      sequence;
    BlockNumber next_page;
    BlockNumber owner_page;
} RoaringOverflowSpecial;   /* 16 bytes */

/*
 * RoaringFreeSpecial — special area for a recycled page on the free list.
 *
 * next_free sits at the same byte offset as RoaringPendingSpecial.next_page
 * (offset 4), so we can read the link using either struct.  When a page is
 * popped from the free list, the caller overwrites it entirely via
 * GENERIC_XLOG_FULL_IMAGE before the WAL record commits.
 */
typedef struct RoaringFreeSpecial
{
    uint8       page_type;  /* ROARING_PAGE_FREE */
    uint8       _pad[3];
    BlockNumber next_free;  /* next page on free list, or InvalidBlockNumber */
} RoaringFreeSpecial;   /* 8 bytes — fits inside any existing special area */

/*
 * T65: Payload page — dense array of int64 PK values indexed by
 * (linear_tid % ROARING_PAYLOAD_PK_PER_PAGE).
 * Capacity: (BLCKSZ - SizeOfPageHeaderData - special) / 8 = 1020 entries.
 */
typedef struct RoaringPayloadSpecial
{
    uint8       page_type;      /* ROARING_PAGE_PAYLOAD */
    uint8       flags;
    uint16      entry_count;    /* highest index written + 1 */
    uint32      reserved;
} RoaringPayloadSpecial;    /* 8 bytes */

#define ROARING_PAYLOAD_PK_PER_PAGE     1020    /* (8192 - 24 - 8) / 8 */

/*
 * T65: Payload directory page — dense array of BlockNumbers.
 * 2-level tree: root dir (level=1) → leaf dir (level=0) → payload pages.
 * Capacity per dir page: (BLCKSZ - SizeOfPageHeaderData - special) / 4 = 2040 entries.
 */
typedef struct RoaringPayloadDirSpecial
{
    uint8       page_type;      /* ROARING_PAGE_PAYLOAD_DIR */
    uint8       level;          /* 1 = root, 0 = leaf */
    uint16      entry_count;    /* number of populated entries */
    uint32      reserved;
} RoaringPayloadDirSpecial; /* 8 bytes */

#define ROARING_PAYLOAD_DIR_ENTRIES_PER_PAGE    2040    /* (8192 - 24 - 8) / 4 */

/* ----------
 * Inline helpers
 * ---------- */

/*
 * roaring_float4_to_key32 — map a float4 to a stable int32 equality key.
 *
 * Uses the raw IEEE 754 bit pattern, with one adjustment: both +0.0 and -0.0
 * map to the same key (0), matching float4eq semantics.  NaN values are NOT
 * handled here — callers must skip NaN before calling this function.
 */
static inline int32
roaring_float4_to_key32(float4 f)
{
	uint32 bits;

	memcpy(&bits, &f, sizeof(bits));
	/* ±0.0 → same key (clear sign bit check on the remaining 31 bits) */
	if ((bits & UINT32_C(0x7FFFFFFF)) == 0)
		return 0;
	return (int32) bits;
}


/*
 * roaring64_cardinality32 — return a roaring64 bitmap's cardinality as uint32.
 * Used by the exact path.  Asserts the count fits in uint32; the stored leaf
 * cardinality field is uint32 and is used only for cost estimation, so an
 * assertion failure indicates a pathologically large value (> 4B matching rows
 * for a single indexed value), not a correctness concern.
 */
static inline uint32
roaring64_cardinality32(const roaring64_bitmap_t *bm)
{
	uint64_t card = roaring64_bitmap_get_cardinality(bm);

	if (card > (uint64_t) UINT32_MAX)
		elog(ERROR, "roaring bitmap cardinality exceeds uint32 max");
	return (uint32) card;
}

/* ----------
 * Per-relation AM cache (rd_amcache)
 *
 * Cached on first metapage read; stale check via meta_lsn using
 * BufferGetLSNAtomic (no content lock needed).  Allows costestimate to skip
 * the buffer read on repeated planning calls, and getbitmap to skip the
 * metapage read entirely when pending is known empty.
 * ---------- */
typedef struct RoaringAmCache
{
    BlockNumber root_blkno;
    uint32      total_entries;
    XLogRecPtr  meta_lsn;
    uint32      pending_count_approx;   /* advisory: sum of insert_counts; stale by up to ROARING_PENDING_PER_PAGE-1 inserts */
    /* Per-shard head pointers so scans can walk all chains without re-reading meta */
    BlockNumber insert_head[ROARING_PENDING_SHARDS];
    BlockNumber merging_head[ROARING_PENDING_SHARDS];
} RoaringAmCache;

/* ----------
 * Scan state
 * ---------- */
typedef struct RoaringScanOpaque
{
    bool    bitmap_loaded;
    bool    needs_recheck;  /* true when any index column uses a hash-keyed type */

    /* amgettuple (IndexScan / IndexOnlyScan) state — exact mode only */
    roaring64_bitmap_t   *scan_bm;    /* combined result bitmap; NULL until first gettuple */
    roaring64_iterator_t *scan_iter;  /* iterator over scan_bm */
} RoaringScanOpaque;

/* ----------
 * Function prototypes
 * ---------- */

/* roaring_util.c */
extern void   roaring_validate_metapage(Relation index,
										const RoaringMetaPageData *meta);
extern Buffer roaring_extend_page(Relation index);
extern Buffer roaring_alloc_page(Relation index, Buffer metabuf,
								 BlockNumber *new_free_list_head_out);
extern void   roaring_wal_and_release(Relation index, Buffer buf);
extern BlockNumber roaring_init_pending_page(Relation index, Buffer metabuf,
											  BlockNumber *new_free_head_out,
											  uint8 page_type);
extern BlockNumber roaring_write_dir_page(Relation index,
                                           RoaringDirEntry *entries,
                                           uint32 count, uint8 level);
extern BlockNumber roaring_write_overflow_chain(Relation index,
                                                const char *data,
                                                size_t total_len);
extern roaring64_bitmap_t *roaring_read_overflow_bitmap(
    Relation index, const RoaringOverflowEntry *oe);

/*
 * roaring_datum_to_key32 — convert a Datum of supported type to an int32
 * index key for use in the low 32 bits of ROARING_COL_KEY() (multi-column).
 *
 * roaring_datum_to_key64 — convert a Datum of supported type to an int64
 * index key for single-column indexes.
 *
 * Supported types: INT8, INT4, INT2, BOOL, DATE, FLOAT4, OID.
 * Callers must skip NULL datums before calling these functions.
 * For FLOAT4: callers must also skip NaN (use roaring_float4_to_key32 guard).
 */
extern int32  roaring_datum_to_key32(Datum d, Oid typid);
extern int64  roaring_datum_to_key64(Datum d, Oid typid);
extern bool   roaring_pending_visible(TransactionId xmin, Snapshot snapshot);

/* roaring_vacuum.c — also called from roaring_insert for back-pressure */
extern void roaring_merge_pending(Relation index);

/* roaring_bgworker.c */
extern PGDLLEXPORT void roaring_bgworker_main(Datum main_arg);
extern void roaring_try_spawn_merge_worker(Relation index);

/* roaring_build.c */
extern IndexBuildResult *roaring_build(Relation heap, Relation index,
                                       struct IndexInfo *indexInfo);
extern void roaring_buildempty(Relation index);

/* roaring_insert.c */
extern bool roaring_insert(Relation index, Datum *values, bool *isnull,
                           ItemPointer ht_ctid, Relation heapRel,
                           IndexUniqueCheck checkUnique,
                           bool indexUnchanged,
                           struct IndexInfo *indexInfo);

/* roaring_scan.c */
extern BlockNumber   roaring_dir_lookup(Relation index, BlockNumber dir_blkno,
                                        int64 value);
extern IndexScanDesc roaring_beginscan(Relation rel, int nkeys, int norderbys);
extern void roaring_rescan(IndexScanDesc scan, ScanKey keys, int nkeys,
                           ScanKey orderbys, int norderbys);
extern int64 roaring_getbitmap(IndexScanDesc scan, TIDBitmap *tbm);
extern bool  roaring_gettuple(IndexScanDesc scan, ScanDirection dir);
extern void  roaring_endscan(IndexScanDesc scan);
extern int64 roaring_count_key_exact(Relation index, BlockNumber root_blkno,
                                     const int64 *keys, int nkeys,
                                     const BlockNumber *insert_heads,
                                     const BlockNumber *merging_heads,
                                     Snapshot snapshot);
extern roaring64_bitmap_t *roaring_build_bitmap_exact(Relation index, BlockNumber root_blkno,
                                                      const int64 *keys, int nkeys,
                                                      const BlockNumber *insert_heads,
                                                      const BlockNumber *merging_heads,
                                                      Snapshot snapshot);

/* roaring_vacuum.c */
extern IndexBulkDeleteResult *roaring_bulkdelete(IndexVacuumInfo *info,
                                                  IndexBulkDeleteResult *stats,
                                                  IndexBulkDeleteCallback callback,
                                                  void *callback_state);
extern IndexBulkDeleteResult *roaring_vacuumcleanup(IndexVacuumInfo *info,
                                                     IndexBulkDeleteResult *stats);

/* roaring_cost.c */
extern void roaring_costestimate(struct PlannerInfo *root,
                                  struct IndexPath *path,
                                  double loop_count,
                                  Cost *indexStartupCost,
                                  Cost *indexTotalCost,
                                  Selectivity *indexSelectivity,
                                  double *indexCorrelation,
                                  double *indexPages);

/* roaring_validate.c (in pg_roaring_index.c for now) */
extern bool roaring_validate(Oid opclassoid);

/* GUC: roaring.pending_merge_threshold (defined in pg_roaring_index.c) */
extern int roaring_pending_merge_threshold_guc;

/* roaring_payload.c */
extern void  roaring_payload_insert(Relation index, uint64 linear_tid, int64 pk);
extern int64 roaring_payload_fetch(Relation index, uint64 linear_tid);

/* roaring_customscan.c */
extern void roaring_customscan_init(void);

/* roaring_stats.c */
extern Datum roaring_index_stats(struct FunctionCallInfoBaseData *fcinfo);

/* roaring_check.c */
extern Datum roaring_index_check(struct FunctionCallInfoBaseData *fcinfo);

#endif /* PG_ROARING_INDEX_H */
