#include "duckdb/optimizer/rule/predicate_factoring.hpp"

#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/column_binding_map.hpp"

namespace duckdb {

PredicateFactoringRule::PredicateFactoringRule(ExpressionRewriter &rewriter) : Rule(rewriter) {
	// Match on a ConjunctionExpression that has at least one ConjunctionExpression as a child
	auto op = make_uniq<ConjunctionExpressionMatcher>();
	op->matchers.push_back(make_uniq<ConjunctionExpressionMatcher>());
	op->policy = SetMatcher::Policy::SOME;
	root = std::move(op);
}

static bool ExpressionIsDisjunction(const Expression &expr) {
	return expr.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION &&
	       expr.GetExpressionType() == ExpressionType::CONJUNCTION_OR;
}

static void ExtractDisjunctedPredicates(Expression &expression, vector<reference<Expression>> &disjuncted_children) {
	if (ExpressionIsDisjunction(expression)) {
		auto &disjunction = expression.Cast<BoundConjunctionExpression>();
		for (auto &child : disjunction.GetChildren()) {
			ExtractDisjunctedPredicates(*child, disjuncted_children);
		}
	} else {
		disjuncted_children.push_back(expression);
	}
}

static bool ColumnBindingIsvalid(const ColumnBinding &column_binding) {
	return column_binding.table_index.IsValid() && column_binding.column_index.IsValid();
}

static bool GetSingleColumnBinding(const Expression &expr, ColumnBinding &column_binding) {
	if (expr.IsVolatile()) {
		return false;
	}

	column_binding = ColumnBinding();

	bool found_multiple = false;
	ExpressionIterator::VisitExpression<BoundColumnRefExpression>(expr, [&](const BoundColumnRefExpression &colref) {
		if (!ColumnBindingIsvalid(column_binding)) {
			column_binding = colref.Binding();
		} else if (column_binding != colref.Binding()) {
			found_multiple = true;
		}
	});

	return ColumnBindingIsvalid(column_binding) && !found_multiple;
}

static void ExtractDisjunctedPredicates(Expression &expression,
                                        column_binding_map_t<vector<reference<Expression>>> &binding_map);

static void ExtractConjunctedPredicates(BoundConjunctionExpression &conjunction,
                                        column_binding_map_t<vector<reference<Expression>>> &binding_map) {
	D_ASSERT(conjunction.GetExpressionType() == ExpressionType::CONJUNCTION_AND);
	for (auto &conjunction_child : conjunction.GetChildren()) {
		if (conjunction_child->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION &&
		    conjunction_child->GetExpressionType() == ExpressionType::CONJUNCTION_AND) {
			ExtractConjunctedPredicates(conjunction_child->Cast<BoundConjunctionExpression>(), binding_map);
		} else {
			ExtractDisjunctedPredicates(*conjunction_child, binding_map);
		}
	}
}

static void ExtractDisjunctedPredicates(Expression &expression,
                                        column_binding_map_t<vector<reference<Expression>>> &binding_map) {
	if (expression.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION &&
	    expression.GetExpressionType() == ExpressionType::CONJUNCTION_AND) {
		ExtractConjunctedPredicates(expression.Cast<BoundConjunctionExpression>(), binding_map);
	} else {
		ColumnBinding single_binding;
		if (GetSingleColumnBinding(expression, single_binding)) {
			binding_map[single_binding].push_back(expression);
		}
	}
}

static column_binding_map_t<vector<reference<Expression>>> GetDisjunctedPredicateMap(Expression &expression) {
	vector<reference<Expression>> disjuncted_children;
	ExtractDisjunctedPredicates(expression, disjuncted_children);
	D_ASSERT(disjuncted_children.size() > 1);

	// Extract predicates of the first child
	auto &first_child = disjuncted_children[0].get();
	D_ASSERT(!ExpressionIsDisjunction(first_child));
	column_binding_map_t<vector<reference<Expression>>> remaining_binding_map;
	ExtractDisjunctedPredicates(first_child, remaining_binding_map);

	for (idx_t child_idx = 1; child_idx < disjuncted_children.size(); child_idx++) {
		auto &child = disjuncted_children[child_idx].get();
		D_ASSERT(!ExpressionIsDisjunction(child));
		column_binding_map_t<vector<reference<Expression>>> child_binding_map;
		ExtractDisjunctedPredicates(child, child_binding_map);

		// Bindings must appear in both maps to be considered for predicate factoring
		for (auto it = remaining_binding_map.begin(); it != remaining_binding_map.end();) {
			auto child_it = child_binding_map.find(it->first);
			if (child_it == child_binding_map.end()) {
				it = remaining_binding_map.erase(it);
			} else {
				for (auto &new_predicate : child_it->second) {
					bool found = false;
					for (auto &existing_predicate : it->second) {
						if (new_predicate.get().Equals(existing_predicate.get())) {
							found = true;
							break;
						}
					}
					if (!found) {
						it->second.push_back(new_predicate.get());
					}
				}
				it++;
			}
		}
	}

	return remaining_binding_map;
}

unique_ptr<Expression> PredicateFactoringRule::Apply(LogicalOperator &op, vector<reference<Expression>> &bindings,
                                                     bool &changes_made, bool is_root) {
	// Only applies to top-level FILTER expressions
	if ((op.type != LogicalOperatorType::LOGICAL_FILTER && op.type != LogicalOperatorType::LOGICAL_ANY_JOIN) ||
	    !is_root) {
		return nullptr;
	}

	// The expression must be an OR TODO: could also implement some common expression extraction for AND
	if (!ExpressionIsDisjunction(bindings[0])) {
		return nullptr;
	}

	ColumnBinding column_binding;
	if (GetSingleColumnBinding(bindings[0], column_binding)) {
		return nullptr; // If it only applies to one column binding it can be pushed down already
	}

	// Extract map from single column binding to expression it is contained in
	const auto binding_map = GetDisjunctedPredicateMap(bindings[0].get());
	if (binding_map.empty()) {
		return nullptr; // None qualify
	}

	unique_ptr<Expression> derived_filter;
	for (auto &entry : binding_map) {
		D_ASSERT(!entry.second.empty());

		// Create disjunction on single-column predicates
		unique_ptr<Expression> column_filter;
		for (auto &expr : entry.second) {
			if (!column_filter) {
				column_filter = expr.get().Copy();
			} else {
				auto new_disjunction = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_OR);
				new_disjunction->GetChildrenMutable().push_back(std::move(column_filter));
				new_disjunction->GetChildrenMutable().push_back(expr.get().Copy());
				column_filter = std::move(new_disjunction);
			}
		}

		// Conjunct each single-column predicate together
		if (!derived_filter) {
			derived_filter = std::move(column_filter);
		} else {
			auto new_conjunction = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
			new_conjunction->GetChildrenMutable().push_back(std::move(column_filter));
			new_conjunction->GetChildrenMutable().push_back(std::move(derived_filter));
			derived_filter = std::move(new_conjunction);
		}
	}

	// Now add the derived filter as an AND to the original expression
	auto result = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
	result->GetChildrenMutable().push_back(bindings[0].get().Copy());
	result->GetChildrenMutable().push_back(std::move(derived_filter));
	return std::move(result);
}

} // namespace duckdb
