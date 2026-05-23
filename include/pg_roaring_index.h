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
#define ROARING_INDEX_VERSION       3   /* bumped when on-disk metapage layout changes */

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
#define ROARING_PAGE_PENDING_DELETE 0x06
#define ROARING_PAGE_TOMBSTONE      0x07

/* Metapage flags */
#define ROARING_FLAG_EXACT          0x01
#define ROARING_FLAG_LOSSY          0x02
#define ROARING_FLAG_HAS_TOMBSTONE  0x04

/* Leaf entry flags */
#define ROARING_ENTRY_INLINE        0x00
#define ROARING_ENTRY_OVERFLOW      0x01

/* Capacities derived from 8KB page size */
#define ROARING_PAGE_SIZE           BLCKSZ
#define ROARING_DIR_ENTRY_SIZE      16      /* int64 high_key + BlockNumber + 4-byte pad */
#define ROARING_PENDING_ENTRY_SIZE  16      /* int64 + uint32 + TransactionId */
#define ROARING_PENDING_PER_PAGE    508     /* (8192-24-32)/16; special is 32 bytes */

/*
 * Overflow pages: bitmap bytes that don't fit on a single leaf page are
 * chained via ROARING_PAGE_OVERFLOW pages.  The first ROARING_OVERFLOW_INLINE_BYTES
 * of the serialized bitmap are stored in the RoaringOverflowEntry itself;
 * the remainder occupies the chain.
 */
#define ROARING_OVERFLOW_INLINE_BYTES   64
/* Usable data area per overflow page */
#define ROARING_OVERFLOW_PAGE_CAP \
    ((Size)(BLCKSZ - SizeOfPageHeaderData \
            - MAXALIGN(sizeof(RoaringOverflowSpecial))))

/* Pending list merge threshold (entries); override via storage param later */
#define ROARING_PENDING_MERGE_THRESHOLD 10000

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

typedef struct RoaringMetaPageData
{
    uint32      magic;
    uint16      version;
    uint16      flags;
    uint16      croaring_format_version;  /* ROARING_EXPECTED_FORMAT_VERSION */
    uint16      _reserved;               /* explicit pad; keep next field 4-byte aligned */

    BlockNumber root_directory_page;
    BlockNumber leftmost_leaf_page;
    BlockNumber rightmost_leaf_page;

    BlockNumber pending_insert_head;
    BlockNumber pending_insert_tail;
    uint32      pending_insert_count;

    /*
     * Set to the pending_insert_head at the start of a merge; cleared when
     * the merge commits.  Crash recovery: if non-InvalidBlockNumber, a prior
     * merge was interrupted; re-merge from this chain on next vacuum.
     * Concurrent scans: amgetbitmap must walk both pending_insert_head and
     * pending_merging_head chains to avoid stale-read gaps.
     * Also acts as a mutex: a second merger that sees this set bails out.
     */
    BlockNumber pending_merging_head;

    /*
     * Set atomically with the first carry page in Step B of merge; cleared in
     * Step C when the carry chain becomes the live pending_insert chain.
     * If non-InvalidBlockNumber during Case 1 crash recovery, these carry pages
     * are orphaned and will leak until REINDEX (no FSM yet).
     */
    BlockNumber pending_carry_head;

    BlockNumber pending_delete_head;    /* exact mode */
    BlockNumber pending_delete_tail;
    uint32      pending_delete_count;

    BlockNumber tombstone_root_page;    /* exact mode */

    /* Statistics (updated on merge) */
    uint32      total_entries;
    uint32      total_heap_pages;
    uint32      avg_cardinality;
    uint32      max_cardinality;
    uint32      min_cardinality;
    uint32      pending_merge_threshold;
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
    char        inline_prefix[64];
} RoaringOverflowEntry;

#define RoaringLeafEntryDataOffset  \
    (offsetof(RoaringLeafEntry, flags) + sizeof(uint8))

typedef struct RoaringLeafSpecial
{
    uint8       page_type;  /* ROARING_PAGE_LEAF or ROARING_PAGE_TOMBSTONE */
    uint8       flags;
    uint16      entry_count;
    BlockNumber left_page;
    BlockNumber right_page;
} RoaringLeafSpecial;   /* 16 bytes */

/*
 * Fixed-size pending entry — 16 bytes, 510 per page.
 * Linearization: (blkno << 9) | (offset - 1)
 *   9 bits for offset: up to 511 tuples/page (MaxHeapTuplesPerPage ≈ 255)
 *   23 bits for blkno: up to 2^23 blocks = 64 GiB table
 *   Container key = linear_tid >> 16 = blkno >> 7: 128 blocks per container
 * Reverse: blkno = linear_tid >> 9; offset = (linear_tid & 0x1FF) + 1
 */
typedef struct RoaringPendingEntry
{
    int64           value;
    uint32          linear_tid;     /* (blkno << 9) | (offset - 1) */
    TransactionId   xmin;
} RoaringPendingEntry;

StaticAssertDecl(sizeof(RoaringPendingEntry) == ROARING_PENDING_ENTRY_SIZE,
                 "RoaringPendingEntry must be 16 bytes");
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
 * roaring_cardinality32 — return bitmap cardinality as uint32.
 * Assert it fits: a roaring32 bitmap can theoretically hold all 2^32 values,
 * which is UINT32_MAX+1 and would silently truncate.  In practice our TID
 * encoding limits cardinality well below 2^32, so this fires only on
 * corruption.
 */
static inline uint32
roaring_cardinality32(const roaring_bitmap_t *bm)
{
	uint64_t card = roaring_bitmap_get_cardinality(bm);

	Assert(card <= (uint64_t) UINT32_MAX);
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
    uint32      pending_count;
    BlockNumber merging_head;
} RoaringAmCache;

/* ----------
 * Scan state
 * ---------- */
typedef struct RoaringScanOpaque
{
    bool    bitmap_loaded;
    bool    needs_recheck;  /* true when any index column uses a hash-keyed type */
} RoaringScanOpaque;

/* ----------
 * Function prototypes
 * ---------- */

/* roaring_util.c */
extern void   roaring_validate_metapage(Relation index,
										const RoaringMetaPageData *meta);
extern Buffer roaring_extend_page(Relation index);
extern void   roaring_wal_and_release(Relation index, Buffer buf);
extern BlockNumber roaring_init_pending_page(Relation index, uint8 page_type);
extern BlockNumber roaring_write_dir_page(Relation index,
                                           RoaringDirEntry *entries,
                                           uint32 count, uint8 level);
extern BlockNumber roaring_write_overflow_chain(Relation index,
                                                const char *data,
                                                size_t total_len,
                                                size_t prefix_len);
extern roaring_bitmap_t *roaring_read_overflow_bitmap(
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

/* roaring_vacuum.c — also called from roaring_insert for back-pressure */
extern void roaring_merge_pending(Relation index);

/* roaring_build.c */
extern IndexBuildResult *roaring_build(Relation heap, Relation index,
                                       struct IndexInfo *indexInfo);
extern IndexBuildResult *roaring_build_lossy(Relation heap, Relation index,
                                             struct IndexInfo *indexInfo);
extern void roaring_buildempty(Relation index);

/* roaring_insert.c */
extern bool roaring_insert(Relation index, Datum *values, bool *isnull,
                           ItemPointer ht_ctid, Relation heapRel,
                           IndexUniqueCheck checkUnique,
                           bool indexUnchanged,
                           struct IndexInfo *indexInfo);
extern bool roaring_insert_lossy(Relation index, Datum *values, bool *isnull,
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
extern int64 roaring_getbitmap_lossy(IndexScanDesc scan, TIDBitmap *tbm);
extern void roaring_endscan(IndexScanDesc scan);

/* roaring_vacuum.c */
extern IndexBulkDeleteResult *roaring_bulkdelete(IndexVacuumInfo *info,
                                                  IndexBulkDeleteResult *stats,
                                                  IndexBulkDeleteCallback callback,
                                                  void *callback_state);
extern IndexBulkDeleteResult *roaring_bulkdelete_lossy(IndexVacuumInfo *info,
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

#endif /* PG_ROARING_INDEX_H */
