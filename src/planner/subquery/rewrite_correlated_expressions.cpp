#include "duckdb/planner/subquery/rewrite_correlated_expressions.hpp"

#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_subquery_expression.hpp"
#include "duckdb/planner/query_node/bound_select_node.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_dependent_join.hpp"
#include "duckdb/optimizer/column_binding_replacer.hpp"

namespace duckdb {

RewriteCorrelatedExpressions::RewriteCorrelatedExpressions(column_binding_map_t<ColumnBinding> current_binding_map,
                                                           column_binding_map_t<ColumnBinding> &correlated_aliases)
    : current_binding_map(std::move(current_binding_map)), correlated_aliases(correlated_aliases) {
}

void RewriteCorrelatedExpressions::Rewrite(LogicalOperator &op, column_binding_map_t<ColumnBinding> current_binding_map,
                                           column_binding_map_t<ColumnBinding> &correlated_aliases) {
	RewriteCorrelatedExpressions rewriter(std::move(current_binding_map), correlated_aliases);
	rewriter.VisitOperator(op);
}

void RewriteCorrelatedExpressions::RegisterCorrelatedBinding(const ColumnBinding &source_binding,
                                                             const ColumnBinding &target_binding) {
	auto source_entry = correlated_aliases.find(source_binding);
	D_ASSERT(source_entry != correlated_aliases.end());
	auto result = correlated_aliases.emplace(target_binding, source_entry->second);
	if (!result.second) {
		D_ASSERT(result.first->second == source_entry->second);
	}
}

void RewriteCorrelatedExpressions::VisitOperator(LogicalOperator &op) {
	if (op.type != LogicalOperatorType::LOGICAL_DEPENDENT_JOIN) {
		VisitOperatorChildren(op);
	}
	// update the bindings in the correlated columns of the dependent join
	if (op.type == LogicalOperatorType::LOGICAL_DEPENDENT_JOIN) {
		auto &plan = op.Cast<LogicalDependentJoin>();
		for (auto &corr : plan.correlated_columns) {
			auto alias_entry = correlated_aliases.find(corr.binding);
			if (alias_entry != correlated_aliases.end()) {
				auto current_entry = current_binding_map.find(alias_entry->second);
				D_ASSERT(current_entry != current_binding_map.end());
				auto original_binding = corr.binding;
				corr.binding = current_entry->second;
				RegisterCorrelatedBinding(original_binding, corr.binding);
			}
		}
	}
	VisitOperatorExpressions(op);
}

unique_ptr<Expression> RewriteCorrelatedExpressions::VisitReplace(BoundColumnRefExpression &expr,
                                                                  unique_ptr<Expression> *expr_ptr) {
	if (expr.Depth() == 0) {
		return nullptr;
	}
	auto alias_entry = correlated_aliases.find(expr.Binding());
	if (alias_entry == correlated_aliases.end()) {
		return nullptr;
	}
	auto current_entry = current_binding_map.find(alias_entry->second);
	D_ASSERT(current_entry != current_binding_map.end());
	auto original_binding = expr.Binding();
	if (original_binding == current_entry->second) {
		expr.DepthMutable() = 0;
		return nullptr;
	}
	expr.BindingMutable() = current_entry->second;
	RegisterCorrelatedBinding(original_binding, expr.Binding());
	// The current representative is produced inside the converted algebra. Once a
	// reference points at it, it is local regardless of the original binder depth.
	expr.DepthMutable() = 0;
	return nullptr;
}

RewriteCorrelatedAggregates::RewriteCorrelatedAggregates(
    column_binding_map_t<CorrelatedAggregateReplacement> &replacement_map)
    : replacement_map(replacement_map) {
}

void RewriteCorrelatedAggregates::Rewrite(LogicalOperator &op,
                                          column_binding_map_t<CorrelatedAggregateReplacement> &replacement_map) {
	RewriteCorrelatedAggregates rewriter(replacement_map);
	rewriter.VisitOperator(op);
}

unique_ptr<Expression> RewriteCorrelatedAggregates::VisitReplace(BoundColumnRefExpression &expr,
                                                                 unique_ptr<Expression> *expr_ptr) {
	auto entry = replacement_map.find(expr.Binding());
	if (entry != replacement_map.end()) {
		// Replace this with CASE WHEN the aggregate group is missing THEN EMPTY_RESULT ELSE AGGREGATE END.
		auto is_null = make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_IS_NULL, LogicalType::BOOLEAN);
		is_null->GetChildrenMutable().push_back(
		    make_uniq<BoundColumnRefExpression>(LogicalType::BIGINT, entry->second.marker));
		auto check = std::move(is_null);
		auto result_if_true = make_uniq<BoundColumnRefExpression>(expr.GetReturnType(), entry->second.empty_result);
		auto result_if_false = std::move(*expr_ptr);
		return make_uniq<BoundCaseExpression>(std::move(check), std::move(result_if_true), std::move(result_if_false));
	}
	return nullptr;
}

} // namespace duckdb
