//===----------------------------------------------------------------------===//
//                         DuckDB
//
// sqllogic_test_executor.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/string.hpp"

namespace duckdb {

void RunSQLLogicTestFile(const string &name, bool auto_switch_test_dir, bool output_sql);

} // namespace duckdb
