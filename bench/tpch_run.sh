#!/usr/bin/env bash
set -e

echo "Setting up TPC-H indexes..."
psql -f tpch_indexes.sql

echo "Creating pgbench scripts..."

cat << 'EOF' > bench_tpch_btree.sql
\set returnflag random(1, 2)
\set linestatus random(1, 2)
SELECT count(*) FROM lineitem_btree WHERE l_returnflag = CASE WHEN :returnflag=1 THEN 'R' ELSE 'A' END AND l_linestatus = CASE WHEN :linestatus=1 THEN 'F' ELSE 'O' END;
EOF

cat << 'EOF' > bench_tpch_comp.sql
\set returnflag random(1, 2)
\set linestatus random(1, 2)
SELECT count(*) FROM lineitem_comp WHERE l_returnflag = CASE WHEN :returnflag=1 THEN 'R' ELSE 'A' END AND l_linestatus = CASE WHEN :linestatus=1 THEN 'F' ELSE 'O' END;
EOF

cat << 'EOF' > bench_tpch_roaring.sql
\set returnflag random(1, 2)
\set linestatus random(1, 2)
SELECT count(*) FROM lineitem_roaring WHERE l_returnflag = CASE WHEN :returnflag=1 THEN 'R' ELSE 'A' END AND l_linestatus = CASE WHEN :linestatus=1 THEN 'F' ELSE 'O' END;
EOF

echo "Running benchmarks..."
echo "--- SINGLE COLUMN B-TREES ---"
pgbench -c 1 -j 1 -T 10 -f bench_tpch_btree.sql

echo "--- COMPOSITE B-TREE ---"
pgbench -c 1 -j 1 -T 10 -f bench_tpch_comp.sql

echo "--- ROARING INDEX ---"
pgbench -c 1 -j 1 -T 10 -f bench_tpch_roaring.sql

echo "Index Sizes:"
psql -c "SELECT indexrelname, pg_size_pretty(pg_relation_size(indexrelid)) FROM pg_stat_user_indexes WHERE indexrelname LIKE 'idx_%' ORDER BY pg_relation_size(indexrelid) DESC;"
