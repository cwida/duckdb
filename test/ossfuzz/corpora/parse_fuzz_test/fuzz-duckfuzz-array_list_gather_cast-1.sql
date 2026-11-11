FROM main.my_table ref_0
  INNER JOIN
      main.my_table AS ref_1
      ON (
          SELECT
              ref_1.struct_of_arrays AS c6
          FROM main.my_table AS ref_2
          WHERE
              EXISTS(SELECT ref_2.fixed_nested_varchar_array WHERE ref_1."union")
      ) and ref_1."varchar" ~~ ref_1."varchar";
