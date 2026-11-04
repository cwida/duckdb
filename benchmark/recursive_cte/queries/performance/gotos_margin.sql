WITH RECURSIVE __m_b0 AS (
        SELECT
            __m_v0,
            __m_v1,
            __m_v2,
            __m_v3,
            __m_v4,
            __m_v5,
            __m_v6
        FROM (
                SELECT
                    __m_v0,
                    __m_v1,
                    __m_v2,
                    __m_v3,
                    __m_v4,
                    __m_v5,
                    CAST((
                                SELECT struct_pack(
                                    orderkey := CAST(o.o_orderkey AS INTEGER),
                                    orderdate := o.o_orderdate)
                                FROM lineitem AS l, orders AS o
                                WHERE ((l.l_orderkey = o.o_orderkey) AND (l.l_partkey = partkey))
                                ORDER BY o.o_orderdate, o.o_orderkey
                                LIMIT 1
                            ) AS STRUCT(orderkey INTEGER, orderdate DATE)) AS __m_v6
                FROM (
                        SELECT
                            *,
                            __m_v0 AS partkey,
                            __m_v1 AS buy,
                            __m_v2 AS sell,
                            __m_v3 AS margin,
                            __m_v4 AS cheapest,
                            __m_v5 AS cheapest_order
                        FROM (
                                SELECT
                                    __m_v0,
                                    __m_v1,
                                    __m_v2,
                                    __m_v3,
                                    __m_v4,
                                    CAST(CAST(NULL AS INTEGER) AS INTEGER) AS __m_v5
                                FROM (
                                        SELECT
                                            *,
                                            __m_v0 AS partkey,
                                            __m_v1 AS buy,
                                            __m_v2 AS sell,
                                            __m_v3 AS margin,
                                            __m_v4 AS cheapest
                                        FROM (
                                                SELECT
                                                    __m_v0,
                                                    __m_v1,
                                                    __m_v2,
                                                    __m_v3,
                                                    CAST(CAST(NULL AS DECIMAL(15, 2)) AS DECIMAL(15, 2)) AS __m_v4
                                                FROM (
                                                        SELECT
                                                            *,
                                                            __m_v0 AS partkey,
                                                            __m_v1 AS buy,
                                                            __m_v2 AS sell,
                                                            __m_v3 AS margin
                                                        FROM (
                                                                SELECT
                                                                    __m_v0,
                                                                    __m_v1,
                                                                    __m_v2,
                                                                    CAST(CAST(NULL AS DECIMAL(15, 2)) AS DECIMAL(15, 2)) AS __m_v3
                                                                FROM (
                                                                        SELECT
                                                                            *,
                                                                            __m_v0 AS partkey,
                                                                            __m_v1 AS buy,
                                                                            __m_v2 AS sell
                                                                        FROM (
                                                                                SELECT
                                                                                    __m_v0,
                                                                                    __m_v1,
                                                                                    CAST(CAST(NULL AS INTEGER) AS INTEGER) AS __m_v2
                                                                                FROM (
                                                                                        SELECT
                                                                                            *,
                                                                                            __m_v0 AS partkey,
                                                                                            __m_v1 AS buy
                                                                                        FROM (
                                                                                                SELECT
                                                                                                    __m_v0,
                                                                                                    CAST(CAST(NULL AS INTEGER) AS INTEGER) AS __m_v1
                                                                                                FROM (
                                                                                                        SELECT
                                                                                                            *,
                                                                                                            __m_v0 AS partkey
                                                                                                        FROM (
                                                                                                                SELECT CAST(CAST((1) AS BIGINT) AS BIGINT) AS __m_v0
                                                                                                            ) AS __m_sc0
                                                                                                    ) AS __m_s0
                                                                                            ) AS __m_sc1
                                                                                    ) AS __m_s1
                                                                            ) AS __m_sc2
                                                                    ) AS __m_s2
                                                            ) AS __m_sc3
                                                    ) AS __m_s3
                                            ) AS __m_sc4
                                    ) AS __m_s4
                            ) AS __m_sc5
                    ) AS __m_s5
            ) AS __m_t
    ),
    __m_machine AS ((
                SELECT
                    (1) AS __m_lbl,
                    __m_v0 AS __m_v0,
                    __m_v1 AS __m_v1,
                    __m_v2 AS __m_v2,
                    __m_v3 AS __m_v3,
                    __m_v4 AS __m_v4,
                    __m_v5 AS __m_v5,
                    __m_v6 AS __m_v6,
                    CAST(NULL AS STRUCT(buy INTEGER, sell INTEGER, margin DECIMAL(15, 2))) AS __m_ret
                FROM __m_b0
            )
        UNION ALL (
                WITH __m_b1 AS (
                        SELECT
                            __m_v0,
                            __m_v1,
                            __m_v2,
                            __m_v3,
                            __m_v4,
                            __m_v5,
                            __m_v6,
                            (this_order IS NOT NULL) AS __m_cond
                        FROM (
                                SELECT
                                    *,
                                    __m_v0 AS partkey,
                                    __m_v1 AS buy,
                                    __m_v2 AS sell,
                                    __m_v3 AS margin,
                                    __m_v4 AS cheapest,
                                    __m_v5 AS cheapest_order,
                                    __m_v6 AS this_order
                                FROM (
                                        SELECT
                                            __m_v0,
                                            __m_v1,
                                            __m_v2,
                                            __m_v3,
                                            __m_v4,
                                            __m_v5,
                                            __m_v6
                                        FROM __m_machine
                                        WHERE (__m_lbl = 1)
                                    ) AS __m_sc6
                            ) AS __m_t
                    ),
                    __m_b3 AS (
                        SELECT __m_v1, __m_v2, __m_v3
                        FROM (
                                SELECT __m_v1, __m_v2, __m_v3
                                FROM __m_b1
                                WHERE (__m_cond IS DISTINCT
                                        FROM TRUE
                                    )
                            ) AS __m_t
                    ),
                    __m_b2 AS (
                        SELECT
                            __m_v0,
                            __m_v1,
                            __m_v2,
                            __m_v3,
                            __m_v4,
                            __m_v5,
                            __m_v6,
                            __m_v7,
                            ((price <= cheapest)) AS __m_cond
                        FROM (
                                SELECT
                                    *,
                                    __m_v0 AS partkey,
                                    __m_v1 AS buy,
                                    __m_v2 AS sell,
                                    __m_v3 AS margin,
                                    __m_v4 AS cheapest,
                                    __m_v5 AS cheapest_order,
                                    __m_v6 AS this_order,
                                    __m_v7 AS price
                                FROM (
                                        SELECT
                                            __m_v0,
                                            __m_v1,
                                            __m_v2,
                                            __m_v3,
                                            CAST(CAST(COALESCE(cheapest, price) AS DECIMAL(15, 2)) AS DECIMAL(15, 2)) AS __m_v4,
                                            __m_v5,
                                            __m_v6,
                                            __m_v7
                                        FROM (
                                                SELECT
                                                    *,
                                                    __m_v0 AS partkey,
                                                    __m_v1 AS buy,
                                                    __m_v2 AS sell,
                                                    __m_v3 AS margin,
                                                    __m_v4 AS cheapest,
                                                    __m_v5 AS cheapest_order,
                                                    __m_v6 AS this_order,
                                                    __m_v7 AS price
                                                FROM (
                                                        SELECT
                                                            __m_v0,
                                                            __m_v1,
                                                            __m_v2,
                                                            __m_v3,
                                                            __m_v4,
                                                            __m_v5,
                                                            __m_v6,
                                                            CAST(CAST((
                                                                                SELECT min(((l.l_extendedprice *(1 - l.l_discount)) *(1 + l.l_tax)))
                                                                                FROM lineitem AS l
                                                                                WHERE ((l.l_orderkey = this_order.orderkey) AND (l.l_partkey = partkey))
                                                                            ) AS DECIMAL(15, 2)) AS DECIMAL(15, 2)) AS __m_v7
                                                        FROM (
                                                                SELECT
                                                                    *,
                                                                    __m_v0 AS partkey,
                                                                    __m_v1 AS buy,
                                                                    __m_v2 AS sell,
                                                                    __m_v3 AS margin,
                                                                    __m_v4 AS cheapest,
                                                                    __m_v5 AS cheapest_order,
                                                                    __m_v6 AS this_order
                                                                FROM (
                                                                        SELECT
                                                                            __m_v0,
                                                                            __m_v1,
                                                                            __m_v2,
                                                                            __m_v3,
                                                                            __m_v4,
                                                                            __m_v5,
                                                                            __m_v6
                                                                        FROM __m_b1
                                                                        WHERE (__m_cond IS NOT DISTINCT
                                                                                FROM TRUE
                                                                            )
                                                                    ) AS __m_sc8
                                                            ) AS __m_s0
                                                    ) AS __m_sc9
                                            ) AS __m_s1
                                        WHERE (TRUE)
                                    ) AS __m_sc10
                            ) AS __m_t
                    ),
                    __m_b6 AS (
                        SELECT
                            __m_v0,
                            __m_v1,
                            __m_v2,
                            __m_v3,
                            __m_v4,
                            __m_v5,
                            __m_v6,
                            __m_v7
                        FROM (
                                SELECT
                                    __m_v0,
                                    __m_v1,
                                    __m_v2,
                                    __m_v3,
                                    __m_v4,
                                    __m_v5,
                                    __m_v6,
                                    __m_v7
                                FROM __m_b2
                                WHERE (__m_cond IS DISTINCT
                                        FROM TRUE
                                    )
                            ) AS __m_t
                    ),
                    __m_b5 AS (
                        SELECT
                            __m_v0,
                            __m_v1,
                            __m_v2,
                            __m_v3,
                            __m_v4,
                            __m_v5,
                            __m_v6,
                            __m_v7
                        FROM (
                                SELECT
                                    __m_v0,
                                    __m_v1,
                                    __m_v2,
                                    __m_v3,
                                    CAST(CAST(price AS DECIMAL(15, 2)) AS DECIMAL(15, 2)) AS __m_v4,
                                    CAST(CAST(this_order.orderkey AS INTEGER) AS INTEGER) AS __m_v5,
                                    __m_v6,
                                    __m_v7
                                FROM (
                                        SELECT
                                            *,
                                            __m_v0 AS partkey,
                                            __m_v1 AS buy,
                                            __m_v2 AS sell,
                                            __m_v3 AS margin,
                                            __m_v6 AS this_order,
                                            __m_v7 AS price
                                        FROM (
                                                SELECT
                                                    __m_v0,
                                                    __m_v1,
                                                    __m_v2,
                                                    __m_v3,
                                                    __m_v6,
                                                    __m_v7
                                                FROM __m_b2
                                                WHERE (__m_cond IS NOT DISTINCT
                                                        FROM TRUE
                                                    )
                                            ) AS __m_sc11
                                    ) AS __m_s0
                                WHERE (TRUE)
                            ) AS __m_t
                    ),
                    __m_b4 AS (
                        SELECT
                            __m_v0,
                            __m_v1,
                            __m_v2,
                            __m_v3,
                            __m_v4,
                            __m_v5,
                            __m_v6,
                            __m_v8,
                            (profit >= margin) AS __m_cond
                        FROM (
                                SELECT
                                    *,
                                    __m_v0 AS partkey,
                                    __m_v1 AS buy,
                                    __m_v2 AS sell,
                                    __m_v3 AS margin,
                                    __m_v4 AS cheapest,
                                    __m_v5 AS cheapest_order,
                                    __m_v6 AS this_order,
                                    __m_v7 AS price,
                                    __m_v8 AS profit
                                FROM (
                                        SELECT
                                            __m_v0,
                                            __m_v1,
                                            __m_v2,
                                            CAST(CAST(COALESCE(margin, profit) AS DECIMAL(15, 2)) AS DECIMAL(15, 2)) AS __m_v3,
                                            __m_v4,
                                            __m_v5,
                                            __m_v6,
                                            __m_v7,
                                            __m_v8
                                        FROM (
                                                SELECT
                                                    *,
                                                    __m_v0 AS partkey,
                                                    __m_v1 AS buy,
                                                    __m_v2 AS sell,
                                                    __m_v3 AS margin,
                                                    __m_v4 AS cheapest,
                                                    __m_v5 AS cheapest_order,
                                                    __m_v6 AS this_order,
                                                    __m_v7 AS price,
                                                    __m_v8 AS profit
                                                FROM (
                                                        SELECT
                                                            __m_v0,
                                                            __m_v1,
                                                            __m_v2,
                                                            __m_v3,
                                                            __m_v4,
                                                            __m_v5,
                                                            __m_v6,
                                                            __m_v7,
                                                            CAST(CAST((price - cheapest) AS DECIMAL(15, 2)) AS DECIMAL(15, 2)) AS __m_v8
                                                        FROM (
                                                                SELECT
                                                                    *,
                                                                    __m_v0 AS partkey,
                                                                    __m_v1 AS buy,
                                                                    __m_v2 AS sell,
                                                                    __m_v3 AS margin,
                                                                    __m_v4 AS cheapest,
                                                                    __m_v5 AS cheapest_order,
                                                                    __m_v6 AS this_order,
                                                                    __m_v7 AS price
                                                                FROM ((
                                                                                SELECT
                                                                                    __m_v0,
                                                                                    __m_v1,
                                                                                    __m_v2,
                                                                                    __m_v3,
                                                                                    __m_v4,
                                                                                    __m_v5,
                                                                                    __m_v6,
                                                                                    __m_v7
                                                                                FROM __m_b5
                                                                            )
                                                                        UNION ALL (
                                                                                SELECT
                                                                                    __m_v0,
                                                                                    __m_v1,
                                                                                    __m_v2,
                                                                                    __m_v3,
                                                                                    __m_v4,
                                                                                    __m_v5,
                                                                                    __m_v6,
                                                                                    __m_v7
                                                                                FROM __m_b6
                                                                            )
                                                                    ) AS __m_sc12
                                                            ) AS __m_s0
                                                        WHERE (TRUE)
                                                    ) AS __m_sc13
                                            ) AS __m_s1
                                        WHERE (TRUE)
                                    ) AS __m_sc14
                            ) AS __m_t
                    ),
                    __m_b9 AS (
                        SELECT
                            __m_v0,
                            __m_v1,
                            __m_v2,
                            __m_v3,
                            __m_v4,
                            __m_v5,
                            __m_v6
                        FROM (
                                SELECT
                                    __m_v0,
                                    __m_v1,
                                    __m_v2,
                                    __m_v3,
                                    __m_v4,
                                    __m_v5,
                                    __m_v6
                                FROM __m_b4
                                WHERE (__m_cond IS DISTINCT
                                        FROM TRUE
                                    )
                            ) AS __m_t
                    ),
                    __m_b8 AS (
                        SELECT
                            __m_v0,
                            __m_v1,
                            __m_v2,
                            __m_v3,
                            __m_v4,
                            __m_v5,
                            __m_v6
                        FROM (
                                SELECT
                                    __m_v0,
                                    CAST(CAST(cheapest_order AS INTEGER) AS INTEGER) AS __m_v1,
                                    CAST(CAST(this_order.orderkey AS INTEGER) AS INTEGER) AS __m_v2,
                                    CAST(CAST(profit AS DECIMAL(15, 2)) AS DECIMAL(15, 2)) AS __m_v3,
                                    __m_v4,
                                    __m_v5,
                                    __m_v6,
                                    __m_v8
                                FROM (
                                        SELECT
                                            *,
                                            __m_v0 AS partkey,
                                            __m_v4 AS cheapest,
                                            __m_v5 AS cheapest_order,
                                            __m_v6 AS this_order,
                                            __m_v8 AS profit
                                        FROM (
                                                SELECT
                                                    __m_v0,
                                                    __m_v4,
                                                    __m_v5,
                                                    __m_v6,
                                                    __m_v8
                                                FROM __m_b4
                                                WHERE (__m_cond IS NOT DISTINCT
                                                        FROM TRUE
                                                    )
                                            ) AS __m_sc15
                                    ) AS __m_s0
                                WHERE (TRUE)
                            ) AS __m_t
                    ),
                    __m_b7 AS (
                        SELECT
                            __m_v0,
                            __m_v1,
                            __m_v2,
                            __m_v3,
                            __m_v4,
                            __m_v5,
                            __m_v6
                        FROM (
                                SELECT
                                    __m_v0,
                                    __m_v1,
                                    __m_v2,
                                    __m_v3,
                                    __m_v4,
                                    __m_v5,
                                    CAST((
                                                SELECT struct_pack(
                                                    orderkey := CAST(o.o_orderkey AS INTEGER),
                                                    orderdate := o.o_orderdate)
                                                FROM lineitem AS l, orders AS o
                                                WHERE ((l.l_orderkey = o.o_orderkey) AND (l.l_partkey = partkey) AND (o.o_orderdate > this_order.orderdate))
                                                ORDER BY o.o_orderdate, o.o_orderkey
                                                LIMIT 1
                                            ) AS STRUCT(orderkey INTEGER, orderdate DATE)) AS __m_v6
                                FROM (
                                        SELECT
                                            *,
                                            __m_v0 AS partkey,
                                            __m_v1 AS buy,
                                            __m_v2 AS sell,
                                            __m_v3 AS margin,
                                            __m_v4 AS cheapest,
                                            __m_v5 AS cheapest_order,
                                            __m_v6 AS this_order
                                        FROM ((
                                                        SELECT
                                                            __m_v0,
                                                            __m_v1,
                                                            __m_v2,
                                                            __m_v3,
                                                            __m_v4,
                                                            __m_v5,
                                                            __m_v6
                                                        FROM __m_b8
                                                    )
                                                UNION ALL (
                                                        SELECT
                                                            __m_v0,
                                                            __m_v1,
                                                            __m_v2,
                                                            __m_v3,
                                                            __m_v4,
                                                            __m_v5,
                                                            __m_v6
                                                        FROM __m_b9
                                                    )
                                            ) AS __m_sc16
                                    ) AS __m_s0
                            ) AS __m_t
                    )(
                        SELECT
                            NULL AS __m_lbl,
                            CAST(NULL AS BIGINT) AS __m_v0,
                            CAST(NULL AS INTEGER) AS __m_v1,
                            CAST(NULL AS INTEGER) AS __m_v2,
                            CAST(NULL AS DECIMAL(15, 2)) AS __m_v3,
                            CAST(NULL AS DECIMAL(15, 2)) AS __m_v4,
                            CAST(NULL AS INTEGER) AS __m_v5,
                            CAST(NULL AS STRUCT(orderkey INTEGER, orderdate DATE)) AS __m_v6,
                            CAST(struct_pack(buy := buy, sell := sell, margin := margin) AS STRUCT(buy INTEGER, sell INTEGER, margin DECIMAL(15, 2))) AS __m_ret
                        FROM (
                                SELECT
                                    *,
                                    __m_v1 AS buy,
                                    __m_v2 AS sell,
                                    __m_v3 AS margin
                                FROM __m_b3
                            ) AS __m_sc7
                    )
                UNION ALL (
                        SELECT
                            (1) AS __m_lbl,
                            __m_v0 AS __m_v0,
                            __m_v1 AS __m_v1,
                            __m_v2 AS __m_v2,
                            __m_v3 AS __m_v3,
                            __m_v4 AS __m_v4,
                            __m_v5 AS __m_v5,
                            __m_v6 AS __m_v6,
                            CAST(NULL AS STRUCT(buy INTEGER, sell INTEGER, margin DECIMAL(15, 2))) AS __m_ret
                        FROM __m_b7
                    )
            )
    )
SELECT __m_ret::VARCHAR AS result
FROM (
        SELECT __m_ret
        FROM __m_machine
        WHERE (__m_lbl IS NULL)
    ) AS __m_r;
