-- Reduced Flummi ray tracer: compiled from flummi/ray_reduced.fl with the
-- pinned Flummi revision (see flummi/README.md for the regeneration
-- commands), final SELECT adapted to validate the image length and checksum.
WITH RECURSIVE
  "🔄"("#️⃣", "🏷️", "📊.1", "px", "py", "ox", "oy", "oz", "step", "att", "sray", "cr", "cg", "cb", "live_sum", "dx", "dy", "dz", "pixel") AS (
    (SELECT CAST((0) AS INTEGER) AS "#️⃣",
            CAST(('start.1') AS VARCHAR) AS "🏷️",
            CAST((NULL) AS VARCHAR) AS "📊.1",
            CAST((NULL) AS BIGINT) AS "px",
            CAST((NULL) AS BIGINT) AS "py",
            CAST((NULL) AS FLOAT) AS "ox",
            CAST((NULL) AS FLOAT) AS "oy",
            CAST((NULL) AS FLOAT) AS "oz",
            CAST((NULL) AS INTEGER) AS "step",
            CAST((NULL) AS FLOAT) AS "att",
            CAST((NULL) AS BOOLEAN) AS "sray",
            CAST((NULL) AS FLOAT) AS "cr",
            CAST((NULL) AS FLOAT) AS "cg",
            CAST((NULL) AS FLOAT) AS "cb",
            CAST((NULL) AS BIGINT) AS "live_sum",
            CAST((NULL) AS DOUBLE) AS "dx",
            CAST((NULL) AS DOUBLE) AS "dy",
            CAST((NULL) AS DOUBLE) AS "dz",
            CAST((NULL) AS VARCHAR) AS "pixel")
      UNION ALL
    (WITH
       "start.1"("#️⃣", "⚙️") AS (
         SELECT "🔄"."#️⃣" AS "#️⃣",
                NULL AS "⚙️"
         FROM   "🔄"
         WHERE  "🔄"."🏷️" IS NOT DISTINCT FROM 'start.1'
       ),
       "fork.1"("#️⃣", "⚙️", "px", "py") AS (
         SELECT "start.1"."#️⃣" AS "#️⃣",
                "start.1"."⚙️" AS "⚙️",
                CAST(("ℚ"."px") AS BIGINT) AS "px",
                CAST(("ℚ"."py") AS BIGINT) AS "py"
         FROM   "start.1",
                (SELECT x.i, y.i
                 FROM   range(96) AS x(i)
                 CROSS JOIN range(64) AS y(i)) AS "ℚ"("px", "py")
       ),
       "assignment.1"("#️⃣", "⚙️", "px", "py", "ox", "oy", "oz", "step", "att", "sray", "cr", "cg", "cb", "live_sum") AS (
         SELECT "fork.1"."#️⃣" AS "#️⃣",
                "fork.1"."⚙️" AS "⚙️",
                "fork.1"."px" AS "px",
                "fork.1"."py" AS "py",
                CAST((0::real) AS FLOAT) AS "ox",
                CAST((0::real) AS FLOAT) AS "oy",
                CAST((-4.5::real) AS FLOAT) AS "oz",
                CAST((0) AS INTEGER) AS "step",
                CAST((1::real) AS FLOAT) AS "att",
                CAST((false) AS BOOLEAN) AS "sray",
                CAST((0::real) AS FLOAT) AS "cr",
                CAST((0::real) AS FLOAT) AS "cg",
                CAST((0::real) AS FLOAT) AS "cb",
                CAST((0::bigint) AS BIGINT) AS "live_sum"
         FROM   "fork.1"
       ),
       "assignment.2"("#️⃣", "⚙️", "px", "py", "ox", "oy", "oz", "step", "att", "sray", "cr", "cg", "cb", "live_sum", "dx", "dy", "dz") AS (
         SELECT "assignment.1"."#️⃣" AS "#️⃣",
                "assignment.1"."⚙️" AS "⚙️",
                "assignment.1"."px" AS "px",
                "assignment.1"."py" AS "py",
                "assignment.1"."ox" AS "ox",
                "assignment.1"."oy" AS "oy",
                "assignment.1"."oz" AS "oz",
                "assignment.1"."step" AS "step",
                "assignment.1"."att" AS "att",
                "assignment.1"."sray" AS "sray",
                "assignment.1"."cr" AS "cr",
                "assignment.1"."cg" AS "cg",
                "assignment.1"."cb" AS "cb",
                "assignment.1"."live_sum" AS "live_sum",
                CAST(((((("assignment.1"."px") + 0.5) / 96 - 0.5) * 1.5)::real / sqrt((((("assignment.1"."px") + 0.5) / 96 - 0.5) * 1.5) ** 2 + ((0.5 - (("assignment.1"."py") + 0.5) / 64) * 1.0) ** 2 + 1)) AS DOUBLE) AS "dx",
                CAST((((0.5 - (("assignment.1"."py") + 0.5) / 64) * 1.0)::real / sqrt((((("assignment.1"."px") + 0.5) / 96 - 0.5) * 1.5) ** 2 + ((0.5 - (("assignment.1"."py") + 0.5) / 64) * 1.0) ** 2 + 1)) AS DOUBLE) AS "dy",
                CAST((1::real / sqrt((((("assignment.1"."px") + 0.5) / 96 - 0.5) * 1.5) ** 2 + ((0.5 - (("assignment.1"."py") + 0.5) / 64) * 1.0) ** 2 + 1)) AS DOUBLE) AS "dz"
         FROM   "assignment.1"
       ),
       "start.2"("#️⃣", "⚙️", "px", "py", "ox", "oy", "oz", "step", "att", "sray", "cr", "cg", "cb", "live_sum", "dx", "dy", "dz") AS (
         SELECT "🔄"."#️⃣" AS "#️⃣",
                NULL AS "⚙️",
                "🔄"."px" AS "px",
                "🔄"."py" AS "py",
                "🔄"."ox" AS "ox",
                "🔄"."oy" AS "oy",
                "🔄"."oz" AS "oz",
                "🔄"."step" AS "step",
                "🔄"."att" AS "att",
                "🔄"."sray" AS "sray",
                "🔄"."cr" AS "cr",
                "🔄"."cg" AS "cg",
                "🔄"."cb" AS "cb",
                "🔄"."live_sum" AS "live_sum",
                "🔄"."dx" AS "dx",
                "🔄"."dy" AS "dy",
                "🔄"."dz" AS "dz"
         FROM   "🔄"
         WHERE  "🔄"."🏷️" IS NOT DISTINCT FROM 'start.2'
       ),
       "merge.1"("#️⃣", "⚙️", "px", "py", "ox", "oy", "oz", "step", "att", "sray", "cr", "cg", "cb", "live_sum", "dx", "dy", "dz") AS (
         (SELECT "assignment.2"."#️⃣" AS "#️⃣",
                 "assignment.2"."⚙️" AS "⚙️",
                 "assignment.2"."px" AS "px",
                 "assignment.2"."py" AS "py",
                 "assignment.2"."ox" AS "ox",
                 "assignment.2"."oy" AS "oy",
                 "assignment.2"."oz" AS "oz",
                 "assignment.2"."step" AS "step",
                 "assignment.2"."att" AS "att",
                 "assignment.2"."sray" AS "sray",
                 "assignment.2"."cr" AS "cr",
                 "assignment.2"."cg" AS "cg",
                 "assignment.2"."cb" AS "cb",
                 "assignment.2"."live_sum" AS "live_sum",
                 "assignment.2"."dx" AS "dx",
                 "assignment.2"."dy" AS "dy",
                 "assignment.2"."dz" AS "dz"
          FROM   "assignment.2")
           UNION ALL
         (SELECT "start.2"."#️⃣" AS "#️⃣",
                 "start.2"."⚙️" AS "⚙️",
                 "start.2"."px" AS "px",
                 "start.2"."py" AS "py",
                 "start.2"."ox" AS "ox",
                 "start.2"."oy" AS "oy",
                 "start.2"."oz" AS "oz",
                 "start.2"."step" AS "step",
                 "start.2"."att" AS "att",
                 "start.2"."sray" AS "sray",
                 "start.2"."cr" AS "cr",
                 "start.2"."cg" AS "cg",
                 "start.2"."cb" AS "cb",
                 "start.2"."live_sum" AS "live_sum",
                 "start.2"."dx" AS "dx",
                 "start.2"."dy" AS "dy",
                 "start.2"."dz" AS "dz"
          FROM   "start.2")
       ),
       "assignment.3"("#️⃣", "⚙️", "px", "py", "ox", "oy", "oz", "step", "att", "sray", "cr", "cg", "cb", "live_sum", "dx", "dy", "dz") AS (
         SELECT "merge.1"."#️⃣" AS "#️⃣",
                "merge.1"."⚙️" AS "⚙️",
                "merge.1"."px" AS "px",
                "merge.1"."py" AS "py",
                "merge.1"."ox" AS "ox",
                "merge.1"."oy" AS "oy",
                "merge.1"."oz" AS "oz",
                "merge.1"."step" AS "step",
                "merge.1"."att" AS "att",
                "merge.1"."sray" AS "sray",
                "merge.1"."cr" AS "cr",
                "merge.1"."cg" AS "cg",
                "merge.1"."cb" AS "cb",
                CAST((("merge.1"."live_sum") + count(*) OVER ()) AS BIGINT) AS "live_sum",
                "merge.1"."dx" AS "dx",
                "merge.1"."dy" AS "dy",
                "merge.1"."dz" AS "dz"
         FROM   "merge.1"
       ),
       "fork.2"("#️⃣", "⚙️", "px", "py", "ox", "oy", "oz", "step", "att", "sray", "cr", "cg", "cb", "live_sum", "dx", "dy", "dz", "hit_id", "hit_t") AS (
         SELECT "assignment.3"."#️⃣" AS "#️⃣",
                "assignment.3"."⚙️" AS "⚙️",
                "assignment.3"."px" AS "px",
                "assignment.3"."py" AS "py",
                "assignment.3"."ox" AS "ox",
                "assignment.3"."oy" AS "oy",
                "assignment.3"."oz" AS "oz",
                "assignment.3"."step" AS "step",
                "assignment.3"."att" AS "att",
                "assignment.3"."sray" AS "sray",
                "assignment.3"."cr" AS "cr",
                "assignment.3"."cg" AS "cg",
                "assignment.3"."cb" AS "cb",
                "assignment.3"."live_sum" AS "live_sum",
                "assignment.3"."dx" AS "dx",
                "assignment.3"."dy" AS "dy",
                "assignment.3"."dz" AS "dz",
                CAST(("ℚ"."hit_id") AS INTEGER) AS "hit_id",
                CAST(("ℚ"."hit_t") AS DOUBLE) AS "hit_t"
         FROM   "assignment.3",
                LATERAL (SELECT coalesce(min_by(h.id, h.t), -1), coalesce(min(h.t), 0::real)
                         FROM (SELECT s.id,
                                      -((("assignment.3"."ox") - s.center_x) * ("assignment.3"."dx") + (("assignment.3"."oy") - s.center_y) * ("assignment.3"."dy") + (("assignment.3"."oz") - s.center_z) * ("assignment.3"."dz"))
                                      - sqrt(((("assignment.3"."ox") - s.center_x) * ("assignment.3"."dx") + (("assignment.3"."oy") - s.center_y) * ("assignment.3"."dy") + (("assignment.3"."oz") - s.center_z) * ("assignment.3"."dz")) ** 2
                                             - (((("assignment.3"."ox") - s.center_x) ** 2 + (("assignment.3"."oy") - s.center_y) ** 2 + (("assignment.3"."oz") - s.center_z) ** 2) - s.radius ** 2)) AS t
                               FROM spheres AS s
                               WHERE ((("assignment.3"."ox") - s.center_x) * ("assignment.3"."dx") + (("assignment.3"."oy") - s.center_y) * ("assignment.3"."dy") + (("assignment.3"."oz") - s.center_z) * ("assignment.3"."dz")) ** 2
                                     > ((("assignment.3"."ox") - s.center_x) ** 2 + (("assignment.3"."oy") - s.center_y) ** 2 + (("assignment.3"."oz") - s.center_z) ** 2) - s.radius ** 2) AS h(id, t)
                         WHERE h.t > 0.0001) AS "ℚ"("hit_id", "hit_t")
       ),
       "assignment.4"("#️⃣", "⚙️", "🔍", "px", "py", "ox", "oy", "oz", "step", "att", "sray", "cr", "cg", "cb", "live_sum", "dx", "dy", "dz", "hit_id", "hit_t") AS (
         SELECT "fork.2"."#️⃣" AS "#️⃣",
                "fork.2"."⚙️" AS "⚙️",
                CAST((("fork.2"."sray")) AS BOOLEAN) AS "🔍",
                "fork.2"."px" AS "px",
                "fork.2"."py" AS "py",
                "fork.2"."ox" AS "ox",
                "fork.2"."oy" AS "oy",
                "fork.2"."oz" AS "oz",
                "fork.2"."step" AS "step",
                "fork.2"."att" AS "att",
                "fork.2"."sray" AS "sray",
                "fork.2"."cr" AS "cr",
                "fork.2"."cg" AS "cg",
                "fork.2"."cb" AS "cb",
                "fork.2"."live_sum" AS "live_sum",
                "fork.2"."dx" AS "dx",
                "fork.2"."dy" AS "dy",
                "fork.2"."dz" AS "dz",
                "fork.2"."hit_id" AS "hit_id",
                "fork.2"."hit_t" AS "hit_t"
         FROM   "fork.2"
       ),
       "start.3"("#️⃣", "⚙️", "px", "py", "live_sum", "pixel") AS (
         SELECT "🔄"."#️⃣" AS "#️⃣",
                NULL AS "⚙️",
                "🔄"."px" AS "px",
                "🔄"."py" AS "py",
                "🔄"."live_sum" AS "live_sum",
                "🔄"."pixel" AS "pixel"
         FROM   "🔄"
         WHERE  "🔄"."🏷️" IS NOT DISTINCT FROM 'start.3'
       ),
       "issynced.1"("#️⃣", "⚙️", "🔍", "px", "py", "live_sum", "pixel") AS (
         SELECT "start.3"."#️⃣" AS "#️⃣",
                "start.3"."⚙️" AS "⚙️",
                NOT EXISTS ((SELECT NULL
                             FROM   "🔄"
                             WHERE  "🔄"."🏷️" <> 'start.3')) AS "🔍",
                "start.3"."px" AS "px",
                "start.3"."py" AS "py",
                "start.3"."live_sum" AS "live_sum",
                "start.3"."pixel" AS "pixel"
         FROM   "start.3"
       ),
       "where.1"("#️⃣", "⚙️", "px", "py", "cr", "cg", "cb", "live_sum", "hit_id") AS (
         SELECT "assignment.4"."#️⃣" AS "#️⃣",
                "assignment.4"."⚙️" AS "⚙️",
                "assignment.4"."px" AS "px",
                "assignment.4"."py" AS "py",
                "assignment.4"."cr" AS "cr",
                "assignment.4"."cg" AS "cg",
                "assignment.4"."cb" AS "cb",
                "assignment.4"."live_sum" AS "live_sum",
                "assignment.4"."hit_id" AS "hit_id"
         FROM   "assignment.4"
         WHERE  "assignment.4"."🔍" IS NOT DISTINCT FROM TRUE
       ),
       "assignment.5"("#️⃣", "⚙️", "px", "py", "cr", "cg", "cb", "live_sum") AS (
         SELECT "where.1"."#️⃣" AS "#️⃣",
                "where.1"."⚙️" AS "⚙️",
                "where.1"."px" AS "px",
                "where.1"."py" AS "py",
                CAST((("where.1"."cr") * CASE WHEN coalesce((SELECT s.material = 'l' FROM spheres AS s WHERE s.id = ("where.1"."hit_id")), false) THEN 1::real ELSE 0.25::real END) AS FLOAT) AS "cr",
                CAST((("where.1"."cg") * CASE WHEN coalesce((SELECT s.material = 'l' FROM spheres AS s WHERE s.id = ("where.1"."hit_id")), false) THEN 1::real ELSE 0.25::real END) AS FLOAT) AS "cg",
                CAST((("where.1"."cb") * CASE WHEN coalesce((SELECT s.material = 'l' FROM spheres AS s WHERE s.id = ("where.1"."hit_id")), false) THEN 1::real ELSE 0.25::real END) AS FLOAT) AS "cb",
                "where.1"."live_sum" AS "live_sum"
         FROM   "where.1"
       ),
       "where.10"("#️⃣", "⚙️", "px", "py", "live_sum", "pixel") AS (
         SELECT "issynced.1"."#️⃣" AS "#️⃣",
                "issynced.1"."⚙️" AS "⚙️",
                "issynced.1"."px" AS "px",
                "issynced.1"."py" AS "py",
                "issynced.1"."live_sum" AS "live_sum",
                "issynced.1"."pixel" AS "pixel"
         FROM   "issynced.1"
         WHERE  "issynced.1"."🔍" IS DISTINCT FROM TRUE
       ),
       "jump.4"("#️⃣", "🏷️", "px", "py", "ox", "oy", "oz", "step", "att", "sray", "cr", "cg", "cb", "live_sum", "dx", "dy", "dz", "pixel") AS (
         SELECT "where.10"."#️⃣" AS "#️⃣",
                'start.3' AS "🏷️",
                "where.10"."px" AS "px",
                "where.10"."py" AS "py",
                NULL AS "ox",
                NULL AS "oy",
                NULL AS "oz",
                NULL AS "step",
                NULL AS "att",
                NULL AS "sray",
                NULL AS "cr",
                NULL AS "cg",
                NULL AS "cb",
                "where.10"."live_sum" AS "live_sum",
                NULL AS "dx",
                NULL AS "dy",
                NULL AS "dz",
                "where.10"."pixel" AS "pixel"
         FROM   "where.10"
       ),
       "where.2"("#️⃣", "⚙️", "px", "py", "ox", "oy", "oz", "step", "att", "sray", "cr", "cg", "cb", "live_sum", "dx", "dy", "dz", "hit_id", "hit_t") AS (
         SELECT "assignment.4"."#️⃣" AS "#️⃣",
                "assignment.4"."⚙️" AS "⚙️",
                "assignment.4"."px" AS "px",
                "assignment.4"."py" AS "py",
                "assignment.4"."ox" AS "ox",
                "assignment.4"."oy" AS "oy",
                "assignment.4"."oz" AS "oz",
                "assignment.4"."step" AS "step",
                "assignment.4"."att" AS "att",
                "assignment.4"."sray" AS "sray",
                "assignment.4"."cr" AS "cr",
                "assignment.4"."cg" AS "cg",
                "assignment.4"."cb" AS "cb",
                "assignment.4"."live_sum" AS "live_sum",
                "assignment.4"."dx" AS "dx",
                "assignment.4"."dy" AS "dy",
                "assignment.4"."dz" AS "dz",
                "assignment.4"."hit_id" AS "hit_id",
                "assignment.4"."hit_t" AS "hit_t"
         FROM   "assignment.4"
         WHERE  "assignment.4"."🔍" IS DISTINCT FROM TRUE
       ),
       "assignment.6"("#️⃣", "⚙️", "🔍", "px", "py", "ox", "oy", "oz", "step", "att", "sray", "cr", "cg", "cb", "live_sum", "dx", "dy", "dz", "hit_id", "hit_t") AS (
         SELECT "where.2"."#️⃣" AS "#️⃣",
                "where.2"."⚙️" AS "⚙️",
                CAST((("where.2"."hit_id") < 0) AS BOOLEAN) AS "🔍",
                "where.2"."px" AS "px",
                "where.2"."py" AS "py",
                "where.2"."ox" AS "ox",
                "where.2"."oy" AS "oy",
                "where.2"."oz" AS "oz",
                "where.2"."step" AS "step",
                "where.2"."att" AS "att",
                "where.2"."sray" AS "sray",
                "where.2"."cr" AS "cr",
                "where.2"."cg" AS "cg",
                "where.2"."cb" AS "cb",
                "where.2"."live_sum" AS "live_sum",
                "where.2"."dx" AS "dx",
                "where.2"."dy" AS "dy",
                "where.2"."dz" AS "dz",
                "where.2"."hit_id" AS "hit_id",
                "where.2"."hit_t" AS "hit_t"
         FROM   "where.2"
       ),
       "where.3"("#️⃣", "⚙️", "px", "py", "att", "live_sum") AS (
         SELECT "assignment.6"."#️⃣" AS "#️⃣",
                "assignment.6"."⚙️" AS "⚙️",
                "assignment.6"."px" AS "px",
                "assignment.6"."py" AS "py",
                "assignment.6"."att" AS "att",
                "assignment.6"."live_sum" AS "live_sum"
         FROM   "assignment.6"
         WHERE  "assignment.6"."🔍" IS NOT DISTINCT FROM TRUE
       ),
       "assignment.7"("#️⃣", "⚙️", "px", "py", "cr", "cg", "cb", "live_sum") AS (
         SELECT "where.3"."#️⃣" AS "#️⃣",
                "where.3"."⚙️" AS "⚙️",
                "where.3"."px" AS "px",
                "where.3"."py" AS "py",
                CAST((("where.3"."att") * 0.05) AS FLOAT) AS "cr",
                CAST((("where.3"."att") * 0.05) AS FLOAT) AS "cg",
                CAST((("where.3"."att") * 0.10) AS FLOAT) AS "cb",
                "where.3"."live_sum" AS "live_sum"
         FROM   "where.3"
       ),
       "where.4"("#️⃣", "⚙️", "px", "py", "ox", "oy", "oz", "step", "att", "sray", "cr", "cg", "cb", "live_sum", "dx", "dy", "dz", "hit_id", "hit_t") AS (
         SELECT "assignment.6"."#️⃣" AS "#️⃣",
                "assignment.6"."⚙️" AS "⚙️",
                "assignment.6"."px" AS "px",
                "assignment.6"."py" AS "py",
                "assignment.6"."ox" AS "ox",
                "assignment.6"."oy" AS "oy",
                "assignment.6"."oz" AS "oz",
                "assignment.6"."step" AS "step",
                "assignment.6"."att" AS "att",
                "assignment.6"."sray" AS "sray",
                "assignment.6"."cr" AS "cr",
                "assignment.6"."cg" AS "cg",
                "assignment.6"."cb" AS "cb",
                "assignment.6"."live_sum" AS "live_sum",
                "assignment.6"."dx" AS "dx",
                "assignment.6"."dy" AS "dy",
                "assignment.6"."dz" AS "dz",
                "assignment.6"."hit_id" AS "hit_id",
                "assignment.6"."hit_t" AS "hit_t"
         FROM   "assignment.6"
         WHERE  "assignment.6"."🔍" IS DISTINCT FROM TRUE
       ),
       "fork.3"("#️⃣", "⚙️", "px", "py", "ox", "oy", "oz", "step", "att", "sray", "cr", "cg", "cb", "live_sum", "dx", "dy", "dz", "hit_t", "snx", "sny", "snz", "mat", "shade_r", "shade_g", "shade_b") AS (
         SELECT "where.4"."#️⃣" AS "#️⃣",
                "where.4"."⚙️" AS "⚙️",
                "where.4"."px" AS "px",
                "where.4"."py" AS "py",
                "where.4"."ox" AS "ox",
                "where.4"."oy" AS "oy",
                "where.4"."oz" AS "oz",
                "where.4"."step" AS "step",
                "where.4"."att" AS "att",
                "where.4"."sray" AS "sray",
                "where.4"."cr" AS "cr",
                "where.4"."cg" AS "cg",
                "where.4"."cb" AS "cb",
                "where.4"."live_sum" AS "live_sum",
                "where.4"."dx" AS "dx",
                "where.4"."dy" AS "dy",
                "where.4"."dz" AS "dz",
                "where.4"."hit_t" AS "hit_t",
                CAST(("ℚ"."snx") AS DOUBLE) AS "snx",
                CAST(("ℚ"."sny") AS DOUBLE) AS "sny",
                CAST(("ℚ"."snz") AS DOUBLE) AS "snz",
                CAST(("ℚ"."mat") AS VARCHAR) AS "mat",
                CAST(("ℚ"."shade_r") AS FLOAT) AS "shade_r",
                CAST(("ℚ"."shade_g") AS FLOAT) AS "shade_g",
                CAST(("ℚ"."shade_b") AS FLOAT) AS "shade_b"
         FROM   "where.4",
                LATERAL (SELECT (("where.4"."ox") + ("where.4"."dx") * ("where.4"."hit_t") - s.center_x) / s.radius,
                                (("where.4"."oy") + ("where.4"."dy") * ("where.4"."hit_t") - s.center_y) / s.radius,
                                (("where.4"."oz") + ("where.4"."dz") * ("where.4"."hit_t") - s.center_z) / s.radius,
                                s.material,
                                ("where.4"."att") * coalesce(s.r, 1::real),
                                ("where.4"."att") * coalesce(s.g, 1::real),
                                ("where.4"."att") * coalesce(s.b, 1::real)
                         FROM   spheres AS s
                         WHERE  s.id = ("where.4"."hit_id")) AS "ℚ"("snx", "sny", "snz", "mat", "shade_r", "shade_g", "shade_b")
       ),
       "assignment.8"("#️⃣", "⚙️", "🔍", "px", "py", "ox", "oy", "oz", "step", "att", "sray", "cr", "cg", "cb", "live_sum", "dx", "dy", "dz", "hit_t", "snx", "sny", "snz", "mat", "shade_r", "shade_g", "shade_b") AS (
         SELECT "fork.3"."#️⃣" AS "#️⃣",
                "fork.3"."⚙️" AS "⚙️",
                CAST((("fork.3"."mat") = 'l') AS BOOLEAN) AS "🔍",
                "fork.3"."px" AS "px",
                "fork.3"."py" AS "py",
                "fork.3"."ox" AS "ox",
                "fork.3"."oy" AS "oy",
                "fork.3"."oz" AS "oz",
                "fork.3"."step" AS "step",
                "fork.3"."att" AS "att",
                "fork.3"."sray" AS "sray",
                "fork.3"."cr" AS "cr",
                "fork.3"."cg" AS "cg",
                "fork.3"."cb" AS "cb",
                "fork.3"."live_sum" AS "live_sum",
                "fork.3"."dx" AS "dx",
                "fork.3"."dy" AS "dy",
                "fork.3"."dz" AS "dz",
                "fork.3"."hit_t" AS "hit_t",
                "fork.3"."snx" AS "snx",
                "fork.3"."sny" AS "sny",
                "fork.3"."snz" AS "snz",
                "fork.3"."mat" AS "mat",
                "fork.3"."shade_r" AS "shade_r",
                "fork.3"."shade_g" AS "shade_g",
                "fork.3"."shade_b" AS "shade_b"
         FROM   "fork.3"
       ),
       "where.5"("#️⃣", "⚙️", "px", "py", "live_sum", "shade_r", "shade_g", "shade_b") AS (
         SELECT "assignment.8"."#️⃣" AS "#️⃣",
                "assignment.8"."⚙️" AS "⚙️",
                "assignment.8"."px" AS "px",
                "assignment.8"."py" AS "py",
                "assignment.8"."live_sum" AS "live_sum",
                "assignment.8"."shade_r" AS "shade_r",
                "assignment.8"."shade_g" AS "shade_g",
                "assignment.8"."shade_b" AS "shade_b"
         FROM   "assignment.8"
         WHERE  "assignment.8"."🔍" IS NOT DISTINCT FROM TRUE
       ),
       "assignment.9"("#️⃣", "⚙️", "px", "py", "cr", "cg", "cb", "live_sum") AS (
         SELECT "where.5"."#️⃣" AS "#️⃣",
                "where.5"."⚙️" AS "⚙️",
                "where.5"."px" AS "px",
                "where.5"."py" AS "py",
                CAST((("where.5"."shade_r")) AS FLOAT) AS "cr",
                CAST((("where.5"."shade_g")) AS FLOAT) AS "cg",
                CAST((("where.5"."shade_b")) AS FLOAT) AS "cb",
                "where.5"."live_sum" AS "live_sum"
         FROM   "where.5"
       ),
       "merge.2"("#️⃣", "⚙️", "px", "py", "cr", "cg", "cb", "live_sum") AS (
         (SELECT "assignment.5"."#️⃣" AS "#️⃣",
                 "assignment.5"."⚙️" AS "⚙️",
                 "assignment.5"."px" AS "px",
                 "assignment.5"."py" AS "py",
                 "assignment.5"."cr" AS "cr",
                 "assignment.5"."cg" AS "cg",
                 "assignment.5"."cb" AS "cb",
                 "assignment.5"."live_sum" AS "live_sum"
          FROM   "assignment.5")
           UNION ALL
         (SELECT "assignment.7"."#️⃣" AS "#️⃣",
                 "assignment.7"."⚙️" AS "⚙️",
                 "assignment.7"."px" AS "px",
                 "assignment.7"."py" AS "py",
                 "assignment.7"."cr" AS "cr",
                 "assignment.7"."cg" AS "cg",
                 "assignment.7"."cb" AS "cb",
                 "assignment.7"."live_sum" AS "live_sum"
          FROM   "assignment.7")
           UNION ALL
         (SELECT "assignment.9"."#️⃣" AS "#️⃣",
                 "assignment.9"."⚙️" AS "⚙️",
                 "assignment.9"."px" AS "px",
                 "assignment.9"."py" AS "py",
                 "assignment.9"."cr" AS "cr",
                 "assignment.9"."cg" AS "cg",
                 "assignment.9"."cb" AS "cb",
                 "assignment.9"."live_sum" AS "live_sum"
          FROM   "assignment.9")
       ),
       "assignment.12"("#️⃣", "⚙️", "px", "py", "live_sum", "pixel") AS (
         SELECT "merge.2"."#️⃣" AS "#️⃣",
                "merge.2"."⚙️" AS "⚙️",
                "merge.2"."px" AS "px",
                "merge.2"."py" AS "py",
                "merge.2"."live_sum" AS "live_sum",
                CAST((lpad((least(("merge.2"."cr"), 1) * 255) :: int :: text, 3, ' ') || ' ' ||
                       lpad((least(("merge.2"."cg"), 1) * 255) :: int :: text, 3, ' ') || ' ' ||
                       lpad((least(("merge.2"."cb"), 1) * 255) :: int :: text, 3, ' ')) AS VARCHAR) AS "pixel"
         FROM   "merge.2"
       ),
       "jump.3"("#️⃣", "🏷️", "px", "py", "ox", "oy", "oz", "step", "att", "sray", "cr", "cg", "cb", "live_sum", "dx", "dy", "dz", "pixel") AS (
         SELECT "assignment.12"."#️⃣" AS "#️⃣",
                'start.3' AS "🏷️",
                "assignment.12"."px" AS "px",
                "assignment.12"."py" AS "py",
                NULL AS "ox",
                NULL AS "oy",
                NULL AS "oz",
                NULL AS "step",
                NULL AS "att",
                NULL AS "sray",
                NULL AS "cr",
                NULL AS "cg",
                NULL AS "cb",
                "assignment.12"."live_sum" AS "live_sum",
                NULL AS "dx",
                NULL AS "dy",
                NULL AS "dz",
                "assignment.12"."pixel" AS "pixel"
         FROM   "assignment.12"
       ),
       "where.6"("#️⃣", "⚙️", "px", "py", "ox", "oy", "oz", "step", "att", "sray", "cr", "cg", "cb", "live_sum", "dx", "dy", "dz", "hit_t", "snx", "sny", "snz", "mat", "shade_r", "shade_g", "shade_b") AS (
         SELECT "assignment.8"."#️⃣" AS "#️⃣",
                "assignment.8"."⚙️" AS "⚙️",
                "assignment.8"."px" AS "px",
                "assignment.8"."py" AS "py",
                "assignment.8"."ox" AS "ox",
                "assignment.8"."oy" AS "oy",
                "assignment.8"."oz" AS "oz",
                "assignment.8"."step" AS "step",
                "assignment.8"."att" AS "att",
                "assignment.8"."sray" AS "sray",
                "assignment.8"."cr" AS "cr",
                "assignment.8"."cg" AS "cg",
                "assignment.8"."cb" AS "cb",
                "assignment.8"."live_sum" AS "live_sum",
                "assignment.8"."dx" AS "dx",
                "assignment.8"."dy" AS "dy",
                "assignment.8"."dz" AS "dz",
                "assignment.8"."hit_t" AS "hit_t",
                "assignment.8"."snx" AS "snx",
                "assignment.8"."sny" AS "sny",
                "assignment.8"."snz" AS "snz",
                "assignment.8"."mat" AS "mat",
                "assignment.8"."shade_r" AS "shade_r",
                "assignment.8"."shade_g" AS "shade_g",
                "assignment.8"."shade_b" AS "shade_b"
         FROM   "assignment.8"
         WHERE  "assignment.8"."🔍" IS DISTINCT FROM TRUE
       ),
       "assignment.10"("#️⃣", "⚙️", "🔍", "px", "py", "ox", "oy", "oz", "step", "att", "sray", "cr", "cg", "cb", "live_sum", "dx", "dy", "dz", "hit_t", "snx", "sny", "snz", "shade_r", "shade_g", "shade_b") AS (
         SELECT "where.6"."#️⃣" AS "#️⃣",
                "where.6"."⚙️" AS "⚙️",
                CAST((("where.6"."mat") = 'r' AND ("where.6"."step") < 6) AS BOOLEAN) AS "🔍",
                "where.6"."px" AS "px",
                "where.6"."py" AS "py",
                "where.6"."ox" AS "ox",
                "where.6"."oy" AS "oy",
                "where.6"."oz" AS "oz",
                "where.6"."step" AS "step",
                "where.6"."att" AS "att",
                "where.6"."sray" AS "sray",
                "where.6"."cr" AS "cr",
                "where.6"."cg" AS "cg",
                "where.6"."cb" AS "cb",
                "where.6"."live_sum" AS "live_sum",
                "where.6"."dx" AS "dx",
                "where.6"."dy" AS "dy",
                "where.6"."dz" AS "dz",
                "where.6"."hit_t" AS "hit_t",
                "where.6"."snx" AS "snx",
                "where.6"."sny" AS "sny",
                "where.6"."snz" AS "snz",
                "where.6"."shade_r" AS "shade_r",
                "where.6"."shade_g" AS "shade_g",
                "where.6"."shade_b" AS "shade_b"
         FROM   "where.6"
       ),
       "where.7"("#️⃣", "⚙️", "px", "py", "ox", "oy", "oz", "step", "att", "sray", "cr", "cg", "cb", "live_sum", "dx", "dy", "dz", "hit_t", "snx", "sny", "snz") AS (
         SELECT "assignment.10"."#️⃣" AS "#️⃣",
                "assignment.10"."⚙️" AS "⚙️",
                "assignment.10"."px" AS "px",
                "assignment.10"."py" AS "py",
                "assignment.10"."ox" AS "ox",
                "assignment.10"."oy" AS "oy",
                "assignment.10"."oz" AS "oz",
                "assignment.10"."step" AS "step",
                "assignment.10"."att" AS "att",
                "assignment.10"."sray" AS "sray",
                "assignment.10"."cr" AS "cr",
                "assignment.10"."cg" AS "cg",
                "assignment.10"."cb" AS "cb",
                "assignment.10"."live_sum" AS "live_sum",
                "assignment.10"."dx" AS "dx",
                "assignment.10"."dy" AS "dy",
                "assignment.10"."dz" AS "dz",
                "assignment.10"."hit_t" AS "hit_t",
                "assignment.10"."snx" AS "snx",
                "assignment.10"."sny" AS "sny",
                "assignment.10"."snz" AS "snz"
         FROM   "assignment.10"
         WHERE  "assignment.10"."🔍" IS NOT DISTINCT FROM TRUE
       ),
       "assignment.11"("#️⃣", "⚙️", "px", "py", "ox", "oy", "oz", "step", "att", "sray", "cr", "cg", "cb", "live_sum", "dx", "dy", "dz") AS (
         SELECT "where.7"."#️⃣" AS "#️⃣",
                "where.7"."⚙️" AS "⚙️",
                "where.7"."px" AS "px",
                "where.7"."py" AS "py",
                CAST((("where.7"."ox") + ("where.7"."dx") * ("where.7"."hit_t") + ("where.7"."snx") * 0.0001) AS FLOAT) AS "ox",
                CAST((("where.7"."oy") + ("where.7"."dy") * ("where.7"."hit_t") + ("where.7"."sny") * 0.0001) AS FLOAT) AS "oy",
                CAST((("where.7"."oz") + ("where.7"."dz") * ("where.7"."hit_t") + ("where.7"."snz") * 0.0001) AS FLOAT) AS "oz",
                CAST((("where.7"."step") + 1) AS INTEGER) AS "step",
                CAST((("where.7"."att") * 0.8) AS FLOAT) AS "att",
                "where.7"."sray" AS "sray",
                "where.7"."cr" AS "cr",
                "where.7"."cg" AS "cg",
                "where.7"."cb" AS "cb",
                "where.7"."live_sum" AS "live_sum",
                CAST((("where.7"."dx") - ("where.7"."snx") * 2 * (("where.7"."dx") * ("where.7"."snx") + ("where.7"."dy") * ("where.7"."sny") + ("where.7"."dz") * ("where.7"."snz"))) AS DOUBLE) AS "dx",
                CAST((("where.7"."dy") - ("where.7"."sny") * 2 * (("where.7"."dx") * ("where.7"."snx") + ("where.7"."dy") * ("where.7"."sny") + ("where.7"."dz") * ("where.7"."snz"))) AS DOUBLE) AS "dy",
                CAST((("where.7"."dz") - ("where.7"."snz") * 2 * (("where.7"."dx") * ("where.7"."snx") + ("where.7"."dy") * ("where.7"."sny") + ("where.7"."dz") * ("where.7"."snz"))) AS DOUBLE) AS "dz"
         FROM   "where.7"
       ),
       "jump.2"("#️⃣", "🏷️", "px", "py", "ox", "oy", "oz", "step", "att", "sray", "cr", "cg", "cb", "live_sum", "dx", "dy", "dz", "pixel") AS (
         SELECT "assignment.11"."#️⃣" AS "#️⃣",
                'start.2' AS "🏷️",
                "assignment.11"."px" AS "px",
                "assignment.11"."py" AS "py",
                "assignment.11"."ox" AS "ox",
                "assignment.11"."oy" AS "oy",
                "assignment.11"."oz" AS "oz",
                "assignment.11"."step" AS "step",
                "assignment.11"."att" AS "att",
                "assignment.11"."sray" AS "sray",
                "assignment.11"."cr" AS "cr",
                "assignment.11"."cg" AS "cg",
                "assignment.11"."cb" AS "cb",
                "assignment.11"."live_sum" AS "live_sum",
                "assignment.11"."dx" AS "dx",
                "assignment.11"."dy" AS "dy",
                "assignment.11"."dz" AS "dz",
                NULL AS "pixel"
         FROM   "assignment.11"
       ),
       "where.8"("#️⃣", "⚙️", "px", "py", "ox", "oy", "oz", "step", "att", "live_sum", "dx", "dy", "dz", "hit_t", "snx", "sny", "snz", "shade_r", "shade_g", "shade_b") AS (
         SELECT "assignment.10"."#️⃣" AS "#️⃣",
                "assignment.10"."⚙️" AS "⚙️",
                "assignment.10"."px" AS "px",
                "assignment.10"."py" AS "py",
                "assignment.10"."ox" AS "ox",
                "assignment.10"."oy" AS "oy",
                "assignment.10"."oz" AS "oz",
                "assignment.10"."step" AS "step",
                "assignment.10"."att" AS "att",
                "assignment.10"."live_sum" AS "live_sum",
                "assignment.10"."dx" AS "dx",
                "assignment.10"."dy" AS "dy",
                "assignment.10"."dz" AS "dz",
                "assignment.10"."hit_t" AS "hit_t",
                "assignment.10"."snx" AS "snx",
                "assignment.10"."sny" AS "sny",
                "assignment.10"."snz" AS "snz",
                "assignment.10"."shade_r" AS "shade_r",
                "assignment.10"."shade_g" AS "shade_g",
                "assignment.10"."shade_b" AS "shade_b"
         FROM   "assignment.10"
         WHERE  "assignment.10"."🔍" IS DISTINCT FROM TRUE
       ),
       "fork.4"("#️⃣", "⚙️", "px", "py", "ox", "oy", "oz", "step", "att", "sray", "cr", "cg", "cb", "live_sum", "dx", "dy", "dz") AS (
         SELECT "where.8"."#️⃣" AS "#️⃣",
                "where.8"."⚙️" AS "⚙️",
                "where.8"."px" AS "px",
                "where.8"."py" AS "py",
                CAST(("ℚ"."ox") AS FLOAT) AS "ox",
                CAST(("ℚ"."oy") AS FLOAT) AS "oy",
                CAST(("ℚ"."oz") AS FLOAT) AS "oz",
                "where.8"."step" AS "step",
                "where.8"."att" AS "att",
                CAST(("ℚ"."sray") AS BOOLEAN) AS "sray",
                CAST(("ℚ"."cr") AS FLOAT) AS "cr",
                CAST(("ℚ"."cg") AS FLOAT) AS "cg",
                CAST(("ℚ"."cb") AS FLOAT) AS "cb",
                "where.8"."live_sum" AS "live_sum",
                CAST(("ℚ"."dx") AS DOUBLE) AS "dx",
                CAST(("ℚ"."dy") AS DOUBLE) AS "dy",
                CAST(("ℚ"."dz") AS DOUBLE) AS "dz"
         FROM   "where.8",
                LATERAL (SELECT ("where.8"."shade_r") * greatest(0.1, ("where.8"."snx") * (s.center_x - (("where.8"."ox") + ("where.8"."dx") * ("where.8"."hit_t"))) / sqrt((s.center_x - (("where.8"."ox") + ("where.8"."dx") * ("where.8"."hit_t"))) ** 2 + (s.center_y - (("where.8"."oy") + ("where.8"."dy") * ("where.8"."hit_t"))) ** 2 + (s.center_z - (("where.8"."oz") + ("where.8"."dz") * ("where.8"."hit_t"))) ** 2)
                                                    + ("where.8"."sny") * (s.center_y - (("where.8"."oy") + ("where.8"."dy") * ("where.8"."hit_t"))) / sqrt((s.center_x - (("where.8"."ox") + ("where.8"."dx") * ("where.8"."hit_t"))) ** 2 + (s.center_y - (("where.8"."oy") + ("where.8"."dy") * ("where.8"."hit_t"))) ** 2 + (s.center_z - (("where.8"."oz") + ("where.8"."dz") * ("where.8"."hit_t"))) ** 2)
                                                    + ("where.8"."snz") * (s.center_z - (("where.8"."oz") + ("where.8"."dz") * ("where.8"."hit_t"))) / sqrt((s.center_x - (("where.8"."ox") + ("where.8"."dx") * ("where.8"."hit_t"))) ** 2 + (s.center_y - (("where.8"."oy") + ("where.8"."dy") * ("where.8"."hit_t"))) ** 2 + (s.center_z - (("where.8"."oz") + ("where.8"."dz") * ("where.8"."hit_t"))) ** 2)),
                                ("where.8"."shade_g") * greatest(0.1, ("where.8"."snx") * (s.center_x - (("where.8"."ox") + ("where.8"."dx") * ("where.8"."hit_t"))) / sqrt((s.center_x - (("where.8"."ox") + ("where.8"."dx") * ("where.8"."hit_t"))) ** 2 + (s.center_y - (("where.8"."oy") + ("where.8"."dy") * ("where.8"."hit_t"))) ** 2 + (s.center_z - (("where.8"."oz") + ("where.8"."dz") * ("where.8"."hit_t"))) ** 2)
                                                    + ("where.8"."sny") * (s.center_y - (("where.8"."oy") + ("where.8"."dy") * ("where.8"."hit_t"))) / sqrt((s.center_x - (("where.8"."ox") + ("where.8"."dx") * ("where.8"."hit_t"))) ** 2 + (s.center_y - (("where.8"."oy") + ("where.8"."dy") * ("where.8"."hit_t"))) ** 2 + (s.center_z - (("where.8"."oz") + ("where.8"."dz") * ("where.8"."hit_t"))) ** 2)
                                                    + ("where.8"."snz") * (s.center_z - (("where.8"."oz") + ("where.8"."dz") * ("where.8"."hit_t"))) / sqrt((s.center_x - (("where.8"."ox") + ("where.8"."dx") * ("where.8"."hit_t"))) ** 2 + (s.center_y - (("where.8"."oy") + ("where.8"."dy") * ("where.8"."hit_t"))) ** 2 + (s.center_z - (("where.8"."oz") + ("where.8"."dz") * ("where.8"."hit_t"))) ** 2)),
                                ("where.8"."shade_b") * greatest(0.1, ("where.8"."snx") * (s.center_x - (("where.8"."ox") + ("where.8"."dx") * ("where.8"."hit_t"))) / sqrt((s.center_x - (("where.8"."ox") + ("where.8"."dx") * ("where.8"."hit_t"))) ** 2 + (s.center_y - (("where.8"."oy") + ("where.8"."dy") * ("where.8"."hit_t"))) ** 2 + (s.center_z - (("where.8"."oz") + ("where.8"."dz") * ("where.8"."hit_t"))) ** 2)
                                                    + ("where.8"."sny") * (s.center_y - (("where.8"."oy") + ("where.8"."dy") * ("where.8"."hit_t"))) / sqrt((s.center_x - (("where.8"."ox") + ("where.8"."dx") * ("where.8"."hit_t"))) ** 2 + (s.center_y - (("where.8"."oy") + ("where.8"."dy") * ("where.8"."hit_t"))) ** 2 + (s.center_z - (("where.8"."oz") + ("where.8"."dz") * ("where.8"."hit_t"))) ** 2)
                                                    + ("where.8"."snz") * (s.center_z - (("where.8"."oz") + ("where.8"."dz") * ("where.8"."hit_t"))) / sqrt((s.center_x - (("where.8"."ox") + ("where.8"."dx") * ("where.8"."hit_t"))) ** 2 + (s.center_y - (("where.8"."oy") + ("where.8"."dy") * ("where.8"."hit_t"))) ** 2 + (s.center_z - (("where.8"."oz") + ("where.8"."dz") * ("where.8"."hit_t"))) ** 2)),
                                ("where.8"."ox") + ("where.8"."dx") * ("where.8"."hit_t") + ("where.8"."snx") * 0.0001,
                                ("where.8"."oy") + ("where.8"."dy") * ("where.8"."hit_t") + ("where.8"."sny") * 0.0001,
                                ("where.8"."oz") + ("where.8"."dz") * ("where.8"."hit_t") + ("where.8"."snz") * 0.0001,
                                (s.center_x - (("where.8"."ox") + ("where.8"."dx") * ("where.8"."hit_t"))) / sqrt((s.center_x - (("where.8"."ox") + ("where.8"."dx") * ("where.8"."hit_t"))) ** 2 + (s.center_y - (("where.8"."oy") + ("where.8"."dy") * ("where.8"."hit_t"))) ** 2 + (s.center_z - (("where.8"."oz") + ("where.8"."dz") * ("where.8"."hit_t"))) ** 2),
                                (s.center_y - (("where.8"."oy") + ("where.8"."dy") * ("where.8"."hit_t"))) / sqrt((s.center_x - (("where.8"."ox") + ("where.8"."dx") * ("where.8"."hit_t"))) ** 2 + (s.center_y - (("where.8"."oy") + ("where.8"."dy") * ("where.8"."hit_t"))) ** 2 + (s.center_z - (("where.8"."oz") + ("where.8"."dz") * ("where.8"."hit_t"))) ** 2),
                                (s.center_z - (("where.8"."oz") + ("where.8"."dz") * ("where.8"."hit_t"))) / sqrt((s.center_x - (("where.8"."ox") + ("where.8"."dx") * ("where.8"."hit_t"))) ** 2 + (s.center_y - (("where.8"."oy") + ("where.8"."dy") * ("where.8"."hit_t"))) ** 2 + (s.center_z - (("where.8"."oz") + ("where.8"."dz") * ("where.8"."hit_t"))) ** 2),
                                true
                         FROM   spheres AS s
                         WHERE  s.material = 'l') AS "ℚ"("cr", "cg", "cb", "ox", "oy", "oz", "dx", "dy", "dz", "sray")
       ),
       "jump.1"("#️⃣", "🏷️", "px", "py", "ox", "oy", "oz", "step", "att", "sray", "cr", "cg", "cb", "live_sum", "dx", "dy", "dz", "pixel") AS (
         SELECT "fork.4"."#️⃣" AS "#️⃣",
                'start.2' AS "🏷️",
                "fork.4"."px" AS "px",
                "fork.4"."py" AS "py",
                "fork.4"."ox" AS "ox",
                "fork.4"."oy" AS "oy",
                "fork.4"."oz" AS "oz",
                "fork.4"."step" AS "step",
                "fork.4"."att" AS "att",
                "fork.4"."sray" AS "sray",
                "fork.4"."cr" AS "cr",
                "fork.4"."cg" AS "cg",
                "fork.4"."cb" AS "cb",
                "fork.4"."live_sum" AS "live_sum",
                "fork.4"."dx" AS "dx",
                "fork.4"."dy" AS "dy",
                "fork.4"."dz" AS "dz",
                NULL AS "pixel"
         FROM   "fork.4"
       ),
       "where.9"("#️⃣", "⚙️", "px", "py", "live_sum", "pixel") AS (
         SELECT "issynced.1"."#️⃣" AS "#️⃣",
                "issynced.1"."⚙️" AS "⚙️",
                "issynced.1"."px" AS "px",
                "issynced.1"."py" AS "py",
                "issynced.1"."live_sum" AS "live_sum",
                "issynced.1"."pixel" AS "pixel"
         FROM   "issynced.1"
         WHERE  "issynced.1"."🔍" IS NOT DISTINCT FROM TRUE
       ),
       "gather.1"("#️⃣", "⚙️", "pixels", "bounce_rows") AS (
         SELECT "where.9"."#️⃣" AS "#️⃣",
                "where.9"."⚙️" AS "⚙️",
                CAST((string_agg(("where.9"."pixel"), E'\n' ORDER BY ("where.9"."py"), ("where.9"."px"))) AS VARCHAR) AS "pixels",
                CAST((max(("where.9"."live_sum"))) AS BIGINT) AS "bounce_rows"
         FROM   "where.9"
         GROUP  BY "where.9"."#️⃣",
                   "where.9"."⚙️"
         HAVING COUNT(*) > 0
       ),
       "assignment.13"("#️⃣", "⚙️", "image") AS (
         SELECT "gather.1"."#️⃣" AS "#️⃣",
                "gather.1"."⚙️" AS "⚙️",
                CAST((E'P3\n96 64\n255\n' || ("gather.1"."pixels") || E'\n' || ("gather.1"."bounce_rows") :: text) AS VARCHAR) AS "image"
         FROM   "gather.1"
       ),
       "emit.1"("#️⃣", "⚙️", "📊.1") AS (
         SELECT "assignment.13"."#️⃣" AS "#️⃣",
                "assignment.13"."⚙️" AS "⚙️",
                "assignment.13"."image" AS "📊.1"
         FROM   "assignment.13"
       ),
       "stop.1"("⚙️") AS (
         SELECT "emit.1"."⚙️"
         FROM   "emit.1"
         WHERE  FALSE
       )
     (SELECT CAST(("jump.4"."#️⃣" + 1) AS INTEGER) AS "#️⃣",
             CAST(("jump.4"."🏷️") AS VARCHAR) AS "🏷️",
             CAST((NULL) AS VARCHAR) AS "📊.1",
             CAST(("jump.4"."px") AS BIGINT) AS "px",
             CAST(("jump.4"."py") AS BIGINT) AS "py",
             CAST(("jump.4"."ox") AS FLOAT) AS "ox",
             CAST(("jump.4"."oy") AS FLOAT) AS "oy",
             CAST(("jump.4"."oz") AS FLOAT) AS "oz",
             CAST(("jump.4"."step") AS INTEGER) AS "step",
             CAST(("jump.4"."att") AS FLOAT) AS "att",
             CAST(("jump.4"."sray") AS BOOLEAN) AS "sray",
             CAST(("jump.4"."cr") AS FLOAT) AS "cr",
             CAST(("jump.4"."cg") AS FLOAT) AS "cg",
             CAST(("jump.4"."cb") AS FLOAT) AS "cb",
             CAST(("jump.4"."live_sum") AS BIGINT) AS "live_sum",
             CAST(("jump.4"."dx") AS DOUBLE) AS "dx",
             CAST(("jump.4"."dy") AS DOUBLE) AS "dy",
             CAST(("jump.4"."dz") AS DOUBLE) AS "dz",
             CAST(("jump.4"."pixel") AS VARCHAR) AS "pixel"
      FROM   "jump.4")
       UNION ALL
     (SELECT CAST(("jump.3"."#️⃣" + 1) AS INTEGER) AS "#️⃣",
             CAST(("jump.3"."🏷️") AS VARCHAR) AS "🏷️",
             CAST((NULL) AS VARCHAR) AS "📊.1",
             CAST(("jump.3"."px") AS BIGINT) AS "px",
             CAST(("jump.3"."py") AS BIGINT) AS "py",
             CAST(("jump.3"."ox") AS FLOAT) AS "ox",
             CAST(("jump.3"."oy") AS FLOAT) AS "oy",
             CAST(("jump.3"."oz") AS FLOAT) AS "oz",
             CAST(("jump.3"."step") AS INTEGER) AS "step",
             CAST(("jump.3"."att") AS FLOAT) AS "att",
             CAST(("jump.3"."sray") AS BOOLEAN) AS "sray",
             CAST(("jump.3"."cr") AS FLOAT) AS "cr",
             CAST(("jump.3"."cg") AS FLOAT) AS "cg",
             CAST(("jump.3"."cb") AS FLOAT) AS "cb",
             CAST(("jump.3"."live_sum") AS BIGINT) AS "live_sum",
             CAST(("jump.3"."dx") AS DOUBLE) AS "dx",
             CAST(("jump.3"."dy") AS DOUBLE) AS "dy",
             CAST(("jump.3"."dz") AS DOUBLE) AS "dz",
             CAST(("jump.3"."pixel") AS VARCHAR) AS "pixel"
      FROM   "jump.3")
       UNION ALL
     (SELECT CAST(("jump.2"."#️⃣" + 1) AS INTEGER) AS "#️⃣",
             CAST(("jump.2"."🏷️") AS VARCHAR) AS "🏷️",
             CAST((NULL) AS VARCHAR) AS "📊.1",
             CAST(("jump.2"."px") AS BIGINT) AS "px",
             CAST(("jump.2"."py") AS BIGINT) AS "py",
             CAST(("jump.2"."ox") AS FLOAT) AS "ox",
             CAST(("jump.2"."oy") AS FLOAT) AS "oy",
             CAST(("jump.2"."oz") AS FLOAT) AS "oz",
             CAST(("jump.2"."step") AS INTEGER) AS "step",
             CAST(("jump.2"."att") AS FLOAT) AS "att",
             CAST(("jump.2"."sray") AS BOOLEAN) AS "sray",
             CAST(("jump.2"."cr") AS FLOAT) AS "cr",
             CAST(("jump.2"."cg") AS FLOAT) AS "cg",
             CAST(("jump.2"."cb") AS FLOAT) AS "cb",
             CAST(("jump.2"."live_sum") AS BIGINT) AS "live_sum",
             CAST(("jump.2"."dx") AS DOUBLE) AS "dx",
             CAST(("jump.2"."dy") AS DOUBLE) AS "dy",
             CAST(("jump.2"."dz") AS DOUBLE) AS "dz",
             CAST(("jump.2"."pixel") AS VARCHAR) AS "pixel"
      FROM   "jump.2")
       UNION ALL
     (SELECT CAST(("jump.1"."#️⃣" + 1) AS INTEGER) AS "#️⃣",
             CAST(("jump.1"."🏷️") AS VARCHAR) AS "🏷️",
             CAST((NULL) AS VARCHAR) AS "📊.1",
             CAST(("jump.1"."px") AS BIGINT) AS "px",
             CAST(("jump.1"."py") AS BIGINT) AS "py",
             CAST(("jump.1"."ox") AS FLOAT) AS "ox",
             CAST(("jump.1"."oy") AS FLOAT) AS "oy",
             CAST(("jump.1"."oz") AS FLOAT) AS "oz",
             CAST(("jump.1"."step") AS INTEGER) AS "step",
             CAST(("jump.1"."att") AS FLOAT) AS "att",
             CAST(("jump.1"."sray") AS BOOLEAN) AS "sray",
             CAST(("jump.1"."cr") AS FLOAT) AS "cr",
             CAST(("jump.1"."cg") AS FLOAT) AS "cg",
             CAST(("jump.1"."cb") AS FLOAT) AS "cb",
             CAST(("jump.1"."live_sum") AS BIGINT) AS "live_sum",
             CAST(("jump.1"."dx") AS DOUBLE) AS "dx",
             CAST(("jump.1"."dy") AS DOUBLE) AS "dy",
             CAST(("jump.1"."dz") AS DOUBLE) AS "dz",
             CAST(("jump.1"."pixel") AS VARCHAR) AS "pixel"
      FROM   "jump.1")
       UNION ALL
     (SELECT CAST(("emit.1"."#️⃣") AS INTEGER) AS "#️⃣",
             CAST((NULL) AS VARCHAR) AS "🏷️",
             CAST(("emit.1"."📊.1") AS VARCHAR) AS "📊.1",
             CAST((NULL) AS BIGINT) AS "px",
             CAST((NULL) AS BIGINT) AS "py",
             CAST((NULL) AS FLOAT) AS "ox",
             CAST((NULL) AS FLOAT) AS "oy",
             CAST((NULL) AS FLOAT) AS "oz",
             CAST((NULL) AS INTEGER) AS "step",
             CAST((NULL) AS FLOAT) AS "att",
             CAST((NULL) AS BOOLEAN) AS "sray",
             CAST((NULL) AS FLOAT) AS "cr",
             CAST((NULL) AS FLOAT) AS "cg",
             CAST((NULL) AS FLOAT) AS "cb",
             CAST((NULL) AS BIGINT) AS "live_sum",
             CAST((NULL) AS DOUBLE) AS "dx",
             CAST((NULL) AS DOUBLE) AS "dy",
             CAST((NULL) AS DOUBLE) AS "dz",
             CAST((NULL) AS VARCHAR) AS "pixel"
      FROM   "emit.1"))
  )
SELECT length("🔄"."📊.1") AS image_bytes,
       md5("🔄"."📊.1") AS image_md5
FROM   "🔄"
WHERE  "🔄"."🏷️" IS NULL;