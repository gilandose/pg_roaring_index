#include "pg_roaring_index.h"

#include "access/tableam.h"
#include "commands/progress.h"
#include "pgstat.h"

IndexBuildResult *
roaring_build(Relation heap, Relation index, struct IndexInfo *indexInfo)
{
    IndexBuildResult *result;

    result = (IndexBuildResult *) palloc0(sizeof(IndexBuildResult));
    result->heap_tuples  = 0;
    result->index_tuples = 0;

    elog(ERROR, "roaring_build: not yet implemented");

    return result;
}

void
roaring_buildempty(Relation index)
{
    elog(ERROR, "roaring_buildempty: not yet implemented");
}
