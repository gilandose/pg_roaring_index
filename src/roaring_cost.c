#include "pg_roaring_index.h"

#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "utils/rel.h"

/*
 * roaring_costestimate
 *
 * Read total_entries (= distinct value count) from the metapage and use
 * uniform-distribution selectivity: 1 / total_entries.
 *
 * Opening the index with AccessShareLock during planning is safe and
 * is the same approach used by contrib/bloom and other custom AMs.
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
	IndexOptInfo   *index		= path->indexinfo;
	double			heap_tuples = index->rel->tuples;
	double			nentries	= 0.0;

	*indexStartupCost = 0.0;
	*indexTotalCost   = 0.0;
	*indexSelectivity = 1.0;
	*indexCorrelation = 0.0;
	*indexPages		  = 1.0;

	if (path->indexclauses == NIL || heap_tuples <= 0)
		return;

	/* Read total_entries from the metapage. */
	{
		Relation			indexRel;
		Buffer				metabuf;
		RoaringMetaPageData *meta;

		indexRel = index_open(index->indexoid, AccessShareLock);
		metabuf  = ReadBuffer(indexRel, ROARING_METAPAGE_BLKNO);
		LockBuffer(metabuf, BUFFER_LOCK_SHARE);
		meta	 = RoaringPageGetMeta(BufferGetPage(metabuf));
		nentries = (double) meta->total_entries;
		UnlockReleaseBuffer(metabuf);
		index_close(indexRel, AccessShareLock);
	}

	if (nentries <= 0)
	{
		*indexSelectivity = 0.0;
		return;
	}

	/* Uniform-distribution selectivity: each value matches 1/nentries fraction. */
	*indexSelectivity = 1.0 / nentries;
	/*
	 * Directory pages (metapage + root) stay in shared_buffers after first
	 * access; only the leaf page costs a random read.
	 */
	*indexStartupCost = random_page_cost;
	*indexTotalCost   = *indexStartupCost +
						(*indexSelectivity) * heap_tuples * cpu_index_tuple_cost;
	*indexPages		  = Max(1.0, (*indexSelectivity) * (double) index->pages);
	*indexCorrelation = 0.0;		/* bitmap AM: no physical correlation */
}
