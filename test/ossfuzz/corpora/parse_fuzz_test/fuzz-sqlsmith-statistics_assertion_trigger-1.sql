SELECT NULL AS c3
FROM
  (SELECT NULL AS c0,
          NULL AS c1,
          NULL AS c2
   FROM main.all_types AS ref_0
   INNER JOIN main.all_types AS ref_1
   INNER JOIN main.all_types AS ref_2
   LEFT JOIN main.all_types AS ref_3 ON (ref_2."struct" = ref_3."struct") ON (ref_3."varchar") ON (ref_1."varchar")
   LEFT JOIN main.all_types AS ref_4
   INNER JOIN main.all_types AS ref_5 ON (NULL) ON (ref_0."double" = ref_5."double")) AS subq_0
WHERE EXISTS
    (SELECT NULL AS c0,
            NULL AS c1,
            NULL AS c2,
            NULL AS c3,
            NULL AS c4,
            NULL AS c5,
            NULL AS c6,
            NULL AS c7
     FROM
       (SELECT NULL AS c1) AS subq_1
     WHERE (
              (SELECT 'duck') ~~ CASE
                                     WHEN ((subq_0.c1 IS NOT NULL)) THEN (CASE
                                                                              WHEN ((
                                                                                       (SELECT 'duck') ~~
                                                                                       (SELECT 'duck'))) THEN (
                                                                                                                 (SELECT 'duck'))
                                                                              ELSE
                                                                                     (SELECT 'duck')
                                                                          END)
                                     ELSE CASE
                                              WHEN ((
                                                       (SELECT 'duck') ~~
                                                       (SELECT 'duck'))) THEN (
                                                                                 (SELECT 'duck'))
                                              ELSE
                                                     (SELECT 'duck')
                                          END
                                 END));
