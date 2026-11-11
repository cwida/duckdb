FROM (VALUES (42)) t(i) PIVOT (current_date() FOR (i) IN (41));
