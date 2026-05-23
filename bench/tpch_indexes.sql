CREATE EXTENSION IF NOT EXISTS pg_roaring_index;

-- Create copies for different indexing strategies
DROP TABLE IF EXISTS lineitem_btree;
CREATE TABLE lineitem_btree AS SELECT * FROM lineitem;

DROP TABLE IF EXISTS lineitem_comp;
CREATE TABLE lineitem_comp AS SELECT * FROM lineitem;

DROP TABLE IF EXISTS lineitem_roaring;
CREATE TABLE lineitem_roaring AS SELECT * FROM lineitem;

-- B-Tree (Single columns)
CREATE INDEX idx_bt_returnflag ON lineitem_btree(l_returnflag);
CREATE INDEX idx_bt_linestatus ON lineitem_btree(l_linestatus);
CREATE INDEX idx_bt_shipmode ON lineitem_btree(l_shipmode);

-- Composite B-Tree
CREATE INDEX idx_comp_return_status_ship ON lineitem_comp(l_returnflag, l_linestatus, l_shipmode);

-- Roaring lossy (which is optimal for this according to README)
CREATE INDEX idx_rr_returnflag ON lineitem_roaring USING roaring_lossy(l_returnflag);
CREATE INDEX idx_rr_linestatus ON lineitem_roaring USING roaring_lossy(l_linestatus);
CREATE INDEX idx_rr_shipmode ON lineitem_roaring USING roaring_lossy(l_shipmode);

VACUUM ANALYZE lineitem_btree;
VACUUM ANALYZE lineitem_comp;
VACUUM ANALYZE lineitem_roaring;
