#!/usr/bin/env bash
# bench/scan_perf.sh — scan performance benchmark + btree comparison matrix
#
# Runs each scenario against both roaring and btree indexes (one index type
# at a time so enable_seqscan=off unambiguously forces the right path).
# Appends machine-readable results to bench/results/scan_perf_matrix.tsv.
#
# Scenarios
# ---------
# fix1_present_multi  4-col AND (c1 nd=5, c2 nd=100, c3 nd=2000, c4 nd=25K),
#                     all values present.  scan-perf processes c4 first (40
#                     rows) before deserializing the 200K c1 bitmap.
#
# fix1_absent_multi   Same 4-col AND but c4 queried 1..50000 (only 1..25000
#                     exist).  ~50% of queries hit an absent c4 → scan-perf
#                     early-exits after one peek; main deserializes c1/c2/c3.
#
# fix1_single         Single-column lookup on c4 only (nd=25K).  Baseline:
#                     no multi-column overhead for either index type.
#
# fix2_IN_pending     IN(50 values) on a table with a 50K-entry roaring
#                     pending list (no VACUUM after last insert).  scan-perf
#                     walks the chain once; main walks it 50 times.
#                     btree: no pending list concept; all 250K rows indexed.
#
# fix3_overflow       Single equality on val (nd=5, ~100K rows/value).
#                     Roaring bitmaps span ~7 overflow pages; scan-perf
#                     prefetches one page ahead.
#
# Usage:
#   PGDATABASE=bench_db bash bench/scan_perf.sh [duration_secs [results_dir]]
#
# Output:
#   Formatted table to stdout; rows appended to $results_dir/scan_perf_matrix.tsv

set -euo pipefail

DB=${PGDATABASE:-contrib_regression}
DURATION=${1:-15}
RESULTS_DIR="${2:-bench/results}"
MATRIX_FILE="$RESULTS_DIR/scan_perf_matrix.tsv"

sql()   { psql -d "$DB" -v ON_ERROR_STOP=1 -q "$@"; }
sql_q() { psql -d "$DB" -v ON_ERROR_STOP=1 -q -c "$1"; }

pgb_run() {
    local file="$1"
    local out
    out=$(pgbench -d "$DB" -T "$DURATION" -c 1 -j 1 --no-vacuum -f "$file" 2>&1) \
        || { echo "0 0"; return; }
    local tps lat
    tps=$(printf '%s\n' "$out" | grep -Eo 'tps = [0-9.]+' | tail -1 | grep -Eo '[0-9.]+')
    lat=$(printf '%s\n' "$out" | grep -Eo 'latency average = [0-9.]+' | grep -Eo '[0-9.]+$')
    printf '%s %s\n' "${tps:-0}" "${lat:-0}"
}

hr() { printf '%s\n' "$(printf -- '-%.0s' {1..72})"; }

# ============================================================
# Setup tables (data only — indexes created per-phase below)
# ============================================================
printf '\nSetting up benchmark tables...\n' >&2

sql <<'SQL'
DROP TABLE IF EXISTS sp_card, sp_pending, sp_overflow CASCADE;

-- Fix 1: 4-column cardinality spread, 1M rows.
CREATE TABLE sp_card (
    id  bigint,
    c1  int,   -- ndistinct=5,     ~200K rows/value
    c2  int,   -- ndistinct=100,   ~10K  rows/value
    c3  int,   -- ndistinct=2000,  ~500  rows/value
    c4  int    -- ndistinct=25000, ~40   rows/value
);
INSERT INTO sp_card
    SELECT i,
           (i % 5)     + 1,
           (i % 100)   + 1,
           (i % 2000)  + 1,
           (i % 25000) + 1
    FROM generate_series(1, 1000000) i;
VACUUM ANALYZE sp_card;

-- Fix 2: base 200K rows; 50K more inserted after roaring index creation
-- (go to pending list for roaring; just committed rows for btree).
CREATE TABLE sp_pending (id bigint, val int);
INSERT INTO sp_pending
    SELECT i, (i % 500) + 1
    FROM generate_series(1, 200000) i;
VACUUM ANALYZE sp_pending;

-- Fix 3: 500K rows, ndistinct=5 → ~100K rows/value (~7 overflow pages/bitmap).
CREATE TABLE sp_overflow (id bigint, val int);
INSERT INTO sp_overflow
    SELECT i, (i % 5) + 1
    FROM generate_series(1, 500000) i;
VACUUM ANALYZE sp_overflow;
SQL

printf 'Tables ready.\n' >&2

# ============================================================
# pgbench scripts (shared across both phases)
# ============================================================

F_MULTI_PRESENT=$(mktemp /tmp/sp_cmp.XXXXXX)
F_MULTI_ABSENT=$(mktemp /tmp/sp_cma.XXXXXX)
F_SINGLE=$(mktemp /tmp/sp_cs.XXXXXX)
F_PEND=$(mktemp /tmp/sp_pend.XXXXXX)
F_OVF=$(mktemp /tmp/sp_ovf.XXXXXX)

trap "rm -f '$F_MULTI_PRESENT' '$F_MULTI_ABSENT' '$F_SINGLE' '$F_PEND' '$F_OVF';
      psql -d '$DB' -q -c 'DROP TABLE IF EXISTS sp_card,sp_pending,sp_overflow CASCADE' 2>/dev/null || true" EXIT

# fix1_present_multi: 4-col AND, all values present
cat > "$F_MULTI_PRESENT" <<'PGB'
\set v1 random(1, 5)
\set v2 random(1, 100)
\set v3 random(1, 2000)
\set v4 random(1, 25000)
SET enable_seqscan = off;
SELECT count(*) FROM sp_card WHERE c1 = :v1 AND c2 = :v2 AND c3 = :v3 AND c4 = :v4;
PGB

# fix1_absent_multi: 4-col AND, ~50% c4 absent
cat > "$F_MULTI_ABSENT" <<'PGB'
\set v1 random(1, 5)
\set v2 random(1, 100)
\set v3 random(1, 2000)
\set v4 random(1, 50000)
SET enable_seqscan = off;
SELECT count(*) FROM sp_card WHERE c1 = :v1 AND c2 = :v2 AND c3 = :v3 AND c4 = :v4;
PGB

# fix1_single: single-col on c4 only
cat > "$F_SINGLE" <<'PGB'
\set v4 random(1, 25000)
SET enable_seqscan = off;
SELECT count(*) FROM sp_card WHERE c4 = :v4;
PGB

# fix2_IN_pending: IN(50 contiguous values)
cat > "$F_PEND" <<'PGB'
\set b random(1, 450)
SET enable_seqscan = off;
SELECT count(*) FROM sp_pending
WHERE val IN (:b,    :b+1,  :b+2,  :b+3,  :b+4,
              :b+5,  :b+6,  :b+7,  :b+8,  :b+9,
              :b+10, :b+11, :b+12, :b+13, :b+14,
              :b+15, :b+16, :b+17, :b+18, :b+19,
              :b+20, :b+21, :b+22, :b+23, :b+24,
              :b+25, :b+26, :b+27, :b+28, :b+29,
              :b+30, :b+31, :b+32, :b+33, :b+34,
              :b+35, :b+36, :b+37, :b+38, :b+39,
              :b+40, :b+41, :b+42, :b+43, :b+44,
              :b+45, :b+46, :b+47, :b+48, :b+49);
PGB

# fix3_overflow: single lookup, large bitmap
cat > "$F_OVF" <<'PGB'
\set v random(1, 5)
SET enable_seqscan = off;
SELECT count(*) FROM sp_overflow WHERE val = :v;
PGB

# ============================================================
# Scenario registry
# ============================================================

SCN_KEYS=(
    fix1_present_multi
    fix1_absent_multi
    fix1_single
    fix2_IN_pending
    fix3_overflow
)
SCN_DESCS=(
    "4-col AND, all present (c1 nd=5…c4 nd=25K)"
    "4-col AND, ~50% c4 absent → early-exit"
    "single-col c4 lookup (nd=25K)"
    "IN(50 vals) with 50K-entry pending list"
    "single lookup, ~100K TIDs, overflow chain"
)
SCN_FILES=(
    "$F_MULTI_PRESENT"
    "$F_MULTI_ABSENT"
    "$F_SINGLE"
    "$F_PEND"
    "$F_OVF"
)
NSCN=${#SCN_KEYS[@]}

# ============================================================
# run_phase <index_type>
#
# Creates indexes of the given type, seeds the pending list if roaring,
# runs all scenarios, populates PHASE_TPS[] and PHASE_LAT[], then tears
# everything down.  Caller copies PHASE_* into its own arrays.
# (Avoids bash 4 nameref; macOS ships bash 3.2.)
# ============================================================

PHASE_TPS=()
PHASE_LAT=()

run_phase() {
    local itype="$1"
    PHASE_TPS=()
    PHASE_LAT=()

    printf '\n  [%s] creating indexes...\n' "$itype" >&2

    if [[ "$itype" == "roaring" || "$itype" == "roaring_lossy" ]]; then
        local am="$itype"
        sql <<SQL
CREATE INDEX sp_card_multi   ON sp_card     USING $am (c1, c2, c3, c4);
CREATE INDEX sp_card_single  ON sp_card     USING $am (c4);
CREATE INDEX sp_pending_idx  ON sp_pending  USING $am (val);
CREATE INDEX sp_overflow_idx ON sp_overflow USING $am (val);
SQL
        # Vacuum so 200K base rows are in the main index; then insert 50K
        # more — they land in the pending list (no post-insert vacuum).
        sql_q "VACUUM sp_pending"
        sql_q "INSERT INTO sp_pending SELECT i, (i%500)+1 FROM generate_series(200001,250000) i"
    elif [[ "$itype" == "btree_comp" ]]; then
        # Composite btree (c1,c2,c3,c4): ideal btree for this exact query.
        sql_q "INSERT INTO sp_pending SELECT i, (i%500)+1 FROM generate_series(200001,250000) i"
        sql <<'SQL'
CREATE INDEX sp_card_multi   ON sp_card     USING btree (c1, c2, c3, c4);
CREATE INDEX sp_card_single  ON sp_card     USING btree (c4);
CREATE INDEX sp_pending_idx  ON sp_pending  USING btree (val);
CREATE INDEX sp_overflow_idx ON sp_overflow USING btree (val);
SQL
        sql_q "VACUUM ANALYZE sp_pending"
    else
        # btree_sep: one single-column index per column — realistic DBA setup.
        # PostgreSQL will BitmapAnd the four BitmapIndexScans automatically.
        sql_q "INSERT INTO sp_pending SELECT i, (i%500)+1 FROM generate_series(200001,250000) i"
        sql <<'SQL'
CREATE INDEX sp_card_c1      ON sp_card     USING btree (c1);
CREATE INDEX sp_card_c2      ON sp_card     USING btree (c2);
CREATE INDEX sp_card_c3      ON sp_card     USING btree (c3);
CREATE INDEX sp_card_c4      ON sp_card     USING btree (c4);
CREATE INDEX sp_pending_idx  ON sp_pending  USING btree (val);
CREATE INDEX sp_overflow_idx ON sp_overflow USING btree (val);
SQL
        sql_q "VACUUM ANALYZE sp_pending"
    fi

    printf '  [%s] running %d scenarios × %ds each...\n' "$itype" "$NSCN" "$DURATION" >&2

    local i tps lat
    for ((i = 0; i < NSCN; i++)); do
        read -r tps lat < <(pgb_run "${SCN_FILES[$i]}")
        PHASE_TPS+=("$tps")
        PHASE_LAT+=("$lat")
        printf '    %-25s  tps=%-10s  lat=%s ms\n' \
            "${SCN_KEYS[$i]}" "$tps" "$lat" >&2
    done

    if [[ "$itype" == "btree_sep" ]]; then
        sql <<'SQL'
DROP INDEX sp_card_c1, sp_card_c2, sp_card_c3, sp_card_c4,
           sp_pending_idx, sp_overflow_idx;
SQL
    else
        sql <<'SQL'
DROP INDEX sp_card_multi, sp_card_single, sp_pending_idx, sp_overflow_idx;
SQL
    fi

    sql_q "DELETE FROM sp_pending WHERE id > 200000"
    sql_q "VACUUM sp_pending"
}

# ============================================================
# Execute both phases
# ============================================================

BRANCH=$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo "unknown")
COMMIT=$(git rev-parse --short HEAD 2>/dev/null || echo "?")
TIMESTAMP=$(date -u +%Y-%m-%dT%H:%M:%SZ)

run_phase "roaring"
declare -a R_TPS=("${PHASE_TPS[@]}") R_LAT=("${PHASE_LAT[@]}")

run_phase "roaring_lossy"
declare -a RL_TPS=("${PHASE_TPS[@]}") RL_LAT=("${PHASE_LAT[@]}")

run_phase "btree_comp"
declare -a BC_TPS=("${PHASE_TPS[@]}") BC_LAT=("${PHASE_LAT[@]}")

run_phase "btree_sep"
declare -a BS_TPS=("${PHASE_TPS[@]}") BS_LAT=("${PHASE_LAT[@]}")

# ============================================================
# Display results
# ============================================================

printf '\n'
hr
printf '  scan_perf benchmark   branch=%-20s  commit=%s\n' "$BRANCH" "$COMMIT"
printf '  duration=%ds per scenario\n' "$DURATION"
hr
printf '\n'

#  Two-line format per scenario:
#    line 1: TPS for all four index types
#    line 2: ratios (roaring_exact as numerator throughout)
printf '  %-25s  %9s  %9s  %9s  %9s\n' \
    "scenario" "r_exact" "r_lossy" "bt_comp" "bt_sep"
printf '  %-25s  %9s  %9s  %9s  %9s\n' \
    "" "r/lossy" "r/bt_c" "r/bt_s" ""
printf '  %s\n' "$(printf -- '-%.0s' {1..70})"

for ((i = 0; i < NSCN; i++)); do
    rtps="${R_TPS[$i]:-0}";  rltps="${RL_TPS[$i]:-0}"
    bctps="${BC_TPS[$i]:-0}"; bstps="${BS_TPS[$i]:-0}"
    r_l=$(awk  "BEGIN{ if ($rltps>0) printf \"%.2fx\", $rtps/$rltps; else print \"N/A\" }")
    r_bc=$(awk "BEGIN{ if ($bctps>0) printf \"%.2fx\", $rtps/$bctps; else print \"N/A\" }")
    r_bs=$(awk "BEGIN{ if ($bstps>0) printf \"%.2fx\", $rtps/$bstps; else print \"N/A\" }")
    printf '  %-25s  %9.0f  %9.0f  %9.0f  %9.0f\n' \
        "${SCN_KEYS[$i]}" "$rtps" "$rltps" "$bctps" "$bstps"
    printf '  %-25s  %9s  %9s  %9s\n' \
        "" "$r_l" "$r_bc" "$r_bs"
done

printf '\n  r/lossy = exact/lossy  r/bt_c = exact/btree_composite  r/bt_s = exact/btree_separate\n'
printf '  >1.00x means roaring_exact is faster; btree_comp = ideal btree, btree_sep = realistic\n'

# ============================================================
# Append to result matrix TSV
# ============================================================

mkdir -p "$RESULTS_DIR"

if [[ ! -f "$MATRIX_FILE" ]]; then
    printf 'timestamp\tbranch\tcommit\tscenario\tindex_type\ttps\tlat_ms\n' \
        > "$MATRIX_FILE"
fi

for ((i = 0; i < NSCN; i++)); do
    printf '%s\t%s\t%s\t%s\troaring\t%s\t%s\n' \
        "$TIMESTAMP" "$BRANCH" "$COMMIT" "${SCN_KEYS[$i]}" \
        "${R_TPS[$i]:-0}"  "${R_LAT[$i]:-0}"  >> "$MATRIX_FILE"
    printf '%s\t%s\t%s\t%s\troaring_lossy\t%s\t%s\n' \
        "$TIMESTAMP" "$BRANCH" "$COMMIT" "${SCN_KEYS[$i]}" \
        "${RL_TPS[$i]:-0}" "${RL_LAT[$i]:-0}" >> "$MATRIX_FILE"
    printf '%s\t%s\t%s\t%s\tbtree_comp\t%s\t%s\n' \
        "$TIMESTAMP" "$BRANCH" "$COMMIT" "${SCN_KEYS[$i]}" \
        "${BC_TPS[$i]:-0}" "${BC_LAT[$i]:-0}" >> "$MATRIX_FILE"
    printf '%s\t%s\t%s\t%s\tbtree_sep\t%s\t%s\n' \
        "$TIMESTAMP" "$BRANCH" "$COMMIT" "${SCN_KEYS[$i]}" \
        "${BS_TPS[$i]:-0}" "${BS_LAT[$i]:-0}" >> "$MATRIX_FILE"
done

printf '\n  results appended → %s\n\n' "$MATRIX_FILE"
