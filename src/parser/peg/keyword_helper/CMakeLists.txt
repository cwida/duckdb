add_library_unity(
  duckdb_parser_peg_keyword_helper OBJECT duckdb_keyword_helper.cpp
  parsed_grammar_keyword_helper.cpp default_keyword_maps.cpp
  grammar_literal_table.cpp)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_parser_peg_keyword_helper>
    PARENT_SCOPE)
