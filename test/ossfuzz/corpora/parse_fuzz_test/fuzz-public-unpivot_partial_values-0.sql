FROM (SELECT 42 c1) AS ta06 UNPIVOT (c1 FOR 'v' IN ( ( c1, 'a' || 'b' ) ) ) ;
