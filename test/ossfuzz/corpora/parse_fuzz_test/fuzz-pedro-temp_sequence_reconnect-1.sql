CREATE TABLE t1 (
	c1 INT,
	CHECK(nextval('t1'))
);
