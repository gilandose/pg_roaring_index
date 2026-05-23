#!/usr/bin/env bash
set -e

SCALE_FACTOR=${1:-0.1}

echo "Setting up TPC-H dbgen..."
if [ ! -d "tpch-dbgen" ]; then
    git clone https://github.com/electrum/tpch-dbgen.git
    cd tpch-dbgen
    make
    cd ..
fi

echo "Generating data for scale factor $SCALE_FACTOR..."
cd tpch-dbgen
./dbgen -s $SCALE_FACTOR -f
cd ..

echo "Loading data into PostgreSQL..."
psql -c "DROP TABLE IF EXISTS lineitem CASCADE;"
psql -c "
CREATE TABLE lineitem (
    l_orderkey      bigint not null,
    l_partkey       integer not null,
    l_suppkey       integer not null,
    l_linenumber    integer not null,
    l_quantity      numeric not null,
    l_extendedprice numeric not null,
    l_discount      numeric not null,
    l_tax           numeric not null,
    l_returnflag    char(1) not null,
    l_linestatus    char(1) not null,
    l_shipdate      date not null,
    l_commitdate    date not null,
    l_receiptdate   date not null,
    l_shipinstruct  char(25) not null,
    l_shipmode      char(10) not null,
    l_comment       varchar(44) not null
);"

echo "Fixing trailing delimiters and importing to PostgreSQL..."
sed 's/|$//' tpch-dbgen/lineitem.tbl > tpch-dbgen/lineitem_fixed.tbl
psql -c "\copy lineitem FROM 'tpch-dbgen/lineitem_fixed.tbl' WITH (FORMAT csv, DELIMITER '|');"

echo "TPC-H lineitem table created and loaded!"
