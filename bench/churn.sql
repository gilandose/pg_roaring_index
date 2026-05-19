-- bench/churn.sql
--
-- Shows how lookup latency evolves as the pending insert list grows, then
-- recovers after VACUUM.  Run after bench/setup.sql.
--
-- The test inserts rows in batches (using a separate churn sequence so IDs
-- don't collide with the baseline rows), measures lookup latency at each
-- checkpoint, runs VACUUM, and measures again.
--
-- Edit these constants to change test scale:
-- Inserts per checkpoint, number of checkpoints, and which value to probe.
\set batch_size   5000
\set num_batches  6
\set probe_val    1

\echo ''
\echo '=== Churn test: lookup latency vs pending list depth ==='
\echo ''
\echo 'AM      | pending_inserts | lookup ms'
\echo '--------+-----------------+----------'

-- ---- Helper: create churn sequence if not present ----
CREATE SEQUENCE IF NOT EXISTS bench_churn_seq START WITH 2000000;

-- ---- btree baseline (no pending list concept) ----
\timing off

-- Reset: truncate churn rows from previous runs.
DELETE FROM bench_btree   WHERE id >= 2000000;
DELETE FROM bench_roaring WHERE id >= 2000000;
ALTER SEQUENCE bench_churn_seq RESTART WITH 2000000;

-- Checkpoint 0 (no pending writes)
\echo '-- btree: checkpoint 0 (baseline, 0 extra rows)'
\timing on
SELECT count(*) FROM bench_btree WHERE val = 1;
\timing off

\echo '-- roaring: checkpoint 0 (baseline, 0 extra rows)'
\timing on
SELECT count(*) FROM bench_roaring WHERE val = 1;
\timing off

-- ---- Insert batches and probe ----
\set total_inserted 0

\echo ''
\echo '-- Inserting batches and measuring lookup latency ...'
\echo ''

-- Batch 1
INSERT INTO bench_btree
    SELECT nextval('bench_churn_seq'), (nextval('bench_churn_seq') % 10000) + 1, 'churn'
    FROM generate_series(1, :batch_size);
INSERT INTO bench_roaring
    SELECT id, val, payload FROM bench_btree WHERE id >= 2000000
    ORDER BY id DESC LIMIT :batch_size;

\echo '-- btree: after batch 1'
\timing on
SELECT count(*) FROM bench_btree   WHERE val = 1;
\timing off
\echo '-- roaring: after batch 1 (pending not merged)'
\timing on
SELECT count(*) FROM bench_roaring WHERE val = 1;
\timing off

-- Batch 2
INSERT INTO bench_btree
    SELECT nextval('bench_churn_seq'), (nextval('bench_churn_seq') % 10000) + 1, 'churn'
    FROM generate_series(1, :batch_size);
INSERT INTO bench_roaring
    SELECT id, val, payload FROM bench_btree WHERE id >= 2000000
    ORDER BY id DESC LIMIT :batch_size;

\echo '-- btree: after batch 2'
\timing on
SELECT count(*) FROM bench_btree   WHERE val = 1;
\timing off
\echo '-- roaring: after batch 2 (pending not merged)'
\timing on
SELECT count(*) FROM bench_roaring WHERE val = 1;
\timing off

-- Batch 3
INSERT INTO bench_btree
    SELECT nextval('bench_churn_seq'), (nextval('bench_churn_seq') % 10000) + 1, 'churn'
    FROM generate_series(1, :batch_size);
INSERT INTO bench_roaring
    SELECT id, val, payload FROM bench_btree WHERE id >= 2000000
    ORDER BY id DESC LIMIT :batch_size;

\echo '-- btree: after batch 3'
\timing on
SELECT count(*) FROM bench_btree   WHERE val = 1;
\timing off
\echo '-- roaring: after batch 3 (pending not merged)'
\timing on
SELECT count(*) FROM bench_roaring WHERE val = 1;
\timing off

-- ---- VACUUM merges pending list ----
\echo ''
\echo '-- Running VACUUM bench_roaring ...'
\timing on
VACUUM bench_roaring;
\timing off
\echo ''

\echo '-- roaring: post-VACUUM (pending merged)'
\timing on
SELECT count(*) FROM bench_roaring WHERE val = 1;
\timing off

\echo ''
\echo 'Churn test complete.'
\echo 'Key observation: roaring lookup latency grows with pending list depth'
\echo 'then returns to baseline post-VACUUM; btree latency is stable throughout.'
\echo ''

-- Cleanup churn rows so subsequent lookups remain comparable.
DELETE FROM bench_btree   WHERE id >= 2000000;
DELETE FROM bench_roaring WHERE id >= 2000000;
ALTER SEQUENCE bench_churn_seq RESTART WITH 2000000;
