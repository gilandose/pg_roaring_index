-- Regression tests for pg_stat_roaring_indexes, roaring_index_stats(),
-- roaring_index_health(), and the roaring.pending_merge_threshold GUC.

CREATE EXTENSION IF NOT EXISTS pg_roaring_index;

-- ----------------------------------------------------------------
-- 1. GUC: set a custom threshold, build an index, verify it stuck
-- ----------------------------------------------------------------
SET roaring.pending_merge_threshold = 5000;

CREATE TABLE stats_test (val int);
INSERT INTO stats_test SELECT i % 100 FROM generate_series(1, 1000) i;
CREATE INDEX stats_idx ON stats_test USING roaring(val);

-- After ambuild: total_entries=100, pending_count=0, threshold=5000
SELECT
    total_entries,
    pending_count,
    pending_threshold,
    bg_merge_running
FROM roaring_index_stats('stats_idx'::regclass);

-- ----------------------------------------------------------------
-- 2. pg_stat_roaring_indexes view
-- ----------------------------------------------------------------
SELECT indexrelname, amname, pending_threshold
FROM pg_stat_roaring_indexes
WHERE indexrelname = 'stats_idx'
ORDER BY indexrelname;

-- ----------------------------------------------------------------
-- 3. roaring_index_health
-- ----------------------------------------------------------------
SELECT roaring_index_health('stats_idx'::regclass);

-- ----------------------------------------------------------------
-- 4. GUC resets to default after RESET
-- ----------------------------------------------------------------
RESET roaring.pending_merge_threshold;
SHOW roaring.pending_merge_threshold;

-- Cleanup
DROP TABLE stats_test;
