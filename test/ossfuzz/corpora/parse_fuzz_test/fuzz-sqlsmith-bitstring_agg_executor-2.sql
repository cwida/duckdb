SELECT bitstring_agg(v, CAST(v AS UBIGINT), CAST(v AS UBIGINT)) FROM tbl;
