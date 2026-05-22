#!/usr/bin/env bash
# bench/mc_multitype.sh — multi-column mixed-type AND benchmark
#
# Demonstrates roaring's advantage for arbitrary multi-column AND queries
# across columns of different types (text, int, date, uuid).
#
# A btree composite index covers one fixed column order.  For ad-hoc AND
# queries you'd need separate single-column indexes per column (BitmapAnd).
# Roaring stores one bitmap per column regardless of type — text and uuid
# use hash_bytes keys; int/date use direct int32/int64 keys.
#
# Table: 1M rows, 4 columns of mixed types:
#   status   text   ndistinct=10   (~100K rows/value)
#   category int4   ndistinct=50   (~20K  rows/value)
#   region   text   ndistinct=20   (~50K  rows/value)
#   created  date   ndistinct=365  (~2.7K rows/value)
#
# 3-column AND query: WHERE status=X AND category=Y AND region=Z
#   Expected result size ≈ 1M / (10 × 50 × 20) = 100 rows
#
# Scenarios:
#   comp_bt      — composite btree (status,category,region): the pre-planned best case
#   3x_bt        — BitmapAnd of 3 single-column btrees (flexible but larger)
#   3x_ro        — 3 roaring exact (hash+int keys)
#   3x_lo        — 3 roaring_lossy (hash+int keys, amrecheck=true)
#   4col_ro      — 4-column roaring single index: ROARING_COL_KEY encoding
#   4col_lo      — 4-column roaring_lossy single index
#
# Usage:
#   PGDATABASE=mydb bash bench/mc_multitype.sh [duration_seconds]
#
# Requires: pg_roaring_index installed from feat/text-support branch.
# Duration default 20 s per run.  Full run ≈ 12 min.

set -euo pipefail

DB=${PGDATABASE:-contrib_regression}
NROWS=1000000
ND_STATUS=10
ND_CAT=50
ND_REGION=20
ND_DATE=365
DURATION=${1:-20}

pgb_tps() {
    local out
    out=$(pgbench -d "$2" -T "$DURATION" -c 1 -j 1 --no-vacuum -f "$1" 2>&1) || { echo 0; return; }
    echo "$out" | grep -Eo 'tps = [0-9.]+' | tail -1 | grep -Eo '[0-9.]+$'
}

sql1()    { psql -d "$DB" -v ON_ERROR_STOP=1 -q "$@"; }
sql_val() { psql -d "$DB" -tAc "$1"; }

teardown() {
    sql1 -c "DROP TABLE IF EXISTS mt_bt, mt_ro, mt_lo, mt_mro, mt_mlo CASCADE;" 2>/dev/null || true
}
trap teardown EXIT

# ---- setup ------------------------------------------------------------------

printf '\n  building tables (%d rows, mixed types: text×2 int×1 date×1) ...\n' "$NROWS" >&2

# Base date: 2024-01-01 + (i % 365) days
sql1 <<SQL
DROP TABLE IF EXISTS mt_bt, mt_ro, mt_lo, mt_mro, mt_mlo CASCADE;

CREATE TABLE mt_bt (
    id       bigint,
    status   text,
    category int4,
    region   text,
    created  date
);

INSERT INTO mt_bt
    SELECT i,
           'status_'   || lpad(((i % $ND_STATUS)  + 1)::text, 3, '0'),
           (i % $ND_CAT)    + 1,
           'region_'   || lpad(((i % $ND_REGION)  + 1)::text, 3, '0'),
           DATE '2024-01-01' + (i % $ND_DATE)
    FROM generate_series(1, $NROWS) i;

CREATE TABLE mt_ro  AS SELECT * FROM mt_bt;
CREATE TABLE mt_lo  AS SELECT * FROM mt_bt;
CREATE TABLE mt_mro AS SELECT * FROM mt_bt;
CREATE TABLE mt_mlo AS SELECT * FROM mt_bt;

-- btree: composite index for the fixed (status,category,region) query shape
CREATE INDEX mt_bt_comp   ON mt_bt  USING btree         (status, category, region);
-- btree: three single-column indexes for flexible BitmapAnd
CREATE INDEX mt_bt_status ON mt_bt  USING btree         (status);
CREATE INDEX mt_bt_cat    ON mt_bt  USING btree         (category);
CREATE INDEX mt_bt_region ON mt_bt  USING btree         (region);
-- roaring: one index per column (separate, BitmapAnd-ed by executor)
CREATE INDEX mt_ro_status ON mt_ro  USING roaring        (status);
CREATE INDEX mt_ro_cat    ON mt_ro  USING roaring        (category);
CREATE INDEX mt_ro_region ON mt_ro  USING roaring        (region);
-- roaring_lossy: one index per column
CREATE INDEX mt_lo_status ON mt_lo  USING roaring_lossy  (status);
CREATE INDEX mt_lo_cat    ON mt_lo  USING roaring_lossy  (category);
CREATE INDEX mt_lo_region ON mt_lo  USING roaring_lossy  (region);
-- roaring: single multi-column index covering all 4 columns
CREATE INDEX mt_mro_idx   ON mt_mro USING roaring        (status, category, region, created);
-- roaring_lossy: single multi-column index covering all 4 columns
CREATE INDEX mt_mlo_idx   ON mt_mlo USING roaring_lossy  (status, category, region, created);

VACUUM ANALYZE mt_bt;
VACUUM ANALYZE mt_ro;
VACUUM ANALYZE mt_lo;
VACUUM ANALYZE mt_mro;
VACUUM ANALYZE mt_mlo;
SQL

# ---- index sizes ------------------------------------------------------------

BT_COMP_B=$(sql_val "SELECT pg_relation_size('mt_bt_comp')")
BT_COMP_SZ=$(sql_val "SELECT pg_size_pretty(pg_relation_size('mt_bt_comp'))")

BT_3_B=$(sql_val "SELECT pg_relation_size('mt_bt_status')+pg_relation_size('mt_bt_cat')+pg_relation_size('mt_bt_region')")
BT_3_SZ=$(sql_val "SELECT pg_size_pretty(pg_relation_size('mt_bt_status')+pg_relation_size('mt_bt_cat')+pg_relation_size('mt_bt_region'))")

RO_3_B=$(sql_val "SELECT pg_relation_size('mt_ro_status')+pg_relation_size('mt_ro_cat')+pg_relation_size('mt_ro_region')")
RO_3_SZ=$(sql_val "SELECT pg_size_pretty(pg_relation_size('mt_ro_status')+pg_relation_size('mt_ro_cat')+pg_relation_size('mt_ro_region'))")

LO_3_B=$(sql_val "SELECT pg_relation_size('mt_lo_status')+pg_relation_size('mt_lo_cat')+pg_relation_size('mt_lo_region')")
LO_3_SZ=$(sql_val "SELECT pg_size_pretty(pg_relation_size('mt_lo_status')+pg_relation_size('mt_lo_cat')+pg_relation_size('mt_lo_region'))")

MRO_B=$(sql_val "SELECT pg_relation_size('mt_mro_idx')")
MRO_SZ=$(sql_val "SELECT pg_size_pretty(pg_relation_size('mt_mro_idx'))")

MLO_B=$(sql_val "SELECT pg_relation_size('mt_mlo_idx')")
MLO_SZ=$(sql_val "SELECT pg_size_pretty(pg_relation_size('mt_mlo_idx'))")

pct() { awk "BEGIN { printf \"%d%%\", ($1>0 ? $2*100/$1 : 0) }"; }

# ---- pgbench scripts --------------------------------------------------------

F_COMP=$(mktemp /tmp/mt_comp.XXXXXX)
F_3BT=$(mktemp /tmp/mt_3bt.XXXXXX)
F_3RO=$(mktemp /tmp/mt_3ro.XXXXXX)
F_3LO=$(mktemp /tmp/mt_3lo.XXXXXX)
F_MRO=$(mktemp /tmp/mt_mro.XXXXXX)
F_MLO=$(mktemp /tmp/mt_mlo.XXXXXX)
trap "rm -f $F_COMP $F_3BT $F_3RO $F_3LO $F_MRO $F_MLO; teardown" EXIT

# Composite btree — disable BitmapAnd so it must use the composite index
cat > "$F_COMP" <<PGB
\set s random(1,$ND_STATUS)
\set c random(1,$ND_CAT)
\set r random(1,$ND_REGION)
SET enable_seqscan=off;
SET enable_bitmapscan=off;
SELECT count(*) FROM mt_bt
WHERE status='status_'||lpad(:s::text,3,'0')
  AND category=:c
  AND region='region_'||lpad(:r::text,3,'0');
PGB

# 3 single-column btrees via BitmapAnd — disable composite index
cat > "$F_3BT" <<PGB
\set s random(1,$ND_STATUS)
\set c random(1,$ND_CAT)
\set r random(1,$ND_REGION)
SET enable_seqscan=off;
SET enable_bitmapscan=on;
SELECT count(*) FROM mt_bt
WHERE status='status_'||lpad(:s::text,3,'0')
  AND category=:c
  AND region='region_'||lpad(:r::text,3,'0');
PGB

cat > "$F_3RO" <<PGB
\set s random(1,$ND_STATUS)
\set c random(1,$ND_CAT)
\set r random(1,$ND_REGION)
SET enable_seqscan=off;
SELECT count(*) FROM mt_ro
WHERE status='status_'||lpad(:s::text,3,'0')
  AND category=:c
  AND region='region_'||lpad(:r::text,3,'0');
PGB

cat > "$F_3LO" <<PGB
\set s random(1,$ND_STATUS)
\set c random(1,$ND_CAT)
\set r random(1,$ND_REGION)
SET enable_seqscan=off;
SELECT count(*) FROM mt_lo
WHERE status='status_'||lpad(:s::text,3,'0')
  AND category=:c
  AND region='region_'||lpad(:r::text,3,'0');
PGB

# Single multi-column roaring index: all 4 columns, 3-col AND query
cat > "$F_MRO" <<PGB
\set s random(1,$ND_STATUS)
\set c random(1,$ND_CAT)
\set r random(1,$ND_REGION)
SET enable_seqscan=off;
SELECT count(*) FROM mt_mro
WHERE status='status_'||lpad(:s::text,3,'0')
  AND category=:c
  AND region='region_'||lpad(:r::text,3,'0');
PGB

cat > "$F_MLO" <<PGB
\set s random(1,$ND_STATUS)
\set c random(1,$ND_CAT)
\set r random(1,$ND_REGION)
SET enable_seqscan=off;
SELECT count(*) FROM mt_mlo
WHERE status='status_'||lpad(:s::text,3,'0')
  AND category=:c
  AND region='region_'||lpad(:r::text,3,'0');
PGB

# ---- run --------------------------------------------------------------------

printf '\nMixed-type multi-column AND  nrows=%d  duration=%ds\n' "$NROWS" "$DURATION"
printf 'Columns: status(text,nd=%d) category(int4,nd=%d) region(text,nd=%d) created(date,nd=%d)\n' \
    "$ND_STATUS" "$ND_CAT" "$ND_REGION" "$ND_DATE"
printf 'Query: WHERE status=X AND category=Y AND region=Z  (expected ≈100 rows)\n\n'

printf '%-26s  %12s  %12s  %8s\n' "scenario" "index_size" "vs_3bt%" "TPS"
printf '%s\n' "$(printf -- '-%.0s' {1..64})"

# composite btree (rebuild for its run, drop for 3-btree run)
sql1 -c "DROP INDEX IF EXISTS mt_bt_comp;"
sql1 -c "CREATE INDEX mt_bt_comp ON mt_bt USING btree (status, category, region);"
printf '  running comp_bt       ...\r' >&2
TPS_COMP=$(pgb_tps "$F_COMP" "$DB")
sql1 -c "DROP INDEX mt_bt_comp;"

printf '  running 3x_bt         ...\r' >&2
TPS_3BT=$(pgb_tps "$F_3BT" "$DB")

printf '  running 3x_ro         ...\r' >&2
TPS_3RO=$(pgb_tps "$F_3RO" "$DB")

printf '  running 3x_lo         ...\r' >&2
TPS_3LO=$(pgb_tps "$F_3LO" "$DB")

printf '  running 4col_ro (mc)  ...\r' >&2
TPS_MRO=$(pgb_tps "$F_MRO" "$DB")

printf '  running 4col_lo (mc)  ...\r' >&2
TPS_MLO=$(pgb_tps "$F_MLO" "$DB")

printf '%-26s  %12s  %12s  %8s\n' \
    "comp_bt(status,cat,region)" "$BT_COMP_SZ" "$(pct $BT_3_B $BT_COMP_B)" "$(printf '%.0f' "$TPS_COMP")"
printf '%-26s  %12s  %12s  %8s\n' \
    "3x_bt (BitmapAnd)" "$BT_3_SZ" "100%" "$(printf '%.0f' "$TPS_3BT")"
printf '%-26s  %12s  %12s  %8s\n' \
    "3x_ro (exact)" "$RO_3_SZ" "$(pct $BT_3_B $RO_3_B)" "$(printf '%.0f' "$TPS_3RO")"
printf '%-26s  %12s  %12s  %8s\n' \
    "3x_lo (lossy)" "$LO_3_SZ" "$(pct $BT_3_B $LO_3_B)" "$(printf '%.0f' "$TPS_3LO")"
printf '%-26s  %12s  %12s  %8s\n' \
    "4col_ro (single mc index)" "$MRO_SZ" "$(pct $BT_3_B $MRO_B)" "$(printf '%.0f' "$TPS_MRO")"
printf '%-26s  %12s  %12s  %8s\n' \
    "4col_lo (single mc index)" "$MLO_SZ" "$(pct $BT_3_B $MLO_B)" "$(printf '%.0f' "$TPS_MLO")"

printf '\n'
printf 'comp_bt  = composite btree (status,cat,region): optimal for this exact query shape\n'
printf '3x_bt    = BitmapAnd of 3 single-column btrees: flexible, works for any column subset\n'
printf '3x_ro    = 3 separate roaring exact indexes, AND-ed by executor\n'
printf '3x_lo    = 3 separate roaring_lossy indexes (amrecheck=true)\n'
printf '4col_ro  = single roaring index on all 4 columns (ROARING_COL_KEY namespace)\n'
printf '4col_lo  = single roaring_lossy index on all 4 columns\n'
printf '\ntext/uuid columns use hash_bytes key (prototype); int/date use direct int32 key.\n'
