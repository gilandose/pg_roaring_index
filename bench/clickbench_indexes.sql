CREATE EXTENSION IF NOT EXISTS pg_roaring_index;

-- Create copies
DROP TABLE IF EXISTS hits_btree;
CREATE TABLE hits_btree AS SELECT * FROM hits;

DROP TABLE IF EXISTS hits_brin;
CREATE TABLE hits_brin AS SELECT * FROM hits;

DROP TABLE IF EXISTS hits_roaring;
CREATE TABLE hits_roaring AS SELECT * FROM hits;

-- Low-cardinality columns
-- Using double quotes to match case exactly just in case, or lowercase since unquoted creates lowercase in postgres
CREATE INDEX idx_hits_bt_region ON hits_btree("RegionID");
CREATE INDEX idx_hits_bt_os ON hits_btree("OS");
CREATE INDEX idx_hits_bt_ua ON hits_btree("UserAgent");

CREATE INDEX idx_hits_brin_region ON hits_brin USING brin("RegionID");
CREATE INDEX idx_hits_brin_os ON hits_brin USING brin("OS");
CREATE INDEX idx_hits_brin_ua ON hits_brin USING brin("UserAgent");

CREATE INDEX idx_hits_rr_region ON hits_roaring USING roaring_lossy("RegionID");
CREATE INDEX idx_hits_rr_os ON hits_roaring USING roaring_lossy("OS");
CREATE INDEX idx_hits_rr_ua ON hits_roaring USING roaring_lossy("UserAgent");

VACUUM ANALYZE hits_btree;
VACUUM ANALYZE hits_brin;
VACUUM ANALYZE hits_roaring;
