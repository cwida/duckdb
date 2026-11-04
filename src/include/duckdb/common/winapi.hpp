//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/winapi.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#ifndef DUCKDB_API
#if defined(_WIN32) && !defined(__MINGW32__)
#ifdef DUCKDB_STATIC_BUILD
#define DUCKDB_API
#else
#ifdef DUCKDB_BUILD_LIBRARY
#define DUCKDB_API __declspec(dllexport)
#else
#define DUCKDB_API __declspec(dllimport)
#endif
#endif
#else
#define DUCKDB_API
#endif
#endif

#ifndef DUCKDB_EXTENSION_API
#ifdef _WIN32
#ifdef DUCKDB_STATIC_BUILD
#define DUCKDB_EXTENSION_API
#else
#define DUCKDB_EXTENSION_API __declspec(dllexport)
#endif
#else
#define DUCKDB_EXTENSION_API __attribute__((visibility("default")))
#endif
#endif
