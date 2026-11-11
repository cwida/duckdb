CREATE TABLE partsupp AS SELECT (random() * 100)::INT AS c2 
FROM range(8000);
