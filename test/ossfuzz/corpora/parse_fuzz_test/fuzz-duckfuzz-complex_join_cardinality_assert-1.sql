SELECT NULL FROM integers AS t1 RIGHT JOIN integers AS t2 ON (NULL) WHERE EXISTS(SELECT NULL FROM integers AS t3 INNER JOIN integers ON ((t3.i + t1.i)) , (SELECT t2.i AS c0));
