#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/aggregate_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/window_function_catalog_entry.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/catalog/catalog_entry/scalar_macro_catalog_entry.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/function/aggregate/distributive_functions.hpp"
#include "duckdb/parser/expression/case_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/window_expression.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/expression_binder/base_select_binder.hpp"
#include "duckdb/planner/query_node/bound_select_node.hpp"

namespace duckdb {

static unique_ptr<Expression> CastWindowExpression(unique_ptr<Expression> bound, const LogicalType &type) {
	if (!bound) {
		return nullptr;
	}
	return BoundCastExpression::AddDefaultCastToType(std::move(bound), type);
}

static bool IsRangeType(const LogicalType &type) {
	switch (type.InternalType()) {
	case PhysicalType::INT8:
	case PhysicalType::INT16:
	case PhysicalType::INT32:
	case PhysicalType::INT64:
	case PhysicalType::UINT8:
	case PhysicalType::UINT16:
	case PhysicalType::UINT32:
	case PhysicalType::UINT64:
	case PhysicalType::INT128:
	case PhysicalType::UINT128:
	case PhysicalType::FLOAT:
	case PhysicalType::DOUBLE:
	case PhysicalType::INTERVAL:
		return true;
	default:
		return false;
	}
}

static LogicalType BindRangeExpression(ClientContext &context, const string &name, unique_ptr<Expression> &bound,
                                       unique_ptr<Expression> &bound_order,
                                       optional_ptr<unique_ptr<WindowRangeBoundary>> origin = nullptr) {
	vector<unique_ptr<Expression>> children;

	D_ASSERT(bound_order);
	children.emplace_back(bound_order->Copy());

	D_ASSERT(bound);
	QueryErrorContext error_context(bound->GetQueryLocation());
	if (bound->GetReturnType() == LogicalType::SQLNULL) {
		throw BinderException(error_context, "Window RANGE expressions cannot be NULL");
	}
	// the function binder casts the operands and folds a constant offset to its value. The origin recognizes a
	// constant offset in the bound call by value, so keep a copy of it - any other offset stays in place
	unique_ptr<Expression> offset_copy = origin && bound->IsFoldable() ? bound->Copy() : nullptr;
	optional_ptr<const Expression> offset = offset_copy ? offset_copy.get() : bound.get();
	children.emplace_back(std::move(bound));

	FunctionBinder function_binder(context);
	auto function =
	    function_binder.BindScalarFunction(Identifier::DefaultSchema(), Identifier(name), std::move(children), true);
	// +/- can be applied to non-scalar types,
	// so we can't rely on function binding to catch all problems.
	if (!IsRangeType(function->GetReturnType())) {
		throw BinderException(error_context, "Invalid type for Window RANGE expression");
	}
	if (origin) {
		*origin = WindowRangeBoundary::Capture(*function, *bound_order, *offset);
	}
	bound = std::move(function);
	return bound->GetReturnType();
}

//! COUNT(*) FILTER (WHERE p) counts the rows where p holds, which is what COUNT(v) counts for a v
//! that is null exactly where p does not. Spelled that way the frame is answered by a segment tree;
//! COUNT(*) has no argument for one to be built over, so it counts the rows a filter leaves in one at
//! a time - a pass over the partition per row, where the frame grows a row at a time.
static void RewriteFilteredCountStar(WindowExpression &window) {
	if (!window.FilterMutable() || !window.GetArguments().empty() || window.Distinct() || !window.ArgOrders().empty()) {
		return;
	}
	auto &qualified = window.GetQualifiedName();
	// COUNT(*) reaches here as COUNT with no argument; the star is what the parser drops
	const auto &spelled = qualified.Name().GetIdentifierName();
	if (!qualified.Catalog().empty() || !qualified.Schema().empty() ||
	    (!StringUtil::CIEquals(spelled, CountFun::Name) && !StringUtil::CIEquals(spelled, CountStarFun::Name))) {
		return;
	}

	auto kept = make_uniq<CaseExpression>();
	CaseCheck check;
	check.when_expr = std::move(window.FilterMutable());
	check.then_expr = ConstantExpression::FromValue(Value::BOOLEAN(true));
	kept->CaseChecksMutable().push_back(std::move(check));
	kept->ElseMutable() = ConstantExpression::FromValue(Value(LogicalType::BOOLEAN));

	window.GetArgumentsMutable().emplace_back(std::move(kept));
	window.FilterMutable() = nullptr;
	window.SetFunctionName(CountFun::Name);
}

BindResult BaseSelectBinder::BindWindowExpression(WindowExpression &window, idx_t depth) {
	QueryErrorContext error_context(window.GetQueryLocation());

	RewriteFilteredCountStar(window);

	//	Check for macros pretending to be aggregates
	EntryLookupInfo function_lookup(CatalogType::SCALAR_FUNCTION_ENTRY, window.GetQualifiedName(), error_context);
	auto entry = GetCatalogEntry(function_lookup, OnEntryNotFound::RETURN_NULL);
	if (entry && entry->type == CatalogType::MACRO_ENTRY) {
		auto macro_expr = window.Copy();
		auto macro = make_uniq<FunctionExpression>(window.GetQualifiedName(), std::move(window.GetArgumentsMutable()),
		                                           std::move(window.FilterMutable()), nullptr, window.Distinct());
		return BindMacro(*macro, entry->Cast<ScalarMacroCatalogEntry>(), depth, macro_expr);
	}
	auto count_star = binder.TryRewriteQualifiedCountStar(window);
	if (count_star) {
		return BindWindowExpression(count_star->Cast<WindowExpression>(), depth);
	}

	auto name = window.GetAlias();

	if (inside_try) {
		throw BinderException("window functions are not allowed in try");
	}
	if (inside_aggregate) {
		throw BinderException(window, "aggregate function calls cannot contain window function calls");
	}
	if (inside_window) {
		throw BinderException(window, "window function calls cannot be nested");
	}
	if (depth > 0) {
		throw BinderException(window, "correlated columns in window functions not supported");
	}

	//  Look up the aggregate function in the catalog
	if (!entry ||
	    (entry->type != CatalogType::AGGREGATE_FUNCTION_ENTRY && entry->type != CatalogType::WINDOW_FUNCTION_ENTRY)) {
		//	Not an aggregate or window function: Look it up to generate error
		Catalog::GetEntry<AggregateFunctionCatalogEntry>(context,
		                                                 QualifiedName(window.GetQualifiedName().Catalog(),
		                                                               window.GetQualifiedName().Schema(),
		                                                               window.FunctionName()),
		                                                 error_context);
	}

	// If we have range expressions, then only one order by clause is allowed.
	const auto is_range = (window.WindowStart() == WindowBoundary::EXPR_PRECEDING_RANGE ||
	                       window.WindowStart() == WindowBoundary::EXPR_FOLLOWING_RANGE ||
	                       window.WindowEnd() == WindowBoundary::EXPR_PRECEDING_RANGE ||
	                       window.WindowEnd() == WindowBoundary::EXPR_FOLLOWING_RANGE);
	if (is_range && window.OrderBy().size() != 1) {
		throw BinderException(error_context, "RANGE frames must have only one ORDER BY expression");
	}
	// bind inside the children of the window function
	// we set the inside_window flag to true to prevent binding nested window functions
	inside_window = true;
	// window arguments are evaluated by the window operator, where a transported error flag has no
	// consumer; nested subqueries in them bind as plain subqueries
	const auto save_transport_try_errors = transport_try_errors;
	transport_try_errors = false;
	ErrorData error;
	vector<unique_ptr<Expression>> bound_args;
	for (auto &child : window.GetArgumentsMutable()) {
		bound_args.push_back(BindChild(child.GetExpressionMutable(), depth, error));
	}
	vector<unique_ptr<Expression>> bound_partitions;
	for (auto &child : window.PartitionsMutable()) {
		auto bound_partition = BindChild(child, depth, error);
		if (!error.HasError() && bound_partition->IsVolatile()) {
			throw BinderException(error_context, "PARTITION BY window expressions cannot be volatile");
		}
		bound_partitions.push_back(std::move(bound_partition));
	}
	vector<unique_ptr<Expression>> bound_orders;
	for (auto &order : window.OrderByMutable()) {
		auto bound_order = BindChild(order.expression, depth, error);
		if (!error.HasError() && bound_order->IsVolatile()) {
			throw BinderException(error_context, "ORDER BY window expressions cannot be volatile");
		}

		//	If the frame is a RANGE frame and the type is a time,
		//	then we have to convert the time to a timestamp to avoid wrapping.
		if (!is_range || error.HasError()) {
			bound_orders.push_back(std::move(bound_order));
			continue;
		}
		const auto type_id = bound_order->GetReturnType().id();
		if (type_id == LogicalTypeId::TIME || type_id == LogicalTypeId::TIME_TZ) {
			//	Convert to time + epoch and rebind
			unique_ptr<ParsedExpression> epoch = ConstantExpression::FromValue(Value::DATE(date_t::epoch()));
			auto bound_epoch = BindChild(epoch, depth, error);
			BindRangeExpression(context, "+", bound_order, bound_epoch);
		}
		bound_orders.push_back(std::move(bound_order));
	}
	auto bound_filter = BindChild(window.FilterMutable(), depth, error);
	auto bound_start = BindChild(window.StartExprMutable(), depth, error);
	auto bound_end = BindChild(window.EndExprMutable(), depth, error);

	vector<unique_ptr<Expression>> bound_arg_orders;
	for (auto &order : window.ArgOrdersMutable()) {
		bound_arg_orders.push_back(BindChild(order.expression, depth, error));
	}

	inside_window = false;
	transport_try_errors = save_transport_try_errors;
	if (error.HasError()) {
		// failed to bind children of window function
		return BindResult(std::move(error));
	}

	//	Restore any collation expressions
	for (auto &bound_order : bound_arg_orders) {
		ExpressionBinder::PushCollation(context, bound_order, bound_order->GetReturnType());
	}
	for (auto &bound_partition : bound_partitions) {
		ExpressionBinder::PushCollation(context, bound_partition, bound_partition->GetReturnType());
	}
	for (auto &bound_order : bound_orders) {
		ExpressionBinder::PushCollation(context, bound_order, bound_order->GetReturnType());
	}

	vector<pair<Identifier, unique_ptr<Expression>>> arguments;
	arguments.reserve(window.GetArguments().size());
	for (idx_t arg_idx = 0; arg_idx < window.GetArgumentsMutable().size(); arg_idx++) {
		auto &arg = window.GetArgumentsMutable()[arg_idx];
		auto bound_arg = std::move(bound_args[arg_idx]);

		// legacy function calls cannot have named arguments, so we ignore the names of the arguments during binding
		// and pass them all positionally. We do alias them by their name though, so that alias-capturing functions
		// (e.g. struct_pack) still work and so that re-serializing to the old format can match arguments by name.
		// Only override the alias when the argument actually carries a name, otherwise we would clobber the
		// display alias the binding assigned (e.g. clearing a column reference's name to its raw binding).
		if (!arg.GetName().empty()) {
			bound_arg->SetAlias(arg.GetName());
		}

		if (window.IsLegacyFunctionCall()) {
			arguments.emplace_back(Identifier(), std::move(bound_arg));
		} else {
			arguments.emplace_back(arg.GetName(), std::move(bound_arg));
		}
	}

	//  Determine the function type.
	LogicalType sql_type;
	unique_ptr<BoundAggregateFunction> aggregate;
	unique_ptr<BoundWindowFunction> window_func;
	unique_ptr<FunctionData> bind_info;
	vector<unique_ptr<Expression>> children;

	if (entry->type == CatalogType::AGGREGATE_FUNCTION_ENTRY) {
		auto &func = entry->Cast<AggregateFunctionCatalogEntry>();

		if (window.HasIgnoreNulls()) {
			throw BinderException(error_context, "RESPECT/IGNORE NULLS is not supported for windowed aggregates");
		}

		// bind the aggregate
		FunctionBinder function_binder(binder);
		auto window_bound_aggregate = function_binder.BindAggregateFunction(
		    func, std::move(arguments), error, nullptr,
		    window.Distinct() ? AggregateType::DISTINCT : AggregateType::NON_DISTINCT);
		// No function found, throw an error
		if (!window_bound_aggregate) {
			error.AddQueryLocation(window);
			error.Throw();
		}

		// create the aggregate
		aggregate = make_uniq<BoundAggregateFunction>(window_bound_aggregate->Function());
		bind_info = std::move(window_bound_aggregate->BindInfoMutable());
		children = std::move(window_bound_aggregate->GetChildrenMutable());
		sql_type = window_bound_aggregate->GetReturnType();

	} else {
		auto &func = entry->Cast<WindowFunctionCatalogEntry>();

		FunctionBinder function_binder(binder);
		// resolve the types of the ORDER BY expressions for the function's bind callback
		vector<LogicalType> order_types;
		for (auto &bound_order : bound_orders) {
			order_types.push_back(bound_order->GetReturnType());
		}
		vector<LogicalType> arg_order_types;
		for (auto &bound_order : bound_arg_orders) {
			arg_order_types.push_back(bound_order->GetReturnType());
		}
		auto window_bound_function =
		    function_binder.BindWindowFunction(func, std::move(arguments), error, order_types, arg_order_types);

		if (!window_bound_function) {
			error.AddQueryLocation(window);
			error.Throw();
		}

		// TODO: Move this into the binder
		auto &bound_function = *window_bound_function->WindowFunction();
		if (window.Distinct() && !bound_function.CanDistinct()) {
			throw BinderException(error_context, "DISTINCT is not implemented for the window function %s",
			                      window.FunctionName());
		}
		if (window.Filter() && !bound_function.CanFilter()) {
			throw BinderException(error_context, "FILTER is not implemented for the window function %s",
			                      window.FunctionName());
		}
		if (!window.ArgOrders().empty() && !bound_function.CanOrderBy()) {
			throw BinderException(error_context, "ORDER BY is not supported for the window function %s",
			                      window.FunctionName());
		}
		if (window.WindowExclude() != WindowExcludeMode::NO_OTHER && !window.ArgOrders().empty() &&
		    !bound_function.CanExclude()) {
			throw BinderException(error_context, "EXCLUDE is not supported for the window function %s",
			                      window.FunctionName());
		}
		if (window.HasIgnoreNulls() && !bound_function.CanIgnoreNulls()) {
			throw BinderException(error_context, "RESPECT/IGNORE NULLS is not supported for the window function %s",
			                      window.FunctionName());
		}

		window_func = std::move(window_bound_function->WindowFunctionMutable());
		bind_info = std::move(window_bound_function->BindInfoMutable());
		children = std::move(window_bound_function->GetChildrenMutable());
		sql_type = window_bound_function->GetReturnType();
	}

	auto result =
	    make_uniq<BoundWindowExpression>(sql_type, std::move(aggregate), std::move(window_func), std::move(bind_info));
	result->GetChildrenMutable() = std::move(children);
	for (auto &bound_partition : bound_partitions) {
		result->PartitionsMutable().push_back(std::move(bound_partition));
	}
	result->IgnoreNullsMutable() = window.IgnoreNulls();
	result->DistinctMutable() = window.Distinct();

	const bool range_start = window.WindowStart() == WindowBoundary::EXPR_PRECEDING_RANGE ||
	                         window.WindowStart() == WindowBoundary::EXPR_FOLLOWING_RANGE;
	const bool range_end = window.WindowEnd() == WindowBoundary::EXPR_PRECEDING_RANGE ||
	                       window.WindowEnd() == WindowBoundary::EXPR_FOLLOWING_RANGE;
	if ((range_start || range_end) && bound_orders.size() == 1) {
		result->RetainSQLRange(range_start ? bound_start.get() : nullptr, range_end ? bound_end.get() : nullptr,
		                       bound_orders[0]->GetReturnType());
	}

	// Convert RANGE boundary expressions to ORDER +/- expressions.
	// Note that PRECEDING and FOLLOWING refer to the sequential order in the frame,
	// not the natural ordering of the type. This means that the offset arithmetic must be reversed
	// for ORDER BY DESC.
	auto &config = DBConfig::GetConfig(context);
	auto range_sense = OrderType::INVALID;
	LogicalType start_type = LogicalType::BIGINT;
	if (window.WindowStart() == WindowBoundary::EXPR_PRECEDING_RANGE) {
		D_ASSERT(window.OrderBy().size() == 1);
		range_sense = config.ResolveOrder(context, window.OrderByMutable()[0].type);
		const auto range_name = (range_sense == OrderType::ASCENDING) ? "-" : "+";
		start_type = BindRangeExpression(context, range_name, bound_start, bound_orders[0],
		                                 &result->SQLRangeStartBoundaryMutable());

	} else if (window.WindowStart() == WindowBoundary::EXPR_FOLLOWING_RANGE) {
		D_ASSERT(window.OrderBy().size() == 1);
		range_sense = config.ResolveOrder(context, window.OrderByMutable()[0].type);
		const auto range_name = (range_sense == OrderType::ASCENDING) ? "+" : "-";
		start_type = BindRangeExpression(context, range_name, bound_start, bound_orders[0],
		                                 &result->SQLRangeStartBoundaryMutable());
	}

	LogicalType end_type = LogicalType::BIGINT;
	if (window.WindowEnd() == WindowBoundary::EXPR_PRECEDING_RANGE) {
		D_ASSERT(window.OrderBy().size() == 1);
		range_sense = config.ResolveOrder(context, window.OrderByMutable()[0].type);
		const auto range_name = (range_sense == OrderType::ASCENDING) ? "-" : "+";
		end_type =
		    BindRangeExpression(context, range_name, bound_end, bound_orders[0], &result->SQLRangeEndBoundaryMutable());

	} else if (window.WindowEnd() == WindowBoundary::EXPR_FOLLOWING_RANGE) {
		D_ASSERT(window.OrderBy().size() == 1);
		range_sense = config.ResolveOrder(context, window.OrderByMutable()[0].type);
		const auto range_name = (range_sense == OrderType::ASCENDING) ? "+" : "-";
		end_type =
		    BindRangeExpression(context, range_name, bound_end, bound_orders[0], &result->SQLRangeEndBoundaryMutable());
	}

	if (result->SQLRangeStartBoundary()) {
		result->SQLRangeStartBoundaryMutable()->boundary = window.WindowStart();
		result->SQLRangeStartBoundaryMutable()->direction = range_sense;
	}
	if (result->SQLRangeEndBoundary()) {
		result->SQLRangeEndBoundaryMutable()->boundary = window.WindowEnd();
		result->SQLRangeEndBoundaryMutable()->direction = range_sense;
	}

	// Cast ORDER and boundary expressions to the same type
	if (range_sense != OrderType::INVALID) {
		D_ASSERT(window.OrderBy().size() == 1);

		auto &bound_order = bound_orders[0];
		D_ASSERT(bound_order);
		auto order_type = bound_order->GetReturnType();
		if (window.StartExpr()) {
			order_type = LogicalType::MaxLogicalType(context, order_type, start_type);
		}
		if (window.EndExpr()) {
			order_type = LogicalType::MaxLogicalType(context, order_type, end_type);
		}

		// Cast all three to match
		auto &original_order = *bound_order;
		bound_order = BoundCastExpression::AddCastToType(context, std::move(bound_order), order_type);
		if (!WindowRangeCast::Capture(*bound_order, original_order, result->SQLRangeOrderCastsMutable())) {
			result->SQLRangeStartBoundaryMutable().reset();
			result->SQLRangeEndBoundaryMutable().reset();
		}
		start_type = end_type = order_type;
	}

	for (idx_t order_idx = 0; order_idx < window.OrderByMutable().size(); order_idx++) {
		auto &order = window.OrderByMutable()[order_idx];
		auto type = config.ResolveOrder(context, order.type);
		auto null_order = config.ResolveNullOrder(context, type, order.null_order);
		result->OrderByMutable().emplace_back(type, null_order, std::move(bound_orders[order_idx]));
	}

	// Argument orders are just like arguments, not frames
	for (idx_t order_idx = 0; order_idx < window.ArgOrdersMutable().size(); order_idx++) {
		auto &order = window.ArgOrdersMutable()[order_idx];
		auto type = config.ResolveOrder(context, order.type);
		auto null_order = config.ResolveNullOrder(context, type, order.null_order);
		result->ArgOrdersMutable().emplace_back(type, null_order, std::move(bound_arg_orders[order_idx]));
	}

	result->FilterMutable() = CastWindowExpression(std::move(bound_filter), LogicalType::BOOLEAN);
	optional_ptr<const Expression> original_start = bound_start.get();
	optional_ptr<const Expression> original_end = bound_end.get();
	result->StartExprMutable() = CastWindowExpression(std::move(bound_start), start_type);
	result->EndExprMutable() = CastWindowExpression(std::move(bound_end), end_type);
	if (result->SQLRangeStartBoundary() &&
	    !WindowRangeCast::Capture(*result->StartExpr(), *original_start,
	                              result->SQLRangeStartBoundaryMutable()->result_casts)) {
		result->SQLRangeStartBoundaryMutable().reset();
	}
	if (result->SQLRangeEndBoundary() &&
	    !WindowRangeCast::Capture(*result->EndExpr(), *original_end,
	                              result->SQLRangeEndBoundaryMutable()->result_casts)) {
		result->SQLRangeEndBoundaryMutable().reset();
	}
	result->WindowStartMutable() = window.WindowStart();
	result->WindowEndMutable() = window.WindowEnd();
	result->WindowExcludeMutable() = window.WindowExclude();

	// move the WINDOW expression into the set of bound windows
	auto &window_type = result->GetReturnType();
	auto window_idx = ColumnBinding::PushExpression(node.windows, std::move(result));
	auto colref =
	    make_uniq<BoundColumnRefExpression>(name, window_type, ColumnBinding(node.window_index, window_idx), depth);
	return BindResult(std::move(colref));
}

} // namespace duckdb
