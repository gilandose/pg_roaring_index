#!/usr/bin/env bash
# Run the benchmark against the Docker postgres container.
# Usage: bash docker/run-bench.sh [extra psql args]
set -euo pipefail

COMPOSE_FILE="$(dirname "$0")/docker-compose.yml"
BENCH_SQL="$(dirname "$0")/bench.sql"

# Start (or ensure running) without re-creating volumes.
docker compose -f "$COMPOSE_FILE" up -d --build

echo "Waiting for postgres to be ready..."
until docker compose -f "$COMPOSE_FILE" exec -T postgres \
        pg_isready -U postgres -d roaring_test -q 2>/dev/null; do
    sleep 1
done
echo "Ready."
echo ""

# Copy bench.sql into the container and run it.
docker compose -f "$COMPOSE_FILE" exec -T postgres \
    psql -U postgres -d roaring_test "$@" < "$BENCH_SQL"
