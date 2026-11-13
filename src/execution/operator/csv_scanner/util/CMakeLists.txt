add_library_unity(duckdb_csv_util OBJECT csv_error.cpp csv_opencsv_record.cpp
                  csv_reader_options.cpp csv_validator.cpp)

set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_csv_util>
    PARENT_SCOPE)
