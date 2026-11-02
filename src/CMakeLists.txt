add_definitions(-DDUCKDB)

if(${DISABLE_THREADS})
  add_definitions(-DDUCKDB_NO_THREADS)
endif()

if(NOT MSVC)
  set(CMAKE_CXX_FLAGS_DEBUG
      "${CMAKE_CXX_FLAGS_DEBUG} -Wextra -Wno-unused-parameter -Wno-redundant-move"
  )
  if(CMAKE_COMPILER_IS_GNUCC)
    if(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER 6)
      set(CMAKE_CXX_FLAGS_DEBUG
          "${CMAKE_CXX_FLAGS_DEBUG} -Wimplicit-fallthrough")
    endif()
  else()
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -Wimplicit-fallthrough")
  endif()
endif()
if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang" OR "${CMAKE_CXX_COMPILER_ID}"
                                                  STREQUAL "AppleClang")
  set(CMAKE_CXX_FLAGS_DEBUG
      "${CMAKE_CXX_FLAGS_DEBUG} -Wimplicit-int-conversion -Wshorten-64-to-32 -Wnarrowing -Wsign-conversion -Wsign-compare -Wconversion -Wtype-limits"
  )
endif()

if(MSVC)
  add_compile_options("/bigobj")
endif()

function(ensure_variable_is_number INPUT_VERSION OUT_RESULT)
  if(NOT "${${INPUT_VERSION}}" MATCHES "^[0-9]+$")
    message(
      WARNING
        "VERSION PARAMETER ${INPUT_VERSION} \"${${INPUT_VERSION}}\" IS NOT A NUMBER - SETTING TO 0"
    )
    set(${OUT_RESULT}
        0
        PARENT_SCOPE)
  else()
    set(${OUT_RESULT}
        ${${INPUT_VERSION}}
        PARENT_SCOPE)
  endif()
endfunction()

add_definitions(-DDUCKDB_MAIN_LIBRARY)

add_subdirectory(optimizer)
add_subdirectory(planner)
add_subdirectory(parser)
add_subdirectory(function)
add_subdirectory(catalog)
add_subdirectory(common)
add_subdirectory(logging)
add_subdirectory(execution)
add_subdirectory(main)
add_subdirectory(parallel)
add_subdirectory(storage)
add_subdirectory(transaction)

set(DUCKDB_LINK_LIBS
    duckdb_fsst
    duckdb_fmt
    duckdb_re2
    duckdb_miniz
    duckdb_utf8proc
    duckdb_hyperloglog
    duckdb_fastpforlib
    duckdb_skiplistlib
    duckdb_mbedtls
    duckdb_yyjson
    duckdb_zstd)

if(JEMALLOC_ENABLED)
  list(APPEND DUCKDB_LINK_LIBS duckdb_jemalloc)
endif()

foreach(LINK_LIB ${DUCKDB_LINK_LIBS})
  list(APPEND LINK_OBJECTS $<TARGET_OBJECTS:${LINK_LIB}>)
endforeach(LINK_LIB)

add_library(duckdb_static STATIC ${ALL_OBJECT_FILES} ${LINK_OBJECTS})
target_link_libraries(duckdb_static PUBLIC ${DUCKDB_SYSTEM_LIBS})
link_threads(duckdb_static PUBLIC)
duckdb_make_native_lto_archive(duckdb_static)

#
# FIXME file(COPY ${CMAKE_SOURCE_DIR}/libduckdb_static.a  DESTINATION
# ${CMAKE_CURRENT_BINARY_DIR}) add_library(duckdb_static STATIC IMPORTED GLOBAL)
# set_property(TARGET duckdb_static PROPERTY IMPORTED_LOCATION
# ${CMAKE_CURRENT_BINARY_DIR}/libduckdb_static.a)
#

if(NATIVE_LTO_STATIC_LIBRARIES)
  add_library(duckdb SHARED)
  target_link_libraries(duckdb PRIVATE -Wl,--whole-archive duckdb_static
                                       -Wl,--no-whole-archive)
else()
  add_library(duckdb SHARED ${ALL_OBJECT_FILES} ${LINK_OBJECTS})
endif()
target_link_libraries(duckdb PUBLIC ${DUCKDB_SYSTEM_LIBS})
link_extension_libraries(duckdb PRIVATE)

link_threads(duckdb PUBLIC)

if(SET_DUCKDB_LIBRARY_VERSION)
  set_target_properties(
    duckdb PROPERTIES VERSION ${DUCKDB_VERSION_NUMBER}
                      SOVERSION ${DUCKDB_MAJOR_VERSION}.${DUCKDB_MINOR_VERSION})

endif()

if(WIN32 AND NOT MINGW)
  ensure_variable_is_number(DUCKDB_MAJOR_VERSION RC_MAJOR_VERSION)
  ensure_variable_is_number(DUCKDB_MINOR_VERSION RC_MINOR_VERSION)
  ensure_variable_is_number(DUCKDB_PATCH_VERSION RC_PATCH_VERSION)
  ensure_variable_is_number(DUCKDB_DEV_ITERATION RC_DEV_ITERATION)

  set(CMAKE_RC_FLAGS
      "${CMAKE_RC_FLAGS} -D DUCKDB_VERSION=\"${DUCKDB_VERSION}\"")
  set(CMAKE_RC_FLAGS
      "${CMAKE_RC_FLAGS} -D DUCKDB_MAJOR_VERSION=\"${RC_MAJOR_VERSION}\"")
  set(CMAKE_RC_FLAGS
      "${CMAKE_RC_FLAGS} -D DUCKDB_MINOR_VERSION=\"${RC_MINOR_VERSION}\"")
  set(CMAKE_RC_FLAGS
      "${CMAKE_RC_FLAGS} -D DUCKDB_PATCH_VERSION=\"${RC_PATCH_VERSION}\"")
  set(CMAKE_RC_FLAGS
      "${CMAKE_RC_FLAGS} -D DUCKDB_DEV_ITERATION=\"${RC_DEV_ITERATION}\"")

  target_sources(duckdb PRIVATE version.rc)
endif()

install(
  DIRECTORY "${PROJECT_SOURCE_DIR}/src/include/duckdb"
  DESTINATION "${INSTALL_INCLUDE_DIR}"
  FILES_MATCHING
  PATTERN "*.hpp"
  PATTERN "*.ipp")
install(
  FILES "${PROJECT_SOURCE_DIR}/src/include/duckdb.hpp"
        "${PROJECT_SOURCE_DIR}/src/include/duckdb.h"
        "${PROJECT_SOURCE_DIR}/src/include/duckdb_extension.h"
        "${PROJECT_SOURCE_DIR}/src/include/duckdb_static_extension.h"
  DESTINATION "${INSTALL_INCLUDE_DIR}")

install(
  TARGETS duckdb duckdb_static
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}"
  RUNTIME DESTINATION "${INSTALL_BIN_DIR}")
