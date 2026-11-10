CREATE TABLE tbl (col STRUCT(i INTEGER, k VARCHAR));
INSERT INTO tbl VALUES ({'i': 1, 'k': 'a'}), ({'i': 2, 'k': 'b'});
SELECT struct_update(col, i := col.i * 10) FROM tbl ORDER BY col.i;
