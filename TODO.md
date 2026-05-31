# pg_roaring_index — TODO

Working notes / backlog. Most recent focus: 100M-row roaring-vs-btree
benchmarking on the `multi-column-count-optimization` branch, which surfaced
several bugs (fixed) and the remaining multi-column work (below).

## Done (committed)

- **`fix(build)`** (`6a5f711`) — wide multi-column builds:
  - directory was sized by emitted-tuple count (`nkeys × nrows`) → grow
    `leaf_entries` on demand to the real leaf-page count.
  - `max_inline` was 4 bytes too large (line pointer subtracted before
    `MAXALIGN`) → boundary-sized bitmaps failed `PageAddItem`; fixed + added a
    defensive overflow fallback.
- **`fix(scan)`** (`c146e4a`) — `needs_recheck` computed in `ambeginscan` read
  uninitialised `sk_attno` → out-of-bounds `rd_opcintype[]` read → intermittent
  SIGSEGV. Moved to `amrescan` where keys are populated.

## Done (working tree, to commit with the multi-column completion)

- **Streaming payload cursor** (`roaring_payload.c`, `roaring_scan.c`,
  `pg_roaring_index.h`) — IndexOnlyScan INCLUDE projection re-read the metapage +
  both dir levels + re-walked the payload chain per tuple. The cursor exploits
  ascending-TID delivery to read each page once per scan.
  - Validated correct (matches btree, incl. multi-page + nested-loop rescans).
  - EQ-27M `sum(id)`: 107M → 0.6M buffer hits, 21.9s → 11.2s.
  - Coupled to files mid-refactor; commit alongside the multi-column work.

## Benchmark suite (added)

- `bench/bench_100m_compare.sh` + `bench/README_bench_100m.md` — roaring vs btree
  on 100M ClickBench `hits`, `count(*)` and `sum(id)` across eq/scatter/tail/
  mixed/IN/IN+AND and btree best/worst predicates. See the README for setup,
  the mandatory `VACUUM` for `sum(id)` IndexOnlyScan, and reference results.

## Done — int8 key support (patch; full redesign deferred)

bigint now works as a key in both single- and multi-column indexes:
- **single-column**: lossless via `roaring_datum_to_key64` (count path now uses
  key64 for single-column instead of the multi-column packed key).
- **multi-column**: hashed into the 32-bit slot (`roaring_datum_to_key32` int8
  case) with executor **recheck** (flagged in `roaring_set_needs_recheck`); the
  exact-count fast path declines int8 multi-column quals (`extract_const_keys`),
  falling back to a recheck bitmap scan. Verified exact vs seqscan, incl.
  values > int32 and hash collisions.

Brought `make installcheck` from 6/8 failing to 4/8 (`roaring_types`,
`roaring_check` now pass).

### Deferred: lossless multi-column bigint (the real redesign)

Hashing means high-cardinality bigint multi-column keys pay recheck cost and
lose the exact count(*) optimization. A first-class fix is **per-column
directories** (each key column gets its own directory root in the metapage, so
every column uses the full 64-bit `key64`). Touches build, scan, count, the
pending-list entry layout (needs a column tag), metapage and vacuum — a sizable
change. Tracked for after pgconf.eu.

## Open — finish multi-column multi-type coverage

- Ensure every supported type works *as a multi-column key* (validated: int4,
  int8-via-hash; verify int2/oid/date/bool/float4/text/uuid/enum end-to-end as
  keys, incl. per-column NULL handling and recheck for the hashed types).

## Done — regression suite green (8/8)

- Fixed the `IS NULL` / key-column IndexOnlyScan bug (see below) → unblocked
  `roaring_basic`, `roaring_multicolumn`, `roaring_include`.
- Regenerated `expected/roaring_customscan.out` and updated the stale test
  comment: multi-column `count(*)` now fires `RoaringCount` (intersection),
  which is the branch's feature — the golden predated it.

## Done — IS NULL / IS NOT NULL via per-column NULL bitmaps (feat/null-bitmap)

NULL rows are now indexed under `ROARING_NULL_COL_KEY(attno)` (a key in the
negative int64 region, disjoint from value keys). `amsearchnulls = true`, and the
scan answers `col IS NULL` from that bitmap (and `IS NOT NULL` as the full index
TID set `andnot` it). IS NULL is a real **index condition** that composes with
equality (`a = x AND b IS NULL` → bitmap AND), works for build-time and pending
(post-build) NULLs, and is correct even under `enable_seqscan = off`. This
**resolves** the earlier IS NULL IndexOnlyScan correctness edge at the source —
the index returns the real NULL rows instead of a synthesized `val = NULL`.
Covered by `expected/roaring_null.out` (9/9). The 1e30 cost gate stays only for
genuinely keyless scans (no WHERE / inequality-only).

Follow-ups for this feature:
- **DONE — `count(*) WHERE col IS NULL` RoaringCount fast path.**
  `extract_const_keys` now recognizes `NullTest(col IS NULL)` and emits
  `ROARING_NULL_COL_KEY(colno+1)` — the same key the write path records — so the
  existing count machinery reads the NULL bitmap's cardinality: single qual via
  the O(1) leaf-card path (or pending fold when unmerged), and composed with
  equality via bitmap AND (`a = x AND b IS NULL`). Valid for *every* column type
  (NULL-ness is exact regardless of value encoding), including hash-keyed
  text/uuid/multi-col int8. `IS NOT NULL` is the complement (not a positive key
  lookup) and is declined — it falls back to a normal scan. Covered in
  `expected/roaring_customscan.out` §6.
- Single-column `int8`: the NULL sentinel has a one-in-2^64 collision with the
  exact bigint value `ROARING_NULL_COL_KEY(1)` — documented; the covering store
  removes it.

### DONE — bare unconstrained key projection via planner steering

**Bare unconstrained key projection** (`SELECT a FROM mc WHERE b = 5`, and
columns constrained only by `IN` / `IS NOT NULL`) used to return NULL for `a`:
the planner chose an IndexOnlyScan (because `amcanreturn` says key columns are
returnable, which keeps `sum(id) WHERE key=x` index-only), but roaring cannot
reconstruct an *unconstrained* key column, so the IOS handed back NULL.

The key realization: roaring's own **bitmap-heap scan already projects any
column correctly from the heap** — `SELECT a WHERE b=5` is correct the moment
the planner picks the bitmap path instead of the IOS. So this is purely a plan
selection problem, fixed with zero storage and no value reconstruction:

- A `set_rel_pathlist_hook` (`roaring_suppress_unreturnable_ios`) inspects each
  roaring IndexOnlyScan path. Using the target list + filter quals (which the
  planner *can* see, unlike `amgettuple`), it detects when a *needed* key column
  has no equality clause and **penalizes that path's `disabled_nodes`**, so the
  planner prefers any heap-projecting plan. Columns appearing only in an index
  condition (e.g. `count(*) WHERE v IS NOT NULL`) are not "needed to return" and
  stay index-only.
- `add_path` may have already pruned every heap-projecting alternative as
  dominated by the (then-cheaper) IOS, so the hook **re-adds a bitmap-heap path**
  over the same index clauses (roaring-native, selectivity-aware) plus a seqscan
  backstop. On larger tables a surviving bitmap path is cheaper and wins anyway.

Result (10M ClickBench): `sum(id) WHERE os=2` stays Index Only Scan at ~0.93 s
(no regression); `SELECT regionid WHERE counterid=62` becomes a Bitmap Heap Scan
returning correct values. Works for *all* key types (the heap has the real
value), so it also covers text/uuid/multi-column int8. Covered by
`expected/roaring_revproj.out` with seqscan-oracle cross-checks.

Also kept from this work: multi-column `int8` key columns are `canreturn = false`
(hashed → recheck-only, never index-only-returnable; text/uuid already were), and
the `IS NULL` IOS tuple now yields NULL rather than a garbage value.

Earlier dead-ends (kept for the record): a per-TID **reverse-bitmap** map in
`amgettuple` reconstructed values from the value-bitmap partition, but the AM
can't see the target list so it mapped *every* unconstrained column —
`sum(id) WHERE os=2` blew up to 12 s on the 10M smoke test. A work-budget gate
bounded the blow-up but still over-mapped and silently NULLed large projections.
Both were abandoned in favour of the planner-steering approach above.

**Considered and rejected: GiST-style "only INCLUDE columns are returnable."**
The tempting fix is `amcanreturn(key col) = false`, returning only INCLUDE
payload columns — exactly what a lossy GiST opclass does. It does *not* work here
because PostgreSQL's `check_index_only` (`optimizer/path/indxpath.c`) requires
every column in `index->indrestrictinfo` (the WHERE-clause columns, **including
those that become index conditions**) to be index-returnable for an
IndexOnlyScan. So flipping key columns to non-returnable would reject the
IndexOnlyScan for *any* query that filters on a key column and projects anything
— killing the flagship `sum(id) … WHERE os = 2` payload-cursor path (107M→0.6M
buffers) and even the trivially-correct `SELECT os … WHERE os = 2`. (The same
mechanism means a lossy-GiST `SELECT include_col WHERE key && box` is in fact a
plain Index Scan with recheck, not an index-only scan.) The covering store is the
only option that closes the bare-projection edge without that regression.

## Open — performance follow-ups (optional)

- **Batched tuple formation for `sum(id)`** — after the payload cursor, the
  remaining tail/mixed-large gap vs btree is per-row `index_form_tuple` +
  iterator overhead, not buffers. Batching tuple construction would let roaring
  challenge btree's `sum(id)` leading-prefix wins.
- **Parallel `ambuild`** — the build is serial (~46 min for 20-col/100M) vs
  btree's parallel ~2 min. nbtree-style parallel tuplesort would parallelize the
  dominant scan+sort phase; the bitmap-assembly phase can stay in the leader
  (no concurrent CRoaring use). See the discussion in the build code.

## Housekeeping

- ASan harness retained for future memory-bug hunts: `docker/Dockerfile.asan`
  (+ `docker-postgres-asan` image). Run a workload under it with `libasan`
  preloaded; note ASan cannot see palloc-chunk-level overflows (use a core-dump
  backtrace for those, as we did for the `needs_recheck` crash).
- Generated files to keep out of commits: `regression.diffs`, `regression.out`,
  `results/`, `.DS_Store`.
