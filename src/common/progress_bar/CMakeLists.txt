add_library_unity(duckdb_progress_bar OBJECT progress_bar.cpp
                  terminal_progress_bar_display.cpp)

set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_progress_bar>
    PARENT_SCOPE)
