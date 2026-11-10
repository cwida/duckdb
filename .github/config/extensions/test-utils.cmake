duckdb_extension_load(test_utils
  GIT_URL https://github.com/duckdb/bwc-test-utils
  # Use the commit before "Update extensions" (that contains the binaries of
  # the commit before that).
  GIT_TAG 2669706d3384ddc2e44d9972728378c8a1afe33c
  # For local dev:
  # SOURCE_DIR "${EXTENSION_CONFIG_BASE_DIR}/../../../../test-utils"
)

include("${EXTENSION_CONFIG_BASE_DIR}/../in_tree_extensions.cmake")
