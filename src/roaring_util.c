#include "pg_roaring_index.h"

#include "access/xloginsert.h"
#include "catalog/pg_type_d.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "utils/builtins.h"
#include "common/hashfn.h"
#include "utils/memutils.h"
#include "utils/uuid.h"

/*
 * roaring_datum_to_key32
 *
 * Convert a non-null Datum of a supported index type to an int32 key for use
 * in ROARING_COL_KEY() (multi-column indexes).  The mapping is injective for
 * all finite, non-NaN values within each type's domain.
 */
int32
roaring_datum_to_key32(Datum d, Oid typid)
{
	switch (typid)
	{
		case INT4OID:
			return DatumGetInt32(d);
		case INT2OID:
			return (int32) DatumGetInt16(d);
		case BOOLOID:
			return DatumGetBool(d) ? 1 : 0;
		case DATEOID:
			return DatumGetInt32(d);		/* DateADT is int32 */
		case FLOAT4OID:
			return roaring_float4_to_key32(DatumGetFloat4(d));
		case OIDOID:
			return (int32) DatumGetObjectId(d);
		case TEXTOID:
		case VARCHAROID:
		{
			text *t = DatumGetTextPP(d);
			return (int32) hash_bytes((const unsigned char *) VARDATA_ANY(t),
									VARSIZE_ANY_EXHDR(t));
		}
		case UUIDOID:
		{
			pg_uuid_t *u = DatumGetUUIDP(d);
			return (int32) hash_bytes(u->data, UUID_LEN);
		}
		default:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("roaring index: unsupported column type %s for multi-column key",
							format_type_be(typid))));
			return 0;					/* unreachable */
	}
}

/*
 * roaring_datum_to_key64
 *
 * Convert a non-null Datum to an int64 leaf key for single-column indexes.
 * INT8 is the only type with a full 64-bit key range; all others are
 * sign/zero-extended to int64.
 */
int64
roaring_datum_to_key64(Datum d, Oid typid)
{
	switch (typid)
	{
		case INT8OID:
			return DatumGetInt64(d);
		case INT4OID:
			return (int64) DatumGetInt32(d);
		case INT2OID:
			return (int64) DatumGetInt16(d);
		case BOOLOID:
			return DatumGetBool(d) ? INT64_C(1) : INT64_C(0);
		case DATEOID:
			return (int64) DatumGetInt32(d);
		case FLOAT4OID:
			return (int64) roaring_float4_to_key32(DatumGetFloat4(d));
		case OIDOID:
			return (int64)(uint32) DatumGetObjectId(d);
		case TEXTOID:
		case VARCHAROID:
		{
			text *t = DatumGetTextPP(d);
			return (int64)(uint32) hash_bytes((const unsigned char *) VARDATA_ANY(t),
											VARSIZE_ANY_EXHDR(t));
		}
		case UUIDOID:
		{
			pg_uuid_t *u = DatumGetUUIDP(d);
			return (int64)(uint32) hash_bytes(u->data, UUID_LEN);
		}
		default:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("roaring index: unsupported column type %s",
							format_type_be(typid))));
			return 0;
	}
}

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

	if (meta->version != ROARING_INDEX_VERSION)
		elog(ERROR,
			 "pg_roaring_index: index \"%s\" was built with on-disk version %u, "
			 "but this build expects version %u — REINDEX required",
			 RelationGetRelationName(index),
			 (unsigned) meta->version,
			 (unsigned) ROARING_INDEX_VERSION);

	if (meta->croaring_format_version != ROARING_EXPECTED_FORMAT_VERSION)
		elog(ERROR,
			 "pg_roaring_index: index \"%s\" was built with CRoaring format "
			 "version %u, but this build expects version %u — REINDEX required",
			 RelationGetRelationName(index),
			 (unsigned) meta->croaring_format_version,
			 (unsigned) ROARING_EXPECTED_FORMAT_VERSION);

	if (meta->num_shards != ROARING_PENDING_SHARDS)
		elog(ERROR,
			 "pg_roaring_index: index \"%s\" has %u pending shards, "
			 "but this build expects %u — REINDEX required",
			 RelationGetRelationName(index),
			 (unsigned) meta->num_shards,
			 (unsigned) ROARING_PENDING_SHARDS);
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
 * roaring_alloc_page
 *
 * Allocate one page buffer, exclusively locked and ready to write.
 *
 * When the metapage's free list is non-empty (free_list_head != Invalid),
 * the head page is popped and returned.  The caller MUST update the
 * metapage image inside their GenericXLog record:
 *
 *   meta_img->free_list_head = *new_free_list_head_out;
 *
 * and MUST register the returned buffer with GENERIC_XLOG_FULL_IMAGE so
 * the recycled page's stale content is replaced atomically.
 *
 * When the free list is empty, the index is extended as usual and
 * *new_free_list_head_out is set to InvalidBlockNumber (no metapage
 * update needed by the caller for free_list_head).
 *
 * Caller must hold metabuf EXCLUSIVE before calling.
 */
Buffer
roaring_alloc_page(Relation index, Buffer metabuf,
				   BlockNumber *new_free_list_head_out)
{
	RoaringMetaPageData *meta = RoaringPageGetMeta(BufferGetPage(metabuf));
	BlockNumber			 head = meta->free_list_head;

	if (head != InvalidBlockNumber)
	{
		Buffer				buf  = ReadBuffer(index, head);
		RoaringFreeSpecial *spc;

		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		spc = (RoaringFreeSpecial *) PageGetSpecialPointer(BufferGetPage(buf));
		*new_free_list_head_out = spc->next_free;
		return buf;
	}

	*new_free_list_head_out = InvalidBlockNumber;
	return ExtendBufferedRel(BMR_REL(index), MAIN_FORKNUM, NULL, EB_LOCK_FIRST);
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
	spc->_pad		 = 0;
	spc->value_min	 = PG_INT64_MAX;
	spc->value_max	 = PG_INT64_MIN;

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

	/* Initialise all overflow pages (no WAL yet). */
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

		MarkBufferDirty(bufs[i]);
	}

	/*
	 * WAL-log the entire chain as one record instead of one record per page.
	 * For a 50-page overflow chain this collapses 50 log_newpage_buffer calls
	 * into a single log_newpages call.
	 */
	if (RelationNeedsWAL(index))
	{
		Page *pages = (Page *) palloc(npages * sizeof(Page));

		for (i = 0; i < npages; i++)
			pages[i] = BufferGetPage(bufs[i]);

		log_newpages(&index->rd_locator, MAIN_FORKNUM,
					 npages, blknos, pages, true);
		pfree(pages);
	}

	for (i = 0; i < npages; i++)
		UnlockReleaseBuffer(bufs[i]);

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

	/* Walk overflow chain, prefetching one page ahead to pipeline I/O. */
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
		if (cur != InvalidBlockNumber)
			PrefetchBuffer(index, MAIN_FORKNUM, cur);
		UnlockReleaseBuffer(ovbuf);
	}

	bm = roaring_bitmap_portable_deserialize_safe(buf, total_len);
	pfree(buf);
	return bm;
}
