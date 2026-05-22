#!/usr/bin/env bash
# bench/mc_scale.sh — 10-column AND scaling: random column subsets
#
# Builds a 10-column table under four index regimes, then for each column
# count k ∈ {2,3,5,7,10} runs NREPS randomly chosen k-column AND queries
# and reports the average TPS.
#
# comp_bt is also tested on its leading k-column prefix (its best case) so
# the table shows both "I built this index for this exact query" and "I'm
# using the one index I happen to have on random column subsets".
#
# Index variants:
#   comp_bt pfx  — composite btree(c1..c10), query on leading k cols
#   comp_bt rand — same index, random k-col subset (usually partial scan)
#   10×_bt       — 10 separate single-col btrees, PostgreSQL external BitmapAnd
#   1×mc_ro      — ONE multi-col roaring exact index, AM internal AND
#   1×mc_lo      — ONE multi-col roaring lossy index, AM internal AND + recheck
#
# Usage:
#   PGDATABASE=mydb bash bench/mc_scale.sh [duration_s [nreps]]

set -euo pipefail

DB=${PGDATABASE:-contrib_regression}
NROWS=1000000
ND=10
NCOLS=10
DURATION=${1:-10}
NREPS=${2:-3}

# ---- helpers ----------------------------------------------------------------

pgb_tps() {
    local out
    out=$(pgbench -d "$2" -T "$DURATION" -c 1 -j 1 --no-vacuum -f "$1" 2>&1) \
        || { echo 0; return; }
    echo "$out" | grep -Eo 'tps = [0-9.]+' | tail -1 | grep -Eo '[0-9.]+$'
}
sql1()    { psql -d "$DB" -v ON_ERROR_STOP=1 -q "$@"; }
sql_val() { psql -d "$DB" -tAc "$1"; }

random_subset() {
    # Emit k space-separated column numbers sampled without replacement from 1..NCOLS
    python3 -c "
import random
cols = random.sample(range(1, $NCOLS+1), $1)
cols.sort()
print(' '.join(map(str, cols)))
"
}

gen_script() {
    # gen_script <table> <ndistinct> <col1> [col2 ...] -> pgbench script
    local table=$1 nd=$2; shift 2
    for c in "$@"; do printf '\set v%d random(1,%d)\n' "$c" "$nd"; done
    printf 'SET enable_seqscan=off;\nSELECT count(*) FROM %s WHERE ' "$table"
    local sep=""
    for c in "$@"; do printf '%sc%d=:v%d' "$sep" "$c" "$c"; sep=" AND "; done
    printf ';\n'
}

avg() {
    # avg val1 val2 ... -> integer average
    local s=0 n=0
    for v in "$@"; do
        [ -n "${v:-}" ] || continue
        s=$(awk "BEGIN{print $s+${v}}")
        n=$((n+1))
    done
    [ "$n" -eq 0 ] && echo 0 && return
    awk "BEGIN{printf \"%.0f\",$s/$n}"
}

rows_per_query() { awk "BEGIN{v=$NROWS/($ND^$1); if(v>=1)printf \"%.0f\",v; else printf \"<1\"}"; }

TMPFILES=()
mkf() { local f; f=$(mktemp /tmp/mc_scale.XXXXXX); TMPFILES+=("$f"); echo "$f"; }
cleanup() {
    rm -f "${TMPFILES[@]+"${TMPFILES[@]}"}" 2>/dev/null || true
    psql -d "$DB" -q -c \
        'DROP TABLE IF EXISTS mc_cbt, mc_sbt, mc_ro, mc_lo CASCADE' \
        2>/dev/null || true
}
trap cleanup EXIT

run() { printf '  %-52s\r' "running $1 ..." >&2; pgb_tps "$2" "$DB"; }

# ---- build ------------------------------------------------------------------

printf '\n  building tables (%d rows × %d cols × ndistinct=%d) ...\n' \
    "$NROWS" "$NCOLS" "$ND" >&2

sql1 <<SQL
DROP TABLE IF EXISTS mc_cbt, mc_sbt, mc_ro, mc_lo CASCADE;

CREATE TABLE mc_cbt (id bigint,
    c1 int,c2 int,c3 int,c4 int,c5 int,
    c6 int,c7 int,c8 int,c9 int,c10 int);
INSERT INTO mc_cbt
    SELECT i,
        (i%$ND)+1,                       ((i/$ND)%$ND)+1,
        ((i/$ND/$ND)%$ND)+1,             ((i/$ND/$ND/$ND)%$ND)+1,
        ((i/$ND/$ND/$ND/$ND)%$ND)+1,
        ((i/$ND/$ND/$ND/$ND/$ND)%$ND)+1,
        ((i/$ND/$ND/$ND/$ND/$ND/$ND)%$ND)+1,
        ((i/$ND/$ND/$ND/$ND/$ND/$ND/$ND)%$ND)+1,
        ((i/$ND/$ND/$ND/$ND/$ND/$ND/$ND/$ND)%$ND)+1,
        ((i/$ND/$ND/$ND/$ND/$ND/$ND/$ND/$ND/$ND)%$ND)+1
    FROM generate_series(1,$NROWS) i;

CREATE TABLE mc_sbt AS SELECT * FROM mc_cbt;
CREATE TABLE mc_ro  AS SELECT * FROM mc_cbt;
CREATE TABLE mc_lo  AS SELECT * FROM mc_cbt;

CREATE INDEX mc_cbt_idx ON mc_cbt USING btree
    (c1,c2,c3,c4,c5,c6,c7,c8,c9,c10);

CREATE INDEX mc_sbt_c1  ON mc_sbt USING btree (c1);
CREATE INDEX mc_sbt_c2  ON mc_sbt USING btree (c2);
CREATE INDEX mc_sbt_c3  ON mc_sbt USING btree (c3);
CREATE INDEX mc_sbt_c4  ON mc_sbt USING btree (c4);
CREATE INDEX mc_sbt_c5  ON mc_sbt USING btree (c5);
CREATE INDEX mc_sbt_c6  ON mc_sbt USING btree (c6);
CREATE INDEX mc_sbt_c7  ON mc_sbt USING btree (c7);
CREATE INDEX mc_sbt_c8  ON mc_sbt USING btree (c8);
CREATE INDEX mc_sbt_c9  ON mc_sbt USING btree (c9);
CREATE INDEX mc_sbt_c10 ON mc_sbt USING btree (c10);

CREATE INDEX mc_ro_idx ON mc_ro  USING roaring
    (c1,c2,c3,c4,c5,c6,c7,c8,c9,c10);
CREATE INDEX mc_lo_idx ON mc_lo  USING roaring_lossy
    (c1,c2,c3,c4,c5,c6,c7,c8,c9,c10);

VACUUM ANALYZE mc_cbt, mc_sbt, mc_ro, mc_lo;
SQL

# ---- index sizes ------------------------------------------------------------

CBT_B=$(sql_val "SELECT pg_relation_size('mc_cbt_idx')")
SBT_B=$(sql_val "SELECT pg_relation_size('mc_sbt_c1')+pg_relation_size('mc_sbt_c2')+pg_relation_size('mc_sbt_c3')+pg_relation_size('mc_sbt_c4')+pg_relation_size('mc_sbt_c5')+pg_relation_size('mc_sbt_c6')+pg_relation_size('mc_sbt_c7')+pg_relation_size('mc_sbt_c8')+pg_relation_size('mc_sbt_c9')+pg_relation_size('mc_sbt_c10')")
RO_B=$(sql_val "SELECT pg_relation_size('mc_ro_idx')")
LO_B=$(sql_val "SELECT pg_relation_size('mc_lo_idx')")

sz() { sql_val "SELECT pg_size_pretty(CAST($1 AS bigint))"; }
pct() { awk "BEGIN{if($1>0)printf \"%d\",int($2*100/$1);else print 0}"; }

CBT_SZ=$(sz "$CBT_B"); SBT_SZ=$(sz "$SBT_B"); RO_SZ=$(sz "$RO_B"); LO_SZ=$(sz "$LO_B")
SBT_PCT=$(pct "$CBT_B" "$SBT_B")
RO_PCT=$(pct  "$CBT_B" "$RO_B")
LO_PCT=$(pct  "$CBT_B" "$LO_B")

# ---- benchmark --------------------------------------------------------------

COL_COUNTS=(2 3 5 7 10)
# Parallel result arrays — index matches COL_COUNTS order
T_CBT_PFX=(); T_CBT_RND=(); T_SBT=(); T_RO=(); T_LO=(); COLS_USED=()

for k in "${COL_COUNTS[@]}"; do
    # Prefix: cols 1..k  (comp_bt best case)
    pfx=(); for ((i=1;i<=k;i++)); do pfx+=("$i"); done
    f=$(mkf); gen_script mc_cbt "$ND" "${pfx[@]}" > "$f"
    T_CBT_PFX+=("$(avg "$(run "comp_bt prefix k=$k (c1..c$k)" "$f")")")

    cbt_r=(); sbt_r=(); ro_r=(); lo_r=()
    used=()

    for ((rep=1;rep<=NREPS;rep++)); do
        read -ra rc <<< "$(random_subset "$k")"
        cname=$(printf 'c%s,' "${rc[@]}" | sed 's/,$//')
        used+=("($cname)")

        f_c=$(mkf); gen_script mc_cbt "$ND" "${rc[@]}" > "$f_c"
        f_s=$(mkf); gen_script mc_sbt "$ND" "${rc[@]}" > "$f_s"
        f_r=$(mkf); gen_script mc_ro  "$ND" "${rc[@]}" > "$f_r"
        f_l=$(mkf); gen_script mc_lo  "$ND" "${rc[@]}" > "$f_l"

        cbt_r+=("$(run "comp_bt rand  k=$k rep=$rep $cname" "$f_c")")
        sbt_r+=("$(run "10x_bt  rand  k=$k rep=$rep $cname" "$f_s")")
        ro_r+=( "$(run "mc_ro   rand  k=$k rep=$rep $cname" "$f_r")")
        lo_r+=( "$(run "mc_lo   rand  k=$k rep=$rep $cname" "$f_l")")
    done

    T_CBT_RND+=("$(avg "${cbt_r[@]}")")
    T_SBT+=(    "$(avg "${sbt_r[@]}")")
    T_RO+=(     "$(avg "${ro_r[@]}")")
    T_LO+=(     "$(avg "${lo_r[@]}")")
    COLS_USED+=("$(printf '%s ' "${used[@]}")")
done

# ---- report -----------------------------------------------------------------

printf '\n10-column AND scaling  nrows=%d  ndistinct=%d/col  %ds/run  %d reps\n\n' \
    "$NROWS" "$ND" "$DURATION" "$NREPS"

printf 'Index sizes:\n'
printf '  %-26s  %10s  (%s)\n'    "comp_bt(c1..c10)" "$CBT_SZ"  "baseline"
printf '  %-26s  %10s  (%d%% of comp_bt)\n' "10×_bt (10-index total)" "$SBT_SZ" "$SBT_PCT"
printf '  %-26s  %10s  (%d%% of comp_bt)\n' "1×mc_ro"  "$RO_SZ"  "$RO_PCT"
printf '  %-26s  %10s  (%d%% of comp_bt)\n' "1×mc_lo"  "$LO_SZ"  "$LO_PCT"
printf '\n'

W=10
FMT="  %-4s  %-7s  %${W}s  %${W}s  %${W}s  %${W}s  %${W}s\n"
SEP="$(printf -- '-%.0s' {1..80})"

printf "$FMT" "k" "rows/q" "comp_bt pfx" "comp_bt rnd" "10×_bt" "1×mc_ro" "1×mc_lo"
printf '  %s\n' "$SEP"

for idx in "${!COL_COUNTS[@]}"; do
    k="${COL_COUNTS[$idx]}"
    rq=$(rows_per_query "$k")
    printf "$FMT" \
        "$k" "$rq" \
        "${T_CBT_PFX[$idx]}" "${T_CBT_RND[$idx]}" \
        "${T_SBT[$idx]}"     "${T_RO[$idx]}"       "${T_LO[$idx]}"
done

printf '  %s\n\n' "$SEP"

printf 'TPS — higher is faster\n\n'

printf 'Column subsets used for random runs:\n'
for idx in "${!COL_COUNTS[@]}"; do
    k="${COL_COUNTS[$idx]}"
    printf '  k=%-2d  %s\n' "$k" "${COLS_USED[$idx]}"
done
printf '\n'

printf 'comp_bt pfx  — composite btree on leading k cols: tree descent is O(log n)\n'
printf 'comp_bt rnd  — same index on a random k-col subset: skips leading cols,\n'
printf '               degrades to full index scan when c1 is not constrained\n'
printf '10×_bt       — 10 single-col btrees; PG intersects bitmaps externally after\n'
printf '               reading O(n/ndistinct) leaf pages per column\n'
printf '1×mc_ro      — ONE 10-col roaring exact; AM intersects internally; any subset;\n'
printf '               reads one O(1)-page bitmap per queried column\n'
printf '1×mc_lo      — ONE 10-col roaring lossy; same + executor heap recheck\n'
