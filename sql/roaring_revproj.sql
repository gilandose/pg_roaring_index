-- ================================================================
-- roaring_revproj: projecting an UNCONSTRAINED key column
--
-- A roaring key column can be returned by an IndexOnlyScan only when it is
-- pinned by an equality clause.  Projecting a key column with no equality
-- clause (SELECT a WHERE b=5, or a column constrained only by IN / IS NOT NULL)
-- cannot be reconstructed from the index, so the planner suppresses the
-- IndexOnlyScan for that shape (roaring_suppress_unreturnable_ios) and uses a
-- bitmap/heap/seq scan that projects the column correctly from the heap.
--
-- The common fast paths — INCLUDE projection (sum(id)) and equality-pinned key
-- projection — must stay index-only.  Correctness is cross-checked against a
-- forced-seqscan oracle.
-- ================================================================
CREATE EXTENSION IF NOT EXISTS pg_roaring_index;

CREATE TABLE rv (id bigint, a int, b int);
-- a and b independent: for a fixed b, a ranges over 0..49
INSERT INTO rv SELECT g, g % 50, (g / 50) % 40
               FROM generate_series(1, 40000) g;
CREATE INDEX rv_idx ON rv USING roaring (a, b) INCLUDE (id);
VACUUM ANALYZE rv;

-- ----------------------------------------------------------------
-- Fast paths stay index-only.
-- ----------------------------------------------------------------
-- INCLUDE projection: the flagship sum(id) path.
EXPLAIN (COSTS OFF) SELECT sum(id) FROM rv WHERE b = 7;
-- Equality-pinned key projection: a is reconstructable from a = 5.
EXPLAIN (COSTS OFF) SELECT sum(id) FROM rv WHERE a = 5;

-- ----------------------------------------------------------------
-- Unconstrained key projection is NOT index-only, and is correct.
-- ----------------------------------------------------------------
-- a has no equality clause here, so the IndexOnlyScan is suppressed.
SELECT 'idx' AS src, a, count(*) FROM rv WHERE b = 7 GROUP BY a ORDER BY a LIMIT 5;
-- projecting a constrained only by IS NOT NULL is likewise not reconstructable.
SELECT 'idx' AS src, a, count(*) FROM rv WHERE b = 7 AND a IS NOT NULL
  GROUP BY a ORDER BY a LIMIT 5;

-- Oracle: force a sequential scan and compare.
SET enable_indexscan = off;
SET enable_bitmapscan = off;
SET enable_indexonlyscan = off;
SELECT 'seq' AS src, a, count(*) FROM rv WHERE b = 7 GROUP BY a ORDER BY a LIMIT 5;
SELECT 'seq' AS src, a, count(*) FROM rv WHERE b = 7 AND a IS NOT NULL
  GROUP BY a ORDER BY a LIMIT 5;
RESET enable_indexscan;
RESET enable_bitmapscan;
RESET enable_indexonlyscan;

-- The degenerate case (seqscan off, paths pruned) must still be correct — the
-- hook adds a bitmap-heap fallback (and a seqscan backstop) so it never NULLs.
SET enable_seqscan = off;
SELECT 'idx_noseq' AS src, a, count(*) FROM rv WHERE b = 7 GROUP BY a ORDER BY a LIMIT 3;
RESET enable_seqscan;

DROP TABLE rv;

-- ----------------------------------------------------------------
-- Partitioned table: set_rel_pathlist runs for each partition child
-- (RELOPT_OTHER_MEMBER_REL), which the hook must steer too — otherwise the
-- per-partition IndexOnlyScan would project NULL.
-- ----------------------------------------------------------------
CREATE TABLE pp (id bigint, a int, b int) PARTITION BY RANGE (id);
CREATE TABLE pp0 PARTITION OF pp FOR VALUES FROM (0) TO (10000);
CREATE TABLE pp1 PARTITION OF pp FOR VALUES FROM (10000) TO (20000);
INSERT INTO pp SELECT g, g % 50, (g / 50) % 40 FROM generate_series(1, 19999) g;
CREATE INDEX ON pp USING roaring (a, b);
VACUUM ANALYZE pp;
SET enable_seqscan = off;
SELECT 'part_idx' AS src, a, count(*) FROM pp WHERE b = 7 GROUP BY a ORDER BY a LIMIT 5;
SET enable_indexscan = off;
SET enable_bitmapscan = off;
SET enable_indexonlyscan = off;
RESET enable_seqscan;
SELECT 'part_seq' AS src, a, count(*) FROM pp WHERE b = 7 GROUP BY a ORDER BY a LIMIT 5;
RESET enable_indexscan;
RESET enable_bitmapscan;
RESET enable_indexonlyscan;
DROP TABLE pp;

-- ----------------------------------------------------------------
-- Multi-column int8 key is hashed (canreturn = false): not returnable, so
-- projecting it is never an IndexOnlyScan, and the values are correct.
-- ----------------------------------------------------------------
CREATE TABLE r8 (id int, k int8, b int);
INSERT INTO r8 SELECT g, (g % 5)::int8 * 1000000000000, g % 3
               FROM generate_series(1, 4000) g;
CREATE INDEX r8_idx ON r8 USING roaring (k, b);
VACUUM ANALYZE r8;
SET enable_seqscan = off;
SELECT k, count(*) FROM r8 WHERE b = 1 GROUP BY k ORDER BY k;
RESET enable_seqscan;
DROP TABLE r8;
