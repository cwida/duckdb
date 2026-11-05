//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/enums/connection_type.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/constants.hpp"

namespace duckdb {

enum class ConnectionType : uint8_t {
	//! A connection opened by a user
	USER,
	//! A connection DuckDB opened for its own use (e.g. a checkpoint)
	INTERNAL,
};

} // namespace duckdb
