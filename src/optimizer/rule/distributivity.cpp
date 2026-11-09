#include "duckdb/optimizer/rule/distributivity.hpp"

#include "duckdb/optimizer/matcher/expression_matcher.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"

namespace duckdb {

DistributivityRule::DistributivityRule(ExpressionRewriter &rewriter) : Rule(rewriter) {
	// we match on an OR expression within a LogicalFilter node
	root = make_uniq<ExpressionMatcher>();
	root->expr_type = make_uniq<SpecificExpressionTypeMatcher>(ExpressionType::CONJUNCTION_OR);
}

void DistributivityRule::AddExpressionSet(Expression &expr, expression_set_t &set) {
	// a volatile conjunct in two branches is two evaluations, so it is never common to the branches
	if (expr.GetExpressionType() == ExpressionType::CONJUNCTION_AND) {
		auto &and_expr = expr.Cast<BoundConjunctionExpression>();
		for (auto &child : and_expr.GetChildrenMutable()) {
			if (!child->IsVolatile()) {
				set.insert(*child);
			}
		}
	} else if (!expr.IsVolatile()) {
		set.insert(expr);
	}
}

unique_ptr<Expression> DistributivityRule::ExtractExpression(BoundConjunctionExpression &conj, idx_t idx,
                                                             const Expression &expr) {
	auto &child = conj.GetChildrenMutable()[idx];
	unique_ptr<Expression> result;
	if (child->GetExpressionType() == ExpressionType::CONJUNCTION_AND) {
		// AND, remove expression from the list
		auto &and_expr = child->Cast<BoundConjunctionExpression>();
		for (idx_t i = 0; i < and_expr.GetChildrenMutable().size(); i++) {
			if (and_expr.GetChildrenMutable()[i]->Equals(expr)) {
				result = std::move(and_expr.GetChildrenMutable()[i]);
				and_expr.GetChildrenMutable().erase_at(i);
				break;
			}
		}
		if (and_expr.GetChildrenMutable().size() == 1) {
			conj.GetChildrenMutable()[idx] = std::move(and_expr.GetChildrenMutable()[0]);
		}
	}
	// not an AND node(e.g. (X AND B) OR X) or this is the last expr,
	// remove the entire expression
	if (!result) {
		D_ASSERT(child->Equals(expr));
		result = std::move(child);
		conj.GetChildrenMutable()[idx] = nullptr;
	}
	D_ASSERT(result);
	return result;
}

unique_ptr<Expression> DistributivityRule::Apply(LogicalOperator &op, vector<reference<Expression>> &bindings,
                                                 bool &changes_made, bool is_root) {
	auto &initial_or = bindings[0].get().Cast<BoundConjunctionExpression>();

	// we want to find expressions that occur in each of the children of the OR
	// i.e. (X AND A) OR (X AND B) => X occurs in all branches
	// first, for the initial child, we create an expression set of which expressions occur
	// this is our initial candidate set (in the example: [X, A])
	expression_set_t candidate_set;
	AddExpressionSet(*initial_or.GetChildrenMutable()[0], candidate_set);
	// now for each of the remaining children, we create a set again and intersect them
	// in our example: the second set would be [X, B]
	// the intersection would leave [X]
	for (idx_t i = 1; i < initial_or.GetChildrenMutable().size(); i++) {
		expression_set_t next_set;
		AddExpressionSet(*initial_or.GetChildrenMutable()[i], next_set);
		expression_set_t intersect_result;
		for (auto &expr : candidate_set) {
			if (next_set.find(expr) != next_set.end()) {
				intersect_result.insert(expr);
			}
		}
		candidate_set = intersect_result;
	}
	if (candidate_set.empty()) {
		// nothing found: abort
		return nullptr;
	}
	// now for each of the remaining expressions in the candidate set we know that it is contained in all branches of
	// the OR
	auto new_root = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
	for (auto &expr : candidate_set) {
		D_ASSERT(initial_or.GetChildrenMutable().size() > 0);

		// extract the expression from the first child of the OR
		auto result = ExtractExpression(initial_or, 0, expr.get());
		// now for the subsequent expressions, simply remove the expression
		for (idx_t i = 1; i < initial_or.GetChildrenMutable().size(); i++) {
			ExtractExpression(initial_or, i, *result);
		}
		// now we add the expression to the new root
		new_root->GetChildrenMutable().push_back(std::move(result));
	}

	// check if we completely erased one of the children of the OR
	// this happens if we have an OR in the form of "X OR (X AND A)"
	// the left child will be completely empty, as it only contains common expressions
	// in this case, any other children are not useful:
	// X OR (X AND A) is the same as "X"
	// since (1) only tuples that do not qualify "X" will not pass this predicate
	//   and (2) all tuples that qualify "X" will pass this predicate
	for (idx_t i = 0; i < initial_or.GetChildrenMutable().size(); i++) {
		if (!initial_or.GetChildrenMutable()[i]) {
			if (new_root->GetChildrenMutable().size() <= 1) {
				return std::move(new_root->GetChildrenMutable()[0]);
			} else {
				return std::move(new_root);
			}
		}
	}
	// finally we need to add the remaining expressions in the OR to the new root
	if (initial_or.GetChildrenMutable().size() == 1) {
		// one child: skip the OR entirely and only add the single child
		new_root->GetChildrenMutable().push_back(std::move(initial_or.GetChildrenMutable()[0]));
	} else if (initial_or.GetChildrenMutable().size() > 1) {
		// multiple children still remain: push them into a new OR and add that to the new root
		auto new_or = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_OR);
		for (auto &child : initial_or.GetChildrenMutable()) {
			new_or->GetChildrenMutable().push_back(std::move(child));
		}
		new_root->GetChildrenMutable().push_back(std::move(new_or));
	}
	// finally return the new root
	if (new_root->GetChildrenMutable().size() == 1) {
		return std::move(new_root->GetChildrenMutable()[0]);
	}
	return std::move(new_root);
}

} // namespace duckdb
