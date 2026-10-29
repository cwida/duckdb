-- Trampoline formulation of flummi_connected_components.sql: selector-routed recursion in the
-- style of "Trampoline-Style Queries for SQL" (CIDR 2025), mechanically
-- translated label-per-branch from the recursive formulation.
-- Trampoline formulation, mechanically translated from rcte.sql by
-- scripts/translate_flummi_trampoline.py: one branch per label, entry
-- filters of other labels become FALSE, the selector mirrors the label.
WITH RECURSIVE
  "🔄"("b", "#️⃣", "🏷️", "📊.1", "📊.2", "node", "component", "components") USING TRAMPOLINE ("b") AS (
  (SELECT CASE WHEN q."🏷️" IS NULL THEN 0 WHEN q."🏷️" = 'start.1' THEN 1 WHEN q."🏷️" = 'start.2' THEN 2 END AS "b", q.* FROM (SELECT CAST((0) AS INTEGER) AS "#️⃣",
            CAST(('start.1') AS VARCHAR) AS "🏷️",
            CAST((NULL) AS INTEGER) AS "📊.1",
            CAST((NULL) AS INTEGER[]) AS "📊.2",
            CAST((NULL) AS INTEGER) AS "node",
            CAST((NULL) AS INTEGER) AS "component",
            CAST((NULL) AS INTEGER) AS "components"
) AS q)
  BRANCH 1: (SELECT CASE WHEN q."🏷️" IS NULL THEN 0 WHEN q."🏷️" = 'start.1' THEN 1 WHEN q."🏷️" = 'start.2' THEN 2 END AS "b", q.* FROM (WITH
       "start.1"("#️⃣", "⚙️") AS (
         SELECT "🔄"."#️⃣" AS "#️⃣",
                NULL AS "⚙️"
         FROM   "🔄"
         WHERE  "🔄"."🏷️" IS NOT DISTINCT FROM 'start.1'
       ),
       "fork.1"("#️⃣", "⚙️", "node") AS (
         SELECT "start.1"."#️⃣" AS "#️⃣",
                "start.1"."⚙️" AS "⚙️",
                CAST(("ℚ"."node") AS INTEGER) AS "node"
         FROM   "start.1",
                (FROM nodes) AS "ℚ"("node")
       ),
       "assignment.1"("#️⃣", "⚙️", "node", "component") AS (
         SELECT "fork.1"."#️⃣" AS "#️⃣",
                "fork.1"."⚙️" AS "⚙️",
                "fork.1"."node" AS "node",
                CAST((("fork.1"."node")) AS INTEGER) AS "component"
         FROM   "fork.1"
       ),
       "assignment.2"("#️⃣", "⚙️", "node", "component", "components") AS (
         SELECT "assignment.1"."#️⃣" AS "#️⃣",
                "assignment.1"."⚙️" AS "⚙️",
                "assignment.1"."node" AS "node",
                "assignment.1"."component" AS "component",
                CAST((bit_xor(("assignment.1"."component")) OVER ()) AS INTEGER) AS "components"
         FROM   "assignment.1"
       ),
       "merge.1"("#️⃣", "⚙️", "node", "component", "components") AS (
         (SELECT "assignment.2"."#️⃣" AS "#️⃣",
                 "assignment.2"."⚙️" AS "⚙️",
                 "assignment.2"."node" AS "node",
                 "assignment.2"."component" AS "component",
                 "assignment.2"."components" AS "components"
          FROM   "assignment.2")
       ),
       "fork.2"("#️⃣", "⚙️", "node", "component", "components") AS (
         SELECT "merge.1"."#️⃣" AS "#️⃣",
                "merge.1"."⚙️" AS "⚙️",
                CAST(("ℚ"."node") AS INTEGER) AS "node",
                "merge.1"."component" AS "component",
                "merge.1"."components" AS "components"
         FROM   "merge.1",
                LATERAL (SELECT there
                         FROM   edges
                         WHERE  here = ("merge.1"."node")
                         AND    there > ("merge.1"."component")) AS "ℚ"("node")
       ),
       "gather.1"("#️⃣", "⚙️", "node", "component", "components") AS (
         SELECT "fork.2"."#️⃣" AS "#️⃣",
                "fork.2"."⚙️" AS "⚙️",
                "fork.2"."node" AS "node",
                CAST((min(("fork.2"."component"))) AS INTEGER) AS "component",
                "fork.2"."components" AS "components"
         FROM   "fork.2"
         GROUP  BY "fork.2"."#️⃣",
                   "fork.2"."components",
                   "fork.2"."node",
                   "fork.2"."⚙️"
         HAVING COUNT(*) > 0
       ),
       "assignment.3"("#️⃣", "⚙️", "node", "component", "components", "old_components") AS (
         SELECT "gather.1"."#️⃣" AS "#️⃣",
                "gather.1"."⚙️" AS "⚙️",
                "gather.1"."node" AS "node",
                "gather.1"."component" AS "component",
                CAST((bit_xor(("gather.1"."component")) OVER ()) AS INTEGER) AS "components",
                CAST((("gather.1"."components")) AS INTEGER) AS "old_components"
         FROM   "gather.1"
       ),
       "assignment.4"("#️⃣", "⚙️", "🔍", "node", "component", "components") AS (
         SELECT "assignment.3"."#️⃣" AS "#️⃣",
                "assignment.3"."⚙️" AS "⚙️",
                CAST((("assignment.3"."old_components") = ("assignment.3"."components")) AS BOOLEAN) AS "🔍",
                "assignment.3"."node" AS "node",
                "assignment.3"."component" AS "component",
                "assignment.3"."components" AS "components"
         FROM   "assignment.3"
       ),
       "where.1"("#️⃣", "⚙️", "node", "component") AS (
         SELECT "assignment.4"."#️⃣" AS "#️⃣",
                "assignment.4"."⚙️" AS "⚙️",
                "assignment.4"."node" AS "node",
                "assignment.4"."component" AS "component"
         FROM   "assignment.4"
         WHERE  "assignment.4"."🔍" IS NOT DISTINCT FROM TRUE
       ),
       "gather.2"("#️⃣", "⚙️", "component", "nodes") AS (
         SELECT "where.1"."#️⃣" AS "#️⃣",
                "where.1"."⚙️" AS "⚙️",
                "where.1"."component" AS "component",
                CAST((list(("where.1"."node"))) AS INTEGER[]) AS "nodes"
         FROM   "where.1"
         GROUP  BY "where.1"."#️⃣",
                   "where.1"."component",
                   "where.1"."⚙️"
         HAVING COUNT(*) > 0
       ),
       "emit.1"("#️⃣", "⚙️", "📊.1", "📊.2") AS (
         SELECT "gather.2"."#️⃣" AS "#️⃣",
                "gather.2"."⚙️" AS "⚙️",
                "gather.2"."component" AS "📊.1",
                "gather.2"."nodes" AS "📊.2"
         FROM   "gather.2"
       ),
       "where.2"("#️⃣", "⚙️", "node", "component", "components") AS (
         SELECT "assignment.4"."#️⃣" AS "#️⃣",
                "assignment.4"."⚙️" AS "⚙️",
                "assignment.4"."node" AS "node",
                "assignment.4"."component" AS "component",
                "assignment.4"."components" AS "components"
         FROM   "assignment.4"
         WHERE  "assignment.4"."🔍" IS DISTINCT FROM TRUE
       ),
       "jump.1"("#️⃣", "🏷️", "node", "component", "components") AS (
         SELECT "where.2"."#️⃣" AS "#️⃣",
                'start.2' AS "🏷️",
                "where.2"."node" AS "node",
                "where.2"."component" AS "component",
                "where.2"."components" AS "components"
         FROM   "where.2"
       )
(SELECT CAST(("emit.1"."#️⃣") AS INTEGER) AS "#️⃣",
             CAST((NULL) AS VARCHAR) AS "🏷️",
             CAST(("emit.1"."📊.1") AS INTEGER) AS "📊.1",
             CAST(("emit.1"."📊.2") AS INTEGER[]) AS "📊.2",
             CAST((NULL) AS INTEGER) AS "node",
             CAST((NULL) AS INTEGER) AS "component",
             CAST((NULL) AS INTEGER) AS "components"
      FROM   "emit.1")
       UNION ALL
     (SELECT CAST(("jump.1"."#️⃣" + 1) AS INTEGER) AS "#️⃣",
             CAST(("jump.1"."🏷️") AS VARCHAR) AS "🏷️",
             CAST((NULL) AS INTEGER) AS "📊.1",
             CAST((NULL) AS INTEGER[]) AS "📊.2",
             CAST(("jump.1"."node") AS INTEGER) AS "node",
             CAST(("jump.1"."component") AS INTEGER) AS "component",
             CAST(("jump.1"."components") AS INTEGER) AS "components"
      FROM   "jump.1")
) AS q)
  BRANCH 2: (SELECT CASE WHEN q."🏷️" IS NULL THEN 0 WHEN q."🏷️" = 'start.1' THEN 1 WHEN q."🏷️" = 'start.2' THEN 2 END AS "b", q.* FROM (WITH
       "start.2"("#️⃣", "⚙️", "node", "component", "components") AS (
         SELECT "🔄"."#️⃣" AS "#️⃣",
                NULL AS "⚙️",
                "🔄"."node" AS "node",
                "🔄"."component" AS "component",
                "🔄"."components" AS "components"
         FROM   "🔄"
         WHERE  "🔄"."🏷️" IS NOT DISTINCT FROM 'start.2'
       ),
       "merge.1"("#️⃣", "⚙️", "node", "component", "components") AS (
         (SELECT "start.2"."#️⃣" AS "#️⃣",
                 "start.2"."⚙️" AS "⚙️",
                 "start.2"."node" AS "node",
                 "start.2"."component" AS "component",
                 "start.2"."components" AS "components"
          FROM   "start.2")
       ),
       "fork.2"("#️⃣", "⚙️", "node", "component", "components") AS (
         SELECT "merge.1"."#️⃣" AS "#️⃣",
                "merge.1"."⚙️" AS "⚙️",
                CAST(("ℚ"."node") AS INTEGER) AS "node",
                "merge.1"."component" AS "component",
                "merge.1"."components" AS "components"
         FROM   "merge.1",
                LATERAL (SELECT there
                         FROM   edges
                         WHERE  here = ("merge.1"."node")
                         AND    there > ("merge.1"."component")) AS "ℚ"("node")
       ),
       "gather.1"("#️⃣", "⚙️", "node", "component", "components") AS (
         SELECT "fork.2"."#️⃣" AS "#️⃣",
                "fork.2"."⚙️" AS "⚙️",
                "fork.2"."node" AS "node",
                CAST((min(("fork.2"."component"))) AS INTEGER) AS "component",
                "fork.2"."components" AS "components"
         FROM   "fork.2"
         GROUP  BY "fork.2"."#️⃣",
                   "fork.2"."components",
                   "fork.2"."node",
                   "fork.2"."⚙️"
         HAVING COUNT(*) > 0
       ),
       "assignment.3"("#️⃣", "⚙️", "node", "component", "components", "old_components") AS (
         SELECT "gather.1"."#️⃣" AS "#️⃣",
                "gather.1"."⚙️" AS "⚙️",
                "gather.1"."node" AS "node",
                "gather.1"."component" AS "component",
                CAST((bit_xor(("gather.1"."component")) OVER ()) AS INTEGER) AS "components",
                CAST((("gather.1"."components")) AS INTEGER) AS "old_components"
         FROM   "gather.1"
       ),
       "assignment.4"("#️⃣", "⚙️", "🔍", "node", "component", "components") AS (
         SELECT "assignment.3"."#️⃣" AS "#️⃣",
                "assignment.3"."⚙️" AS "⚙️",
                CAST((("assignment.3"."old_components") = ("assignment.3"."components")) AS BOOLEAN) AS "🔍",
                "assignment.3"."node" AS "node",
                "assignment.3"."component" AS "component",
                "assignment.3"."components" AS "components"
         FROM   "assignment.3"
       ),
       "where.1"("#️⃣", "⚙️", "node", "component") AS (
         SELECT "assignment.4"."#️⃣" AS "#️⃣",
                "assignment.4"."⚙️" AS "⚙️",
                "assignment.4"."node" AS "node",
                "assignment.4"."component" AS "component"
         FROM   "assignment.4"
         WHERE  "assignment.4"."🔍" IS NOT DISTINCT FROM TRUE
       ),
       "gather.2"("#️⃣", "⚙️", "component", "nodes") AS (
         SELECT "where.1"."#️⃣" AS "#️⃣",
                "where.1"."⚙️" AS "⚙️",
                "where.1"."component" AS "component",
                CAST((list(("where.1"."node"))) AS INTEGER[]) AS "nodes"
         FROM   "where.1"
         GROUP  BY "where.1"."#️⃣",
                   "where.1"."component",
                   "where.1"."⚙️"
         HAVING COUNT(*) > 0
       ),
       "emit.1"("#️⃣", "⚙️", "📊.1", "📊.2") AS (
         SELECT "gather.2"."#️⃣" AS "#️⃣",
                "gather.2"."⚙️" AS "⚙️",
                "gather.2"."component" AS "📊.1",
                "gather.2"."nodes" AS "📊.2"
         FROM   "gather.2"
       ),
       "where.2"("#️⃣", "⚙️", "node", "component", "components") AS (
         SELECT "assignment.4"."#️⃣" AS "#️⃣",
                "assignment.4"."⚙️" AS "⚙️",
                "assignment.4"."node" AS "node",
                "assignment.4"."component" AS "component",
                "assignment.4"."components" AS "components"
         FROM   "assignment.4"
         WHERE  "assignment.4"."🔍" IS DISTINCT FROM TRUE
       ),
       "jump.1"("#️⃣", "🏷️", "node", "component", "components") AS (
         SELECT "where.2"."#️⃣" AS "#️⃣",
                'start.2' AS "🏷️",
                "where.2"."node" AS "node",
                "where.2"."component" AS "component",
                "where.2"."components" AS "components"
         FROM   "where.2"
       )
(SELECT CAST(("emit.1"."#️⃣") AS INTEGER) AS "#️⃣",
             CAST((NULL) AS VARCHAR) AS "🏷️",
             CAST(("emit.1"."📊.1") AS INTEGER) AS "📊.1",
             CAST(("emit.1"."📊.2") AS INTEGER[]) AS "📊.2",
             CAST((NULL) AS INTEGER) AS "node",
             CAST((NULL) AS INTEGER) AS "component",
             CAST((NULL) AS INTEGER) AS "components"
      FROM   "emit.1")
       UNION ALL
     (SELECT CAST(("jump.1"."#️⃣" + 1) AS INTEGER) AS "#️⃣",
             CAST(("jump.1"."🏷️") AS VARCHAR) AS "🏷️",
             CAST((NULL) AS INTEGER) AS "📊.1",
             CAST((NULL) AS INTEGER[]) AS "📊.2",
             CAST(("jump.1"."node") AS INTEGER) AS "node",
             CAST(("jump.1"."component") AS INTEGER) AS "component",
             CAST(("jump.1"."components") AS INTEGER) AS "components"
      FROM   "jump.1")
) AS q)
  )
SELECT count(*) AS components,
       sum(len("🔄"."📊.2"))::BIGINT AS nodes,
       sum("🔄"."📊.1")::BIGINT AS component_sum
FROM   "🔄"
WHERE  "🔄"."🏷️" IS NULL;
