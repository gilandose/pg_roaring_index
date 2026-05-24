#include "pg_roaring_index.h"

#include "access/relation.h"
#include "funcapi.h"
#include "storage/bufmgr.h"
#include "utils/builtins.h"
#include "utils/rel.h"

PG_FUNCTION_INFO_V1(roaring_index_stats);

/*
 * roaring_index_stats(regclass) RETURNS RECORD
 *
 * Read the metapage of a roaring or roaring_lossy index and return one row
 * of diagnostic fields.  Used by the pg_stat_roaring_indexes view and the
 * roaring_index_health() probe function.
 */
Datum
roaring_index_stats(PG_FUNCTION_ARGS)
{
	Oid					 indexoid = PG_GETARG_OID(0);
	Relation			 index;
	Buffer				 metabuf;
	RoaringMetaPageData *meta;
	TupleDesc			 tupdesc;
	Datum				 values[9];
	bool				 nulls[9];
	HeapTuple			 htuple;
	uint32				 pending_count = 0;
	int					 i;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR,
			 "roaring_index_stats: must be called in a context that accepts a composite");
	BlessTupleDesc(tupdesc);

	index   = relation_open(indexoid, AccessShareLock);
	metabuf = ReadBuffer(index, ROARING_METAPAGE_BLKNO);
	LockBuffer(metabuf, BUFFER_LOCK_SHARE);
	meta    = RoaringPageGetMeta(BufferGetPage(metabuf));

	for (i = 0; i < ROARING_PENDING_SHARDS; i++)
		pending_count += meta->shards[i].insert_count;

	memset(nulls, false, sizeof(nulls));
	values[0] = Int64GetDatum((int64) meta->total_entries);
	values[1] = Int64GetDatum((int64) pending_count);
	values[2] = Int32GetDatum((int32) meta->pending_merge_threshold);
	values[3] = BoolGetDatum((meta->flags & ROARING_FLAG_LOSSY) != 0);
	values[4] = BoolGetDatum((meta->flags & ROARING_FLAG_BGMERGE_SPAWNED) != 0);
	values[5] = Int32GetDatum((int32) meta->version);
	values[6] = Int64GetDatum((int64) meta->root_directory_page);
	values[7] = Int64GetDatum((int64) meta->leftmost_leaf_page);
	values[8] = Int64GetDatum((int64) meta->free_list_head);

	UnlockReleaseBuffer(metabuf);
	relation_close(index, AccessShareLock);

	htuple = heap_form_tuple(tupdesc, values, nulls);
	PG_RETURN_DATUM(HeapTupleGetDatum(htuple));
}
