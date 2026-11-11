SELECT NULL AS c1
FROM
    (SELECT NULL AS c2) AS subq_0
LEFT JOIN
    (SELECT DISTINCT
        ref_3."varchar" AS c7
    FROM all_types AS ref_3
    WHERE EXISTS(
        SELECT ref_3."varchar" AS c3)) AS subq_1
    ON ((subq_1.c7 ~~ (SELECT "varchar" FROM all_types)));
