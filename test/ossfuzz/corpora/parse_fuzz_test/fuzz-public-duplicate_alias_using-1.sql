SELECT c1
FROM t1 AS same_alias JOIN t1 AS same_alias2 USING (c1),
     t1 AS same_alias JOIN t1 AS same_alias2 USING (c1);
