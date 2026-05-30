# pg_roaring_index

A PostgreSQL [index access method](https://www.postgresql.org/docs/current/indexam.html)
that stores a **[Roaring bitmap](https://roaringbitmap.org/)** of row locations
for each distinct value of an indexed column. It is built for **equality filters
on low‑to‑moderate cardinality columns** — the categorical, foreign‑key,
status‑flag, tenant‑id, and device/region/OS style columns that B‑trees index
poorly and that dominate analytical filtering on transactional tables.

Its headline capability is a **custom `count(*)` path** that answers
`count(*) … WHERE a = x AND b = y` by intersecting bitmaps and reading their
cardinalities — without scanning matching rows or touching the heap.

```sql
CREATE EXTENSION pg_roaring_index;

CREATE INDEX ON events USING roaring (tenant_id, event_type, country, device);

-- Counted by intersecting bitmaps: no heap access, order-independent.
SELECT count(*) FROM events
 WHERE country = 'GB' AND device = 'mobile';
```

> Status: research-quality / pre-1.0. Exact (TID-level) mode, `roaring64`
> bitmaps, fully WAL-logged via `generic_xlog`. See [Limitations](#limitations).

---

## The problem it solves

Consider a wide table — `status`, `region`, `tier`, `category`, `device`, … —
each column with tens to thousands of distinct values, queried on arbitrary
subsets:

```sql
WHERE region = 'eu-west' AND tier = 'pro' AND category = 'analytics'
WHERE status = 'active' AND region = 'us-east'
WHERE tier   = 'free'   AND category = 'storage'
```

You can't pre-build a composite B-tree for every combination, and a multi-column
B-tree only seeks efficiently when its **leading** columns are constrained. With
single-column B-trees, each scan still walks `O(rows / ndistinct)` leaf pages to
collect TIDs before the intersection starts.

A roaring index stores the entire TID set for a value as one serialized bitmap.
Fetching it is one directory lookup plus a page read, regardless of how many rows
match — and **any subset of the indexed columns can be constrained**, in any
order. For `count(*)`, it never materializes the rows at all.

---

## When to use it

Reach for `pg_roaring_index` when **most** of these hold:

- **Equality / `IN` predicates** (`=`, `IN (...)`), not ranges.
- **Low-to-moderate cardinality** columns (a handful to a few million distinct
  values): country, status, category, tenant, device, OS, ad/engine id, flags.
- **`count(*)` and aggregate-heavy** workloads — where it shines (often 10×–1000×
  faster than B-tree; see [Benchmarks](#benchmarks)).
- **Multi-column AND filters where the column order varies.** Roaring is
  **order-independent**; a B-tree lives or dies by whether the query matches its
  key order.
- **High write churn.** Inserts land in an MVCC-aware, sharded pending list and
  merge into the bitmaps in batches (GIN-style) — no per-row bitmap rewrites, no
  page splits under load.
- You want compact indexes: a 20-column index over 100M rows was **6.3 GB vs the
  equivalent B-tree's 11 GB**.

## When *not* to use it

Prefer a B-tree (or BRIN / GIN) when **any** of these apply:

- **Range, ordering, or pattern queries**: `<`, `>`, `BETWEEN`, `ORDER BY`,
  `LIKE 'foo%'`, merge joins. Roaring answers equality only and returns no order.
- **High-cardinality / unique keys**: primary keys, emails, UUID surrogate keys —
  a B-tree is smaller and faster.
- **Single-row OLTP point lookups that fetch the row.** Roaring is about *sets*.
- **64-bit / text / uuid columns as *high-cardinality* keys.** These are
  hash-encoded in multi-column keys and rely on executor recheck — fine at low
  cardinality, wasteful at high. (`int8` as a *single-column* key is lossless.)

## Supported key types

Default operator classes (equality `=`) cover:

`int2` · `int4` · `int8` · `bool` · `date` · `float4` · `oid` · `enum` · `text` · `uuid`

`text`, `uuid`, and multi-column `int8` are **hash-encoded** with executor
recheck for exactness. An `INCLUDE` column may carry an `int8` payload for
index-only projection (e.g. `SELECT sum(id) … WHERE …`).

---

## Quick start

**Prerequisites** (macOS / Homebrew shown; Linux is analogous):

```sh
brew install postgresql@18
export PATH="/opt/homebrew/opt/postgresql@18/bin:$PATH"

# Vendored CRoaring amalgamation (one-time; gitignored)
bash scripts/fetch-croaring.sh
```

**Build, install, test:**

```sh
make
make install            # into $(pg_config --pkglibdir)
make installcheck       # 8/8 regression tests (needs a running cluster)
```

Or run it containerized:

```sh
cd docker && docker compose up -d   # PostgreSQL 18 with the extension installed
```

**Use it:**

```sql
CREATE EXTENSION pg_roaring_index;

-- single column
CREATE INDEX ON hits USING roaring (os);

-- multi-column + INCLUDE payload (index-only sum projection)
CREATE INDEX ON hits USING roaring (region, os, device) INCLUDE (id);

SELECT count(*) FROM hits WHERE os = 2;                     -- RoaringCount path
SELECT count(*) FROM hits WHERE region = 5 AND device = 1;  -- bitmap AND
SELECT sum(id)  FROM hits WHERE os = 2;                     -- index-only
```

---

## Benchmarks

100M-row [ClickBench `hits`](https://github.com/ClickHouse/ClickBench) dataset, a
20-column roaring index vs an equivalent 20-column B-tree, single-threaded, warm
cache. Methodology and a re-runnable harness:
[`bench/README_bench_100m.md`](bench/README_bench_100m.md).

**`count(*)` — roaring's strength.** It intersects bitmaps and reads
cardinalities, so it is nearly flat in result size and independent of column
order:

| Predicate | Rows | Roaring | B-tree | Speedup |
|---|---:|---:|---:|---:|
| `os = 2` (non-leading column) | 27.0M | **56 ms** | 4,229 ms | **~75×** |
| `searchengineid=3 AND sex=1 AND ismobile=1` | 154k | **203 ms** | 658 ms | 3.2× |
| `urlcategoryid = 9911` (deepest column) | 4.6M | **3.5 ms** | 7,237 ms | **~2000×** |
| `searchengineid IN (2,3,4)` | 13.8M | **120 ms** | 9,638 ms | 80× |
| `regionid=2 AND os=2 AND sex=2` (leading prefix) | 590k | 195 ms | **63 ms** | B-tree 3× |

The pattern: **B-tree's speed swings ~100× with whether the predicate matches its
key order; roaring is order-indifferent.** B-tree still wins when it can seek a
tight contiguous *leading* prefix.

For `sum(id)` (index-only `INCLUDE` projection) roaring wins on highly selective
results and loses to B-tree on large result sets with a usable leading prefix
(B-tree streams the inline payload from a contiguous range). Full table in the
bench README.

---

## How it works (short version)

Each distinct value maps to a Roaring bitmap of **linearized TIDs**
(`(block << 9) | (offset - 1)`). A 1–3 level sorted **directory** locates the
leaf page for a value; the leaf stores the bitmap inline, or chains to
**overflow** pages for large bitmaps. New rows append to an MVCC-aware, 8-way
**sharded pending list** and are merged into the bitmaps in batches. Multi-column
indexes namespace each column's values into one key space
(`(attno << 32) | key32`) and intersect per-column bitmaps at scan time. An
optional `INCLUDE` column lives in a separate dense **payload** store for
index-only projection. Every page write goes through `generic_xlog` (no custom
WAL resource manager).

See **[DESIGN.md](DESIGN.md)** for the on-disk layout, diagrams, and the
read / write / merge protocols.

---

## Limitations

- **Equality only** — no range, ordering, or pattern support. (`IS NULL` and
  `IS NOT NULL` *are* supported — each column keeps a NULL bitmap, so
  `WHERE a = 5 AND b IS NULL` composes as a bitmap intersection.)
- **Hash-encoded keys** (`text`, `uuid`, multi-column `int8`) rely on executor
  recheck for exactness — extra heap work at high cardinality.
- **Lossless multi-column `int8`** and a couple of correctness edges (e.g.
  projecting a bare *unconstrained* key column of a multi-column index,
  `SELECT a … WHERE b = 5`, and `IS NULL` under a forced `enable_seqscan=off`)
  are pending a per-column covering store. See [`TODO.md`](TODO.md).
- **Serial index build.** `ambuild` is single-threaded (parallel build is on the
  roadmap); a wide 20-column / 100M-row build takes tens of minutes.

## Project layout

```
src/            access method: build, insert, scan, vacuum, cost, customscan, payload, util
src/vendor/     vendored CRoaring amalgamation (fetched, gitignored)
sql/            CREATE EXTENSION SQL + regression test inputs
expected/       regression test expected output
bench/          100M roaring-vs-btree benchmark suite (+ README)
docker/         containerized PostgreSQL 18 build
DESIGN.md       on-disk layout and protocols (with diagrams)
TODO.md         roadmap and known limitations
```

## License

[PostgreSQL License](LICENSE) — a liberal OSI-approved license, the same one used
by PostgreSQL itself.
