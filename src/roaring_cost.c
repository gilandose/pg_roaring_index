#include "pg_roaring_index.h"

#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"

/*
 * roaring_costestimate
 *
 * Read exact cardinality from the leaf entry header (no bitmap deserialization)
 * and report perfect selectivity to the planner.
 *
 * TODO (Phase 1, step 1.6): look up the scan key value in the directory,
 * read the leaf entry, extract cardinality field, compute:
 *
 *   selectivity     = cardinality / RelationGetNumberOfBlocks(heap)
 *   indexTotalCost  = cardinality * seq_page_cost
 *   indexStartupCost = 0.01  (one cached directory read)
 */
void
roaring_costestimate(struct PlannerInfo *root,
                     struct IndexPath *path,
                     double loop_count,
                     Cost *indexStartupCost,
                     Cost *indexTotalCost,
                     Selectivity *indexSelectivity,
                     double *indexCorrelation,
                     double *indexPages)
{
    *indexStartupCost  = 0.0;
    *indexTotalCost    = 0.0;
    *indexSelectivity  = 1.0;   /* conservative until real cardinality is wired up */
    *indexCorrelation  = 0.0;
    *indexPages        = 1.0;
}
