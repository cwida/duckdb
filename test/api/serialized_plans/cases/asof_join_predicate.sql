CREATE TABLE tt1 (i INTEGER, j VARCHAR);
INSERT INTO tt1 VALUES (2, 'A'), (4, 'B'), (5, 'A'), (6, '*');
CREATE TABLE tt2 (i INTEGER, j VARCHAR, k VARCHAR);
INSERT INTO tt2 VALUES (1, 'A', 'I'), (3, 'B', 'II');
SELECT tt1.i, tt2.k FROM tt1 ASOF JOIN tt2 ON tt1.i >= tt2.i AND (tt1.j = tt2.j OR tt1.j = '*') ORDER BY tt1.i;
