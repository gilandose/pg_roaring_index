#!/usr/bin/env bash
set -e

echo "Downloading ClickBench schema..."
curl -sL https://raw.githubusercontent.com/ClickHouse/ClickBench/main/postgresql/create.sql > clickbench_create.sql

echo "Creating hits table..."
psql -c "DROP TABLE IF EXISTS hits CASCADE;"
psql -f clickbench_create.sql

echo "Downloading and inserting 1 million rows from ClickBench..."
# Download from uncompressed hits.tsv and head it.
curl -sL https://datasets.clickhouse.com/hits_compatible/hits.tsv.gz | \
    gzip -d | head -n 1000000 > hits_1m.tsv

echo "Importing to PostgreSQL..."
psql -c "\copy hits FROM 'hits_1m.tsv' WITH (FORMAT csv, DELIMITER E'\t');"

echo "ClickBench hits table created and loaded!"
