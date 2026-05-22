#!/usr/bin/env bash
# bench/mc_vs_btree.sh — single multi-column roaring index vs btree variants
#
# One roaring index on (c1..c5) handles any column-subset AND query with an
# internal bitmap intersection.  Compare against:
#
#   comp_bt   — one composite btree(c1..c5): fastest for its exact column order;
#               a separate composite is needed for every different access pattern
#   5×_bt     — five separate single-column btrees: any subset works; PostgreSQL
#               does an external BitmapAnd after reading O(rows/ndistinct) leaf
#               pages from each btree
#   1×mc_ro   — ONE multi-column roaring exact index: any subset; AM intersects
#               bitmaps internally before returning a single TIDBitmap
#   1×mc_lo   — ONE multi-column roaring lossy index: same + heap recheck;
#               smallest footprint
#
# Query shapes tested (all on a 1M-row, 5-column table, ndistinct=10 per col):
#   5-col AND    all 5 columns constrained     (≈ 10 rows/query)
#   4-col AND    c1..c4                        (≈100 rows/query)
#   3-col prefix c1,c2,c3                      (≈  1K rows/query)
#   3-col suffix c3,c4,c5  ← comp_bt unusable: no leading columns
#
# Usage:
#   PGDATABASE=mydb bash bench/mc_vs_btree.sh [duration_seconds]
#
# Each pgbench run lasts DURATION seconds (default 15).  Full run ≈ 12 min.

set -euo pipefail

DB=${PGDATABASE:-contrib_regression}
NROWS=1000000
ND=10          # ndistinct per column
DURATION=${1:-15}

# ---- helpers ----------------------------------------------------------------

pgb_tps() {
    local out
    out=$(pgbench -d "$2" -T "$DURATION" -c 1 -j 1 --no-vacuum -f "$1" 2>&1) \
        || { echo 0; return; }
    echo "$out" | grep -Eo 'tps = [0-9.]+' | tail -1 | grep -Eo '[0-9.]+$'
}
sql1()    { psql -d "$DB" -v ON_ERROR_STOP=1 -q "$@"; }
sql_val() { psql -d "$DB" -tAc "$1"; }
fmt_tps() { printf '%.0f' "${1:-0}"; }

# ---- build ------------------------------------------------------------------

printf '\n  building tables (%d rows × 5 cols × ndistinct=%d) ...\n' \
    "$NROWS" "$ND" >&2

sql1 <<SQL
DROP TABLE IF EXISTS mc_cbt, mc_sbt, mc_ro, mc_lo CASCADE;

CREATE TABLE mc_cbt (id bigint, c1 int, c2 int, c3 int, c4 int, c5 int);
INSERT INTO mc_cbt
    SELECT i,
           (i % $ND) + 1,
           ((i / $ND) % $ND) + 1,
           ((i / ($ND*$ND)) % $ND) + 1,
           ((i / ($ND*$ND*$ND)) % $ND) + 1,
           ((i / ($ND*$ND*$ND*$ND)) % $ND) + 1
    FROM generate_series(1, $NROWS) i;

CREATE TABLE mc_sbt AS SELECT * FROM mc_cbt;
CREATE TABLE mc_ro  AS SELECT * FROM mc_cbt;
CREATE TABLE mc_lo  AS SELECT * FROM mc_cbt;

-- Composite btree on all 5 columns (pre-planned for this exact query shape)
CREATE INDEX mc_cbt_idx ON mc_cbt USING btree (c1, c2, c3, c4, c5);

-- Five separate single-column btrees (flexible, external BitmapAnd)
CREATE INDEX mc_sbt_c1 ON mc_sbt USING btree (c1);
CREATE INDEX mc_sbt_c2 ON mc_sbt USING btree (c2);
CREATE INDEX mc_sbt_c3 ON mc_sbt USING btree (c3);
CREATE INDEX mc_sbt_c4 ON mc_sbt USING btree (c4);
CREATE INDEX mc_sbt_c5 ON mc_sbt USING btree (c5);

-- Single multi-column roaring exact index (AM does internal AND)
CREATE INDEX mc_ro_idx ON mc_ro USING roaring (c1, c2, c3, c4, c5);

-- Single multi-column roaring lossy index (AM does internal AND + heap recheck)
CREATE INDEX mc_lo_idx ON mc_lo USING roaring_lossy (c1, c2, c3, c4, c5);

VACUUM ANALYZE mc_cbt, mc_sbt, mc_ro, mc_lo;
SQL

# ---- sizes ------------------------------------------------------------------

CBT_SZ=$(sql_val "SELECT pg_size_pretty(pg_relation_size('mc_cbt_idx'))")
SBT_SZ=$(sql_val "SELECT pg_size_pretty(
    pg_relation_size('mc_sbt_c1') + pg_relation_size('mc_sbt_c2') +
    pg_relation_size('mc_sbt_c3') + pg_relation_size('mc_sbt_c4') +
    pg_relation_size('mc_sbt_c5'))")
RO_SZ=$(sql_val "SELECT pg_size_pretty(pg_relation_size('mc_ro_idx'))")
LO_SZ=$(sql_val "SELECT pg_size_pretty(pg_relation_size('mc_lo_idx'))")

CBT_B=$(sql_val "SELECT pg_relation_size('mc_cbt_idx')")
SBT_B=$(sql_val "SELECT pg_relation_size('mc_sbt_c1') + pg_relation_size('mc_sbt_c2') +
    pg_relation_size('mc_sbt_c3') + pg_relation_size('mc_sbt_c4') +
    pg_relation_size('mc_sbt_c5')")
RO_B=$(sql_val "SELECT pg_relation_size('mc_ro_idx')")
LO_B=$(sql_val "SELECT pg_relation_size('mc_lo_idx')")

pct() { awk "BEGIN { if ($1>0) printf \"%d\", int($2*100/$1); else print 0 }"; }
SBT_PCT=$(pct "$CBT_B" "$SBT_B")
RO_PCT=$(pct  "$CBT_B" "$RO_B")
LO_PCT=$(pct  "$CBT_B" "$LO_B")

# ---- pgbench scripts --------------------------------------------------------

TMPFILES=()
mkf() { local f; f=$(mktemp /tmp/mc_bench.XXXXXX); TMPFILES+=("$f"); echo "$f"; }

F_CBT5=$(mkf); F_CBT4=$(mkf); F_CBT3P=$(mkf); F_CBT3S=$(mkf)
F_SBT5=$(mkf); F_SBT4=$(mkf); F_SBT3P=$(mkf); F_SBT3S=$(mkf)
F_RO5=$(mkf);  F_RO4=$(mkf);  F_RO3P=$(mkf);  F_RO3S=$(mkf)
F_LO5=$(mkf);  F_LO4=$(mkf);  F_LO3P=$(mkf);  F_LO3S=$(mkf)

cleanup() {
    rm -f "${TMPFILES[@]+"${TMPFILES[@]}"}" 2>/dev/null || true
    psql -d "$DB" -q -c \
        'DROP TABLE IF EXISTS mc_cbt, mc_sbt, mc_ro, mc_lo CASCADE' \
        2>/dev/null || true
}
trap cleanup EXIT

# Header shared by all scripts: 5 random values in [1..ND]
HDR=$(printf '\set v1 random(1,%d)\n\set v2 random(1,%d)\n\set v3 random(1,%d)\n\set v4 random(1,%d)\n\set v5 random(1,%d)\nSET enable_seqscan=off;\n' \
    "$ND" "$ND" "$ND" "$ND" "$ND")

write_script() {
    printf '%s%s\n' "$HDR" "$2" > "$1"
}

write_script "$F_CBT5"  "SELECT count(*) FROM mc_cbt WHERE c1=:v1 AND c2=:v2 AND c3=:v3 AND c4=:v4 AND c5=:v5;"
write_script "$F_CBT4"  "SELECT count(*) FROM mc_cbt WHERE c1=:v1 AND c2=:v2 AND c3=:v3 AND c4=:v4;"
write_script "$F_CBT3P" "SELECT count(*) FROM mc_cbt WHERE c1=:v1 AND c2=:v2 AND c3=:v3;"
write_script "$F_CBT3S" "SELECT count(*) FROM mc_cbt WHERE c3=:v3 AND c4=:v4 AND c5=:v5;"
write_script "$F_SBT5"  "SELECT count(*) FROM mc_sbt WHERE c1=:v1 AND c2=:v2 AND c3=:v3 AND c4=:v4 AND c5=:v5;"
write_script "$F_SBT4"  "SELECT count(*) FROM mc_sbt WHERE c1=:v1 AND c2=:v2 AND c3=:v3 AND c4=:v4;"
write_script "$F_SBT3P" "SELECT count(*) FROM mc_sbt WHERE c1=:v1 AND c2=:v2 AND c3=:v3;"
write_script "$F_SBT3S" "SELECT count(*) FROM mc_sbt WHERE c3=:v3 AND c4=:v4 AND c5=:v5;"
write_script "$F_RO5"   "SELECT count(*) FROM mc_ro  WHERE c1=:v1 AND c2=:v2 AND c3=:v3 AND c4=:v4 AND c5=:v5;"
write_script "$F_RO4"   "SELECT count(*) FROM mc_ro  WHERE c1=:v1 AND c2=:v2 AND c3=:v3 AND c4=:v4;"
write_script "$F_RO3P"  "SELECT count(*) FROM mc_ro  WHERE c1=:v1 AND c2=:v2 AND c3=:v3;"
write_script "$F_RO3S"  "SELECT count(*) FROM mc_ro  WHERE c3=:v3 AND c4=:v4 AND c5=:v5;"
write_script "$F_LO5"   "SELECT count(*) FROM mc_lo  WHERE c1=:v1 AND c2=:v2 AND c3=:v3 AND c4=:v4 AND c5=:v5;"
write_script "$F_LO4"   "SELECT count(*) FROM mc_lo  WHERE c1=:v1 AND c2=:v2 AND c3=:v3 AND c4=:v4;"
write_script "$F_LO3P"  "SELECT count(*) FROM mc_lo  WHERE c1=:v1 AND c2=:v2 AND c3=:v3;"
write_script "$F_LO3S"  "SELECT count(*) FROM mc_lo  WHERE c3=:v3 AND c4=:v4 AND c5=:v5;"

# ---- run --------------------------------------------------------------------

run() {
    printf '  running %-30s ...\r' "$1" >&2
    pgb_tps "$2" "$DB"
}

T_CBT5=$(run "comp_bt   5-col AND"      "$F_CBT5")
T_CBT4=$(run "comp_bt   4-col AND"      "$F_CBT4")
T_CBT3P=$(run "comp_bt  3-col prefix"   "$F_CBT3P")
T_CBT3S=$(run "comp_bt  3-col suffix"   "$F_CBT3S")
T_SBT5=$(run "5x_bt     5-col AND"      "$F_SBT5")
T_SBT4=$(run "5x_bt     4-col AND"      "$F_SBT4")
T_SBT3P=$(run "5x_bt    3-col prefix"   "$F_SBT3P")
T_SBT3S=$(run "5x_bt    3-col suffix"   "$F_SBT3S")
T_RO5=$(run "1x_mc_ro   5-col AND"      "$F_RO5")
T_RO4=$(run "1x_mc_ro   4-col AND"      "$F_RO4")
T_RO3P=$(run "1x_mc_ro  3-col prefix"   "$F_RO3P")
T_RO3S=$(run "1x_mc_ro  3-col suffix"   "$F_RO3S")
T_LO5=$(run "1x_mc_lo   5-col AND"      "$F_LO5")
T_LO4=$(run "1x_mc_lo   4-col AND"      "$F_LO4")
T_LO3P=$(run "1x_mc_lo  3-col prefix"   "$F_LO3P")
T_LO3S=$(run "1x_mc_lo  3-col suffix"   "$F_LO3S")

# ---- report -----------------------------------------------------------------

printf '\nMulti-column AND benchmark  nrows=%d  ndistinct=%d/col  duration=%ds\n\n' \
    "$NROWS" "$ND" "$DURATION"

printf 'Index footprint (comp_bt = 1 index; 5×_bt = 5 indexes; mc_ro/lo = 1 index):\n'
printf '  %-22s  %10s\n' "comp_bt(c1..c5)"     "$CBT_SZ  (baseline)"
printf '  %-22s  %10s  (%d%% of comp_bt)\n' "5×_bt (5-index total)"  "$SBT_SZ" "$SBT_PCT"
printf '  %-22s  %10s  (%d%% of comp_bt)\n' "1×mc_ro"                "$RO_SZ"  "$RO_PCT"
printf '  %-22s  %10s  (%d%% of comp_bt)\n' "1×mc_lo"                "$LO_SZ"  "$LO_PCT"
printf '\n'

SEP="$(printf -- '-%.0s' {1..72})"
FMT='%-28s  %8s  %8s  %10s  %10s\n'

printf "$FMT" "query (TPS, higher=faster)" "comp_bt" "5×_bt" "1×mc_ro" "1×mc_lo"
printf '%s\n' "$SEP"

printf "$FMT" \
    "5-col AND  (≈10 rows)"   "$(fmt_tps "$T_CBT5")"  "$(fmt_tps "$T_SBT5")"  "$(fmt_tps "$T_RO5")"  "$(fmt_tps "$T_LO5")"
printf "$FMT" \
    "4-col AND  (≈100 rows)"  "$(fmt_tps "$T_CBT4")"  "$(fmt_tps "$T_SBT4")"  "$(fmt_tps "$T_RO4")"  "$(fmt_tps "$T_LO4")"
printf "$FMT" \
    "3-col prefix (≈1K rows)" "$(fmt_tps "$T_CBT3P")" "$(fmt_tps "$T_SBT3P")" "$(fmt_tps "$T_RO3P")" "$(fmt_tps "$T_LO3P")"
printf "$FMT" \
    "3-col suffix (≈1K rows)" "$(fmt_tps "$T_CBT3S")" \
    "$(fmt_tps "$T_SBT3S")" "$(fmt_tps "$T_RO3S")" "$(fmt_tps "$T_LO3S")"

printf '%s\n\n' "$SEP"

printf 'comp_bt   — composite btree(c1..c5): prefix queries use the leading edge;\n'
printf '            suffix query (no c1/c2) forces a full index scan — O(n) cost\n'
printf '5×_bt     — five separate single-col btrees; PostgreSQL intersects externally;\n'
printf '            reads O(rows/ndistinct) btree leaf pages per column before AND\n'
printf '1×mc_ro   — ONE multi-col roaring exact index; AM intersects bitmaps internally;\n'
printf '            handles any column subset; TID-level precision, no recheck\n'
printf '1×mc_lo   — ONE multi-col roaring lossy index; same + executor rechecks heap;\n'
printf '            smallest index, best for memory-constrained workloads\n'
printf '\n'
printf 'Expected row counts assume uniform distribution and independence between columns.\n'
printf 'ndistinct=%d: 1-col≈%dK rows, 2-col≈%dK rows, 3-col≈%d rows, 4-col≈%d rows, 5-col≈%d rows\n' \
    "$ND" "$((NROWS / ND / 1000))" \
    "$((NROWS / ND / ND / 1000 + 1))" \
    "$((NROWS / ND / ND / ND))" \
    "$((NROWS / ND / ND / ND / ND))" \
    "$((NROWS / ND / ND / ND / ND / ND))"
