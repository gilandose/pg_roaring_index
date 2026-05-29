-- roaring_include.sql
--
-- T65: INCLUDE column payload store.
--
-- Tests: INDEX ... INCLUDE (bigint_col) with build + insert + VACUUM paths,
-- IndexOnlyScan plan shape, correct payload retrieval, DELETE + re-VACUUM
-- correctness, and IS NULL behaviour on the key column.

CREATE EXTENSION IF NOT EXISTS pg_roaring_index;

-- ================================================================
-- 1. Basic build: INCLUDE column populated during ambuild
-- ================================================================
CREATE TABLE t_inc (id bigint, val bigint);
INSERT INTO t_inc VALUES
    (101, 10),
    (102, 10),
    (103, 20),
    (104, 30);

-- val is the key; id is stored as INCLUDE payload.
CREATE INDEX t_inc_idx ON t_inc USING roaring (val) INCLUDE (id);
VACUUM t_inc;

SET enable_seqscan = off;

-- Plan shape: IndexScan is expected (key column val is not returnable
-- for IndexOnlyScan since NULLs are not stored; INCLUDE column id IS
-- returnable, so an IndexOnlyScan for "SELECT id ... WHERE val = X" may
-- be chosen after VACUUM warms the visibility map).
EXPLAIN (COSTS OFF) SELECT id FROM t_inc WHERE val = 10 ORDER BY id;

-- Correct values returned from payload.
SELECT id FROM t_inc WHERE val = 10 ORDER BY id;   -- 101, 102
SELECT id FROM t_inc WHERE val = 20 ORDER BY id;   -- 103
SELECT id FROM t_inc WHERE val = 99 ORDER BY id;   -- (empty)

-- ================================================================
-- 2. Pending list: aminsert populates payload before pending entry
-- ================================================================
INSERT INTO t_inc VALUES (105, 10);  -- goes to pending list

-- id=105 should be visible via pending scan and return the correct payload.
SELECT id FROM t_inc WHERE val = 10 ORDER BY id;   -- 101, 102, 105

VACUUM t_inc;
-- After merge: same results from leaf bitmaps.
SELECT id FROM t_inc WHERE val = 10 ORDER BY id;   -- 101, 102, 105

-- ================================================================
-- 3. DELETE + VACUUM: deleted TIDs removed from bitmap
-- ================================================================
DELETE FROM t_inc WHERE id = 101;
VACUUM t_inc;

SELECT id FROM t_inc WHERE val = 10 ORDER BY id;   -- 102, 105 only

-- ================================================================
-- 4. Multi-row update: DELETE old TID + INSERT new TID
-- ================================================================
UPDATE t_inc SET id = 999 WHERE id = 103;
VACUUM t_inc;

SELECT id FROM t_inc WHERE val = 20 ORDER BY id;   -- 999

-- ================================================================
-- 5. IS NULL on key column uses heap (NULLs not indexed)
-- ================================================================
RESET enable_seqscan;
INSERT INTO t_inc VALUES (NULL, NULL);  -- NULL key: not indexed

SELECT count(*) AS null_val FROM t_inc WHERE val IS NULL;  -- 1 via seqscan

-- ================================================================
-- 6. int4 key column with bigint INCLUDE
-- ================================================================
CREATE TABLE t_inc4 (pk bigint, k int4);
INSERT INTO t_inc4 SELECT i * 100, (i % 5) + 1 FROM generate_series(1, 20) i;
CREATE INDEX t_inc4_idx ON t_inc4 USING roaring (k) INCLUDE (pk);
VACUUM t_inc4;

SET enable_seqscan = off;

SELECT pk FROM t_inc4 WHERE k = 1 ORDER BY pk;  -- 500, 1000, 1500, 2000

-- Pending path.
INSERT INTO t_inc4 VALUES (9900, 1);
SELECT pk FROM t_inc4 WHERE k = 1 ORDER BY pk;  -- 500, 1000, 1500, 2000, 9900

VACUUM t_inc4;
SELECT pk FROM t_inc4 WHERE k = 1 ORDER BY pk;  -- same

-- ================================================================
-- Cleanup
-- ================================================================
RESET enable_seqscan;
DROP TABLE t_inc;
DROP TABLE t_inc4;
