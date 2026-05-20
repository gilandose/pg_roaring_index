#include "pg_roaring_index.h"

#include "access/amapi.h"
#include "access/reloptions.h"
#include "catalog/pg_am.h"
#include "commands/vacuum.h"
#include "utils/guc.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(roaring_handler);
PG_FUNCTION_INFO_V1(roaring_page_handler);

/*
 * roaring_validate
 *
 * Validate an operator class for use with this AM.  Called by CREATE INDEX.
 * For now accept any operator class associated with our AM.
 */
bool
roaring_validate(Oid opclassoid)
{
    return true;
}

/*
 * roaring_handler
 *
 * Entry point: returns an IndexAmRoutine describing this AM to the executor.
 */
Datum
roaring_handler(PG_FUNCTION_ARGS)
{
    IndexAmRoutine *amroutine = makeNode(IndexAmRoutine);

    amroutine->amstrategies   = 1;      /* equality only */
    amroutine->amsupport      = 0;
    amroutine->amoptsprocnum  = 0;
    amroutine->amcanorder     = false;
    amroutine->amcanorderbyop = false;
    amroutine->amcanbackward  = false;
    amroutine->amcanunique    = false;
    amroutine->amcanmulticol  = false;
    amroutine->amoptionalkey  = false;
    amroutine->amsearcharray  = true;
    amroutine->amsearchnulls  = false;
    amroutine->amstorage      = false;
    amroutine->amclusterable  = false;
    amroutine->ampredlocks    = false;
    amroutine->amcanparallel  = false;
    amroutine->amcaninclude   = false;
    amroutine->amusemaintenanceworkmem = false;
    amroutine->amparallelvacuumoptions = VACUUM_OPTION_PARALLEL_BULKDEL;
    amroutine->amkeytype      = InvalidOid;

    amroutine->ambuild           = roaring_build;
    amroutine->ambuildempty      = roaring_buildempty;
    amroutine->aminsert          = roaring_insert;
    amroutine->ambulkdelete      = roaring_bulkdelete;
    amroutine->amvacuumcleanup   = roaring_vacuumcleanup;
    amroutine->amcanreturn       = NULL;
    amroutine->amcostestimate    = roaring_costestimate;
    amroutine->amoptions         = NULL;
    amroutine->amproperty        = NULL;
    amroutine->ambuildphasename  = NULL;
    amroutine->amvalidate        = roaring_validate;
    amroutine->amadjustmembers   = NULL;
    amroutine->ambeginscan       = roaring_beginscan;
    amroutine->amrescan          = roaring_rescan;
    amroutine->amgettuple        = NULL; /* bitmap AM: no amgettuple */
    amroutine->amgetbitmap       = roaring_getbitmap;
    amroutine->amendscan         = roaring_endscan;
    amroutine->ammarkpos         = NULL;
    amroutine->amrestrpos        = NULL;
    amroutine->amestimateparallelscan = NULL;
    amroutine->aminitparallelscan     = NULL;
    amroutine->amparallelrescan       = NULL;

    PG_RETURN_POINTER(amroutine);
}

/*
 * roaring_page_handler
 *
 * Lossy (page-level) variant.  Bitmaps store block numbers instead of
 * linearized TIDs.  amrecheck=true tells the executor to recheck every
 * heap tuple on each matched page.  Index is 10-100x smaller than the
 * exact variant at low cardinality; best for multi-column AND queries.
 */
Datum
roaring_page_handler(PG_FUNCTION_ARGS)
{
    IndexAmRoutine *amroutine = makeNode(IndexAmRoutine);

    amroutine->amstrategies   = 1;
    amroutine->amsupport      = 0;
    amroutine->amoptsprocnum  = 0;
    amroutine->amcanorder     = false;
    amroutine->amcanorderbyop = false;
    amroutine->amcanbackward  = false;
    amroutine->amcanunique    = false;
    amroutine->amcanmulticol  = false;
    amroutine->amoptionalkey  = false;
    amroutine->amsearcharray  = true;
    amroutine->amsearchnulls  = false;
    amroutine->amstorage      = false;
    amroutine->amclusterable  = false;
    amroutine->ampredlocks    = false;
    amroutine->amcanparallel  = false;
    amroutine->amcaninclude   = false;
    amroutine->amusemaintenanceworkmem = false;
    amroutine->amparallelvacuumoptions = VACUUM_OPTION_PARALLEL_BULKDEL;
    amroutine->amkeytype      = InvalidOid;

    amroutine->ambuild           = roaring_build_lossy;
    amroutine->ambuildempty      = roaring_buildempty;
    amroutine->aminsert          = roaring_insert_lossy;
    amroutine->ambulkdelete      = roaring_bulkdelete_lossy;
    amroutine->amvacuumcleanup   = roaring_vacuumcleanup;   /* shared */
    amroutine->amcanreturn       = NULL;
    amroutine->amcostestimate    = roaring_costestimate;    /* shared */
    amroutine->amoptions         = NULL;
    amroutine->amproperty        = NULL;
    amroutine->ambuildphasename  = NULL;
    amroutine->amvalidate        = roaring_validate;
    amroutine->amadjustmembers   = NULL;
    amroutine->ambeginscan       = roaring_beginscan;       /* shared */
    amroutine->amrescan          = roaring_rescan;          /* shared */
    amroutine->amgettuple        = NULL;
    amroutine->amgetbitmap       = roaring_getbitmap_lossy;
    amroutine->amendscan         = roaring_endscan;         /* shared */
    amroutine->ammarkpos         = NULL;
    amroutine->amrestrpos        = NULL;
    amroutine->amestimateparallelscan = NULL;
    amroutine->aminitparallelscan     = NULL;
    amroutine->amparallelrescan       = NULL;

    PG_RETURN_POINTER(amroutine);
}
