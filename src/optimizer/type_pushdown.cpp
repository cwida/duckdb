#include "duckdb/optimizer/type_pushdown.hpp"
#include "duckdb/common/assert.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/projection_index.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/planner/logical_operator_visitor.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_limit.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"

/*
 * This pass implements type pushdown for file readers. If a LOGICAL_PROJECTION
 * has a LOGICAL_GET as a child, and for some column that's cast in
 * LOGICAL_PROJECTION there is no other usage (including uncasted) in the query,
 * and file reader supports projection expression pushdown, we can push down the
 * type cast into LOGICAL_GET.
 *
 * Example: SELECT ts::TIMESTAMP FROM file_reader();
 * We can push TIMESTAMP as ts's output type to file_reader();
 */

namespace duckdb {

// A passthrough projection only forwards its child columns, e.g. a VIEW's
// "SELECT col".
static bool IsPassthrough(const LogicalProjection &projection) {
	if (projection.expressions.empty()) {
		return false; // don't register empty projections in Projections
	}
	for (const auto &e : projection.expressions) {
		if (e->GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
			return false;
		}
	}
	return true;
}

void FindGetsAndProjections(LogicalOperator &op, Analyses &analyses, Projections &projections) {
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_GET: {
		if (auto &get = op.Cast<LogicalGet>(); get.function.projection_expression_pushdown != nullptr) {
			analyses.emplace(get.table_index, GetAnalysis {get, {}});
		}
		break;
	}
	case LogicalOperatorType::LOGICAL_PROJECTION: {
		LogicalProjection &projection = op.Cast<LogicalProjection>();
		D_ASSERT(projection.children.size() == 1);
		auto &child = *projection.children[0];
		if (!IsPassthrough(projection)) {
			break;
		}

		LogicalGet *get = nullptr;

		// queries with LIMIT may include a STREAMING_LIMIT between PROJECTION and GET.
		// See test/optimizer/pushdown/scalar_function_pushdown_limit.test
		if (child.type == LogicalOperatorType::LOGICAL_LIMIT) {
			if (auto &limit = child.Cast<LogicalLimit>();
			    limit.children.size() == 1 && limit.children[0]->type == LogicalOperatorType::LOGICAL_GET) {
				get = &limit.children[0]->Cast<LogicalGet>();
			}
		} else if (child.type == LogicalOperatorType::LOGICAL_GET) {
			get = &child.Cast<LogicalGet>();
		}

		if (get != nullptr && get->function.projection_expression_pushdown != nullptr) {
			projections.emplace(projection.table_index, projection);
		}
		break;
	}
	default:
		break;
	}

	for (auto &child : op.children) {
		FindGetsAndProjections(*child, analyses, projections);
	}
}

// A column binding into a GET is an index into its column ids, not a column id
// itself. Virtual columns (e.g. "filename") have no entry in returned_types, so
// nothing can be pushed into them.
static bool IsRealGetColumn(const LogicalGet &get, ProjectionIndex column_index) {
	const auto &column_ids = get.GetColumnIds();
	return column_index < column_ids.size() && !column_ids[column_index].IsVirtualColumn();
}

optional<GetBinding> Resolve(ColumnBinding binding, Analyses &analyses, const Projections &projections) {
	if (const auto it = analyses.find(binding.table_index); it != analyses.end()) {
		if (!IsRealGetColumn(it->second.get, binding.column_index)) {
			return nullopt;
		}
		return {{it->second, binding.column_index, nullptr, binding.column_index}};
	}

	const auto projection_it = projections.find(binding.table_index);
	if (projection_it == projections.end()) {
		return nullopt;
	}

	LogicalProjection &projection = projection_it->second;
	const auto &inner = projection.expressions[binding.column_index];
	if (inner->GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
		return nullopt;
	}
	const ColumnBinding get_binding = inner->Cast<BoundColumnRefExpression>().Binding();
	if (const auto it = analyses.find(get_binding.table_index); it != analyses.end()) {
		if (!IsRealGetColumn(it->second.get, get_binding.column_index)) {
			return nullopt;
		}
		return {{it->second, get_binding.column_index, &projection, binding.column_index}};
	}
	return nullopt;
}

/**
 * A GET reachable through a single-child chain of projections. A join or any
 * other multi-child operator breaks the chain.
 *
 * LOGICAL_FILTER also breaks the chain. In SELECT CAST/TRY_CAST(x) WHERE g(x)
 * we can't push down cast if g(x) isn't pushed. If g(x) filters some rows
 * on which cast would fail, the query passed by default but fails if cast is pushed
 * and runs before g(x).
 *
 * See test/sql/copy/csv/test_insert_into_types.test in duckdb (cast not pushed past a join)
 */
static bool ReachesPushdownGet(const LogicalOperator &op) {
	const LogicalOperator *cur = &op;
	while (cur->children.size() == 1) {
		cur = cur->children[0].get();
		switch (cur->type) {
		case LogicalOperatorType::LOGICAL_GET:
			return cur->Cast<LogicalGet>().function.projection_expression_pushdown != nullptr;
		case LogicalOperatorType::LOGICAL_PROJECTION:
			continue;
		default:
			return false;
		}
	}
	return false;
}

void CastCollect::VisitOperator(LogicalOperator &op) {
	/*
	 * Logical projection expressions are columns which reference underlying
	 * GETs. Don't process them, as they would add conflicts for every column
	 * used in projection. Example: PROJECTION(col) -> GET(col). We don't want
	 * to visit BoundColumnRefExpression in PROJECTION to avoid registering a
	 * non-existent conflict.
	 *
	 * However, CastReplace will visit them because we need to update their
	 * types if pushdown succeeded.
	 */
	if (op.type != LogicalOperatorType::LOGICAL_PROJECTION) {
		return LogicalOperatorVisitor::VisitOperator(op);
	}
	auto &projection = op.Cast<LogicalProjection>();

	// Only push casts from a projection that forwards just column refs and
	// casts and reaches a GET without a join in between. A constant or other
	// expression makes the projection ineligible.
	// See test/sql/copy/csv/test_csv_error_message_type.test (top-level cast
	// to VARCHAR must still push) and test_large_integer_detection.test (a
	// nested cast to VARCHAR must not).
	bool clean = ReachesPushdownGet(projection);
	for (const auto &e : projection.expressions) {
		if (e->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF || BoundCastExpression::IsCast(*e)) {
			continue;
		}
		clean = false;
	}
	if (clean) {
		for (const auto &e : projection.expressions) {
			if (BoundCastExpression::IsCast(*e)) {
				top_level_casts.insert(e.get());
			}
		}
	}
	if (projections.count(projection.table_index)) {
		VisitOperatorChildren(op);
		return;
	}

	LogicalOperatorVisitor::VisitOperator(op);
}

unique_ptr<Expression> CastCollect::VisitReplace(BoundColumnRefExpression &expr, unique_ptr<Expression> *ptr) {
	if (const auto binding = Resolve(expr.Binding(), analyses, projections)) {
		// Column is used without cast applied to it, register a conflict.
		// Not emplace() as we need to update the value if it was present
		binding->analysis.col_to_expr[binding->column_index] = nullptr;
	}
	return std::move(*ptr);
}

unique_ptr<Expression> CastCollect::VisitReplace(BoundFunctionExpression &expr, unique_ptr<Expression> *ptr) {
	if (!BoundCastExpression::IsCast(expr)) {
		// Descend into non-cast function children so e.g. fn(col, other) still sees "col"
		return nullptr;
	}
	auto &cast_child = BoundCastExpression::Child(expr);
	if (cast_child.GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
		// Descend into children so e.g. fn(col, other) still sees "col" and
		// registers a conflict
		return nullptr;
	}
	const auto &bound_col = cast_child.Cast<BoundColumnRefExpression>();
	const auto binding = Resolve(bound_col.Binding(), analyses, projections);
	if (!binding) {
		return nullptr;
	}
	auto &col_to_cast = binding->analysis.col_to_expr;

	if (auto it = col_to_cast.find(binding->column_index); it == col_to_cast.end()) {
		// Only a top-level projection cast starts a candidate.
		if (top_level_casts.count(&expr)) {
			col_to_cast.emplace(binding->column_index, &expr);
		}
	} else if (it->second == nullptr || it->second->GetReturnType() != expr.GetReturnType() ||
	           BoundCastExpression::IsTryCast(it->second->Cast<BoundFunctionExpression>()) !=
	               BoundCastExpression::IsTryCast(expr)) {
		// Different target type, or already a conflict.
		// If reader can push CAST but not TRY_CAST or vice versa, we can't
		// pretend thery are the same
		it->second = nullptr;
	}

	return std::move(*ptr);
}

unique_ptr<Expression> CastReplace::VisitReplace(BoundColumnRefExpression &expr, unique_ptr<Expression> *ptr) {
	const auto binding = Resolve(expr.Binding(), analyses, projections);
	if (!binding) {
		return std::move(*ptr);
	}

	const auto &[analysis, column_index, projection, projection_column_index] = *binding;
	if (CanPushdownColumn(analysis, column_index)) {
		const LogicalType return_type = analysis.get.returned_types[analysis.StorageIndex(column_index)];
		expr.SetReturnType(return_type);
		// LogicalProjection types are resolved by calling
		// LogicalProjection::ResolveTypes, so we need to check whether types in
		// projection have been resolved, and updated them only if needed.
		if (projection != nullptr && !projection->types.empty()) {
			projection->types[projection_column_index] = return_type;
		}
	}

	return std::move(*ptr);
}

unique_ptr<Expression> CastReplace::VisitReplace(BoundFunctionExpression &expr, unique_ptr<Expression> *ptr) {
	if (!BoundCastExpression::IsCast(expr)) {
		return nullptr;
	}
	if (BoundCastExpression::Child(expr).GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
		return nullptr; // Same as in ScalarFnCollect::VisitReplace
	}
	auto &bound_col_base = BoundCastExpression::ChildMutable(expr);
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
	// Same as in CastReplace::VisitReplace(BoundColumnRefExpression)
	if (projection != nullptr && !projection->types.empty()) {
		projection->types[projection_column_index] = return_type;
	}
	return std::move(bound_col_base);
}

CastCollect::CastCollect(Analyses &analyses, const Projections &projections)
    : analyses(analyses), projections(projections) {
}

CastReplace::CastReplace(Analyses &analyses, const Projections &projections)
    : analyses(analyses), projections(projections) {
}

bool CanPushdownColumn(const GetAnalysis &analysis, ProjectionIndex idx) {
	const auto it = analysis.col_to_expr.find(idx);
	return it != analysis.col_to_expr.end() && it->second != nullptr;
}

idx_t GetAnalysis::StorageIndex(ProjectionIndex idx) const {
	return get.GetColumnIds()[idx].GetPrimaryIndex();
}
} // namespace duckdb
