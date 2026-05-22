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
