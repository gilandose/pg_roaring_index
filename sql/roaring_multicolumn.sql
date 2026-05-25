-- Regression tests for multi-column index support (2-column and 3-column).
-- Both/all columns must be int4.  Each column is independently namespaced
-- using ROARING_COL_KEY(attno, value) = ((int64)(attno) << 32) | (uint32)value.
-- Multi-column AND queries collect one bitmap per scan key and intersect
-- them inside the AM before returning results to the executor.

CREATE EXTENSION IF NOT EXISTS pg_roaring_index;

-- ----------------------------------------------------------------
-- 1. Exact (roaring) 2-column index
-- ----------------------------------------------------------------
CREATE TABLE mc_exact (
    id      int,
    tenant  int4,
    location int4,
    payload text
);

INSERT INTO mc_exact VALUES
    (1,  1, 10, 'a'),
    (2,  1, 10, 'b'),   -- tenant=1, location=10: 2 rows
    (3,  1, 20, 'c'),   -- tenant=1, location=20: 1 row
    (4,  2, 10, 'd'),   -- tenant=2, location=10: 1 row
    (5,  2, 30, 'e'),   -- tenant=2, location=30: 1 row
    (6,  3, 10, 'f');   -- tenant=3, location=10: 1 row

CREATE INDEX mc_exact_idx ON mc_exact USING roaring (tenant, location);
VACUUM mc_exact;

SET enable_seqscan = off;
SET enable_bitmapscan = on;

-- (a) Both columns: exact lookup.
SELECT count(*) AS both_t1_l10  FROM mc_exact WHERE tenant = 1 AND location = 10;  -- 2
SELECT count(*) AS both_t1_l20  FROM mc_exact WHERE tenant = 1 AND location = 20;  -- 1
SELECT count(*) AS both_t2_l10  FROM mc_exact WHERE tenant = 2 AND location = 10;  -- 1
SELECT count(*) AS both_absent  FROM mc_exact WHERE tenant = 9 AND location = 99;  -- 0

-- (b) Col1 only: prefix scan returns all rows for that tenant.
SELECT count(*) AS prefix_t1    FROM mc_exact WHERE tenant = 1;  -- 3
SELECT count(*) AS prefix_t2    FROM mc_exact WHERE tenant = 2;  -- 2
SELECT count(*) AS prefix_t9    FROM mc_exact WHERE tenant = 9;  -- 0

-- (c) Plan shape: IndexScan chosen when amgettuple is registered.
EXPLAIN (COSTS OFF) SELECT id FROM mc_exact WHERE tenant = 1 AND location = 10;
EXPLAIN (COSTS OFF) SELECT id FROM mc_exact WHERE tenant = 1;

-- ----------------------------------------------------------------
-- 2. Pending list visible within the same transaction
-- ----------------------------------------------------------------
INSERT INTO mc_exact VALUES (7, 1, 10, 'pending');  -- pending, not yet merged
SELECT count(*) AS both_t1_l10_pending FROM mc_exact WHERE tenant = 1 AND location = 10;  -- 3
SELECT count(*) AS prefix_t1_pending   FROM mc_exact WHERE tenant = 1;  -- 4

-- ----------------------------------------------------------------
-- 3. VACUUM merges pending list
-- ----------------------------------------------------------------
VACUUM mc_exact;
SELECT count(*) AS both_t1_l10_post  FROM mc_exact WHERE tenant = 1 AND location = 10;  -- 3
SELECT count(*) AS prefix_t1_post    FROM mc_exact WHERE tenant = 1;  -- 4

-- ----------------------------------------------------------------
-- 4. ambulkdelete + re-scan
-- ----------------------------------------------------------------
DELETE FROM mc_exact WHERE id = 1;  -- remove one (tenant=1, location=10) row
VACUUM mc_exact;
SELECT count(*) AS both_t1_l10_partial FROM mc_exact WHERE tenant = 1 AND location = 10;  -- 2
SELECT count(*) AS prefix_t1_partial   FROM mc_exact WHERE tenant = 1;  -- 3

-- ----------------------------------------------------------------
-- 5. Lossy (roaring_lossy) 2-column index
-- ----------------------------------------------------------------
CREATE TABLE mc_lossy (
    id      int,
    tenant  int4,
    location int4
);

INSERT INTO mc_lossy VALUES
    (1,  1, 10),
    (2,  1, 10),
    (3,  1, 20),
    (4,  2, 10),
    (5,  2, 30),
    (6,  3, 10);

CREATE INDEX mc_lossy_idx ON mc_lossy USING roaring_lossy (tenant, location);
VACUUM mc_lossy;

-- Lossy mode: amrecheck=true means results are correct but come via page bitmap.
SELECT count(*) AS lossy_both_t1_l10 FROM mc_lossy WHERE tenant = 1 AND location = 10;  -- 2
SELECT count(*) AS lossy_prefix_t1   FROM mc_lossy WHERE tenant = 1;  -- 3
SELECT count(*) AS lossy_prefix_t9   FROM mc_lossy WHERE tenant = 9;  -- 0

-- ----------------------------------------------------------------
-- 6. NULL handling: rows with NULL in either column are not indexed
-- ----------------------------------------------------------------
RESET enable_seqscan;
INSERT INTO mc_exact VALUES (20, NULL, 10, 'null_tenant');
INSERT INTO mc_exact VALUES (21, 1, NULL, 'null_loc');
-- NULLs are not indexed; queries with equality skip them (seq scan needed for IS NULL).
SELECT count(*) AS null_tenant_rows FROM mc_exact WHERE tenant IS NULL;  -- 1 via seqscan
SELECT count(*) AS null_loc_rows    FROM mc_exact WHERE tenant = 1 AND location IS NULL;  -- 1 via seqscan (NULL row not indexed)

-- ----------------------------------------------------------------
-- 7. 3-column exact index: internal AND across three keys
-- ----------------------------------------------------------------
CREATE TABLE mc3 (
    id     int,
    status int4,
    region int4,
    tier   int4
);

INSERT INTO mc3 VALUES
    (1,  1, 10, 100),
    (2,  1, 10, 200),  -- status=1, region=10, tier=200
    (3,  1, 20, 100),  -- status=1, region=20, tier=100
    (4,  2, 10, 100),
    (5,  1, 10, 100);  -- duplicate key set, different row

CREATE INDEX mc3_idx ON mc3 USING roaring (status, region, tier);
VACUUM mc3;

SET enable_seqscan = off;

-- All 3 columns constrained: AND of all three bitmaps.
SELECT count(*) AS all3_s1_r10_t100 FROM mc3 WHERE status = 1 AND region = 10 AND tier = 100;  -- 2
SELECT count(*) AS all3_s1_r10_t200 FROM mc3 WHERE status = 1 AND region = 10 AND tier = 200;  -- 1
SELECT count(*) AS all3_absent      FROM mc3 WHERE status = 9 AND region = 10 AND tier = 100;  -- 0

-- 2-column subset: status + region only.
SELECT count(*) AS two_s1_r10 FROM mc3 WHERE status = 1 AND region = 10;  -- 3
SELECT count(*) AS two_s1_r20 FROM mc3 WHERE status = 1 AND region = 20;  -- 1

-- Single column: status only.
SELECT count(*) AS one_s1 FROM mc3 WHERE status = 1;  -- 4
SELECT count(*) AS one_s2 FROM mc3 WHERE status = 2;  -- 1

RESET enable_seqscan;
DROP TABLE mc3;

-- ----------------------------------------------------------------
-- Cleanup
-- ----------------------------------------------------------------
RESET enable_seqscan;
DROP TABLE mc_exact;
DROP TABLE mc_lossy;
