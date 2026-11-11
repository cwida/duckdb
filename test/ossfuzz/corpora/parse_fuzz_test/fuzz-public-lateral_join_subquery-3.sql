SELECT  c01 from values('string') as _(c01), LATERAL ( WITH ta02 AS MATERIALIZED ( SELECT 'string' ) ( SELECT 'string' ) INTERSECT ALL ( SELECT 'string' ) );
