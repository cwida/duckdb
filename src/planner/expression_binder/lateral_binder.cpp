#include "duckdb/planner/expression_binder/lateral_binder.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/logical_operator_visitor.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_subquery_expression.hpp"
#include "duckdb/planner/operator/logical_cte.hpp"
#include "duckdb/planner/operator/logical_dependent_join.hpp"

namespace duckdb {

LateralBinder::LateralBinder(Binder &binder, ClientContext &context) : ExpressionBinder(binder, context) {
}

void LateralBinder::ExtractCorrelatedColumns(Expression &expr) {
	if (expr.GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
		auto &bound_colref = expr.Cast<BoundColumnRefExpression>();
		if (bound_colref.Depth() > 0) {
			// add the correlated column info
			CorrelatedColumnInfo info(bound_colref);
			if (std::find(correlated_columns.begin(), correlated_columns.end(), info) == correlated_columns.end()) {
				correlated_columns.AddColumn(std::move(info)); // TODO is adding to the front OK here?
			}
		}
	}
	ExpressionIterator::EnumerateChildren(expr, [&](Expression &child) { ExtractCorrelatedColumns(child); });
}

BindResult LateralBinder::BindColumnRef(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth, bool root_expression) {
	if (depth == 0) {
		throw InternalException("Lateral binder can only bind correlated columns");
	}
	auto result = ExpressionBinder::BindExpression(expr_ptr, depth);
	if (result.HasError()) {
		return result;
	}
	ExtractCorrelatedColumns(*result.expression);
	return result;
}

BindResult LateralBinder::BindExpression(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth, bool root_expression) {
	auto &expr = *expr_ptr;
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::DEFAULT:
		return BindUnsupportedExpression(expr, depth, "LATERAL join cannot contain DEFAULT clause!");
	case ExpressionClass::WINDOW:
		return BindResult("LATERAL join cannot contain window functions!");
	case ExpressionClass::COLUMN_REF:
		return BindColumnRef(expr_ptr, depth, root_expression);
	default:
		return ExpressionBinder::BindExpression(expr_ptr, depth);
	}
}

string LateralBinder::UnsupportedAggregateMessage() {
	return "LATERAL join cannot contain aggregates!";
}

//! Returns the column reference with a reduced depth, or nullptr if the depth does not need to be reduced
static unique_ptr<Expression> ReduceColumnRefDepth(const BoundColumnRefExpression &expr,
                                                   const CorrelatedColumns &correlated_columns) {
	if (expr.Depth() == 0) {
		return nullptr;
	}
	for (auto &correlated : correlated_columns) {
		if (correlated.binding == expr.Binding()) {
			D_ASSERT(expr.Depth() > 1);
			return expr.WithDepth(expr.Depth() - 1);
		}
	}
	return nullptr;
}

static void ReduceColumnDepth(CorrelatedColumns &columns, const CorrelatedColumns &affected_columns) {
	for (auto &s_correlated : columns) {
		for (auto &affected : affected_columns) {
			if (affected == s_correlated) {
				s_correlated.depth--;
				break;
			}
		}
	}
}

class ExpressionDepthReducerRecursive : public LogicalOperatorVisitor {
public:
	explicit ExpressionDepthReducerRecursive(const CorrelatedColumns &correlated) : correlated_columns(correlated) {
	}

	void VisitExpression(unique_ptr<Expression> *expression) override {
		auto &expr = **expression;
		if (expr.GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
			auto reduced = ReduceColumnRefDepth(expr.Cast<BoundColumnRefExpression>(), correlated_columns);
			if (reduced) {
				*expression = std::move(reduced);
			}
		} else if (expr.GetExpressionType() == ExpressionType::SUBQUERY) {
			ReduceExpressionSubquery(expr.Cast<BoundSubqueryExpression>(), correlated_columns);
		}
		LogicalOperatorVisitor::VisitExpression(expression);
	}

	void VisitOperator(LogicalOperator &op) override {
		if (op.type == LogicalOperatorType::LOGICAL_DEPENDENT_JOIN) {
			// rewrite correlated columns in child joins
			auto &bound_join = op.Cast<LogicalDependentJoin>();
			ReduceColumnDepth(bound_join.correlated_columns, correlated_columns);
		}
		// visit the children of the table ref
		LogicalOperatorVisitor::VisitOperator(op);
	}

	static void ReduceExpressionSubquery(BoundSubqueryExpression &expr, const CorrelatedColumns &correlated_columns) {
		ReduceColumnDepth(expr.GetBinder()->correlated_columns, correlated_columns);
		if (expr.Subquery().plan) {
			ExpressionDepthReducerRecursive recursive(correlated_columns);
			recursive.VisitOperator(*expr.Subquery().plan);
		}
	}

private:
	const CorrelatedColumns &correlated_columns;
};

class ExpressionDepthReducer : public LogicalOperatorVisitor {
public:
	explicit ExpressionDepthReducer(const CorrelatedColumns &correlated) : correlated_columns(correlated) {
	}

protected:
	unique_ptr<Expression> VisitReplace(BoundColumnRefExpression &expr, unique_ptr<Expression> *expr_ptr) override {
		return ReduceColumnRefDepth(expr, correlated_columns);
	}

	unique_ptr<Expression> VisitReplace(BoundSubqueryExpression &expr, unique_ptr<Expression> *expr_ptr) override {
		ExpressionDepthReducerRecursive::ReduceExpressionSubquery(expr, correlated_columns);
		return nullptr;
	}

	const CorrelatedColumns &correlated_columns;
};

void LateralBinder::ReduceExpressionDepth(LogicalOperator &op, const CorrelatedColumns &correlated) {
	ExpressionDepthReducer depth_reducer(correlated);
	depth_reducer.VisitOperator(op);
}

static void AddCorrelation(CorrelatedColumns &columns, CorrelatedColumnInfo info) {
	if (std::find(columns.begin(), columns.end(), info) == columns.end()) {
		columns.AddColumn(std::move(info));
	}
}

class LateralScopeInserter : public LogicalOperatorVisitor {
public:
	explicit LateralScopeInserter(const unordered_set<TableIndex> &lateral_bindings)
	    : lateral_bindings(lateral_bindings) {
	}

	CorrelatedColumns Insert(LogicalOperator &op) {
		CollectLocalBindings(op);
		VisitOperator(op);
		return std::move(correlated_columns);
	}

protected:
	void VisitOperator(LogicalOperator &op) override {
		if (op.type == LogicalOperatorType::LOGICAL_DEPENDENT_JOIN) {
			AdjustCorrelatedColumns(op.Cast<LogicalDependentJoin>().correlated_columns, scope_depth + 1);
			scope_depth++;
			LogicalOperatorVisitor::VisitOperator(op);
			scope_depth--;
			return;
		}
		if (op.type == LogicalOperatorType::LOGICAL_RECURSIVE_CTE ||
		    op.type == LogicalOperatorType::LOGICAL_MATERIALIZED_CTE) {
			AdjustCorrelatedColumns(op.Cast<LogicalCTE>().correlated_columns, scope_depth);
		}
		LogicalOperatorVisitor::VisitOperator(op);
	}

	unique_ptr<Expression> VisitReplace(BoundColumnRefExpression &expr, unique_ptr<Expression> *expr_ptr) override {
		if (!NeedsRebase(expr.Binding(), expr.Depth(), scope_depth)) {
			return nullptr;
		}
		auto rebased = expr.WithDepth(expr.Depth() + 1);
		AddCorrelation(correlated_columns, CorrelatedColumnInfo(rebased->Cast<BoundColumnRefExpression>()));
		return rebased;
	}

	unique_ptr<Expression> VisitReplace(BoundSubqueryExpression &expr, unique_ptr<Expression> *expr_ptr) override {
		AdjustCorrelatedColumns(expr.GetBinder()->correlated_columns, scope_depth + 1);
		if (expr.Subquery().plan) {
			CollectLocalBindings(*expr.Subquery().plan);
			scope_depth++;
			VisitOperator(*expr.Subquery().plan);
			scope_depth--;
		}
		return nullptr;
	}

private:
	void CollectLocalBindings(LogicalOperator &op) {
		for (auto &binding : op.GetColumnBindings()) {
			local_bindings.insert(binding.table_index);
		}
		for (auto &child : op.children) {
			CollectLocalBindings(*child);
		}
	}

	bool NeedsRebase(const ColumnBinding &binding, idx_t depth, idx_t current_scope_depth) const {
		if (local_bindings.count(binding.table_index)) {
			return false;
		}
		if (lateral_bindings.count(binding.table_index)) {
			return true;
		}
		return current_scope_depth == 0 ? depth > 0 : depth > current_scope_depth;
	}

	void AdjustCorrelatedColumns(CorrelatedColumns &columns, idx_t current_scope_depth) {
		for (auto &column : columns) {
			if (!NeedsRebase(column.binding, column.depth, current_scope_depth)) {
				continue;
			}
			column.depth++;
			AddCorrelation(correlated_columns, column);
		}
	}

	const unordered_set<TableIndex> &lateral_bindings;
	unordered_set<TableIndex> local_bindings;
	CorrelatedColumns correlated_columns;
	idx_t scope_depth = 0;
};

CorrelatedColumns LateralBinder::InsertLateralScope(LogicalOperator &op,
                                                    const unordered_set<TableIndex> &lateral_bindings) {
	LateralScopeInserter inserter(lateral_bindings);
	return inserter.Insert(op);
}

} // namespace duckdb
