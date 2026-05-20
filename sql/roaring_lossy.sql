-- Regression tests for pg_roaring_index lossy (page-level) opclass.
-- Covers: build, equality lookup, IN() query, pending list, VACUUM merge,
-- delete+reinsert, int4 opclass.  Uses roaring_lossy AM (amrecheck=true).

-- Extension already loaded by roaring_basic.sql in the same regression run,
-- but if run standalone we need it.
CREATE EXTENSION IF NOT EXISTS pg_roaring_index;
SELECT amname FROM pg_am WHERE amname = 'roaring_lossy';

-- ----------------------------------------------------------------
-- 1. Basic build and equality lookups
-- ----------------------------------------------------------------
CREATE TABLE lossy_test (id int, val bigint);
INSERT INTO lossy_test VALUES
    (1, 1), (2, 1),
    (3, 2),
    (4, 3), (5, 3), (6, 3);

CREATE INDEX lossy_test_idx ON lossy_test USING roaring_lossy (val);

VACUUM lossy_test;

SET enable_seqscan = off;

SELECT count(*) AS cnt_val1 FROM lossy_test WHERE val = 1;
SELECT count(*) AS cnt_val2 FROM lossy_test WHERE val = 2;
SELECT count(*) AS cnt_val3 FROM lossy_test WHERE val = 3;
SELECT count(*) AS cnt_val99 FROM lossy_test WHERE val = 99;

-- ----------------------------------------------------------------
-- 2. Plan shape — Bitmap Heap Scan with Recheck Cond
-- ----------------------------------------------------------------
EXPLAIN (COSTS OFF) SELECT id FROM lossy_test WHERE val = 1;

-- ----------------------------------------------------------------
-- 3. IN() / SK_SEARCHARRAY path
-- ----------------------------------------------------------------
SELECT count(*) AS cnt_in FROM lossy_test WHERE val IN (1, 3);

-- ----------------------------------------------------------------
-- 4. Pending list visible within the same transaction
-- ----------------------------------------------------------------
INSERT INTO lossy_test VALUES (8, 4), (9, 4), (10, 4);
SELECT count(*) AS cnt_val4_pending FROM lossy_test WHERE val = 4;

INSERT INTO lossy_test VALUES (11, 1);
SELECT count(*) AS cnt_val1_mixed FROM lossy_test WHERE val = 1;

-- ----------------------------------------------------------------
-- 5. VACUUM merges pending list
-- ----------------------------------------------------------------
VACUUM lossy_test;

SELECT count(*) AS cnt_val1_post_merge FROM lossy_test WHERE val = 1;
SELECT count(*) AS cnt_val4_post_merge FROM lossy_test WHERE val = 4;

-- ----------------------------------------------------------------
-- 6. Delete + reinsert (empty-bitmap path after merge)
-- ----------------------------------------------------------------
DELETE FROM lossy_test WHERE val = 2;
INSERT INTO lossy_test VALUES (20, 2), (21, 2);
VACUUM lossy_test;
SELECT count(*) AS cnt_val2_reinserted FROM lossy_test WHERE val = 2;

-- val=4 unaffected
SELECT count(*) AS cnt_val4_stable FROM lossy_test WHERE val = 4;

-- ----------------------------------------------------------------
-- 7. int4 opclass
-- ----------------------------------------------------------------
CREATE TABLE lossy_int4 (val int4);
INSERT INTO lossy_int4 SELECT generate_series(1, 5);
CREATE INDEX lossy_int4_idx ON lossy_int4 USING roaring_lossy (val);
VACUUM lossy_int4;

SELECT count(*) AS cnt_int4_3  FROM lossy_int4 WHERE val = 3;
SELECT count(*) AS cnt_int4_10 FROM lossy_int4 WHERE val = 10;

-- ----------------------------------------------------------------
-- Cleanup
-- ----------------------------------------------------------------
RESET enable_seqscan;
DROP TABLE lossy_test;
DROP TABLE lossy_int4;
