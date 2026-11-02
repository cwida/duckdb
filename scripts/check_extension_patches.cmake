# Verifies that every extension with patches also passes APPLY_PATCHES.
#
# Patches under .github/patches/extensions/<name> are only used when the matching
# duckdb_extension_load() call passes APPLY_PATCHES. Without it the patches are
# silently ignored, which is how the mysql_scanner, postgres_scanner,
# sqlite_scanner and sqlsmith patches went unapplied after their GIT_TAGs were
# bumped.
#
# Run with:
#   cmake -DCONFIG_DIR=.github/config -DPATCH_DIR=.github/patches/extensions \
#         -P scripts/check_extension_patches.cmake

cmake_minimum_required(VERSION 3.14...3.29)

if(NOT CONFIG_DIR OR NOT PATCH_DIR)
    message(FATAL_ERROR "CONFIG_DIR and PATCH_DIR must be set")
endif()

# Reach the maximal set of duckdb_extension_load() calls: guards such as
# if (NOT MINGW AND NOT ${WASM_ENABLED}) would otherwise hide an extension whose
# patch directory exists unconditionally.
set(WASM_ENABLED 0)
set(MUSL_ENABLED 0)
set(MINGW OFF)
set(WIN32 OFF)
set(BUILD_COMPLETE_EXTENSION_SET 1)
set(VORTEX_ENABLED ON)
set(MYSQL_SCANNER_ENABLED ON)

set(EXTENSION_CONFIG_BASE_DIR "${CONFIG_DIR}/extensions")

# Record the extensions that ask for their patches to be applied.
set(APPLYING "")
macro(duckdb_extension_load NAME)
    cmake_parse_arguments(EXT "APPLY_PATCHES;DONT_BUILD;LOAD_TESTS" "GIT_URL;GIT_TAG" "" ${ARGN})
    if(EXT_APPLY_PATCHES)
        list(APPEND APPLYING "${NAME}")
    endif()
endmacro()
# What gets linked does not matter for patches.
macro(duckdb_extension_statically_link)
endmacro()

# test-utils.cmake sits in extensions/ but is a top-level config itself: it
# includes ../in_tree_extensions.cmake.
foreach(config
        "${CONFIG_DIR}/bundled_extensions.cmake"
        "${CONFIG_DIR}/external_extensions.cmake"
        "${CONFIG_DIR}/in_tree_extensions.cmake"
        "${CONFIG_DIR}/out_of_tree_extensions.cmake"
        "${CONFIG_DIR}/rust_based_extensions.cmake"
        "${EXTENSION_CONFIG_BASE_DIR}/test-utils.cmake")
    include("${config}")
endforeach()
list(REMOVE_DUPLICATES APPLYING)
list(SORT APPLYING)

# Collect the extensions that actually have patches on disk.
set(HAVING "")
file(GLOB entries "${PATCH_DIR}/*")
foreach(entry ${entries})
    if(IS_DIRECTORY "${entry}")
        file(GLOB patches "${entry}/*.patch")
        if(patches)
            get_filename_component(name "${entry}" NAME)
            list(APPEND HAVING "${name}")
            set(PATCHES_${name} "")
            foreach(patch ${patches})
                get_filename_component(patch_name "${patch}" NAME)
                list(APPEND PATCHES_${name} "${patch_name}")
            endforeach()
            list(SORT PATCHES_${name})
        endif()
    endif()
endforeach()
list(SORT HAVING)

set(ERRORS "")
foreach(name ${HAVING})
    if(NOT "${name}" IN_LIST APPLYING)
        list(LENGTH PATCHES_${name} count)
        if(count EQUAL 1)
            set(text "${name}: 1 patch exists, no APPLY_PATCHES")
        else()
            set(text "${name}: ${count} patches exist, no APPLY_PATCHES")
        endif()
        foreach(patch ${PATCHES_${name}})
            string(APPEND text "\n  ${patch}")
        endforeach()
        list(APPEND ERRORS "${text}")
    endif()
endforeach()
foreach(name ${APPLYING})
    if(NOT "${name}" IN_LIST HAVING)
        list(APPEND ERRORS "${name}: no patches, APPLY_PATCHES is present")
    endif()
endforeach()

if(ERRORS)
    foreach(error ${ERRORS})
        message(NOTICE "${error}")
    endforeach()
    message(FATAL_ERROR "extension patch check failed, see .github/patches/extensions/README.md")
endif()

list(LENGTH APPLYING count)
message(STATUS "ok: ${count} extensions apply patches, all accounted for")
