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
#define ROARING_INDEX_VERSION       1   /* not ROARING_VERSION — clashes with CRoaring */

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
#define ROARING_PENDING_PER_PAGE    510     /* (8192-24-16)/16 */

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
 *   23 bits for blkno: up to 2^23 blocks = 64 TB table
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

typedef struct RoaringPendingSpecial
{
    uint8           page_type;  /* ROARING_PAGE_PENDING_INSERT/DELETE */
    uint8           flags;
    uint16          entry_count;
    BlockNumber     next_page;
    TransactionId   xmin_low;   /* earliest xmin on this page */
} RoaringPendingSpecial; /* 16 bytes */

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
 * Scan state
 * ---------- */
typedef struct RoaringScanOpaque
{
    bool    bitmap_loaded;
} RoaringScanOpaque;

/* ----------
 * Function prototypes
 * ---------- */

/* roaring_util.c */
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

/* roaring_vacuum.c — also called from roaring_insert for back-pressure */
extern void roaring_merge_pending(Relation index);

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
extern void roaring_endscan(IndexScanDesc scan);

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

#endif /* PG_ROARING_INDEX_H */
