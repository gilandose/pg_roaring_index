#include "pg_roaring_index.h"

#include "access/relscan.h"

IndexScanDesc
roaring_beginscan(Relation rel, int nkeys, int norderbys)
{
    IndexScanDesc scan;
    RoaringScanOpaque *so;

    scan = RelationGetIndexScan(rel, nkeys, norderbys);

    so = (RoaringScanOpaque *) palloc0(sizeof(RoaringScanOpaque));
    so->bitmap_loaded = false;

    scan->opaque = so;
    return scan;
}

void
roaring_rescan(IndexScanDesc scan, ScanKey keys, int nkeys,
               ScanKey orderbys, int norderbys)
{
    RoaringScanOpaque *so = (RoaringScanOpaque *) scan->opaque;

    so->bitmap_loaded = false;

    if (keys && scan->numberOfKeys > 0)
        memmove(scan->keyData, keys, scan->numberOfKeys * sizeof(ScanKeyData));
}

int64
roaring_getbitmap(IndexScanDesc scan, TIDBitmap *tbm)
{
    elog(ERROR, "roaring_getbitmap: not yet implemented");
    return 0;
}

void
roaring_endscan(IndexScanDesc scan)
{
    pfree(scan->opaque);
    scan->opaque = NULL;
}
