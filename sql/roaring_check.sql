-- roaring_check.sql — amcheck, IN-list, cross-type operators, multi-level index
--
-- Covers T36 items: amcheck on multi-level indexes, IN-list edge cases,
-- cross-type operator families, rescan with different key counts.

CREATE EXTENSION IF NOT EXISTS pg_roaring_index;

SET enable_seqscan = off;
SET enable_bitmapscan = on;

-- ================================================================
-- 1. amcheck on a two-level directory index
--
-- Need > max_dir (~510) distinct values so the directory has two levels.
-- Generate 600 distinct values; each gets its own leaf entry.
-- ================================================================
CREATE TABLE chk_big (id int, val bigint);
INSERT INTO chk_big SELECT i, i FROM generate_series(1, 600) i;
CREATE INDEX chk_big_idx ON chk_big USING roaring (val);
-- Build writes leaf + dir pages directly; no pending list to merge.

-- Structural check: must return without error.
SELECT roaring_index_check('chk_big_idx'::regclass);
SELECT roaring_index_check('chk_big_idx'::regclass, false);  -- heapallindexed=false

-- Spot-check a few values.
SELECT count(*) AS cnt_300 FROM chk_big WHERE val = 300;
SELECT count(*) AS cnt_601 FROM chk_big WHERE val = 601;  -- absent

DROP TABLE chk_big;

-- ================================================================
-- 2. IN-list (SK_SEARCHARRAY) edge cases
-- ================================================================
CREATE TABLE t_in (id int, val bigint);
INSERT INTO t_in SELECT i, (i % 10) + 1 FROM generate_series(1, 50) i;
CREATE INDEX t_in_idx ON t_in USING roaring (val);
VACUUM t_in;

-- Normal IN list.
SELECT count(*) AS in_1_3 FROM t_in WHERE val IN (1, 3);       -- 5+5=10

-- Single-element IN (planner may rewrite to =, but index must handle both).
SELECT count(*) AS in_single FROM t_in WHERE val IN (5);        -- 5

-- IN list with absent values.
SELECT count(*) AS in_absent FROM t_in WHERE val IN (11, 12);   -- 0

-- IN list where one element is present, others absent.
SELECT count(*) AS in_mixed FROM t_in WHERE val IN (2, 99, 100); -- 5

-- IN list with all 10 values.
SELECT count(*) AS in_all FROM t_in WHERE val IN (1,2,3,4,5,6,7,8,9,10); -- 50

DROP TABLE t_in;

-- ================================================================
-- 3. Cross-type operator families
--
-- Operators =(int8,int4), =(int8,int2) etc. were added in T27.
-- Verify the planner chooses the index for cross-type comparisons.
-- ================================================================
CREATE TABLE t_cross (id int, v8 bigint, v4 int, v2 smallint);
INSERT INTO t_cross VALUES (1,100,100,100),(2,200,200,200),(3,300,300,300);

CREATE INDEX t_cross_v8 ON t_cross USING roaring (v8);
CREATE INDEX t_cross_v4 ON t_cross USING roaring (v4);
CREATE INDEX t_cross_v2 ON t_cross USING roaring (v2);
VACUUM t_cross;

-- bigint col vs int4 literal (no cast needed in standard SQL).
SELECT id FROM t_cross WHERE v8 = 200::int4 ORDER BY id;        -- 2
SELECT id FROM t_cross WHERE v8 = 300::int2 ORDER BY id;        -- 3

-- int4 col vs bigint literal.
SELECT id FROM t_cross WHERE v4 = 100::int8 ORDER BY id;        -- 1

-- int2 col vs int4 literal.
SELECT id FROM t_cross WHERE v2 = 200::int4 ORDER BY id;        -- 2

DROP TABLE t_cross;

-- ================================================================
-- 4. Rescan: re-use the same scan with different key values
-- ================================================================
CREATE TABLE t_rescan (id int, val bigint);
INSERT INTO t_rescan SELECT i, (i % 5) + 1 FROM generate_series(1, 25) i;
CREATE INDEX t_rescan_idx ON t_rescan USING roaring (val);
VACUUM t_rescan;

-- Simulate rescan via two sequential queries (regression coverage for
-- the bitmap_loaded flag reset path in roaring_rescan).
SELECT count(*) AS rs1 FROM t_rescan WHERE val = 1;  -- 5
SELECT count(*) AS rs2 FROM t_rescan WHERE val = 2;  -- 5
SELECT count(*) AS rs3 FROM t_rescan WHERE val = 6;  -- 0 (absent)

DROP TABLE t_rescan;

-- ================================================================
-- 5. Pending-list scan: values visible immediately after INSERT
--    (no VACUUM) and again after VACUUM merges them.
-- ================================================================
CREATE TABLE t_pending_scan (id int, val bigint);
CREATE INDEX t_ps_idx ON t_pending_scan USING roaring (val);

INSERT INTO t_pending_scan SELECT i, (i % 5) + 1 FROM generate_series(1, 20) i;
-- Pending list only (not yet merged); values must still be visible.
SELECT count(*) AS ps_before_vacuum FROM t_pending_scan WHERE val = 1;  -- 4

VACUUM t_pending_scan;
-- After merge: same count from main index.
SELECT count(*) AS ps_after_vacuum FROM t_pending_scan WHERE val = 1;   -- 4

-- IN list from pending list.
INSERT INTO t_pending_scan SELECT 100 + i, (i % 3) + 6 FROM generate_series(1, 9) i;
SELECT count(*) AS ps_in FROM t_pending_scan WHERE val IN (6, 7, 8);    -- 9

DROP TABLE t_pending_scan;

RESET enable_seqscan;
RESET enable_bitmapscan;
