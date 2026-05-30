#!/usr/bin/env bash
#
# bench_100m_compare.sh — roaring vs btree head-to-head on the 100M ClickBench
# "hits" dataset, for count(*) and sum(id) (INCLUDE projection) across a fixed
# set of equality / scatter / tail / mixed predicates.
#
# See bench/README_bench_100m.md for full setup, schema and interpretation.
#
# Usage:
#   bench/bench_100m_compare.sh                 # both count(*) and sum(id)
#   METRIC=count bench/bench_100m_compare.sh    # count(*) only
#   METRIC=sum   bench/bench_100m_compare.sh    # sum(id) only
#   REPEAT=5 bench/bench_100m_compare.sh        # 1 warm-up + 5 timed runs (min reported)
#
# Env overrides:
#   CONTAINER  docker container name           (default: docker-postgres-1)
#   DBNAME     database                        (default: roaring_test)
#   DBUSER     role                            (default: postgres)
#   ROARING_TBL  roaring comparison table      (default: hits_roaring_comp)
#   BTREE_TBL    btree comparison table        (default: hits_btree_comp)
#
# Prerequisites (one-time): both tables loaded with the 100M rows, each carrying
# an "id" bigint column, indexed with the 20-col INCLUDE(id) index, and VACUUMed
# so the visibility map is all-visible (required for IndexOnlyScan on sum(id)).
set -euo pipefail

CONTAINER="${CONTAINER:-docker-postgres-1}"
DBNAME="${DBNAME:-roaring_test}"
DBUSER="${DBUSER:-postgres}"
ROARING_TBL="${ROARING_TBL:-hits_roaring_comp}"
BTREE_TBL="${BTREE_TBL:-hits_btree_comp}"
METRIC="${METRIC:-both}"
REPEAT="${REPEAT:-3}"

# label|predicate  (selectivities are for the loaded ClickBench 100M dataset)
PREDICATES=(
  "EQ-27M       |os=2"
  "SCATTER-20k  |searchengineid=2 AND sex=2 AND os=2 AND age=0"
  "SCATTER-155k |searchengineid=3 AND sex=1 AND ismobile=1"
  "TAIL-562k    |regionid=229 AND searchengineid=2 AND sex=2"
  "MIXED-20k    |regionid=229 AND advengineid=2 AND age=0"
  "MIXED-590k   |regionid=2 AND os=2 AND sex=2"
  # Btree worst case sample: predicates on deep / non-leading key columns with
  # no leading-column (regionid) constraint.  Btree can only skip-scan the whole
  # index; roaring treats each column as an independent bitmap.  (col# = the
  # column's 1-based position in the 20-col index.)
  "BWORST-c20   |urlcategoryid=9911"                  # col 20, 4.6M
  "BWORST-c14   |searchengineid=2"                    # col 14, 9.5M
  "BWORST-c12   |counterid=62"                        # col 12, 738k
  "BWORST-AND-a |searchengineid=2 AND age=0"          # col 14+11, 3.0M
  "BWORST-AND-b |advengineid=2 AND isrefresh=0"       # col 10+18, 372k
  # IN-lists on non-leading columns: roaring unions the per-value bitmaps;
  # btree skip-scans once per array element.
  "IN-c14       |searchengineid IN (2,3,4)"           # col 14, 13.8M
  "IN-c20       |urlcategoryid IN (9911,2300,5000)"   # col 20, 4.6M
  "IN-c10       |advengineid IN (2,3,13)"             # col 10, 457k
  # IN-list AND'd with equality quals on other non-leading columns.
  "INAND-a      |searchengineid IN (2,3,4) AND sex=2"          # 4.4M
  "INAND-b      |advengineid IN (2,3,13) AND os=2"             # 111k
  "INAND-c      |searchengineid IN (2,3) AND age=0 AND sex=2"  # 98k
  # Btree best case: tight contiguous leading-column prefix (regionid=#1,
  # os=#2, useragent=#3).  Btree seeks an exact sorted range; for sum(id) it
  # streams the inline id payload — its structural strength.  (For count(*)
  # roaring's cardinality read is usually still competitive/faster.)
  "BBEST-c1     |regionid=2"                               # col 1, 6.7M
  "BBEST-c1-2   |regionid=229 AND os=2"                    # cols 1-2, 4.2M
  "BBEST-c1-3   |regionid=229 AND os=2 AND useragent=3"    # cols 1-3, 1.26M
)

# Run `expr` against `tbl`/`pred` REPEAT+1 times (first = warm-up) and echo the
# minimum execution time in ms of the timed runs.
bestof() {
  local tbl="$1" expr="$2" pred="$3"
  local sql i
  sql=$'\\timing on\nSET enable_seqscan=off;\nSET max_parallel_workers_per_gather=0;\n'
  for ((i=0; i<=REPEAT; i++)); do
    sql+="SELECT ${expr} FROM ${tbl} WHERE ${pred};"$'\n'
  done
  docker exec -i "$CONTAINER" psql -U "$DBUSER" -d "$DBNAME" 2>/dev/null <<<"$sql" \
    | grep '^Time:' | awk '{print $2}' | tail -n "$REPEAT" | sort -n | head -1
}

run_metric() {
  local expr="$1" title="$2" p lbl pred rr bb
  echo
  echo "### ${title}  (min of ${REPEAT} warm runs, ms)"
  printf "%-13s %14s %14s   %s\n" "predicate" "roaring" "btree" "winner"
  printf -- "------------- -------------- --------------   ------\n"
  for p in "${PREDICATES[@]}"; do
    lbl="${p%%|*}"; lbl="${lbl//[[:space:]]/}"
    pred="${p#*|}"; pred="$(echo "$pred" | sed 's/^[[:space:]]*//')"
    rr=$(bestof "$ROARING_TBL" "$expr" "$pred")
    bb=$(bestof "$BTREE_TBL"   "$expr" "$pred")
    win=$(awk -v r="$rr" -v b="$bb" 'BEGIN{
      if (r=="" || b=="") {print "n/a"; exit}
      if (r<b) printf "roaring %.2fx", b/r; else printf "btree %.2fx", r/b}')
    printf "%-13s %12s ms %12s ms   %s\n" "$lbl" "$rr" "$bb" "$win"
  done
}

echo "pg_roaring_index — 100M roaring vs btree   ($(date))"
echo "container=$CONTAINER db=$DBNAME  roaring=$ROARING_TBL btree=$BTREE_TBL"

case "$METRIC" in
  count) run_metric "count(*)" "count(*)" ;;
  sum)   run_metric "sum(id)"  "sum(id)  [IndexOnlyScan, INCLUDE projection]" ;;
  both)
    run_metric "count(*)" "count(*)"
    run_metric "sum(id)"  "sum(id)  [IndexOnlyScan, INCLUDE projection]"
    ;;
  *) echo "unknown METRIC=$METRIC (use count|sum|both)" >&2; exit 1 ;;
esac
