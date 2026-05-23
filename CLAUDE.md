# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

`pg_roaring_index` is a native PostgreSQL Index Access Method (AM) implementing roaring bitmap indexes for equality lookups on moderate-cardinality columns with high write churn. The target workload: multi-tenant tables with 1K–10M distinct values, DELETE+INSERT patterns, uncorrelated physical layout.

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

### Two Operator Classes (two modes)

Both modes are implemented and registered.

| Opclass | AM | Mode | Bitmap type | Granularity |
|---|---|---|---|---|
| `roaring_int8_tid_ops` / `roaring_int4_tid_ops` | `roaring` | Exact | Roaring32 | TID-level |
| `roaring_int8_page_ops` / `roaring_int4_page_ops` | `roaring_lossy` | Lossy | Roaring32 | Page-level |

Exact mode stores linearized TIDs (`(blkno << 9) | (offset-1)`). Lossy mode stores block numbers; `amrecheck=true` — the executor rechecks every row on matched pages. Lossy is 10-100× smaller at low cardinality.

### On-Disk Page Types

Every page carries a `page_type` byte in its special space. The index relation contains these page types in order:

1. **Page 0 — Metapage**: magic (`0x524F4152`), version, root directory block, pending list head/tail, tombstone root, statistics
2. **Pages 1..D — Directory**: 2-level sorted array of `(high_key: int64, child_page: BlockNumber)` entries, 16 bytes each (int64 + BlockNumber + 4-byte pad). Always cached.
3. **Pages D+1..L — Leaf**: Standard PG line-pointer layout. Each item is `RoaringLeafEntry`: `value int64 | cardinality uint32 | flags uint8 | bitmap_data bytes[]`. Entries sorted by value, binary-searched within page. `RoaringLeafSpecial` holds `left_page`/`right_page` for prefix scan
4. **Overflow pages**: Continuation of bitmaps > ~7KB, chained via `next_page` in special space
5. **Pending insert pages**: Fixed 16-byte entries (`value int64 | linear_tid uint32 | xmin TransactionId`), 510 per page, append-only linked list. `RoaringPendingSpecial` carries `value_min`/`value_max` for page-skip during scan.
6. **Pending delete / tombstone pages**: Data structures exist in the metapage and header but the tombstone path is not implemented. `ambulkdelete` instead modifies leaf bitmaps inline.

### Critical Implementation Notes

**Pending entry format is 16 bytes**:
```c
typedef struct {
    int64           value;
    uint32          linear_tid;    /* (blkno<<9)|(offset-1) for exact; blkno for lossy */
    TransactionId   xmin;          /* for MVCC visibility checks */
} RoaringPendingEntry;
/* 510 entries per 8KB page */
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
src/pg_roaring_index.c    AM handler registration for both roaring and roaring_lossy
src/roaring_build.c       ambuild: sort-then-batch heap scan → write leaf+dir pages
src/roaring_insert.c      aminsert: pending list append (exact + lossy variants)
src/roaring_scan.c        amgetbitmap: dir lookup + bitmap deserialize + pending OR
                          roaring_getbitmap_lossy: tbm_add_page variant
src/roaring_vacuum.c      ambulkdelete: inline leaf bitmap modification
                          amvacuumcleanup: crash recovery + roaring_merge_pending
src/roaring_util.c        page alloc, WAL helpers, overflow chain read/write
src/roaring_cost.c        amcostestimate
src/vendor/croaring/      CRoaring amalgamation (gitignored, fetch via scripts/)
sql/                      CREATE EXTENSION SQL
expected/                 regression test expected output
bench/                    sweep.sh (cardinality sweep), multicolumn.sh (AND benchmark)
```

## Key References

- `src/backend/access/gin/ginfast.c` — pending list template (study before implementing)
- `src/backend/access/gin/ginvacuum.c` — GIN vacuum / cleanup model
- `src/backend/access/nbtree/README` — formal concurrency protocol model to emulate
- `src/backend/access/brin/` — BRIN no-op ambulkdelete + resummarize pattern (lossy mode template)
- `src/backend/storage/page/generic_xlog.c` — WAL strategy used throughout
- CRoaring: `roaring_bitmap_portable_deserialize_safe()`, `roaring_bitmap_andnot_inplace()`, `roaring_bitmap_get_cardinality()`, `roaring_bitmap_add_many()` are the core API surface (Roaring32 throughout — `roaring64_*` API is not used)

## Implementation Status

**Implemented:**
- `ambuild` (sort-then-batch, single pass over sorted TID array)
- `aminsert` with pending list, MVCC xmin, back-pressure threshold
- `amgetbitmap` with directory lookup, overflow chains, pending OR, dual-chain scan, amsearcharray (IN queries)
- `ambulkdelete`: inline leaf bitmap modification, parallel-safe (`VACUUM_OPTION_PARALLEL_BULKDEL`)
- `amvacuumcleanup`: crash recovery from interrupted merge + `roaring_merge_pending`
- `amcostestimate`: cardinality from metapage, RoaringAmCache for plan-time skip
- Both exact (`roaring`) and lossy (`roaring_lossy`) AMs fully registered
- WAL safety: all page writes via `generic_xlog`; `pending_merging_head` crash-recovery anchor
- REINDEX CONCURRENTLY: works without code changes

**Not implemented (known deferred):**
- Tombstone/pending-delete path — `ambulkdelete` modifies leaf bitmaps inline instead
- Composite key support (`(company_id, location_id)` packed into int64)
- Adaptive threshold opclass (`roaring_auto_ops`, switches exact↔lossy per value)
- Lossy dead-block cleanup — stale block numbers accumulate until re-insert
