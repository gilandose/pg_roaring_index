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

-- int4 variant: DatumGetInt64 is safe for int4 on 64-bit (PG sign-extends)
CREATE OPERATOR CLASS roaring_int4_tid_ops
    DEFAULT FOR TYPE int4 USING roaring AS
    OPERATOR 1 =(int4, int4);
