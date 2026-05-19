-- bench/teardown.sql — drop all benchmark tables and sequences.
DROP TABLE IF EXISTS bench_btree   CASCADE;
DROP TABLE IF EXISTS bench_gin     CASCADE;
DROP TABLE IF EXISTS bench_brin    CASCADE;
DROP TABLE IF EXISTS bench_roaring CASCADE;
DROP SEQUENCE IF EXISTS bench_churn_seq;
\echo 'Teardown complete.'
