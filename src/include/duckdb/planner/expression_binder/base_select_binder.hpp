//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/expression_binder/base_select_binder.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/expression_map.hpp"
#include "duckdb/planner/expression_binder.hpp"

namespace duckdb {
class BoundColumnRefExpression;
class WindowExpression;

class BoundSelectNode;

//! The BaseSelectBinder is the base binder of the SELECT, HAVING and QUALIFY binders. It can bind aggregates and window
//! functions.
class BaseSelectBinder : public ExpressionBinder {
	friend class ColumnQualifier;

public:
	BaseSelectBinder(Binder &binder, ClientContext &context, BoundSelectNode &node);

	bool BoundAggregates() {
		return bound_aggregate;
	}
	void ResetBindings() {
		this->bound_aggregate = false;
		this->bound_columns.clear();
	}

	static bool IsFunctionallyDependent(const unique_ptr<Expression> &expr, const vector<reference<Expression>> &deps);

protected:
	BindResult BindExpression(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth,
	                          bool root_expression = false) override;

	BindResult BindAggregate(FunctionExpression &expr, AggregateFunctionCatalogEntry &function, idx_t depth) override;

	BindResult BindGroupingFunction(OperatorExpression &op, idx_t depth) override;

	//! Binds a WINDOW expression and returns the result.
	virtual BindResult BindWindowExpression(WindowExpression &expr, idx_t depth);
	virtual BindResult BindColumnRef(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth, bool root_expression);

	ProjectionIndex TryBindGroup(ParsedExpression &expr);
	ProjectionIndex FindGroup(ParsedExpression &expr);
	bool MatchesGroup(ParsedExpression &expr) override;
	bool MatchesGroupWithoutSubquery(ParsedExpression &expr) override;
	bool HasCompoundGroups() override;
	bool ClaimsAlias(ColumnRefExpression &colref) override;
	BindResult BindGroup(ParsedExpression &expr, idx_t depth, ProjectionIndex group_index);

protected:
	bool inside_window = false;
	bool inside_aggregate = false;
	bool bound_aggregate = false;
	bool inside_aggregate_filter = false;

	BoundSelectNode &node;
};

} // namespace duckdb
