SELECT c0, count(c0) over () AS count_window FROM (SELECT 1) t0(c0) GROUP BY c0 QUALIFY count_window;
