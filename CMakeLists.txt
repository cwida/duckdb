cmake_minimum_required(VERSION 3.14...3.29)

if(NOT CLANG_TIDY)
  find_package(Python3)
endif()

project(DuckDB)

find_package(Threads REQUIRED)

set(DUCKDB_SYSTEM_LIBS ${CMAKE_DL_LIBS})
if(MSVC OR MINGW)
  list(APPEND DUCKDB_SYSTEM_LIBS ws2_32 rstrtmgr)
endif()
if(MSVC)
  list(APPEND DUCKDB_SYSTEM_LIBS bcrypt)
endif()

# Consume optional vcpkg-related cache variables passed by CI/Makefile to avoid
# "manually-specified variables were not used by the project" warnings in
# extension-config-only runs.
if(DEFINED VCPKG_BUILD)
  set(_duckdb_vcpkg_build "${VCPKG_BUILD}")
endif()
if(DEFINED VCPKG_MANIFEST_DIR)
  set(_duckdb_vcpkg_manifest_dir "${VCPKG_MANIFEST_DIR}")
endif()

set(DUCKDB_MODULE_BASE_DIR "${CMAKE_CURRENT_LIST_DIR}")

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Copy compile_commands.json into <repo-root>/.cache/clangd/ after every
# successful build. .cache/ is intentionally outside build/ so it survives `make
# clean` (which does rm -rf build). The build-type name is derived from the last
# component of CMAKE_BINARY_DIR, so build dirs must be two levels below the repo
# root (e.g. build/debug, .cache/clangd/debug) for the repo-root derivation
# (CMAKE_BINARY_DIR/../..) to be correct. This works in both standalone duckdb
# builds and extension builds where duckdb/ is a submodule.
if(CMAKE_EXPORT_COMPILE_COMMANDS AND UNIX)
  get_filename_component(_clangd_build_name "${CMAKE_BINARY_DIR}" NAME)
  get_filename_component(_repo_root "${CMAKE_BINARY_DIR}/../.." ABSOLUTE)
  set(_clangd_cache_dir "${_repo_root}/.cache/clangd")
  add_custom_target(
    clangd_cache ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "${_clangd_cache_dir}/${_clangd_build_name}"
    COMMAND
      ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_BINARY_DIR}/compile_commands.json"
      "${_clangd_cache_dir}/${_clangd_build_name}/compile_commands.json"
    COMMAND
      ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_BINARY_DIR}/compile_commands.json"
      "${_clangd_cache_dir}/compile_commands.json"
    COMMENT "Updating .cache/clangd (${_clangd_build_name})"
    VERBATIM)
endif()

if(DEFINED CMAKE_CXX_STANDARD)
  set(_duckdb_allowed_cxx_std 17 20 23 26)
  if(NOT CMAKE_CXX_STANDARD IN_LIST _duckdb_allowed_cxx_std)
    message(
      FATAL_ERROR
        "DuckDB requires C++17 or higher; got CMAKE_CXX_STANDARD=${CMAKE_CXX_STANDARD}. "
        "Pass -DCMAKE_CXX_STANDARD=17 (or higher), or unset it to use the default."
    )
  endif()
endif()

set(CMAKE_CXX_STANDARD
    17
    CACHE STRING "Default C++ standard (minimum 17)")
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_VERBOSE_MAKEFILE OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_MACOSX_RPATH 1)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(NOT DEFINED CMAKE_C_COMPILER_LAUNCHER)
  find_program(COMPILER_LAUNCHER NAMES ccache sccache)
  if(COMPILER_LAUNCHER)
    message(STATUS "Using ${COMPILER_LAUNCHER} as C compiler launcher")
    set(CMAKE_C_COMPILER_LAUNCHER
        "${COMPILER_LAUNCHER}"
        CACHE STRING "" FORCE)
  endif()
endif()

if(NOT DEFINED CMAKE_CXX_COMPILER_LAUNCHER)
  find_program(COMPILER_LAUNCHER NAMES ccache sccache)
  if(COMPILER_LAUNCHER)
    message(STATUS "Using ${COMPILER_LAUNCHER} as C++ compiler launcher")
    set(CMAKE_CXX_COMPILER_LAUNCHER
        "${COMPILER_LAUNCHER}"
        CACHE STRING "" FORCE)
  endif()
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(DEFAULT_BUILD_TYPE "Release")
  message(STATUS "Setting build type to '${DEFAULT_BUILD_TYPE}'.")
  set(CMAKE_BUILD_TYPE
      "${DEFAULT_BUILD_TYPE}"
      CACHE STRING "Choose the type of build." FORCE)
endif()

if(NOT DUCKDB_LINKER
   AND DEFINED ENV{CI}
   AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_program(DUCKDB_MOLD_LINKER mold)
  if(DUCKDB_MOLD_LINKER)
    set(DUCKDB_LINKER "mold")
  endif()
endif()

if(DUCKDB_LINKER)
  set(DUCKDB_LINKER_FLAG "-fuse-ld=${DUCKDB_LINKER}")
  set(DUCKDB_LINKER_MESSAGE "Using linker from DUCKDB_LINKER=${DUCKDB_LINKER}")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${DUCKDB_LINKER_FLAG}")
  set(CMAKE_SHARED_LINKER_FLAGS
      "${CMAKE_SHARED_LINKER_FLAGS} ${DUCKDB_LINKER_FLAG}")
  set(CMAKE_MODULE_LINKER_FLAGS
      "${CMAKE_MODULE_LINKER_FLAGS} ${DUCKDB_LINKER_FLAG}")
  if(CMAKE_BUILD_TYPE STREQUAL "Release" AND DUCKDB_LINKER STREQUAL "mold")
    # --icf=safe was introduced in mold 1.1; older mold (e.g. the 1.0.3 shipped
    # in Ubuntu 22.04) aborts the link with "mold: unknown --icf argument:
    # safe". Probe the version and only enable identical code folding when the
    # linker actually supports it.
    execute_process(
      COMMAND ${DUCKDB_LINKER} --version
      OUTPUT_VARIABLE DUCKDB_MOLD_VERSION_OUTPUT
      ERROR_QUIET)
    if(DUCKDB_MOLD_VERSION_OUTPUT MATCHES "mold ([0-9]+\\.[0-9]+(\\.[0-9]+)?)")
      set(DUCKDB_MOLD_VERSION "${CMAKE_MATCH_1}")
      if(DUCKDB_MOLD_VERSION VERSION_GREATER_EQUAL "1.1")
        set(DUCKDB_MOLD_FLAGS "-Wl,--icf=safe")
        set(CMAKE_EXE_LINKER_FLAGS
            "${CMAKE_EXE_LINKER_FLAGS} ${DUCKDB_MOLD_FLAGS}")
        set(CMAKE_SHARED_LINKER_FLAGS
            "${CMAKE_SHARED_LINKER_FLAGS} ${DUCKDB_MOLD_FLAGS}")
        set(CMAKE_MODULE_LINKER_FLAGS
            "${CMAKE_MODULE_LINKER_FLAGS} ${DUCKDB_MOLD_FLAGS}")
        set(DUCKDB_LINKER_MESSAGE
            "${DUCKDB_LINKER_MESSAGE} with (${DUCKDB_MOLD_FLAGS})")
      else()
        set(DUCKDB_LINKER_MESSAGE
            "${DUCKDB_LINKER_MESSAGE} (mold ${DUCKDB_MOLD_VERSION} too old for --icf=safe, skipping ICF)"
        )
      endif()
    else()
      set(DUCKDB_LINKER_MESSAGE
          "${DUCKDB_LINKER_MESSAGE} (could not determine mold version, skipping ICF)"
      )
    endif()
  endif()
  message(STATUS "${DUCKDB_LINKER_MESSAGE}")
endif()

# Determine install paths default to gnu standard installation directories (lib,
# bin, include) https://cmake.org/cmake/help/latest/module/GNUInstallDirs.html
include(GNUInstallDirs)
set(INSTALL_LIB_DIR
    ${CMAKE_INSTALL_LIBDIR}
    CACHE PATH "Installation directory for libraries")
set(INSTALL_BIN_DIR
    ${CMAKE_INSTALL_BINDIR}
    CACHE PATH "Installation directory for executables")
set(INSTALL_INCLUDE_DIR
    ${CMAKE_INSTALL_INCLUDEDIR}
    CACHE PATH "Installation directory for header files")
if(WIN32 AND NOT CYGWIN)
  set(DEF_INSTALL_CMAKE_DIR cmake)
else()
  set(DEF_INSTALL_CMAKE_DIR ${INSTALL_LIB_DIR}/cmake/DuckDB)
endif()
set(INSTALL_CMAKE_DIR
    ${DEF_INSTALL_CMAKE_DIR}
    CACHE PATH "Installation directory for CMake files")
set(DUCKDB_EXPORT_SET "DuckDBExports")

option(
  EXTENSION_STATIC_BUILD
  "Extension build linking statically with DuckDB. Required for building linux loadable extensions."
  TRUE)

if(ZOS)
  set(EXTENSION_STATIC_BUILD TRUE)
endif()

if(WIN32)
  set(EXTENSION_STATIC_BUILD TRUE)
  add_definitions(-D_SILENCE_ALL_MS_EXT_DEPRECATION_WARNINGS=1)
endif()

# Split functions and data into separate sections in release builds so unused
# sections can be discarded by the linker
if("${CMAKE_CXX_COMPILER_ID}" MATCHES "GNU|Clang" AND NOT MSVC)
  set(CMAKE_CXX_FLAGS_RELEASE
      "${CMAKE_CXX_FLAGS_RELEASE} -ffunction-sections -fdata-sections")
elseif(WIN32 AND MSVC)
  set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /Gy")
endif()

option(SHADOW_FORBIDDEN_FUNCTIONS
       "Compile time test on usage of deprecated functions" FALSE)
if(SHADOW_FORBIDDEN_FUNCTIONS)
  set(CMAKE_CXX_FLAGS
      "${CMAKE_CXX_FLAGS} -include 'duckdb/common/shadow_forbidden_functions.hpp'"
  )
endif()

option(DISABLE_UNITY "Disable unity builds." FALSE)
option(USE_WASM_THREADS "Should threads be used" FALSE)
if(${USE_WASM_THREADS})
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -pthread")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread")
  set(WASM_THREAD_FLAGS -pthread -sSHARED_MEMORY=1)
endif()

option(FORCE_COLORED_OUTPUT
       "Always produce ANSI-colored output (GNU/Clang only)." FALSE)
if(${FORCE_COLORED_OUTPUT})
  if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
    add_compile_options(-fdiagnostics-color=always)
  elseif("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang$")
    add_compile_options(-fcolor-diagnostics)
  endif()
endif()

# Enable Clang thread-safety annotations when using Clang compiler
# TODO(hjiang): enable error for thread annotation warnings, because of certain
# extensions.
if("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang$")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wthread-safety")
endif()

if(DUCKDB_EXPLICIT_PLATFORM)
  add_definitions(-DDUCKDB_CUSTOM_PLATFORM=${DUCKDB_EXPLICIT_PLATFORM})
endif()

option(BUILD_COMPLETE_EXTENSION_SET
       "Whether we need to actually build the complete set" TRUE)
if(DEFINED ENV{BUILD_COMPLETE_EXTENSION_SET})
  set(BUILD_COMPLETE_EXTENSION_SET "$ENV{BUILD_COMPLETE_EXTENSION_SET}")
endif()

option(WASM_ENABLED "Are DuckDB-Wasm extensions build enabled" FALSE)
if(DEFINED ENV{WASM_EXTENSIONS})
  set(WASM_ENABLED "$ENV{WASM_EXTENSIONS}")
endif()
option(MUSL_ENABLED "Are Musl extensions build enabled" FALSE)
if(DEFINED ENV{DUCKDB_PLATFORM})
  if("$ENV{DUCKDB_PLATFORM}" STREQUAL "linux_amd64_musl"
     OR "$ENV{DUCKDB_PLATFORM}" STREQUAL "linux_arm64_musl")
    set(MUSL_ENABLED ON)
  endif()
endif()

if(MUSL_ENABLED)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D__MUSL_ENABLED__")
  add_link_options(-Wl,-z,stack-size=8388608)
endif()

set(M32_FLAG "")
if(FORCE_32_BIT)
  set(M32_FLAG " -m32 ")
endif()

set(OS_NAME "unknown")
set(OS_ARCH "amd64")

string(REGEX MATCH "(arm64|aarch64)" IS_ARM "${CMAKE_SYSTEM_PROCESSOR}")
if(IS_ARM)
  set(OS_ARCH "arm64")
elseif(FORCE_32_BIT)
  set(OS_ARCH "i386")
endif()

if(APPLE)
  set(OS_NAME "osx")
elseif(WIN32)
  set(OS_NAME "windows")
elseif(UNIX)
  set(OS_NAME "linux") # sorry BSD
else()
  set(OS_NAME "unknown")
endif()

# Codesign a target with get-task-allow so lldb can attach to it on macOS. Only
# applied for debug build types where you'd actually want to debug.
function(duckdb_codesign_for_debugging target)
  if(APPLE AND (CMAKE_BUILD_TYPE STREQUAL "Debug" OR CMAKE_BUILD_TYPE STREQUAL
                                                     "RelWithDebInfo"))
    set(ENTITLEMENTS_FILE "${CMAKE_BINARY_DIR}/debug_entitlements.plist")
    if(NOT EXISTS "${ENTITLEMENTS_FILE}")
      file(
        WRITE "${ENTITLEMENTS_FILE}"
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"https://www.apple.com/DTDs/PropertyList-1.0.dtd\">"
        "<plist version=\"1.0\"><dict><key>com.apple.security.get-task-allow</key><true/></dict></plist>"
      )
    endif()
    set(CODESIGN_SCRIPT "${CMAKE_BINARY_DIR}/codesign_debug.cmake")
    if(NOT EXISTS "${CODESIGN_SCRIPT}")
      file(
        WRITE "${CODESIGN_SCRIPT}"
        "execute_process(COMMAND codesign -s - -f --entitlements \"\${ENTITLEMENTS_FILE}\" \"\${TARGET_FILE}\" ERROR_QUIET)\n"
      )
    endif()
    add_custom_command(
      TARGET ${target}
      POST_BUILD
      COMMAND ${CMAKE_COMMAND} -DTARGET_FILE=$<TARGET_FILE:${target}>
              -DENTITLEMENTS_FILE=${ENTITLEMENTS_FILE} -P "${CODESIGN_SCRIPT}"
      VERBATIM)
  endif()
endfunction()

option(FORCE_WARN_UNUSED "Unused code objects lead to compiler warnings." FALSE)

option(
  EXTENSION_TESTS_ONLY
  "Only load the tests for extensions, don't actually build them; useful for testing loadable extensions"
  FALSE)
set(EXTENSION_DIRECTORIES
    "~/.duckdb/extensions"
    CACHE STRING
          "Default extension directories (;-separated list on win, : on unix)")
option(WASM_LOADABLE_EXTENSIONS "WebAssembly build with loadable extensions."
       FALSE)
option(ENABLE_SANITIZER "Enable address sanitizer." TRUE)
option(ENABLE_THREAD_SANITIZER "Enable thread sanitizer." FALSE)
option(ENABLE_UBSAN "Enable undefined behavior sanitizer." TRUE)
option(
  DISABLE_VPTR_SANITIZER
  "Disable vptr sanitizer; work-around for sanitizer false positive on Macbook M1"
  FALSE)
option(STANDALONE_DEBUG "add clang compile option -fstandalone-debug" FALSE)
option(RELEASE_SANITIZER "Enable address sanitizer in release mode." FALSE)
option(COVERAGE "Enable clang source-based code coverage instrumentation."
       FALSE)

if(COVERAGE)
  if(NOT "${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang$")
    message(FATAL_ERROR "COVERAGE=1 requires a clang-compatible compiler")
  endif()
  set(CMAKE_C_FLAGS
      "${CMAKE_C_FLAGS} -fprofile-instr-generate -fcoverage-mapping")
  set(CMAKE_CXX_FLAGS
      "${CMAKE_CXX_FLAGS} -fprofile-instr-generate -fcoverage-mapping")
  set(CMAKE_EXE_LINKER_FLAGS
      "${CMAKE_EXE_LINKER_FLAGS} -fprofile-instr-generate")
  set(CMAKE_SHARED_LINKER_FLAGS
      "${CMAKE_SHARED_LINKER_FLAGS} -fprofile-instr-generate")
  set(CMAKE_MODULE_LINKER_FLAGS
      "${CMAKE_MODULE_LINKER_FLAGS} -fprofile-instr-generate")
endif()

if(${ENABLE_THREAD_SANITIZER})
  if(${ENABLE_SANITIZER})
    message(
      WARNING
        "Both thread and address sanitizers are enabled. This is not supported. The address sanitizer will be disabled, and we will run with only the thread sanitizer."
    )
  endif()
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DDUCKDB_THREAD_SANITIZER")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=thread")
elseif(${ENABLE_SANITIZER})
  if(FORCE_ASSERT OR RELEASE_SANITIZER)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address")
  else()
    set(CXX_EXTRA_DEBUG "${CXX_EXTRA_DEBUG} -fsanitize=address")
  endif()
endif()

if(NOT ${DISABLE_VPTR_SANITIZER})
  if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64")
    if("${CMAKE_CXX_COMPILER_VERSION}" VERSION_GREATER 14.0)
      message(
        WARNING
          "Not disabling vptr sanitizer on M1 Macbook - set DISABLE_VPTR_SANITIZER manually if you run into issues with false positives in the sanitizer"
      )
    else()
      set(DISABLE_VPTR_SANITIZER TRUE)
    endif()
  endif()
endif()

if(${ENABLE_UBSAN})
  if(${ENABLE_THREAD_SANITIZER})
    message(
      WARNING
        "Both thread and undefined sanitizers are enabled. This is not supported. The undefined sanitizer will be disabled, and we will run with only the thread sanitizer."
    )
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DDUCKDB_THREAD_SANITIZER")
  else()
    if(FORCE_ASSERT)
      set(CMAKE_CXX_FLAGS
          "${CMAKE_CXX_FLAGS} -fsanitize=undefined -fno-sanitize-recover=all")
      if(${DISABLE_VPTR_SANITIZER})
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-sanitize=vptr")
      endif()
    else()
      set(CXX_EXTRA_DEBUG
          "${CXX_EXTRA_DEBUG} -fsanitize=undefined -fno-sanitize-recover=all")
      if(${DISABLE_VPTR_SANITIZER})
        set(CXX_EXTRA_DEBUG "${CXX_EXTRA_DEBUG} -fno-sanitize=vptr")
      endif()
    endif()
  endif()
endif()

option(EXPLICIT_EXCEPTIONS "Explicitly enable C++ exceptions." FALSE)
if(${EXPLICIT_EXCEPTIONS})
  set(CXX_EXTRA "${CXX_EXTRA} -fexceptions")
endif()

option(EXPORT_DYNAMIC_SYMBOLS "Export dynamic symbols." FALSE)
if(${EXPORT_DYNAMIC_SYMBOLS})
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -rdynamic")
endif()

set(VERSIONING_TAG_MATCH "v*.*.*")

if(UNSAFE_NUMERIC_CAST)
  add_definitions(-DUNSAFE_NUMERIC_CAST=1)
endif()
foreach(REMOVED_OPTION ENABLE_EXTENSION_AUTOLOADING
                       ENABLE_EXTENSION_AUTOINSTALL)
  if(DEFINED ${REMOVED_OPTION})
    message(
      WARNING
        "${REMOVED_OPTION} has no effect: autoload_known_extensions and autoinstall_known_extensions default to true, change them at runtime instead"
    )
    unset(${REMOVED_OPTION} CACHE)
  endif()
endforeach()

option(
  OSX_BUILD_UNIVERSAL
  "Build both architectures on OSX and create a single binary containing both."
  FALSE)
if(OSX_BUILD_UNIVERSAL)
  if(NOT APPLE)
    message(FATAL_ERROR "This only makes sense on OSX")
  endif()
  set(CMAKE_OSX_ARCHITECTURES
      "x86_64;arm64"
      CACHE STRING "Build architectures for Mac OS X" FORCE)
  set(CMAKE_OSX_DEPLOYMENT_TARGET
      11.0
      CACHE STRING "Minimum OS X deployment version" FORCE)
endif()

if(OSX_BUILD_ARCH)
  message(STATUS "building for OSX architecture: ${OSX_BUILD_ARCH}")
  if(NOT APPLE)
    message(FATAL_ERROR "This only makes sense on OSX")
  endif()
  set(CMAKE_OSX_ARCHITECTURES
      "${OSX_BUILD_ARCH}"
      CACHE STRING "Build architectures for Mac OS X" FORCE)
  set(CMAKE_OSX_DEPLOYMENT_TARGET
      11.0
      CACHE STRING "Minimum OS X deployment version" FORCE)
endif()

set(SUN FALSE)
if(${CMAKE_SYSTEM_NAME} STREQUAL "SunOS")
  set(CXX_EXTRA "${CXX_EXTRA} -mimpure-text")
  add_definitions(-DSUN=1)
  set(SUN TRUE)
endif()

include(CheckCXXSymbolExists)
check_cxx_symbol_exists(fdatasync "unistd.h" DUCKDB_HAVE_FDATASYNC)
if(DUCKDB_HAVE_FDATASYNC)
  add_definitions(-DDUCKDB_HAVE_FDATASYNC=1)
endif()

if(NOT DUCKDB_EXPLICIT_VERSION
   AND DEFINED ENV{DUCKDB_VERSION}
   AND NOT "$ENV{DUCKDB_VERSION}" STREQUAL "")
  set(DUCKDB_EXPLICIT_VERSION "$ENV{DUCKDB_VERSION}")
endif()
if(NOT DUCKDB_EXPLICIT_VERSION AND OVERRIDE_GIT_DESCRIBE)
  set(DUCKDB_EXPLICIT_VERSION "${OVERRIDE_GIT_DESCRIBE}")
endif()
if(NOT DUCKDB_EXPLICIT_VERSION
   AND DEFINED ENV{OVERRIDE_GIT_DESCRIBE}
   AND NOT "$ENV{OVERRIDE_GIT_DESCRIBE}" STREQUAL "")
  set(DUCKDB_EXPLICIT_VERSION "$ENV{OVERRIDE_GIT_DESCRIBE}")
endif()
if(NOT DEFINED GIT_COMMIT_HASH
   AND DEFINED ENV{DUCKDB_COMMIT}
   AND NOT "$ENV{DUCKDB_COMMIT}" STREQUAL "")
  set(GIT_COMMIT_HASH "$ENV{DUCKDB_COMMIT}")
endif()

find_package(Git)
if(Git_FOUND AND NOT DEFINED GIT_COMMIT_HASH)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} log -1 --format=%H
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    RESULT_VARIABLE GIT_HASH_RESULT
    OUTPUT_VARIABLE GIT_COMMIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(GIT_HASH_RESULT)
    unset(GIT_COMMIT_HASH)
    message(
      WARNING
        "git is available (at ${GIT_EXECUTABLE}) but has failed to execute 'log -1 --format=%H'. "
        "Consider providing DUCKDB_COMMIT")
  endif()
endif()
if(NOT DEFINED GIT_COMMIT_HASH)
  set(GIT_COMMIT_HASH "0123456789")
endif()
string(LENGTH "${GIT_COMMIT_HASH}" GIT_COMMIT_HASH_LENGTH)
if(NOT GIT_COMMIT_HASH MATCHES "^[a-fA-F0-9]+$"
   OR GIT_COMMIT_HASH_LENGTH LESS 10
   OR GIT_COMMIT_HASH_LENGTH GREATER 64)
  message(FATAL_ERROR "Invalid DuckDB commit '${GIT_COMMIT_HASH}'")
endif()
string(SUBSTRING "${GIT_COMMIT_HASH}" 0 10 GIT_COMMIT_HASH)
string(TOLOWER "${GIT_COMMIT_HASH}" GIT_COMMIT_HASH)

if(DUCKDB_EXPLICIT_VERSION)
  set(DUCKDB_VERSION "${DUCKDB_EXPLICIT_VERSION}")
  if(DUCKDB_VERSION MATCHES
     "^v([0-9]+)[.]([0-9]+)[.]([0-9]+)(-(alpha|rc|dev)([0-9]+))?$")
    set(DUCKDB_MAJOR_VERSION "${CMAKE_MATCH_1}")
    set(DUCKDB_MINOR_VERSION "${CMAKE_MATCH_2}")
    set(DUCKDB_PATCH_VERSION "${CMAKE_MATCH_3}")
    if(CMAKE_MATCH_5 STREQUAL "dev")
      set(DUCKDB_DEV_ITERATION "${CMAKE_MATCH_6}")
    else()
      set(DUCKDB_DEV_ITERATION "0")
    endif()
  else()
    set(DUCKDB_DEV_ITERATION "0")
  endif()
else()
  set(DUCKDB_RELEASE_VERSION_FILE
      "${CMAKE_CURRENT_SOURCE_DIR}/scripts/ci/release_version.txt")
  file(READ "${DUCKDB_RELEASE_VERSION_FILE}" DUCKDB_RELEASE_VERSION)
  string(STRIP "${DUCKDB_RELEASE_VERSION}" DUCKDB_RELEASE_VERSION)
  if(NOT DUCKDB_RELEASE_VERSION MATCHES "^([0-9]+)[.]([0-9]+)$")
    message(
      FATAL_ERROR
        "Invalid release version '${DUCKDB_RELEASE_VERSION}' in ${DUCKDB_RELEASE_VERSION_FILE}"
    )
  endif()
  set(DUCKDB_MAJOR_VERSION "${CMAKE_MATCH_1}")
  set(DUCKDB_MINOR_VERSION "${CMAKE_MATCH_2}")
  set(DUCKDB_PATCH_VERSION "0")
  set(DUCKDB_DEV_ITERATION "0")
  if(Git_FOUND)
    execute_process(
      COMMAND ${GIT_EXECUTABLE} rev-list --count HEAD
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      RESULT_VARIABLE GIT_COUNT_RESULT
      OUTPUT_VARIABLE DUCKDB_DEV_ITERATION
      OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(GIT_COUNT_RESULT)
      set(DUCKDB_DEV_ITERATION "0")
    endif()
  endif()
  set(DUCKDB_VERSION "v${DUCKDB_RELEASE_VERSION}.0-dev${DUCKDB_DEV_ITERATION}")
endif()

set(DUCKDB_VERSION_NUMBER
    "${DUCKDB_MAJOR_VERSION}.${DUCKDB_MINOR_VERSION}.${DUCKDB_PATCH_VERSION}")

if(DUCKDB_VERSION MATCHES "-dev[0-9]+$")
  set(DUCKDB_NORMALIZED_VERSION "${GIT_COMMIT_HASH}")
else()
  set(DUCKDB_NORMALIZED_VERSION "${DUCKDB_VERSION}")
endif()

if(EMSCRIPTEN)
  set(EXTENSION_POSTFIX ".wasm")
else()
  set(EXTENSION_POSTFIX "")
endif()

message(
  STATUS
    "git hash ${GIT_COMMIT_HASH}, version ${DUCKDB_VERSION}, extension folder ${DUCKDB_NORMALIZED_VERSION}"
)

option(BUILD_MAIN_DUCKDB_LIBRARY
       "Build the main duckdb library and executable." TRUE)
option(BUILD_EXTENSIONS_ONLY
       "Build all extension as linkable, and don't build core." FALSE)
option(BUILD_BENCHMARKS "Enable building of the benchmark suite." FALSE)
option(GENERATE_EXTENSION_ENTRIES "Build for generating extension_entries.hpp"
       FALSE)
option(FORCE_QUERY_LOG
       "If enabled, all queries will be logged to the specified path" OFF)
option(BUILD_SHELL "Build the DuckDB Shell and SQLite API Wrappers" TRUE)
option(DISABLE_THREADS "Disable support for multi-threading" FALSE)
option(
  ENABLE_EXTENSION_LOAD
  "Build libduckdb_loadable_extensions.a and link it into the DuckDB targets, so they can install and load external extensions"
  TRUE)
option(DISABLE_EXTENSION_LOAD
       "Deprecated, use ENABLE_EXTENSION_LOAD=OFF instead" FALSE)
option(
  ENABLE_BUILTIN_HTTPLIB
  "Build libduckdb_httplib.a and link it into the DuckDB targets as their default HTTP client"
  TRUE)
option(DISABLE_BUILTIN_HTTPLIB
       "Deprecated, use ENABLE_BUILTIN_HTTPLIB=OFF instead" FALSE)
option(LINK_CORE_EXTENSIONS
       "Also statically link the extensions named in BUILD_EXTENSIONS" FALSE)
set(STATICALLY_LINK_EXTENSIONS
    ""
    CACHE
      STRING
      "Extensions the DuckDB targets link, e.g. 'json parquet' or 'none' (empty: the built ones the extension configs list with duckdb_extension_statically_link)"
)
set(DUCKDB_CAPABILITIES
    ""
    CACHE
      STRING
      "Capabilities the DuckDB targets link: 'httplib', 'loadable_extensions', 'local_extension_repository' or 'none' (empty: all of them)"
)
option(DISABLE_STR_INLINE "Debug setting: disable inlining of strings" FALSE)
option(DISABLE_MEMORY_SAFETY
       "Debug setting: disable memory access checks at runtime" FALSE)
option(DISABLE_ASSERTIONS "Debug setting: disable assertions" FALSE)
option(ALTERNATIVE_VERIFY "Debug setting: use alternative verify mode" FALSE)
option(DISABLE_POINTER_SALT
       "Debug setting: verify correct results without pointer salt" FALSE)
option(
  HASH_ZERO
  "Debug setting: verify hash collision resolution by setting all hashes to 0"
  FALSE)
option(RUN_SLOW_VERIFIERS
       "Debug setting: enable a more extensive set of verifiers" FALSE)
option(DESTROY_UNPINNED_BLOCKS
       "Debug setting: destroy unpinned buffer-managed blocks" FALSE)
option(
  FORCE_ASYNC_SINK_SOURCE
  "Debug setting: forces sinks/sources to block the first 2 times they're called"
  FALSE)
option(
  DEBUG_ALLOCATION
  "Debug setting: keep track of outstanding allocations to detect memory leaks"
  FALSE)
option(DEBUG_STACKTRACE
       "Debug setting: print a stracktrace on asserts and when testing crashes"
       FALSE)
option(DEBUG_MOVE "Debug setting: Ensure std::move is being used" FALSE)
option(
  VERIFY_VECTOR
  "Debug setting: verify vectors (options: dictionary_expression, dictionary_operator, constant_operator, sequence_operator, nested_shuffle, variant_vector)"
  "none")
option(
  CLANG_TIDY
  "Enable build for clang-tidy, this disables all source files excluding the core database. This does not produce a working build."
  FALSE)
option(BUILD_UNITTESTS "Build the unittest runner." TRUE)
option(ENABLE_UNITTEST_CPP_TESTS "Build the C++ Unit Tests." TRUE)
option(NATIVE_LTO_STATIC_LIBRARIES
       "Run LTO before writing static libraries so non-LTO linkers can use them"
       OFF)
if(NATIVE_LTO_STATIC_LIBRARIES)
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux"
     OR NOT "${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang$"
     OR NOT CMAKE_LTO)
    message(
      FATAL_ERROR
        "NATIVE_LTO_STATIC_LIBRARIES requires a Linux Clang build with CMAKE_LTO set"
    )
  endif()
  find_program(DUCKDB_LLD_LINKER NAMES ld.lld)
  if(NOT DUCKDB_LLD_LINKER)
    message(
      FATAL_ERROR
        "NATIVE_LTO_STATIC_LIBRARIES requires ld.lld to produce native relocatable objects"
    )
  endif()
endif()
option(
  EXTENSION_CONFIG_BUILD
  "Produce extension configuration artifacts instead of building. (such as shared vcpkg.json, extensions.txt)"
  FALSE)
option(
  CRASH_ON_ASSERT
  "Trigger a sigabort on an assert failing, instead of throwing an exception"
  FALSE)
option(FORCE_ASSERT "Enable checking of assertions, even in release mode" FALSE)
option(FORCE_DEBUG "Force adding a debug define, even in release mode" FALSE)

option(TREAT_WARNINGS_AS_ERRORS "Treat warnings as errors" FALSE)
option(EXPORT_DLL_SYMBOLS "Export dll symbols on Windows, else import" TRUE)
option(BUILD_RDTSC "Enable the rdtsc instruction." FALSE)
set(SMALLER_BINARY
    ""
    CACHE
      STRING
      "Produce a smaller binary by trimming specialized code paths. This can negatively affect performance. Set to ON/1 to trim everything, or to a comma-separated list of feature or group names. Use 'list' to print the available names."
)
set(SMALLER_BINARY_EXCEPT
    ""
    CACHE STRING
          "Feature or group names to keep, subtracted from SMALLER_BINARY.")
option(NATIVE_ARCH "Compile targeting the native architecture" FALSE)
set(DUCKDB_OPTIMIZATION_PROFILE
    "CLI"
    CACHE STRING "Release architecture profile: CLI, EXTENSION, or NONE")
set_property(CACHE DUCKDB_OPTIMIZATION_PROFILE PROPERTY STRINGS CLI EXTENSION
                                                        NONE)
option(ENABLE_JEMALLOC
       "Enable jemalloc as the memory allocator (only on supported platforms)"
       TRUE)
option(OVERRIDE_NEW_DELETE
       "Override C++ new/delete (only when jemalloc is enabled)" FALSE)
option(
  SET_DUCKDB_LIBRARY_VERSION
  "Whether or not to set the DuckDB Library version and emit versioned shared libraries"
  FALSE)
set(PREBUILT_BINARY
    ""
    CACHE FILEPATH "Prebuilt DuckDB static library")
set(PREBUILT_EXTENSIONS
    ""
    CACHE STRING "Extensions supplied as prebuilt static libraries")
if(PREBUILT_EXTENSIONS AND NOT PREBUILT_BINARY)
  message(FATAL_ERROR "PREBUILT_EXTENSIONS requires PREBUILT_BINARY")
endif()

if(DISABLE_EXTENSION_LOAD)
  message(
    WARNING
      "DISABLE_EXTENSION_LOAD is deprecated, use ENABLE_EXTENSION_LOAD=OFF")
  set(ENABLE_EXTENSION_LOAD FALSE)
endif()

if(DISABLE_BUILTIN_HTTPLIB)
  message(
    WARNING
      "DISABLE_BUILTIN_HTTPLIB is deprecated, use ENABLE_BUILTIN_HTTPLIB=OFF")
  set(ENABLE_BUILTIN_HTTPLIB FALSE)
endif()
if(NOT ENABLE_EXTENSION_LOAD)
  set(ENABLE_BUILTIN_HTTPLIB FALSE)
endif()

if(${BUILD_RDTSC})
  add_compile_definitions(RDTSC)
endif()

if(BUILD_EXTENSIONS_ONLY)
  set(BUILD_MAIN_DUCKDB_LIBRARY FALSE)
endif()

if(EXTENSION_CONFIG_BUILD)
  set(BUILD_MAIN_DUCKDB_LIBRARY FALSE)
endif()

if(EXTENSION_CONFIG_BUILD AND TARGET clangd_cache)
  set_target_properties(clangd_cache PROPERTIES EXCLUDE_FROM_ALL 1)
endif()

if(NOT BUILD_MAIN_DUCKDB_LIBRARY)
  set(BUILD_UNITTESTS FALSE)
  set(BUILD_SHELL FALSE)
  set(STATICALLY_LINK_EXTENSIONS core_functions)
endif()

if(DISABLE_BUILTIN_EXTENSIONS)
  message(
    WARNING
      "DISABLE_BUILTIN_EXTENSIONS is deprecated, use STATICALLY_LINK_EXTENSIONS=core_functions"
  )
  unset(DISABLE_BUILTIN_EXTENSIONS CACHE)
  if("${STATICALLY_LINK_EXTENSIONS}" STREQUAL "")
    set(STATICALLY_LINK_EXTENSIONS core_functions)
  endif()
endif()

if(GENERATE_EXTENSION_ENTRIES)
  set(STATICALLY_LINK_EXTENSIONS none)
endif()

if(TREAT_WARNINGS_AS_ERRORS)
  message("Treating warnings as errors.")
endif()

if(NATIVE_ARCH)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -mcpu=native")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mcpu=native")
  else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -march=native")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=native")
  endif()
endif()

if(OVERRIDE_NEW_DELETE)
  if(NOT ENABLE_JEMALLOC)
    message(FATAL_ERROR "Cannot override new/delete without jemalloc enabled")
  endif()
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DDUCKDB_OVERRIDE_NEW_DELETE")
endif()

if(CRASH_ON_ASSERT)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DDUCKDB_CRASH_ON_ASSERT")
endif()

if(DISABLE_STR_INLINE)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DDUCKDB_DEBUG_NO_INLINE")
endif()

if(DISABLE_MEMORY_SAFETY)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DDUCKDB_DEBUG_NO_SAFETY")
endif()

if(DISABLE_ASSERTIONS)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DDISABLE_ASSERTIONS")
endif()

# Build without C++ RTTI. A dependency that cannot build this way can re-enable
# it for its own targets, but only if its class hierarchy is self-contained: a
# type deriving from a duckdb type needs the base's typeinfo, which this build
# never emits.
if(DISABLE_RTTI)
  if(MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /GR-")
  else()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti")
  endif()
endif()

if(DESTROY_UNPINNED_BLOCKS)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DDUCKDB_DEBUG_DESTROY_BLOCKS")
endif()

if(FORCE_ASYNC_SINK_SOURCE)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DDUCKDB_DEBUG_ASYNC_SINK_SOURCE")
endif()

if(RUN_SLOW_VERIFIERS)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DDUCKDB_RUN_SLOW_VERIFIERS")
endif()

if(ALTERNATIVE_VERIFY)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DDUCKDB_ALTERNATIVE_VERIFY")
endif()

if(DISABLE_POINTER_SALT)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DDUCKDB_DISABLE_POINTER_SALT")
endif()

if(HASH_ZERO)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DDUCKDB_HASH_ZERO")
endif()

if(LATEST_STORAGE)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DDUCKDB_LATEST_STORAGE")
endif()

if(DEBUG_ALLOCATION)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DDUCKDB_DEBUG_ALLOCATION")
endif()

if(DEBUG_MOVE)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DDUCKDB_DEBUG_MOVE")
endif()

if(CLANG_TIDY)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DDUCKDB_CLANG_TIDY")
endif()

# Parse the feature registry in smaller_binary.hpp, which is the single source
# of truth for both the feature names and their group membership.
set(SB_REGISTRY_HEADER
    "${CMAKE_CURRENT_SOURCE_DIR}/src/include/duckdb/common/smaller_binary.hpp")
file(STRINGS "${SB_REGISTRY_HEADER}" SB_REGISTRY_LINES
     REGEX "^#define DUCKDB_SB_FEATURE_")
set(SB_ALL_FEATURES "")
set(SB_ALL_GROUPS "")
foreach(SB_LINE ${SB_REGISTRY_LINES})
  if(NOT
     SB_LINE
     MATCHES
     "^#define DUCKDB_SB_FEATURE_([A-Za-z0-9_]+) +DUCKDB_SB_DEFAULT.*// *group: *([A-Za-z0-9_]+)"
  )
    message(FATAL_ERROR "Malformed entry in ${SB_REGISTRY_HEADER}: ${SB_LINE}")
  endif()
  list(APPEND SB_ALL_FEATURES "${CMAKE_MATCH_1}")
  list(APPEND SB_ALL_GROUPS "${CMAKE_MATCH_2}")
  list(APPEND SB_GROUP_${CMAKE_MATCH_2} "${CMAKE_MATCH_1}")
endforeach()
list(REMOVE_DUPLICATES SB_ALL_GROUPS)

# Expands feature names, group names and the 'all' alias into a flat list of
# feature names.
function(sb_resolve_names OUT_VAR OPTION_NAME)
  set(SB_RESULT "")
  string(REPLACE "," ";" SB_NAMES "${ARGN}")
  foreach(SB_NAME ${SB_NAMES})
    string(STRIP "${SB_NAME}" SB_NAME)
    if(SB_NAME STREQUAL "")
      continue()
    elseif(SB_NAME STREQUAL "all")
      list(APPEND SB_RESULT ${SB_ALL_FEATURES})
    elseif(SB_NAME IN_LIST SB_ALL_FEATURES)
      list(APPEND SB_RESULT "${SB_NAME}")
    elseif(SB_NAME IN_LIST SB_ALL_GROUPS)
      list(APPEND SB_RESULT ${SB_GROUP_${SB_NAME}})
    else()
      string(REPLACE ";" ", " SB_FEATURE_HELP "${SB_ALL_FEATURES}")
      string(REPLACE ";" ", " SB_GROUP_HELP "${SB_ALL_GROUPS}")
      message(
        FATAL_ERROR
          "Unknown ${OPTION_NAME} name '${SB_NAME}'.\n"
          "  groups:   all, ${SB_GROUP_HELP}\n"
          "  features: ${SB_FEATURE_HELP}")
    endif()
  endforeach()
  list(REMOVE_DUPLICATES SB_RESULT)
  set(${OUT_VAR}
      "${SB_RESULT}"
      PARENT_SCOPE)
endfunction()

if(SMALLER_BINARY STREQUAL "list")
  string(REPLACE ";" "\n    " SB_FEATURE_HELP "${SB_ALL_FEATURES}")
  string(REPLACE ";" "\n    " SB_GROUP_HELP "${SB_ALL_GROUPS}")
  message(FATAL_ERROR "SMALLER_BINARY groups:\n    all\n    ${SB_GROUP_HELP}\n"
                      "SMALLER_BINARY features:\n    ${SB_FEATURE_HELP}")
endif()

if(SMALLER_BINARY)
  if(SMALLER_BINARY MATCHES "^(1|ON|On|on|TRUE|True|true|YES|Yes|yes|Y|y)$")
    set(SB_ENABLED ${SB_ALL_FEATURES})
  else()
    sb_resolve_names(SB_ENABLED "SMALLER_BINARY" "${SMALLER_BINARY}")
  endif()
  if(SMALLER_BINARY_EXCEPT)
    sb_resolve_names(SB_EXCEPT "SMALLER_BINARY_EXCEPT"
                     "${SMALLER_BINARY_EXCEPT}")
    list(REMOVE_ITEM SB_ENABLED ${SB_EXCEPT})
  endif()
elseif(SMALLER_BINARY_EXCEPT)
  message(FATAL_ERROR "SMALLER_BINARY_EXCEPT is set but SMALLER_BINARY is not")
else()
  set(SB_ENABLED "")
endif()

if(SB_ENABLED)
  # Emit the umbrella when everything is trimmed, otherwise flip on the
  # requested features one by one. The registry entries are #ifndef-guarded, so
  # these -D flags win over the defaults.
  list(LENGTH SB_ENABLED SB_ENABLED_COUNT)
  list(LENGTH SB_ALL_FEATURES SB_TOTAL_COUNT)
  if(SB_ENABLED_COUNT EQUAL SB_TOTAL_COUNT)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DDUCKDB_SMALLER_BINARY_ALL")
  else()
    foreach(SB_FEATURE ${SB_ENABLED})
      set(CMAKE_CXX_FLAGS
          "${CMAKE_CXX_FLAGS} -DDUCKDB_SB_FEATURE_${SB_FEATURE}=DUCKDB_SB_ON")
    endforeach()
  endif()
  string(REPLACE ";" ", " SB_ENABLED_MSG "${SB_ENABLED}")
  message(
    STATUS
      "Smaller binary: trimming ${SB_ENABLED_COUNT}/${SB_TOTAL_COUNT} features (${SB_ENABLED_MSG})"
  )
endif()

if(FORCE_ASSERT)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DDUCKDB_FORCE_ASSERT")
endif()

if(FORCE_DEBUG)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DDEBUG")
endif()

if(STANDALONE_DEBUG)
  if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug" OR NOT "${CMAKE_CXX_COMPILER_ID}"
                                              MATCHES "Clang$")
    message(FATAL_ERROR "Only clang compiler supports -fstandalone-debug")
  endif()
  add_compile_options(-fstandalone-debug)
endif()

function(is_number input_string return_var)
  if("${input_string}" MATCHES "^[0-9]+$")
    set(${return_var}
        TRUE
        PARENT_SCOPE)
  else()
    set(${return_var}
        FALSE
        PARENT_SCOPE)
  endif()
endfunction()

set(STANDARD_VECTOR_SIZE
    ""
    CACHE STRING "Set a custom STANDARD_VECTOR_SIZE at compile time")

if(DEFINED STANDARD_VECTOR_SIZE AND NOT STANDARD_VECTOR_SIZE STREQUAL "")
  is_number(${STANDARD_VECTOR_SIZE} is_number_result)
  if(is_number_result)
    set(CMAKE_CXX_FLAGS
        "${CMAKE_CXX_FLAGS} -DSTANDARD_VECTOR_SIZE=${STANDARD_VECTOR_SIZE}")
    message(STATUS "STANDARD_VECTOR_SIZE is set to ${STANDARD_VECTOR_SIZE}")
  else()
    message(
      FATAL_ERROR
        "STANDARD_VECTOR_SIZE must be a number, not ${STANDARD_VECTOR_SIZE}")
  endif()
endif()

if(NOT MSVC)
  set(CXX_EXTRA_DEBUG
      "${CXX_EXTRA_DEBUG} -Wunused -Werror=vla -Wnarrowing -pedantic")

  if(${FORCE_WARN_UNUSED})
    set(CXX_EXTRA "${CXX_EXTRA} -Wunused")
  endif()
  if(TREAT_WARNINGS_AS_ERRORS)
    set(CXX_EXTRA "${CXX_EXTRA} -Werror")
  endif()
  if(FORCE_DEBUG)
    set(CXX_EXTRA "${CXX_EXTRA} -Wall ${CXX_EXTRA_DEBUG}")
  endif()

  if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION
                                                   VERSION_GREATER 8.0)
    if(REDUCE_SYMBOLS)
      set(CXX_EXTRA "${CXX_EXTRA} -g1 -gsplit-dwarf")
    endif()
  elseif("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang$"
         AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER 9.0)
    if(REDUCE_SYMBOLS)
      set(CXX_EXTRA "${CXX_EXTRA} -gline-tables-only")
    endif()
  else()
    message(WARNING "Please use a recent compiler for debug builds")
  endif()

  set(CMAKE_CXX_FLAGS_DEBUG
      "${CMAKE_CXX_FLAGS_DEBUG} -g -O0 -DDEBUG -Wall ${M32_FLAG} ${CXX_EXTRA}")
  set(CMAKE_CXX_FLAGS_RELEASE
      "${CMAKE_CXX_FLAGS_RELEASE} -O3 -DNDEBUG ${M32_FLAG} ${CXX_EXTRA}")

  set(CMAKE_LTO_JOBS
      ""
      CACHE STRING "Number of LTO backend jobs")
  if("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang$" AND CMAKE_LTO)
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -flto=${CMAKE_LTO}")
    if(CMAKE_LTO_JOBS AND CMAKE_LTO STREQUAL "thin")
      foreach(LINKER_TYPE EXE SHARED MODULE)
        set(CMAKE_${LINKER_TYPE}_LINKER_FLAGS_RELEASE
            "${CMAKE_${LINKER_TYPE}_LINKER_FLAGS_RELEASE} -flto-jobs=${CMAKE_LTO_JOBS}"
        )
        set(CMAKE_${LINKER_TYPE}_LINKER_FLAGS_RELWITHDEBINFO
            "${CMAKE_${LINKER_TYPE}_LINKER_FLAGS_RELWITHDEBINFO} -flto-jobs=${CMAKE_LTO_JOBS}"
        )
      endforeach()
    endif()
  elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_LTO)
    if(CMAKE_LTO_JOBS)
      set(CMAKE_CXX_FLAGS_RELEASE
          "${CMAKE_CXX_FLAGS_RELEASE} -flto=${CMAKE_LTO_JOBS}")
    else()
      set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -flto")
    endif()
  endif()

  set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} ${CXX_EXTRA_DEBUG}")
  set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELEASE} -g")
else()
  set(CMAKE_CXX_WINDOWS_FLAGS
      "/wd4244 /wd4267 /wd4200 /wd26451 /wd26495 /D_CRT_SECURE_NO_WARNINGS /utf-8"
  )
  if(TREAT_WARNINGS_AS_ERRORS)
    set(CMAKE_CXX_WINDOWS_FLAGS "${CMAKE_CXX_WINDOWS_FLAGS} /WX")
  endif()
  # remove warning from CXX flags
  string(REGEX REPLACE "/W[0-4]" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
  # add to-be-ignored warnings
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_WINDOWS_FLAGS}")
endif()

# todo use CHECK_CXX_COMPILER_FLAG(-fsanitize=address SUPPORTS_SANITIZER) etc.

set(CMAKE_C_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
set(CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")

set(DUCKDB_RELEASE_LINK_JOBS
    4
    CACHE STRING "Maximum number of concurrent linker jobs for Release builds")
set(DUCKDB_DEBUG_LINK_JOBS
    2
    CACHE STRING
          "Maximum number of concurrent linker jobs for non-Release builds")

# Throttle linker concurrency (mainly effective for Ninja) to avoid OOM in
# debug-like builds.
set_property(
  GLOBAL PROPERTY JOB_POOLS release_link_jobs=${DUCKDB_RELEASE_LINK_JOBS}
                  debug_link_jobs=${DUCKDB_DEBUG_LINK_JOBS})
if(NOT DEFINED CMAKE_JOB_POOL_LINK)
  if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(CMAKE_JOB_POOL_LINK release_link_jobs)
  else()
    set(CMAKE_JOB_POOL_LINK debug_link_jobs)
  endif()
endif()
message(
  STATUS
    "Build workers: compile=$ENV{CMAKE_BUILD_PARALLEL_LEVEL} link_release=${DUCKDB_RELEASE_LINK_JOBS} link_non_release=${DUCKDB_DEBUG_LINK_JOBS}"
)

include_directories(src/include)
if(NOT EXTENSION_CONFIG_BUILD)
  include_directories(third_party/fsst)
  include_directories(third_party/fmt/include)
  include_directories(third_party/hyperloglog)
  include_directories(third_party/fastpforlib)
  include_directories(third_party/skiplist)
  include_directories(third_party/ska_sort)
  include_directories(third_party/fast_float)
  include_directories(third_party/re2)
  include_directories(third_party/miniz)
  include_directories(third_party/utf8proc/include)
  include_directories(third_party/concurrentqueue)
  include_directories(third_party/pcg)
  include_directories(third_party/pdqsort)
  include_directories(third_party/tdigest)
  include_directories(third_party/mbedtls/include)
  include_directories(third_party/httplib)
  include_directories(third_party/jaro_winkler)
  include_directories(third_party/vergesort)
  include_directories(third_party/yyjson/include)
  include_directories(third_party/zstd/include)
endif()

# todo only regenerate ub file if one of the input files changed hack alert
function(enable_unity_build UB_SUFFIX SOURCE_VARIABLE_NAME)
  set(files ${${SOURCE_VARIABLE_NAME}})

  # Generate a unique filename for the unity build translation unit
  set(unit_build_file ${CMAKE_CURRENT_BINARY_DIR}/ub_${UB_SUFFIX}.cpp)
  set(temp_unit_build_file ${CMAKE_CURRENT_BINARY_DIR}/ub_${UB_SUFFIX}.cpp.tmp)
  # Exclude all translation units from compilation
  set_source_files_properties(${files} PROPERTIES HEADER_FILE_ONLY true)

  set(rebuild FALSE)
  # check if any of the source files have changed
  foreach(source_file ${files})
    if(${CMAKE_CURRENT_SOURCE_DIR}/${source_file} IS_NEWER_THAN
       ${unit_build_file})
      set(rebuild TRUE)
    endif()
  endforeach(source_file)
  # write a temporary file
  file(WRITE ${temp_unit_build_file} "// Unity Build generated by CMake\n")
  foreach(source_file ${files})
    # Include relative to the ub file's own directory (not an absolute source
    # path), so the generated unity source is byte-identical regardless of the
    # checkout location. This lets ccache/sccache share objects between
    # different working copies (e.g. git worktrees) instead of missing on the
    # embedded absolute path.
    if(WIN32)
      # MSVC applies the MAX_PATH limit to the unnormalized concatenation of the
      # including file's directory and a relative include, so a deep enough
      # checkout fails with C1083 on paths that resolve well below the limit.
      # Use absolute paths on Windows.
      set(rel_source_file ${CMAKE_CURRENT_SOURCE_DIR}/${source_file})
    else()
      file(RELATIVE_PATH rel_source_file ${CMAKE_CURRENT_BINARY_DIR}
           ${CMAKE_CURRENT_SOURCE_DIR}/${source_file})
    endif()
    file(APPEND ${temp_unit_build_file} "#include \"${rel_source_file}\"\n")
  endforeach(source_file)

  execute_process(
    COMMAND ${CMAKE_COMMAND} -E compare_files ${unit_build_file}
            ${temp_unit_build_file}
    RESULT_VARIABLE compare_result
    OUTPUT_VARIABLE bla
    ERROR_VARIABLE bla)
  if(compare_result EQUAL 0)
    # files are identical: do nothing
  elseif(compare_result EQUAL 1)
    # files are different: rebuild
    set(rebuild TRUE)
  else()
    # error while compiling: rebuild
    set(rebuild TRUE)
  endif()

  if(${rebuild})
    file(WRITE ${unit_build_file} "// Unity Build generated by CMake\n")
    foreach(source_file ${files})
      if(WIN32)
        set(rel_source_file ${CMAKE_CURRENT_SOURCE_DIR}/${source_file})
      else()
        file(RELATIVE_PATH rel_source_file ${CMAKE_CURRENT_BINARY_DIR}
             ${CMAKE_CURRENT_SOURCE_DIR}/${source_file})
      endif()
      file(APPEND ${unit_build_file} "#include \"${rel_source_file}\"\n")
    endforeach(source_file)
  endif()

  # Complement list of translation units with the name of ub
  set(${SOURCE_VARIABLE_NAME}
      ${${SOURCE_VARIABLE_NAME}} ${unit_build_file}
      PARENT_SCOPE)
endfunction(enable_unity_build)

function(add_library_unity NAME MODE)
  set(SRCS ${ARGN})
  if(NOT DISABLE_UNITY)
    enable_unity_build(${NAME} SRCS)
  endif()
  add_library(${NAME} OBJECT ${SRCS})
  if(MSVC)
    target_compile_options(${NAME} PRIVATE /bigobj)
  endif()
endfunction()

function(disable_target_warnings NAME)
  if(MSVC)
    target_compile_options(${NAME} PRIVATE "/W0")
  elseif("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang$"
         OR "${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
    target_compile_options(${NAME} PRIVATE "-w")
  endif()
endfunction()

# Capture the current dir so the below function also works when included in a
# different project
set(_DUCKDB_PROJECT_SRC_DIR ${PROJECT_SOURCE_DIR})
set(_DUCKDB_PROJECT_BIN_DIR ${PROJECT_BINARY_DIR})

function(add_third_party NAME)
  add_subdirectory(${_DUCKDB_PROJECT_SRC_DIR}/third_party/${NAME}
                   ${_DUCKDB_PROJECT_BIN_DIR}/third_party/${NAME})
endfunction()

# this depends on disable_target_warnings so the order matters
include(${PROJECT_SOURCE_DIR}/extension/extension_build_tools.cmake)

include(${PROJECT_SOURCE_DIR}/scripts/architecture_flags.cmake)

# These compile definitions are consumed by sources in src/, so they must be
# configured before add_subdirectory(src).
add_definitions(-DDUCKDB_MAJOR_VERSION=${DUCKDB_MAJOR_VERSION})
add_definitions(-DDUCKDB_MINOR_VERSION=${DUCKDB_MINOR_VERSION})
add_definitions(-DDUCKDB_PATCH_VERSION=${DUCKDB_PATCH_VERSION})

if(BUILD_MAIN_DUCKDB_LIBRARY
   AND NOT PREBUILT_BINARY
   AND ENABLE_JEMALLOC
   AND CMAKE_SIZEOF_VOID_P EQUAL 8
   AND NOT FORCE_32_BIT
   AND OS_NAME STREQUAL "linux"
   AND NOT EMSCRIPTEN
   AND NOT CLANG_TIDY
   AND NOT ANDROID
   AND NOT ZOS
   AND NOT BSD
   AND NOT ${WASM_ENABLED}
   AND NOT ${MUSL_ENABLED})
  set(JEMALLOC_ENABLED TRUE)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DDUCKDB_ENABLE_JEMALLOC")
  include_directories(third_party/jemalloc/include)
  add_third_party(jemalloc)
endif()

if(PREBUILT_BINARY)
  get_filename_component(PREBUILT_BINARY_PATH "${PREBUILT_BINARY}" ABSOLUTE)
  if(NOT EXISTS "${PREBUILT_BINARY_PATH}")
    message(
      FATAL_ERROR
        "Prebuilt DuckDB static library does not exist: ${PREBUILT_BINARY_PATH}"
    )
  endif()

  message(
    STATUS "Using pre-built static library from file ${PREBUILT_BINARY_PATH}")
  add_library(duckdb_static STATIC IMPORTED GLOBAL)
  set_property(TARGET duckdb_static PROPERTY IMPORTED_LOCATION
                                             "${PREBUILT_BINARY_PATH}")
  target_link_libraries(duckdb_static INTERFACE ${DUCKDB_SYSTEM_LIBS})
  link_threads(duckdb_static INTERFACE)

  if(BUILD_SHELL AND NOT SUN)
    get_filename_component(_DUCKDB_PREBUILT_LIBRARY_DIRECTORY
                           "${PREBUILT_BINARY_PATH}" DIRECTORY)
    if(MSVC)
      set(_DUCKDB_PREBUILT_SHELL_PATH
          "${_DUCKDB_PREBUILT_LIBRARY_DIRECTORY}/duckdb_shell.lib")
    else()
      set(_DUCKDB_PREBUILT_SHELL_PATH
          "${_DUCKDB_PREBUILT_LIBRARY_DIRECTORY}/libduckdb_shell.a")
    endif()
    if(NOT EXISTS "${_DUCKDB_PREBUILT_SHELL_PATH}")
      message(
        FATAL_ERROR
          "Prebuilt DuckDB shell library does not exist: ${_DUCKDB_PREBUILT_SHELL_PATH}"
      )
    endif()
    message(
      STATUS
        "Using pre-built shell library from file ${_DUCKDB_PREBUILT_SHELL_PATH}"
    )
  endif()

  # capabilities shipped next to the prebuilt library become their archive
  # targets
  get_filename_component(_DUCKDB_PREBUILT_DIRECTORY "${PREBUILT_BINARY_PATH}"
                         DIRECTORY)
  foreach(DUCKDB_PREBUILT_CAPABILITY duckdb_httplib duckdb_loadable_extensions)
    if(MSVC)
      set(_DUCKDB_PREBUILT_CAPABILITY_PATH
          "${_DUCKDB_PREBUILT_DIRECTORY}/${DUCKDB_PREBUILT_CAPABILITY}.lib")
    else()
      set(_DUCKDB_PREBUILT_CAPABILITY_PATH
          "${_DUCKDB_PREBUILT_DIRECTORY}/lib${DUCKDB_PREBUILT_CAPABILITY}.a")
    endif()
    if(EXISTS "${_DUCKDB_PREBUILT_CAPABILITY_PATH}"
       AND NOT TARGET ${DUCKDB_PREBUILT_CAPABILITY})
      add_library(${DUCKDB_PREBUILT_CAPABILITY} STATIC IMPORTED GLOBAL)
      set_property(
        TARGET ${DUCKDB_PREBUILT_CAPABILITY}
        PROPERTY IMPORTED_LOCATION "${_DUCKDB_PREBUILT_CAPABILITY_PATH}")
      target_link_libraries(${DUCKDB_PREBUILT_CAPABILITY}
                            INTERFACE duckdb_static)
      message(
        STATUS
          "Using pre-built capability from file ${_DUCKDB_PREBUILT_CAPABILITY_PATH}"
      )
    endif()
  endforeach()
  # local_extension_repository names this build's repository, so it is built
  # here rather than shipped
  add_subdirectory(src/main/extension/local_extension_repository
                   src/main/extension/local_extension_repository)

  foreach(
    DUCKDB_PREBUILT_LINK_LIBRARY
    duckdb_fastpforlib
    duckdb_fmt
    duckdb_fsst
    duckdb_hyperloglog
    duckdb_mbedtls
    duckdb_miniz
    duckdb_re2
    duckdb_skiplistlib
    duckdb_utf8proc
    duckdb_yyjson
    duckdb_zstd)
    if(NOT TARGET ${DUCKDB_PREBUILT_LINK_LIBRARY})
      add_library(${DUCKDB_PREBUILT_LINK_LIBRARY} INTERFACE IMPORTED GLOBAL)
      target_link_libraries(${DUCKDB_PREBUILT_LINK_LIBRARY}
                            INTERFACE duckdb_static)
    endif()
  endforeach()

  add_subdirectory(tools)
  add_subdirectory(extension)
elseif(BUILD_MAIN_DUCKDB_LIBRARY)
  add_subdirectory(src)

  add_subdirectory(tools)
  add_subdirectory(extension)
endif()
duckdb_link_deferred_extension_libraries()

if(NOT EXTENSION_CONFIG_BUILD)
  if(PREBUILT_BINARY)
    message(STATUS "Skipping third_party/ because PREBUILT_BINARY is set")
  else()
    add_third_party(fastpforlib)
    add_third_party(fmt)
    add_third_party(fsst)
    add_third_party(hyperloglog)
    add_third_party(mbedtls)
    add_third_party(miniz)
    add_third_party(re2)
    add_third_party(skiplist)
    add_third_party(utf8proc)
    add_third_party(yyjson)
    add_third_party(zstd)
  endif()
  if(NOT DUCKDB_EXPLICIT_PLATFORM)
    set(VERSION_SOURCES tools/utils/test_platform.cpp)

    add_executable(duckdb_platform_binary ${VERSION_SOURCES})
    link_threads(duckdb_platform_binary "")

    set_target_properties(duckdb_platform_binary
                          PROPERTIES OUTPUT_NAME duckdb_platform_binary)
    set_target_properties(
      duckdb_platform_binary PROPERTIES RUNTIME_OUTPUT_DIRECTORY
                                        ${PROJECT_BINARY_DIR})
    add_custom_command(
      OUTPUT ${PROJECT_BINARY_DIR}/duckdb_platform_out
      DEPENDS duckdb_platform_binary
      COMMAND
        $<TARGET_FILE:duckdb_platform_binary> >
        ${PROJECT_BINARY_DIR}/duckdb_platform_out ||
        (
        echo
        "Provide explicit DUCKDB_PLATFORM=your_target_arch to avoid build-type detection of the platform"
        &&
        exit
        1)
      WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
  else()
    add_custom_command(
      OUTPUT ${PROJECT_BINARY_DIR}/duckdb_platform_out
      COMMAND ${CMAKE_COMMAND} -E echo_append \"${DUCKDB_EXPLICIT_PLATFORM}\" >
              ${PROJECT_BINARY_DIR}/duckdb_platform_out
      WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
  endif()
  add_custom_target(duckdb_platform
                    DEPENDS ${PROJECT_BINARY_DIR}/duckdb_platform_out)
endif()

if(NOT CLANG_TIDY)
  if(${BUILD_UNITTESTS})
    add_subdirectory(test)
    if(NOT WIN32 AND ${BUILD_BENCHMARKS})
      add_subdirectory(benchmark)
    endif()
  endif()
endif()

duckdb_apply_architecture_flags()

if(NOT ${EXTENSION_CONFIG_BUILD})
  # Write the export set for build and install tree
  install(EXPORT "${DUCKDB_EXPORT_SET}" DESTINATION "${INSTALL_CMAKE_DIR}")
  export(EXPORT "${DUCKDB_EXPORT_SET}"
         FILE "${PROJECT_BINARY_DIR}/${DUCKDB_EXPORT_SET}.cmake")
endif()

# Only write the cmake package configuration if the templates exist
set(CMAKE_CONFIG_TEMPLATE "${CMAKE_CURRENT_SOURCE_DIR}/DuckDBConfig.cmake.in")
set(CMAKE_CONFIG_VERSION_TEMPLATE
    "${CMAKE_CURRENT_SOURCE_DIR}/DuckDBConfigVersion.cmake.in")
if(EXISTS ${CMAKE_CONFIG_TEMPLATE} AND EXISTS ${CMAKE_CONFIG_VERSION_TEMPLATE})

  # Configure cmake package config for the build tree
  set(CONF_INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/src/include")
  configure_file(${CMAKE_CONFIG_TEMPLATE}
                 "${PROJECT_BINARY_DIR}/DuckDBConfig.cmake" @ONLY)

  # Configure cmake package config for the install tree
  file(RELATIVE_PATH REL_INCLUDE_DIR
       "${CMAKE_INSTALL_PREFIX}/${INSTALL_CMAKE_DIR}"
       "${CMAKE_INSTALL_PREFIX}/${INSTALL_INCLUDE_DIR}")
  set(CONF_INCLUDE_DIRS "\${DuckDB_CMAKE_DIR}/${REL_INCLUDE_DIR}")
  configure_file(
    ${CMAKE_CONFIG_TEMPLATE}
    "${PROJECT_BINARY_DIR}${CMAKE_FILES_DIRECTORY}/DuckDBConfig.cmake" @ONLY)

  # Configure cmake package version for build and install tree
  configure_file(${CMAKE_CONFIG_VERSION_TEMPLATE}
                 "${PROJECT_BINARY_DIR}/DuckDBConfigVersion.cmake" @ONLY)

  # Install the cmake package
  install(
    FILES "${PROJECT_BINARY_DIR}${CMAKE_FILES_DIRECTORY}/DuckDBConfig.cmake"
          "${PROJECT_BINARY_DIR}/DuckDBConfigVersion.cmake"
    DESTINATION "${INSTALL_CMAKE_DIR}")
endif()
