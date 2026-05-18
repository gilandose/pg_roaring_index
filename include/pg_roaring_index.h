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
#define ROARING_DIR_ENTRY_SIZE      12      /* int64 high_key + BlockNumber */
#define ROARING_PENDING_ENTRY_SIZE  24      /* int64 + uint64 + TransactionId + pad */
#define ROARING_PENDING_PER_PAGE    339     /* (8192-24-16)/24 */

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
 * Fixed-size pending entry — 24 bytes, 339 per page.
 * linear_tid = (BlockNumber << 16) | OffsetNumber; needs uint64 since
 * BlockNumber alone can be up to 2^32-1 (doesn't fit with offset in uint32).
 */
typedef struct RoaringPendingEntry
{
    int64           value;
    uint64          linear_tid;     /* (block << 16) | offset */
    TransactionId   xmin;
    uint32          reserved;       /* explicit padding */
} RoaringPendingEntry;

StaticAssertDecl(sizeof(RoaringPendingEntry) == ROARING_PENDING_ENTRY_SIZE,
                 "RoaringPendingEntry must be 24 bytes");

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
