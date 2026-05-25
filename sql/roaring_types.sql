-- Regression tests for type expansion: int2, bool, date, float4, oid.
-- Each type is tested in both exact (roaring) and lossy (roaring_lossy) modes,
-- for single-column and multi-column configurations.

CREATE EXTENSION IF NOT EXISTS pg_roaring_index;

SET enable_seqscan = off;
SET enable_bitmapscan = on;

-- ================================================================
-- 1.  int2 (smallint)
-- ================================================================
CREATE TABLE t_int2 (id int, v int2);
INSERT INTO t_int2 VALUES
    (1, 10), (2, 10), (3, 20), (4, 30), (5, 30), (6, 30);

CREATE INDEX t_int2_ro ON t_int2 USING roaring (v);
VACUUM t_int2;

SELECT count(*) AS int2_10   FROM t_int2 WHERE v = 10::int2;    -- 2
SELECT count(*) AS int2_30   FROM t_int2 WHERE v = 30::int2;    -- 3
SELECT count(*) AS int2_none FROM t_int2 WHERE v = 99::int2;    -- 0

-- Cross-type: int2 col vs int4 literal
SELECT count(*) AS int2_int4 FROM t_int2 WHERE v = 10;          -- 2

CREATE INDEX t_int2_lo ON t_int2 USING roaring_lossy (v);
VACUUM t_int2;

SELECT count(*) AS int2_lo_10   FROM t_int2 WHERE v = 10::int2; -- 2
SELECT count(*) AS int2_lo_30   FROM t_int2 WHERE v = 30::int2; -- 3

DROP TABLE t_int2;

-- ================================================================
-- 2.  bool
-- ================================================================
CREATE TABLE t_bool (id int, v boolean);
INSERT INTO t_bool VALUES
    (1, true), (2, true), (3, false), (4, false), (5, false), (6, true);

CREATE INDEX t_bool_ro ON t_bool USING roaring (v);
VACUUM t_bool;

SELECT count(*) AS bool_true  FROM t_bool WHERE v = true;   -- 3
SELECT count(*) AS bool_false FROM t_bool WHERE v = false;  -- 3

CREATE INDEX t_bool_lo ON t_bool USING roaring_lossy (v);
VACUUM t_bool;

SELECT count(*) AS bool_lo_true FROM t_bool WHERE v = true; -- 3

DROP TABLE t_bool;

-- ================================================================
-- 3.  date
-- ================================================================
CREATE TABLE t_date (id int, v date);
INSERT INTO t_date VALUES
    (1, '2024-01-01'), (2, '2024-01-01'),
    (3, '2024-06-15'), (4, '2024-06-15'), (5, '2024-06-15'),
    (6, '2025-12-31');

CREATE INDEX t_date_ro ON t_date USING roaring (v);
VACUUM t_date;

SELECT count(*) AS date_jan  FROM t_date WHERE v = '2024-01-01'::date;  -- 2
SELECT count(*) AS date_jun  FROM t_date WHERE v = '2024-06-15'::date;  -- 3
SELECT count(*) AS date_none FROM t_date WHERE v = '2000-01-01'::date;  -- 0

CREATE INDEX t_date_lo ON t_date USING roaring_lossy (v);
VACUUM t_date;

SELECT count(*) AS date_lo_jan FROM t_date WHERE v = '2024-01-01'::date; -- 2

DROP TABLE t_date;

-- ================================================================
-- 4.  float4
-- ================================================================
CREATE TABLE t_float4 (id int, v float4);
INSERT INTO t_float4 VALUES
    (1, 1.5), (2, 1.5), (3, 2.5), (4, 2.5), (5, 2.5), (6, -1.0);

CREATE INDEX t_float4_ro ON t_float4 USING roaring (v);
VACUUM t_float4;

SELECT count(*) AS f4_1_5  FROM t_float4 WHERE v = 1.5::float4;    -- 2
SELECT count(*) AS f4_2_5  FROM t_float4 WHERE v = 2.5::float4;    -- 3
SELECT count(*) AS f4_neg  FROM t_float4 WHERE v = -1.0::float4;   -- 1
SELECT count(*) AS f4_none FROM t_float4 WHERE v = 99.0::float4;   -- 0

-- ±0.0 must map to the same index key.
INSERT INTO t_float4 VALUES (7, 0.0), (8, -0.0);
VACUUM t_float4;
SELECT count(*) AS f4_zero FROM t_float4 WHERE v = 0.0::float4;    -- 2

CREATE INDEX t_float4_lo ON t_float4 USING roaring_lossy (v);
VACUUM t_float4;

SELECT count(*) AS f4_lo_1_5 FROM t_float4 WHERE v = 1.5::float4;  -- 2

DROP TABLE t_float4;

-- ================================================================
-- 5.  oid
-- ================================================================
CREATE TABLE t_oid (id int, v oid);
INSERT INTO t_oid VALUES
    (1, 100), (2, 100), (3, 200), (4, 200), (5, 200), (6, 300);

CREATE INDEX t_oid_ro ON t_oid USING roaring (v);
VACUUM t_oid;

SELECT count(*) AS oid_100  FROM t_oid WHERE v = 100::oid;    -- 2
SELECT count(*) AS oid_200  FROM t_oid WHERE v = 200::oid;    -- 3
SELECT count(*) AS oid_none FROM t_oid WHERE v = 999::oid;    -- 0

CREATE INDEX t_oid_lo ON t_oid USING roaring_lossy (v);
VACUUM t_oid;

SELECT count(*) AS oid_lo_100 FROM t_oid WHERE v = 100::oid;  -- 2

DROP TABLE t_oid;

-- ================================================================
-- 6.  Multi-column index with mixed types (int2 + bool + date)
-- ================================================================
CREATE TABLE t_mixed (
    id     int,
    status int2,
    active boolean,
    day    date
);

INSERT INTO t_mixed VALUES
    (1, 1, true,  '2024-01-01'),
    (2, 1, true,  '2024-01-01'),
    (3, 1, false, '2024-01-01'),
    (4, 2, true,  '2024-06-15'),
    (5, 2, false, '2024-06-15');

CREATE INDEX t_mixed_ro ON t_mixed USING roaring (status, active, day);
VACUUM t_mixed;

-- All 3 columns constrained.
SELECT count(*) AS mix_all3
    FROM t_mixed WHERE status = 1::int2 AND active = true AND day = '2024-01-01'::date;  -- 2

-- 2-column subset.
SELECT count(*) AS mix_status_active
    FROM t_mixed WHERE status = 1::int2 AND active = true;  -- 2

-- Single column from multi-col index.
SELECT count(*) AS mix_status1
    FROM t_mixed WHERE status = 1::int2;  -- 3

-- Absent combination.
SELECT count(*) AS mix_absent
    FROM t_mixed WHERE status = 9::int2 AND active = true;  -- 0

DROP TABLE t_mixed;

-- ================================================================
-- 7.  enum (exact and lossy)
-- ================================================================
CREATE TYPE mood AS ENUM ('happy', 'neutral', 'sad');

CREATE TABLE t_enum (id int, v mood);
INSERT INTO t_enum VALUES
    (1, 'happy'), (2, 'happy'), (3, 'neutral'),
    (4, 'sad'), (5, 'sad'), (6, 'sad');

CREATE INDEX t_enum_ro ON t_enum USING roaring (v);
VACUUM t_enum;

SELECT count(*) AS enum_happy   FROM t_enum WHERE v = 'happy'::mood;    -- 2
SELECT count(*) AS enum_neutral FROM t_enum WHERE v = 'neutral'::mood;  -- 1
SELECT count(*) AS enum_sad     FROM t_enum WHERE v = 'sad'::mood;      -- 3
SELECT count(*) AS enum_none    FROM t_enum WHERE v = 'happy'::mood AND v = 'sad'::mood;  -- 0

-- Pending list visible before VACUUM.
INSERT INTO t_enum VALUES (7, 'happy');
SELECT count(*) AS enum_happy_pending FROM t_enum WHERE v = 'happy'::mood;  -- 3

VACUUM t_enum;
SELECT count(*) AS enum_happy_post FROM t_enum WHERE v = 'happy'::mood;  -- 3

-- ambulkdelete.
DELETE FROM t_enum WHERE id = 1;
VACUUM t_enum;
SELECT count(*) AS enum_happy_partial FROM t_enum WHERE v = 'happy'::mood;  -- 2

CREATE INDEX t_enum_lo ON t_enum USING roaring_lossy (v);
VACUUM t_enum;

SELECT count(*) AS enum_lo_sad  FROM t_enum WHERE v = 'sad'::mood;    -- 3
SELECT count(*) AS enum_lo_none FROM t_enum WHERE v = 'neutral'::mood AND v = 'sad'::mood;  -- 0

DROP TABLE t_enum;
DROP TYPE mood;

RESET enable_seqscan;
