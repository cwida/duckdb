insert into all_types select * exclude(small_enum, medium_enum, large_enum) from test_all_types();
