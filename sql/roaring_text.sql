-- roaring_text.sql — hash-keyed type regression tests: text, varchar, uuid
--
-- These types use hash_any(bytes) → int32 as the bitmap key.  Collisions are
-- theoretically possible but negligible at small cardinality.  Tests verify
-- round-trip correctness for distinct low-cardinality values.

CREATE EXTENSION IF NOT EXISTS pg_roaring_index;

-- ----------------------------------------------------------------
-- text — exact (TID-level)
-- ----------------------------------------------------------------
CREATE TABLE t_text (id int, status text);
INSERT INTO t_text VALUES
    (1, 'active'), (2, 'inactive'), (3, 'active'),
    (4, 'pending'), (5, 'active'), (6, 'inactive');

CREATE INDEX t_text_idx ON t_text USING roaring (status);

SET enable_seqscan = off;
SELECT id FROM t_text WHERE status = 'active'   ORDER BY id;
SELECT id FROM t_text WHERE status = 'inactive' ORDER BY id;
SELECT id FROM t_text WHERE status = 'pending'  ORDER BY id;
SELECT id FROM t_text WHERE status = 'missing'  ORDER BY id;

-- IN query (SK_SEARCHARRAY)
SELECT id FROM t_text WHERE status IN ('active', 'pending') ORDER BY id;

RESET enable_seqscan;
DROP TABLE t_text;

-- ----------------------------------------------------------------
-- text — lossy (page-level)
-- ----------------------------------------------------------------
CREATE TABLE t_text_lo (id int, status text);
INSERT INTO t_text_lo VALUES
    (1, 'active'), (2, 'inactive'), (3, 'active'),
    (4, 'pending'), (5, 'active'), (6, 'inactive');

CREATE INDEX t_text_lo_idx ON t_text_lo USING roaring_lossy (status);

SET enable_seqscan = off;
SELECT id FROM t_text_lo WHERE status = 'active'   ORDER BY id;
SELECT id FROM t_text_lo WHERE status = 'inactive' ORDER BY id;
RESET enable_seqscan;
DROP TABLE t_text_lo;

-- ----------------------------------------------------------------
-- uuid
-- ----------------------------------------------------------------
CREATE TABLE t_uuid (id int, tenant_id uuid);
INSERT INTO t_uuid VALUES
    (1, 'a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11'),
    (2, 'b0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11'),
    (3, 'a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11'),
    (4, 'c0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11'),
    (5, 'a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11');

CREATE INDEX t_uuid_idx ON t_uuid USING roaring (tenant_id);

SET enable_seqscan = off;
SELECT id FROM t_uuid WHERE tenant_id = 'a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11' ORDER BY id;
SELECT id FROM t_uuid WHERE tenant_id = 'b0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11' ORDER BY id;
SELECT id FROM t_uuid WHERE tenant_id = 'ffffffff-ffff-ffff-ffff-ffffffffffff' ORDER BY id;

-- IN query
SELECT id FROM t_uuid
WHERE tenant_id IN (
    'a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11',
    'b0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11'
) ORDER BY id;

RESET enable_seqscan;
DROP TABLE t_uuid;

-- ----------------------------------------------------------------
-- multi-column: text + int (mixed hash + exact)
-- ----------------------------------------------------------------
CREATE TABLE t_mixed (id int, status text, val int4);
INSERT INTO t_mixed VALUES
    (1, 'active', 10), (2, 'active', 20), (3, 'inactive', 10),
    (4, 'inactive', 20), (5, 'active', 10), (6, 'pending', 30);

CREATE INDEX t_mixed_idx ON t_mixed USING roaring (status, val);

SET enable_seqscan = off;
SELECT id FROM t_mixed WHERE status = 'active'   AND val = 10 ORDER BY id;
SELECT id FROM t_mixed WHERE status = 'inactive' AND val = 20 ORDER BY id;
SELECT id FROM t_mixed WHERE status = 'pending'  AND val = 30 ORDER BY id;
RESET enable_seqscan;
DROP TABLE t_mixed;
