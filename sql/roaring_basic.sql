-- Basic smoke test: extension loads and AM registers.
CREATE EXTENSION pg_roaring_index;

SELECT amname FROM pg_am WHERE amname = 'roaring';
