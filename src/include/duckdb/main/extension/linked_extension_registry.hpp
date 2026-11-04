//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/extension/linked_extension_registry.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/string.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/winapi.hpp"
#include "duckdb_static_extension.h"

#include <functional>

namespace duckdb {
class DatabaseInstance;
class DuckDB;

//! An extension compiled into this binary, as a name and the callable that loads it.
struct LinkedExtension {
	string name;
	//! empty when what was registered is not an extension
	std::function<void(DuckDB &)> load;
	//! set instead of load when what was registered is not an extension, called for every database opened
	std::function<void(DatabaseInstance &)> database_callback;
};

//! What a describe function described, once validated.
struct StaticExtensionDescription {
	string name;
	string version;
	//! the DuckDB version a C++ entry point was built against, or the C API version a C entry point targets
	string api_version;
	duckdb_extension_descriptor descriptor;
};

//! The extensions linked into this binary, registered through duckdb_register_static_extension.
class LinkedExtensionRegistry {
public:
	//! Calls describe and validates the descriptor it fills. Returns the reason on failure, otherwise an empty string.
	DUCKDB_API static string Describe(duckdb_extension_describe_t describe, StaticExtensionDescription &result);
	//! A copy, in registration order. Throws if a registration failed.
	DUCKDB_API static vector<LinkedExtension> Get();
};

} // namespace duckdb
