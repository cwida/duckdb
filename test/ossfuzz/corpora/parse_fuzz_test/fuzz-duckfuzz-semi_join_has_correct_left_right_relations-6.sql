SELECT NULL
FROM   all_types
       	INNER JOIN (SELECT NULL)
             RIGHT JOIN all_types AS ref_2
                 LEFT JOIN all_types AS ref_7
                   INNER JOIN all_types AS ref_8
                   ON (( ref_7.int_array = ref_8.int_array ))
                   ON ( NULL )
                   INNER JOIN all_types AS ref_9
                    ON ( ref_2."varchar" )
               ON (( "Version"() IS NULL ))
       ON ( EXISTS(SELECT ref_7."bit" AS c0
           FROM   all_types AS ref_10
           WHERE  0
           ));
