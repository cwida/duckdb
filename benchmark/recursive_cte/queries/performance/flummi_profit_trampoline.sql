WITH RECURSIVE
__m_b0 AS (
        SELECT __m_v1, __m_v2, __m_v3
        FROM (
                SELECT
                    __m_v0,
                    __m_v1,
                    __m_v2,
                    CAST(startDate0 AS DATE) AS __m_v3
                FROM (
                        SELECT
                            *,
                            __m_v0 AS startDate0,
                            __m_v1 AS endDate,
                            __m_v2 AS givenStore
                        FROM (
                                SELECT
                                    CAST(CAST((DATE '2001-12-29') AS DATE) AS DATE) AS __m_v0,
                                    CAST(CAST((DATE '2001-12-29' + 8000) AS DATE) AS DATE) AS __m_v1,
                                    CAST(CAST((1) AS INTEGER) AS INTEGER) AS __m_v2
                            ) AS __m_sc0
                    ) AS __m_s0
            ) AS __m_t
    ),
__m_machine(__m_lbl, __m_v1, __m_v2, __m_v3, __m_o0) USING TRAMPOLINE (__m_lbl) AS (
(
                SELECT
                    1 AS __m_lbl,
                    __m_v1 AS __m_v1,
                    __m_v2 AS __m_v2,
                    __m_v3 AS __m_v3,
                    CAST(NULL AS STRUCT(dt DATE, profit DECIMAL(15, 2))) AS __m_o0
                FROM __m_b0
            )
BRANCH 1: (
                WITH __m_b1 AS (
                        SELECT
                            __m_v1,
                            __m_v2,
                            __m_v3,
                            (startDate > endDate) AS __m_cond
                        FROM (
                                SELECT
                                    *,
                                    __m_v1 AS endDate,
                                    __m_v2 AS givenStore,
                                    __m_v3 AS startDate
                                FROM (
                                        SELECT __m_v1, __m_v2, __m_v3
                                        FROM __m_machine
                                        WHERE (__m_lbl = 1)
                                    ) AS __m_sc1
                            ) AS __m_t
                    ),
                    __m_b4 AS (
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
                        SELECT __m_v1, __m_v2, __m_v3, __m_v5
                        FROM (
                                SELECT
                                    __m_v1,
                                    __m_v2,
                                    __m_v3,
                                    __m_v4,
                                    CAST(CAST((
                                                        SELECT sum(ss_net_profit)
                                                        FROM store_sales
                                                        WHERE ((ss_sold_date_sk = dateSk) AND (ss_store_sk = givenStore))
                                                    ) AS DECIMAL(15, 2)) AS DECIMAL(15, 2)) AS __m_v5
                                FROM (
                                        SELECT
                                            *,
                                            __m_v1 AS endDate,
                                            __m_v2 AS givenStore,
                                            __m_v3 AS startDate,
                                            __m_v4 AS dateSk
                                        FROM (
                                                SELECT
                                                    __m_v1,
                                                    __m_v2,
                                                    __m_v3,
                                                    CAST(CAST((
                                                                        SELECT d_date_sk
                                                                        FROM date_dim
                                                                        WHERE (d_date = startDate)
                                                                    ) AS INTEGER) AS INTEGER) AS __m_v4
                                                FROM (
                                                        SELECT
                                                            *,
                                                            __m_v1 AS endDate,
                                                            __m_v2 AS givenStore,
                                                            __m_v3 AS startDate
                                                        FROM (
                                                                SELECT
                                                                    __m_v1,
                                                                    __m_v2,
                                                                    __m_v3
                                                                FROM __m_b4
                                                            ) AS __m_sc2
                                                    ) AS __m_s0
                                            ) AS __m_sc3
                                    ) AS __m_s1
                            ) AS __m_t
                    ),
                    __m_b5 AS (
                        SELECT __m_v1, __m_v2, __m_v3
                        FROM (
                                SELECT
                                    __m_v1,
                                    __m_v2,
                                    CAST((startDate + to_days(CAST(trunc(CAST(1 AS DOUBLE)) AS INTEGER))) AS DATE) AS __m_v3
                                FROM (
                                        SELECT
                                            *,
                                            __m_v1 AS endDate,
                                            __m_v2 AS givenStore,
                                            __m_v3 AS startDate
                                        FROM (
                                                SELECT __m_v1, __m_v2, __m_v3
                                                FROM __m_b2
                                            ) AS __m_sc5
                                    ) AS __m_s0
                                WHERE (TRUE)
                            ) AS __m_t
                    ),
                    __m_b3 AS (
                        SELECT 0 AS __m_unit
                        FROM (
                                SELECT 0 AS __m_unit
                                FROM __m_b1
                                WHERE (__m_cond IS NOT DISTINCT
                                        FROM TRUE
                                    )
                            ) AS __m_t
                    )(
                        SELECT
                            0 AS __m_lbl,
                            CAST(NULL AS DATE) AS __m_v1,
                            CAST(NULL AS INTEGER) AS __m_v2,
                            CAST(NULL AS DATE) AS __m_v3,
                            CAST(struct_pack(dt := startDate, profit := dayProfit) AS STRUCT(dt DATE, profit DECIMAL(15, 2))) AS __m_o0
                        FROM (
                                SELECT
                                    *,
                                    __m_v1 AS endDate,
                                    __m_v2 AS givenStore,
                                    __m_v3 AS startDate,
                                    __m_v5 AS dayProfit
                                FROM __m_b2
                            ) AS __m_sc4
                    )
                UNION ALL (
                        SELECT
                            1 AS __m_lbl,
                            __m_v1 AS __m_v1,
                            __m_v2 AS __m_v2,
                            __m_v3 AS __m_v3,
                            CAST(NULL AS STRUCT(dt DATE, profit DECIMAL(15, 2))) AS __m_o0
                        FROM __m_b5
                    )
            )
)

SELECT count(*) AS days, md5(string_agg(__m_o0::VARCHAR, chr(10) ORDER BY __m_o0::VARCHAR)) AS digest
FROM (
        SELECT __m_o0
        FROM __m_machine
        WHERE (__m_lbl = 0)
    ) AS __m_r;
