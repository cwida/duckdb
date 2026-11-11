create table all_types as select * exclude(small_enum, medium_enum, large_enum)
from test_all_types();
