#!/usr/bin/env bash
# bench/multicolumn.sh — multi-column AND benchmark
#
# Demonstrates roaring's advantage for arbitrary multi-column equality filters.
#
# A btree covers one column-order combination per index.  For k-column AND
# queries over N columns with unknown access patterns you'd need C(N,k)*k!
# composite indexes to cover every permutation — or fall back to BitmapAnd
# of single-column btrees, each of which reads O(rows/ndistinct) leaf pages
# before the intersection even starts.
#
# Roaring reads one serialised bitmap per column (O(1) pages at typical
# cardinalities) and hands TIDs to PostgreSQL's TIDBitmap for BitmapAnd.
# Index size is also 10-100x smaller than btree at low cardinality, so more
# of it fits in shared_buffers.
#
# Table: 1M rows, 5 independent columns each ndistinct=100 (10K rows/value).
# Query: WHERE c1=X AND c2=Y AND c3=Z  (random values; expected ≈1 row)
#
# Scenarios:
#   comp_bt  — composite btree on (c1,c2,c3): the pre-planned optimum;
#              requires knowing the query shape in advance
#   3x_bt    — BitmapAnd of 3 single-column btrees
#   3x_ro    — BitmapAnd of 3 roaring exact indexes
#   3x_lo    — BitmapAnd of 3 roaring_lossy indexes
#
# Usage:
#   PGDATABASE=mydb bash bench/multicolumn.sh [duration_seconds]
#
# Each pgbench run lasts DURATION seconds (default 20).  Full run ≈ 10 min.

set -euo pipefail

DB=${PGDATABASE:-contrib_regression}
NROWS=1000000
ND=100          # ndistinct per column
DURATION=${1:-20}

pgb_tps() {
    local out
    out=$(pgbench -d "$2" -T "$DURATION" -c 1 -j 1 --no-vacuum -f "$1" 2>&1) || { echo 0; return; }
    echo "$out" | grep -Eo 'tps = [0-9.]+' | tail -1 | grep -Eo '[0-9.]+$'
}

sql1()    { psql -d "$DB" -v ON_ERROR_STOP=1 -q "$@"; }
sql_val() { psql -d "$DB" -tAc "$1"; }

# ---- setup ------------------------------------------------------------------

printf '\n  building tables (1M rows × 5 cols × ndistinct=%d) ...\n' "$ND" >&2

sql1 <<SQL
DROP TABLE IF EXISTS mc_bt, mc_ro, mc_lo CASCADE;

CREATE TABLE mc_bt (
    id bigint,
    c1 int, c2 int, c3 int, c4 int, c5 int
);
INSERT INTO mc_bt
    SELECT i,
           (i % $ND) + 1,
           ((i / $ND) % $ND) + 1,
           ((i / ($ND*$ND)) % $ND) + 1,
           ((i / ($ND*$ND*$ND)) % $ND) + 1,
           ((i / ($ND*$ND*$ND*$ND)) % $ND) + 1
    FROM generate_series(1, $NROWS) i;

CREATE TABLE mc_ro AS SELECT * FROM mc_bt;
CREATE TABLE mc_lo AS SELECT * FROM mc_bt;

-- btree: composite index for (c1,c2,c3) — the pre-planned query shape
CREATE INDEX mc_bt_comp  ON mc_bt USING btree  (c1, c2, c3);
-- btree: three single-column indexes for flexible BitmapAnd
CREATE INDEX mc_bt_c1    ON mc_bt USING btree  (c1);
CREATE INDEX mc_bt_c2    ON mc_bt USING btree  (c2);
CREATE INDEX mc_bt_c3    ON mc_bt USING btree  (c3);

CREATE INDEX mc_ro_c1    ON mc_ro USING roaring       (c1);
CREATE INDEX mc_ro_c2    ON mc_ro USING roaring       (c2);
CREATE INDEX mc_ro_c3    ON mc_ro USING roaring       (c3);

CREATE INDEX mc_lo_c1    ON mc_lo USING roaring_lossy (c1);
CREATE INDEX mc_lo_c2    ON mc_lo USING roaring_lossy (c2);
CREATE INDEX mc_lo_c3    ON mc_lo USING roaring_lossy (c3);

VACUUM ANALYZE mc_bt;
VACUUM ANALYZE mc_ro;
VACUUM ANALYZE mc_lo;
SQL

# ---- index sizes ------------------------------------------------------------

BT_COMP_SZ=$(sql_val "SELECT pg_size_pretty(pg_relation_size('mc_bt_comp'))")
BT_1COL_SZ=$(sql_val "SELECT pg_size_pretty(pg_relation_size('mc_bt_c1'))")
RO_1COL_SZ=$(sql_val "SELECT pg_size_pretty(pg_relation_size('mc_ro_c1'))")
LO_1COL_SZ=$(sql_val "SELECT pg_size_pretty(pg_relation_size('mc_lo_c1'))")

# 3-index totals
BT_3_B=$(sql_val "SELECT pg_relation_size('mc_bt_c1')+pg_relation_size('mc_bt_c2')+pg_relation_size('mc_bt_c3')")
RO_3_B=$(sql_val "SELECT pg_relation_size('mc_ro_c1')+pg_relation_size('mc_ro_c2')+pg_relation_size('mc_ro_c3')")
LO_3_B=$(sql_val "SELECT pg_relation_size('mc_lo_c1')+pg_relation_size('mc_lo_c2')+pg_relation_size('mc_lo_c3')")
BT_3_SZ=$(sql_val "SELECT pg_size_pretty(pg_relation_size('mc_bt_c1')+pg_relation_size('mc_bt_c2')+pg_relation_size('mc_bt_c3'))")
RO_3_SZ=$(sql_val "SELECT pg_size_pretty(pg_relation_size('mc_ro_c1')+pg_relation_size('mc_ro_c2')+pg_relation_size('mc_ro_c3'))")
LO_3_SZ=$(sql_val "SELECT pg_size_pretty(pg_relation_size('mc_lo_c1')+pg_relation_size('mc_lo_c2')+pg_relation_size('mc_lo_c3'))")

RO_PCT=$(awk "BEGIN { if ($BT_3_B>0) printf \"%d\", $RO_3_B*100/$BT_3_B; else print 0 }")
LO_PCT=$(awk "BEGIN { if ($BT_3_B>0) printf \"%d\", $LO_3_B*100/$BT_3_B; else print 0 }")

# ---- pgbench scripts --------------------------------------------------------

F_COMP=$(mktemp /tmp/mc_comp.XXXXXX)
F_3BT=$(mktemp /tmp/mc_3bt.XXXXXX)
F_3RO=$(mktemp /tmp/mc_3ro.XXXXXX)
F_3LO=$(mktemp /tmp/mc_3lo.XXXXXX)

trap "rm -f $F_COMP $F_3BT $F_3RO $F_3LO; psql -d '$DB' -q -c 'DROP TABLE IF EXISTS mc_bt,mc_ro,mc_lo CASCADE' 2>/dev/null || true" EXIT

# Composite btree: force it over single-col indexes
cat > "$F_COMP" <<'PGB'
\set v1 random(1,100)
\set v2 random(1,100)
\set v3 random(1,100)
SET enable_seqscan=off;
SET enable_bitmapscan=off;
SELECT count(*) FROM mc_bt WHERE c1=:v1 AND c2=:v2 AND c3=:v3;
PGB

# 3 single-column btrees via BitmapAnd — disable composite index
cat > "$F_3BT" <<'PGB'
\set v1 random(1,100)
\set v2 random(1,100)
\set v3 random(1,100)
SET enable_seqscan=off;
SET enable_bitmapscan=on;
SELECT count(*) FROM mc_bt WHERE c1=:v1 AND c2=:v2 AND c3=:v3;
PGB

cat > "$F_3RO" <<'PGB'
\set v1 random(1,100)
\set v2 random(1,100)
\set v3 random(1,100)
SET enable_seqscan=off;
SELECT count(*) FROM mc_ro WHERE c1=:v1 AND c2=:v2 AND c3=:v3;
PGB

cat > "$F_3LO" <<'PGB'
\set v1 random(1,100)
\set v2 random(1,100)
\set v3 random(1,100)
SET enable_seqscan=off;
SELECT count(*) FROM mc_lo WHERE c1=:v1 AND c2=:v2 AND c3=:v3;
PGB

# Drop composite index so the 3-btree run uses only single-col indexes
sql1 -c "DROP INDEX mc_bt_comp;"

# ---- run --------------------------------------------------------------------

printf '\nMulti-column AND benchmark  nrows=%d  ndistinct=%d  duration=%ds\n' \
    "$NROWS" "$ND" "$DURATION"
printf 'Query: WHERE c1=X AND c2=Y AND c3=Z  (expected ≈1 row per query)\n\n'

printf '%-20s  %12s  %12s  %8s\n' "scenario" "index_size(3×)" "size_vs_3bt%" "TPS"
printf '%s\n' "$(printf -- '-%.0s' {1..58})"

# Rebuild composite index for its run
sql1 -c "CREATE INDEX mc_bt_comp ON mc_bt USING btree (c1,c2,c3);"
printf '  running comp_bt ...\r' >&2
TPS_COMP=$(pgb_tps "$F_COMP" "$DB")
sql1 -c "DROP INDEX mc_bt_comp;"

printf '  running 3x_bt   ...\r' >&2
TPS_3BT=$(pgb_tps "$F_3BT" "$DB")

printf '  running 3x_ro   ...\r' >&2
TPS_3RO=$(pgb_tps "$F_3RO" "$DB")

printf '  running 3x_lo   ...\r' >&2
TPS_3LO=$(pgb_tps "$F_3LO" "$DB")

printf '%-20s  %12s  %12s  %8s\n' \
    "comp_bt(c1,c2,c3)" "$BT_COMP_SZ" "— (baseline)" "$(printf '%.0f' "$TPS_COMP")"
printf '%-20s  %12s  %12s  %8s\n' \
    "3x_bt" "$BT_3_SZ" "100%" "$(printf '%.0f' "$TPS_3BT")"
printf '%-20s  %12s  %12s  %8s\n' \
    "3x_roaring" "$RO_3_SZ" "${RO_PCT}%" "$(printf '%.0f' "$TPS_3RO")"
printf '%-20s  %12s  %12s  %8s\n' \
    "3x_roaring_lossy" "$LO_3_SZ" "${LO_PCT}%" "$(printf '%.0f' "$TPS_3LO")"

printf '\n'
printf 'comp_bt  — composite btree on (c1,c2,c3): fastest, but only for this exact query shape\n'
printf '3x_bt    — BitmapAnd of 3 single-column btrees: flexible, high index I/O at low ndistinct\n'
printf '3x_ro    — BitmapAnd of 3 roaring exact: flexible, index reads O(1) pages per column\n'
printf '3x_lo    — BitmapAnd of 3 roaring_lossy: flexible, smallest indexes, recheck on heap\n'
printf '\nIndex sizes above are the 3-index total (3 × one column).\n'
printf 'With 20 columns you would multiply: btree 20×, roaring 20× at the respective per-index size.\n'
