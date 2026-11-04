# Copyright 2015 The RE2 Authors.  All Rights Reserved. Use of this source code
# is governed by a BSD-style license that can be found in the LICENSE file.

cmake_minimum_required(VERSION 3.5...3.29)

project(RE2 CXX)

set(CMAKE_CXX_VISIBILITY_PRESET hidden)

include(CTest)

# CMake seems to have no way to enable/disable testing per subproject, so we
# provide an option similar to BUILD_TESTING, but just for RE2.
option(RE2_BUILD_TESTING "enable testing for RE2" OFF)

set(EXTRA_TARGET_LINK_LIBRARIES)

if(CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
  if(MSVC_VERSION LESS 1900)
    message(FATAL_ERROR "you need Visual Studio 2015 or later")
  endif()
  if(BUILD_SHARED_LIBS)
    # See http://www.kitware.com/blog/home/post/939 for details.
    cmake_minimum_required(VERSION 3.4)
    set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)
  endif()
  # CMake defaults to /W3, but some users like /W4 (or /Wall) and /WX, so we
  # disable various warnings that aren't particularly helpful.
  add_compile_options(/wd4100
                      /wd4201
                      /wd4456
                      /wd4457
                      /wd4702
                      /wd4815)
  # Without a byte order mark (BOM), Visual Studio assumes that the source file
  # is encoded using the current user code page, so we specify UTF-8.
  add_compile_options(/utf-8)
elseif(CYGWIN OR MINGW)
  # See https://stackoverflow.com/questions/38139631 for details.
  add_compile_options(-std=gnu++17)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-std=c++17)
endif()

add_definitions(-DRE2_ON_VALGRIND)

if(WIN32)
  add_definitions(-DUNICODE
                  -D_UNICODE
                  -DSTRICT
                  -DNOMINMAX)
  add_definitions(-D_CRT_SECURE_NO_WARNINGS -D_SCL_SECURE_NO_WARNINGS)
elseif(UNIX)
  # add_compile_options(-pthread) list(APPEND EXTRA_TARGET_LINK_LIBRARIES
  # -pthread)
endif()

set(RE2_SOURCES

        re2/bitmap256.cc
        re2/compile.cc
    re2/bitstate.cc
    re2/dfa.cc
    re2/filtered_re2.cc
    re2/mimics_pcre.cc
    re2/nfa.cc
    re2/onepass.cc
    re2/parse.cc
    re2/perl_groups.cc
    re2/prefilter.cc
    re2/prefilter_tree.cc
    re2/prog.cc
    re2/re2.cc
    re2/regexp.cc
    re2/set.cc
    re2/simplify.cc
    re2/stringpiece.cc
    re2/tostring.cc
    re2/unicode_casefold.cc
    re2/unicode_groups.cc
    util/rune.cc
    util/strutil.cc
)

add_library(duckdb_re2 STATIC ${RE2_SOURCES})

target_include_directories(
  duckdb_re2
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)

install(TARGETS duckdb_re2
        EXPORT "${DUCKDB_EXPORT_SET}"
        LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
        ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")

disable_target_warnings(duckdb_re2)
