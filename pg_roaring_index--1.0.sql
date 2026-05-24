\echo Use "CREATE EXTENSION pg_roaring_index" to load this file. \quit

-- AM handler function
CREATE FUNCTION pg_roaring_index_handler(internal)
    RETURNS index_am_handler
    AS 'MODULE_PATHNAME', 'roaring_handler'
    LANGUAGE C;

-- Register the access method
CREATE ACCESS METHOD roaring
    TYPE INDEX
    HANDLER pg_roaring_index_handler;

-- Equality strategy for the operator class
-- Strategy 1 = equality (standard convention)

-- Operator class: exact (TID-level, Roaring64) — Phase 1 default
CREATE OPERATOR CLASS roaring_int8_tid_ops
    DEFAULT FOR TYPE int8 USING roaring AS
    OPERATOR 1 =(int8, int8);

-- Cross-type: bigint col = integer literal (e.g. WHERE col_int8 = 42)
ALTER OPERATOR FAMILY roaring_int8_tid_ops USING roaring ADD
    OPERATOR 1 =(int8, int4);

-- int4 variant: DatumGetInt64 is safe for int4 on 64-bit (PG sign-extends)
CREATE OPERATOR CLASS roaring_int4_tid_ops
    DEFAULT FOR TYPE int4 USING roaring AS
    OPERATOR 1 =(int4, int4);

-- Cross-type: integer col = bigint literal (e.g. WHERE col_int4 = 42::bigint)
ALTER OPERATOR FAMILY roaring_int4_tid_ops USING roaring ADD
    OPERATOR 1 =(int4, int8);

CREATE OPERATOR CLASS roaring_int2_tid_ops
    DEFAULT FOR TYPE int2 USING roaring AS
    OPERATOR 1 =(int2, int2);

-- Cross-type: smallint col compared to integer/bigint literals
ALTER OPERATOR FAMILY roaring_int2_tid_ops USING roaring ADD
    OPERATOR 1 =(int2, int4),
    OPERATOR 1 =(int2, int8);

CREATE OPERATOR CLASS roaring_bool_tid_ops
    DEFAULT FOR TYPE bool USING roaring AS
    OPERATOR 1 =(boolean, boolean);

CREATE OPERATOR CLASS roaring_date_tid_ops
    DEFAULT FOR TYPE date USING roaring AS
    OPERATOR 1 =(date, date);

CREATE OPERATOR CLASS roaring_float4_tid_ops
    DEFAULT FOR TYPE float4 USING roaring AS
    OPERATOR 1 =(float4, float4);

CREATE OPERATOR CLASS roaring_oid_tid_ops
    DEFAULT FOR TYPE oid USING roaring AS
    OPERATOR 1 =(oid, oid);

-- Hash-keyed types (prototype): text, varchar, uuid.
-- Keys are hash_any(bytes) → int32; collisions are theoretically possible
-- but negligible at low-to-moderate cardinality (< ~100K distinct values).
CREATE OPERATOR CLASS roaring_text_tid_ops
    DEFAULT FOR TYPE text USING roaring AS
    OPERATOR 1 =(text, text);

CREATE OPERATOR CLASS roaring_uuid_tid_ops
    DEFAULT FOR TYPE uuid USING roaring AS
    OPERATOR 1 =(uuid, uuid);

-- ----------------------------------------------------------------
-- Lossy (page-level) opclasses — roaring_page_ops
--
-- Stores block numbers instead of TIDs.  amrecheck=true so the executor
-- rechecks heap tuples on each matched page.  Index is 10-100x smaller
-- than the exact variant at low cardinality; best for multi-column AND
-- queries or when index size dominates (e.g. ndistinct < 1000).
-- ----------------------------------------------------------------

CREATE FUNCTION pg_roaring_page_handler(internal)
    RETURNS index_am_handler
    AS 'MODULE_PATHNAME', 'roaring_page_handler'
    LANGUAGE C;

CREATE ACCESS METHOD roaring_lossy
    TYPE INDEX
    HANDLER pg_roaring_page_handler;

CREATE OPERATOR CLASS roaring_int8_page_ops
    DEFAULT FOR TYPE int8 USING roaring_lossy AS
    OPERATOR 1 =(int8, int8);

ALTER OPERATOR FAMILY roaring_int8_page_ops USING roaring_lossy ADD
    OPERATOR 1 =(int8, int4);

CREATE OPERATOR CLASS roaring_int4_page_ops
    DEFAULT FOR TYPE int4 USING roaring_lossy AS
    OPERATOR 1 =(int4, int4);

ALTER OPERATOR FAMILY roaring_int4_page_ops USING roaring_lossy ADD
    OPERATOR 1 =(int4, int8);

CREATE OPERATOR CLASS roaring_int2_page_ops
    DEFAULT FOR TYPE int2 USING roaring_lossy AS
    OPERATOR 1 =(int2, int2);

ALTER OPERATOR FAMILY roaring_int2_page_ops USING roaring_lossy ADD
    OPERATOR 1 =(int2, int4),
    OPERATOR 1 =(int2, int8);

CREATE OPERATOR CLASS roaring_bool_page_ops
    DEFAULT FOR TYPE bool USING roaring_lossy AS
    OPERATOR 1 =(boolean, boolean);

CREATE OPERATOR CLASS roaring_date_page_ops
    DEFAULT FOR TYPE date USING roaring_lossy AS
    OPERATOR 1 =(date, date);

CREATE OPERATOR CLASS roaring_float4_page_ops
    DEFAULT FOR TYPE float4 USING roaring_lossy AS
    OPERATOR 1 =(float4, float4);

CREATE OPERATOR CLASS roaring_oid_page_ops
    DEFAULT FOR TYPE oid USING roaring_lossy AS
    OPERATOR 1 =(oid, oid);

CREATE OPERATOR CLASS roaring_text_page_ops
    DEFAULT FOR TYPE text USING roaring_lossy AS
    OPERATOR 1 =(text, text);

CREATE OPERATOR CLASS roaring_uuid_page_ops
    DEFAULT FOR TYPE uuid USING roaring_lossy AS
    OPERATOR 1 =(uuid, uuid);

-- ----------------------------------------------------------------
-- Observability
-- ----------------------------------------------------------------

-- roaring_index_stats: read the metapage of a roaring/roaring_lossy index
-- and return one diagnostic row.
CREATE FUNCTION roaring_index_stats(
    indexrelid regclass,
    OUT total_entries   bigint,
    OUT pending_count   bigint,
    OUT pending_threshold int,
    OUT is_lossy        bool,
    OUT bg_merge_running bool,
    OUT index_version   int,
    OUT root_blkno      bigint,
    OUT leftmost_leaf   bigint,
    OUT free_list_head  bigint
) RETURNS RECORD
    AS 'MODULE_PATHNAME', 'roaring_index_stats'
    LANGUAGE C STRICT;

-- pg_stat_roaring_indexes: one row per roaring/roaring_lossy index.
CREATE VIEW pg_stat_roaring_indexes AS
    SELECT
        c.oid              AS indexrelid,
        n.nspname          AS schemaname,
        c.relname          AS indexrelname,
        am.amname,
        s.total_entries,
        s.pending_count,
        s.pending_threshold,
        s.is_lossy,
        s.bg_merge_running,
        s.index_version,
        s.root_blkno,
        s.leftmost_leaf,
        s.free_list_head
    FROM pg_class c
    JOIN pg_namespace n  ON n.oid  = c.relnamespace
    JOIN pg_am am        ON am.oid = c.relam
    JOIN LATERAL roaring_index_stats(c.oid) s ON true
    WHERE am.amname IN ('roaring', 'roaring_lossy')
      AND c.relkind = 'i';

-- roaring_index_check: structural integrity check (amcheck equivalent).
-- Pass heapallindexed => true to also verify every index entry has a live
-- heap tuple (not yet implemented; currently raises a NOTICE).
CREATE FUNCTION roaring_index_check(
    index           regclass,
    heapallindexed  boolean DEFAULT false
) RETURNS void
    STRICT
    LANGUAGE C
    AS 'MODULE_PATHNAME', 'roaring_index_check';

-- roaring_index_health: simple textual health probe.
CREATE FUNCTION roaring_index_health(regclass) RETURNS text
    LANGUAGE sql STRICT
    AS $$
        SELECT
            CASE
                WHEN NOT bg_merge_running AND pending_count >= pending_threshold
                    THEN 'merge overdue'
                WHEN bg_merge_running AND pending_count >= pending_threshold
                    THEN 'merge running (behind)'
                WHEN bg_merge_running
                    THEN 'merge in progress'
                WHEN pending_count > 0
                    THEN 'pending inserts'
                ELSE 'ok'
            END
        FROM roaring_index_stats($1)
    $$;
