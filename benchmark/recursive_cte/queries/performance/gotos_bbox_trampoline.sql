WITH RECURSIVE
__m_b0 AS (
        SELECT __m_v0, (p_start IS NULL) AS __m_cond
        FROM (
                SELECT *, __m_v0 AS p_start
                FROM (
                        SELECT CAST(CAST(({'x': 1, 'y': 19}::vec2) AS STRUCT(x INTEGER, y INTEGER)) AS STRUCT(x INTEGER, y INTEGER)) AS __m_v0
                    ) AS __m_sc0
            ) AS __m_t
    ),
__m_b2 AS (
        SELECT __m_v0, __m_v1, __m_v2, __m_v3, __m_v4
        FROM (
                SELECT
                    __m_v0,
                    __m_v1,
                    __m_v2,
                    __m_v3,
                    CAST(p_start AS STRUCT(x INTEGER, y INTEGER)) AS __m_v4
                FROM (
                        SELECT
                            *,
                            __m_v0 AS p_start,
                            __m_v1 AS "track?",
                            __m_v2 AS goal,
                            __m_v3 AS bbox
                        FROM (
                                SELECT
                                    __m_v0,
                                    __m_v1,
                                    __m_v2,
                                    CAST(CAST(NULL AS box) AS STRUCT(lx INTEGER, ly INTEGER, hx INTEGER, hy INTEGER)) AS __m_v3
                                FROM (
                                        SELECT
                                            *,
                                            __m_v0 AS p_start,
                                            __m_v1 AS "track?",
                                            __m_v2 AS goal
                                        FROM (
                                                SELECT
                                                    __m_v0,
                                                    __m_v1,
                                                    CAST(CAST(NULL AS vec2) AS STRUCT(x INTEGER, y INTEGER)) AS __m_v2
                                                FROM (
                                                        SELECT
                                                            *,
                                                            __m_v0 AS p_start,
                                                            __m_v1 AS "track?"
                                                        FROM (
                                                                SELECT
                                                                    __m_v0,
                                                                    CAST(FALSE AS BOOLEAN) AS __m_v1
                                                                FROM (
                                                                        SELECT
                                                                            *,
                                                                            __m_v0 AS p_start
                                                                        FROM (
                                                                                SELECT __m_v0
                                                                                FROM __m_b0
                                                                                WHERE (__m_cond IS DISTINCT
                                                                                        FROM TRUE
                                                                                    )
                                                                            ) AS __m_sc1
                                                                    ) AS __m_s0
                                                            ) AS __m_sc2
                                                    ) AS __m_s1
                                            ) AS __m_sc3
                                    ) AS __m_s2
                            ) AS __m_sc4
                    ) AS __m_s3
            ) AS __m_t
    ),
__m_b1 AS (
        SELECT 0 AS __m_unit
        FROM (
                SELECT 0 AS __m_unit
                FROM __m_b0
                WHERE (__m_cond IS NOT DISTINCT
                        FROM TRUE
                    )
            ) AS __m_t
    ),
__m_machine(__m_lbl, __m_v0, __m_v1, __m_v2, __m_v3, __m_v4, __m_ret) USING TRAMPOLINE (__m_lbl) AS (
(
                SELECT
                    1 AS __m_lbl,
                    __m_v0 AS __m_v0,
                    __m_v1 AS __m_v1,
                    __m_v2 AS __m_v2,
                    __m_v3 AS __m_v3,
                    __m_v4 AS __m_v4,
                    CAST(NULL AS STRUCT(lx INTEGER, ly INTEGER, hx INTEGER, hy INTEGER)) AS __m_ret
                FROM __m_b2
            )
BRANCH 1: (
                WITH __m_b3 AS (
                        SELECT
                            __m_v0,
                            __m_v1,
                            __m_v2,
                            __m_v3,
                            __m_v4,
                            ("track?" AND ("current" = goal)) AS __m_cond
                        FROM (
                                SELECT
                                    *,
                                    __m_v0 AS p_start,
                                    __m_v1 AS "track?",
                                    __m_v2 AS goal,
                                    __m_v3 AS bbox,
                                    __m_v4 AS "current"
                                FROM (
                                        SELECT
                                            __m_v0,
                                            __m_v1,
                                            __m_v2,
                                            __m_v3,
                                            __m_v4
                                        FROM __m_machine
                                        WHERE (__m_lbl = 1)
                                    ) AS __m_sc5
                            ) AS __m_t
                    ),
                    __m_b6 AS (
                        SELECT __m_v0, __m_v1, __m_v2, __m_v3, __m_v4
                        FROM (
                                SELECT
                                    __m_v0,
                                    __m_v1,
                                    __m_v2,
                                    __m_v3,
                                    __m_v4
                                FROM __m_b3
                                WHERE (__m_cond IS DISTINCT
                                        FROM TRUE
                                    )
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
                            ((((NOT "track?") AND dir."track?"))) AS __m_cond
                        FROM (
                                SELECT
                                    *,
                                    __m_v0 AS p_start,
                                    __m_v1 AS "track?",
                                    __m_v2 AS goal,
                                    __m_v3 AS bbox,
                                    __m_v4 AS "current",
                                    __m_v5 AS square,
                                    __m_v6 AS dir
                                FROM (
                                        SELECT
                                            __m_v0,
                                            __m_v1,
                                            __m_v2,
                                            __m_v3,
                                            __m_v4,
                                            __m_v5,
                                            CAST((
                                                        SELECT d
                                                        FROM directions AS d
                                                        WHERE ((square.ll = d.ll) AND (square.lr = d.lr) AND (square.ul = d.ul) AND (square.ur = d.ur))
                                                    ) AS STRUCT(ll BOOLEAN, lr BOOLEAN, ul BOOLEAN, ur BOOLEAN, dir STRUCT(
                                                        x INTEGER,
                                                        y INTEGER), "track?" BOOLEAN)) AS __m_v6
                                        FROM (
                                                SELECT
                                                    *,
                                                    __m_v0 AS p_start,
                                                    __m_v1 AS "track?",
                                                    __m_v2 AS goal,
                                                    __m_v3 AS bbox,
                                                    __m_v4 AS "current",
                                                    __m_v5 AS square
                                                FROM (
                                                        SELECT
                                                            __m_v0,
                                                            __m_v1,
                                                            __m_v2,
                                                            __m_v3,
                                                            __m_v4,
                                                            CAST((
                                                                        SELECT s
                                                                        FROM squares AS s
                                                                        WHERE (s.xy = "current")
                                                                    ) AS STRUCT(xy STRUCT(
                                                                        x INTEGER,
                                                                        y INTEGER), ll BOOLEAN, lr BOOLEAN, ul BOOLEAN, ur BOOLEAN)) AS __m_v5
                                                        FROM (
                                                                SELECT
                                                                    *,
                                                                    __m_v0 AS p_start,
                                                                    __m_v1 AS "track?",
                                                                    __m_v2 AS goal,
                                                                    __m_v3 AS bbox,
                                                                    __m_v4 AS "current"
                                                                FROM (
                                                                        SELECT
                                                                            __m_v0,
                                                                            __m_v1,
                                                                            __m_v2,
                                                                            __m_v3,
                                                                            __m_v4
                                                                        FROM __m_b6
                                                                    ) AS __m_sc6
                                                            ) AS __m_s0
                                                    ) AS __m_sc7
                                            ) AS __m_s1
                                    ) AS __m_sc8
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
                                SELECT *
                                FROM (
                                        SELECT
                                            __m_v0,
                                            __m_v1,
                                            __m_v2,
                                            CAST(CAST(box_pt(goal.x, goal.y) AS box) AS STRUCT(lx INTEGER, ly INTEGER, hx INTEGER, hy INTEGER)) AS __m_v3,
                                            __m_v4,
                                            __m_v5,
                                            __m_v6
                                        FROM (
                                                SELECT
                                                    *,
                                                    __m_v0 AS p_start,
                                                    __m_v1 AS "track?",
                                                    __m_v2 AS goal,
                                                    __m_v3 AS bbox,
                                                    __m_v4 AS "current",
                                                    __m_v5 AS square,
                                                    __m_v6 AS dir
                                                FROM (
                                                        SELECT
                                                            __m_v0,
                                                            CAST(TRUE AS BOOLEAN) AS __m_v1,
                                                            CAST(CAST("current" AS vec2) AS STRUCT(x INTEGER, y INTEGER)) AS __m_v2,
                                                            __m_v3,
                                                            __m_v4,
                                                            __m_v5,
                                                            __m_v6
                                                        FROM (
                                                                SELECT
                                                                    *,
                                                                    __m_v0 AS p_start,
                                                                    __m_v3 AS bbox,
                                                                    __m_v4 AS "current",
                                                                    __m_v5 AS square,
                                                                    __m_v6 AS dir
                                                                FROM (
                                                                        SELECT
                                                                            __m_v0,
                                                                            __m_v3,
                                                                            __m_v4,
                                                                            __m_v5,
                                                                            __m_v6
                                                                        FROM __m_b4
                                                                        WHERE (__m_cond IS NOT DISTINCT
                                                                                FROM TRUE
                                                                            )
                                                                    ) AS __m_sc9
                                                            ) AS __m_s0
                                                    ) AS __m_sc10
                                            ) AS __m_s1
                                    ) AS __m_s2
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
                            __m_v6
                        FROM (
                                SELECT *
                                FROM (
                                        SELECT
                                            __m_v0,
                                            __m_v1,
                                            __m_v2,
                                            CAST(
                                                CASE
                                                    WHEN ("track?") THEN (CAST(CAST(bound_box(bbox, box_pt("current".x, "current".y)) AS box) AS STRUCT(lx INTEGER, ly INTEGER, hx INTEGER, hy INTEGER)))
                                                    ELSE bbox
                                                END AS STRUCT(
                                                    lx INTEGER,
                                                    ly INTEGER,
                                                    hx INTEGER,
                                                    hy INTEGER)) AS __m_v3,
                                            __m_v4,
                                            __m_v5,
                                            __m_v6
                                        FROM (
                                                SELECT
                                                    *,
                                                    __m_v0 AS p_start,
                                                    __m_v1 AS "track?",
                                                    __m_v2 AS goal,
                                                    __m_v3 AS bbox,
                                                    __m_v4 AS "current",
                                                    __m_v5 AS square,
                                                    __m_v6 AS dir
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
                                                    ) AS __m_sc11
                                            ) AS __m_s0
                                    ) AS __m_s1
                                WHERE (TRUE)
                            ) AS __m_t
                    ),
                    __m_b10 AS (
                        SELECT __m_v0, __m_v1, __m_v2, __m_v3, __m_v4
                        FROM (
                                SELECT
                                    __m_v0,
                                    __m_v1,
                                    __m_v2,
                                    __m_v3,
                                    CAST(CAST(struct_pack(x :=("current".x + dir.dir.x), y :=("current".y + dir.dir.y)) AS vec2) AS STRUCT(x INTEGER, y INTEGER)) AS __m_v4,
                                    __m_v6
                                FROM (
                                        SELECT
                                            *,
                                            __m_v0 AS p_start,
                                            __m_v1 AS "track?",
                                            __m_v2 AS goal,
                                            __m_v3 AS bbox,
                                            __m_v4 AS "current",
                                            __m_v6 AS dir
                                        FROM (
                                                SELECT
                                                    __m_v0,
                                                    __m_v1,
                                                    __m_v2,
                                                    __m_v3,
                                                    __m_v4,
                                                    __m_v6
                                                FROM __m_b7
                                            ) AS __m_sc12
                                    ) AS __m_s0
                                WHERE (TRUE)
                            ) AS __m_t
                    ),
                    __m_b5 AS (
                        SELECT __m_v3
                        FROM (
                                SELECT __m_v3
                                FROM __m_b3
                                WHERE (__m_cond IS NOT DISTINCT
                                        FROM TRUE
                                    )
                            ) AS __m_t
                    )(
                        SELECT
                            1 AS __m_lbl,
                            __m_v0 AS __m_v0,
                            __m_v1 AS __m_v1,
                            __m_v2 AS __m_v2,
                            __m_v3 AS __m_v3,
                            __m_v4 AS __m_v4,
                            CAST(NULL AS STRUCT(lx INTEGER, ly INTEGER, hx INTEGER, hy INTEGER)) AS __m_ret
                        FROM __m_b10
                    )
                UNION ALL (
                        SELECT
                            0 AS __m_lbl,
                            CAST(NULL AS STRUCT(x INTEGER, y INTEGER)) AS __m_v0,
                            CAST(NULL AS BOOLEAN) AS __m_v1,
                            CAST(NULL AS STRUCT(x INTEGER, y INTEGER)) AS __m_v2,
                            CAST(NULL AS STRUCT(lx INTEGER, ly INTEGER, hx INTEGER, hy INTEGER)) AS __m_v3,
                            CAST(NULL AS STRUCT(x INTEGER, y INTEGER)) AS __m_v4,
                            CAST(bbox AS STRUCT(lx INTEGER, ly INTEGER, hx INTEGER, hy INTEGER)) AS __m_ret
                        FROM (
                                SELECT *, __m_v3 AS bbox
                                FROM __m_b5
                            ) AS __m_sc13
                    )
            )
)

SELECT __m_ret::VARCHAR AS result
FROM ((
                SELECT CAST(NULL AS STRUCT(lx INTEGER, ly INTEGER, hx INTEGER, hy INTEGER)) AS __m_ret
                FROM __m_b1
            )
        UNION ALL (
                SELECT __m_ret
                FROM __m_machine
                WHERE (__m_lbl = 0)
            )
    ) AS __m_r;
