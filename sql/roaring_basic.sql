-- Regression tests for pg_roaring_index.
-- Covers: extension load, build, scan, pending list, VACUUM (ambulkdelete +
-- vacuumcleanup/merge), delete+reinsert, empty-bitmap handling, int4 opclass.

CREATE EXTENSION pg_roaring_index;
SELECT amname FROM pg_am WHERE amname = 'roaring';

-- ----------------------------------------------------------------
-- 1. Basic build and equality lookups
-- ----------------------------------------------------------------
CREATE TABLE roaring_test (id int, val bigint);
INSERT INTO roaring_test VALUES
    (1, 1), (2, 1),           -- val=1: 2 rows
    (3, 2),                   -- val=2: 1 row
    (4, 3), (5, 3), (6, 3);   -- val=3: 3 rows

CREATE INDEX roaring_test_idx ON roaring_test USING roaring (val);

-- Merge pending list into main index before testing.
VACUUM roaring_test;

SET enable_seqscan = off;

SELECT count(*) AS cnt_val1 FROM roaring_test WHERE val = 1;
SELECT count(*) AS cnt_val2 FROM roaring_test WHERE val = 2;
SELECT count(*) AS cnt_val3 FROM roaring_test WHERE val = 3;
SELECT count(*) AS cnt_val99 FROM roaring_test WHERE val = 99;  -- absent

-- ----------------------------------------------------------------
-- 2. Plan shape
-- ----------------------------------------------------------------
-- amgettuple registered: planner chooses IndexScan for small result sets.
EXPLAIN (COSTS OFF) SELECT id FROM roaring_test WHERE val = 1;

-- BitmapIndexScan is still reachable when IndexScan is disabled.
SET enable_indexscan = off;
EXPLAIN (COSTS OFF) SELECT id FROM roaring_test WHERE val = 1;
RESET enable_indexscan;

-- ----------------------------------------------------------------
-- 3. NULL values are not indexed (seq scan required for IS NULL)
-- ----------------------------------------------------------------
RESET enable_seqscan;
INSERT INTO roaring_test VALUES (7, NULL);
SELECT count(*) AS cnt_null FROM roaring_test WHERE val IS NULL;

-- ----------------------------------------------------------------
-- 4. Pending list visible within the same transaction
-- ----------------------------------------------------------------
SET enable_seqscan = off;

INSERT INTO roaring_test VALUES (8, 4), (9, 4), (10, 4);  -- val=4: pending
SELECT count(*) AS cnt_val4_pending FROM roaring_test WHERE val = 4;

-- Mixed: val=1 has 2 rows in main index + 1 new row in pending.
INSERT INTO roaring_test VALUES (11, 1);
SELECT count(*) AS cnt_val1_mixed FROM roaring_test WHERE val = 1;

-- ----------------------------------------------------------------
-- 5. VACUUM merges pending list into main index
-- ----------------------------------------------------------------
VACUUM roaring_test;

SELECT count(*) AS cnt_val1_post_merge FROM roaring_test WHERE val = 1;
SELECT count(*) AS cnt_val4_post_merge FROM roaring_test WHERE val = 4;

-- ----------------------------------------------------------------
-- 6. ambulkdelete: partial delete then VACUUM
-- ----------------------------------------------------------------
DELETE FROM roaring_test WHERE id = 1;  -- remove one val=1 row (id=1)
VACUUM roaring_test;
SELECT count(*) AS cnt_val1_partial FROM roaring_test WHERE val = 1;

-- ----------------------------------------------------------------
-- 7. ambulkdelete: delete all rows for a value
-- ----------------------------------------------------------------
DELETE FROM roaring_test WHERE val = 2;
VACUUM roaring_test;
SELECT count(*) AS cnt_val2_deleted FROM roaring_test WHERE val = 2;

-- ----------------------------------------------------------------
-- 8. Delete-all then reinsert (empty-bitmap + pending-merge path)
-- ----------------------------------------------------------------
DELETE FROM roaring_test WHERE val = 3;
INSERT INTO roaring_test VALUES (20, 3), (21, 3);  -- 2 new rows via pending
VACUUM roaring_test;
SELECT count(*) AS cnt_val3_reinserted FROM roaring_test WHERE val = 3;

-- val=4 unaffected throughout
SELECT count(*) AS cnt_val4_stable FROM roaring_test WHERE val = 4;

-- ----------------------------------------------------------------
-- 9. int4 operator class
-- ----------------------------------------------------------------
CREATE TABLE roaring_int4 (val int4);
INSERT INTO roaring_int4 SELECT generate_series(1, 5);
CREATE INDEX roaring_int4_idx ON roaring_int4 USING roaring (val);
VACUUM roaring_int4;

SELECT count(*) AS cnt_int4_3  FROM roaring_int4 WHERE val = 3;   -- 1
SELECT count(*) AS cnt_int4_10 FROM roaring_int4 WHERE val = 10;  -- 0

-- ----------------------------------------------------------------
-- 10. REINDEX CONCURRENTLY smoke test
-- ----------------------------------------------------------------
CREATE TABLE reindex_test (id int, val bigint);
INSERT INTO reindex_test SELECT i, (i % 5) + 1 FROM generate_series(1, 20) i;
CREATE INDEX reindex_test_idx ON reindex_test USING roaring (val);
VACUUM reindex_test;

SET enable_seqscan = off;
SELECT count(*) AS before_reindex FROM reindex_test WHERE val = 3;

REINDEX INDEX CONCURRENTLY reindex_test_idx;

SELECT count(*) AS after_reindex FROM reindex_test WHERE val = 3;
RESET enable_seqscan;
DROP TABLE reindex_test;

-- ----------------------------------------------------------------
-- 11. Free-list recycling: repeated insert+vacuum cycles
--
-- Each VACUUM merges the pending chain and recycles the pending pages
-- onto the free list.  The next round of inserts should reuse those
-- pages.  This test verifies correctness after multiple recycling
-- rounds: the recycled pages must not carry stale data.
--
-- Round A: insert 600 rows (val 1..6, 100 each).  Insert 600 more rows
-- with val=99 all going to one shard, pushing that shard past
-- ROARING_PENDING_PER_PAGE (508) so roaring_alloc_page is exercised on
-- the next VACUUM's Step C allocation.
-- ----------------------------------------------------------------
CREATE TABLE recycle_test (id serial, val bigint);

SET enable_seqscan = off;

-- Round A
INSERT INTO recycle_test (val) SELECT (i % 6) + 1 FROM generate_series(1, 600) i;
VACUUM recycle_test;
SELECT count(*) AS r_a_val1 FROM recycle_test WHERE val = 1;  -- expect 100
SELECT count(*) AS r_a_val6 FROM recycle_test WHERE val = 6;  -- expect 100

-- Round B: trigger a page extension in the shard for val=99 (509 inserts
-- into one shard ensures roaring_alloc_page is called on the next VACUUM).
INSERT INTO recycle_test (val) SELECT 99 FROM generate_series(1, 509);
VACUUM recycle_test;
SELECT count(*) AS r_b_val99 FROM recycle_test WHERE val = 99;  -- expect 509
SELECT count(*) AS r_b_val1  FROM recycle_test WHERE val = 1;   -- expect 100

-- Round C: reuse recycled pages; verify no data corruption.
INSERT INTO recycle_test (val) SELECT (i % 6) + 1 FROM generate_series(1, 300) i;
VACUUM recycle_test;
SELECT count(*) AS r_c_val1 FROM recycle_test WHERE val = 1;  -- expect 150

RESET enable_seqscan;
DROP TABLE recycle_test;

-- ----------------------------------------------------------------
-- Cleanup
-- ----------------------------------------------------------------
RESET enable_seqscan;
DROP TABLE roaring_test;
DROP TABLE roaring_int4;
