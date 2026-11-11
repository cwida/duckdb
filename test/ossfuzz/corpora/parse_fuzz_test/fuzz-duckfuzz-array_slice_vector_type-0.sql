SELECT
	length(original) == length(altered) + 1
from (
	select
		x as original,
		array_pop_front(x) as altered
	FROM test_vector_types('x') AS t1(x)
);
