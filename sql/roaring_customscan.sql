-- roaring_customscan.sql
--
-- T64: CustomScan (RoaringCount) count(*) shortcut.
--
-- Covers: plan shape, correctness (no-pending fast path, pending slow path,
-- IN list, post-REINDEX CONCURRENTLY correctness), lossy AM fallback,
-- multi-column index fallback, cross-check against seq scan.

CREATE EXTENSION IF NOT EXISTS pg_roaring_index;

-- ================================================================
-- 1. Plan shape
--
-- 1000 rows, 10 distinct values (100 rows each).  The CustomScan
-- cost (1.0) beats Aggregate + BitmapIndexScan at this scale.
-- ================================================================
CREATE TABLE t_cs (id int, val bigint);
INSERT INTO t_cs SELECT i, (i % 10) + 1 FROM generate_series(1, 1000) i;
CREATE INDEX t_cs_idx ON t_cs USING roaring (val);
VACUUM t_cs;

SET enable_seqscan = off;

-- Custom Scan (RoaringCount) is chosen over Aggregate + index scan.
EXPLAIN (COSTS OFF) SELECT count(*) FROM t_cs WHERE val = 3;

-- IN list: still a RoaringCount (sum of disjoint bitmaps).
EXPLAIN (COSTS OFF) SELECT count(*) FROM t_cs WHERE val IN (1, 2, 3);

-- Lossy AM: RoaringCount does not fire for roaring_lossy
-- (block-level bitmaps don't give exact row counts).
CREATE TABLE t_cs_lo (id int, val bigint);
INSERT INTO t_cs_lo SELECT i, (i % 10) + 1 FROM generate_series(1, 1000) i;
CREATE INDEX t_cs_lo_idx ON t_cs_lo USING roaring_lossy (val);
VACUUM t_cs_lo;
EXPLAIN (COSTS OFF) SELECT count(*) FROM t_cs_lo WHERE val = 3;
DROP TABLE t_cs_lo;

-- Multi-column index: RoaringCount does not fire
-- (intersection semantics; column-sum ≠ row count).
CREATE TABLE t_cs2 (id int, a int4, b int4);
INSERT INTO t_cs2 SELECT i, (i % 5) + 1, (i % 3) + 1
                  FROM generate_series(1, 500) i;
CREATE INDEX t_cs2_idx ON t_cs2 USING roaring (a, b);
VACUUM t_cs2;
EXPLAIN (COSTS OFF) SELECT count(*) FROM t_cs2 WHERE a = 1 AND b = 1;
DROP TABLE t_cs2;

-- ================================================================
-- 2. Correctness (fast path: no pending list)
-- ================================================================
SELECT count(*) AS cnt_1   FROM t_cs WHERE val = 1;     -- 100
SELECT count(*) AS cnt_10  FROM t_cs WHERE val = 10;    -- 100
SELECT count(*) AS cnt_99  FROM t_cs WHERE val = 99;    -- 0 (absent)

-- IN list: sums of disjoint value bitmaps.
SELECT count(*) AS in_2  FROM t_cs WHERE val IN (1, 2);           -- 200
SELECT count(*) AS in_4  FROM t_cs WHERE val IN (3, 5, 7, 9);    -- 400
SELECT count(*) AS in_all FROM t_cs WHERE val IN (1,2,3,4,5,6,7,8,9,10); -- 1000

-- ================================================================
-- 3. Correctness (slow path: pending list present)
--
-- When pending entries exist, roaring_count_exec builds the actual
-- leaf OR pending bitmap so duplicate TIDs collapse (idempotent OR).
-- ================================================================
INSERT INTO t_cs SELECT 2000 + i, 99 FROM generate_series(1, 50) i;
-- val=99 lives only in pending (not yet merged).
SELECT count(*) AS pend_99  FROM t_cs WHERE val = 99;   -- 50
-- Mix: val=1 from leaf, val=99 from pending.
SELECT count(*) AS pend_mix FROM t_cs WHERE val IN (1, 99); -- 150

VACUUM t_cs;
-- Post-merge: same counts from main index (no pending).
SELECT count(*) AS post_99  FROM t_cs WHERE val = 99;   -- 50
SELECT count(*) AS post_mix FROM t_cs WHERE val IN (1, 99); -- 150

-- ================================================================
-- 4. REINDEX CONCURRENTLY: no double-count
--
-- REINDEX re-inserts every heap row via aminsert during its second
-- heap scan, creating pending entries that duplicate leaf TIDs.
-- roaring_count_key_exact deduplicates via idempotent OR so the
-- count stays at 4, not 8.
-- ================================================================
CREATE TABLE t_cs_ri (id int, val bigint);
INSERT INTO t_cs_ri SELECT i, (i % 3) + 1 FROM generate_series(1, 12) i;
CREATE INDEX t_cs_ri_idx ON t_cs_ri USING roaring (val);
VACUUM t_cs_ri;

SELECT count(*) AS before_ri FROM t_cs_ri WHERE val = 1;  -- 4

REINDEX INDEX CONCURRENTLY t_cs_ri_idx;

SELECT count(*) AS after_ri FROM t_cs_ri WHERE val = 1;   -- 4 (not 8)

-- amgetbitmap must also return 4 (always idempotent via OR).
SET enable_indexscan = off;
SELECT count(*) AS bitmap_ri FROM t_cs_ri WHERE val = 1;  -- 4
RESET enable_indexscan;

DROP TABLE t_cs_ri;

-- ================================================================
-- 5. Cross-check: CustomScan result matches sequential scan
-- ================================================================
SELECT count(*) AS idx_5 FROM t_cs WHERE val = 5;         -- 100 via CustomScan

RESET enable_seqscan;
SET enable_bitmapscan = off;
SET enable_indexscan  = off;
SELECT count(*) AS seq_5 FROM t_cs WHERE val = 5;         -- 100 via SeqScan
RESET enable_bitmapscan;
RESET enable_indexscan;

-- ================================================================
-- Cleanup
-- ================================================================
RESET enable_seqscan;
DROP TABLE t_cs;
