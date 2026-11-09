#include "duckdb/optimizer/filter_pushdown.hpp"
#include "duckdb/parser/expression_map.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"

namespace duckdb {

using Filter = FilterPushdown::Filter;

//! A representation of a coalesce expression that removes unnecessary usages of
//! `coalesce` in its arguments when such usages don't affect the final value of
//! the expression.
//! E.g. `coalesce(a, coalesce(b, c))` is equivalent to `coalesce(a, b, c)` so
//! that similarly, with some abuse of notation,
//! `FlattenedCoalesce::Of({a, coalesce(b, c)}) == FlattenedCoalesce.Of({a, b, c})`
struct FlattenedCoalesce {
public:
	vector<reference<Expression>> args;

	bool operator==(const FlattenedCoalesce &other) const {
		if (args.size() != other.args.size()) {
			return false;
		}

		for (idx_t i = 0; i < args.size(); i++) {
			if (!args[i].get().Equals(other.args[i].get())) {
				return false;
			}
		}

		return true;
	}

	static FlattenedCoalesce Of(const vector<reference<Expression>> &expressions) {
		vector<reference<Expression>> args {};
		for (auto expr : expressions) {
			EnumerateFlattenedCoalesceArgs(expr, [&](Expression &arg) { args.push_back(arg); });
		}
		return {args};
	}

private:
	static void EnumerateFlattenedCoalesceArgs(Expression &expr, const std::function<void(Expression &arg)> &callback) {
		if (expr.GetExpressionType() == ExpressionType::OPERATOR_COALESCE) {
			ExpressionIterator::EnumerateChildren(
			    expr, [&](Expression &arg) { EnumerateFlattenedCoalesceArgs(arg, callback); });
		} else {
			callback(expr);
		}
	}
};

struct FlattenedCoalesceHash {
	hash_t operator()(const FlattenedCoalesce &coalesce) const {
		hash_t hash = 0;
		for (auto arg : coalesce.args) {
			hash = CombineHash(hash, arg.get().Hash());
		}
		return hash;
	}
};

//! Replace all occurrences of `exprs_to_replace` in `expr` with `replacement_expr`
static unique_ptr<Expression> ReplaceIn(unique_ptr<Expression> expr, const expression_set_t &exprs_to_replace,
                                        const Expression &replacement_expr) {
	ExpressionIterator::EnumerateExpression(expr, [&](unique_ptr<Expression> &sub_expr) {
		if (exprs_to_replace.find(*sub_expr) != exprs_to_replace.end()) {
			sub_expr = replacement_expr.Copy();
		}
	});

	return expr;
}

//! True if replacing all the `args` expressions occurring in `expr` with a
//! fixed constant would make the `expr` a scalar value.
static bool ExprIsFunctionOnlyOf(const Expression &expr, const expression_set_t &args) {
	auto expr_to_check = expr.Copy();

	ExpressionIterator::EnumerateExpression(expr_to_check, [&](unique_ptr<Expression> &sub_expr) {
		if (args.find(*sub_expr) != args.end()) {
			auto null_value = make_uniq<BoundConstantExpression>(Value(sub_expr->GetReturnType()));
			sub_expr = std::move(null_value);
		}
	});

	return expr_to_check->IsScalar();
}

//! Whenever a filter is of the form `P(coalesce(l, r))` or `P(coalesce(r, l))`
//! where `P` is some predicate that depends only on `coalesce(l, r)` and there
//! is a join condition of the form `l = r` where `l` and `r` are join keys for
//! the left and right table respectively, then pushdown `P(l)` to the left
//! table, `P(r)` to the right table, and remove the original filter.
static bool
PushDownFiltersOnCoalescedEqualJoinKeys(vector<unique_ptr<Filter>> &filters, vector<JoinCondition> &join_conditions,
                                        const std::function<void(unique_ptr<Expression> filter)> &pushdown_left,
                                        const std::function<void(unique_ptr<Expression> filter)> &pushdown_right) {
	// Generate set of all possible coalesced join keys expressions to later
	// discover filters on such expressions which are candidates for pushdown
	unordered_map<FlattenedCoalesce, reference<const JoinCondition>, FlattenedCoalesceHash>
	    join_cond_by_coalesced_join_keys;

	for (auto &cond : join_conditions) {
		if (cond.IsComparison() && cond.GetComparisonType() == ExpressionType::COMPARE_EQUAL) {
			auto &left = cond.GetLHS();
			auto &right = cond.GetRHS();
			auto coalesce_left_right = FlattenedCoalesce::Of({std::ref(left), std::ref(right)});
			auto coalesce_right_left = FlattenedCoalesce::Of({std::ref(right), std::ref(left)});
			join_cond_by_coalesced_join_keys.emplace(coalesce_left_right, std::ref(cond));
			join_cond_by_coalesced_join_keys.emplace(coalesce_right_left, std::ref(cond));
			join_cond_by_coalesced_join_keys.emplace(coalesce_left_right, std::ref(cond));
			join_cond_by_coalesced_join_keys.emplace(coalesce_right_left, std::ref(cond));
		}
	}

	if (join_cond_by_coalesced_join_keys.empty()) {
		return false;
	}

	bool has_applied_pushdown = false;
	for (idx_t i = 0; i < filters.size(); i++) {
		auto &filter = filters[i]->filter;
		if (filter->IsVolatile() || filter->CanThrow()) {
			continue;
		}

		// occurrences of equivalent coalesce expressions on the same join keys
		// which need to be replaced if the filter is to be pushed down
		expression_set_t coalesce_exprs_to_replace;
		const JoinCondition *join_cond_ptr = nullptr;
		bool many_non_equivalent_coalesce_exprs = false;

		ExpressionIterator::EnumerateExpression(filter, [&](Expression &sub_expr) {
			if (many_non_equivalent_coalesce_exprs ||
			    sub_expr.GetExpressionType() != ExpressionType::OPERATOR_COALESCE) {
				return;
			}

			auto sub_expr_flattened_coalesce = FlattenedCoalesce::Of({sub_expr});
			auto join_cond_it = join_cond_by_coalesced_join_keys.find(sub_expr_flattened_coalesce);
			if (join_cond_it == join_cond_by_coalesced_join_keys.end()) {
				return;
			}

			auto new_join_cond_ptr = &join_cond_it->second.get();
			if (join_cond_ptr && new_join_cond_ptr != join_cond_ptr) {
				many_non_equivalent_coalesce_exprs = true;
				return;
			}

			join_cond_ptr = new_join_cond_ptr;
			coalesce_exprs_to_replace.insert(sub_expr);
		});

		if (coalesce_exprs_to_replace.empty() || many_non_equivalent_coalesce_exprs ||
		    !ExprIsFunctionOnlyOf(*filter, coalesce_exprs_to_replace)) {
			continue;
		}

		auto left_filter = ReplaceIn(filter->Copy(), coalesce_exprs_to_replace, join_cond_ptr->GetLHS());
		auto right_filter = ReplaceIn(filter->Copy(), coalesce_exprs_to_replace, join_cond_ptr->GetRHS());
		pushdown_left(std::move(left_filter));
		pushdown_right(std::move(right_filter));
		filters.erase_at(i);
		has_applied_pushdown = true;
		i--;
	}

	return has_applied_pushdown;
}

unique_ptr<LogicalOperator> FilterPushdown::PushdownOuterJoin(unique_ptr<LogicalOperator> op,
                                                              unordered_set<TableIndex> &left_bindings,
                                                              unordered_set<TableIndex> &right_bindings) {
	if (op->type != LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
		return FinishPushdown(std::move(op));
	}

	auto &join = op->Cast<LogicalComparisonJoin>();
	D_ASSERT(join.join_type == JoinType::OUTER);

	FilterPushdown left_pushdown(optimizer, convert_mark_joins, projection_mode);
	FilterPushdown right_pushdown(optimizer, convert_mark_joins, projection_mode);
	auto has_applied_pushdown = PushDownFiltersOnCoalescedEqualJoinKeys(
	    filters, join.conditions, [&](unique_ptr<Expression> filter) { left_pushdown.AddFilter(std::move(filter)); },
	    [&](unique_ptr<Expression> filter) { right_pushdown.AddFilter(std::move(filter)); });

	if (!has_applied_pushdown) {
		return FinishPushdown(std::move(op));
	}

	left_pushdown.GenerateFilters();
	right_pushdown.GenerateFilters();
	left_pushdown.RewriteChild(*op, 0);
	right_pushdown.RewriteChild(*op, 1);
	return PushFinalFilters(std::move(op));
}

} // namespace duckdb
