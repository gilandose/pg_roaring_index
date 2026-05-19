-- bench/sizes.sql
--
-- Report table + index sizes for all four AMs.
-- Run after bench/setup.sql.

\echo ''
\echo '=== Index sizes ==='
\echo ''

SELECT
    am                                                  AS "AM",
    pg_size_pretty(heap_bytes)                          AS "heap",
    pg_size_pretty(idx_bytes)                           AS "index",
    round(idx_bytes::numeric / heap_bytes * 100, 1)     AS "idx/heap %",
    pg_size_pretty(heap_bytes + idx_bytes)              AS "total"
FROM (
    SELECT
        CASE relname
            WHEN 'bench_btree'   THEN 'btree'
            WHEN 'bench_gin'     THEN 'gin'
            WHEN 'bench_brin'    THEN 'brin'
            WHEN 'bench_roaring' THEN 'roaring'
        END                                                       AS am,
        pg_relation_size(c.oid)                                   AS heap_bytes,
        (SELECT sum(pg_relation_size(i.indexrelid))
         FROM   pg_index i WHERE i.indrelid = c.oid)             AS idx_bytes
    FROM pg_class c
    WHERE relname IN ('bench_btree','bench_gin','bench_brin','bench_roaring')
      AND relkind = 'r'
) s
ORDER BY am;

\echo ''
\echo '=== Row counts ==='
\echo ''

SELECT 'bench_btree'   AS table, count(*) AS rows FROM bench_btree
UNION ALL
SELECT 'bench_gin',             count(*)           FROM bench_gin
UNION ALL
SELECT 'bench_brin',            count(*)           FROM bench_brin
UNION ALL
SELECT 'bench_roaring',         count(*)           FROM bench_roaring
ORDER BY 1;
