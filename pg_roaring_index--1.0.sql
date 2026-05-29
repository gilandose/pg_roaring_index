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

-- Cross-type: both directions so the planner can use the index regardless
-- of whether the literal is on the left or right of the operator.
ALTER OPERATOR FAMILY roaring_int8_tid_ops USING roaring ADD
    OPERATOR 1 =(int8, int4),
    OPERATOR 1 =(int8, int2),
    OPERATOR 1 =(int4, int8),
    OPERATOR 1 =(int2, int8);

CREATE OPERATOR CLASS roaring_int4_tid_ops
    DEFAULT FOR TYPE int4 USING roaring AS
    OPERATOR 1 =(int4, int4);

ALTER OPERATOR FAMILY roaring_int4_tid_ops USING roaring ADD
    OPERATOR 1 =(int4, int8),
    OPERATOR 1 =(int4, int2),
    OPERATOR 1 =(int8, int4),
    OPERATOR 1 =(int2, int4);

CREATE OPERATOR CLASS roaring_int2_tid_ops
    DEFAULT FOR TYPE int2 USING roaring AS
    OPERATOR 1 =(int2, int2);

ALTER OPERATOR FAMILY roaring_int2_tid_ops USING roaring ADD
    OPERATOR 1 =(int2, int4),
    OPERATOR 1 =(int2, int8),
    OPERATOR 1 =(int4, int2),
    OPERATOR 1 =(int8, int2);

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

-- Enum types: stored internally as Oid; key extraction is identical to oid.
CREATE OPERATOR CLASS roaring_enum_tid_ops
    DEFAULT FOR TYPE anyenum USING roaring AS
    OPERATOR 1 =(anyenum, anyenum);

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
-- Observability
-- ----------------------------------------------------------------

-- roaring_index_stats: read the metapage of a roaring index and return one diagnostic row.
CREATE FUNCTION roaring_index_stats(
    indexrelid regclass,
    OUT total_entries   bigint,
    OUT pending_count   bigint,
    OUT pending_threshold int,
    OUT bg_merge_running bool,
    OUT index_version   int,
    OUT root_blkno      bigint,
    OUT leftmost_leaf   bigint,
    OUT free_list_head  bigint
) RETURNS RECORD
    AS 'MODULE_PATHNAME', 'roaring_index_stats'
    LANGUAGE C STRICT;

-- pg_stat_roaring_indexes: one row per roaring index.
CREATE VIEW pg_stat_roaring_indexes AS
    SELECT
        c.oid              AS indexrelid,
        n.nspname          AS schemaname,
        c.relname          AS indexrelname,
        am.amname,
        s.total_entries,
        s.pending_count,
        s.pending_threshold,
        s.bg_merge_running,
        s.index_version,
        s.root_blkno,
        s.leftmost_leaf,
        s.free_list_head
    FROM pg_class c
    JOIN pg_namespace n  ON n.oid  = c.relnamespace
    JOIN pg_am am        ON am.oid = c.relam
    JOIN LATERAL roaring_index_stats(c.oid) s ON true
    WHERE am.amname = 'roaring'
      AND c.relkind = 'i';

-- roaring_index_check: structural integrity check (amcheck equivalent).
-- Verifies page types, sort order, overflow chains, pending lists, and the
-- free list.  heapallindexed is not yet implemented; passing true raises
-- ERROR (feature_not_supported).  Only call with the default false.
--
-- Concurrent-writer caveat: the check holds only AccessShareLock, so
-- concurrent inserts and merges can advance pointer fields between the
-- metapage snapshot and the chain walks, potentially causing spurious
-- errors on a heavily-written index.  Run on a quiesced or read-only index
-- for a definitive result.
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
