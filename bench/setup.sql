-- bench/setup.sql
--
-- Creates four identical tables (one per AM) and loads them with benchmark
-- data, then builds all indexes.  Run once before any benchmark.
--
-- Scale parameters — edit these or override with:
--   psql -v nrows=1000000 -v ndistinct=10000 -f bench/setup.sql
--
\if :{?nrows}
\else
\set nrows     1000000
\endif
\if :{?ndistinct}
\else
\set ndistinct 10000
\endif

\echo ''
\echo '=== pg_roaring_index benchmark setup ==='
\echo ''
\echo 'Parameters:'
\echo '  nrows     = ' :nrows
\echo '  ndistinct = ' :ndistinct
\echo ''

CREATE EXTENSION IF NOT EXISTS btree_gin;
CREATE EXTENSION IF NOT EXISTS pg_roaring_index;

-- ----------------------------------------------------------------
-- Tables (identical schema and data; one index type each)
-- ----------------------------------------------------------------
DROP TABLE IF EXISTS bench_btree   CASCADE;
DROP TABLE IF EXISTS bench_gin     CASCADE;
DROP TABLE IF EXISTS bench_brin    CASCADE;
DROP TABLE IF EXISTS bench_roaring CASCADE;

CREATE TABLE bench_btree (
    id      bigint NOT NULL,
    val     bigint NOT NULL,
    payload text   NOT NULL
);
CREATE TABLE bench_gin (
    id      bigint NOT NULL,
    val     bigint NOT NULL,
    payload text   NOT NULL
);
CREATE TABLE bench_brin (
    id      bigint NOT NULL,
    val     bigint NOT NULL,
    payload text   NOT NULL
);
CREATE TABLE bench_roaring (
    id      bigint NOT NULL,
    val     bigint NOT NULL,
    payload text   NOT NULL
);

-- Data: sequential IDs, values distributed uniformly over [1, ndistinct].
-- Rows are stored in insert order, so physical layout is deliberately
-- uncorrelated with val — the target workload for roaring.

\echo 'Loading bench_btree ...'
INSERT INTO bench_btree
    SELECT i, (i % :ndistinct) + 1, repeat('x', 20)
    FROM   generate_series(1, :nrows) i;

\echo 'Loading bench_gin ...'
INSERT INTO bench_gin    SELECT * FROM bench_btree;

\echo 'Loading bench_brin ...'
INSERT INTO bench_brin   SELECT * FROM bench_btree;

\echo 'Loading bench_roaring ...'
INSERT INTO bench_roaring SELECT * FROM bench_btree;

-- ----------------------------------------------------------------
-- Indexes
-- ----------------------------------------------------------------
\echo 'Building btree index ...'
CREATE INDEX bench_btree_idx   ON bench_btree   USING btree   (val);

\echo 'Building gin index (btree_gin int8_ops) ...'
CREATE INDEX bench_gin_idx     ON bench_gin     USING gin     (val int8_ops);

\echo 'Building brin index (minmax) ...'
-- NOTE: brin is included for size/build comparison only.  With uniform
-- uncorrelated data (our target workload), each page range covers all
-- ndistinct values so brin equality lookups degrade to seq scan.
CREATE INDEX bench_brin_idx    ON bench_brin    USING brin    (val);

\echo 'Building roaring index ...'
CREATE INDEX bench_roaring_idx ON bench_roaring USING roaring (val);

-- Merge pending inserts into the main roaring index before benchmarking.
VACUUM ANALYZE bench_btree;
VACUUM ANALYZE bench_gin;
VACUUM ANALYZE bench_brin;
VACUUM ANALYZE bench_roaring;

\echo ''
\echo 'Setup complete.  Run bench/sizes.sql to verify, then bench/run.sh to benchmark.'
\echo ''
