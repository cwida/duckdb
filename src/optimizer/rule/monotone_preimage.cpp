#include "duckdb/optimizer/rule/monotone_preimage.hpp"
#include "duckdb/main/settings.hpp"

#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/value_operations/value_operations.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/arg_properties.hpp"
#include "duckdb/optimizer/expression_rewriter.hpp"
#include "duckdb/optimizer/matcher/expression_matcher.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

namespace duckdb {

MonotonePreimageRule::MonotonePreimageRule(ExpressionRewriter &rewriter) : Rule(rewriter) {
	// match a comparison between a foldable constant and a unary function of some expression
	auto op = make_uniq<ComparisonExpressionMatcher>();
	op->policy = SetMatcher::Policy::UNORDERED;
	op->matchers.push_back(make_uniq<FoldableConstantMatcher>());
	auto func = make_uniq<FunctionExpressionMatcher>();
	func->matchers.push_back(make_uniq<ExpressionMatcher>());
	func->policy = SetMatcher::Policy::ORDERED;
	op->matchers.push_back(std::move(func));
	root = std::move(op);
}

//! Finite, midpoint-overflow-safe integer domain of an exact-ordered column type.
static bool GetFiniteDomain(const LogicalType &type, int64_t &lo, int64_t &hi, bool &has_infinity) {
	has_infinity = false;
	switch (type.id()) {
	case LogicalTypeId::TINYINT:
		lo = NumericLimits<int8_t>::Minimum();
		hi = NumericLimits<int8_t>::Maximum();
		return true;
	case LogicalTypeId::SMALLINT:
		lo = NumericLimits<int16_t>::Minimum();
		hi = NumericLimits<int16_t>::Maximum();
		return true;
	case LogicalTypeId::INTEGER:
		lo = NumericLimits<int32_t>::Minimum();
		hi = NumericLimits<int32_t>::Maximum();
		return true;
	case LogicalTypeId::DATE:
		// exclude the ±infinity sentinels so bisection stays inside the finite, totally-ordered range
		lo = int64_t(date_t::ninfinity().days) + 1;
		hi = int64_t(date_t::infinity().days) - 1;
		has_infinity = true;
		return true;
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ:
		// every non-sentinel int64 is a valid, comparable timestamp (make_timestamp accepts all of them). Search
		// between the ±infinity sentinels so that hi+1 and lo-1 stay representable. Apply handles the finite
		// INT64_MIN below -infinity.
		lo = timestamp_t::ninfinity().value + 1;
		hi = timestamp_t::infinity().value - 1;
		has_infinity = true;
		return true;
	default:
		// BIGINT's full int64 domain would overflow the never-true bisection sentinels, other timestamp
		// resolutions and HUGEINT need a wider midpoint, and floating types are only monotone up to
		// rounding: all left to a later extension.
		return false;
	}
}

//! Evaluate f at the probe value with internal representation `i`, returning false if the result is
//! NULL or the evaluation fails (e.g. a domain error) - in which case the rewrite must bail.
static bool EvalFunc(ClientContext &context, const BoundFunctionExpression &func, const LogicalType &col_type,
                     int64_t i, Value &out) {
	auto clone = func.Copy();
	auto &clone_func = clone->Cast<BoundFunctionExpression>();
	clone_func.GetChildrenMutable()[0] = make_uniq<BoundConstantExpression>(Value::Numeric(col_type, i));
	return ExpressionExecutor::TryEvaluateScalar(context, clone_func, out) && !out.IsNull();
}

//! Midpoint of [l, h] (l <= h) that never overflows, even on the full int64 domain where l + h or
//! h - l would.
static int64_t SafeMidpoint(int64_t l, int64_t h) {
	return l + static_cast<int64_t>((static_cast<uint64_t>(h) - static_cast<uint64_t>(l)) / 2);
}

//! First index in [lo, hi] where `pred` becomes true, assuming pred is false on a prefix and true on
//! a suffix. Returns hi+1 if never true. Sets ok=false if any probe cannot be evaluated.
template <class PRED>
static int64_t FirstTrueSuffix(PRED pred, int64_t lo, int64_t hi, bool &ok) {
	bool v;
	if (!pred(hi, v, ok) || !ok) {
		return 0;
	}
	if (!v) {
		return hi + 1;
	}
	if (!pred(lo, v, ok) || !ok) {
		return 0;
	}
	if (v) {
		return lo;
	}
	int64_t l = lo, h = hi; // pred(l) false, pred(h) true
	while (static_cast<uint64_t>(h) - static_cast<uint64_t>(l) > 1) {
		int64_t mid = SafeMidpoint(l, h);
		if (!pred(mid, v, ok) || !ok) {
			return 0;
		}
		if (v) {
			h = mid;
		} else {
			l = mid;
		}
	}
	return h;
}

//! Last index in [lo, hi] where `pred` is true, assuming pred is true on a prefix and false on a
//! suffix. Returns lo-1 if never true.
template <class PRED>
static int64_t LastTruePrefix(PRED pred, int64_t lo, int64_t hi, bool &ok) {
	bool v;
	if (!pred(lo, v, ok) || !ok) {
		return 0;
	}
	if (!v) {
		return lo - 1;
	}
	if (!pred(hi, v, ok) || !ok) {
		return 0;
	}
	if (v) {
		return hi;
	}
	int64_t l = lo, h = hi; // pred(l) true, pred(h) false
	while (static_cast<uint64_t>(h) - static_cast<uint64_t>(l) > 1) {
		int64_t mid = SafeMidpoint(l, h);
		if (!pred(mid, v, ok) || !ok) {
			return 0;
		}
		if (v) {
			l = mid;
		} else {
			h = mid;
		}
	}
	return l;
}

//! Emit `col >= bound` (lower) or `col <= bound` (upper). On types with ±infinity a bound on the edge of the
//! finite domain is written as a strict comparison against the adjacent sentinel instead (`col > -infinity`,
//! `col < infinity`), as nothing lies between the two.
static unique_ptr<Expression> MakeColBound(bool lower, const BoundColumnRefExpression &col, const LogicalType &col_type,
                                           int64_t bound, int64_t dom_edge, bool has_infinity) {
	if (has_infinity && bound == dom_edge) {
		auto sentinel = lower ? Value::NegativeInfinity(col_type) : Value::Infinity(col_type);
		return BoundComparisonExpression::Create(lower ? ExpressionType::COMPARE_GREATERTHAN
		                                               : ExpressionType::COMPARE_LESSTHAN,
		                                         col.Copy(), make_uniq<BoundConstantExpression>(std::move(sentinel)));
	}
	return BoundComparisonExpression::Create(
	    lower ? ExpressionType::COMPARE_GREATERTHANOREQUALTO : ExpressionType::COMPARE_LESSTHANOREQUALTO, col.Copy(),
	    make_uniq<BoundConstantExpression>(Value::Numeric(col_type, bound)));
}

unique_ptr<Expression> MonotonePreimageRule::Apply(LogicalOperator &op, vector<reference<Expression>> &bindings,
                                                   bool &changes_made, bool is_root) {
	auto &comparison = bindings[0].get().Cast<BoundFunctionExpression>();
	auto &constant_expr = bindings[1].get();
	auto &func = bindings[2].get().Cast<BoundFunctionExpression>();

	// the matched function must be a monotone, deterministic unary function of a bare column
	if (func.GetChildren().size() != 1 || !func.Function().HasArgProperties() ||
	    func.Function().GetStability() == FunctionStability::VOLATILE) {
		return nullptr;
	}
	auto m = func.Function().GetArgProperties(0).monotonicity;
	if (!IsKnownMonotonic(m) || m == Monotonicity::CONSTANT) {
		return nullptr;
	}
	if (func.GetChildren()[0]->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		return nullptr;
	}
	auto &col = func.GetChildren()[0]->Cast<BoundColumnRefExpression>();
	auto &col_type = col.GetReturnType();

	int64_t dom_lo, dom_hi;
	bool has_infinity;
	if (!GetFiniteDomain(col_type, dom_lo, dom_hi, has_infinity)) {
		return nullptr;
	}
	// Infinity sentinels can make date/time functions return NULL for non-NULL inputs.
	if (has_infinity && (!is_root || op.type != LogicalOperatorType::LOGICAL_FILTER)) {
		return nullptr;
	}

	// the bisection probes evaluate the function itself, which must not observe its binding environment
	if (Settings::Get<PortablePlanSetting>(GetContext()) && func.ObservesBindingContext()) {
		return nullptr;
	}
	// evaluate the constant and cast it to the function's return type for the corner comparisons
	if (!ExpressionRewriter::CanConstantFold(GetContext(), constant_expr)) {
		return nullptr;
	}
	Value c;
	if (!ExpressionExecutor::TryEvaluateScalar(GetContext(), constant_expr, c) || c.IsNull()) {
		return nullptr;
	}
	if (c.type() != func.GetReturnType()) {
		auto cast = c.DefaultTryCastAs(func.GetReturnType());
		if (!cast) {
			return nullptr;
		}
		c = std::move(*cast);
	}

	// normalize the comparison so the function is on the left: f(col) OP c
	const bool func_is_left = RefersToSameObject(*comparison.GetChildren()[0], func);
	auto cmp = comparison.GetExpressionType();
	if (!func_is_left) {
		cmp = FlipComparisonExpression(cmp);
	}
	switch (cmp) {
	case ExpressionType::COMPARE_EQUAL:
	case ExpressionType::COMPARE_LESSTHAN:
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
	case ExpressionType::COMPARE_GREATERTHAN:
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		break;
	default:
		return nullptr;
	}

	auto &context = GetContext();
	bool ok = true;
	auto ge = [&](int64_t i, bool &v, bool &okref) {
		Value fv;
		if (!EvalFunc(context, func, col_type, i, fv)) {
			okref = false;
			return false;
		}
		v = ValueOperations::GreaterThanEquals(fv, c);
		return true;
	};
	auto gt = [&](int64_t i, bool &v, bool &okref) {
		Value fv;
		if (!EvalFunc(context, func, col_type, i, fv)) {
			okref = false;
			return false;
		}
		v = ValueOperations::GreaterThan(fv, c);
		return true;
	};

	// only search for the boundary/boundaries this operator actually uses: >= and < need the f>=c
	// crossover, > and <= need the f>c crossover, and = needs both.
	const bool need_ge = cmp == ExpressionType::COMPARE_GREATERTHANOREQUALTO ||
	                     cmp == ExpressionType::COMPARE_LESSTHAN || cmp == ExpressionType::COMPARE_EQUAL;
	const bool need_gt = cmp == ExpressionType::COMPARE_GREATERTHAN ||
	                     cmp == ExpressionType::COMPARE_LESSTHANOREQUALTO || cmp == ExpressionType::COMPARE_EQUAL;

	// [a, b] is the inclusive internal-integer preimage interval (clamped to the finite domain)
	int64_t a, b;
	if (IsMonotonicIncreasing(m)) {
		int64_t x_ge = need_ge ? FirstTrueSuffix(ge, dom_lo, dom_hi, ok) : 0;
		int64_t x_gt = need_gt ? FirstTrueSuffix(gt, dom_lo, dom_hi, ok) : 0;
		if (!ok) {
			return nullptr;
		}
		switch (cmp) {
		case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
			a = x_ge;
			b = dom_hi;
			break;
		case ExpressionType::COMPARE_GREATERTHAN:
			a = x_gt;
			b = dom_hi;
			break;
		case ExpressionType::COMPARE_LESSTHAN:
			a = dom_lo;
			b = x_ge - 1;
			break;
		case ExpressionType::COMPARE_LESSTHANOREQUALTO:
			a = dom_lo;
			b = x_gt - 1;
			break;
		default: // EQUAL
			a = x_ge;
			b = x_gt - 1;
			break;
		}
	} else {
		int64_t y_ge = need_ge ? LastTruePrefix(ge, dom_lo, dom_hi, ok) : 0;
		int64_t y_gt = need_gt ? LastTruePrefix(gt, dom_lo, dom_hi, ok) : 0;
		if (!ok) {
			return nullptr;
		}
		switch (cmp) {
		case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
			a = dom_lo;
			b = y_ge;
			break;
		case ExpressionType::COMPARE_GREATERTHAN:
			a = dom_lo;
			b = y_gt;
			break;
		case ExpressionType::COMPARE_LESSTHAN:
			a = y_ge + 1;
			b = dom_hi;
			break;
		case ExpressionType::COMPARE_LESSTHANOREQUALTO:
			a = y_gt + 1;
			b = dom_hi;
			break;
		default: // EQUAL
			a = y_gt + 1;
			b = y_ge;
			break;
		}
	}

	// clamp to the finite domain
	a = MaxValue<int64_t>(a, dom_lo);
	b = MinValue<int64_t>(b, dom_hi);

	if (a > b) {
		// empty preimage. On types with ±infinity, f(±inf) is NULL (not false), so returning a plain
		// constant would mishandle those rows - bail instead.
		if (has_infinity) {
			return nullptr;
		}
		return ExpressionRewriter::ConstantOrNull(GetContext(), col.Copy(), Value::BOOLEAN(false));
	}

	// emit finite-domain bounds. On infinity types both bounds are always kept so ±infinity (whose f is
	// NULL) is excluded by construction; on infinity-free types a trivial bound is dropped.
	const bool need_lower = a > dom_lo || has_infinity;
	const bool need_upper = b < dom_hi || has_infinity;
	if (!need_lower && !need_upper) {
		return nullptr; // no narrowing
	}
	// include the finite value below -infinity when it satisfies the comparison
	bool below_sentinel_matches = false;
	if (has_infinity && a == dom_lo) {
		Value below_sentinel;
		if (!EvalFunc(context, func, col_type, dom_lo - 2, below_sentinel)) {
			return nullptr;
		}
		auto check = BoundComparisonExpression::Create(cmp, make_uniq<BoundConstantExpression>(below_sentinel),
		                                               make_uniq<BoundConstantExpression>(c));
		Value matches;
		if (!ExpressionExecutor::TryEvaluateScalar(context, *check, matches) || matches.IsNull()) {
			return nullptr;
		}
		below_sentinel_matches = BooleanValue::Get(matches);
	}
	unique_ptr<Expression> lower;
	unique_ptr<Expression> upper;
	if (need_lower && below_sentinel_matches) {
		lower =
		    BoundComparisonExpression::Create(ExpressionType::COMPARE_NOTEQUAL, col.Copy(),
		                                      make_uniq<BoundConstantExpression>(Value::NegativeInfinity(col_type)));
	} else if (need_lower) {
		lower = MakeColBound(true, col, col_type, a, dom_lo, has_infinity);
	}
	if (need_upper) {
		upper = MakeColBound(false, col, col_type, b, dom_hi, has_infinity);
	}
	if (lower && upper) {
		return make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(lower),
		                                             std::move(upper));
	}
	return lower ? std::move(lower) : std::move(upper);
}

} // namespace duckdb
