# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

`pg_roaring_index` is a native PostgreSQL Index Access Method (AM) implementing roaring bitmap indexes for equality lookups on moderate-cardinality columns with high write churn. The target workload: multi-tenant tables with 1K–10M distinct values, DELETE+INSERT patterns, uncorrelated physical layout.

Implementation follows the design in `vaults/Design.md`, `vaults/Page.md`, and `vaults/Lossy.md`.

## Build and Test

**Prerequisites** (macOS via Homebrew):

```sh
brew install postgresql@17        # or whichever version
export PATH="/opt/homebrew/opt/postgresql@17/bin:$PATH"

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

| Operator class | Mode | Bitmap type | Granularity | Phase |
|---|---|---|---|---|
| `roaring_tid_ops` | Exact | Roaring64 | TID-level | Phase 1 (current) |
| `roaring_page_ops` | Lossy | Roaring32 | Page-level | Phase 2 (deferred) |

**Build Phase 1 only.** Lossy mode design is in `vaults/Lossy.md` but is not being implemented yet.

### On-Disk Page Types

Every page carries a `page_type` byte in its special space. The index relation contains these page types in order:

1. **Page 0 — Metapage**: magic (`0x524F4152`), version, root directory block, pending list head/tail, tombstone root, statistics
2. **Pages 1..D — Directory**: 2-level sorted array of `(high_key: int64, child_page: BlockNumber)` entries, 12 bytes each. 11 pages fits 164K entries; always cached
3. **Pages D+1..L — Leaf**: Standard PG line-pointer layout. Each item is `RoaringLeafEntry`: `value int64 | cardinality uint32 | flags uint8 | bitmap_data bytes[]`. Entries sorted by value, binary-searched within page. `RoaringLeafSpecial` holds `left_page`/`right_page` for prefix scan
4. **Overflow pages**: Continuation of bitmaps > ~7KB, chained via `next_page` in special space
5. **Pending insert pages**: Fixed 16-byte entries (`value int64 | page_or_tid uint32 | xmin TransactionId`), 512 per page, append-only linked list
6. **Pending delete pages** (exact mode): Same structure; entries merged into tombstone bitmaps
7. **Tombstone leaf pages** (exact mode): Same format as main leaf pages; maps `value → Roaring64 bitmap of dead TIDs`

### Critical Implementation Notes

**Pending entry format is 16 bytes** (not 12 as in early design drafts):
```c
typedef struct {
    int64           value;
    uint32          page_or_tid;   /* BlockNumber (lossy) or linearized TID (exact) */
    TransactionId   xmin;          /* for MVCC visibility checks */
} RoaringPendingEntry;
/* 512 entries per 8KB page */
```

**MVCC visibility on pending list**: model on `ginHeapTupleFastInsert` / GIN's visibility checks, not a raw `TransactionId` compare. Aborted transactions' entries must be filtered at scan time or removed during cleanup — requires xmin.

**Locking protocol for pending append**: Reading the metapage for `pending_insert_tail`, then locking that page has a TOCTOU race. Use a retry loop: re-read metapage after acquiring the tail page lock and finding it full.

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

### Source File Layout (planned)

```
pg_roaring_index.c        AM handler + operator class SQL definitions
roaring_build.c           ambuild: heap scan → hash aggregate → write pages
roaring_insert.c          aminsert: pending list append
roaring_scan.c            amgetbitmap: directory lookup + bitmap deserialization + pending OR
roaring_vacuum.c          ambulkdelete (no-op exact, tombstone append) + amvacuumcleanup (merge)
roaring_pending.c         append, merge, truncate, back-pressure
roaring_page.c            page init, leaf entry read/write, directory read/write
roaring_cost.c            amcostestimate
roaring_compact.c         tombstone compaction (exact mode)
src/vendor/croaring/      CRoaring vendored source
sql/                      CREATE EXTENSION SQL
expected/                 regression test expected output
```

## Key References

- `src/backend/access/gin/ginfast.c` — pending list template (study before implementing)
- `src/backend/access/gin/ginvacuum.c` — GIN vacuum / cleanup model
- `src/backend/access/nbtree/README` — formal concurrency protocol model to emulate
- `src/backend/access/brin/` — BRIN no-op ambulkdelete + resummarize pattern (lossy mode template)
- `src/backend/storage/page/generic_xlog.c` — WAL strategy used throughout
- CRoaring: `roaring_bitmap_portable_deserialize_safe()`, `roaring64_bitmap_andnot_inplace()`, `roaring_bitmap_get_cardinality()` are the core API surface

## Implementation Order (Phase 1)

1. `ambuild` + `amgetbitmap` — static index, read-only after build (no pending, no tombstones yet)
2. `aminsert` — pending list append with xmin, back-pressure, TOCTOU-safe tail locking
3. `ambulkdelete` — tombstone pending list for deletes (exact mode)
4. `amvacuumcleanup` — pending merge + tombstone compaction
5. WAL safety audit (generic_xlog, crash-safe merge ordering)
6. `amcostestimate` — exact cardinality from entry header
7. Concurrent access correctness (xmin visibility, parallel `amgetbitmap`)
8. Composite key support: pack `(company_id int32, location_id int32)` into single int64, prefix scan via doubly-linked leaves
9. Regression tests + benchmarks
