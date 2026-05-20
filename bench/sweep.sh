#!/usr/bin/env bash
# bench/sweep.sh — cardinality sweep benchmark
#
# Varies ndistinct (10 → 100 000) at fixed 1 M rows and measures:
#   • equality lookup TPS         (WHERE val = X)
#   • multi-value OR lookup TPS   (WHERE val IN (x1 .. x10))
#   • index size
#   • build time
#
# Usage:
#   PGDATABASE=mydb bash bench/sweep.sh [duration_seconds]
#
# Each pgbench run lasts DURATION seconds (default 20).  Full sweep ≈ 15 min.
# Note: GIN has no scalar bigint opclass in PG18; comparison is btree vs roaring.

set -euo pipefail

DB=${PGDATABASE:-contrib_regression}
NROWS=1000000
DURATION=${1:-20}

# ---- helpers ----------------------------------------------------------------

pgb_tps() {
    # pgb_tps <script_file> <db>  → prints TPS as a plain number
    local out
    out=$(pgbench -d "$2" -T "$DURATION" -c 1 -j 1 --no-vacuum -f "$1" 2>&1) || { echo 0; return; }
    echo "$out" | grep -Eo 'tps = [0-9.]+' | tail -1 | grep -Eo '[0-9.]+$'
}

sql1() { psql -d "$DB" -v ON_ERROR_STOP=1 -q "$@"; }
sql_val() { psql -d "$DB" -tAc "$1"; }

ratio() { awk "BEGIN { if ($2>0) printf \"%.2f\", $1/$2; else print \"N/A\" }"; }

# ---- per-ndistinct setup ----------------------------------------------------

setup() {
    local nd=$1
    sql1 <<SQL
DROP TABLE IF EXISTS sw_bt, sw_ro CASCADE;
CREATE TABLE sw_bt (id bigint, val bigint, payload text);
CREATE TABLE sw_ro (id bigint, val bigint, payload text);
INSERT INTO sw_bt
    SELECT i, (i % $nd) + 1, repeat('x',20)
    FROM   generate_series(1, $NROWS) i;
INSERT INTO sw_ro SELECT * FROM sw_bt;
CREATE INDEX sw_bt_idx ON sw_bt USING btree   (val);
CREATE INDEX sw_ro_idx ON sw_ro USING roaring (val);
VACUUM ANALYZE sw_bt;
VACUUM ANALYZE sw_ro;
SQL
}

teardown() { sql1 -c "DROP TABLE IF EXISTS sw_bt, sw_ro CASCADE;" 2>/dev/null || true; }

trap teardown EXIT

# ---- temp pgbench script files ----------------------------------------------

F_BT=$(mktemp /tmp/sw_bt.XXXXXX)
F_RO=$(mktemp /tmp/sw_ro.XXXXXX)
F_BT_OR=$(mktemp /tmp/sw_bt_or.XXXXXX)
F_RO_OR=$(mktemp /tmp/sw_ro_or.XXXXXX)
trap "rm -f $F_BT $F_RO $F_BT_OR $F_RO_OR; teardown" EXIT

write_scripts() {
    local nd=$1
    printf '\set v random(1,%d)\nSET enable_seqscan=off;\nSELECT count(*) FROM sw_bt WHERE val=:v;\n' "$nd" > "$F_BT"
    printf '\set v random(1,%d)\nSET enable_seqscan=off;\nSELECT count(*) FROM sw_ro WHERE val=:v;\n' "$nd" > "$F_RO"

    # 10-value IN(...) — exercises amsearcharray path on roaring
    printf '\set v random(1,%d)\nSET enable_seqscan=off;\nSELECT count(*) FROM sw_bt WHERE val IN (:v,:v+1,:v+2,:v+3,:v+4,:v+5,:v+6,:v+7,:v+8,:v+9);\n' "$nd" > "$F_BT_OR"
    printf '\set v random(1,%d)\nSET enable_seqscan=off;\nSELECT count(*) FROM sw_ro WHERE val IN (:v,:v+1,:v+2,:v+3,:v+4,:v+5,:v+6,:v+7,:v+8,:v+9);\n' "$nd" > "$F_RO_OR"
}

# ---- sweep ------------------------------------------------------------------

printf '\nCardinality sweep  nrows=%d  duration=%ds per test\n' "$NROWS" "$DURATION"
printf 'Each AM benchmarked in isolation (one index per table).\n\n'

printf '%-10s %-9s | %-9s %-9s %-7s | %-9s %-9s | %-9s %-9s\n' \
    "ndistinct" "TIDs/val" "bt_size" "ro_size" "ro/bt%" \
    "bt_eq_tps" "ro_eq_tps" "bt_or_tps" "ro_or_tps"
printf '%s\n' "$(printf -- '-%.0s' {1..95})"

for ND in 10 100 1000 10000 100000; do
    TIDS=$((NROWS / ND))

    printf '  loading  ndistinct=%-8d ...\r' "$ND" >&2
    setup "$ND"
    write_scripts "$ND"

    BT_SZ=$(sql_val "SELECT pg_size_pretty(pg_relation_size('sw_bt_idx'))")
    RO_SZ=$(sql_val "SELECT pg_size_pretty(pg_relation_size('sw_ro_idx'))")

    BT_SZ_B=$(sql_val "SELECT pg_relation_size('sw_bt_idx')")
    RO_SZ_B=$(sql_val "SELECT pg_relation_size('sw_ro_idx')")
    RO_PCT=$(awk "BEGIN { if ($BT_SZ_B>0) printf \"%d\", $RO_SZ_B*100/$BT_SZ_B; else print 0 }")

    printf '  running  btree eq  ndistinct=%-8d ...\r' "$ND" >&2
    BT_EQ=$(pgb_tps "$F_BT" "$DB")

    printf '  running  roaring eq ndistinct=%-8d ...\r' "$ND" >&2
    RO_EQ=$(pgb_tps "$F_RO" "$DB")

    printf '  running  btree OR  ndistinct=%-8d ...\r' "$ND" >&2
    BT_OR=$(pgb_tps "$F_BT_OR" "$DB")

    printf '  running  roaring OR ndistinct=%-8d ...\r' "$ND" >&2
    RO_OR=$(pgb_tps "$F_RO_OR" "$DB")

    printf '%-10s %-9s | %-9s %-9s %-7s | %-9s %-9s | %-9s %-9s\n' \
        "$ND" "$TIDS" "$BT_SZ" "$RO_SZ" "${RO_PCT}%" \
        "$(printf '%.0f' "$BT_EQ")" "$(printf '%.0f' "$RO_EQ")" \
        "$(printf '%.0f' "$BT_OR")" "$(printf '%.0f' "$RO_OR")"
done

printf '\n'
printf 'ro/bt%%    = roaring index size as %% of btree (lower = smaller)\n'
printf 'bt_eq_tps = btree  equality TPS   (WHERE val = X)\n'
printf 'ro_eq_tps = roaring equality TPS  (WHERE val = X)\n'
printf 'bt_or_tps = btree  10-value IN    (WHERE val IN (x..x+9))\n'
printf 'ro_or_tps = roaring 10-value IN   (exercises amsearcharray path)\n'
