include_directories(include)

add_definitions(-DHAVE_LINENOISE=1)
include_directories(../../third_party/utf8proc/include)
include_directories(linenoise/include)
add_definitions(-DUSE_DUCKDB_SHELL_WRAPPER)

include_directories(../../third_party/utf8proc/include)

if(_DUCKDB_PREBUILT_SHELL_PATH)
  add_library(duckdb_shell STATIC IMPORTED)
  set_property(TARGET duckdb_shell PROPERTY IMPORTED_LOCATION
                                            "${_DUCKDB_PREBUILT_SHELL_PATH}")
else()
  add_subdirectory(linenoise)
  set(SHELL_SOURCES
      shell_command_line_option.cpp
      shell_extension.cpp
      shell_grammar_extension.cpp
      shell.cpp
      shell_helpers.cpp
      shell_manual.cpp
      shell_metadata_command.cpp
      shell_prompt.cpp
      shell_renderer.cpp
      shell_highlight.cpp
      shell_profiler_renderer.cpp
      shell_progress_bar.cpp
      shell_render_table_metadata.cpp
      shell_windows.cpp
      $<TARGET_OBJECTS:duckdb_linenoise>)

  add_library(duckdb_shell STATIC ${SHELL_SOURCES})
  set_target_properties(
    duckdb_shell
    PROPERTIES ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/src"
               ARCHIVE_OUTPUT_DIRECTORY_DEBUG "${PROJECT_BINARY_DIR}/src"
               ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${PROJECT_BINARY_DIR}/src"
               ARCHIVE_OUTPUT_DIRECTORY_RELWITHDEBINFO
               "${PROJECT_BINARY_DIR}/src"
               ARCHIVE_OUTPUT_DIRECTORY_MINSIZEREL "${PROJECT_BINARY_DIR}/src")
  duckdb_make_native_lto_archive(duckdb_shell)
endif()

add_executable(shell shell_main.cpp)

target_link_libraries(shell duckdb_shell duckdb_static
                      ${DUCKDB_EXTRA_LINK_FLAGS})
link_threads(shell "")
if(STATIC_LIBCPP)
  message("Statically linking CLI")
  target_link_libraries(shell -static-libstdc++ -static-libgcc)
endif()

link_extension_libraries(shell "")

if(NOT AMALGAMATION_BUILD AND NOT WIN32)
  target_link_libraries(shell duckdb_utf8proc)
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

if(WIN32 AND NOT MINGW)
  string(TIMESTAMP DUCKDB_COPYRIGHT_YEAR "%Y")
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
  set(CMAKE_RC_FLAGS
      "${CMAKE_RC_FLAGS} -D DUCKDB_COPYRIGHT_YEAR=\"${DUCKDB_COPYRIGHT_YEAR}\"")
  target_sources(shell PRIVATE rc/duckdb.rc)
  # Opt into the Windows Segment Heap to avoid the default NT heap's lock
  # contention under multi-threaded allocation (see #24027). CMake merges
  # .manifest sources into the linker manifest via mt.exe.
  target_sources(shell PRIVATE rc/duckdb.manifest)
endif()

set_target_properties(shell PROPERTIES OUTPUT_NAME duckdb)
set_target_properties(shell PROPERTIES RUNTIME_OUTPUT_DIRECTORY
                                       ${PROJECT_BINARY_DIR})

if(NOT WIN32)
  configure_file("${PROJECT_SOURCE_DIR}/scripts/samply_profile_launcher.py.in"
                 "${PROJECT_BINARY_DIR}/profile" @ONLY)
  file(
    CHMOD
    "${PROJECT_BINARY_DIR}/profile"
    PERMISSIONS
    OWNER_READ
    OWNER_WRITE
    OWNER_EXECUTE
    GROUP_READ
    GROUP_EXECUTE
    WORLD_READ
    WORLD_EXECUTE)
endif()

duckdb_codesign_for_debugging(shell)

install(TARGETS shell RUNTIME DESTINATION "${INSTALL_BIN_DIR}")
