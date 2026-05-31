# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

`pg_roaring_index` is a custom PostgreSQL Index Access Method (AM) implementing roaring bitmap indexes for equality lookups. While initially evaluated on moderate-cardinality columns (1K–10M distinct values) with high write churn, these are not hard limits. The index design should remain open-minded to broader use cases, varying cardinalities, and new column types.

Implementation follows the design in `vaults/Design.md`, `vaults/Page.md`, and `vaults/Lossy.md`.

## Build and Test

**Prerequisites** (macOS via Homebrew):

```sh
brew install postgresql@18        # or whichever version
export PATH="/opt/homebrew/opt/postgresql@18/bin:$PATH"

# Fetch CRoaring amalgamation (one-time; gitignored)
bash scripts/fetch-croaring.sh
```

**Build**:

```sh
make                    # compile pg_roaring_index.so
make install            # install to $(pg_config --pkglibdir)
make installcheck       # run regression tests (needs a running PG cluster)
make clean
```

> **macOS stale-dylib trap**: `make install` replaces the file on disk, but any
> PostgreSQL backends that already `dlopen`'d the old `.dylib` keep using the
> old version in memory.  Always restart the server after install before running
> tests or benchmarks:
> ```sh
> pg_ctl restart -D /opt/homebrew/var/postgresql@18
> ```
> Symptom of loading the wrong version: crash reports whose UUID does not match
> `dwarfdump --uuid /opt/homebrew/lib/postgresql@18/pg_roaring_index.dylib`.

**Single regression test**:

```sh
psql -U postgres -f sql/roaring_basic.sql | diff - expected/roaring_basic.out
```

CRoaring is vendored as a two-file amalgamation (`src/vendor/croaring/roaring.{c,h}`) fetched by `scripts/fetch-croaring.sh`. The files are gitignored — every checkout needs to run the fetch script once.

## Python Tooling

Always use `uv run python` (or `uv pip`) when executing Python scripts or managing dependencies. Never use standard `python` or `pip` commands without `uv`.

## C Coding Standards

Follow PostgreSQL source conventions exactly:

- Use `pgindent` for formatting (tabs, not spaces; Allman braces)
- Type names: `CamelCase` (e.g., `RoaringMetaPageData`). Functions: `snake_case` with module prefix (e.g., `roaring_pending_append`)
- Palloc, not malloc. `elog(ERROR, ...)` for errors, never `abort()`
- All page modifications through `generic_xlog` — no custom WAL resource manager
- Buffer locking: always `LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE)` before writing, `LockBuffer(buf, BUFFER_LOCK_SHARE)` before reading
- Always call `UnlockReleaseBuffer()` even on error paths (use `PG_TRY`/`PG_FINALLY`)
- Read `src/backend/access/gin/ginfast.c` in the PostgreSQL source tree before implementing the pending list — it is the direct template for our approach

## Architecture

### Operator Classes (exact mode only)

The index runs exclusively in **exact** mode via the `roaring` access method,
storing linearized TIDs (`(blkno << 9) | (offset - 1)`) in `roaring64` bitmaps.
The `roaring_lossy` AM and its page-level opclasses were removed before launch
(commit `3531df1`) — exact mode is the only mode.

Default (`= equality`) operator classes, one per supported type:

`roaring_int2_tid_ops` · `roaring_int4_tid_ops` · `roaring_int8_tid_ops` ·
`roaring_bool_tid_ops` · `roaring_date_tid_ops` · `roaring_float4_tid_ops` ·
`roaring_oid_tid_ops` · `roaring_enum_tid_ops` · `roaring_text_tid_ops` ·
`roaring_uuid_tid_ops`

`text`, `uuid`, and multi-column `int8` keys are hash-encoded into the key slot
and rely on executor recheck for exactness; single-column `int8` is lossless.
The CRoaring `roaring64_*` API is used throughout (the linearized TID needs the
full 64-bit domain).

### On-Disk Page Types

Every page carries a `page_type` byte in its special space. The index relation contains these page types in order:

1. **Page 0 — Metapage**: magic (`0x524F4152`), version (3), root directory block, 8-way sharded pending list heads (`insert_head`/`merging_head` per shard), payload directory head, statistics. (A tombstone root field exists but the tombstone path is unimplemented.)
2. **Pages 1..D — Directory**: 2-level sorted array of `(high_key: int64, child_page: BlockNumber)` entries, 16 bytes each (int64 + BlockNumber + 4-byte pad). Always cached.
3. **Pages D+1..L — Leaf**: Standard PG line-pointer layout. Each item is `RoaringLeafEntry`: `value int64 | cardinality uint32 | flags uint8 | bitmap_data bytes[]`. Entries sorted by value (negative keys — per-column NULL bitmaps — sort first), binary-searched within page. `RoaringLeafSpecial` holds `left_page`/`right_page` for prefix scan.
4. **Overflow pages**: Continuation of bitmaps > ~7KB, chained via `next_page` in special space.
5. **Pending insert pages**: Fixed 24-byte entries (`value int64 | linear_tid uint64 | xmin TransactionId | _pad uint32`), `ROARING_PENDING_PER_PAGE` = 339 per page, append-only linked list (8 sharded chains). `RoaringPendingSpecial` carries `value_min`/`value_max` for page-skip during scan.
6. **Payload pages (`PAYLOAD` 0x07 / `PAYLOAD_DIR` 0x08)**: dense per-TID `int64` array for an `INCLUDE` column, located via a payload directory; read by the streaming payload cursor for IndexOnlyScan projection.
7. **Tombstone / pending-delete pages**: header data structures exist but the path is unimplemented — `ambulkdelete` modifies leaf bitmaps inline instead.

### Critical Implementation Notes

**Pending entry format is 24 bytes**:
```c
typedef struct {
    int64           value;         /* the (possibly column-namespaced) key */
    uint64          linear_tid;    /* ((uint64)blkno << 9) | (offset - 1) */
    TransactionId   xmin;          /* for MVCC visibility checks */
    uint32          _pad;          /* keep sizeof == 24, array alignment */
} RoaringPendingEntry;
/* ROARING_PENDING_PER_PAGE = 339 entries per 8KB page; 8 sharded chains */
```

**MVCC visibility on pending list**: GIN-style four-state check in `roaring_pending_visible`: handles own-xid, committed, in-progress, aborted, and frozen xids. Aborted entries are dropped silently in `roaring_merge_pending` and by the aborted-xmin check in `ambulkdelete`.

**Locking protocol for pending append**: The entire `roaring_pending_append` holds the metapage exclusive lock throughout — this eliminates the TOCTOU race without needing a retry loop. New inserts queue on the metapage lock, which is held only briefly per insert.

**Crash-safe merge ordering**: Write all updated leaf pages first, then truncate pending pages. OR into bitmaps is idempotent — if we crash after leaf writes but before truncating pending, a retry re-ORs the same values harmlessly. Mark pending pages "merge in progress" before starting.

**Directory growth**: The 2-level directory must handle values beyond the initial estimate. Either rebuild during `REINDEX` (simplest, acceptable for moderate cardinality growth) or implement a level-addition protocol.

**Pending back-pressure**: Implement a hard cap analogous to `gin_pending_list_limit`. When pending exceeds threshold, trigger an inline merge during `aminsert` rather than letting the list grow unbounded.

**Cost estimation and cardinality**: `amcostestimate` reads exact cardinality from the leaf entry header (no bitmap deserialization). Between merges, stored cardinality is stale by pending list contents — bound the error or read pending counts during planning.

**work_mem and lossiness**: Even in exact mode, the executor converts `TIDBitmap` to lossy page entries under memory pressure. "No recheck in exact mode" is conditional on `work_mem` being sufficient.

### AM Routine Registration

```c
/* pg_roaring_index.c */
Datum roaring_handler(PG_FUNCTION_ARGS) {
    IndexAmRoutine *amroutine = makeNode(IndexAmRoutine);
    amroutine->amgetbitmap   = roaring_getbitmap;
    amroutine->ambuild       = roaring_build;
    amroutine->aminsert      = roaring_insert;
    amroutine->ambulkdelete  = roaring_bulkdelete;
    amroutine->amvacuumcleanup = roaring_vacuumcleanup;
    amroutine->amcostestimate = roaring_costestimate;
    /* ... */
}
```

### Source File Layout

```
src/pg_roaring_index.c    roaring AM handler registration (incl. amcanreturn)
src/roaring_build.c       ambuild: sort-then-batch heap scan → write leaf+dir pages
src/roaring_insert.c      aminsert: pending list append (single + multi-column)
src/roaring_scan.c        amgetbitmap / amgettuple: dir lookup, bitmap deserialize,
                          pending OR, NULL bitmaps, reverse-bitmap key projection
src/roaring_vacuum.c      ambulkdelete: inline leaf bitmap modification
                          amvacuumcleanup: crash recovery + roaring_merge_pending
src/roaring_customscan.c  RoaringCount custom scan: count(*) via bitmap intersection
src/roaring_payload.c     INCLUDE payload store + streaming payload cursor
src/roaring_util.c        page alloc, WAL helpers, overflow chain, key<->datum
src/roaring_cost.c        amcostestimate
src/roaring_check.c       roaringcheck() index verification function
src/roaring_stats.c       roaring_index_stats() introspection
src/roaring_bgworker.c    background merge worker
src/vendor/croaring/      CRoaring amalgamation (gitignored, fetch via scripts/)
sql/                      CREATE EXTENSION SQL + regression test inputs
expected/                 regression test expected output
bench/                    100M roaring-vs-btree benchmark suite (+ README)
docker/                   containerized PostgreSQL 18 build
```

## Key References

- `src/backend/access/gin/ginfast.c` — pending list template (study before implementing)
- `src/backend/access/gin/ginvacuum.c` — GIN vacuum / cleanup model
- `src/backend/access/nbtree/README` — formal concurrency protocol model to emulate
- `src/backend/access/brin/` — BRIN no-op ambulkdelete + resummarize pattern
- `src/backend/storage/page/generic_xlog.c` — WAL strategy used throughout
- CRoaring: `roaring64_bitmap_portable_deserialize_safe()`, `roaring64_bitmap_and_inplace()`, `roaring64_bitmap_get_cardinality()`, `roaring64_bitmap_add()` are the core API surface (the `roaring64_*` API is used throughout, for the 64-bit linearized TID domain)

## Implementation Status

**Implemented:**
- `ambuild` (sort-then-batch, single pass over sorted TID array; tuplesort spill to disk)
- `aminsert` with 8-way sharded pending list, MVCC xmin, back-pressure threshold
- `amgetbitmap` / `amgettuple` (IndexScan + IndexOnlyScan) with directory lookup, overflow chains, pending OR, `amsearcharray` (IN queries)
- Multi-column keys (`(attno << 32) | key32`), per-column bitmap intersection at scan time
- Per-column **NULL bitmaps**: `IS NULL` / `IS NOT NULL` answered from the index (`amsearchnulls`), composable with equality
- `INCLUDE` payload column with a streaming payload cursor for index-only projection (e.g. `sum(id)`)
- **Reverse-bitmap projection**: returns an unconstrained key column (`SELECT a WHERE b=5`, also IN / IS NOT NULL) for the lossless key types; `amcanreturn` honest (text/uuid + multi-col int8 are non-returnable)
- **RoaringCount** custom scan: `count(*)` (incl. multi-column AND and `IS NULL`) by intersecting bitmaps / reading cardinalities
- `ambulkdelete`: inline leaf bitmap modification, parallel-safe (`VACUUM_OPTION_PARALLEL_BULKDEL`)
- `amvacuumcleanup`: crash recovery from interrupted merge + `roaring_merge_pending`
- `amcostestimate`: cardinality from metapage, RoaringAmCache for plan-time skip
- WAL safety: all page writes via `generic_xlog`; `merging_head` crash-recovery anchor
- REINDEX CONCURRENTLY: works without code changes

**Not implemented (known deferred):**
- Tombstone/pending-delete path — `ambulkdelete` modifies leaf bitmaps inline instead
- Lossless multi-column `int8` (and a per-column covering store to make hashed key columns returnable) — hashed multi-column `int8`/`text`/`uuid` keys rely on executor recheck
- Composite key support (`(company_id, location_id)` packed into one key)
- Parallel `ambuild` (build is currently serial)
