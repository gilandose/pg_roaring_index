# pg_roaring_index

A native PostgreSQL [Index Access Method](https://www.postgresql.org/docs/current/indexam.html) that stores [roaring bitmaps](https://roaringbitmap.org/) per distinct value. Designed for equality lookups on moderate-cardinality columns in tables with high DELETE+INSERT churn and uncorrelated physical layout.

---

## The problem it solves

Consider a table with 20 columns — `status`, `region`, `tier`, `category`, etc. — each with 10–10 000 distinct values. Your queries filter on arbitrary subsets:

```sql
WHERE region = 'eu-west' AND tier = 'pro' AND category = 'analytics'
WHERE status = 'active' AND region = 'us-east'
WHERE tier = 'free' AND category = 'storage' AND status = 'trial'
```

You can't pre-build a composite btree for every combination. With single-column btrees, PostgreSQL's `BitmapAnd` node handles the intersection — but each btree scan must walk O(rows / ndistinct) leaf pages to collect TIDs before the intersection even starts. At ndistinct=100 with 1M rows, that's ~20 leaf pages per column per query.

A roaring index stores the entire TID set for each value as a serialised bitmap. Fetching it is one page read regardless of how many rows match. `BitmapAnd` of three roaring indexes costs three page reads total.

---

## Quick start

```sql
CREATE EXTENSION pg_roaring_index;

-- Exact mode: TID-level precision
CREATE INDEX ON events USING roaring (status);
CREATE INDEX ON events USING roaring (region);
CREATE INDEX ON events USING roaring (tier);

-- Any combination now resolves via BitmapAnd
SELECT * FROM events WHERE status = 'active' AND region = 'eu-west' AND tier = 'pro';
```

No configuration needed. Works with `IN()`, `= ANY()`, and multi-column `BitmapAnd` automatically.

---

## Two modes

| Mode | `USING` | Granularity | Recheck | Best for |
|---|---|---|---|---|
| Exact | `roaring` | TID | No | Medium cardinality, write churn, multi-column AND |
| Lossy | `roaring_lossy` | Page | Yes | Very low cardinality, smallest possible footprint |

Lossy mode stores block numbers instead of individual TIDs. Each matched page is rechecked by the executor — correct, but trades CPU for dramatically smaller indexes (1–12% of btree size at ndistinct ≤ 100).

```sql
-- Lossy: smallest indexes, executor rechecks each matched page
CREATE INDEX ON events USING roaring_lossy (status);
```

Both modes support `int8` and `int4` columns and cross-type `int8 = int4` comparisons.

---

## Benchmarks

All results: PostgreSQL 18, 1M rows, macOS/aarch64, 20s pgbench runs.

### Multi-column AND  (`WHERE c1 = X AND c2 = Y AND c3 = Z`, ndistinct = 100)

This is the primary use case. `comp_bt` is a composite btree on `(c1, c2, c3)` — the best-case pre-planned index that requires knowing the query shape in advance. The other three scenarios require no advance planning and cover any column combination.

```
scenario              index size (3×)   vs 3×btree      TPS
────────────────────────────────────────────────────────────
comp_bt(c1,c2,c3)            30 MB      — (baseline)   14801   ← oracle: knows query shape
3×btree                      20 MB            100%      1231   ← flexible, slow
3×roaring                  6296 kB             30%      1397   ← flexible, 14% faster, 70% smaller
3×roaring_lossy             976 kB              4%      6920   ← flexible, 5.6× faster, 96% smaller
```

`roaring_lossy` reaches **47% of the composite btree's throughput** using **independent single-column indexes** and **4% of the btree index space**. On a 20-column table: 20× btree (single-column) ≈ 134 MB; 20× roaring_lossy ≈ 6 MB.

### Single-column equality sweep (`WHERE val = X` and `WHERE val IN (…)`)

Btree wins at high cardinality on single-column equality — that is not the target workload. The table shows where roaring is competitive and where it is not.

```
ndistinct  rows/val │ bt_size  ro_size  lo_size  ro/bt  lo/bt │ bt_eq   ro_eq  lo_eq │ bt_in   ro_in  lo_in
──────────────────────────────────────────────────────────────────────────────────────────────────────────────
       10   100 000 │ 6800 kB  2040 kB   120 kB    30%     1% │   321      85     20 │    76      53     29
      100    10 000 │ 6816 kB  2448 kB   848 kB    35%    12% │  2670     195     23 │   331      85     16
    1 000     1 000 │ 7168 kB  2704 kB  2032 kB    37%    28% │ 12662    1462    174 │  2564     656    114
   10 000       100 │ 6712 kB  8048 kB  2392 kB   119%    35% │ 22327    7981   1467 │ 10339    3132    970
  100 000        10 │ 9256 kB    13 MB  5976 kB   142%    64% │ 21168   18582   8638 │ 13692   10187   5781
```

`_eq` = equality TPS (`WHERE val = X`). `_in` = 10-value IN-list TPS.

**Sweet spot**: ndistinct 10–1 000. Index is 28–37% of btree size; lossy stays under 28%. Exact mode TPS is lower than btree for single-column equality — the gain comes in multi-column AND, where one roaring page read replaces O(rows/ndistinct) btree leaf reads per column.

**Not the right tool**: ndistinct > 10 000. Roaring index grows larger than btree (bitmaps don't compress well at low TIDs/value); btree wins on both size and TPS.

---

## When to use roaring

**Use it when:**
- Table has multiple low-to-moderate cardinality columns (ndistinct 10–10 000)
- Queries filter on arbitrary subsets of those columns — you can't predict which composite index to build
- High DELETE+INSERT churn (roaring appends to a pending list; no page splits under write load)
- Index footprint matters — 20 roaring_lossy indexes fit in RAM where 20 btrees don't

**Stick with btree when:**
- Single-column equality on a high-cardinality column (ndistinct > 10 000) — btree wins
- Range queries (`>`, `<`, `BETWEEN`) — roaring only supports equality
- You know the exact query shape — a composite btree on those columns is 10× faster
- Columns are updated in-place (HOT updates skip btree maintenance; roaring must insert new pending entries)

---

## Installation

**Prerequisites**: PostgreSQL 14–18, C compiler, `pg_config` on PATH.

```sh
# 1. Fetch CRoaring amalgamation (one-time; files are gitignored)
bash scripts/fetch-croaring.sh

# 2. Build and install
make
make install

# 3. Enable in your database
psql -c "CREATE EXTENSION pg_roaring_index;"
```

**macOS (Homebrew)**:
```sh
brew install postgresql@18
export PATH="/opt/homebrew/opt/postgresql@18/bin:$PATH"
bash scripts/fetch-croaring.sh && make && make install
```

**Regression tests**:
```sh
make installcheck   # requires a running cluster
```

---

## Pending list and VACUUM

Inserts go to an append-only pending list (GIN-style). VACUUM merges the pending list into the main index. Queries scan both the main index and the pending list so results are always correct between vacuums.

Back-pressure triggers an inline merge when the pending list exceeds a threshold (default 10 000 entries), preventing unbounded growth under sustained insert load.

---

## Known limitations

- **Equality only** — no range scans, no ordering, no partial indexes
- **int8 and int4 only** — other types require a custom opclass
- **Lossy dead-block accumulation** — deleted rows leave stale block numbers in lossy indexes until the rows are re-inserted. Extra heap rechecks result; no wrong answers.
- **No composite key packing** — `(company_id, location_id)` as a single int64 is on the roadmap but not yet implemented
- **ndistinct > 10 000** — roaring indexes grow larger than btree; use btree instead

---

## How it works

Each distinct column value maps to one leaf entry in a sorted B-tree-like directory. The entry stores a serialised [CRoaring](https://github.com/RoaringBitmap/CRoaring) bitmap of all linearized TIDs (`(blkno << 9) | (offset - 1)`) matching that value. Bitmaps fit in a single 8 KB page for the target cardinality range; larger bitmaps chain to overflow pages.

At query time, `amgetbitmap` fetches the bitmap for each scan key (one directory lookup + one page read), deserialises it, and adds TIDs to PostgreSQL's `TIDBitmap`. For multi-column `BitmapAnd`, each index contributes its bitmap independently; PostgreSQL intersects them in the executor.

Lossy mode (`roaring_lossy`) stores block numbers instead of TIDs. The bitmap is ~512× smaller per entry; `amrecheck = true` tells the executor to recheck predicates on each matched page.
