/*
 * roaring_customscan.c
 *
 * T64: CustomScan provider for O(1) count(*) on roaring indexes.
 *
 * Detects the pattern:
 *   SELECT count(*) FROM t WHERE indexed_col = X
 *   SELECT count(*) FROM t WHERE indexed_col IN (X1, X2, ...)
 *
 * and replaces the normal Aggregate+Scan plan with a CustomScan that reads
 * cardinality directly from leaf entry headers, then walks the pending list
 * for an exact MVCC-correct count.  No bitmap decode, no heap access.
 *
 * Restrictions (all must hold for the optimisation to fire):
 *   - Exact (roaring) AM only — lossy stores block counts, not row counts.
 *   - Single-column index only.
 *   - Single equality or scalar-array (IN) qual, all-Const right-hand side.
 *   - No GROUP BY, HAVING, DISTINCT, LIMIT, or ORDER BY.
 *   - Single base relation (no joins).
 *
 * For IN lists: bitmaps for distinct values of the same column are disjoint,
 * so the result is the exact sum of individual cardinalities.
 */

#include "pg_roaring_index.h"

#include "access/genam.h"
#include "access/relation.h"
#include "catalog/pg_am.h"
#include "catalog/pg_type_d.h"
#include "executor/executor.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "optimizer/pathnode.h"
#include "optimizer/planner.h"
#include "storage/bufmgr.h"
#include "commands/explain_format.h"
#include "utils/array.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include <math.h>

/* ----------------------------------------------------------------
 * Private state stored per-scan
 * ---------------------------------------------------------------- */
typedef struct RoaringQual
{
	int			nkeys;
	int64	   *keys;
} RoaringQual;

typedef struct RoaringCountState
{
	CustomScanState css;
	Oid			indexoid;
	RoaringQual *quals;
	int			nquals;
	bool		done;
} RoaringCountState;

/* ----------------------------------------------------------------
 * Saved previous hook pointer
 * ---------------------------------------------------------------- */
static create_upper_paths_hook_type prev_create_upper_paths_hook = NULL;

/* Cached AM OID — set in roaring_customscan_init(), never changes. */
static Oid roaring_exact_amoid = InvalidOid;

/* ----------------------------------------------------------------
 * Forward declarations
 * ---------------------------------------------------------------- */
static Plan *roaring_count_plan_path(PlannerInfo *root,
									 RelOptInfo *rel,
									 CustomPath *best_path,
									 List *tlist,
									 List *clauses,
									 List *custom_plans);
static Node *roaring_count_create_scan_state(CustomScan *cscan);
static void  roaring_count_begin(CustomScanState *node, EState *estate,
								 int eflags);
static TupleTableSlot *roaring_count_exec(CustomScanState *node);
static void  roaring_count_end(CustomScanState *node);
static void  roaring_count_rescan(CustomScanState *node);
static void  roaring_count_explain(CustomScanState *node,
								   List *ancestors,
								   ExplainState *es);

static CustomPathMethods roaring_count_path_methods = {
	.CustomName     = "RoaringCount",
	.PlanCustomPath = roaring_count_plan_path,
};

static CustomScanMethods roaring_count_scan_methods = {
	.CustomName            = "RoaringCount",
	.CreateCustomScanState = roaring_count_create_scan_state,
};

static CustomExecMethods roaring_count_exec_methods = {
	.CustomName        = "RoaringCount",
	.BeginCustomScan   = roaring_count_begin,
	.ExecCustomScan    = roaring_count_exec,
	.EndCustomScan     = roaring_count_end,
	.ReScanCustomScan  = roaring_count_rescan,
	.ExplainCustomScan = roaring_count_explain,
};

/* ----------------------------------------------------------------
 * Pattern detection helpers
 * ---------------------------------------------------------------- */

/*
 * is_count_star - return true if the query is a bare count(*) with no
 * GROUP BY, HAVING, DISTINCT, LIMIT, or ORDER BY.
 */
static bool
is_count_star(PlannerInfo *root)
{
	Query	   *parse = root->parse;
	TargetEntry *tle;
	Aggref	   *agg;

	if (list_length(parse->targetList) != 1)
		return false;
	if (parse->groupClause != NIL || parse->havingQual != NULL)
		return false;
	if (parse->distinctClause != NIL || parse->sortClause != NIL)
		return false;
	if (parse->limitCount != NULL || parse->limitOffset != NULL)
		return false;

	tle = linitial(parse->targetList);
	if (!IsA(tle->expr, Aggref))
		return false;

	agg = (Aggref *) tle->expr;
	/* count(*): aggstar=true, no FILTER, no DISTINCT, int8 result */
	if (!agg->aggstar || agg->aggdistinct != NIL || agg->aggfilter != NULL)
		return false;
	if (agg->aggtype != INT8OID)
		return false;

	return true;
}

/*
 * extract_const_keys - try to extract int64 roaring keys from a qual clause.
 *
 * Handles three forms:
 *   OpExpr(Var = Const)           → single key
 *   ScalarArrayOpExpr(Var = ANY(ArrayExpr of Consts))  → key list
 *   NullTest(Var IS NULL)         → single per-column NULL key
 *
 * Returns a palloc'd array via *keys_out / *nkeys_out, or false on failure.
 * The Var's varattno (1-based) is returned in *attno_out.
 * The Const values are converted to int64 roaring keys using the column's
 * type (read from opcintype of the matched index column).
 */
static bool
extract_const_keys(Node *clause, IndexOptInfo *ii,
				   int *attno_out, int64 **keys_out, int *nkeys_out)
{
	if (IsA(clause, OpExpr))
	{
		OpExpr *op = (OpExpr *) clause;
		Var	   *var;
		Const  *c;
		int		colno;	/* 0-based index column */
		int64	key;
		Oid		typid;

		if (list_length(op->args) != 2)
			return false;
		if (!IsA(linitial(op->args), Var) || !IsA(lsecond(op->args), Const))
			return false;

		var = (Var *) linitial(op->args);
		c   = (Const *) lsecond(op->args);
		if (c->constisnull)
			return false;

		/* Match Var to index column (indexkeys are 1-based attno). */
		colno = -1;
		for (int k = 0; k < ii->nkeycolumns; k++)
		{
			if (ii->indexkeys[k] == var->varattno)
			{
				colno = k;
				break;
			}
		}
		if (colno < 0)
			return false;

		typid = ii->opcintype[colno];
		/*
		 * Skip hash-keyed types — collisions make the exact count wrong, so we
		 * decline the fast path and let a recheck scan answer it.  text/uuid are
		 * always hashed; int8 only in a multi-column key (single-column stores
		 * it losslessly via roaring_datum_to_key64).
		 */
		if (typid == TEXTOID || typid == UUIDOID)
			return false;
		if (typid == INT8OID && ii->nkeycolumns > 1)
			return false;
		/* NaN float4 is not indexed. */
		if (typid == FLOAT4OID && isnan(DatumGetFloat4(c->constvalue)))
			return false;
		/*
		 * anyenum opclass stores opcintype = ANYENUMOID (a pseudo-type).
		 * roaring_datum_to_key*() needs the concrete enum OID to cast correctly;
		 * use the Const's own type instead.
		 */
		if (typid == ANYENUMOID)
			typid = c->consttype;

		/*
		 * Single-column indexes store the raw int64 key (matching the build
		 * path); multi-column indexes namespace a 32-bit key by column.
		 */
		if (ii->nkeycolumns == 1)
			key = roaring_datum_to_key64(c->constvalue, typid);
		else
			key = ROARING_COL_KEY(colno + 1,
								   roaring_datum_to_key32(c->constvalue, typid));

		*attno_out  = var->varattno;
		*keys_out   = palloc(sizeof(int64));
		(*keys_out)[0] = key;
		*nkeys_out  = 1;
		return true;
	}

	if (IsA(clause, ScalarArrayOpExpr))
	{
		ScalarArrayOpExpr *sao = (ScalarArrayOpExpr *) clause;
		Var		   *var;
		Const	   *ac;
		ArrayType  *arr;
		Oid			elemtype;
		Oid			typid;
		int			colno;
		int			nelems;
		Datum	   *elems;
		bool	   *nulls;
		int64	   *keys;
		int			nkeys = 0;

		if (list_length(sao->args) != 2)
			return false;
		if (!IsA(linitial(sao->args), Var) || !IsA(lsecond(sao->args), Const))
			return false;

		var = (Var *) linitial(sao->args);
		ac  = (Const *) lsecond(sao->args);
		if (ac->constisnull)
			return false;

		colno = -1;
		for (int k = 0; k < ii->nkeycolumns; k++)
		{
			if (ii->indexkeys[k] == var->varattno)
			{
				colno = k;
				break;
			}
		}
		if (colno < 0)
			return false;

		typid = ii->opcintype[colno];
		if (typid == TEXTOID || typid == UUIDOID)
			return false;
		if (typid == INT8OID && ii->nkeycolumns > 1)
			return false;

		arr      = DatumGetArrayTypeP(ac->constvalue);
		elemtype = ARR_ELEMTYPE(arr);
		/* anyenum opclass: use concrete element type for key conversion. */
		if (typid == ANYENUMOID)
			typid = elemtype;
		{
			int16 elmlen;
			bool  elmbyval;
			char  elmalign;

			get_typlenbyvalalign(elemtype, &elmlen, &elmbyval, &elmalign);
			deconstruct_array(arr, elemtype, elmlen, elmbyval, elmalign,
							  &elems, &nulls, &nelems);
		}
		if (nelems == 0)
		{
			pfree(elems);
			pfree(nulls);
			return false;
		}

		keys = palloc(nelems * sizeof(int64));
		for (int i = 0; i < nelems; i++)
		{
			if (nulls[i])
				continue;
			if (typid == FLOAT4OID && isnan(DatumGetFloat4(elems[i])))
				continue;
			keys[nkeys++] = (ii->nkeycolumns == 1)
				? roaring_datum_to_key64(elems[i], typid)
				: ROARING_COL_KEY(colno + 1,
								   roaring_datum_to_key32(elems[i], typid));
		}
		pfree(elems);
		pfree(nulls);

		if (nkeys == 0)
			return false;

		*attno_out = var->varattno;
		*keys_out  = keys;
		*nkeys_out = nkeys;
		return true;
	}

	if (IsA(clause, NullTest))
	{
		NullTest *nt = (NullTest *) clause;
		Node	 *arg;
		Var		 *var;
		int		  colno;

		/*
		 * Only "col IS NULL" is a positive key lookup against the per-column
		 * NULL bitmap (ROARING_NULL_COL_KEY).  "col IS NOT NULL" is the
		 * complement (all rows andnot the NULL set), which the key-array count
		 * model cannot express, so we decline it and let a regular scan answer.
		 * Row-typed IS NULL (argisrow) has SQL semantics we do not index.
		 */
		if (nt->nulltesttype != IS_NULL || nt->argisrow)
			return false;

		arg = (Node *) nt->arg;
		/* A binary-compatible cast may wrap the column reference. */
		while (IsA(arg, RelabelType))
			arg = (Node *) ((RelabelType *) arg)->arg;
		if (!IsA(arg, Var))
			return false;
		var = (Var *) arg;

		/* Match Var to an index key column (indexkeys are 1-based attno). */
		colno = -1;
		for (int k = 0; k < ii->nkeycolumns; k++)
		{
			if (ii->indexkeys[k] == var->varattno)
			{
				colno = k;
				break;
			}
		}
		if (colno < 0)
			return false;

		/*
		 * NULL-ness is exact for every column type — it does not depend on the
		 * value encoding — so this path is valid even for hash-keyed
		 * (text/uuid/multi-column int8) columns, unlike the equality paths above.
		 * The key matches the one the write path records for NULL rows.
		 */
		*attno_out     = var->varattno;
		*keys_out      = palloc(sizeof(int64));
		(*keys_out)[0] = ROARING_NULL_COL_KEY(colno + 1);
		*nkeys_out     = 1;
		return true;
	}

	return false;
}

/* ----------------------------------------------------------------
 * Main planner hook
 * ---------------------------------------------------------------- */
static void
roaring_create_upper_paths(PlannerInfo *root, UpperRelationKind stage,
						   RelOptInfo *input_rel, RelOptInfo *upper_rel,
						   void *extra)
{
	IndexOptInfo *best_ii  = NULL;
	int64		 *keys	   = NULL;
	int			  nkeys	   = 0;
	ListCell	 *lc;

	if (prev_create_upper_paths_hook)
		prev_create_upper_paths_hook(root, stage, input_rel, upper_rel, extra);

	if (stage != UPPERREL_GROUP_AGG)
		return;

	/*
	 * Lazy AM OID lookup — syscache is unavailable at _PG_init time, so we
	 * cache the OID on first use inside the planner hook instead.
	 */
	if (roaring_exact_amoid == InvalidOid)
	{
		HeapTuple	tup = SearchSysCache1(AMNAME, CStringGetDatum("roaring"));

		if (!HeapTupleIsValid(tup))
			return;
		roaring_exact_amoid = ((Form_pg_am) GETSTRUCT(tup))->oid;
		ReleaseSysCache(tup);
	}

	if (!is_count_star(root))
		return;

	/* Must be a single base relation — no joins. */
	if (input_rel->reloptkind != RELOPT_BASEREL)
		return;

	/* Find a single-column roaring (exact) index with a matching qual. */
	foreach(lc, input_rel->indexlist)
	{
		IndexOptInfo *ii     = (IndexOptInfo *) lfirst(lc);
		ListCell	 *qlc;
		bool		  all_ok = true;
		RoaringQual	 *temp_quals;
		int			  num_quals = 0;

		if (ii->relam != roaring_exact_amoid)
			continue;
		if (ii->indpred != NIL)
			continue;	/* partial index: predicate may exclude rows */

		temp_quals = palloc(list_length(input_rel->baserestrictinfo) * sizeof(RoaringQual));

		foreach(qlc, input_rel->baserestrictinfo)
		{
			RestrictInfo *ri = (RestrictInfo *) lfirst(qlc);
			int		  this_attno;
			int64	 *this_keys;
			int		  this_nkeys;

			if (!extract_const_keys((Node *) ri->clause, ii,
									&this_attno, &this_keys, &this_nkeys))
			{
				all_ok = false;
				break;
			}

			temp_quals[num_quals].nkeys = this_nkeys;
			temp_quals[num_quals].keys = this_keys;
			num_quals++;
		}

		if (all_ok && num_quals > 0)
		{
			List *private_quals = NIL;
			CustomPath *cpath;
			int q, i;

			/* Found a usable index! */
			best_ii = ii;
			
			/* Save into private data */
			for (q = 0; q < num_quals; q++)
			{
				List *key_list = NIL;
				for (i = 0; i < temp_quals[q].nkeys; i++)
				{
					key_list = lappend(key_list,
									   makeConst(INT8OID, -1, InvalidOid,
												 sizeof(int64),
												 Int64GetDatum(temp_quals[q].keys[i]),
												 false, true));
				}
				private_quals = lappend(private_quals, key_list);
			}
			
			cpath = makeNode(CustomPath);

			cpath->path.pathtype          = T_CustomScan;
			cpath->path.parent            = upper_rel;
			cpath->path.pathtarget        = upper_rel->reltarget;
			cpath->path.param_info        = NULL;
			cpath->path.parallel_aware    = false;
			cpath->path.parallel_safe     = false;
			cpath->path.parallel_workers  = 0;
			cpath->path.rows              = 1;
			cpath->path.startup_cost      = 0;
			cpath->path.total_cost        = 1;	/* nearly free */
			cpath->path.pathkeys          = NIL;
			cpath->flags                  = 0;
			cpath->custom_paths           = NIL;
			cpath->custom_private         = list_make2(makeInteger(best_ii->indexoid),
													   private_quals);
			cpath->methods                = &roaring_count_path_methods;

			add_path(upper_rel, (Path *) cpath);
			break;
		}
	}
}

/* ----------------------------------------------------------------
 * PlanCustomPath — convert CustomPath to CustomScan plan node
 * ---------------------------------------------------------------- */
static Plan *
roaring_count_plan_path(PlannerInfo *root,
						RelOptInfo *rel,
						CustomPath *best_path,
						List *tlist,
						List *clauses,
						List *custom_plans)
{
	CustomScan *cscan = makeNode(CustomScan);

	/*
	 * Use tlist directly for both custom_scan_tlist and scan.plan.targetlist.
	 *
	 * tlist contains the Aggref(count(*)) from UPPERREL_GROUP_AGG.  If we put
	 * a Var(INDEX_VAR,1) in scan.plan.targetlist instead, the planner wraps us
	 * in a Result node whose targetlist has the Aggref.  During executor init
	 * the Result looks for the Aggref in our (sub)plan's targetlist, fails to
	 * find it, and raises "variable not found in subplan target list".
	 *
	 * The correct pattern (same as FDW aggregate pushdown) is to set both
	 * lists to tlist.  ExecInitCustomScan will call ExecTypeFromTL on
	 * custom_scan_tlist to build the scan-slot TupleDesc (one INT8 column),
	 * then map each Aggref in scan.plan.targetlist to its matching position in
	 * custom_scan_tlist via the pushed-down-aggregate mechanism.  Our exec
	 * method stores the count in tts_values[0]; the evaluator reads it back.
	 */
	cscan->custom_scan_tlist    = tlist;
	cscan->scan.plan.targetlist = tlist;

	cscan->scan.plan.startup_cost = best_path->path.startup_cost;
	cscan->scan.plan.total_cost   = best_path->path.total_cost;
	cscan->scan.plan.plan_rows    = 1;
	cscan->scan.plan.plan_width   = sizeof(int64);
	cscan->scan.scanrelid         = 0;	/* no heap scan */
	cscan->flags                  = 0;
	cscan->custom_plans           = NIL;
	cscan->custom_exprs           = NIL;
	cscan->custom_private         = best_path->custom_private;
	cscan->methods                = &roaring_count_scan_methods;

	return (Plan *) cscan;
}

/* ----------------------------------------------------------------
 * Execution
 * ---------------------------------------------------------------- */

static Node *
roaring_count_create_scan_state(CustomScan *cscan)
{
	RoaringCountState *state =
		(RoaringCountState *) palloc0(sizeof(RoaringCountState));

	NodeSetTag(state, T_CustomScanState);
	state->css.methods = &roaring_count_exec_methods;
	return (Node *) state;
}

static void
roaring_count_begin(CustomScanState *node, EState *estate, int eflags)
{
	RoaringCountState *state = (RoaringCountState *) node;
	CustomScan		  *cscan = (CustomScan *) node->ss.ps.plan;
	Oid				   indexoid;
	List			  *quals_list;
	ListCell		  *lc;
	int				   q = 0;

	/* Decode custom_private: [Integer(indexoid), List(List(key_consts))] */
	indexoid   = (Oid) intVal(linitial(cscan->custom_private));
	quals_list = (List *) lsecond(cscan->custom_private);

	state->indexoid = indexoid;
	state->nquals   = list_length(quals_list);
	state->quals    = palloc(state->nquals * sizeof(RoaringQual));
	state->done     = false;

	foreach(lc, quals_list)
	{
		List	 *key_list = (List *) lfirst(lc);
		ListCell *klc;
		int		  i = 0;

		state->quals[q].nkeys = list_length(key_list);
		state->quals[q].keys  = palloc(state->quals[q].nkeys * sizeof(int64));

		foreach(klc, key_list)
		{
			Const *c = (Const *) lfirst(klc);
			state->quals[q].keys[i++] = DatumGetInt64(c->constvalue);
		}
		q++;
	}
}

/*
 * lookup_leaf_cardinality
 *
 * Find 'value' in the leaf pages starting at 'root_blkno' and return its
 * stored cardinality.  Returns 0 if the value is not present.
 */
static int64
lookup_leaf_cardinality(Relation index, BlockNumber root_blkno, int64 value)
{
	BlockNumber leaf_blkno = roaring_dir_lookup(index, root_blkno, value);

	if (leaf_blkno == InvalidBlockNumber)
		return 0;

	for (;;)
	{
		Buffer				 buf;
		Page				 page;
		RoaringLeafSpecial	*lspc;
		OffsetNumber		 lo, hi;
		bool				 found = false;
		int64				 card  = 0;

		buf  = ReadBuffer(index, leaf_blkno);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);

		lo = 1;
		hi = PageGetMaxOffsetNumber(page);
		while (lo <= hi)
		{
			OffsetNumber	  mid = (lo + hi) / 2;
			RoaringLeafEntry *le  = (RoaringLeafEntry *)
									PageGetItem(page, PageGetItemId(page, mid));

			if (le->value == value)
			{
				card  = (int64) le->cardinality;
				found = true;
				break;
			}
			else if (le->value < value)
				lo = mid + 1;
			else
				hi = mid - 1;
		}

		if (found)
		{
			UnlockReleaseBuffer(buf);
			return card;
		}

		/* Follow right sibling if a concurrent split moved the value. */
		lspc = (RoaringLeafSpecial *) PageGetSpecialPointer(page);
		if (lspc->right_page != InvalidBlockNumber)
		{
			OffsetNumber maxoff = PageGetMaxOffsetNumber(page);

			if (maxoff >= 1)
			{
				RoaringLeafEntry *last = (RoaringLeafEntry *)
					PageGetItem(page, PageGetItemId(page, maxoff));

				if (last->value < value)
				{
					BlockNumber right = lspc->right_page;

					UnlockReleaseBuffer(buf);
					leaf_blkno = right;
					continue;
				}
			}
		}

		UnlockReleaseBuffer(buf);
		return 0;
	}
}

/*
 * count_pending_for_keys
 *
 * Walk one pending chain (starting at start_blkno) and count visible entries
 * whose value is in the sorted keys array.  Returns the count.
 */
static int64
count_pending_for_keys(Relation index, BlockNumber start_blkno,
					   const int64 *keys, int nkeys, Snapshot snapshot)
{
	BlockNumber cur   = start_blkno;
	int64		total = 0;

	while (cur != InvalidBlockNumber)
	{
		Buffer				  buf;
		Page				  page;
		RoaringPendingSpecial *spc;
		RoaringPendingEntry	  *raw;
		uint16				  k;

		buf  = ReadBuffer(index, cur);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		spc  = (RoaringPendingSpecial *) PageGetSpecialPointer(page);
		raw  = (RoaringPendingEntry *) PageGetContents(page);

		/* Page-level skip using value_min / value_max. */
		if (keys[0] <= spc->value_max && keys[nkeys - 1] >= spc->value_min)
		{
			for (k = 0; k < spc->entry_count; k++)
			{
				int64 v  = raw[k].value;
				int   lo = 0,
					  hi = nkeys - 1;

				while (lo <= hi)
				{
					int mid = (lo + hi) / 2;

					if (keys[mid] == v)
					{
						if (roaring_pending_visible(raw[k].xmin, snapshot))
							total++;
						break;
					}
					else if (keys[mid] < v)
						lo = mid + 1;
					else
						hi = mid - 1;
				}
			}
		}

		cur = spc->next_page;
		UnlockReleaseBuffer(buf);
	}

	return total;
}

static TupleTableSlot *
roaring_count_exec(CustomScanState *node)
{
	RoaringCountState *state  = (RoaringCountState *) node;
	TupleTableSlot	  *slot   = node->ss.ss_ScanTupleSlot;
	Relation		   index;
	Buffer			   metabuf;
	Page			   metapage;
	RoaringMetaPageData *meta;
	BlockNumber		   root_blkno;
	Snapshot		   snapshot;
	int64			   total = 0;

	if (state->done)
		return ExecClearTuple(slot);
	state->done = true;

	index   = index_open(state->indexoid, AccessShareLock);
	metabuf = ReadBuffer(index, ROARING_METAPAGE_BLKNO);
	LockBuffer(metabuf, BUFFER_LOCK_SHARE);
	metapage = BufferGetPage(metabuf);
	meta     = RoaringPageGetMeta(metapage);
	roaring_validate_metapage(index, meta);
	root_blkno = meta->root_directory_page;

	/* Sum cardinalities (O(1) fast path when no pending). */
	{
		BlockNumber insert_heads[ROARING_PENDING_SHARDS];
		BlockNumber merging_heads[ROARING_PENDING_SHARDS];
		bool		any_pending = false;
		int			s;

		for (s = 0; s < ROARING_PENDING_SHARDS; s++)
		{
			insert_heads[s]  = meta->shards[s].insert_head;
			merging_heads[s] = meta->shards[s].merging_head;
			if (insert_heads[s] != InvalidBlockNumber ||
				merging_heads[s] != InvalidBlockNumber)
				any_pending = true;
		}

		if (state->nquals == 1 && !any_pending)
		{
			for (int i = 0; i < state->quals[0].nkeys; i++)
				total += lookup_leaf_cardinality(index, root_blkno, state->quals[0].keys[i]);
		}
		else
		{
			/*
			 * We have either multiple quals (AND conditions to intersect),
			 * or a pending list (need to deduplicate leaf and pending TIDs).
			 * Build the exact bitmap for each qual, then AND them together.
			 */
			roaring64_bitmap_t *final_bitmap = NULL;
			snapshot = GetActiveSnapshot();

			for (int q = 0; q < state->nquals; q++)
			{
				roaring64_bitmap_t *q_bitmap = roaring_build_bitmap_exact(
					index, root_blkno, state->quals[q].keys, state->quals[q].nkeys,
					insert_heads, merging_heads, snapshot);

				if (q == 0)
					final_bitmap = q_bitmap;
				else
				{
					roaring64_bitmap_and_inplace(final_bitmap, q_bitmap);
					roaring64_bitmap_free(q_bitmap);
				}
			}

			if (final_bitmap != NULL)
			{
				total = roaring64_bitmap_get_cardinality(final_bitmap);
				roaring64_bitmap_free(final_bitmap);
			}
		}
	}

	UnlockReleaseBuffer(metabuf);
	index_close(index, AccessShareLock);

	ExecClearTuple(slot);
	slot->tts_values[0] = Int64GetDatum(total);
	slot->tts_isnull[0] = false;
	return ExecStoreVirtualTuple(slot);
}

static void
roaring_count_end(CustomScanState *node)
{
	/* index was opened and closed inside exec; nothing to do here */
}

static void
roaring_count_rescan(CustomScanState *node)
{
	((RoaringCountState *) node)->done = false;
}

static void
roaring_count_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	RoaringCountState *state = (RoaringCountState *) node;
	StringInfoData	   buf;

	initStringInfo(&buf);
	appendStringInfo(&buf, "%d qual%s",
					 state->nquals, state->nquals == 1 ? "" : "s");
	ExplainPropertyText("RoaringCount", buf.data, es);
	pfree(buf.data);
}

/* ----------------------------------------------------------------
 * Initialisation — called from _PG_init
 * ---------------------------------------------------------------- */
void
roaring_customscan_init(void)
{
	/*
	 * AM OID lookup is deferred to first use in the hook; syscache is
	 * unavailable at _PG_init time during postmaster startup.
	 */
	RegisterCustomScanMethods(&roaring_count_scan_methods);
	prev_create_upper_paths_hook = create_upper_paths_hook;
	create_upper_paths_hook      = roaring_create_upper_paths;
}
