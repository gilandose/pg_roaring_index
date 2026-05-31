-- ================================================================
-- roaring_revproj: reverse-bitmap projection of returnable key columns
--
-- An IndexOnlyScan that projects a key column whose value is NOT pinned by
-- an equality scan key (SELECT a WHERE b = 5, or a column constrained only by
-- IN / IS NOT NULL) reconstructs the value from the index: a column's value
-- bitmaps partition its TIDs, so the bitmap (hence key) containing a TID gives
-- the value; a TID in no value bitmap is NULL for that column.
--
-- Each index result is cross-checked against a seqscan oracle.
-- ================================================================
CREATE EXTENSION IF NOT EXISTS pg_roaring_index;

CREATE TABLE rp (id int, a int, b int, d date, f float4);
INSERT INTO rp SELECT g,
       CASE WHEN g % 7 = 0 THEN NULL ELSE g % 10 END,   -- a: scattered NULLs
       g % 4,                                            -- b
       DATE '2020-01-01' + (g % 5),                      -- d
       (g % 3)::float4 + 0.5                             -- f
  FROM generate_series(1, 200) g;
CREATE INDEX rp_idx ON rp USING roaring (a, b, d, f);
-- post-build (pending) rows, including a NULL key column
INSERT INTO rp VALUES (201, 5, 1, DATE '2020-01-02', 1.5),
                      (202, NULL, 1, DATE '2020-01-02', 2.5);
VACUUM rp;   -- merge pending into the leaf bitmaps

SET enable_seqscan = off;

-- Plan: projecting the unconstrained key column a is still an Index Only Scan.
EXPLAIN (COSTS OFF) SELECT a FROM rp WHERE b = 1;

-- int reconstruction incl. NULLs (unconstrained projection).
SELECT a AS a_idx, count(*) FROM rp WHERE b = 1 GROUP BY a ORDER BY a NULLS LAST;
-- date + float4 reconstruction.
SELECT d AS d_idx, count(*) FROM rp WHERE b = 2 GROUP BY d ORDER BY d;
SELECT f AS f_idx, count(*) FROM rp WHERE b = 2 GROUP BY f ORDER BY f;
-- projected column constrained by IN (value varies per row → reverse map).
SELECT a AS in_idx, count(*) FROM rp WHERE a IN (3,5,9) AND b = 1 GROUP BY a ORDER BY a;
-- projected column constrained only by IS NOT NULL (value unknown → reverse).
SELECT a AS nn_idx, count(*) FROM rp WHERE a IS NOT NULL AND b = 3 GROUP BY a ORDER BY a;

-- Seqscan oracle: every count above must match.
SET enable_seqscan = on;
SET enable_indexscan = off;
SET enable_bitmapscan = off;
SELECT a AS a_seq, count(*) FROM rp WHERE b = 1 GROUP BY a ORDER BY a NULLS LAST;
SELECT d AS d_seq, count(*) FROM rp WHERE b = 2 GROUP BY d ORDER BY d;
SELECT f AS f_seq, count(*) FROM rp WHERE b = 2 GROUP BY f ORDER BY f;
SELECT a AS in_seq, count(*) FROM rp WHERE a IN (3,5,9) AND b = 1 GROUP BY a ORDER BY a;
SELECT a AS nn_seq, count(*) FROM rp WHERE a IS NOT NULL AND b = 3 GROUP BY a ORDER BY a;
RESET enable_indexscan;
RESET enable_bitmapscan;
RESET enable_seqscan;
DROP TABLE rp;

-- ----------------------------------------------------------------
-- Unmerged pending: the projected column's value lives only in the
-- pending list (no VACUUM) and must still be reconstructed.
-- ----------------------------------------------------------------
CREATE TABLE rpp (id int, a int, b int);
INSERT INTO rpp SELECT g, g % 6, g % 3 FROM generate_series(1, 60) g;
CREATE INDEX rpp_idx ON rpp USING roaring (a, b);
INSERT INTO rpp VALUES (101, 2, 1), (102, 4, 1), (103, NULL, 1);  -- pending

SET enable_seqscan = off;
SELECT a AS pend_idx, count(*) FROM rpp WHERE b = 1 GROUP BY a ORDER BY a NULLS LAST;
SET enable_seqscan = on;
SET enable_indexscan = off;
SET enable_bitmapscan = off;
SELECT a AS pend_seq, count(*) FROM rpp WHERE b = 1 GROUP BY a ORDER BY a NULLS LAST;
RESET enable_indexscan;
RESET enable_bitmapscan;
RESET enable_seqscan;
DROP TABLE rpp;

-- ----------------------------------------------------------------
-- Multi-column int8 key is hashed (canreturn = false): projecting it is
-- NOT an Index Only Scan, but the values are still correct (via the heap).
-- ----------------------------------------------------------------
CREATE TABLE r8 (id int, a int8, b int);
INSERT INTO r8 SELECT g, (g % 5)::int8 * 1000000000000, g % 3
               FROM generate_series(1, 60) g;
CREATE INDEX r8_idx ON r8 USING roaring (a, b);
VACUUM r8;
SET enable_seqscan = off;
EXPLAIN (COSTS OFF) SELECT a FROM r8 WHERE b = 1;   -- Index Scan, not Index Only
SELECT a AS i8_idx, count(*) FROM r8 WHERE b = 1 GROUP BY a ORDER BY a;
RESET enable_seqscan;
DROP TABLE r8;
