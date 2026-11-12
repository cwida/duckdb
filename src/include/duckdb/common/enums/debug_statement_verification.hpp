//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/enums/debug_statement_verification.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/constants.hpp"

namespace duckdb {

enum class DebugStatementVerification : uint8_t {
	NONE,
	COPY_STATEMENT,
	REPARSE_STATEMENT,
	SERIALIZE_STATEMENT,
	PREPARED_STATEMENT,
	EXPLAIN_STATEMENT,
	EXPLAIN_SQL,
	EXPLAIN_SQL_STRICT
};

} // namespace duckdb
