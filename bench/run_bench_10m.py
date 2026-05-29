import os
import time
import psycopg2
import json
import threading
from jinja2 import Environment, FileSystemLoader

DB_NAME = "benchdb"
TABLE_NAME = "hits_10m"

# The same 10-column index setup
COLUMNS = "os, sex, javascriptenable, regionid, advengineid, isoldcounter, dontcounthits, goodevent, age"
INCLUDE = "userid"

QUERIES = [
    {
        "name": "Equality (os=2)",
        "query": f"SELECT sum(userid) FROM {TABLE_NAME} WHERE os = 2;",
        "type": "sum"
    },
    {
        "name": "Scatter 3-col",
        "query": f"SELECT sum(userid) FROM {TABLE_NAME} WHERE isoldcounter = 1 AND age = 30 AND advengineid = 2;",
        "type": "sum"
    },
    {
        "name": "Tail 3-col",
        "query": f"SELECT sum(userid) FROM {TABLE_NAME} WHERE regionid = 225 AND dontcounthits = 0 AND sex = 2;",
        "type": "sum"
    },
    {
        "name": "Count(*) Fast (os=2)",
        "query": f"SELECT count(*) FROM {TABLE_NAME} WHERE os = 2;",
        "type": "count"
    }
]

def get_conn():
    return psycopg2.connect(dbname=DB_NAME)

def setup_indexes():
    conn = get_conn()
    conn.autocommit = True
    cursor = conn.cursor()
    
    # Enable roaring
    cursor.execute("CREATE EXTENSION IF NOT EXISTS pg_roaring_index;")
    
    # Drop existing
    try:
        cursor.execute("DROP INDEX idx_10m_btree;")
    except Exception:
        pass
    try:
        cursor.execute("DROP INDEX idx_10m_roaring;")
    except Exception:
        pass
    
    # We will build BTree first
    print("Building BTree...")
    t0 = time.time()
    cursor.execute(f"CREATE INDEX idx_10m_btree ON {TABLE_NAME}({COLUMNS}) INCLUDE ({INCLUDE});")
    btree_time = time.time() - t0
    
    cursor.execute("SELECT pg_relation_size('idx_10m_btree');")
    btree_size = cursor.fetchone()[0]
    
    print("Building Roaring Exact...")
    cursor.execute("SET maintenance_work_mem = '1GB';")
    t0 = time.time()
    cursor.execute(f"CREATE INDEX idx_10m_roaring ON {TABLE_NAME} USING roaring({COLUMNS}) INCLUDE ({INCLUDE});")
    roaring_time = time.time() - t0
    
    cursor.execute("SELECT pg_relation_size('idx_10m_roaring');")
    roaring_size = cursor.fetchone()[0]
    
    cursor.execute(f"VACUUM ANALYZE {TABLE_NAME};")
    conn.close()
    
    return {
        "BTree": {"build_time": btree_time, "size": btree_size},
        "Roaring": {"build_time": roaring_time, "size": roaring_size}
    }

def execute_bench(index_name, use_roaring, concurrent_write=False):
    conn = get_conn()
    cursor = conn.cursor()
    
    # Force usage of specific index
    cursor.execute("SET enable_seqscan = off;")
    if use_roaring:
        cursor.execute("SET enable_indexscan = on;")
        cursor.execute("SET enable_bitmapscan = on;")
    else:
        cursor.execute("SET enable_indexscan = on;")
        cursor.execute("SET enable_bitmapscan = off;") # BTree use index only scan mostly
        
    results = {}
    
    stop_insert = False
    insert_thread = None
    if concurrent_write:
        print("Starting concurrent writer thread...")
        def writer():
            w_conn = get_conn()
            w_conn.autocommit = True
            w_cursor = w_conn.cursor()
            while not stop_insert:
                # Insert a dummy row 
                w_cursor.execute(f"INSERT INTO {TABLE_NAME} (userid, os, sex, javascriptenable, regionid, advengineid, isoldcounter, dontcounthits, goodevent, age) VALUES (999999, 2, 2, 1, 225, 2, 1, 0, 1, 30);")
            w_conn.close()
        insert_thread = threading.Thread(target=writer)
        insert_thread.start()
        time.sleep(1) # Let writer spin up
    
    for q in QUERIES:
        print(f"[{index_name}] Executing {q['name']}...")
        best_time = float('inf')
        result_val = None
        for _ in range(3):
            t0 = time.time()
            cursor.execute(q['query'])
            res = cursor.fetchone()[0]
            dur = time.time() - t0
            if dur < best_time:
                best_time = dur
            result_val = res
            
        results[q['name']] = {
            "time_ms": best_time * 1000.0,
            "result": result_val
        }
        
    if concurrent_write:
        stop_insert = True
        insert_thread.join()
        
    conn.close()
    return results

def run_seqscan_baseline():
    conn = get_conn()
    cursor = conn.cursor()
    cursor.execute("SET enable_seqscan = on;")
    cursor.execute("SET enable_indexscan = off;")
    cursor.execute("SET enable_bitmapscan = off;")
    
    results = {}
    for q in QUERIES:
        print(f"[SeqScan] Executing {q['name']}...")
        cursor.execute(q['query'])
        res = cursor.fetchone()[0]
        results[q['name']] = res
        
    conn.close()
    return results

def generate_html(data):
    # Just a simple hardcoded HTML for now to avoid dealing with jinja2 envs
    html = """
    <html><head><title>10M Benchmark</title></head>
    <body style="font-family: sans-serif;">
    <h1>Roaring Index 10M Row Benchmark</h1>
    <h2>Build Stats</h2>
    <table border="1" cellpadding="5">
      <tr><th>Index</th><th>Build Time (s)</th><th>Size (MB)</th></tr>
      <tr><td>BTree</td><td>{btree_build:.2f}</td><td>{btree_size:.2f}</td></tr>
      <tr><td>Roaring</td><td>{roaring_build:.2f}</td><td>{roaring_size:.2f}</td></tr>
    </table>
    
    <h2>Query Performance (ms)</h2>
    <table border="1" cellpadding="5">
      <tr><th>Query</th><th>BTree</th><th>Roaring</th><th>Roaring (Concurrent Write)</th></tr>
    """
    
    for q in QUERIES:
        name = q['name']
        bt = data['BTree'][name]['time_ms']
        r = data['Roaring'][name]['time_ms']
        rc = data['Roaring_Concurrent'][name]['time_ms']
        html += f"<tr><td>{name}</td><td>{bt:.2f}</td><td>{r:.2f}</td><td>{rc:.2f}</td></tr>"
        
    html += """
    </table>
    </body></html>
    """
    
    html = html.format(
        btree_build=data['stats']['BTree']['build_time'],
        btree_size=data['stats']['BTree']['size'] / (1024*1024),
        roaring_build=data['stats']['Roaring']['build_time'],
        roaring_size=data['stats']['Roaring']['size'] / (1024*1024)
    )
    
    with open('bench_10m_dashboard.html', 'w') as f:
        f.write(html)
    print("Dashboard written to bench_10m_dashboard.html")

def main():
    print("=== Gathering SeqScan Baselines ===")
    baselines = run_seqscan_baseline()
    
    print("=== Building Indexes ===")
    stats = setup_indexes()
    
    all_results = {"stats": stats}
    
    print("=== BTree Tests ===")
    all_results["BTree"] = execute_bench("BTree", use_roaring=False, concurrent_write=False)
    
    print("=== Roaring Exact Tests ===")
    # Temporarily drop BTree to ensure Roaring is used
    conn = get_conn()
    conn.autocommit = True
    try:
        conn.cursor().execute("DROP INDEX idx_10m_btree;")
    except Exception:
        pass
    conn.close()
    
    all_results["Roaring"] = execute_bench("Roaring", use_roaring=True, concurrent_write=False)
    
    print("=== Roaring Exact (Concurrent Write) ===")
    all_results["Roaring_Concurrent"] = execute_bench("Roaring (Concurrent)", use_roaring=True, concurrent_write=True)
    
    print("=== Asserting Correctness ===")
    for q in QUERIES:
        name = q['name']
        baseline = baselines[name]
        bt_res = all_results['BTree'][name]['result']
        r_res = all_results['Roaring'][name]['result']
        # Roaring concurrent will have different results due to inserts, so we don't assert it strictly
        
        if baseline != bt_res:
            print(f"BUG! BTree mismatch on {name}: SeqScan={baseline}, BTree={bt_res}")
        if baseline != r_res:
            print(f"BUG! Roaring mismatch on {name}: SeqScan={baseline}, Roaring={r_res}")
        
        if baseline == bt_res and baseline == r_res:
            print(f"✓ {name} Correctness Passed!")

    generate_html(all_results)
    
if __name__ == '__main__':
    main()
