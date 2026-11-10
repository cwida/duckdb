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
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
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

class AggregateRewriteRule {
public:
	explicit AggregateRewriteRule(Optimizer &optimizer_p) : optimizer(optimizer_p) {
	}
	virtual ~AggregateRewriteRule() = default;

public:
	virtual bool ShouldSkip(const LogicalAggregate &aggr) const = 0;
	virtual bool ShouldRewrite(const LogicalAggregate &aggr, Expression &expr) {
		return true;
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

//! Internal LogicalOperatorVisitor that does the actual rewriting
class AggregateFunctionRewriterInternal : public LogicalOperatorVisitor {
public:
	AggregateFunctionRewriterInternal(Optimizer &optimizer, AggregateRewriteRule &rule)
	    : optimizer(optimizer), rule(rule) {
	}

private:
	struct RewriteInfo {
		idx_t count_idx;
		vector<unique_ptr<Expression>> additional_expressions;
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

	void RewriteAggregates(unique_ptr<LogicalOperator> &op) {
		auto &aggr = op->Cast<LogicalAggregate>();
		if (rule.ShouldSkip(aggr)) {
			return;
		}
		const idx_t aggr_count = aggr.expressions.size();

		unordered_map<idx_t, RewriteInfo> rewrites;
		for (idx_t i = 0; i < aggr_count; ++i) {
			auto &expr = aggr.expressions[i];
			vector<reference<Expression>> bindings;
			if (!rule.matcher->Match(*expr, bindings)) {
				continue;
			}
			if (!rule.ShouldRewrite(aggr, *expr)) {
				continue;
			}

			RewriteInfo rewrite_info;
			auto count_arg = rule.Rewrite(expr, bindings, rewrite_info.additional_expressions);

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

			rewrite_info.count_idx = aggr.expressions.size();
			rewrites.emplace(i, std::move(rewrite_info));
			aggr.expressions.push_back(std::move(count_aggr));
		}

		if (rewrites.empty()) {
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
			if (rewrite_entry == rewrites.end()) {
				// Not rewritten — pass through as-is
				projection_expressions.push_back(std::move(aggr_ref));
				continue;
			}

			auto &rewrite_info = rewrite_entry->second;
			ColumnBinding count_binding(aggr.aggregate_index, ProjectionIndex(rewrite_info.count_idx));
			auto count_ref = make_uniq<BoundColumnRefExpression>(
			    aggr.expressions[rewrite_info.count_idx]->GetReturnType(), count_binding);

			rewrite_info.additional_expressions.push_back(std::move(count_ref));
			auto final_result = rule.CreateProjectionExpression(aggr_type, std::move(aggr_ref),
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

} // namespace duckdb
