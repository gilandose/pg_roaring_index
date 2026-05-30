# 100M roaring-vs-btree benchmark suite

Head-to-head of the `roaring` index AM against `btree` on the 100M-row
ClickBench `hits` dataset, for **`count(*)`** and **`sum(id)`** (INCLUDE-column
projection) across a fixed predicate set spanning equality / scatter / tail /
mixed selectivities.

Driver: [`bench_100m_compare.sh`](./bench_100m_compare.sh).

---

## 1. What it measures

Two query shapes, each `roaring` vs `btree`, both forced onto their index
(`enable_seqscan=off`) and single-threaded (`max_parallel_workers_per_gather=0`)
for an apples-to-apples access-method comparison:

| Shape | Roaring path | Btree path |
|---|---|---|
| `count(*) WHERE …` | `Custom Scan (RoaringCount)` — intersect + count bitmaps, no row access | `Aggregate → Index Only Scan` — count matching index entries |
| `sum(id) WHERE …` | `Index Only Scan` + streaming payload cursor (INCLUDE `id`) | `Index Only Scan` — `id` inline in the index tuple |

`id` is a `bigint` **INCLUDE** column (not a key column — see *Known limitations*).

## 2. One-time setup

Requires a running container (see `docker/docker-compose.yml`; default service
`docker-postgres-1`, DB `roaring_test`, port 5433) with two comparison tables,
each a full copy of the 100M `hits` table **carrying an `id bigint` column**:

- `hits_roaring_comp` — indexed with the 20-col `roaring` index
- `hits_btree_comp`   — indexed with the equivalent 20-col `btree` index

Both indexes use the same 20 int4 key columns and `INCLUDE (id)`:

```sql
-- 20 equality-friendly int4 columns spanning low→high cardinality
CREATE INDEX idx_roaring20_inc ON hits_roaring_comp USING roaring
  (regionid, os, useragent, ismobile, sex, clienttimezone, cookieenable,
   javascriptenable, robotness, advengineid, age, counterid, goodevent,
   searchengineid, traficsourceid, isoldcounter, dontcounthits, isrefresh,
   socialsourcenetworkid, urlcategoryid) INCLUDE (id);

CREATE INDEX idx_btree20_inc ON hits_btree_comp USING btree
  (regionid, os, useragent, ismobile, sex, clienttimezone, cookieenable,
   javascriptenable, robotness, advengineid, age, counterid, goodevent,
   searchengineid, traficsourceid, isoldcounter, dontcounthits, isrefresh,
   socialsourcenetworkid, urlcategoryid) INCLUDE (id);
```

Build notes:
- Set `SET maintenance_work_mem = '8GB'` (or higher) before `CREATE INDEX` — the
  roaring build emits `nkeys × nrows` (= 20 × 100M = 2B) sort tuples and spills
  heavily below that.
- The roaring build is **serial** (~45 min at this scale); btree builds in
  parallel (~2 min). Sizes: roaring ≈ 6.3 GB, btree ≈ 11 GB.

**Critical — VACUUM before measuring `sum(id)`:**

```sql
VACUUM hits_roaring_comp;   -- and hits_btree_comp
```

`sum(id)` can only run as an `IndexOnlyScan` when the table's visibility map is
all-visible. Without VACUUM the planner falls back to a `Bitmap Heap Scan`
(heap recheck) and `sum(id)` is ~5× slower. Verify:

```sql
SELECT relname, round(100.0*relallvisible/NULLIF(relpages,0),1) AS pct_allvisible
FROM pg_class WHERE relname IN ('hits_roaring_comp','hits_btree_comp');  -- want 100.0
```

## 3. Predicate set

Fixed in the driver; selectivities are for the loaded 100M dataset:

| Label | Predicate | Rows | Shape |
|---|---|---:|---|
| `EQ-27M`       | `os=2` | 26,990,818 | single-col equality (non-leading for btree) |
| `SCATTER-20k`  | `searchengineid=2 AND sex=2 AND os=2 AND age=0` | 20,824 | 4 non-prefix cols |
| `SCATTER-155k` | `searchengineid=3 AND sex=1 AND ismobile=1` | 154,680 | 3 non-prefix cols |
| `TAIL-562k`    | `regionid=229 AND searchengineid=2 AND sex=2` | 562,401 | regionid prefix + extra |
| `MIXED-20k`    | `regionid=229 AND advengineid=2 AND age=0` | 19,856 | prefix + non-prefix, selective |
| `MIXED-590k`   | `regionid=2 AND os=2 AND sex=2` | 590,136 | contiguous 2-col prefix + extra |

**Btree worst-case sample** — deep / non-leading columns, no `regionid` constraint
(`col#` = position in the 20-col index):

| Label | Predicate | Rows | Note |
|---|---|---:|---|
| `BWORST-c20`   | `urlcategoryid=9911` | 4,612,950 | deepest column (#20) |
| `BWORST-c14`   | `searchengineid=2` | 9,458,459 | column #14 |
| `BWORST-c12`   | `counterid=62` | 738,172 | column #12 |
| `BWORST-AND-a` | `searchengineid=2 AND age=0` | 3,036,851 | cols #14 + #11 |
| `BWORST-AND-b` | `advengineid=2 AND isrefresh=0` | 372,034 | cols #10 + #18 |
| `IN-c14`       | `searchengineid IN (2,3,4)` | 13,801,767 | IN-list, col #14 |
| `IN-c20`       | `urlcategoryid IN (9911,2300,5000)` | 4,612,950 | IN-list, col #20 |
| `IN-c10`       | `advengineid IN (2,3,13)` | 457,129 | IN-list, col #10 |
| `INAND-a`      | `searchengineid IN (2,3,4) AND sex=2` | 4,366,459 | IN-list AND equality |
| `INAND-b`      | `advengineid IN (2,3,13) AND os=2` | 111,149 | IN-list AND equality |
| `INAND-c`      | `searchengineid IN (2,3) AND age=0 AND sex=2` | 98,481 | IN-list AND 2 equalities |

**Btree best-case sample** — tight contiguous *leading* prefix (`regionid`=#1,
`os`=#2, `useragent`=#3). Btree seeks an exact sorted range:

| Label | Predicate | Rows | Note |
|---|---|---:|---|
| `BBEST-c1`   | `regionid=2` | 6,687,587 | leading col only |
| `BBEST-c1-2` | `regionid=229 AND os=2` | 4,205,638 | 2-col prefix |
| `BBEST-c1-3` | `regionid=229 AND os=2 AND useragent=3` | 1,255,842 | 3-col prefix |

(`regionid` is the btree leading column, so predicates that constrain it let
btree seek a contiguous range; predicates that don't force btree to skip-scan or
full-scan the index.)

## 4. Running

```sh
bench/bench_100m_compare.sh                 # count(*) and sum(id)
METRIC=count bench/bench_100m_compare.sh    # count(*) only
METRIC=sum   bench/bench_100m_compare.sh    # sum(id) only
REPEAT=5 bench/bench_100m_compare.sh        # 1 warm-up + 5 timed runs (min reported)
```

Env overrides: `CONTAINER`, `DBNAME`, `DBUSER`, `ROARING_TBL`, `BTREE_TBL`.
Each cell is the **minimum of `REPEAT` warm runs** (one warm-up discarded).

## 5. Reference results (100M, single-threaded, warm)

`count(*)` — roaring's bitmap intersect+count is roughly flat in result size:

| Predicate | Roaring | Btree | Winner |
|---|---:|---:|---|
| EQ-27M | 56 ms | 4229 ms | **roaring 75×** |
| SCATTER-20k | 323 ms | 435 ms | roaring 1.3× |
| SCATTER-155k | 203 ms | 658 ms | roaring 3.2× |
| TAIL-562k | 219 ms | 437 ms | roaring 2.0× |
| MIXED-20k | 119 ms | 133 ms | roaring 1.1× |
| MIXED-590k | 195 ms | 63 ms | btree 3.1× |
| BWORST-c20 | 3.6 ms | 7056 ms | **roaring ~1900×** |
| BWORST-c14 | 43 ms | 7492 ms | roaring 173× |
| BWORST-c12 | 0.9 ms | 3318 ms | **roaring ~3700×** |
| BWORST-AND-a | 160 ms | 4047 ms | roaring 25× |
| BWORST-AND-b | 117 ms | 2492 ms | roaring 21× |
| IN-c14 | 120 ms | 9638 ms | roaring 80× |
| IN-c20 | 3.7 ms | 9893 ms | **roaring ~2700×** |
| IN-c10 | 13 ms | 2907 ms | roaring 222× |
| INAND-a | 266 ms | 3769 ms | roaring 14× |
| INAND-b | 71 ms | 892 ms | roaring 13× |
| INAND-c | 331 ms | 1364 ms | roaring 4× |
| BBEST-c1 | 37 ms | 983 ms | roaring 27× |
| BBEST-c1-2 | 150 ms | 649 ms | roaring 4.3× |
| BBEST-c1-3 | 218 ms | 193 ms | btree 1.1× |

`sum(id)` (IndexOnlyScan, INCLUDE projection):

| Predicate | Roaring | Btree | Winner |
|---|---:|---:|---|
| EQ-27M | 11186 ms | 7526 ms | btree 1.5× |
| SCATTER-20k | 364 ms | 442 ms | roaring 1.2× |
| SCATTER-155k | 616 ms | 664 ms | ~tie |
| TAIL-562k | 1277 ms | 432 ms | btree 3.0× |
| MIXED-20k | 76 ms | 137 ms | roaring 1.8× |
| MIXED-590k | 800 ms | 65 ms | btree 12× |
| BWORST-c20 | 1074 ms | 7257 ms | **roaring 6.8×** |
| BBEST-c1-2 | 2772 ms | 642 ms | btree 4.3× |
| BBEST-c1-3 | 1331 ms | 192 ms | btree 6.9× |

## 6. Interpretation

- **`count(*)` is roaring's strong suit.** RoaringCount counts via bitmap
  intersection without touching rows, so it's near-flat in result size and wins
  almost everywhere; it loses only when btree can count a tight contiguous
  prefix range (`MIXED-590k`).
- **Column order is btree's Achilles heel, and roaring's indifference is the
  whole point.** The `BWORST-*` / `IN-*` sample queries deep / non-leading
  columns with no `regionid` constraint: btree can only skip-scan the whole
  index (~2.5–10s across the sample), while roaring treats each column as an
  independent bitmap (count(*) **21×–3700×** faster — consistently, not as a
  one-off). IN-lists behave the same: roaring unions per-value bitmaps; btree
  skip-scans once per array element. Btree's advantage on `MIXED-590k` and its
  collapse across the worst-case sample are the same coin: btree lives or dies
  by whether the predicate matches its key order; roaring does not care.
- **`sum(id)` is mixed.** Roaring wins on highly selective results (it fetches
  few payloads); btree wins as the result grows and it has a usable `regionid`
  prefix, because it streams the inline `id` from sorted index tuples. Roaring's
  remaining cost at scale is per-row work (iterator advance + `index_form_tuple`
  per tuple), not buffer traffic — the streaming payload cursor already cut the
  per-tuple payload re-walk (e.g. EQ-27M: 107M → 0.6M buffer hits, 21.9s → 11.2s).
- **Btree's best case is specifically `sum(id)` on a leading prefix.** Given a
  tight contiguous prefix (`BBEST-*`), btree seeks an exact sorted range and
  streams the inline `id` — 4–7× faster than roaring there. But note the metric
  asymmetry: for `count(*)` even those leading-prefix predicates mostly favour
  roaring (e.g. `regionid=2`: roaring 27×), because roaring reads a stored
  cardinality instead of scanning the range; btree only reaches parity once the
  prefix is deep enough to make the range tiny (`BBEST-c1-3` ≈ tie).

## 7. Container management

The `.so` is compiled into the image; rebuild + restart to pick up source changes:

```sh
cd docker && docker compose build && docker compose up -d
```

(Inside the Linux container there is no macOS stale-dylib trap — a fresh
container process always loads the freshly built `.so`.)

## 8. Known limitations / gotchas

- **bigint key columns are unsupported on this branch.** The multi-column key
  packs each value into 32 bits (`ROARING_COL_KEY`), so a `bigint` *key* column
  raises `roaring index: unsupported column type bigint for multi-column key`
  (`roaring_util.c`). bigint is fine as an **INCLUDE** column (as `id` is here).
  This is why several `expected/` regression tests are currently red.
- **Unqualified roaring scans return 0 rows** — the roaring index can't serve a
  no-`WHERE` full scan (e.g. `GROUP BY col` with `enable_seqscan=off`), so don't
  use it to probe column distributions; query the heap (seqscan) for that.
- **Single-threaded by design here.** The driver sets
  `max_parallel_workers_per_gather=0`. Roaring's `ambuild` and scans are serial;
  btree build/scan can parallelize, so allowing parallelism changes the picture.
- **PG18 btree skip-scan** keeps btree competitive even on non-prefix predicates;
  the comparison reflects PG ≥ 18 behaviour.
