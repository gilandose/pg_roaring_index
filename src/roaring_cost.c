#include "pg_roaring_index.h"

#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "nodes/pathnodes.h"
#include "utils/memutils.h"
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

	if (heap_tuples <= 0)
		return;

	/*
	 * A scan with no equality index clause (e.g. WHERE col IS NULL, an
	 * inequality, or an unqualified scan reached via amoptionalkey) cannot be
	 * answered by a roaring value->bitmap index: it would enumerate every
	 * indexed TID, and for IS NULL it cannot even see the matching rows (NULLs
	 * are not indexed).  Price such paths out of consideration so the planner
	 * falls back to a seqscan — the correct answer for those predicates — even
	 * under enable_seqscan=off.  REINDEX's validate_index calls the AM directly
	 * and is unaffected by this estimate.
	 */
	if (path->indexclauses == NIL)
	{
		*indexStartupCost = *indexTotalCost = 1.0e30;
		*indexSelectivity = 1.0;
		return;
	}

	/*
	 * Read total_entries from the metapage — or from rd_amcache if the
	 * cache was populated by a recent getbitmap call in this session.
	 * A slightly stale total_entries is fine for selectivity estimation.
	 */
	{
		Relation		indexRel   = index_open(index->indexoid, AccessShareLock);
		RoaringAmCache *cache    = (RoaringAmCache *) indexRel->rd_amcache;
		bool		    cache_ok = false;

		if (cache != NULL && cache->total_entries > 0)
		{
			Buffer     tmp     = ReadBuffer(indexRel, ROARING_METAPAGE_BLKNO);
			XLogRecPtr cur_lsn = BufferGetLSNAtomic(tmp);
			ReleaseBuffer(tmp);
			if (cur_lsn == cache->meta_lsn)
			{
				nentries = (double) cache->total_entries;
				cache_ok = true;
			}
		}

		if (!cache_ok)
		{
			Buffer				metabuf;
			RoaringMetaPageData *meta;

			metabuf  = ReadBuffer(indexRel, ROARING_METAPAGE_BLKNO);
			LockBuffer(metabuf, BUFFER_LOCK_SHARE);
			meta	 = RoaringPageGetMeta(BufferGetPage(metabuf));
			nentries = (double) meta->total_entries;

			/* Warm the cache for subsequent planning calls. */
			cache = (RoaringAmCache *)
				MemoryContextAllocZero(CacheMemoryContext,
									   sizeof(RoaringAmCache));
			{
				uint32 total = 0;
				int    si;

				for (si = 0; si < ROARING_PENDING_SHARDS; si++)
				{
					total += meta->shards[si].insert_count;
					cache->insert_head[si]  = meta->shards[si].insert_head;
					cache->merging_head[si] = meta->shards[si].merging_head;
				}
				cache->pending_count_approx = total;
			}
			cache->root_blkno    = meta->root_directory_page;
			cache->total_entries = meta->total_entries;
			cache->meta_lsn      = PageGetLSN(BufferGetPage(metabuf));
			indexRel->rd_amcache = cache;

			UnlockReleaseBuffer(metabuf);
		}

		index_close(indexRel, AccessShareLock);
	}

	if (nentries <= 0)
	{
		*indexSelectivity = 0.0;
		return;
	}

	/*
	 * Use clauselist_selectivity so the planner sees actual column statistics
	 * (MCVs, histograms) rather than a uniform 1/nentries estimate.  This
	 * prevents the index from being chosen for high-cardinality scans where
	 * a seqscan would be cheaper.
	 *
	 * Clamp to [1/nentries, 1.0] so we never estimate more rows than the
	 * uniform model would for a single distinct value.
	 */
	{
		/*
		 * path->indexclauses is a list of IndexClause nodes (PG12+), not
		 * RestrictInfo directly.  Extract the rinfo pointers before calling
		 * clauselist_selectivity, which expects RestrictInfo or plain exprs.
		 */
		List	   *rinfos = NIL;
		ListCell   *lc;
		Selectivity sel;

		foreach(lc, path->indexclauses)
		{
			IndexClause *ic = lfirst_node(IndexClause, lc);
			rinfos = lappend(rinfos, ic->rinfo);
		}

		sel = clauselist_selectivity(root, rinfos,
									 index->rel->relid,
									 JOIN_INNER, NULL);
		*indexSelectivity = Max(1.0 / nentries, Min(sel, 1.0));
	}
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
