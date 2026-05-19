#!/usr/bin/env bash
# bench/run.sh — run all pg_roaring_index benchmarks and print a summary.
#
# Prerequisites:
#   psql and pgbench must be on PATH.
#   A running PostgreSQL cluster reachable via the standard PG* env vars
#   (PGHOST, PGPORT, PGUSER, PGDATABASE) or a local socket.
#
# Usage:
#   bash bench/run.sh [--setup] [--teardown] [--duration 30] [--clients 1]
#
#   --setup      Run bench/setup.sql first (drop + recreate tables/indexes)
#   --teardown   Drop tables at the end
#   --duration N pgbench duration in seconds per test (default: 30)
#   --clients  N pgbench client count (default: 1)
#   --nrows    N rows per table      (default: 1000000)
#   --ndistinct N distinct values    (default: 10000)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# ---- Defaults ----
DO_SETUP=0
DO_TEARDOWN=0
DURATION=30
CLIENTS=1
NROWS=1000000
NDISTINCT=10000

while [[ $# -gt 0 ]]; do
    case "$1" in
        --setup)      DO_SETUP=1      ;;
        --teardown)   DO_TEARDOWN=1   ;;
        --duration)   DURATION="$2";  shift ;;
        --clients)    CLIENTS="$2";   shift ;;
        --nrows)      NROWS="$2";     shift ;;
        --ndistinct)  NDISTINCT="$2"; shift ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
    shift
done

PSQL_OPTS=(-v ON_ERROR_STOP=1)
PGB_OPTS=(-T "$DURATION" -c "$CLIENTS" -j "$CLIENTS" --no-vacuum
          -D ndistinct="$NDISTINCT")

# ---- Helpers ----
hr()  { printf '%s\n' "$(printf '─%.0s' {1..60})"; }
hdr() { echo; hr; printf '  %s\n' "$*"; hr; echo; }

run_psql() { psql "${PSQL_OPTS[@]}" "$@"; }

run_pgbench() {
    local label="$1"; shift
    local out
    # pgbench exits non-zero on errors but also prints to stdout
    out=$(pgbench "${PGB_OPTS[@]}" "$@" 2>&1) || { echo "pgbench failed for $label"; echo "$out"; return 1; }
    local tps latency
    tps=$(echo "$out"     | grep -oP 'tps = \K[\d.]+' | tail -1)
    latency=$(echo "$out" | grep -oP 'latency average = \K[\d.]+')
    printf '  %-12s  %8.1f TPS   %7.3f ms avg latency\n' "$label" "${tps:-0}" "${latency:-0}"
}

# ---- Setup ----
if [[ $DO_SETUP -eq 1 ]]; then
    hdr "Setup (nrows=$NROWS, ndistinct=$NDISTINCT)"
    run_psql -v nrows="$NROWS" -v ndistinct="$NDISTINCT" \
             -f "$SCRIPT_DIR/setup.sql"
fi

# ---- Index sizes ----
hdr "Index sizes"
run_psql -f "$SCRIPT_DIR/sizes.sql"

# ---- Index build times ----
hdr "Index build times"
run_psql -f "$SCRIPT_DIR/build.sql"

# ---- Lookup throughput ----
hdr "Lookup throughput  (pgbench -T ${DURATION}s, ${CLIENTS} client(s))"
echo "  WHERE val = random(1..$NDISTINCT)"
echo
for am in btree gin brin roaring; do
    run_pgbench "$am" -f "$SCRIPT_DIR/lookup_${am}.pgbench"
done

# ---- Churn / pending list growth ----
hdr "Churn: lookup latency vs pending list depth"
run_psql -f "$SCRIPT_DIR/churn.sql"

# ---- Teardown ----
if [[ $DO_TEARDOWN -eq 1 ]]; then
    hdr "Teardown"
    run_psql -f "$SCRIPT_DIR/teardown.sql"
fi

hdr "Done"
