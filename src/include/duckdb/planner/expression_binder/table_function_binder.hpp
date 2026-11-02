//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/expression_binder/table_function_binder.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/expression_binder.hpp"

namespace duckdb {

//! The table function binder can bind standard table function parameters (i.e., non-table-in-out functions)
class TableFunctionBinder : public ExpressionBinder {
public:
	//! How an identifier that is not a lambda parameter or lateral column is bound. FOLLOW_SETTING: resolve SQL value
	//! functions (e.g. user), then convert to a string as table_function_identifier_conversion allows. ALLOW: the
	//! identifier is a name (e.g. a COPY column list): bind it as the string it spells, without warning and without
	//! resolving SQL value functions.
	enum class IdentifierConversionPolicy : uint8_t { FOLLOW_SETTING, ALLOW };

	TableFunctionBinder(
	    Binder &binder, ClientContext &context, string table_function_name = string(), string clause = "Table function",
	    IdentifierConversionPolicy identifier_conversion_policy = IdentifierConversionPolicy::FOLLOW_SETTING);

protected:
	BindResult BindLambdaReference(LambdaRefExpression &expr, idx_t depth);
	BindResult BindColumnReference(unique_ptr<ParsedExpression> &expr, idx_t depth, bool root_expression);
	BindResult BindExpression(unique_ptr<ParsedExpression> &expr, idx_t depth, bool root_expression = false) override;

	string UnsupportedAggregateMessage() override;

private:
	string table_function_name;
	string clause;
	IdentifierConversionPolicy identifier_conversion_policy;
};

} // namespace duckdb
