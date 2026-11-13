//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/topn_optimizer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/constants.hpp"
#include "duckdb/common/optional_ptr.hpp"

namespace duckdb {
class ClientContext;
class LogicalOperator;
class LogicalTopN;
class Optimizer;

class TopN {
public:
	explicit TopN(ClientContext &context);

	//! Optimize ORDER BY + LIMIT to TopN
	unique_ptr<LogicalOperator> Optimize(unique_ptr<LogicalOperator> op);
	//! Whether we can perform the optimization on this operator
	static bool CanOptimize(LogicalOperator &op, optional_ptr<ClientContext> context = nullptr);
	//! Pushes a dynamic filter "binding <comparison_type> [boundary]" into the scans below the operator that produce
	//! the column. Returns the filter, or nullptr if no scan can evaluate it.
	static shared_ptr<DynamicFilterData> PushDynamicFilterIntoScans(ClientContext &context, LogicalOperator &child,
	                                                                ColumnBinding binding, const LogicalType &type,
	                                                                bool nulls_first, ExpressionType comparison_type);

private:
	void PushdownDynamicFilters(LogicalTopN &op);

private:
	ClientContext &context;
};

} // namespace duckdb
