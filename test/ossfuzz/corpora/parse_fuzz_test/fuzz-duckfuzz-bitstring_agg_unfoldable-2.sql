SELECT bitstring_agg(i, 42, i) FROM range(10) t(i);
