#include "pg_roaring_index.h"

#include "access/xloginsert.h"
#include "storage/bufmgr.h"

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
