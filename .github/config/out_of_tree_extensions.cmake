#
# This config file holds all out-of-tree extension that are built with DuckDB's CI
#
# to build duckdb with this configuration run:
#   EXTENSION_CONFIGS=.github/config/out_of_tree_extensions.cmake make
#
#  Note that many of these packages require vcpkg, and a merged manifest must be created to
#  compile multiple of them.
#
#  After setting up vcpkg, build using e.g. the following commands:
#  EXTENSION_CONFIGS=.github/config/out_of_tree_extensions.cmake make extension_configuration
#  EXTENSION_CONFIGS=.github/config/out_of_tree_extensions.cmake make debug
#
#  Make sure the VCPKG_TOOLCHAIN_PATH and VCPKG_TARGET_TRIPLET are set. For example:
#  VCPKG_TOOLCHAIN_PATH=~/vcpkg/scripts/buildsystems/vcpkg.cmake
#  VCPKG_TARGET_TRIPLET=arm64-osx

set(DUCKDB_MAIN_EXTENSION_CONFIG_TYPE "OUT_OF_TREE")
include("${CMAKE_CURRENT_LIST_DIR}/core.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/cloud.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/search.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/scanner.cmake")
unset(DUCKDB_MAIN_EXTENSION_CONFIG_TYPE)
