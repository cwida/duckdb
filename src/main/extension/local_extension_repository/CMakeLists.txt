# The local_extension_repository capability: automatic installs default to this
# build's extension repository. Built in every build, including one against a
# prebuilt DuckDB, since the path it holds belongs to the build that makes it.
file(TO_CMAKE_PATH "${LOCAL_EXTENSION_REPO_DIR}" LOCAL_EXTENSION_REPOSITORY)
configure_file(local_extension_repository.cpp.in
               ${CMAKE_CURRENT_BINARY_DIR}/local_extension_repository.cpp @ONLY)
add_library(duckdb_local_extension_repository STATIC
            ${CMAKE_CURRENT_BINARY_DIR}/local_extension_repository.cpp)
target_link_libraries(duckdb_local_extension_repository duckdb_static)
