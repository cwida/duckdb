//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/filter_statistics.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"

namespace duckdb {

class LogicalOperator;
class Optimizer;

class FilterStatisticsOptimizer {
public:
	explicit FilterStatisticsOptimizer(Optimizer &optimizer);

	void Optimize(unique_ptr<LogicalOperator> &plan);

private:
	bool HasMultiBindingFilter(const LogicalOperator &op) const;
	bool ContainsDelimJoin(const LogicalOperator &op) const;
	//! Whether a filter or join condition of the plan reads a scanned column that holds only NULLs
	bool ComparesAllNullColumn(LogicalOperator &plan) const;

private:
	Optimizer &optimizer;
};

} // namespace duckdb
