#include "duckdb/optimizer/aggregate_function_rewriter.hpp"

#include "duckdb/function/aggregate/distributive_function_utils.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/function/aggregate/distributive_functions.hpp"
#include "duckdb/optimizer/matcher/expression_matcher.hpp"
#include "duckdb/optimizer/aggregate_rewrite.hpp"
#include "duckdb/function/builtin_function_lookup.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression_nullability.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"

namespace duckdb {

static void RewriteAggregateCallbacks(Optimizer &optimizer, unique_ptr<LogicalOperator> &op) {
	if (op->type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
		auto &aggregate_op = op->Cast<LogicalAggregate>();
		for (auto &expr : aggregate_op.expressions) {
			auto &aggregate = expr->Cast<BoundAggregateExpression>();
			AggregateRewriteInput input(optimizer, aggregate_op, aggregate);
			auto rewrite = TryDirectAggregateRewrite(input);
			if (!rewrite) {
				continue;
			}
			expr = std::move(rewrite);
		}
	}
	for (auto &child : op->children) {
		RewriteAggregateCallbacks(optimizer, child);
	}
}

//! The helper aggregates a rule needs next to a rewritten aggregate
struct AggregateRewriteHelpers {
	//! Whether COUNT([x]) is appended: its argument is what Rewrite returns, its reference is the last additional
	//! expression of the projection
	bool count = true;
	//! Further helper aggregates: their references follow the additional expressions of the rule, before the COUNT
	vector<unique_ptr<Expression>> extras;
	//! Whether a helper that equals an aggregate of the operator re-uses that aggregate instead of being appended
	bool dedup = false;
	//! Whether the rewritten aggregate is the result: no projection expression is created for it
	bool in_place = false;
};

class AggregateRewriteRule {
public:
	explicit AggregateRewriteRule(Optimizer &optimizer_p) : optimizer(optimizer_p) {
	}
	virtual ~AggregateRewriteRule() = default;

public:
	virtual bool ShouldSkip(const LogicalAggregate &aggr) const = 0;
	virtual bool Match(Expression &expr, vector<reference<Expression>> &bindings) {
		return matcher->Match(expr, bindings);
	}
	virtual bool ShouldRewrite(const LogicalAggregate &aggr, Expression &expr) {
		return true;
	}
	virtual void GetHelpers(const LogicalAggregate &aggr, Expression &expr, AggregateRewriteHelpers &helpers) {
	}
	virtual unique_ptr<Expression> Rewrite(unique_ptr<Expression> &expr, vector<reference<Expression>> &bindings,
	                                       vector<unique_ptr<Expression>> &additional_expressions) = 0;
	virtual unique_ptr<Expression>
	CreateProjectionExpression(const LogicalType &aggr_type, unique_ptr<Expression> aggr_ref,
	                           vector<unique_ptr<Expression>> additional_expressions) = 0;

public:
	Optimizer &optimizer;
	unique_ptr<ExpressionMatcher> matcher;
};

static unique_ptr<SetTypesMatcher> GetSmallIntegerTypesMatcher() {
	vector<LogicalType> types {LogicalTypeId::TINYINT,  LogicalTypeId::SMALLINT, LogicalTypeId::INTEGER,
	                           LogicalTypeId::BIGINT,   LogicalTypeId::UTINYINT, LogicalTypeId::USMALLINT,
	                           LogicalTypeId::UINTEGER, LogicalTypeId::UBIGINT};
	return make_uniq<SetTypesMatcher>(std::move(types));
}

//! AVG(x) -> SUM(x) / COUNT(x)
class AvgRewriteRule : public AggregateRewriteRule {
public:
	AvgRewriteRule(Optimizer &optimizer, LogicalOperator &root)
	    : AggregateRewriteRule(optimizer), nullability(optimizer.context, root) {
		auto op = make_uniq<AggregateExpressionMatcher>();
		op->function = make_uniq<SpecificFunctionMatcher>("avg");
		op->type = make_uniq<NumericTypeMatcher>();
		op->policy = SetMatcher::Policy::ORDERED;
		auto child_matcher = make_uniq<StableExpressionMatcher>();
		child_matcher->type = make_uniq<NumericTypeMatcher>();
		op->matchers.push_back(std::move(child_matcher));
		matcher = std::move(op);

		auto count = make_uniq<AggregateExpressionMatcher>();
		count->function = make_uniq<ManyFunctionMatcher>(identifier_set_t {"count", "count_star"});
		count->policy = SetMatcher::Policy::SOME;
		count_matcher = std::move(count);
	}

public:
	bool ShouldSkip(const LogicalAggregate &aggr) const override {
		return !aggr.grouping_functions.empty();
	}

	bool ShouldRewrite(const LogicalAggregate &aggr, Expression &expr) override {
		if (aggr.groups.empty()) {
			return true;
		}
		if (optimizer.OptimizerDisabled(OptimizerType::COMMON_AGGREGATE)) {
			return false;
		}
		auto &child = *expr.Cast<BoundAggregateExpression>().GetChildren()[0];
		FunctionBinder binder(optimizer.context);
		vector<unique_ptr<Expression>> sum_args;
		sum_args.push_back(child.Copy());
		auto sum = binder.BindAggregateFunction(
		    GetBuiltinAggregateFunction(optimizer.context, "sum", {child.GetReturnType()}), std::move(sum_args));
		vector<unique_ptr<Expression>> count_args;
		count_args.push_back(child.Copy());
		auto count = binder.BindAggregateFunction(
		    GetBuiltinAggregateFunction(optimizer.context, "count", {child.GetReturnType()}), std::move(count_args));

		D_ASSERT(aggr.children.size() == 1);
		auto is_not_null = [&](const Expression &input) {
			if (input.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
				return !input.Cast<BoundConstantExpression>().GetValue().IsNull();
			}
			return nullability.IsNotNull(*aggr.children[0], input);
		};
		const auto can_share_count =
		    !optimizer.OptimizerDisabled(OptimizerType::STATISTICS_PROPAGATION) && is_not_null(child);
		for (auto &other : aggr.expressions) {
			// Identical AVGs already share their native state.
			if (expr.Equals(*other)) {
				continue;
			}
			if (sum->Equals(*other) || count->Equals(*other)) {
				return true;
			}
			if (!can_share_count || other->IsVolatile()) {
				continue;
			}
			vector<reference<Expression>> bindings;
			if (!count_matcher->Match(*other, bindings) && !matcher->Match(*other, bindings)) {
				continue;
			}
			auto &children = other->Cast<BoundAggregateExpression>().GetChildren();
			if (children.empty() || (children.size() == 1 && is_not_null(*children[0]))) {
				return true;
			}
		}
		return false;
	}

	unique_ptr<Expression> Rewrite(unique_ptr<Expression> &expr, vector<reference<Expression>> &bindings,
	                               vector<unique_ptr<Expression>> &additional_expressions) override {
		FunctionBinder function_binder(optimizer.context);

		// Move the child out of AVG(x)
		auto avg_child = std::move(bindings[0].get().Cast<BoundAggregateExpression>().GetChildrenMutable()[0]);

		// Replace AVG(x) with SUM(x)
		auto sum_fun = GetBuiltinAggregateFunction(optimizer.context, "sum", {avg_child->GetReturnType()});
		vector<unique_ptr<Expression>> args;
		args.push_back(std::move(avg_child));
		auto count_arg = args.back()->Copy();
		expr = function_binder.BindAggregateFunction(sum_fun, std::move(args));

		return count_arg;
	}

	unique_ptr<Expression> CreateProjectionExpression(const LogicalType &, unique_ptr<Expression> aggr_ref,
	                                                  vector<unique_ptr<Expression>> additional_expressions) override {
		// SUM(x) / COUNT(x): additional_expressions[0] is the COUNT(x) ref
		return optimizer.BindScalarFunction("/", std::move(aggr_ref), std::move(additional_expressions[0]));
	}

private:
	NotNullExpressionAnalyzer nullability;
	unique_ptr<ExpressionMatcher> count_matcher;
};

//! SUM(x + C) -> SUM(x) + C * COUNT(x)
class SumRewriteRule : public AggregateRewriteRule {
public:
	explicit SumRewriteRule(Optimizer &optimizer) : AggregateRewriteRule(optimizer) {
		// Match SUM(x + C) or SUM(C + x) on integer types
		auto op = make_uniq<AggregateExpressionMatcher>();
		op->function = make_uniq<SpecificFunctionMatcher>("sum");
		op->policy = SetMatcher::Policy::UNORDERED;

		auto arithmetic = make_uniq<FunctionExpressionMatcher>();
		arithmetic->function = make_uniq<SpecificFunctionMatcher>("+");
		arithmetic->type = make_uniq<IntegerTypeMatcher>();
		auto child_constant_matcher = make_uniq<ConstantExpressionMatcher>();
		auto child_expression_matcher = make_uniq<StableExpressionMatcher>();
		child_constant_matcher->type = GetSmallIntegerTypesMatcher();
		child_expression_matcher->type = GetSmallIntegerTypesMatcher();
		arithmetic->matchers.push_back(std::move(child_constant_matcher));
		arithmetic->matchers.push_back(std::move(child_expression_matcher));
		arithmetic->policy = SetMatcher::Policy::SOME;
		op->matchers.push_back(std::move(arithmetic));

		matcher = std::move(op);
	}

public:
	bool ShouldSkip(const LogicalAggregate &aggr) const override {
		// This can probably be relaxed to be the same check as AvgRewriteRule,
		// but behaviour (for now) is the same as the old SumRewriterOptimizer
		return !aggr.groups.empty();
	}

	unique_ptr<Expression> Rewrite(unique_ptr<Expression> &, vector<reference<Expression>> &bindings,
	                               vector<unique_ptr<Expression>> &additional_expressions) override {
		auto &sum = bindings[0].get().Cast<BoundAggregateExpression>();
		auto &addition = bindings[1].get().Cast<BoundFunctionExpression>();
		idx_t const_idx = addition.GetChildren()[0]->GetExpressionType() == ExpressionType::VALUE_CONSTANT ? 0 : 1;
		auto const_expr = std::move(addition.GetChildrenMutable()[const_idx]);
		auto main_expr = std::move(addition.GetChildrenMutable()[1 - const_idx]);

		// Turn SUM(x + C) into SUM(x)
		sum.GetChildrenMutable()[0] = main_expr->Copy();

		additional_expressions.push_back(std::move(const_expr));
		return main_expr;
	}

	unique_ptr<Expression> CreateProjectionExpression(const LogicalType &aggr_type, unique_ptr<Expression> aggr_ref,
	                                                  vector<unique_ptr<Expression>> additional_expressions) override {
		// SUM(x) + C * COUNT(x): additional_expressions[0] is the const, additional_expressions[1] is COUNT(x) ref
		auto cast_const =
		    BoundCastExpression::AddCastToType(optimizer.context, std::move(additional_expressions[0]), aggr_type);
		auto cast_count =
		    BoundCastExpression::AddCastToType(optimizer.context, std::move(additional_expressions[1]), aggr_type);
		auto multiply = optimizer.BindScalarFunction("*", std::move(cast_count), std::move(cast_const));
		return optimizer.BindScalarFunction("+", std::move(aggr_ref), std::move(multiply));
	}
};

//! list(expr ORDER BY expr <sense> <nulls>) => list_sort(list(expr), <sense>, <nulls>)
class BindingMatcher : public ExpressionMatcher {
public:
	explicit BindingMatcher(idx_t binding_idx) : ExpressionMatcher(ExpressionClass::INVALID), binding_idx(binding_idx) {
	}

	bool Match(Expression &expr, vector<reference<Expression>> &bindings) override {
		if (!expr.Equals(bindings[binding_idx])) {
			return false;
		}
		bindings.push_back(expr);
		return true;
	}

	const idx_t binding_idx;
};

class OrderedAggregateMatcher : public ExpressionMatcher {
public:
	OrderedAggregateMatcher() : ExpressionMatcher(ExpressionClass::BOUND_AGGREGATE) {
	}

	bool Match(Expression &expr_p, vector<reference<Expression>> &bindings) override {
		if (!ExpressionMatcher::Match(expr_p, bindings)) {
			return false;
		}
		auto &expr = expr_p.Cast<BoundAggregateExpression>();
		// don't rewrite state-export aggregates - list(x ORDER BY x) EXPORT_STATE would become
		// list_sort(list(x) EXPORT_STATE, ...), which cannot bind list_sort on the AGGREGATE_STATE result
		if (expr.StateExportMode() == AggregateStateExportMode::STATE_EXPORT) {
			return false;
		}
		if (!FunctionMatcher::Match(function, expr.Function().GetName())) {
			return false;
		}
		if (!SetMatcher::Match(matchers, expr.GetChildrenMutable(), bindings, policy)) {
			return false;
		}
		if (!expr.GetOrderBys() && !order_bys.empty()) {
			return false;
		}
		if (order_bys.size() != expr.GetOrderBys()->orders.size()) {
			return false;
		}
		for (idx_t i = 0; i < order_bys.size(); ++i) {
			if (!order_bys[i]->Match(*expr.GetOrderBys()->orders[i].expression, bindings)) {
				return false;
			}
		}
		return true;
	}

	//! The matchers for the child expressions
	vector<unique_ptr<ExpressionMatcher>> matchers;
	//! The function name to match
	unique_ptr<FunctionMatcher> function;
	//! The set matcher matching policy to use
	SetMatcher::Policy policy;
	//! The matchers for the order by expressions
	vector<unique_ptr<ExpressionMatcher>> order_bys;
};

class ListRewriteRule : public AggregateRewriteRule {
public:
	explicit ListRewriteRule(Optimizer &optimizer) : AggregateRewriteRule(optimizer) {
		auto op = make_uniq<OrderedAggregateMatcher>();
		op->function = make_uniq<SpecificFunctionMatcher>("list");
		op->policy = SetMatcher::Policy::ORDERED;
		op->matchers.push_back(make_uniq<StableExpressionMatcher>());
		op->order_bys.emplace_back(make_uniq<BindingMatcher>(idx_t(1)));

		matcher = std::move(op);
	}

	bool ShouldSkip(const LogicalAggregate &aggr) const override {
		return false;
	}

	unique_ptr<Expression> Rewrite(unique_ptr<Expression> &expr, vector<reference<Expression>> &bindings,
	                               vector<unique_ptr<Expression>> &additional_expressions) override;

	unique_ptr<Expression> CreateProjectionExpression(const LogicalType &, unique_ptr<Expression> aggr_ref,
	                                                  vector<unique_ptr<Expression>> addnl) override {
		return optimizer.BindScalarFunction("list_sort", std::move(aggr_ref), std::move(addnl[0]), std::move(addnl[1]));
	}
};

unique_ptr<Expression> ListRewriteRule::Rewrite(unique_ptr<Expression> &expr, vector<reference<Expression>> &bindings,
                                                vector<unique_ptr<Expression>> &additional_expressions) {
	auto &aggr = bindings[0].get().Cast<BoundAggregateExpression>();

	auto &order_bys = aggr.GetOrderBysMutable();
	auto &order_by = order_bys->orders[0];

	auto sense = make_uniq<BoundConstantExpression>(EnumUtil::ToChars(order_by.type));
	auto nulls = make_uniq<BoundConstantExpression>(EnumUtil::ToChars(order_by.null_order));

	additional_expressions.emplace_back(std::move(sense));
	additional_expressions.emplace_back(std::move(nulls));

	order_bys.reset();

	return nullptr;
}

//! f(CASE WHEN c THEN x END) -> f(x) FILTER (WHERE c), so that the state is only updated for the matching rows and
//! aggregates that share a condition evaluate it once per chunk. The rewritten forms:
//!
//! f(CASE WHEN c THEN x END) -> f(x) FILTER (WHERE c)
//!     for sum / count / avg / min / max / bool_and / bool_or: both are NULL (0 for count) without a matching row
//! count(CASE WHEN c THEN k END) -> count(*) FILTER (WHERE c)
//!     for a non-NULL constant k
//! sum(CASE WHEN c THEN x ELSE 0 END) -> coalesce(sum(x) FILTER (WHERE c), 0)
//!     if x is provably not NULL
//! sum(CASE WHEN c THEN x ELSE 0 END) -> CASE WHEN count(*) > count(*) FILTER (WHERE c)
//!                                            THEN coalesce(sum(x) FILTER (WHERE c), 0)
//!                                            ELSE sum(x) FILTER (WHERE c) END
//!     if x is nullable: a group in which every row matches and every x is NULL is NULL (a sum over NULLs), any other
//!     group without a non-NULL matching x holds a row that contributed the ELSE 0
//! sum(CASE WHEN c THEN k ELSE 0 END) -> count(*) FILTER (WHERE c) * k
//!     for an integer constant k
//!
//! Where the aggregate returns a row for an empty input (no groups, or a grouping set without columns), the ELSE 0
//! forms are wrapped in CASE WHEN count(*) > 0 THEN ... END: the sum over no rows is NULL.
//!
//! The CASE only evaluates x for the matching rows, whereas the FILTER form evaluates it for every row. Hence x must
//! not be able to throw and must not be volatile.
class CaseToFilterRule : public AggregateRewriteRule {
public:
	CaseToFilterRule(Optimizer &optimizer, LogicalOperator &root, bool use_statistics_p)
	    : AggregateRewriteRule(optimizer), nullability(optimizer.context, root),
	      use_statistics(use_statistics_p && !optimizer.OptimizerDisabled(OptimizerType::STATISTICS_PROPAGATION)) {
	}

private:
	enum class CaseShape {
		//! Not a rewritable aggregate
		INVALID,
		//! f(CASE WHEN c THEN x END)
		ELSE_NULL,
		//! sum(CASE WHEN c THEN x ELSE 0 END)
		ELSE_ZERO,
		//! sum(CASE WHEN c THEN k ELSE 0 END) for an integer constant k
		CONSTANT_THEN
	};

private:
	static const BoundCaseExpression &GetCase(const BoundAggregateExpression &aggr) {
		return aggr.GetChildren()[0]->Cast<BoundCaseExpression>();
	}

	static bool IsRewritableFunction(const string &name) {
		static constexpr const char *FUNCTIONS[] = {"sum", "count", "avg", "min", "max", "bool_and", "bool_or"};
		for (auto &function_name : FUNCTIONS) {
			if (name == function_name) {
				return true;
			}
		}
		return false;
	}

	static bool IsNonNullConstant(const Expression &expr) {
		return expr.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT &&
		       !expr.Cast<BoundConstantExpression>().GetValue().IsNull();
	}

	static bool IsZero(const Value &value) {
		if (value.IsNull() || !value.type().IsNumeric()) {
			return false;
		}
		return value == Value::Numeric(value.type(), 0);
	}

	static CaseShape Classify(const Expression &expr) {
		if (expr.GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
			return CaseShape::INVALID;
		}
		auto &aggr = expr.Cast<BoundAggregateExpression>();
		if (aggr.StateExportMode() == AggregateStateExportMode::STATE_EXPORT || aggr.GetOrderBys() ||
		    aggr.GetFilter() || aggr.IsDistinct() || aggr.GetChildren().size() != 1) {
			return CaseShape::INVALID;
		}
		auto &name = aggr.Function().GetName().GetIdentifierName();
		if (!IsRewritableFunction(name)) {
			return CaseShape::INVALID;
		}
		auto &child = *aggr.GetChildren()[0];
		if (child.GetExpressionClass() != ExpressionClass::BOUND_CASE) {
			return CaseShape::INVALID;
		}
		auto &case_expr = child.Cast<BoundCaseExpression>();
		if (case_expr.CaseChecks().size() != 1) {
			return CaseShape::INVALID;
		}
		auto &when_expr = *case_expr.CaseChecks()[0].when_expr;
		auto &then_expr = *case_expr.CaseChecks()[0].then_expr;
		auto &else_expr = case_expr.Else();
		if (when_expr.IsVolatile() || !when_expr.IsConsistent() || when_expr.HasSubquery() || when_expr.IsAggregate() ||
		    when_expr.IsWindow()) {
			return CaseShape::INVALID;
		}
		// the CASE evaluates the THEN branch for the matching rows only, the FILTER form for every row
		if (then_expr.CanThrow() || then_expr.IsVolatile() || then_expr.HasSubquery()) {
			return CaseShape::INVALID;
		}
		// min / max over strings or nested types update their state faster from the vector of the CASE than from a
		// slice of the argument
		if ((name == "min" || name == "max") && !TypeIsConstantSize(then_expr.GetReturnType().InternalType())) {
			return CaseShape::INVALID;
		}
		const auto is_sum = name == "sum";
		// sum and avg add value * count for a constant input vector instead of adding the value count times, which
		// is another floating point number. A constant THEN branch is such a vector in every chunk: the filtered sum
		// would never be the sum of the CASE form, so that it keeps its CASE. A column is not covered by this: it
		// only is a constant vector where the scan or a join hands one out (a constant or RLE segment, the one-row
		// side of a cross join), and there every sum over it adds the product - with a user-written FILTER, too
		if ((is_sum || name == "avg") && then_expr.GetReturnType().IsFloating() &&
		    then_expr.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
			return CaseShape::INVALID;
		}
		// the binder casts the branches to the type of the CASE, so that f(x) binds like f(CASE ...)
		if (then_expr.GetReturnType() != child.GetReturnType()) {
			return CaseShape::INVALID;
		}
		if (else_expr.GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
			return CaseShape::INVALID;
		}
		auto &else_value = else_expr.Cast<BoundConstantExpression>().GetValue();
		if (else_value.IsNull()) {
			return CaseShape::ELSE_NULL;
		}
		if (!is_sum || !IsZero(else_value)) {
			return CaseShape::INVALID;
		}
		// k * count(*) is only the exact sum for an integer k
		const auto sum_type = aggr.GetReturnType().id();
		if (IsNonNullConstant(then_expr) && then_expr.GetReturnType().IsIntegral() &&
		    (sum_type == LogicalTypeId::HUGEINT || sum_type == LogicalTypeId::UHUGEINT)) {
			return CaseShape::CONSTANT_THEN;
		}
		return CaseShape::ELSE_ZERO;
	}

	//! Binds count(*) FILTER (WHERE filter)
	unique_ptr<Expression> BindFilteredCount(const Expression &filter) const {
		FunctionBinder function_binder(optimizer.context);
		auto count_fun = GetBuiltinAggregateFunction(optimizer.context, CountFun::Name, {});
		return function_binder.BindAggregateFunction(count_fun, {}, filter.Copy(), AggregateType::NON_DISTINCT);
	}

	//! Binds the filtered aggregate that replaces f(CASE WHEN c THEN x ...), or returns nullptr if it cannot replace it
	unique_ptr<Expression> BindFiltered(const BoundAggregateExpression &aggr, CaseShape shape) const {
		auto &check = GetCase(aggr).CaseChecks()[0];
		auto &name = aggr.Function().GetName().GetIdentifierName();
		const auto count_star =
		    shape == CaseShape::CONSTANT_THEN ||
		    (shape == CaseShape::ELSE_NULL && name == "count" && IsNonNullConstant(*check.then_expr));
		unique_ptr<Expression> result;
		if (count_star) {
			result = BindFilteredCount(*check.when_expr);
			if (result->GetReturnType() != LogicalType::BIGINT) {
				return nullptr;
			}
		} else {
			auto function = TryGetBuiltinAggregateFunction(optimizer.context, aggr.Function().GetName(),
			                                               {check.then_expr->GetReturnType()});
			if (!function) {
				return nullptr;
			}
			FunctionBinder function_binder(optimizer.context);
			vector<unique_ptr<Expression>> children;
			children.push_back(check.then_expr->Copy());
			result = function_binder.BindAggregateFunction(function, std::move(children), check.when_expr->Copy(),
			                                               AggregateType::NON_DISTINCT);
			if (result->GetReturnType() != aggr.GetReturnType()) {
				return nullptr;
			}
		}
		result->SetAlias(aggr.GetAlias());
		return result;
	}

	//! Whether the function returns a value whenever its arguments are values. The default NULL handling only states
	//! that a NULL argument gives NULL, not the converse (struct_extract of a NULL field, list_extract past the end)
	static bool PreservesNotNull(const BoundFunctionExpression &function) {
		if (function.Function().GetNullHandling() != FunctionNullHandling::DEFAULT_NULL_HANDLING) {
			return false;
		}
		auto &name = function.Function().GetName().GetIdentifierName();
		return function.IsOperator() && (name == "+" || name == "-" || name == "*");
	}

	//! Whether the expression is provably not NULL at the output of the operator
	bool IsNotNull(LogicalOperator &input, const Expression &expr) {
		switch (expr.GetExpressionClass()) {
		case ExpressionClass::BOUND_CONSTANT:
			return IsNonNullConstant(expr);
		case ExpressionClass::BOUND_COLUMN_REF:
			return use_statistics && nullability.IsNotNull(input, expr);
		case ExpressionClass::BOUND_FUNCTION: {
			auto &function = expr.Cast<BoundFunctionExpression>();
			if (BoundCastExpression::IsCast(expr)) {
				// a cast between numeric types returns a value or throws
				auto &source = BoundCastExpression::Child(function);
				return !BoundCastExpression::IsTryCast(function) && source.GetReturnType().IsNumeric() &&
				       expr.GetReturnType().IsNumeric() && IsNotNull(input, source);
			}
			if (!PreservesNotNull(function) || function.GetChildren().empty()) {
				return false;
			}
			for (auto &child : function.GetChildren()) {
				if (!IsNotNull(input, *child)) {
					return false;
				}
			}
			return true;
		}
		default:
			return false;
		}
	}

	//! The constant of the projection, typed as the sum: the 0 of coalesce(..., 0) or the factor k of count(*)
	static optional<Value> GetConstant(const BoundAggregateExpression &aggr, CaseShape shape) {
		if (shape == CaseShape::CONSTANT_THEN) {
			auto &then_expr = *GetCase(aggr).CaseChecks()[0].then_expr;
			return then_expr.Cast<BoundConstantExpression>().GetValue().DefaultTryCastAs(aggr.GetReturnType());
		}
		return Value::INTEGER(0).DefaultTryCastAs(aggr.GetReturnType());
	}

	//! Whether the aggregate returns a row for an empty input
	static bool HasEmptyGroupingSet(const LogicalAggregate &aggr) {
		if (aggr.groups.empty()) {
			return true;
		}
		for (auto &grouping_set : aggr.grouping_sets) {
			if (grouping_set.empty()) {
				return true;
			}
		}
		return false;
	}

public:
	bool ShouldSkip(const LogicalAggregate &) const override {
		return false;
	}

	bool Match(Expression &expr, vector<reference<Expression>> &bindings) override {
		if (Classify(expr) == CaseShape::INVALID) {
			return false;
		}
		bindings.push_back(expr);
		return true;
	}

	bool ShouldRewrite(const LogicalAggregate &, Expression &expr) override {
		auto &aggr = expr.Cast<BoundAggregateExpression>();
		const auto shape = Classify(expr);
		if (shape != CaseShape::ELSE_NULL && !GetConstant(aggr, shape)) {
			return false;
		}
		return BindFiltered(aggr, shape) != nullptr;
	}

	void GetHelpers(const LogicalAggregate &aggr, Expression &expr, AggregateRewriteHelpers &helpers) override {
		auto &aggregate = expr.Cast<BoundAggregateExpression>();
		helpers.count = false;
		helpers.dedup = true;
		switch (Classify(expr)) {
		case CaseShape::ELSE_NULL:
			// f(x) FILTER (WHERE c) is the result
			helpers.in_place = true;
			return;
		case CaseShape::ELSE_ZERO: {
			auto &check = GetCase(aggregate).CaseChecks()[0];
			if (!IsNotNull(*aggr.children[0], *check.then_expr)) {
				// count(*) > count(*) FILTER (WHERE c): a row of the group took the ELSE 0
				helpers.count = true;
				helpers.extras.push_back(BindFilteredCount(*check.when_expr));
				return;
			}
			break;
		}
		default:
			break;
		}
		// count(*) > 0: the aggregate has seen a row
		helpers.count = HasEmptyGroupingSet(aggr);
	}

	unique_ptr<Expression> Rewrite(unique_ptr<Expression> &expr, vector<reference<Expression>> &,
	                               vector<unique_ptr<Expression>> &additional_expressions) override {
		auto &aggr = expr->Cast<BoundAggregateExpression>();
		const auto shape = Classify(*expr);
		auto filtered = BindFiltered(aggr, shape);
		D_ASSERT(filtered);
		if (shape != CaseShape::ELSE_NULL) {
			auto constant = GetConstant(aggr, shape);
			if (!constant) {
				throw InternalException("CaseToFilterRule::Rewrite - ShouldRewrite accepted a constant that the "
				                        "type of the sum cannot hold");
			}
			additional_expressions.push_back(make_uniq<BoundConstantExpression>(std::move(*constant)));
		}
		expr = std::move(filtered);
		// the helper count (if any) is count(*)
		return nullptr;
	}

	unique_ptr<Expression> CreateProjectionExpression(const LogicalType &aggr_type, unique_ptr<Expression> aggr_ref,
	                                                  vector<unique_ptr<Expression>> additional_expressions) override {
		// additional_expressions: the constant, then count(*) FILTER (WHERE c) for the nullable ELSE 0 form, then
		// count(*) if the rewrite needs it
		D_ASSERT(!additional_expressions.empty());
		auto constant = std::move(additional_expressions[0]);
		unique_ptr<Expression> count_ref;
		unique_ptr<Expression> matching_ref;
		if (additional_expressions.size() > 1) {
			count_ref = std::move(additional_expressions.back());
		}
		if (additional_expressions.size() > 2) {
			matching_ref = std::move(additional_expressions[1]);
		}

		unique_ptr<Expression> result;
		if (aggr_ref->GetReturnType() == aggr_type) {
			// coalesce(sum(x) FILTER (WHERE c), 0)
			auto coalesce = make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_COALESCE, aggr_type);
			coalesce->GetChildrenMutable().push_back(aggr_ref->Copy());
			coalesce->GetChildrenMutable().push_back(std::move(constant));
			if (matching_ref) {
				// only a group with a row that did not match holds the ELSE 0: without one the sum is the result
				auto has_else_rows = BoundComparisonExpression::Create(ExpressionType::COMPARE_GREATERTHAN,
				                                                       std::move(count_ref), std::move(matching_ref));
				return make_uniq<BoundCaseExpression>(std::move(has_else_rows), std::move(coalesce),
				                                      std::move(aggr_ref));
			}
			result = std::move(coalesce);
		} else {
			// count(*) FILTER (WHERE c) * k
			result = BoundCastExpression::AddCastToType(optimizer.context, std::move(aggr_ref), aggr_type);
			if (constant->Cast<BoundConstantExpression>().GetValue() != Value::Numeric(aggr_type, 1)) {
				result = optimizer.BindScalarFunction("*", std::move(result), std::move(constant));
				result = BoundCastExpression::AddCastToType(optimizer.context, std::move(result), aggr_type);
			}
		}
		if (!count_ref) {
			return result;
		}
		// the sum over an empty input is NULL
		auto has_rows = BoundComparisonExpression::Create(ExpressionType::COMPARE_GREATERTHAN, std::move(count_ref),
		                                                  make_uniq<BoundConstantExpression>(Value::BIGINT(0)));
		return make_uniq<BoundCaseExpression>(std::move(has_rows), std::move(result),
		                                      make_uniq<BoundConstantExpression>(Value(aggr_type)));
	}

private:
	NotNullExpressionAnalyzer nullability;
	//! Whether a column may be proven NOT NULL: the proof reads the statistics of the scanned tables, which the
	//! optimizer does not use for a plan with a data-modifying CTE (the rows change while the query runs)
	const bool use_statistics;
};

//! Internal LogicalOperatorVisitor that does the actual rewriting
class AggregateFunctionRewriterInternal : public LogicalOperatorVisitor {
public:
	AggregateFunctionRewriterInternal(Optimizer &optimizer, AggregateRewriteRule &rule)
	    : optimizer(optimizer), rule(rule) {
	}

private:
	struct RewriteInfo {
		//! The indexes of the helper aggregates: the extra helpers of the rule, followed by the COUNT (if any)
		vector<idx_t> helper_indexes;
		vector<unique_ptr<Expression>> additional_expressions;
		//! The result type of the aggregate before the rewrite
		LogicalType original_type;
		bool in_place;
	};

public:
	void Optimize(unique_ptr<LogicalOperator> &op) {
		if (op->type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
			RewriteAggregates(op);
		}
		VisitOperator(*op);
	}

	void VisitOperator(LogicalOperator &op) override {
		switch (op.type) {
		case LogicalOperatorType::LOGICAL_UNION:
		case LogicalOperatorType::LOGICAL_EXCEPT:
		case LogicalOperatorType::LOGICAL_INTERSECT:
		case LogicalOperatorType::LOGICAL_MATERIALIZED_CTE:
		case LogicalOperatorType::LOGICAL_PROJECTION: {
			AggregateFunctionRewriterInternal rewriter(optimizer, rule);
			rewriter.StandardVisitOperator(op);
			return;
		}
		default:
			break;
		}
		StandardVisitOperator(op);
	}

private:
	void StandardVisitOperator(LogicalOperator &op) {
		for (auto &child : op.children) {
			Optimize(child);
		}
		if (!aggregate_map.empty()) {
			VisitOperatorExpressions(op);
		}
	}

	unique_ptr<Expression> VisitReplace(BoundColumnRefExpression &expr, unique_ptr<Expression> *) override {
		const auto entry = aggregate_map.find(expr.Binding());
		if (entry != aggregate_map.end()) {
			expr.BindingMutable() = entry->second;
		}
		return nullptr;
	}

	//! Appends a helper aggregate - or, with dedup, finds an equal aggregate - and returns its index
	static idx_t AddHelper(LogicalAggregate &aggr, unique_ptr<Expression> helper, bool dedup) {
		if (dedup) {
			for (idx_t i = 0; i < aggr.expressions.size(); i++) {
				if (aggr.expressions[i]->Equals(*helper)) {
					return i;
				}
			}
		}
		aggr.expressions.push_back(std::move(helper));
		return aggr.expressions.size() - 1;
	}

	void RewriteAggregates(unique_ptr<LogicalOperator> &op) {
		auto &aggr = op->Cast<LogicalAggregate>();
		if (rule.ShouldSkip(aggr)) {
			return;
		}
		const idx_t aggr_count = aggr.expressions.size();

		unordered_map<idx_t, RewriteInfo> rewrites;
		bool needs_projection = false;
		for (idx_t i = 0; i < aggr_count; ++i) {
			auto &expr = aggr.expressions[i];
			vector<reference<Expression>> bindings;
			if (!rule.Match(*expr, bindings)) {
				continue;
			}
			if (!rule.ShouldRewrite(aggr, *expr)) {
				continue;
			}
			AggregateRewriteHelpers helpers;
			rule.GetHelpers(aggr, *expr, helpers);

			RewriteInfo rewrite_info;
			rewrite_info.original_type = expr->GetReturnType();
			rewrite_info.in_place = helpers.in_place;
			auto count_arg = rule.Rewrite(expr, bindings, rewrite_info.additional_expressions);

			for (auto &helper : helpers.extras) {
				rewrite_info.helper_indexes.push_back(AddHelper(aggr, std::move(helper), helpers.dedup));
			}
			if (helpers.count) {
				// Add COUNT([x]) to the aggregate list - the count set holds both the unary count and count_star
				FunctionBinder function_binder(optimizer.context);
				vector<unique_ptr<Expression>> count_args;
				vector<LogicalType> count_arg_types;
				if (count_arg) {
					count_arg_types.push_back(count_arg->GetReturnType());
					count_args.push_back(std::move(count_arg));
				}
				auto count_fun = GetBuiltinAggregateFunction(optimizer.context, CountFun::Name, count_arg_types);
				auto count_aggr = function_binder.BindAggregateFunction(count_fun, std::move(count_args), nullptr,
				                                                        AggregateType::NON_DISTINCT);
				rewrite_info.helper_indexes.push_back(AddHelper(aggr, std::move(count_aggr), helpers.dedup));
			}
			needs_projection = needs_projection || !rewrite_info.in_place;
			rewrites.emplace(i, std::move(rewrite_info));
		}

		if (!needs_projection) {
			// nothing was rewritten, or every rewritten aggregate is the result as it is
			return;
		}

		// Push a projection that re-computes the original result expressions
		auto proj_index = optimizer.binder.GenerateTableIndex();
		const auto group_count = aggr.groups.size();
		vector<unique_ptr<Expression>> projection_expressions;

		// Pass group bindings through and register them in the aggregate_map
		for (idx_t group_idx = 0; group_idx < group_count; group_idx++) {
			ColumnBinding aggregate_binding(aggr.group_index, ProjectionIndex(group_idx));
			aggregate_map[aggregate_binding] = ColumnBinding(proj_index, ProjectionIndex(group_idx));
			auto group_ref =
			    make_uniq<BoundColumnRefExpression>(aggr.groups[group_idx]->GetReturnType(), aggregate_binding);
			projection_expressions.push_back(std::move(group_ref));
		}

		for (idx_t i = 0; i < aggr_count; i++) {
			ColumnBinding aggregate_binding(aggr.aggregate_index, ProjectionIndex(i));
			aggregate_map[aggregate_binding] = ColumnBinding(proj_index, ProjectionIndex(group_count + i));
			auto &aggr_type = aggr.expressions[i]->GetReturnType();
			auto aggr_ref = make_uniq<BoundColumnRefExpression>(aggr_type, aggregate_binding);

			const auto rewrite_entry = rewrites.find(i);
			if (rewrite_entry == rewrites.end() || rewrite_entry->second.in_place) {
				// Not rewritten, or rewritten in place — pass through as-is
				projection_expressions.push_back(std::move(aggr_ref));
				continue;
			}

			auto &rewrite_info = rewrite_entry->second;
			for (auto &helper_idx : rewrite_info.helper_indexes) {
				ColumnBinding helper_binding(aggr.aggregate_index, ProjectionIndex(helper_idx));
				rewrite_info.additional_expressions.push_back(
				    make_uniq<BoundColumnRefExpression>(aggr.expressions[helper_idx]->GetReturnType(), helper_binding));
			}
			auto final_result = rule.CreateProjectionExpression(rewrite_info.original_type, std::move(aggr_ref),
			                                                    std::move(rewrite_info.additional_expressions));
			projection_expressions.push_back(std::move(final_result));
		}

		// Pass grouping-function bindings through after the original aggregates. Any helper aggregates added by the
		// rewrite remain private to this projection.
		for (idx_t grouping_idx = 0; grouping_idx < aggr.grouping_functions.size(); grouping_idx++) {
			ColumnBinding grouping_binding(aggr.groupings_index, ProjectionIndex(grouping_idx));
			aggregate_map[grouping_binding] =
			    ColumnBinding(proj_index, ProjectionIndex(group_count + aggr_count + grouping_idx));
			projection_expressions.push_back(
			    make_uniq<BoundColumnRefExpression>(LogicalType::BIGINT, grouping_binding));
		}

		auto proj = make_uniq<LogicalProjection>(proj_index, std::move(projection_expressions));
		if (op->has_estimated_cardinality) {
			proj->SetEstimatedCardinality(op->estimated_cardinality);
		}
		proj->children.push_back(std::move(op));
		op = std::move(proj);
	}

private:
	Optimizer &optimizer;
	AggregateRewriteRule &rule;
	column_binding_map_t<ColumnBinding> aggregate_map;
};

AggregateFunctionRewriter::AggregateFunctionRewriter(Optimizer &optimizer) : optimizer(optimizer) {
}

AggregateFunctionRewriter::~AggregateFunctionRewriter() {
}

void AggregateFunctionRewriter::Optimize(unique_ptr<LogicalOperator> &op) {
	RewriteAggregateCallbacks(optimizer, op);
	vector<unique_ptr<AggregateRewriteRule>> rules;
	rules.push_back(make_uniq<AvgRewriteRule>(optimizer, *op));
	rules.push_back(make_uniq<SumRewriteRule>(optimizer));
	rules.push_back(make_uniq<ListRewriteRule>(optimizer));
	// Run each rule as an independent pass so that transformations by an earlier rule affect later rules
	for (auto &rule : rules) {
		AggregateFunctionRewriterInternal rewriter(optimizer, *rule);
		rewriter.Optimize(op);
	}
}

void AggregateFunctionRewriter::RewriteCaseToFilter(unique_ptr<LogicalOperator> &op, bool use_statistics) {
	CaseToFilterRule rule(optimizer, *op, use_statistics);
	AggregateFunctionRewriterInternal rewriter(optimizer, rule);
	rewriter.Optimize(op);
}

} // namespace duckdb
