\set QUIET 1
\timing off

-- -----------------------------------------------------------------------
-- pg_roaring_index benchmark
-- Table: items  (1 000 000 rows, 10 000 distinct location_id values)
-- -----------------------------------------------------------------------

\echo ''
\echo '======================================================'
\echo ' pg_roaring_index  |  benchmark'
\echo '======================================================'
\echo ''

-- ---- 0. Confirm table stats ------------------------------------------
\echo '--- Table statistics ---'
SELECT
    count(*)                                    AS total_rows,
    count(DISTINCT location_id)                 AS distinct_locations,
    round(count(*)::numeric/count(DISTINCT location_id), 1)
                                                AS avg_rows_per_location,
    pg_size_pretty(pg_relation_size('items'))   AS heap_size
FROM items;
\echo ''

-- ---- 1. Sequential scan baseline -------------------------------------
\echo '--- 1. Sequential scan (no index) ---'
DROP INDEX IF EXISTS idx_items_btree_loc;

EXPLAIN (ANALYZE, BUFFERS, FORMAT TEXT)
SELECT count(*) FROM items WHERE location_id = 42;
\echo ''

-- ---- 2. B-tree index -------------------------------------------------
\echo '--- 2. Create B-tree index ---'
CREATE INDEX IF NOT EXISTS idx_items_btree_loc ON items (location_id);
ANALYZE items;

\echo ''
\echo '--- 2a. B-tree: single location lookup ---'
EXPLAIN (ANALYZE, BUFFERS, FORMAT TEXT)
SELECT count(*) FROM items WHERE location_id = 42;

\echo ''
\echo '--- 2b. B-tree: multi-column filter (location + status) ---'
EXPLAIN (ANALYZE, BUFFERS, FORMAT TEXT)
SELECT count(*) FROM items WHERE location_id = 42 AND status = 'active';

\echo ''
\echo '--- 2c. B-tree: OR across 5 locations ---'
EXPLAIN (ANALYZE, BUFFERS, FORMAT TEXT)
SELECT count(*) FROM items
WHERE location_id IN (42, 137, 501, 888, 999);

-- ---- 3. Index sizes --------------------------------------------------
\echo ''
\echo '--- 3. Index sizes ---'
SELECT
    indexrelname,
    pg_size_pretty(pg_relation_size(indexrelid)) AS index_size
FROM pg_stat_user_indexes
WHERE relname = 'items'
ORDER BY pg_relation_size(indexrelid) DESC;

-- ---- 4. Roaring index ------------------------------------------------
\echo ''
\echo '--- 4. Roaring index ---'

-- Confirm the AM is registered.
SELECT amname, amtype FROM pg_am WHERE amname = 'roaring';

DROP INDEX IF EXISTS idx_items_roaring_loc;

\echo ''
\echo '--- 4a. Build roaring index ---'
\timing on
CREATE INDEX idx_items_roaring_loc
    ON items USING roaring (location_id);
\timing off
ANALYZE items;

\echo ''
\echo '--- 4b. Roaring: single location lookup (planner choice) ---'
EXPLAIN (ANALYZE, BUFFERS, FORMAT TEXT)
SELECT count(*) FROM items WHERE location_id = 42;

\echo ''
\echo '--- 4c. Roaring: isolated bitmap scan (B-tree dropped temporarily) ---'
-- Drop B-tree so only the roaring index is available, then restore it.
DROP INDEX IF EXISTS idx_items_btree_loc;
SET enable_seqscan = off;
EXPLAIN (ANALYZE, BUFFERS, FORMAT TEXT)
SELECT count(*) FROM items WHERE location_id = 42;
RESET ALL;
CREATE INDEX idx_items_btree_loc ON items (location_id);
ANALYZE items;

\echo ''
\echo '--- 4d. Correctness check: roaring vs btree counts ---'
SELECT
    (SELECT count(*) FROM items WHERE location_id = 42)            AS total_loc42,
    (SELECT count(*) FROM items WHERE location_id = 42
        AND status = 'active')                                      AS active_loc42;

\echo ''
\echo '--- 4e. Index sizes ---'
SELECT
    indexrelname,
    pg_size_pretty(pg_relation_size(indexrelid)) AS index_size
FROM pg_stat_user_indexes
WHERE relname = 'items'
ORDER BY pg_relation_size(indexrelid) DESC;

\echo ''
\echo '======================================================'
\echo ' Done.'
\echo '======================================================'
