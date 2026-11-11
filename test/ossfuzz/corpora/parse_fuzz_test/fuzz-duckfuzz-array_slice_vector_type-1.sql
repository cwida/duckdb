select case
	when x IS NULL
		then
			NULL
		else
			true
		end
	from test_vector_types('x') t1(x);
