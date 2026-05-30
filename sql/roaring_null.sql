-- ================================================================
-- roaring_null: IS NULL / IS NOT NULL answered from per-column NULL bitmaps
-- ================================================================
CREATE EXTENSION IF NOT EXISTS pg_roaring_index;

-- ----------------------------------------------------------------
-- 1. Single column: build-time NULLs + post-build (pending) NULLs
-- ----------------------------------------------------------------
CREATE TABLE rn1 (id int, v int);
-- v is NULL for g % 4 = 0  (25 of 100), else g % 5
INSERT INTO rn1 SELECT g, CASE WHEN g % 4 = 0 THEN NULL ELSE g % 5 END
                FROM generate_series(1, 100) g;
CREATE INDEX rn1_idx ON rn1 USING roaring (v);
-- one NULL + one non-NULL via the pending list (after build)
INSERT INTO rn1 VALUES (101, NULL), (102, 3);

SET enable_seqscan = off;
-- 25 build NULLs + 1 pending NULL = 26
SELECT count(*) AS isnull_idx     FROM rn1 WHERE v IS NULL;
-- 102 rows - 26 NULLs = 76
SELECT count(*) AS notnull_idx    FROM rn1 WHERE v IS NOT NULL;
-- equality must still exclude NULLs
SELECT count(*) AS eq3_idx        FROM rn1 WHERE v = 3;

-- seqscan oracle (must match the index counts above)
SET enable_seqscan = on;
SET enable_indexscan = off;
SET enable_bitmapscan = off;
SELECT count(*) AS isnull_seq     FROM rn1 WHERE v IS NULL;
SELECT count(*) AS notnull_seq    FROM rn1 WHERE v IS NOT NULL;
SELECT count(*) AS eq3_seq        FROM rn1 WHERE v = 3;
RESET enable_indexscan;
RESET enable_bitmapscan;

-- After a merge the pending NULL is folded into the NULL-key bitmap.
VACUUM rn1;
SET enable_seqscan = off;
SELECT count(*) AS isnull_post_merge FROM rn1 WHERE v IS NULL;   -- still 26
RESET enable_seqscan;

-- ----------------------------------------------------------------
-- 2. Multi-column: IS NULL composes with equality via bitmap AND
-- ----------------------------------------------------------------
CREATE TABLE rn2 (id int, a int, b int);
-- a = g % 3 ; b is NULL for even g (50 of 100)
INSERT INTO rn2 SELECT g, g % 3, CASE WHEN g % 2 = 0 THEN NULL ELSE g % 7 END
                FROM generate_series(1, 100) g;
CREATE INDEX rn2_idx ON rn2 USING roaring (a, b);

SET enable_seqscan = off;
SELECT count(*) AS b_isnull_idx       FROM rn2 WHERE b IS NULL;            -- 50
SELECT count(*) AS a1_bnull_idx       FROM rn2 WHERE a = 1 AND b IS NULL;  -- g≡4 mod 6
SELECT count(*) AS a1_bnotnull_idx    FROM rn2 WHERE a = 1 AND b IS NOT NULL;
SELECT count(*) AS a0_b3_idx          FROM rn2 WHERE a = 0 AND b = 3;

-- seqscan oracle
SET enable_seqscan = on;
SET enable_indexscan = off;
SET enable_bitmapscan = off;
SELECT count(*) AS b_isnull_seq       FROM rn2 WHERE b IS NULL;
SELECT count(*) AS a1_bnull_seq       FROM rn2 WHERE a = 1 AND b IS NULL;
SELECT count(*) AS a1_bnotnull_seq    FROM rn2 WHERE a = 1 AND b IS NOT NULL;
SELECT count(*) AS a0_b3_seq          FROM rn2 WHERE a = 0 AND b = 3;
RESET enable_indexscan;
RESET enable_bitmapscan;
RESET enable_seqscan;

DROP TABLE rn1, rn2;
