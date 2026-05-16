#include "pg_roaring_index.h"

/*
 * roaring_bulkdelete
 *
 * Exact mode: append dead TIDs to pending delete list (tombstone path).
 * Lossy mode: no-op (stale page references accumulate; recheck handles them).
 *
 * Modeled on brinbulkdelete which is also a no-op — BRIN precedent.
 */
IndexBulkDeleteResult *
roaring_bulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
                   IndexBulkDeleteCallback callback, void *callback_state)
{
    if (stats == NULL)
        stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));

    /* TODO (Phase 1, step 1.3): walk pending insert list, filter aborted xmins;
     * append dead TIDs to pending delete list for tombstone merge. */

    return stats;
}

/*
 * roaring_vacuumcleanup
 *
 * Called after heap VACUUM.  Responsibilities:
 *   1. Merge pending insert list into main leaf pages.
 *   2. Merge pending delete list into tombstone leaf pages (exact mode).
 *   3. Compact entries where tombstone size > threshold (exact mode).
 *   4. Update metapage statistics.
 */
IndexBulkDeleteResult *
roaring_vacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
    if (stats == NULL)
        stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));

    /* TODO (Phase 1, step 1.4): implement merge + compaction. */

    return stats;
}
