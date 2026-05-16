-- Load the extension and create the test schema.
-- Runs once on first `docker compose up` (postgres initdb hook).

CREATE EXTENSION pg_roaring_index;

-- -----------------------------------------------------------------------
-- Test table: simulates a multi-tenant menu/catalog system.
--
--   1 000 000 rows
--   10 000 distinct location_id values  (~100 rows per location)
--       5 distinct status values
--     100 distinct region_id values
--
-- This is the reference workload from the design docs, scaled down 160x.
-- -----------------------------------------------------------------------

CREATE TABLE items (
    id          bigserial PRIMARY KEY,
    location_id int       NOT NULL,
    status      text      NOT NULL,
    region_id   int       NOT NULL,
    name        text      NOT NULL,
    payload     text      NOT NULL    -- realistic row width
);

-- Populate 1 000 000 rows.
-- Uses a fixed seed so results are reproducible across runs.
SELECT setseed(0.42);

INSERT INTO items (location_id, status, region_id, name, payload)
SELECT
    (random() * 9999)::int + 1,
    (ARRAY['active','inactive','pending','archived','suspended'])
        [ floor(random() * 5)::int + 1 ],
    (random() * 99)::int + 1,
    'item_' || i,
    repeat('x', 80)
FROM generate_series(1, 1000000) AS i;

VACUUM ANALYZE items;

-- Report what we built.
SELECT
    count(*)                                    AS total_rows,
    count(DISTINCT location_id)                 AS distinct_locations,
    round(count(*)::numeric / count(DISTINCT location_id), 1)
                                                AS avg_rows_per_location,
    count(DISTINCT status)                      AS distinct_statuses,
    count(DISTINCT region_id)                   AS distinct_regions,
    pg_size_pretty(pg_total_relation_size('items')) AS table_size
FROM items;
