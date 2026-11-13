#
# This is the extension configuration for extensions that are maintained externally
#
#

set(DUCKDB_EXTEND ${DUCKDB_EXTEND} vortex)
include("${EXTENSION_CONFIG_BASE_DIR}/vortex.cmake")
include("${EXTENSION_CONFIG_BASE_DIR}/lance.cmake")
