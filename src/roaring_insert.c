#include "pg_roaring_index.h"

#include "access/generic_xlog.h"

bool
roaring_insert(Relation index, Datum *values, bool *isnull,
               ItemPointer ht_ctid, Relation heapRel,
               IndexUniqueCheck checkUnique,
               bool indexUnchanged,
               struct IndexInfo *indexInfo)
{
    elog(ERROR, "roaring_insert: not yet implemented");
    return false;
}
