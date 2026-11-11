SELECT NULL
FROM
    t AS ref_2,
    (SELECT (SELECT NULL
         FROM (FROM t AS ref_5,
              (SELECT ref_5.ps_supplycost AS c0,
                      ref_2.n_name AS c1))));
