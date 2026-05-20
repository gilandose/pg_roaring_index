#!/usr/bin/env bash
# bench/sweep.sh — cardinality sweep benchmark
#
# Varies ndistinct (10 → 100 000) at fixed 1 M rows and measures:
#   • equality lookup TPS         (WHERE val = X)
#   • multi-value OR lookup TPS   (WHERE val IN (x1 .. x10))
#   • index size
#   • build time
#
# Compares: btree vs roaring (exact) vs roaring_lossy (page-level)
#
# Usage:
#   PGDATABASE=mydb bash bench/sweep.sh [duration_seconds]
#
# Each pgbench run lasts DURATION seconds (default 20).  Full sweep ≈ 25 min.

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

# ---- per-ndistinct setup ----------------------------------------------------

setup() {
    local nd=$1
    sql1 <<SQL
DROP TABLE IF EXISTS sw_bt, sw_ro, sw_lo CASCADE;
CREATE TABLE sw_bt (id bigint, val bigint, payload text);
CREATE TABLE sw_ro (id bigint, val bigint, payload text);
CREATE TABLE sw_lo (id bigint, val bigint, payload text);
INSERT INTO sw_bt
    SELECT i, (i % $nd) + 1, repeat('x',20)
    FROM   generate_series(1, $NROWS) i;
INSERT INTO sw_ro SELECT * FROM sw_bt;
INSERT INTO sw_lo SELECT * FROM sw_bt;
CREATE INDEX sw_bt_idx ON sw_bt USING btree        (val);
CREATE INDEX sw_ro_idx ON sw_ro USING roaring      (val);
CREATE INDEX sw_lo_idx ON sw_lo USING roaring_lossy (val);
VACUUM ANALYZE sw_bt;
VACUUM ANALYZE sw_ro;
VACUUM ANALYZE sw_lo;
SQL
}

teardown() { sql1 -c "DROP TABLE IF EXISTS sw_bt, sw_ro, sw_lo CASCADE;" 2>/dev/null || true; }

trap teardown EXIT

# ---- temp pgbench script files ----------------------------------------------

F_BT=$(mktemp /tmp/sw_bt.XXXXXX)
F_RO=$(mktemp /tmp/sw_ro.XXXXXX)
F_LO=$(mktemp /tmp/sw_lo.XXXXXX)
F_BT_OR=$(mktemp /tmp/sw_bt_or.XXXXXX)
F_RO_OR=$(mktemp /tmp/sw_ro_or.XXXXXX)
F_LO_OR=$(mktemp /tmp/sw_lo_or.XXXXXX)
trap "rm -f $F_BT $F_RO $F_LO $F_BT_OR $F_RO_OR $F_LO_OR; teardown" EXIT

write_scripts() {
    local nd=$1
    printf '\set v random(1,%d)\nSET enable_seqscan=off;\nSELECT count(*) FROM sw_bt WHERE val=:v;\n' "$nd" > "$F_BT"
    printf '\set v random(1,%d)\nSET enable_seqscan=off;\nSELECT count(*) FROM sw_ro WHERE val=:v;\n' "$nd" > "$F_RO"
    printf '\set v random(1,%d)\nSET enable_seqscan=off;\nSELECT count(*) FROM sw_lo WHERE val=:v;\n' "$nd" > "$F_LO"

    # 10-value IN(...) — exercises amsearcharray path
    printf '\set v random(1,%d)\nSET enable_seqscan=off;\nSELECT count(*) FROM sw_bt WHERE val IN (:v,:v+1,:v+2,:v+3,:v+4,:v+5,:v+6,:v+7,:v+8,:v+9);\n' "$nd" > "$F_BT_OR"
    printf '\set v random(1,%d)\nSET enable_seqscan=off;\nSELECT count(*) FROM sw_ro WHERE val IN (:v,:v+1,:v+2,:v+3,:v+4,:v+5,:v+6,:v+7,:v+8,:v+9);\n' "$nd" > "$F_RO_OR"
    printf '\set v random(1,%d)\nSET enable_seqscan=off;\nSELECT count(*) FROM sw_lo WHERE val IN (:v,:v+1,:v+2,:v+3,:v+4,:v+5,:v+6,:v+7,:v+8,:v+9);\n' "$nd" > "$F_LO_OR"
}

# ---- sweep ------------------------------------------------------------------

printf '\nCardinality sweep  nrows=%d  duration=%ds per test\n' "$NROWS" "$DURATION"
printf 'btree vs roaring (exact TID) vs roaring_lossy (page-level, amrecheck=true)\n\n'

printf '%-10s %-9s | %-9s %-9s %-9s %-7s %-7s | %-9s %-9s %-9s | %-9s %-9s %-9s\n' \
    "ndistinct" "TIDs/val" \
    "bt_size" "ro_size" "lo_size" "ro/bt%" "lo/bt%" \
    "bt_eq" "ro_eq" "lo_eq" \
    "bt_in" "ro_in" "lo_in"
printf '%s\n' "$(printf -- '-%.0s' {1..130})"

for ND in 10 100 1000 10000 100000; do
    TIDS=$((NROWS / ND))

    printf '  loading  ndistinct=%-8d ...\r' "$ND" >&2
    setup "$ND"
    write_scripts "$ND"

    BT_SZ=$(sql_val "SELECT pg_size_pretty(pg_relation_size('sw_bt_idx'))")
    RO_SZ=$(sql_val "SELECT pg_size_pretty(pg_relation_size('sw_ro_idx'))")
    LO_SZ=$(sql_val "SELECT pg_size_pretty(pg_relation_size('sw_lo_idx'))")

    BT_SZ_B=$(sql_val "SELECT pg_relation_size('sw_bt_idx')")
    RO_SZ_B=$(sql_val "SELECT pg_relation_size('sw_ro_idx')")
    LO_SZ_B=$(sql_val "SELECT pg_relation_size('sw_lo_idx')")
    RO_PCT=$(awk "BEGIN { if ($BT_SZ_B>0) printf \"%d\", $RO_SZ_B*100/$BT_SZ_B; else print 0 }")
    LO_PCT=$(awk "BEGIN { if ($BT_SZ_B>0) printf \"%d\", $LO_SZ_B*100/$BT_SZ_B; else print 0 }")

    printf '  running  btree eq      ndistinct=%-8d ...\r' "$ND" >&2
    BT_EQ=$(pgb_tps "$F_BT" "$DB")

    printf '  running  roaring eq    ndistinct=%-8d ...\r' "$ND" >&2
    RO_EQ=$(pgb_tps "$F_RO" "$DB")

    printf '  running  lossy eq      ndistinct=%-8d ...\r' "$ND" >&2
    LO_EQ=$(pgb_tps "$F_LO" "$DB")

    printf '  running  btree IN      ndistinct=%-8d ...\r' "$ND" >&2
    BT_OR=$(pgb_tps "$F_BT_OR" "$DB")

    printf '  running  roaring IN    ndistinct=%-8d ...\r' "$ND" >&2
    RO_OR=$(pgb_tps "$F_RO_OR" "$DB")

    printf '  running  lossy IN      ndistinct=%-8d ...\r' "$ND" >&2
    LO_OR=$(pgb_tps "$F_LO_OR" "$DB")

    printf '%-10s %-9s | %-9s %-9s %-9s %-7s %-7s | %-9s %-9s %-9s | %-9s %-9s %-9s\n' \
        "$ND" "$TIDS" \
        "$BT_SZ" "$RO_SZ" "$LO_SZ" "${RO_PCT}%" "${LO_PCT}%" \
        "$(printf '%.0f' "$BT_EQ")" "$(printf '%.0f' "$RO_EQ")" "$(printf '%.0f' "$LO_EQ")" \
        "$(printf '%.0f' "$BT_OR")" "$(printf '%.0f' "$RO_OR")" "$(printf '%.0f' "$LO_OR")"
done

printf '\n'
printf 'ro/bt%% = roaring exact index size as %% of btree\n'
printf 'lo/bt%% = roaring lossy index size as %% of btree\n'
printf '_eq    = equality TPS  (WHERE val = X)\n'
printf '_in    = 10-value IN() (WHERE val IN (x..x+9))\n'
printf 'lossy recheck: executor rescans matched heap pages — correct but extra CPU at high ndistinct\n'
