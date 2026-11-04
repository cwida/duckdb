//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/type_pushdown.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once
#include "duckdb/common/optional.hpp"
#include "duckdb/common/projection_index.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/planner/operator/logical_get.hpp"

namespace duckdb {
class ClientContext;

struct GetAnalysis {
	LogicalGet &get;
	/**
	 * for CAST(col), mapping of "col scan index" -> "expression".
	 * "expression" is nullptr iff column is used with a different expression
	 * or without expression in the query plan.
	 */
	unordered_map<ProjectionIndex, const Expression *> col_to_expr;

	idx_t StorageIndex(ProjectionIndex idx) const;
};

using Analyses = unordered_map<TableIndex, GetAnalysis>;

/*
 * SELECT CAST(col AS T) FROM '*.csv' yields a PROJECTION CAST(col) -> GET (csv)
 * plan. PROJECTION's "col" table_index is 1, GET's table_index is 0.
 * So we want to track original table_index for GET in case column is found
 * in filter we failed to push down (i.e. WHERE CAST(col AS T) > 0) as well as
 * projection's table_index.
 *
 * So we keep a mapping of
 *
 * "projection table index" to "projection operator".
 *
 * to resolve this.
 * For simplicity, current implementation is limited to one level i.e.
 * PROJECTION -> GET (i.e. read from VIEW) is pushed down but VIEW->VIEW->GET
 * or VIEW->CTE->GET is not.
 *
 * Storing a reference is fine because the plan outlives the optimizer pass.
 */
using Projections = unordered_map<TableIndex, LogicalProjection &>;

/**
 * Collect CAST(col) expressions. If "col" is used without CAST in "plan",
 * record in "analyses.conflicts"
 */
struct CastCollect final : LogicalOperatorVisitor {
	Analyses &analyses;
	const Projections &projections;
	// Casts that are direct outputs of a clean projection over a GET. Only these
	// start a pushdown candidate; a nested cast may push down a different value.
	// See test/sql/copy/csv/auto/test_large_integer_detection.test
	unordered_set<const Expression *> top_level_casts;

	CastCollect(Analyses &analyses, const Projections &projections);
	void VisitOperator(LogicalOperator &op) override;
	unique_ptr<Expression> VisitReplace(BoundColumnRefExpression &expr, unique_ptr<Expression> *ptr) override;
	unique_ptr<Expression> VisitReplace(BoundFunctionExpression &expr, unique_ptr<Expression> *ptr) override;
};

/*
 * For "col" in columns collected by ScalarFnCollect, replace CAST(col) to "col"
 * if "col" doesn't have conflicting usage. Update return types for bound
 * columns and logical projections referencing this column.
 */
struct CastReplace final : LogicalOperatorVisitor {
	Analyses &analyses;
	const Projections &projections;

	CastReplace(Analyses &analyses, const Projections &aliases);
	unique_ptr<Expression> VisitReplace(BoundColumnRefExpression &expr, unique_ptr<Expression> *ptr) override;
	unique_ptr<Expression> VisitReplace(BoundFunctionExpression &expr, unique_ptr<Expression> *ptr) override;
};

void FindGetsAndProjections(LogicalOperator &op, Analyses &analyses, Projections &aliases);

struct GetBinding {
	GetAnalysis &analysis;
	// Column index within LogicalGet
	ProjectionIndex column_index;
	// If column binding was part of a projection, this is non-nullptr
	LogicalProjection *projection;
	// If column binding was part of projection, column index within
	// LogicalProjection wrapping a LogicalGet
	ProjectionIndex projection_column_index;
};

/*
 * Given a column binding, resolve it to a GET and a GET's column scan index.
 * Returns nullopt for virtual columns and columns which are neither part of
 * GET nor part of PROJECTION wrapping a GET.
 */
optional<GetBinding> Resolve(ColumnBinding binding, Analyses &analyses, const Projections &projections);

template <class Collect, class Replace>
unique_ptr<LogicalOperator> PushdownOptimize(ClientContext &context, unique_ptr<LogicalOperator> op) {
	Analyses analyses;
	Projections projections;
	FindGetsAndProjections(*op, analyses, projections);
	if (analyses.empty()) {
		return op;
	}
	Collect(analyses, projections).VisitOperator(*op);

	bool any_pushed = false;
	for (auto &[_, analysis] : analyses) {
		for (auto &[column_index, expr] : analysis.col_to_expr) {
			if (expr == nullptr) { // Conflict for column
				continue;
			}
			// Resolve() never yields a virtual column as a candidate
			D_ASSERT(!analysis.get.GetColumnIds()[column_index].IsVirtualColumn());
			TableFunctionProjectionExpressionInput input {analysis.get, *expr, column_index};
			if (analysis.get.function.projection_expression_pushdown(context, input)) {
				analysis.get.has_pushed_projection = true;
				analysis.get.returned_types[analysis.StorageIndex(column_index)] = expr->GetReturnType();
				if (!analysis.get.types.empty()) {
					analysis.get.ResolveOperatorTypes();
				}
				any_pushed = true;
			} else { // failed to push down expression, can't replace it
				expr = nullptr;
			}
		}
	}

	if (any_pushed) {
		Replace(analyses, projections).VisitOperator(*op);
	}
	return op;
}

class TypePushdown {
public:
	explicit inline TypePushdown(ClientContext &context) : context(context) {
	}
	inline unique_ptr<LogicalOperator> Optimize(unique_ptr<LogicalOperator> op) {
		return PushdownOptimize<CastCollect, CastReplace>(context, std::move(op));
	}

private:
	ClientContext &context;
};

bool CanPushdownColumn(const GetAnalysis &analysis, ProjectionIndex idx);
} // namespace duckdb
