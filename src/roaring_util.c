#include "pg_roaring_index.h"

#include "access/xloginsert.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "utils/memutils.h"

/*
 * roaring_validate_metapage
 *
 * Check magic and CRoaring format version.  Called from every AM entry point
 * that reads the metapage so a struct-layout mismatch produces an immediate
 * error rather than silent corruption or an infinite retry loop.
 *
 * Caller must hold the metapage buffer locked (share or exclusive).
 */
void
roaring_validate_metapage(Relation index, const RoaringMetaPageData *meta)
{
	if (meta->magic != ROARING_MAGIC)
		elog(ERROR,
			 "pg_roaring_index: bad magic 0x%08X in metapage of index \"%s\" "
			 "(expected 0x%08X) — index may be corrupt",
			 meta->magic, RelationGetRelationName(index), ROARING_MAGIC);

	if (meta->croaring_format_version != ROARING_EXPECTED_FORMAT_VERSION)
		elog(ERROR,
			 "pg_roaring_index: index \"%s\" was built with CRoaring format "
			 "version %u, but this build expects version %u — REINDEX required",
			 RelationGetRelationName(index),
			 (unsigned) meta->croaring_format_version,
			 (unsigned) ROARING_EXPECTED_FORMAT_VERSION);
}

/*
 * roaring_extend_page
 *
 * Extend the index relation by one page and return the buffer,
 * exclusively locked and ready to write.
 */
Buffer
roaring_extend_page(Relation index)
{
	return ExtendBufferedRel(BMR_REL(index), MAIN_FORKNUM, NULL,
							 EB_LOCK_FIRST);
}

/*
 * roaring_wal_and_release
 *
 * Mark a buffer dirty, emit a full-page WAL record if needed, then release.
 * Used only during bulk-build and index-init paths where GenericXLog is not
 * appropriate (we write entire new pages at once).
 */
void
roaring_wal_and_release(Relation index, Buffer buf)
{
	MarkBufferDirty(buf);
	if (RelationNeedsWAL(index))
		log_newpage_buffer(buf, true);
	UnlockReleaseBuffer(buf);
}

/*
 * roaring_init_pending_page
 *
 * Extend the index by one page, initialise it as an empty pending-list page
 * of the given type, WAL-log it, and return its block number.
 */
BlockNumber
roaring_init_pending_page(Relation index, uint8 page_type)
{
	Buffer				  buf;
	Page				  page;
	RoaringPendingSpecial *spc;
	BlockNumber			  blkno;

	buf   = roaring_extend_page(index);
	blkno = BufferGetBlockNumber(buf);
	page  = BufferGetPage(buf);

	PageInit(page, BLCKSZ, sizeof(RoaringPendingSpecial));
	spc				 = (RoaringPendingSpecial *) PageGetSpecialPointer(page);
	spc->page_type	 = page_type;
	spc->flags		 = 0;
	spc->entry_count = 0;
	spc->next_page	 = InvalidBlockNumber;
	spc->xmin_low	 = InvalidTransactionId;

	roaring_wal_and_release(index, buf);
	return blkno;
}

/*
 * roaring_write_dir_page
 *
 * Write a single directory page holding `count` entries at `level`.
 * level = 0: children are leaf pages.
 * level > 0: children are directory pages at level-1.
 * Returns the block number of the new page.
 */
BlockNumber
roaring_write_dir_page(Relation index,
					   RoaringDirEntry *entries, uint32 count, uint8 level)
{
	Buffer			   buf;
	Page			   page;
	RoaringDirSpecial *spc;
	RoaringDirEntry   *data;
	BlockNumber		   blkno;

	buf   = roaring_extend_page(index);
	blkno = BufferGetBlockNumber(buf);
	page  = BufferGetPage(buf);

	PageInit(page, BLCKSZ, sizeof(RoaringDirSpecial));

	spc				 = (RoaringDirSpecial *) PageGetSpecialPointer(page);
	spc->page_type	 = ROARING_PAGE_DIRECTORY;
	spc->level		 = level;
	spc->entry_count = count;
	spc->right_page	 = InvalidBlockNumber;
	spc->reserved	 = 0;

	data = (RoaringDirEntry *) PageGetContents(page);
	memcpy(data, entries, count * sizeof(RoaringDirEntry));
	((PageHeader) page)->pd_lower =
		(LocationIndex)(SizeOfPageHeaderData + count * sizeof(RoaringDirEntry));

	roaring_wal_and_release(index, buf);
	return blkno;
}

/*
 * roaring_write_overflow_chain
 *
 * Write the portion of a serialized bitmap that exceeds the inline capacity
 * to a chain of ROARING_PAGE_OVERFLOW pages.  Only bytes [prefix_len..total_len)
 * go to overflow pages; the first prefix_len bytes are stored by the caller
 * inside the RoaringOverflowEntry on the leaf page.
 *
 * Returns the block number of the first overflow page, or InvalidBlockNumber
 * if chain_len == 0.  Uses roaring_wal_and_release (full-page WAL) for
 * each new page — safe for both build and merge paths.
 */
BlockNumber
roaring_write_overflow_chain(Relation index,
							 const char *data, size_t total_len,
							 size_t prefix_len)
{
	const char *chain_data  = data + prefix_len;
	size_t		chain_len   = total_len - prefix_len;
	Size		cap			= ROARING_OVERFLOW_PAGE_CAP;
	int			npages, i;
	Buffer	   *bufs;
	BlockNumber *blknos;
	BlockNumber first;

	if (chain_len == 0)
		return InvalidBlockNumber;

	npages = (int)((chain_len + cap - 1) / cap);
	bufs   = palloc(npages * sizeof(Buffer));
	blknos = palloc(npages * sizeof(BlockNumber));

	for (i = 0; i < npages; i++)
	{
		bufs[i]   = roaring_extend_page(index);
		blknos[i] = BufferGetBlockNumber(bufs[i]);
	}

	for (i = 0; i < npages; i++)
	{
		Page					p   = BufferGetPage(bufs[i]);
		RoaringOverflowSpecial *spc;
		char				   *dp;
		size_t					co  = (size_t)i * cap;
		size_t					cl  = Min(cap, chain_len - co);

		PageInit(p, BLCKSZ, sizeof(RoaringOverflowSpecial));
		spc				= (RoaringOverflowSpecial *) PageGetSpecialPointer(p);
		spc->page_type  = ROARING_PAGE_OVERFLOW;
		spc->flags		= 0;
		spc->sequence	= (uint16) i;
		spc->next_page	= (i + 1 < npages) ? blknos[i + 1] : InvalidBlockNumber;
		spc->owner_page = InvalidBlockNumber;

		dp = PageGetContents(p);
		memcpy(dp, chain_data + co, cl);
		((PageHeader) p)->pd_lower =
			(LocationIndex)(SizeOfPageHeaderData + cl);

		roaring_wal_and_release(index, bufs[i]);
	}

	first = blknos[0];
	pfree(bufs);
	pfree(blknos);
	return first;
}

/*
 * roaring_read_overflow_bitmap
 *
 * Reassemble and deserialize the bitmap for a leaf entry that uses overflow
 * pages.  Concatenates the inline_prefix bytes with data from the chain.
 */
roaring_bitmap_t *
roaring_read_overflow_bitmap(Relation index, const RoaringOverflowEntry *oe)
{
	size_t		 total_len = oe->total_len;
	Size		 cap	   = ROARING_OVERFLOW_PAGE_CAP;
	size_t		 off;
	BlockNumber	 cur;
	roaring_bitmap_t *bm;
	char		*buf;

	/* Guard against a corrupt total_len that would cause a huge palloc. */
	if (total_len == 0 || total_len > (size_t) MaxAllocSize)
		elog(ERROR,
			 "pg_roaring_index: corrupt overflow entry: invalid total_len %zu",
			 total_len);

	buf = palloc(total_len);

	/* Inline prefix stored directly in the entry. */
	off = Min(sizeof(oe->inline_prefix), total_len);
	memcpy(buf, oe->inline_prefix, off);

	/* Walk overflow chain. */
	cur = oe->overflow_blkno;
	while (cur != InvalidBlockNumber && off < total_len)
	{
		Buffer					ovbuf = ReadBuffer(index, cur);
		Page					ovpage;
		RoaringOverflowSpecial *spc;
		char				   *dp;
		size_t					cl;

		LockBuffer(ovbuf, BUFFER_LOCK_SHARE);
		ovpage = BufferGetPage(ovbuf);
		spc    = (RoaringOverflowSpecial *) PageGetSpecialPointer(ovpage);
		dp     = PageGetContents(ovpage);
		cl     = Min(cap, total_len - off);
		memcpy(buf + off, dp, cl);
		off   += cl;
		cur    = spc->next_page;
		UnlockReleaseBuffer(ovbuf);
	}

	bm = roaring_bitmap_portable_deserialize_safe(buf, total_len);
	pfree(buf);
	return bm;
}
