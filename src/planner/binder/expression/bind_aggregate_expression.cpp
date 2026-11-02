#include "duckdb/catalog/catalog_entry/aggregate_function_catalog_entry.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/pair.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/function/scalar/generic_common.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression_binder/base_select_binder.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/query_node/bound_select_node.hpp"
#include "duckdb/main/settings.hpp"

namespace duckdb {

bool BaseSelectBinder::IsFunctionallyDependent(const unique_ptr<Expression> &expr,
                                               const vector<reference<Expression>> &deps) {
	//	Volatile expressions can't depend on anything else
	if (expr->IsVolatile()) {
		return false;
	}
	//	Constant expressions are always FD
	if (expr->IsFoldable()) {
		return true;
	}
	// If the expression matches ANY of the dependencies, then it is FD on them
	for (const auto &dep : deps) {
		// We don't need to check volatility of the dependencies because we checked it for the expression.
		if (expr->Equals(dep.get())) {
			return true;
		}
	}

	// The expression doesn't match any dependency, so check ALL children.
	bool has_children = false;
	bool are_dependent = true;
	ExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<Expression> &child) {
		has_children = true;
		are_dependent &= IsFunctionallyDependent(child, deps);
	});
	return has_children && are_dependent;
}

static Value NegatePercentileValue(const Value &v, const bool desc) {
	if (v.IsNull()) {
		return v;
	}

	const auto frac = v.GetValue<double>();
	if (frac < 0 || frac > 1) {
		throw BinderException("PERCENTILEs can only take parameters in the range [0, 1]");
	}

	if (!desc) {
		return v;
	}

	// A zero fraction has no sign to negate (-0 == 0), which would drop the descending order.
	// Emit a negative zero so the sign survives to the aggregate bind data.
	if (frac == 0) {
		return Value::DOUBLE(-0.0);
	}

	const auto &type = v.type();
	switch (type.id()) {
	case LogicalTypeId::DECIMAL: {
		// Negate DECIMALs as DECIMAL.
		const auto integral = IntegralValue::Get(v);
		const auto width = DecimalType::GetWidth(type);
		const auto scale = DecimalType::GetScale(type);
		switch (type.InternalType()) {
		case PhysicalType::INT16:
			return Value::DECIMAL(Cast::Operation<hugeint_t, int16_t>(-integral), width, scale);
		case PhysicalType::INT32:
			return Value::DECIMAL(Cast::Operation<hugeint_t, int32_t>(-integral), width, scale);
		case PhysicalType::INT64:
			return Value::DECIMAL(Cast::Operation<hugeint_t, int64_t>(-integral), width, scale);
		case PhysicalType::INT128:
			return Value::DECIMAL(-integral, width, scale);
		default:
			throw InternalException("Unknown DECIMAL type");
		}
	}
	default:
		// Everything else can just be a DOUBLE
		return Value::DOUBLE(-v.GetValue<double>());
	}
}

static void NegatePercentileFractions(ClientContext &context, unique_ptr<Expression> &bound, bool desc) {
	if (!bound->IsFoldable()) {
		return;
	}

	Value value = ExpressionExecutor::EvaluateScalar(context, *bound);
	if (value.type().id() == LogicalTypeId::LIST) {
		vector<Value> values;
		for (const auto &element_val : ListValue::GetChildren(value)) {
			values.push_back(NegatePercentileValue(element_val, desc));
		}
		if (values.empty()) {
			throw BinderException("Empty list in percentile not allowed");
		}
		bound = make_uniq<BoundConstantExpression>(Value::LIST(values));
	} else {
		bound = make_uniq<BoundConstantExpression>(NegatePercentileValue(value, desc));
	}
}

BindResult BaseSelectBinder::BindAggregate(FunctionExpression &aggr, AggregateFunctionCatalogEntry &func, idx_t depth) {
	auto count_star = binder.TryRewriteQualifiedCountStar(aggr);
	if (count_star) {
		return BindAggregate(count_star->Cast<FunctionExpression>(), func, depth);
	}
	if (inside_try) {
		throw BinderException("aggregates are not allowed inside the TRY expression");
	}
	if (inside_aggregate_filter) {
		throw BinderException(aggr, "aggregate functions are not allowed in FILTER");
	}
	if (inside_aggregate) {
		throw BinderException(aggr, "aggregate function calls cannot be nested");
	}
	// an aggregate is computed over the rows of the query, not over the elements an enclosing lambda is applied to -
	// so the parameters of the enclosing lambdas can not be referenced in the aggregate
	struct InaccessibleLambdaBindings {
		InaccessibleLambdaBindings(ExpressionBinder &binder_p, idx_t count)
		    : binder(binder_p), previous_count(binder.inaccessible_lambda_binding_count) {
			binder.inaccessible_lambda_binding_count = count;
		}
		~InaccessibleLambdaBindings() {
			binder.inaccessible_lambda_binding_count = previous_count;
		}
		ExpressionBinder &binder;
		idx_t previous_count;
	};
	InaccessibleLambdaBindings inaccessible_lambdas(*this, lambda_bindings ? lambda_bindings->size() : 0);

	// first bind the child of the aggregate expression (if any)
	this->bound_aggregate = true;
	unique_ptr<Expression> bound_filter;
	ErrorData error;

	inside_aggregate_filter = true;
	this->inside_aggregate = true;
	// aggregate arguments are evaluated below the select list projection of the enclosing node, where
	// a transported error flag has no consumer; nested subqueries in them bind as plain subqueries
	const auto save_transport_try_errors = transport_try_errors;
	transport_try_errors = false;
	auto initial_bound_column_count = GetBoundColumns().size();
	// Now we bind the filter (if any)
	unique_ptr<Expression> bound_filter_child;
	if (aggr.Filter()) {
		bound_filter_child = BindChild(aggr.FilterMutable(), 0, error);
	}
	inside_aggregate_filter = false;

	// Handle ordered-set aggregates by moving the single ORDER BY expression to the front of the children.
	//	https://www.postgresql.org/docs/current/functions-aggregate.html#FUNCTIONS-ORDEREDSET-TABLE
	// We also have to handle ORDER BY in the argument list, so note how many arguments we should have
	// and only inject the ordering expression if there are too few.
	idx_t ordered_set_agg = 0;
	bool negate_fractions = false;
	if (aggr.OrderBy() && aggr.OrderBy()->orders.size() == 1) {
		const auto &func_name = aggr.FunctionName();
		if (func_name == "mode") {
			ordered_set_agg = 1;
		} else if (func_name == "quantile_cont" || func_name == "quantile_disc") {
			ordered_set_agg = 2;

			auto &config = DBConfig::GetConfig(context);
			const auto &order = aggr.OrderBy()->orders[0];
			const auto sense = config.ResolveOrder(context, order.type);
			negate_fractions = (sense == OrderType::DESCENDING);
		}
	}

	vector<unique_ptr<Expression>> bound_arguments;
	for (idx_t i = 0; i < aggr.GetArguments().size(); ++i) {
		auto &child = aggr.GetArgumentsMutable()[i];
		bound_arguments.push_back(BindChild(child.GetExpressionMutable(), 0, error));
		// We have to negate the fractions for PERCENTILE_XXXX DESC
		if (!error.HasError() && ordered_set_agg && i == aggr.GetArguments().size() - 1) {
			NegatePercentileFractions(context, bound_arguments.back(), negate_fractions);
		}
	}

	// Bind the ORDER BYs, if any
	vector<unique_ptr<Expression>> bound_orders;
	if (aggr.OrderBy() && !aggr.OrderBy()->orders.empty()) {
		for (auto &order : aggr.OrderByMutable()->orders) {
			if (order.expression->GetExpressionType() == ExpressionType::VALUE_CONSTANT) {
				auto &const_expr = order.expression->Cast<ConstantExpression>();
				if (const_expr.GetLiteral().kind != LiteralKind::INTEGER) {
					auto order_by_non_integer_literal = Settings::Get<OrderByNonIntegerLiteralSetting>(context);
					if (!order_by_non_integer_literal) {
						throw BinderException(
						    *order.expression,
						    "ORDER BY non-integer literal has no effect.\n* SET order_by_non_integer_literal=true to "
						    "allow this behavior.\n\nPerhaps you misplaced ORDER BY; ORDER BY must appear "
						    "after all regular arguments of the aggregate.");
					}
				}
			}
			bound_orders.push_back(BindChild(order.expression, 0, error));
		}
	}

	inside_aggregate = false;
	transport_try_errors = save_transport_try_errors;
	TruncateBoundColumns(initial_bound_column_count);
	if (error.HasError()) {
		// the arguments decided that this level owns the aggregate, so a child that does not bind here is an error
		return BindResult(std::move(error));
	}

	if (aggr.Filter()) {
		bound_filter = BoundCastExpression::AddCastToType(context, std::move(bound_filter_child), LogicalType::BOOLEAN);
	}

	// all children bound successfully - collect them (with their explicit names, if any) into the full argument list.
	// The positional/named split and (for legacy calls) the alias capture are resolved later, per candidate overload.
	vector<pair<Identifier, unique_ptr<Expression>>> arguments;

	if (ordered_set_agg) {
		const bool order_sensitive = (aggr.FunctionName() == "mode");
		// Inject missing ordering arguments as positional arguments
		if (aggr.GetArguments().size() < ordered_set_agg) {
			for (auto &bound_order : bound_orders) {
				if (order_sensitive) {
					arguments.emplace_back(string(), bound_order->Copy());
				} else {
					arguments.emplace_back(string(), std::move(bound_order));
				}
			}
		}
		if (!order_sensitive) {
			aggr.OrderByMutable()->orders.clear();
		}
	}

	for (idx_t arg_idx = 0; arg_idx < aggr.GetArgumentsMutable().size(); arg_idx++) {
		auto &arg = aggr.GetArgumentsMutable()[arg_idx];
		auto bound_arg = std::move(bound_arguments[arg_idx]);
		// legacy function calls cannot have named arguments, so we ignore the names of the arguments during binding
		// and pass them all positionally, aliasing them by their name (see BindFunction for the rationale)
		if (!arg.GetName().empty()) {
			bound_arg->SetAlias(arg.GetName());
		}
		if (aggr.IsLegacyFunctionCall()) {
			arguments.emplace_back(string(), std::move(bound_arg));
		} else {
			arguments.emplace_back(arg.GetName(), std::move(bound_arg));
		}
	}

	// Bind any sort columns, unless the aggregate is order-insensitive
	unique_ptr<BoundOrderModifier> order_bys;
	if (!aggr.OrderBy()->orders.empty()) {
		order_bys = make_uniq<BoundOrderModifier>();
		auto &config = DBConfig::GetConfig(context);
		for (idx_t order_idx = 0; order_idx < aggr.OrderByMutable()->orders.size(); order_idx++) {
			auto &order = aggr.OrderByMutable()->orders[order_idx];
			auto order_expr = std::move(bound_orders[order_idx]);
			PushCollation(context, order_expr, order_expr->GetReturnType());
			const auto sense = config.ResolveOrder(context, order.type);
			const auto null_order = config.ResolveNullOrder(context, sense, order.null_order);
			order_bys->orders.emplace_back(sense, null_order, std::move(order_expr));
		}
	}

	// If the aggregate is DISTINCT then the ORDER BYs need to be functional dependencies of the arguments.
	if (aggr.Distinct() && order_bys) {
		vector<reference<Expression>> arg_refs;
		arg_refs.reserve(arguments.size());
		for (auto &arg : arguments) {
			arg_refs.emplace_back(*arg.second);
		}
		bool in_args = true;
		for (const auto &order_by : order_bys->orders) {
			in_args &= IsFunctionallyDependent(order_by.expression, arg_refs);
		}

		if (!in_args) {
			throw BinderException("In a DISTINCT aggregate, ORDER BY expressions must appear in the argument list");
		}
	}

	// Bind the function
	FunctionBinder function_binder(binder);
	auto aggregate =
	    function_binder.BindAggregateFunction(func, std::move(arguments), error, std::move(bound_filter),
	                                          aggr.Distinct() ? AggregateType::DISTINCT : AggregateType::NON_DISTINCT);
	// No function found, throw an error
	if (!aggregate) {
		error.AddQueryLocation(aggr);
		error.Throw();
	}

	// If the function cannot be used as an aggregate, but can be used as a window function, throw a specific error
	// message
	if (!aggregate->Function().CanAggregate() && aggregate->Function().CanWindow()) {
		auto msg = StringUtil::Format("Function %s can only be used as a window function", func.name);
		error = BinderException(msg);
		error.AddQueryLocation(aggr);
		error.Throw();
	}

	// attach the ORDER BY before the state export: an ordered aggregate's exported type depends on the ORDER BY keys
	aggregate->GetOrderBysMutable() = std::move(order_bys);
	if (aggr.ExportState()) {
		aggregate = ExportAggregateFunction::Bind(std::move(aggregate));
	}

	// check for all the aggregates if this aggregate already exists
	ProjectionIndex aggr_index;
	auto entry = node.aggregate_map.find(*aggregate);
	if (entry == node.aggregate_map.end()) {
		// new aggregate: insert into aggregate list
		auto &aggr_ref = *aggregate;
		aggr_index = ColumnBinding::PushExpression(node.aggregates, std::move(aggregate));
		node.aggregate_map[aggr_ref] = aggr_index;
	} else {
		// duplicate aggregate: simplify refer to this aggregate
		aggr_index = entry->second;
	}
	auto &bound_aggr = *node.aggregates[aggr_index];

	// now create a column reference referring to the aggregate
	auto colref = make_uniq<BoundColumnRefExpression>(
	    aggr.GetAlias().empty() ? Identifier(bound_aggr.ToString()) : aggr.GetAlias(), bound_aggr.GetReturnType(),
	    ColumnBinding(node.aggregate_index, aggr_index), depth);
	// move the aggregate expression into the set of bound aggregates
	return BindResult(std::move(colref));
}
} // namespace duckdb
