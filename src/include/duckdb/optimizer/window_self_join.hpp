//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/count_window_elimination.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/optimizer/optimizer.hpp"

#include "duckdb/optimizer/column_binding_replacer.hpp"
#include "duckdb/planner/bound_parameter_map.hpp"

namespace duckdb {

class WindowSelfJoinOptimizer {
public:
	static bool CanOptimize(const LogicalOperator &op);

	explicit WindowSelfJoinOptimizer(Optimizer &optimizer);

	unique_ptr<LogicalOperator> Optimize(unique_ptr<LogicalOperator> op);

private:
	bool CanOptimize(const BoundWindowExpression &w_expr, const BoundWindowExpression &w_expr0) const;
	unique_ptr<LogicalOperator> OptimizeInternal(unique_ptr<LogicalOperator> op, ColumnBindingReplacer &replacer);

	Optimizer &optimizer;
	//! The parameters of the enclosing PREPARE, which copied parameter expressions must keep pointing to
	optional_ptr<bound_parameter_map_t> parameter_data;
};

} // namespace duckdb
