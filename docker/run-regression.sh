#!/usr/bin/env bash
set -e

cd "$(dirname "$0")/.."

echo "Building and starting clean PostgreSQL container..."
# Rebuild to ensure latest extension code is compiled
docker compose -f docker/docker-compose.yml build
# Force recreate to ensure a clean container state
docker compose -f docker/docker-compose.yml up -d --force-recreate

echo "Waiting for PostgreSQL to be ready..."
until docker compose -f docker/docker-compose.yml exec postgres pg_isready -U postgres -d roaring_test; do
  sleep 1
done

echo "Running Python regression suite via uv..."
# Set env variables so the script connects to the exposed container
export PGDATABASE=roaring_test
export PGUSER=postgres
export PGPASSWORD=roaring
export PGPORT=5433
export PGHOST=localhost

# Use uv run python to execute the suite
uv run python bench/run_regression.py

echo "Tearing down container..."
docker compose -f docker/docker-compose.yml down
