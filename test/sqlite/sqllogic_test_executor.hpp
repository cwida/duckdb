//===----------------------------------------------------------------------===//
//                         DuckDB
//
// sqllogic_test_executor.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/string.hpp"
#include "duckdb/common/vector.hpp"

namespace duckdb {

struct SQLLogicTestStats {
	idx_t skip_mode = 0;
	vector<string> partial_skip_reasons;
};

SQLLogicTestStats RunSQLLogicTestFile(const string &name, bool auto_switch_test_dir, bool output_sql);

} // namespace duckdb
