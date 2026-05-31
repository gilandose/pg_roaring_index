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
		"Threshold for triggering a background merge of the pending insert list. Read live on each insert; the metapage stores the build-time value separately.",
		&roaring_pending_merge_threshold_guc,
		ROARING_PENDING_MERGE_THRESHOLD,
		1000,
		10000000,
		PGC_USERSET,
		0,
		NULL, NULL, NULL
	);
	MarkGUCPrefixReserved("roaring");
	roaring_customscan_init();
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

    if (opcintype != INT8OID    && opcintype != INT4OID  &&
        opcintype != INT2OID    && opcintype != BOOLOID  &&
        opcintype != DATEOID    && opcintype != FLOAT4OID &&
        opcintype != OIDOID     && opcintype != TEXTOID   &&
        opcintype != UUIDOID    && opcintype != ANYENUMOID &&
        get_typtype(opcintype) != TYPTYPE_ENUM)
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
 * roaring_canreturn
 *
 * Return true if column attno can be returned from the index without a heap
 * fetch.  Key columns are returned from scan keys (equality AM); INCLUDE
 * columns are returned from the payload store (T65).  Hash-keyed types store
 * hash(value) not the original value, so they are not returnable.
 */
static bool
roaring_canreturn(Relation index, int attno)
{
	int nkeyatts = IndexRelationGetNumberOfKeyAttributes(index);
	Oid opcintype;

	/*
	 * INCLUDE columns (attno > nkeyatts) are stored verbatim in the payload
	 * store, so they are always returnable.  They also have no opclass entry,
	 * so rd_opcintype[attno-1] would read past the array — return early.
	 */
	if (attno > nkeyatts)
		return true;

	/*
	 * Key columns carry only value->bitmap, not a per-TID value.  A returnable
	 * key column's value is recovered either from an equality scan key (the
	 * common case, e.g. sum(id) WHERE key=x stays index-only) or, when the
	 * column is projected but not equality-constrained (SELECT a FROM t WHERE
	 * b=5), by the reverse-bitmap lookup in roaring_gettuple — the value bitmaps
	 * of a column partition its TIDs, so the bitmap containing a TID identifies
	 * the value.  This requires the key to be losslessly recoverable:
	 *
	 *   - text/uuid store hash(value): not recoverable — never returnable.
	 *   - int8 in a MULTI-column index is hashed into the 32-bit key slot
	 *     (executor recheck), so it too is not recoverable.  Single-column int8
	 *     keeps the full 64-bit value and stays returnable.
	 *
	 * The remaining key types (int2/int4/oid/date/bool/float4/enum) encode the
	 * value injectively in the key and are recoverable; see
	 * roaring_key32_to_datum().
	 */
	opcintype = index->rd_opcintype[attno - 1];
	if (opcintype == TEXTOID || opcintype == UUIDOID)
		return false;
	if (opcintype == INT8OID && nkeyatts > 1)
		return false;
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

    /*
     * Integer/bool/date opclasses store true SQL equality keys and could in
     * principle advertise consistent-equality semantics to the planner (for
     * hash-join optimisations).  However, text/uuid opclasses store hash(key)
     * rather than the key itself, so they do not satisfy true equality.
     * Since the flag would need to be per-opclass rather than per-AM, we
     * cannot set it globally.  Revisit if/when text/uuid moves to real-equality
     * storage (e.g. a dictionary page).
     */
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
    amroutine->amsearchnulls  = true;   /* IS NULL / IS NOT NULL via NULL bitmaps */
    amroutine->amstorage      = false;
    amroutine->amclusterable  = false;
    amroutine->ampredlocks    = false;
    amroutine->amcanparallel  = false;
    amroutine->amcaninclude   = true;    /* T65: supports INCLUDE columns */
    amroutine->amusemaintenanceworkmem = false;
    amroutine->amparallelvacuumoptions = VACUUM_OPTION_PARALLEL_BULKDEL;
    amroutine->amkeytype      = InvalidOid;

    amroutine->ambuild           = roaring_build;
    amroutine->ambuildempty      = roaring_buildempty;
    amroutine->aminsert          = roaring_insert;
    amroutine->ambulkdelete      = roaring_bulkdelete;
    amroutine->amvacuumcleanup   = roaring_vacuumcleanup;
    amroutine->amcanreturn       = roaring_canreturn;  /* T65: payload projection */
    amroutine->amcostestimate    = roaring_costestimate;
    amroutine->amoptions         = NULL;
    amroutine->amproperty        = NULL;
    amroutine->ambuildphasename  = NULL;
    amroutine->amvalidate        = roaring_validate;
    amroutine->amadjustmembers   = NULL;
    amroutine->ambeginscan       = roaring_beginscan;
    amroutine->amrescan          = roaring_rescan;
    amroutine->amgettuple        = roaring_gettuple;  /* IndexScan / IndexOnlyScan */
    amroutine->amgetbitmap       = roaring_getbitmap;
    amroutine->amendscan         = roaring_endscan;
    amroutine->ammarkpos         = NULL;
    amroutine->amrestrpos        = NULL;
    amroutine->amestimateparallelscan = NULL;
    amroutine->aminitparallelscan     = NULL;
    amroutine->amparallelrescan       = NULL;

    PG_RETURN_POINTER(amroutine);
}

