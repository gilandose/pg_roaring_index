#include "pg_roaring_index.h"

#include "access/relscan.h"
#include "storage/bufmgr.h"
#include "utils/rel.h"

/* ----------------------------------------------------------------
 * roaring_dir_lookup
 *
 * Walk the directory tree starting at dir_blkno, looking for the
 * leaf page that would contain 'value'.
 *
 * Directory level convention:
 *   level=0  → children are leaf pages (binary-search within the leaf)
 *   level=N  → children are dir pages at level N-1 (recurse)
 *
 * Returns InvalidBlockNumber if value is beyond all high_keys.
 * ---------------------------------------------------------------- */
static BlockNumber
roaring_dir_lookup(Relation index, BlockNumber dir_blkno, int64 value)
{
	Buffer			   buf;
	Page			   page;
	RoaringDirSpecial *spc;
	RoaringDirEntry   *entries;
	uint32			   count;
	uint8			   level;
	BlockNumber		   child;
	uint32			   lo, hi;

	buf     = ReadBuffer(index, dir_blkno);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page    = BufferGetPage(buf);
	spc     = (RoaringDirSpecial *) PageGetSpecialPointer(page);
	entries = (RoaringDirEntry *) PageGetContents(page);
	count   = spc->entry_count;
	level   = spc->level;

	/* Binary search: find leftmost entry with high_key >= value. */
	lo = 0;
	hi = count;
	while (lo < hi)
	{
		uint32 mid = (lo + hi) / 2;
		if (entries[mid].high_key < value)
			lo = mid + 1;
		else
			hi = mid;
	}

	if (lo >= count)
	{
		/* value exceeds all high_keys in this directory. */
		UnlockReleaseBuffer(buf);
		return InvalidBlockNumber;
	}

	child = entries[lo].child_page;
	UnlockReleaseBuffer(buf);

	if (level == 0)
		return child;		/* child is a leaf page */

	return roaring_dir_lookup(index, child, value);	/* recurse into sub-dir */
}

/* ----------------------------------------------------------------
 * roaring_beginscan / roaring_rescan / roaring_endscan
 * ---------------------------------------------------------------- */
IndexScanDesc
roaring_beginscan(Relation rel, int nkeys, int norderbys)
{
	IndexScanDesc	   scan;
	RoaringScanOpaque *so;

	scan = RelationGetIndexScan(rel, nkeys, norderbys);

	so				  = (RoaringScanOpaque *) palloc0(sizeof(RoaringScanOpaque));
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

void
roaring_endscan(IndexScanDesc scan)
{
	pfree(scan->opaque);
	scan->opaque = NULL;
}

/* ----------------------------------------------------------------
 * roaring_getbitmap
 *
 * Lookup the single equality scan key in the roaring index and
 * populate the TIDBitmap with matching TIDs.
 * ---------------------------------------------------------------- */
int64
roaring_getbitmap(IndexScanDesc scan, TIDBitmap *tbm)
{
	Relation			index = scan->indexRelation;
	RoaringScanOpaque  *so    = (RoaringScanOpaque *) scan->opaque;
	int64				ntids = 0;
	int64				scan_value;

	Buffer				metabuf;
	RoaringMetaPageData *meta;
	BlockNumber			root_blkno;
	BlockNumber			leaf_blkno;

	Buffer				leafbuf;
	Page				leafpage;
	OffsetNumber		lo, hi;
	OffsetNumber		found_off = InvalidOffsetNumber;
	RoaringLeafEntry   *le = NULL;

	roaring64_bitmap_t *bm;
	roaring64_iterator_t *it;

	/* getbitmap is called once per scan; subsequent calls return 0. */
	if (so->bitmap_loaded)
		return 0;
	so->bitmap_loaded = true;

	if (scan->numberOfKeys < 1)
		return 0;

	scan_value = DatumGetInt64(scan->keyData[0].sk_argument);

	/* ---- 1. Read metapage ---- */
	metabuf = ReadBuffer(index, ROARING_METAPAGE_BLKNO);
	LockBuffer(metabuf, BUFFER_LOCK_SHARE);
	meta	   = RoaringPageGetMeta(BufferGetPage(metabuf));

	if (meta->magic != ROARING_MAGIC)
		elog(ERROR, "pg_roaring_index: bad magic in metapage of index \"%s\"",
			 RelationGetRelationName(index));

	root_blkno = meta->root_directory_page;
	UnlockReleaseBuffer(metabuf);

	if (root_blkno == InvalidBlockNumber)
		return 0;			/* empty index */

	/* ---- 2. Walk directory to find the right leaf page ---- */
	leaf_blkno = roaring_dir_lookup(index, root_blkno, scan_value);
	if (leaf_blkno == InvalidBlockNumber)
		return 0;			/* value beyond all indexed values */

	/* ---- 3. Binary-search leaf page for exact value ---- */
	leafbuf  = ReadBuffer(index, leaf_blkno);
	LockBuffer(leafbuf, BUFFER_LOCK_SHARE);
	leafpage = BufferGetPage(leafbuf);

	lo = 1;
	hi = PageGetMaxOffsetNumber(leafpage);
	while (lo <= hi)
	{
		OffsetNumber	  mid = (lo + hi) / 2;
		RoaringLeafEntry *e   = (RoaringLeafEntry *)
								PageGetItem(leafpage,
											PageGetItemId(leafpage, mid));
		if (e->value == scan_value)
		{
			le		  = e;
			found_off = mid;
			break;
		}
		else if (e->value < scan_value)
			lo = mid + 1;
		else
			hi = mid - 1;
	}

	if (le == NULL)
	{
		UnlockReleaseBuffer(leafbuf);
		return 0;			/* value not found */
	}

	if (le->flags != ROARING_ENTRY_INLINE)
	{
		UnlockReleaseBuffer(leafbuf);
		elog(ERROR, "pg_roaring_index: overflow entries not yet implemented");
	}

	/* ---- 4. Deserialize bitmap ---- */
	{
		Size item_len  = ItemIdGetLength(PageGetItemId(leafpage, found_off));
		Size bitmap_len = item_len - sizeof(RoaringLeafEntry);

		bm = roaring64_bitmap_portable_deserialize_safe(
				(const char *)(le + 1), bitmap_len);

		if (bm == NULL)
		{
			UnlockReleaseBuffer(leafbuf);
			elog(ERROR, "pg_roaring_index: failed to deserialize bitmap for value "
				 INT64_FORMAT, scan_value);
		}
	}

	UnlockReleaseBuffer(leafbuf);

	/* ---- 5. Add all TIDs to the TIDBitmap ---- */
	{
#define ROARING_TID_BATCH 512
		ItemPointerData tid_buf[ROARING_TID_BATCH];
		int				tid_count = 0;

		it = roaring64_iterator_create(bm);
		while (roaring64_iterator_has_value(it))
		{
			uint64		 linear = roaring64_iterator_value(it);
			BlockNumber  block  = (BlockNumber)(linear >> 16);
			OffsetNumber off    = (OffsetNumber)(linear & 0xFFFF);

			if (tid_count == ROARING_TID_BATCH)
			{
				tbm_add_tuples(tbm, tid_buf, tid_count, false);
				tid_count = 0;
			}

			ItemPointerSet(&tid_buf[tid_count], block, off);
			tid_count++;
			ntids++;

			roaring64_iterator_advance(it);
		}
		if (tid_count > 0)
			tbm_add_tuples(tbm, tid_buf, tid_count, false);

		roaring64_iterator_free(it);
#undef ROARING_TID_BATCH
	}

	roaring64_bitmap_free(bm);
	return ntids;
}
