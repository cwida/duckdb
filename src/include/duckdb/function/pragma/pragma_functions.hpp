//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/pragma/pragma_functions.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/function/pragma_function.hpp"
#include "duckdb/function/built_in_functions.hpp"
#include "duckdb/common/optional_idx.hpp"

namespace duckdb {

struct PragmaQueries {
	static void RegisterFunction(BuiltinFunctions &set);
};

struct PragmaFunctions {
	static void RegisterFunction(BuiltinFunctions &set);
};

string PragmaShowTables(ClientContext &context, const string &catalog = "", optional_idx schema_oid = optional_idx());
string PragmaShowTablesExpanded();
string PragmaShowDatabases();
string PragmaShowVariables();
string PragmaShow(const string &table_name);

} // namespace duckdb
