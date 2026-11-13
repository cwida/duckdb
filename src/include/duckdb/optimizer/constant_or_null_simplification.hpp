//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/constant_or_null_simplification.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/logical_operator.hpp"

namespace duckdb {
class ClientContext;
class NotNullExpressionAnalyzer;

class ConstantOrNullSimplification {
public:
	explicit ConstantOrNullSimplification(ClientContext &context);

	unique_ptr<LogicalOperator> Optimize(unique_ptr<LogicalOperator> op);

private:
	void OptimizeInternal(unique_ptr<LogicalOperator> &op, bool plan_has_side_effects,
	                      NotNullExpressionAnalyzer &analyzer);
	unique_ptr<Expression> SimplifyExpression(LogicalOperator &input, unique_ptr<Expression> expr,
	                                          NotNullExpressionAnalyzer &analyzer, bool allow_folding);
	void OptimizeFilter(unique_ptr<LogicalOperator> &op, bool plan_has_side_effects,
	                    NotNullExpressionAnalyzer &analyzer);

private:
	ClientContext &context;
};

} // namespace duckdb
