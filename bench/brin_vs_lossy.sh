#!/usr/bin/env bash
# bench/brin_vs_lossy.sh — BRIN vs roaring vs roaring_lossy under write churn
#
# Demonstrates the write-churn resilience claim:
#   • Sorted build: BRIN starts with near-perfect physical correlation,
#     compact index, and fast equality lookups.
#   • After DELETE+INSERT churn: new rows land at end-of-table with mixed
#     values → BRIN page ranges widen → false positives skyrocket → it
#     degrades toward a sequential scan.  roaring and roaring_lossy remain
#     correct because they store TIDs / block numbers directly.
#   • After VACUUM: roaring removes dead TIDs and shrinks.  roaring_lossy
#     removes stale block numbers via vacuumcleanup resummarize.  BRIN
#     re-summarizes the new pages but their ranges are now wide
#     (min=1, max=ndistinct), so it only partially recovers.
#
# Usage:
#   PGDATABASE=mydb bash bench/brin_vs_lossy.sh [ndistinct [duration_sec]]
#
# Defaults: ndistinct=1000, duration=10 s per pgbench run.
# Approximate wall time: 3 churn rounds × 2 phases × 4 indexes × duration
#   + build/vacuum overhead ≈ 15 min at 10 s, 30 min at 20 s.
#
# Churn design:
#   Each round deletes a distinct 1/CHURN_FACTOR of the ORIGINAL rows
#   (id % CHURN_FACTOR = round, id <= NROWS) — non-overlapping across rounds,
#   protected against deleting previously-inserted churn rows.
#   New rows are generated into a temp table and inserted into all four
#   tables identically, so they always stay in sync.
#
# Columns:
#   bt_size / ro_size / br_size / lo_size  index sizes
#   corr    pg_stats.correlation for val (updated by ANALYZE only)
#   {bt,ro,br,lo}_tps  pgbench TPS (enable_seqscan=off, enable_bitmapscan=on)

set -euo pipefail

DB=${PGDATABASE:-contrib_regression}
NROWS=1000000
NDISTINCT=${1:-1000}
DURATION=${2:-10}
CHURN_PCT=20        # % of ORIGINAL rows deleted and reinserted per round
CHURN_ROUNDS=3
CHURN_FACTOR=$(( 100 / CHURN_PCT ))   # 5 for 20% — modulo divisor for delete
CHURN_N=$(( NROWS / CHURN_FACTOR ))   # exact rows removed/added per round
ROWS_PER_VAL=$(( NROWS / NDISTINCT ))

# ---- helpers ----------------------------------------------------------------

pgb_tps() {
    local out
    out=$(pgbench -d "$DB" -T "$DURATION" -c 1 -j 1 --no-vacuum -f "$1" 2>&1) \
        || { echo 0; return; }
    echo "$out" | grep -Eo 'tps = [0-9.]+' | tail -1 | grep -Eo '[0-9.]+$'
}

sql1() { psql -d "$DB" -v ON_ERROR_STOP=1 -q "$@"; }
sql_val() { psql -d "$DB" -tAc "$1"; }

sz() {
    sql_val "SELECT pg_size_pretty(pg_relation_size('$1'))"
}

corr() {
    # correlation from pg_stats — valid only after ANALYZE
    sql_val "SELECT coalesce(round(correlation::numeric, 3)::text, '--') \
             FROM pg_stats WHERE tablename = '$1' AND attname = 'val'"
}

# ---- pgbench script files ---------------------------------------------------

F_BT=$(mktemp /tmp/bl_bt.XXXXXX)
F_RO=$(mktemp /tmp/bl_ro.XXXXXX)
F_BR=$(mktemp /tmp/bl_br.XXXXXX)
F_LO=$(mktemp /tmp/bl_lo.XXXXXX)

cleanup() {
    rm -f "$F_BT" "$F_RO" "$F_BR" "$F_LO"
    psql -d "$DB" -q -c \
        "DROP TABLE IF EXISTS bl_bt, bl_ro, bl_br, bl_lo CASCADE;
         DROP SEQUENCE IF EXISTS bl_churn_seq;" 2>/dev/null || true
}
trap cleanup EXIT

printf '\set v random(1,%d)\nSET enable_seqscan=off;\nSET enable_bitmapscan=on;\nSELECT count(*) FROM bl_bt WHERE val=:v;\n' \
    "$NDISTINCT" > "$F_BT"
printf '\set v random(1,%d)\nSET enable_seqscan=off;\nSET enable_bitmapscan=on;\nSELECT count(*) FROM bl_ro WHERE val=:v;\n' \
    "$NDISTINCT" > "$F_RO"
printf '\set v random(1,%d)\nSET enable_seqscan=off;\nSET enable_bitmapscan=on;\nSELECT count(*) FROM bl_br WHERE val=:v;\n' \
    "$NDISTINCT" > "$F_BR"
printf '\set v random(1,%d)\nSET enable_seqscan=off;\nSET enable_bitmapscan=on;\nSELECT count(*) FROM bl_lo WHERE val=:v;\n' \
    "$NDISTINCT" > "$F_LO"

# ---- output helpers ---------------------------------------------------------

print_header() {
    printf '\n%-26s | %-9s %-9s %-9s %-9s | %-6s | %-7s %-7s %-7s %-7s\n' \
        "Phase" "bt_size" "ro_size" "br_size" "lo_size" \
        "corr" "bt_tps" "ro_tps" "br_tps" "lo_tps"
    printf '%s\n' "$(printf -- '-%.0s' {1..112})"
}

measure() {
    local label="$1" do_corr="${2:-0}"
    local bt_sz ro_sz br_sz lo_sz c bt_tps ro_tps br_tps lo_tps
    bt_sz=$(sz bl_bt_idx)
    ro_sz=$(sz bl_ro_idx)
    br_sz=$(sz bl_br_idx)
    lo_sz=$(sz bl_lo_idx)
    if [[ $do_corr -eq 1 ]]; then
        c=$(corr bl_lo)
    else
        c="--"
    fi
    printf '  measuring btree  ...\r' >&2;  bt_tps=$(pgb_tps "$F_BT")
    printf '  measuring roaring...\r' >&2;  ro_tps=$(pgb_tps "$F_RO")
    printf '  measuring brin   ...\r' >&2;  br_tps=$(pgb_tps "$F_BR")
    printf '  measuring lossy  ...\r' >&2;  lo_tps=$(pgb_tps "$F_LO")
    printf '%-80s\r' '' >&2
    printf '%-26s | %-9s %-9s %-9s %-9s | %-6s | %-7.0f %-7.0f %-7.0f %-7.0f\n' \
        "$label" "$bt_sz" "$ro_sz" "$br_sz" "$lo_sz" "$c" \
        "${bt_tps:-0}" "${ro_tps:-0}" "${br_tps:-0}" "${lo_tps:-0}"
}

# ---- build: insert sorted by val (BRIN-friendly layout) ---------------------

printf '\nBRIN vs roaring vs roaring_lossy under write churn\n'
printf 'nrows=%d  ndistinct=%d  rows/val=%d  duration=%ds\n' \
    "$NROWS" "$NDISTINCT" "$ROWS_PER_VAL" "$DURATION"
printf 'churn=%d%%  (%d rows replaced) × %d rounds\n\n' \
    "$CHURN_PCT" "$CHURN_N" "$CHURN_ROUNDS"

printf 'Building tables (sorted by val for maximum BRIN correlation)...\n' >&2

sql1 <<SQL
DROP TABLE IF EXISTS bl_bt, bl_ro, bl_br, bl_lo CASCADE;
DROP SEQUENCE IF EXISTS bl_churn_seq;

CREATE TABLE bl_bt (id bigint, val bigint, payload text);
CREATE TABLE bl_ro (id bigint, val bigint, payload text);
CREATE TABLE bl_br (id bigint, val bigint, payload text);
CREATE TABLE bl_lo (id bigint, val bigint, payload text);

-- Insert sorted by val: rows 1..(NROWS/NDISTINCT) get val=1, etc.
-- Gives BRIN contiguous page runs per value → correlation ≈ 1.
INSERT INTO bl_bt
    SELECT i                               AS id,
           ((i - 1) / $ROWS_PER_VAL) + 1  AS val,
           repeat('x', 20)                AS payload
    FROM   generate_series(1, $NROWS) i;

INSERT INTO bl_ro SELECT * FROM bl_bt;
INSERT INTO bl_br SELECT * FROM bl_bt;
INSERT INTO bl_lo SELECT * FROM bl_bt;

CREATE INDEX bl_bt_idx ON bl_bt USING btree         (val);
CREATE INDEX bl_ro_idx ON bl_ro USING roaring       (val);
CREATE INDEX bl_br_idx ON bl_br USING brin          (val);
CREATE INDEX bl_lo_idx ON bl_lo USING roaring_lossy (val);

VACUUM ANALYZE bl_bt;
VACUUM ANALYZE bl_ro;
VACUUM ANALYZE bl_br;
VACUUM ANALYZE bl_lo;

-- Sequence for churn IDs — starts above NROWS to avoid collisions.
CREATE SEQUENCE bl_churn_seq START WITH $((NROWS + 1));
SQL

print_header
measure "Build (sorted)" 1

# ---- churn rounds -----------------------------------------------------------

for round in $(seq 1 "$CHURN_ROUNDS"); do
    printf '  churn round %d/%d: deleting 1/%d of original rows, reinserting at end...\n' \
        "$round" "$CHURN_ROUNDS" "$CHURN_FACTOR" >&2

    sql1 <<SQL
-- Delete exactly 1/CHURN_FACTOR of the ORIGINAL rows (id <= NROWS) using
-- a modulo pattern, different offset each round → non-overlapping subsets.
-- id <= NROWS guard ensures we never delete previously inserted churn rows.
DELETE FROM bl_bt WHERE id % $CHURN_FACTOR = $round AND id <= $NROWS;
DELETE FROM bl_ro WHERE id % $CHURN_FACTOR = $round AND id <= $NROWS;
DELETE FROM bl_br WHERE id % $CHURN_FACTOR = $round AND id <= $NROWS;
DELETE FROM bl_lo WHERE id % $CHURN_FACTOR = $round AND id <= $NROWS;

-- Generate new rows into a temp table so all four get identical data.
-- Round-robin val distribution: new rows cover all ndistinct values.
-- New IDs come from the sequence → always append at end of table.
CREATE TEMP TABLE bl_new_rows AS
    SELECT nextval('bl_churn_seq')      AS id,
           ((i - 1) % $NDISTINCT) + 1  AS val,
           repeat('x', 20)             AS payload
    FROM   generate_series(1, $CHURN_N) i;

INSERT INTO bl_bt SELECT * FROM bl_new_rows;
INSERT INTO bl_ro SELECT * FROM bl_new_rows;
INSERT INTO bl_br SELECT * FROM bl_new_rows;
INSERT INTO bl_lo SELECT * FROM bl_new_rows;

DROP TABLE bl_new_rows;
SQL

    # Measure immediately — no vacuum.  BRIN has not re-summarized new pages
    # (autosummarize=off by default).  roaring pending list has new entries.
    measure "Churn ${round} (no vacuum)" 0

    printf '  vacuuming...\r' >&2
    sql1 -c "VACUUM ANALYZE bl_bt"
    sql1 -c "VACUUM ANALYZE bl_ro"
    sql1 -c "VACUUM ANALYZE bl_br"
    sql1 -c "VACUUM ANALYZE bl_lo"
    printf '%-40s\r' '' >&2

    # After VACUUM: roaring removes dead TIDs from leaf bitmaps; roaring_lossy
    # removes stale block numbers via vacuumcleanup resummarize.  BRIN
    # re-summarizes all pages including appended ones — but those ranges now
    # span val 1..NDISTINCT, so it only partially recovers.
    measure "Churn ${round} +vacuum" 1
done

# ---- footer -----------------------------------------------------------------

printf '\n'
printf 'corr    = pg_stats.correlation for val (updated by ANALYZE; -- = stale)\n'
printf 'bt_tps  = btree TPS (reference baseline)\n'
printf 'ro_tps  = roaring TPS  (exact; amrecheck=false; dead TIDs removed by ambulkdelete)\n'
printf 'br_tps  = BRIN TPS     (amrecheck=true; degrades as page ranges widen after churn)\n'
printf 'lo_tps  = roaring_lossy TPS (amrecheck=true; pending list scanned pre-vacuum)\n'
printf '\n'
printf 'After churn: appended pages contain all %d values → BRIN ranges for\n' "$NDISTINCT"
printf 'those pages have min=1 max=%d → every equality query touches them.\n' "$NDISTINCT"
printf 'Post-vacuum BRIN re-summarizes but range widths stay wide — no recovery.\n'
printf 'roaring removes dead TIDs on vacuum (exact, no false positives), but ro_size grows\n'
printf '  as live TIDs scatter across more block ranges — more heap pages per query.\n'
printf 'roaring_lossy keeps all new pages (each contains all values) — lo_size stays large,\n'
printf '  but amrecheck is cheap so lo_tps stays high despite the bloated bitmap.\n'
