SELECT ( SELECT to_centuries ( v1 ) FROM v0 WHERE fmod ( fmod ( v1 , 3.100000 ) , 3.100000 ) OVER( ORDER BY v1 ) = 70000 ) AS century_result;
