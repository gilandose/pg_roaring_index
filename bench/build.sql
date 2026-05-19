-- bench/build.sql
--
-- Measures CREATE INDEX time for each AM by dropping and recreating every
-- index.  Run after bench/setup.sql (data must already be loaded).
-- Uses \timing so each CREATE INDEX line shows wall time.

\echo ''
\echo '=== Index build times (wall clock via \timing) ==='
\echo ''

\timing on

\echo '-- btree'
DROP INDEX IF EXISTS bench_btree_idx;
CREATE INDEX bench_btree_idx ON bench_btree USING btree (val);

\echo '-- gin (btree_gin int8_ops)'
DROP INDEX IF EXISTS bench_gin_idx;
CREATE INDEX bench_gin_idx ON bench_gin USING gin (val int8_ops);

\echo '-- brin'
DROP INDEX IF EXISTS bench_brin_idx;
CREATE INDEX bench_brin_idx ON bench_brin USING brin (val);

\echo '-- roaring'
DROP INDEX IF EXISTS bench_roaring_idx;
CREATE INDEX bench_roaring_idx ON bench_roaring USING roaring (val);

\timing off

-- Merge pending inserts so subsequent lookup benchmarks don't measure a
-- cold pending list.
VACUUM bench_roaring;

\echo ''
\echo 'Build timing complete.  Indexes are ready for lookup benchmarks.'
\echo ''
