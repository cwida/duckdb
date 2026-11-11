SELECT
    (
        SELECT
            (
                SELECT (SELECT ref_2.i)
                FROM mytable AS ref_2
            )
        WHERE
            (
                SELECT subq_0.i AS c1
            )
    )
FROM mytable AS subq_0;
