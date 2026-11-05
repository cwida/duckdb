#include "duckdb/optimizer/scalar_fn_pushdown.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"

namespace duckdb {
void ScalarFnCollect::VisitOperator(LogicalOperator &op) {
	/*
	 * Logical projection expressions are columns which reference underlying
	 * GETs. Don't process them, as they would add conflicts for every column
	 * used in projection. Example: PROJECTION(col) -> GET(col). We don't want
	 * to visit BoundColumnRefExpression in PROJECTION to avoid registering a
	 * non-existent conflict.
	 *
	 * However, ScalarFnReplace will visit them because we need to update their
	 * types if pushdown succeeded.
	 */
	if (op.type == LogicalOperatorType::LOGICAL_PROJECTION &&
	    projections.count(op.Cast<LogicalProjection>().table_index)) {
		VisitOperatorChildren(op);
		return;
	}
	LogicalOperatorVisitor::VisitOperator(op);
}

unique_ptr<Expression> ScalarFnCollect::VisitReplace(BoundColumnRefExpression &expr, unique_ptr<Expression> *ptr) {
	if (const auto binding = Resolve(expr.Binding(), analyses, projections)) {
		// Column is used without function applied to it, register a conflict.
		// Not emplace() as we need to update the value if it was present
		binding->analysis.col_to_expr[binding->column_index] = nullptr;
	}
	return std::move(*ptr);
}

unique_ptr<Expression> ScalarFnCollect::VisitReplace(BoundFunctionExpression &expr, unique_ptr<Expression> *ptr) {
	if (BoundCastExpression::IsCast(expr)) {
		// Casts are handled by the dedicated cast pushdown (CastCollect/CastReplace). Descend into the cast so its
		// child column is registered as a conflict for scalar-function pushdown, matching the pre-function behavior.
		return nullptr;
	}
	if (expr.GetChildren().size() != 1 ||
	    expr.GetChildren()[0]->GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
		// Descend into children so e.g. fn(col, other) still sees "col" and
		// registers a conflict
		return nullptr;
	}
	const auto &bound_col = expr.GetChildren()[0]->Cast<BoundColumnRefExpression>();
	const auto binding = Resolve(bound_col.Binding(), analyses, projections);
	if (!binding) {
		return nullptr;
	}
	auto &col_to_expr = binding->analysis.col_to_expr;

	if (auto it = col_to_expr.find(binding->column_index); it == col_to_expr.end()) {
		// This is the first time we see the column used by a single function.
		col_to_expr.emplace(binding->column_index, &expr);
	} else if (it->second == nullptr || !it->second->Equals(expr)) {
		// Either column is used with different function in "expr" or
		// there already is a conflict.
		it->second = nullptr;
	}

	return std::move(*ptr);
}

unique_ptr<Expression> ScalarFnReplace::VisitReplace(BoundColumnRefExpression &expr, unique_ptr<Expression> *ptr) {
	const auto binding = Resolve(expr.Binding(), analyses, projections);
	if (!binding) {
		return std::move(*ptr);
	}

	const auto &[analysis, column_index, projection, projection_column_index] = *binding;
	if (CanPushdownColumn(analysis, column_index)) {
		const LogicalType return_type = analysis.get.returned_types[analysis.StorageIndex(column_index)];
		expr.SetReturnType(return_type);
		if (projection != nullptr && !projection->types.empty()) {
			projection->types[projection_column_index] = return_type;
		}
	}

	return std::move(*ptr);
}

unique_ptr<Expression> ScalarFnReplace::VisitReplace(BoundFunctionExpression &expr, unique_ptr<Expression> *ptr) {
	if (BoundCastExpression::IsCast(expr)) {
		// Casts are handled by the dedicated cast pushdown (CastCollect/CastReplace).
		return nullptr;
	}
	if (expr.GetChildren().size() != 1 ||
	    expr.GetChildren()[0]->GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
		return nullptr; // Same as in ScalarFnCollect::VisitReplace
	}
	unique_ptr<Expression> &bound_col_base = expr.GetChildrenMutable()[0];
	const auto &bound_col = bound_col_base->Cast<BoundColumnRefExpression>();
	const auto binding = Resolve(bound_col.Binding(), analyses, projections);
	if (!binding) {
		return nullptr;
	}

	const auto &[analysis, column_index, projection, projection_column_index] = *binding;
	if (!CanPushdownColumn(analysis, column_index)) {
		return std::move(*ptr);
	}

	const LogicalType return_type = analysis.get.returned_types[analysis.StorageIndex(column_index)];
	bound_col_base->SetReturnType(return_type);
	if (projection != nullptr && !projection->types.empty()) {
		projection->types[projection_column_index] = return_type;
	}
	return std::move(bound_col_base);
}

ScalarFnCollect::ScalarFnCollect(Analyses &analyses, const Projections &projections)
    : analyses(analyses), projections(projections) {
}

ScalarFnReplace::ScalarFnReplace(Analyses &analyses, const Projections &projections)
    : analyses(analyses), projections(projections) {
}
} // namespace duckdb
