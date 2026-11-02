add_library_unity(
  duckdb_common_tree_renderer
  OBJECT
  compact_tree_renderer.cpp
  json_tree_renderer.cpp
  html_tree_renderer.cpp
  graphviz_tree_renderer.cpp
  text_tree_renderer.cpp
  yaml_tree_renderer.cpp
  mermaid_tree_renderer.cpp
  tree_renderer.cpp)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_common_tree_renderer>
    PARENT_SCOPE)
