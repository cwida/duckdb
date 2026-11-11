SELECT NULL
FROM
    t AS ref_2,
    (SELECT (SELECT NULL
         FROM (FROM t AS ref_5,
              (SELECT ref_2.n_name AS c1))));
