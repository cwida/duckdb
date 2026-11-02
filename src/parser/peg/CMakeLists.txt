add_subdirectory(tokenizer)
add_subdirectory(transformer)
add_subdirectory(keyword_helper)

add_library_unity(
  duckdb_parser_peg
  OBJECT
  autocomplete_core.cpp
  keyword_map.cpp
  matcher.cpp
  matcher_first_set.cpp
  operator_matcher.cpp
  dialect_extension.cpp
  matcher_process.cpp
  matcher_stack.cpp
  compiled_grammar.cpp
  grammar_change.cpp
  matcher_factory.cpp
  parser_packrat.cpp
  peg_parser.cpp
  parsed_grammar.cpp
  peg_rule_id.cpp
  passthrough_dialect.cpp
  sql_formatter.cpp
  syntax_error.cpp)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_parser_peg>
    PARENT_SCOPE)
