CREATE TABLE nested_struct(cik BIGINT, entityName VARCHAR, a STRUCT(b STRUCT(c INT, d INT), c STRUCT(e INT, f INT)));
INSERT INTO nested_struct VALUES (42, 'entity', {'b': {'c': 42, 'd': 43}, 'c': {'e': 44, 'f': 45}}), (142, 'entity2', {'b': {'c': 142, 'd': 143}, 'c': {'e': 144, 'f': 145}});
SELECT unnest(a, recursive := true) FROM nested_struct ORDER BY a.b.c LIMIT 1;
