SELECT bitstring_agg(i, 0, (100 + random())::BIGINT) FROM range(10) t(i);
