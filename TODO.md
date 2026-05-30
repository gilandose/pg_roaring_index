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

## Open — finish multi-column so it works fully (multi-column, multi-type)

Primary goal. The branch packs each key column value into 32 bits
(`ROARING_COL_KEY`), so today:

1. **bigint key columns are rejected** — `roaring index: unsupported column type
   bigint for multi-column key` (`roaring_util.c:58`). This fails 6/8 regression
   tests (they index a single bigint column). Need real 64-bit-capable key
   handling (or a documented, tested narrowing) so multi-column works across
   `int2/int4/int8/oid/date/float4/bool/text/uuid/...` as the single-column path
   already does.
2. **Multi-type coverage** — ensure every supported column type works *as a key*
   in a multi-column index (currently validated mainly on int4): types,
   collation/hashing for text/uuid (recheck), NULL handling per column.
3. **Regression suite green** — once (1)/(2) land, `make installcheck` should
   pass (`roaring_basic/_multicolumn/_types/_check/_customscan/_include`).
   Update `expected/` only where behaviour intentionally changed.

## Open — correctness gaps

- **Unqualified roaring scan returns 0 rows** — a no-`WHERE` scan via the roaring
  index (e.g. `GROUP BY col` with `enable_seqscan=off`) yields an empty bitmap
  instead of all rows. Should at minimum raise a clear error (or refuse the path)
  rather than silently return nothing.

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
