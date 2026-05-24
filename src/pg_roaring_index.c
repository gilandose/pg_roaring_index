#include "pg_roaring_index.h"

#include "access/amapi.h"
#include "access/reloptions.h"
#include "catalog/pg_am.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_type_d.h"
#include "commands/vacuum.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(roaring_handler);
PG_FUNCTION_INFO_V1(roaring_page_handler);

int roaring_pending_merge_threshold_guc = ROARING_PENDING_MERGE_THRESHOLD;

void
_PG_init(void)
{
	DefineCustomIntVariable(
		"roaring.pending_merge_threshold",
		"Number of pending entries before triggering a background merge.",
		"Applied at index creation time; stored in the metapage.",
		&roaring_pending_merge_threshold_guc,
		ROARING_PENDING_MERGE_THRESHOLD,
		1000,
		10000000,
		PGC_USERSET,
		0,
		NULL, NULL, NULL
	);
	MarkGUCPrefixReserved("roaring");
}

/*
 * roaring_validate
 *
 * Validate an operator class for use with this AM.  Called by CREATE INDEX.
 * Checks:
 *   1. opcintype is in the supported set.
 *   2. The opfamily has a strategy-1 (equality) entry with
 *      amoplefttype = amoprighttype = opcintype.
 *   3. That operator's return type is bool.
 */
bool
roaring_validate(Oid opclassoid)
{
    HeapTuple        ht;
    Form_pg_opclass  opcform;
    Oid              opcintype;
    Oid              opfamilyoid;
    CatCList        *catlist;
    int              i;
    bool             found_equality;

    ht = SearchSysCache1(CLAOID, ObjectIdGetDatum(opclassoid));
    if (!HeapTupleIsValid(ht))
        elog(ERROR, "cache lookup failed for operator class %u", opclassoid);
    opcform     = (Form_pg_opclass) GETSTRUCT(ht);
    opcintype   = opcform->opcintype;
    opfamilyoid = opcform->opcfamily;
    ReleaseSysCache(ht);

    if (opcintype != INT8OID  && opcintype != INT4OID  &&
        opcintype != INT2OID  && opcintype != BOOLOID  &&
        opcintype != DATEOID  && opcintype != FLOAT4OID &&
        opcintype != OIDOID   && opcintype != TEXTOID   &&
        opcintype != UUIDOID)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
                 errmsg("roaring index operator class must use a supported type"),
                 errdetail("Type %s is not supported.  Supported types: "
                           "bigint, integer, smallint, boolean, date, real, oid, "
                           "text, uuid (text/uuid use hash keys — "
                           "hash collisions produce false-positive rechecks, not wrong results).",
                           format_type_be(opcintype))));

    /*
     * Verify that strategy 1 (equality) exists for opcintype in the opfamily,
     * and that its operator returns boolean.
     */
    found_equality = false;
    catlist = SearchSysCacheList1(AMOPSTRATEGY,
                                   ObjectIdGetDatum(opfamilyoid));
    for (i = 0; i < catlist->n_members; i++)
    {
        Form_pg_amop amopform;

        ht       = &catlist->members[i]->tuple;
        amopform = (Form_pg_amop) GETSTRUCT(ht);

        if (amopform->amopstrategy != 1)
            continue;
        if (amopform->amoplefttype  != opcintype)
            continue;
        if (amopform->amoprighttype != opcintype)
            continue;

        if (get_op_rettype(amopform->amopopr) != BOOLOID)
        {
            ReleaseSysCacheList(catlist);
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
                     errmsg("roaring: strategy 1 operator for type %s "
                            "must return boolean",
                            format_type_be(opcintype))));
        }

        found_equality = true;
        break;
    }
    ReleaseSysCacheList(catlist);

    if (!found_equality)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
                 errmsg("roaring index operator class for type %s "
                        "does not define a strategy 1 (equality) operator",
                        format_type_be(opcintype))));

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
    amroutine->amcanmulticol  = true;
    amroutine->amoptionalkey  = true;   /* any column subset is valid */
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
 * linearized TIDs.  Index is 10-100x smaller than the exact variant at
 * low cardinality; best for multi-column AND queries.
 *
 * Recheck: roaring_getbitmap_lossy calls tbm_add_page(), which causes
 * PG's TIDBitmap to mark every returned page as lossy — the executor
 * rechecks each heap tuple automatically.  amrecheck (IndexAmRoutine)
 * controls per-tuple recheck for exact-mode bitmaps only; it plays no
 * role in the lossy recheck path.
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
    amroutine->amcanmulticol  = true;
    amroutine->amoptionalkey  = true;   /* any column subset is valid */
    amroutine->amsearcharray  = true;
    amroutine->amsearchnulls  = false;
    amroutine->amstorage      = false;
    amroutine->amclusterable  = false;
    amroutine->ampredlocks    = false;
    amroutine->amcanparallel  = false;
    amroutine->amcaninclude   = false;
    amroutine->amusemaintenanceworkmem = false;
    amroutine->amsummarizing  = true;   /* stores block-granularity summaries */
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
