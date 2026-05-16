FROM postgres:18

# Install build toolchain (postgresql-server-dev-$PG_MAJOR for headers + pg_config)
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        postgresql-server-dev-${PG_MAJOR} \
        curl \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build/pg_roaring_index

# Copy only what's needed to compile
COPY Makefile pg_roaring_index.control pg_roaring_index--1.0.sql ./
COPY include/ include/
COPY src/     src/
COPY scripts/ scripts/

RUN ([ -f src/vendor/croaring/roaring.h ] || bash scripts/fetch-croaring.sh) \
    && make \
    && make install \
    && cd / && rm -rf /build

# Remove build tools to keep runtime image lean
RUN apt-get remove -y --purge build-essential postgresql-server-dev-${PG_MAJOR} curl \
    && apt-get autoremove -y \
    && rm -rf /var/lib/apt/lists/*
