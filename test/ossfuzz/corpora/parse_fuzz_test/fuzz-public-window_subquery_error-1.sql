SELECT 'string' IN ( SELECT string_agg ('str') OVER ( PARTITION BY c02 + 'string' IN ( SELECT 'string' )) );
