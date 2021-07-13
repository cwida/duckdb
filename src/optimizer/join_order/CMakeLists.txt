add_library_unity(duckdb_optimizer_join_order OBJECT query_graph.cpp
                  join_relation_set.cpp)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_optimizer_join_order>
    PARENT_SCOPE)
