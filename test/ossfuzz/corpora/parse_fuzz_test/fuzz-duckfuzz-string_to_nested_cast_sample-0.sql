CREATE TABLE part AS SELECT concat('this is a long part name', i) AS p_name FROM range(20000) t(i);
