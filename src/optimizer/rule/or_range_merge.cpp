#include "duckdb/optimizer/rule/or_range_merge.hpp"

#include "duckdb/planner/expression/bound_between_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_lambda_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/common/algorithm.hpp"

namespace duckdb {

namespace {

//! One side of a range; a side that is not present leaves the range open on that side
struct RangeBound {
	bool present = false;
	Value value;
	bool inclusive = false;
};

//! A child of the OR that restricts one expression to a range with constant bounds
struct RangePredicate {
	RangePredicate(idx_t child_idx, Expression &input, RangeBound lower, RangeBound upper)
	    : child_idx(child_idx), input(input), lower(std::move(lower)), upper(std::move(upper)) {
	}

	//! The position of the child in the OR
	idx_t child_idx;
	//! The expression the range restricts
	reference<Expression> input;
	RangeBound lower;
	RangeBound upper;
	//! Whether the predicate has been assigned to a group of predicates on the same input
	bool grouped = false;
};

//! Types whose constants compare the same way through Value as through the comparison operators
bool SupportsRangeMerge(const LogicalType &type) {
	switch (type.InternalType()) {
	case PhysicalType::INT8:
	case PhysicalType::INT16:
	case PhysicalType::INT32:
	case PhysicalType::INT64:
	case PhysicalType::INT128:
	case PhysicalType::UINT8:
	case PhysicalType::UINT16:
	case PhysicalType::UINT32:
	case PhysicalType::UINT64:
	case PhysicalType::UINT128:
	case PhysicalType::FLOAT:
	case PhysicalType::DOUBLE:
	case PhysicalType::VARCHAR:
	case PhysicalType::INTERVAL:
		return true;
	default:
		return false;
	}
}

//! Whether the expression holds a constant anywhere in its tree; a lambda body is not a child of its function, so it
//! is walked explicitly
bool HasConstant(const Expression &expr) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
		return true;
	}
	bool found = false;
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_LAMBDA) {
		auto &lambda = expr.Cast<BoundLambdaExpression>();
		found = HasConstant(*lambda.LambdaExpr());
		for (auto &capture : lambda.Captures()) {
			found = found || HasConstant(*capture);
		}
	}
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) { found = found || HasConstant(child); });
	return found;
}

//! Whether equal inputs compute the same value: constants compare by value, so an input that holds one may only
//! look the same (v + INTERVAL '1 month' and v + INTERVAL '30 days', atan2(x, 0.0) and atan2(x, -0.0))
bool IsMergeableInput(const Expression &input) {
	return !input.IsVolatile() && !HasConstant(input);
}

//! A constant bound: a non-NULL BoundConstantExpression with the type of the input
bool TryGetBound(const Expression &expr, const LogicalType &input_type, bool inclusive, RangeBound &bound) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
		return false;
	}
	auto &value = expr.Cast<BoundConstantExpression>().GetValue();
	if (value.IsNull() || value.type() != input_type) {
		return false;
	}
	bound.present = true;
	bound.value = value;
	bound.inclusive = inclusive;
	return true;
}

//! x > a, x >= a, x < b, x <= b with a constant on either side: a range that is open on one side
bool TryGetComparisonRange(Expression &expr, optional_ptr<Expression> &input, RangeBound &lower, RangeBound &upper) {
	auto comparison_type = expr.GetExpressionType();
	switch (comparison_type) {
	case ExpressionType::COMPARE_LESSTHAN:
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
	case ExpressionType::COMPARE_GREATERTHAN:
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		break;
	default:
		return false;
	}
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return false;
	}
	auto &comparison = expr.Cast<BoundFunctionExpression>();
	auto &left = BoundComparisonExpression::LeftMutable(comparison);
	auto &right = BoundComparisonExpression::RightMutable(comparison);
	optional_ptr<Expression> constant;
	if (right->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
		input = left.get();
		constant = right.get();
	} else if (left->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
		// a > x is x < a
		input = right.get();
		constant = left.get();
		comparison_type = FlipComparisonExpression(comparison_type);
	} else {
		return false;
	}
	bool inclusive = comparison_type == ExpressionType::COMPARE_LESSTHANOREQUALTO ||
	                 comparison_type == ExpressionType::COMPARE_GREATERTHANOREQUALTO;
	bool is_upper = comparison_type == ExpressionType::COMPARE_LESSTHAN ||
	                comparison_type == ExpressionType::COMPARE_LESSTHANOREQUALTO;
	return TryGetBound(*constant, input->GetReturnType(), inclusive, is_upper ? upper : lower);
}

//! A child of the OR that is a range on one expression with constant bounds: BETWEEN, a single comparison, or an AND
//! of a lower and an upper comparison on the same expression (how the binder plans BETWEEN on a column)
bool TryGetRange(Expression &expr, optional_ptr<Expression> &input, RangeBound &lower, RangeBound &upper) {
	switch (expr.GetExpressionType()) {
	case ExpressionType::COMPARE_BETWEEN: {
		if (expr.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
			return false;
		}
		auto &between = expr.Cast<BoundFunctionExpression>();
		if (!BoundBetweenExpression::HasValidBindData(between)) {
			return false;
		}
		input = BoundBetweenExpression::InputMutable(between).get();
		auto &input_type = input->GetReturnType();
		return TryGetBound(BoundBetweenExpression::LowerBound(between), input_type,
		                   BoundBetweenExpression::LowerInclusive(between), lower) &&
		       TryGetBound(BoundBetweenExpression::UpperBound(between), input_type,
		                   BoundBetweenExpression::UpperInclusive(between), upper);
	}
	case ExpressionType::CONJUNCTION_AND: {
		auto &conjunction = expr.Cast<BoundConjunctionExpression>();
		auto &children = conjunction.GetChildrenMutable();
		if (children.size() != 2) {
			return false;
		}
		optional_ptr<Expression> second_input;
		if (!TryGetComparisonRange(*children[0], input, lower, upper) ||
		    !TryGetComparisonRange(*children[1], second_input, lower, upper)) {
			return false;
		}
		// one lower and one upper bound on the same expression
		return lower.present && upper.present && input->Equals(*second_input) && IsMergeableInput(*input);
	}
	default:
		return TryGetComparisonRange(expr, input, lower, upper);
	}
}

//! Whether the range is non-empty, which the merge relies on when it extends an earlier range over this one
bool IsNonEmptyRange(const RangeBound &lower, const RangeBound &upper) {
	if (!lower.present || !upper.present) {
		return true;
	}
	if (lower.value < upper.value) {
		return true;
	}
	return lower.value == upper.value && lower.inclusive && upper.inclusive;
}

//! Orders ranges by their lower bound: open first, then by value, with inclusive before exclusive on a tie
bool LowerBoundLessThan(const RangeBound &left, const RangeBound &right) {
	if (!left.present || !right.present) {
		return !left.present && right.present;
	}
	if (left.value == right.value) {
		return left.inclusive && !right.inclusive;
	}
	return left.value < right.value;
}

template <class T>
bool IsSuccessor(const Value &upper, const Value &lower) {
	// lower > upper, so subtracting one from lower cannot underflow
	return lower.GetValueUnsafe<T>() - T(1) == upper.GetValueUnsafe<T>();
}

//! Whether lower is the integer that follows upper, e.g. x <= 20 and x >= 21 cover every integer from 1 to 40
bool IsAdjacentInteger(const Value &upper, const Value &lower) {
	auto &type = upper.type();
	if (!type.IsIntegral()) {
		return false;
	}
	switch (type.InternalType()) {
	case PhysicalType::INT8:
		return IsSuccessor<int8_t>(upper, lower);
	case PhysicalType::INT16:
		return IsSuccessor<int16_t>(upper, lower);
	case PhysicalType::INT32:
		return IsSuccessor<int32_t>(upper, lower);
	case PhysicalType::INT64:
		return IsSuccessor<int64_t>(upper, lower);
	case PhysicalType::INT128:
		return IsSuccessor<hugeint_t>(upper, lower);
	case PhysicalType::UINT8:
		return IsSuccessor<uint8_t>(upper, lower);
	case PhysicalType::UINT16:
		return IsSuccessor<uint16_t>(upper, lower);
	case PhysicalType::UINT32:
		return IsSuccessor<uint32_t>(upper, lower);
	case PhysicalType::UINT64:
		return IsSuccessor<uint64_t>(upper, lower);
	case PhysicalType::UINT128:
		return IsSuccessor<uhugeint_t>(upper, lower);
	default:
		return false;
	}
}

//! Whether a range that starts at lower continues the range that ends at upper, i.e. their union has no hole
bool Connects(const RangeBound &upper, const RangeBound &lower) {
	if (!upper.present || !lower.present) {
		return true;
	}
	if (lower.value < upper.value) {
		return true;
	}
	if (lower.value == upper.value) {
		// [1, 20] and (20, 40] both cover 20 together, [1, 20) and (20, 40] leave it out
		return upper.inclusive || lower.inclusive;
	}
	return upper.inclusive && lower.inclusive && IsAdjacentInteger(upper.value, lower.value);
}

//! Extends the upper bound of a range to the upper bound of a range it connects to, when that one reaches further
void ExtendUpperBound(RangeBound &upper, const RangeBound &other) {
	if (!upper.present) {
		return;
	}
	if (!other.present || upper.value < other.value) {
		upper = other;
	} else if (upper.value == other.value) {
		upper.inclusive = upper.inclusive || other.inclusive;
	}
}

unique_ptr<Expression> MakeComparison(ExpressionType type, const Expression &input, const RangeBound &bound) {
	return BoundComparisonExpression::Create(type, input.Copy(), make_uniq<BoundConstantExpression>(bound.value));
}

//! The merged range: a BETWEEN when both bounds are present, otherwise a single comparison
unique_ptr<Expression> MakeRange(const Expression &input, const RangeBound &lower, const RangeBound &upper) {
	auto lower_type =
	    lower.inclusive ? ExpressionType::COMPARE_GREATERTHANOREQUALTO : ExpressionType::COMPARE_GREATERTHAN;
	auto upper_type = upper.inclusive ? ExpressionType::COMPARE_LESSTHANOREQUALTO : ExpressionType::COMPARE_LESSTHAN;
	if (!lower.present) {
		return MakeComparison(upper_type, input, upper);
	}
	if (!upper.present) {
		return MakeComparison(lower_type, input, lower);
	}
	if (input.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		// like the binder: a column reference is cheap to copy and two comparisons are what the pushdown works with
		return make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND,
		                                             MakeComparison(lower_type, input, lower),
		                                             MakeComparison(upper_type, input, upper));
	}
	return BoundBetweenExpression::Create(input.Copy(), make_uniq<BoundConstantExpression>(lower.value),
	                                      make_uniq<BoundConstantExpression>(upper.value), lower.inclusive,
	                                      upper.inclusive);
}

} // namespace

OrRangeMergeRule::OrRangeMergeRule(ExpressionRewriter &rewriter) : Rule(rewriter) {
	// match on an OR expression
	root = make_uniq<ExpressionMatcher>();
	root->expr_type = make_uniq<SpecificExpressionTypeMatcher>(ExpressionType::CONJUNCTION_OR);
}

unique_ptr<Expression> OrRangeMergeRule::Apply(LogicalOperator &op, vector<reference<Expression>> &bindings,
                                               bool &changes_made, bool is_root) {
	auto &disjunction = bindings[0].get().Cast<BoundConjunctionExpression>();
	auto &children = disjunction.GetChildrenMutable();

	// collect the children that are ranges with constant bounds
	vector<RangePredicate> ranges;
	for (idx_t child_idx = 0; child_idx < children.size(); child_idx++) {
		optional_ptr<Expression> input;
		RangeBound lower;
		RangeBound upper;
		if (!TryGetRange(*children[child_idx], input, lower, upper)) {
			continue;
		}
		if (!SupportsRangeMerge(input->GetReturnType()) || !IsMergeableInput(*input) ||
		    !IsNonEmptyRange(lower, upper)) {
			continue;
		}
		ranges.emplace_back(child_idx, *input, std::move(lower), std::move(upper));
	}
	if (ranges.size() < 2) {
		return nullptr;
	}

	// merge the ranges on the same input: sorted by lower bound, every range that connects to the running range
	// extends it; the merged range replaces the first of its members, the other members are dropped
	vector<unique_ptr<Expression>> replacements(children.size());
	vector<bool> dropped(children.size(), false);
	bool merged_any = false;
	for (idx_t i = 0; i < ranges.size(); i++) {
		if (ranges[i].grouped) {
			continue;
		}
		vector<idx_t> group;
		for (idx_t j = i; j < ranges.size(); j++) {
			if (!ranges[j].grouped && ranges[j].input.get().Equals(ranges[i].input.get())) {
				ranges[j].grouped = true;
				group.push_back(j);
			}
		}
		if (group.size() < 2) {
			continue;
		}
		std::sort(group.begin(), group.end(),
		          [&](idx_t a, idx_t b) { return LowerBoundLessThan(ranges[a].lower, ranges[b].lower); });

		vector<idx_t> members;
		RangeBound lower;
		RangeBound upper;
		auto flush = [&]() {
			if (members.size() < 2) {
				return;
			}
			merged_any = true;
			idx_t first = *std::min_element(members.begin(), members.end());
			replacements[first] = MakeRange(ranges[group[0]].input.get(), lower, upper);
			for (auto member : members) {
				dropped[member] = member != first;
			}
		};
		for (auto range_idx : group) {
			auto &range = ranges[range_idx];
			// a range that is open on both sides is not a range anymore: start over instead
			bool connects = !members.empty() && Connects(upper, range.lower) && (lower.present || range.upper.present);
			if (!connects) {
				flush();
				members.clear();
				lower = range.lower;
				upper = range.upper;
			} else {
				ExtendUpperBound(upper, range.upper);
			}
			members.push_back(range.child_idx);
		}
		flush();
	}
	if (!merged_any) {
		return nullptr;
	}

	vector<unique_ptr<Expression>> new_children;
	for (idx_t child_idx = 0; child_idx < children.size(); child_idx++) {
		if (dropped[child_idx]) {
			continue;
		}
		if (replacements[child_idx]) {
			new_children.push_back(std::move(replacements[child_idx]));
		} else {
			new_children.push_back(std::move(children[child_idx]));
		}
	}
	if (new_children.size() == 1) {
		return std::move(new_children[0]);
	}
	auto result = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_OR);
	for (auto &child : new_children) {
		result->GetChildrenMutable().push_back(std::move(child));
	}
	return std::move(result);
}

} // namespace duckdb
