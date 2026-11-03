//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/map_entry_encoding.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

// Deliberately doesn't include DuckDB headers, so the encodings extension can use compilation caches efficiently.
#include <cstddef>

namespace duckdb {

//! Encoding Map Entry Struct used for byte replacement
typedef struct {
	size_t key_len;
	const char *key;
	size_t value_len;
	const char *value;
} map_entry_encoding;

} // namespace duckdb
