//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/expression_nullability.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/table_index.hpp"
#include "duckdb/common/vector.hpp"

#include <functional>

namespace duckdb {

class ClientContext;
class BoundColumnRefExpression;
class Expression;
class LogicalCTE;
class LogicalOperator;

//! Returns whether an expression becomes NULL when the provided column reference becomes NULL.
bool ExpressionBecomesNull(const Expression &expr,
                           const std::function<bool(const BoundColumnRefExpression &)> &column_becomes_null);

//! Returns whether the filter cannot be TRUE when expr is NULL.
bool FilterRejectsNull(const Expression &filter, const Expression &expr);

//! Conservatively proves that an expression cannot be NULL at a logical operator's output.
class NotNullExpressionAnalyzer {
public:
	explicit NotNullExpressionAnalyzer(ClientContext &context, optional_ptr<LogicalOperator> plan_root = nullptr);

	bool IsNotNull(LogicalOperator &op, const Expression &expr);

private:
	bool IsNotNull(LogicalOperator &op, const Expression &expr, vector<TableIndex> &seen_ctes);
	optional_ptr<LogicalCTE> FindCTE(TableIndex cte_index);

private:
	ClientContext &context;
	optional_ptr<LogicalOperator> plan_root;
};

} // namespace duckdb
