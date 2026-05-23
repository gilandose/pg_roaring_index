#!/usr/bin/env python3
import time
import subprocess
import sys
import random
import os

def execute_sql(sql):
    cmd = ["psql", "-t", "-A", "-c", sql]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"SQL Error:\n{result.stderr}", file=sys.stderr)
        sys.exit(1)
    return result.stdout.strip()

def run_performance_test():
    print("--- 1. Write Performance & Size Test ---")
    sql_setup = """
    DROP TABLE IF EXISTS test_data CASCADE;
    CREATE TABLE test_data (id serial PRIMARY KEY, c1 int);
    INSERT INTO test_data (c1) SELECT (random() * 100)::int FROM generate_series(1, 100000);
    CREATE EXTENSION IF NOT EXISTS pg_roaring_index;
    
    DROP TABLE IF EXISTS t_btree; CREATE TABLE t_btree AS SELECT * FROM test_data;
    DROP TABLE IF EXISTS t_roaring; CREATE TABLE t_roaring AS SELECT * FROM test_data;
    DROP TABLE IF EXISTS t_lossy; CREATE TABLE t_lossy AS SELECT * FROM test_data;
    """
    execute_sql(sql_setup)
    
    # Time initial build
    t0 = time.time()
    execute_sql("CREATE INDEX idx_bt ON t_btree(c1);")
    bt_build = time.time() - t0
    
    t0 = time.time()
    execute_sql("CREATE INDEX idx_rr ON t_roaring USING roaring(c1);")
    rr_build = time.time() - t0
    
    t0 = time.time()
    execute_sql("CREATE INDEX idx_rl ON t_lossy USING roaring_lossy(c1);")
    rl_build = time.time() - t0
    
    print(f"Index Build (100k rows): BTree={bt_build:.3f}s, RoaringExact={rr_build:.3f}s, RoaringLossy={rl_build:.3f}s")
    
    # Time pending append
    append_sql = "INSERT INTO {} (c1) SELECT (random() * 100)::int FROM generate_series(1, 10000);"
    
    t0 = time.time()
    execute_sql(append_sql.format("t_btree"))
    bt_append = time.time() - t0
    
    t0 = time.time()
    execute_sql(append_sql.format("t_roaring"))
    rr_append = time.time() - t0
    
    t0 = time.time()
    execute_sql(append_sql.format("t_lossy"))
    rl_append = time.time() - t0
    
    print(f"Pending Append (10k rows): BTree={bt_append:.3f}s, RoaringExact={rr_append:.3f}s, RoaringLossy={rl_append:.3f}s")
    
    # Sizes
    sizes_sql = """
    SELECT pg_relation_size('idx_bt') AS bt_size,
           pg_relation_size('idx_rr') AS rr_size,
           pg_relation_size('idx_rl') AS rl_size;
    """
    sizes = execute_sql(sizes_sql).split('|')
    print(f"Sizes (bytes): BTree={sizes[0]}, RoaringExact={sizes[1]}, RoaringLossy={sizes[2]}")
    if int(sizes[2]) > int(sizes[0]):
        print("WARNING: Roaring lossy is larger than B-tree for ndistinct=100. This is unexpected.")

def run_correctness_test():
    print("\n--- 2. Differential Correctness Test ---")
    
    for i in range(50):
        val = random.randint(0, 100)
        bt_res = execute_sql(f"SET enable_seqscan=off; SELECT COUNT(*) FROM t_btree WHERE c1 = {val}")
        rr_res = execute_sql(f"SET enable_seqscan=off; SELECT COUNT(*) FROM t_roaring WHERE c1 = {val}")
        rl_res = execute_sql(f"SET enable_seqscan=off; SELECT COUNT(*) FROM t_lossy WHERE c1 = {val}")
        
        if not (bt_res == rr_res == rl_res):
            print(f"FAILURE on val {val}! BT={bt_res}, RR={rr_res}, RL={rl_res}")
            sys.exit(1)
            
    print("50 random queries passed exact differential comparison against B-Tree.")

if __name__ == "__main__":
    print("Starting fast regression suite...")
    run_performance_test()
    run_correctness_test()
    print("Regression suite completed successfully!")
