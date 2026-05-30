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

## Open — regression failures still red (4/8, pre-existing, NOT int8)

- `roaring_customscan`, `roaring_include`: **stale EXPLAIN goldens** — code emits
  `RoaringCount: N qual(s)` (was `N keys`) and IndexOnlyScan where the golden
  shows IndexScan (VM now all-visible). Regenerate `expected/` once confirmed
  intentional.
- `roaring_basic`, `roaring_multicolumn`: **`IS NULL` returns wrong rows under
  `enable_seqscan=off`** — see below.

## Open — correctness gaps (no-key / non-equality scans)

`amoptionalkey = true` lets the planner run the roaring index with **zero
equality scan keys** (when `enable_seqscan=off` and no usable equality qual).
The scan then misbehaves; two faces of the same root cause:

- **`WHERE col IS NULL`** returns *all indexed (non-NULL) rows* instead of 0
  (e.g. 6 instead of 1). Fails `roaring_basic` / `roaring_multicolumn` under
  their `SET enable_seqscan=off`. Normal `IS NULL` (seqscan allowed) is correct.
- **Unqualified scan** (`GROUP BY col`, no `WHERE`) returns 0 rows instead of
  all.

Fix direction: when the scan has no equality key, the roaring index cannot
answer the predicate — it should contribute nothing and let the executor filter
(or the path should not be chosen). Decide between (a) return empty + rely on
recheck/seqscan fallback, or (b) `amoptionalkey = false` (forces ≥1 key, but
must still allow constraining a column *subset* of a multi-column index).

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
