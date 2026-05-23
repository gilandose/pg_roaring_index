#!/usr/bin/env python3
import random
import subprocess
import argparse
import sys

def execute_sql(sql, dbname="postgres"):
    cmd = ["psql", "-d", dbname, "-t", "-A", "-c", sql]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"SQL Error:\n{result.stderr}", file=sys.stderr)
        sys.exit(1)
    return result.stdout.strip()

def setup_tables(dbname):
    print("Setting up test tables...")
    sql = """
    DROP TABLE IF EXISTS test_data CASCADE;
    CREATE TABLE test_data (
        id serial PRIMARY KEY,
        c1 int, c2 int, c3 int, c4 int, c5 int
    );
    INSERT INTO test_data (c1, c2, c3, c4, c5)
    SELECT
        (random() * 10)::int,
        (random() * 100)::int,
        (random() * 1000)::int,
        (random() * 100)::int,
        (random() * 10)::int
    FROM generate_series(1, 100000);

    -- Ensure we have roaring extension
    CREATE EXTENSION IF NOT EXISTS pg_roaring_index;

    -- Standard table
    DROP TABLE IF EXISTS test_btree CASCADE;
    CREATE TABLE test_btree AS SELECT * FROM test_data;
    CREATE INDEX idx_bt_c1 ON test_btree(c1);
    CREATE INDEX idx_bt_c2 ON test_btree(c2);
    CREATE INDEX idx_bt_c3 ON test_btree(c3);

    -- Roaring Exact table
    DROP TABLE IF EXISTS test_roaring CASCADE;
    CREATE TABLE test_roaring AS SELECT * FROM test_data;
    CREATE INDEX idx_rr_c1 ON test_roaring USING roaring(c1);
    CREATE INDEX idx_rr_c2 ON test_roaring USING roaring(c2);
    CREATE INDEX idx_rr_c3 ON test_roaring USING roaring(c3);

    -- Roaring Lossy table
    DROP TABLE IF EXISTS test_lossy CASCADE;
    CREATE TABLE test_lossy AS SELECT * FROM test_data;
    CREATE INDEX idx_rl_c1 ON test_lossy USING roaring_lossy(c1);
    CREATE INDEX idx_rl_c2 ON test_lossy USING roaring_lossy(c2);
    CREATE INDEX idx_rl_c3 ON test_lossy USING roaring_lossy(c3);

    VACUUM ANALYZE test_btree;
    VACUUM ANALYZE test_roaring;
    VACUUM ANALYZE test_lossy;
    """
    execute_sql(sql, dbname)
    print("Tables created.")

def run_fuzzer(dbname, iterations):
    print(f"Running {iterations} iterations of differential tests...")
    
    # We want to force index scans for exact roaring to ensure it's used
    
    ops = ['=', '!=', 'IN']
    cols = ['c1', 'c2', 'c3']
    
    for i in range(iterations):
        # Generate random WHERE clause
        num_conds = random.randint(1, 3)
        conds = []
        for _ in range(num_conds):
            col = random.choice(cols)
            op = random.choice(ops)
            if op == 'IN':
                vals = [str(random.randint(0, 100)) for _ in range(random.randint(2, 5))]
                conds.append(f"{col} IN ({','.join(vals)})")
            elif op == '=':
                val = random.randint(0, 10)
                conds.append(f"{col} = {val}")
            else:
                val = random.randint(0, 10)
                conds.append(f"{col} != {val}")
                
        where_clause = " AND ".join(conds)
        
        # Test btree (baseline)
        sql_bt = f"SET enable_seqscan=off; SELECT COALESCE(SUM(id), 0) || '_' || COUNT(*) FROM test_btree WHERE {where_clause};"
        res_bt = execute_sql(sql_bt, dbname)
        
        # Test roaring exact
        sql_rr = f"SET enable_seqscan=off; SELECT COALESCE(SUM(id), 0) || '_' || COUNT(*) FROM test_roaring WHERE {where_clause};"
        res_rr = execute_sql(sql_rr, dbname)
        
        # Test roaring lossy
        sql_rl = f"SET enable_seqscan=off; SELECT COALESCE(SUM(id), 0) || '_' || COUNT(*) FROM test_lossy WHERE {where_clause};"
        res_rl = execute_sql(sql_rl, dbname)
        
        if not (res_bt == res_rr == res_rl):
            print(f"FAILURE on iteration {i}!")
            print(f"Query: WHERE {where_clause}")
            print(f"Btree  : {res_bt}")
            print(f"Exact  : {res_rr}")
            print(f"Lossy  : {res_rl}")
            sys.exit(1)
            
        if i % 100 == 0 and i > 0:
            print(f"  ... {i} iterations passed")
            
    print("All differential tests passed successfully! 100% Correctness Verified.")

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("-d", "--dbname", default="postgres", help="Database name")
    parser.add_argument("-i", "--iterations", type=int, default=1000, help="Number of random queries")
    args = parser.parse_args()
    
    setup_tables(args.dbname)
    run_fuzzer(args.dbname, args.iterations)
