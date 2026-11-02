#include "duckdb/planner/expression_binder/check_binder.hpp"

#include "duckdb/planner/table_binding.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"

namespace duckdb {

CheckBinder::CheckBinder(Binder &binder, ClientContext &context, Identifier table_p, const ColumnList &columns,
                         physical_index_set_t &bound_columns)
    : ExpressionBinder(binder, context), table(std::move(table_p)), columns(columns), bound_columns(bound_columns) {
	target_type = LogicalType::INTEGER;
}

BindResult CheckBinder::BindExpression(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth, bool root_expression) {
	auto &expr = *expr_ptr;
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::WINDOW:
		return BindResult("window functions are not allowed in check constraints");
	case ExpressionClass::SUBQUERY:
		return BindResult("cannot use subquery in check constraint");
	case ExpressionClass::COLUMN_REF:
		return BindCheckColumn(expr.Cast<ColumnRefExpression>());
	default:
		return ExpressionBinder::BindExpression(expr_ptr, depth);
	}
}

string CheckBinder::UnsupportedAggregateMessage() {
	return "aggregate functions are not allowed in check constraints";
}

BindResult ExpressionBinder::BindQualifiedColumnName(ColumnRefExpression &colref, const Identifier &table_name) {
	idx_t struct_start = 0;
	if (colref.ColumnNames()[0] == table_name) {
		struct_start++;
	}
	// the first name (after the table name) is the column, the remaining names are struct fields
	auto result = make_uniq_base<ParsedExpression, ColumnRefExpression>(colref.ColumnNames()[struct_start]);
	for (idx_t i = struct_start + 1; i < colref.ColumnNames().size(); i++) {
		result = CreateStructExtract(std::move(result), colref.ColumnNames()[i]);
	}
	return BindExpression(result, 0);
}

BindResult CheckBinder::BindCheckColumn(ColumnRefExpression &colref) {
	if (lambda_bindings && !lambda_bindings->empty()) {
		// FIXME: support lambda parameters and captured columns in CHECK constraints
		throw NotImplementedException("Lambda functions are currently not supported in CHECK constraints.");
	}

	if (colref.ColumnNames().size() > 1) {
		return BindQualifiedColumnName(colref, table);
	}
	if (!columns.ColumnExists(colref.ColumnNames()[0])) {
		throw BinderException("Table does not contain column %s referenced in check constraint!",
		                      colref.ColumnNames()[0]);
	}
	auto &col = columns.GetColumn(colref.ColumnNames()[0]);
	if (col.Generated()) {
		auto bound_expression = col.GeneratedExpression().Copy();
		return BindExpression(bound_expression, 0, false);
	}
	bound_columns.insert(col.Physical());
	D_ASSERT(col.StorageOid() != DConstants::INVALID_INDEX);
	return BindResult(make_uniq<BoundReferenceExpression>(col.Type(), col.StorageOid()));
}

} // namespace duckdb
