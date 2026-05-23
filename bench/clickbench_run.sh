#!/usr/bin/env bash
set -e

echo "Setting up ClickBench indexes..."
psql -f clickbench_indexes.sql

echo "Creating pgbench scripts..."

cat << 'EOF' > bench_cb_btree.sql
\set region random(1, 100)
\set os random(1, 10)
SELECT count(*) FROM hits_btree WHERE "RegionID" = :region AND "OS" = :os;
EOF

cat << 'EOF' > bench_cb_brin.sql
\set region random(1, 100)
\set os random(1, 10)
SELECT count(*) FROM hits_brin WHERE "RegionID" = :region AND "OS" = :os;
EOF

cat << 'EOF' > bench_cb_roaring.sql
\set region random(1, 100)
\set os random(1, 10)
SELECT count(*) FROM hits_roaring WHERE "RegionID" = :region AND "OS" = :os;
EOF

echo "Running benchmarks..."
echo "--- SINGLE COLUMN B-TREES ---"
pgbench -c 1 -j 1 -T 10 -f bench_cb_btree.sql

echo "--- BRIN ---"
pgbench -c 1 -j 1 -T 10 -f bench_cb_brin.sql

echo "--- ROARING INDEX ---"
pgbench -c 1 -j 1 -T 10 -f bench_cb_roaring.sql

echo "Index Sizes:"
psql -c "SELECT indexrelname, pg_size_pretty(pg_relation_size(indexrelid)) FROM pg_stat_user_indexes WHERE indexrelname LIKE 'idx_hits_%' ORDER BY pg_relation_size(indexrelid) DESC;"
