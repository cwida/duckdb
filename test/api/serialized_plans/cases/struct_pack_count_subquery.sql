CREATE TABLE t (x BIGINT, y VARCHAR, b BOOLEAN);
CREATE TABLE o (k BIGINT);
INSERT INTO t VALUES (1, 'a', true), (2, 'b', false), (3, 'c', NULL);
INSERT INTO o VALUES (1), (1), (3);
SELECT x, (SELECT {'y': t.y, 'n': count(*), 'b': t.b} FROM o WHERE o.k = t.x) AS s FROM t ORDER BY x;
