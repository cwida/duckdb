#include "duckdb/catalog/catalog.hpp"
#include "duckdb/logging/log_manager.hpp"
#include "duckdb/catalog/catalog_entry/aggregate_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/scalar_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/scalar_macro_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/window_function_catalog_entry.hpp"
#include "duckdb/common/assert.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/lambda_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_lambda_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression_binder.hpp"
#include "duckdb/planner/column_qualifier.hpp"
#include "duckdb/main/settings.hpp"

#include <functional>

namespace duckdb {

namespace {

static bool TypeContainsDecimal(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::DECIMAL:
		return true;
	case LogicalTypeId::LIST:
		return TypeContainsDecimal(ListType::GetChildType(type));
	case LogicalTypeId::ARRAY:
		return TypeContainsDecimal(ArrayType::GetChildType(type));
	case LogicalTypeId::STRUCT:
	case LogicalTypeId::TUPLE: {
		for (const auto &child : StructType::GetChildTypes(type)) {
			if (TypeContainsDecimal(child.second)) {
				return true;
			}
		}
		return false;
	}
	case LogicalTypeId::MAP:
		return TypeContainsDecimal(MapType::KeyType(type)) || TypeContainsDecimal(MapType::ValueType(type));
	case LogicalTypeId::UNION: {
		for (const auto &child : UnionType::CopyMemberTypes(type)) {
			if (TypeContainsDecimal(child.second)) {
				return true;
			}
		}
		return false;
	}
	default:
		return false;
	}
}

class ListReduceBindLambdaContext final : public BindLambdaContext {
public:
	explicit ListReduceBindLambdaContext(LogicalType accumulator_type) : accumulator_type(std::move(accumulator_type)) {
	}

	const LogicalType accumulator_type;
};

LogicalType ListReduceBindLambdaOverride(ClientContext &context, const vector<LogicalType> &function_child_types,
                                         const idx_t parameter_idx,
                                         optional_ptr<BindLambdaContext> bind_lambda_context) {
	D_ASSERT(bind_lambda_context);
	auto bind_context = bind_lambda_context->Cast<ListReduceBindLambdaContext>();

	auto list_child_type = function_child_types[0];
	if (list_child_type.id() != LogicalTypeId::SQLNULL && list_child_type.id() != LogicalTypeId::UNKNOWN) {
		if (list_child_type.id() == LogicalTypeId::ARRAY) {
			list_child_type = ArrayType::GetChildType(list_child_type);
		} else if (list_child_type.id() == LogicalTypeId::LIST) {
			list_child_type = ListType::GetChildType(list_child_type);
		} else {
			throw InternalException("The first argument must be a list or array type");
		}
	}
	switch (parameter_idx) {
	case 0:
		return bind_context.accumulator_type;
	case 1:
		return list_child_type;
	case 2:
		return LogicalType::BIGINT;
	default:
		throw BinderException("This lambda function only supports up to three lambda parameters!");
	}
}

using bind_lambda_expression_t =
    std::function<BindResult(LambdaExpression &, idx_t, const vector<LogicalType> &,
                             optional_ptr<bind_lambda_function_t>, optional_ptr<BindLambdaContext>)>;

struct ListReduceRebindResult {
	bool did_rebind = false;
	vector<LogicalType> capture_child_types;
	bind_lambda_function_t override_bind_lambda = nullptr;
	unique_ptr<BindLambdaContext> override_bind_lambda_context;
	LogicalType override_accumulator_type_storage;
	bool override_has_accumulator_type = false;
};

//! Avoid repeated rebinds for DECIMAL type widening by forcing the lambda return type to the chosen accumulator type
//! when decimals are involved
static void CastDecimalLambdaToAccumulatorType(ClientContext &context, BindResult &bind_lambda_result,
                                               const LogicalType &accumulator_type) {
	auto &rebound_lambda_expr = bind_lambda_result.expression->Cast<BoundLambdaExpression>();
	const auto return_type = rebound_lambda_expr.LambdaExpr()->GetReturnType();
	if (return_type == accumulator_type ||
	    (!TypeContainsDecimal(accumulator_type) && !TypeContainsDecimal(return_type))) {
		return;
	}
	auto cast_expr = BoundCastExpression::AddCastToType(context, std::move(rebound_lambda_expr.LambdaExprMutable()),
	                                                    accumulator_type);
	if (!cast_expr) {
		throw BinderException("Could not cast lambda return type %s to accumulator type %s", return_type.ToString(),
		                      accumulator_type.ToString());
	}
	rebound_lambda_expr.LambdaExprMutable() = std::move(cast_expr);
}

ListReduceRebindResult MaybeRebindListReduceLambda(ClientContext &context, idx_t depth,
                                                   const vector<LogicalType> &function_child_types,
                                                   bind_lambda_function_t bind_lambda_function,
                                                   BindResult &bind_lambda_result,
                                                   const bind_lambda_expression_t &bind_lambda_expression,
                                                   optional_ptr<BindLambdaContext> bind_lambda_context,
                                                   const unique_ptr<ParsedExpression> &lambda_expr_copy) {
	ListReduceRebindResult result;
	result.capture_child_types = function_child_types;

	const bool has_initial = function_child_types.size() == 3;
	auto &bound_lambda_expr = bind_lambda_result.expression->Cast<BoundLambdaExpression>();
	const auto &lambda_return_type = bound_lambda_expr.LambdaExpr()->GetReturnType();

	auto list_child_type = function_child_types[0];
	if (list_child_type.id() != LogicalTypeId::SQLNULL && list_child_type.id() != LogicalTypeId::UNKNOWN) {
		if (list_child_type.id() == LogicalTypeId::ARRAY) {
			list_child_type = ArrayType::GetChildType(list_child_type);
		} else if (list_child_type.id() == LogicalTypeId::LIST) {
			list_child_type = ListType::GetChildType(list_child_type);
		} else {
			throw BinderException("The first argument must be a list or array type");
		}
	}

	LogicalType accumulator_type;
	if (has_initial) {
		const auto &initial_type = function_child_types[2];
		if (!LogicalType::TryGetMaxLogicalType(context, initial_type, lambda_return_type, accumulator_type)) {
			throw BinderException("No common super type between initial value type %s and lambda return type %s",
			                      initial_type.ToString(), lambda_return_type.ToString());
		}
	} else {
		if (!LogicalType::TryGetMaxLogicalType(context, list_child_type, lambda_return_type, accumulator_type)) {
			throw BinderException("No common super type between list element type %s and lambda return type %s",
			                      list_child_type.ToString(), lambda_return_type.ToString());
		}
	}

	if (has_initial) {
		D_ASSERT(lambda_expr_copy);
		auto rebind_child_types = function_child_types;
		rebind_child_types[2] = accumulator_type;

		auto lambda_expr_rebind_copy = lambda_expr_copy->Copy();
		auto &lambda_expr_rebind = lambda_expr_rebind_copy->Cast<LambdaExpression>();
		bind_lambda_result = bind_lambda_expression(lambda_expr_rebind, depth, rebind_child_types,
		                                            &bind_lambda_function, bind_lambda_context);
		if (bind_lambda_result.HasError()) {
			bind_lambda_result.error.Throw();
		}

		CastDecimalLambdaToAccumulatorType(context, bind_lambda_result, accumulator_type);
		result.did_rebind = true;
		result.capture_child_types = std::move(rebind_child_types);
	} else if (accumulator_type != list_child_type) {
		if (!lambda_expr_copy) {
			throw InternalException("list_reduce lambda copy is missing for rebind");
		}
		auto &lambda_expr_rebind = lambda_expr_copy->Cast<LambdaExpression>();
		result.override_bind_lambda = &ListReduceBindLambdaOverride;
		result.override_bind_lambda_context = make_uniq<ListReduceBindLambdaContext>(accumulator_type);
		bind_lambda_result = bind_lambda_expression(lambda_expr_rebind, depth, function_child_types,
		                                            &result.override_bind_lambda, *result.override_bind_lambda_context);
		if (bind_lambda_result.HasError()) {
			bind_lambda_result.error.Throw();
		}
		CastDecimalLambdaToAccumulatorType(context, bind_lambda_result, accumulator_type);
		result.did_rebind = true;
		result.override_accumulator_type_storage = accumulator_type;
		result.override_has_accumulator_type = true;
	}

	return result;
}

} // namespace

BindResult ExpressionBinder::TryBindLambdaOrJson(FunctionExpression &function, idx_t depth, CatalogEntry &func,
                                                 const LambdaSyntaxType syntax_type) {
	if (syntax_type == LambdaSyntaxType::LAMBDA_KEYWORD) {
		// lambda x: x + 1 syntax.
		return BindLambdaFunction(function, func.Cast<ScalarFunctionCatalogEntry>(), depth);
	}

	auto setting = Settings::Get<LambdaSyntaxSetting>(context);
	bool invalid_syntax = setting != LambdaSyntax::ENABLE_SINGLE_ARROW && syntax_type == LambdaSyntaxType::SINGLE_ARROW;
	// only arrow lambdas reach this point
	bool warn_deprecated_syntax = setting == LambdaSyntax::DEFAULT;
	const string msg = "Deprecated lambda arrow (->) detected. Please transition to the new lambda syntax, "
	                   "i.e.., lambda x, i: x + i, before DuckDB's next release.\n"
	                   "Use SET lambda_syntax='ENABLE_SINGLE_ARROW' to revert to the deprecated behavior.\n"
	                   "For more information, see https://duckdb.org/docs/current/sql/functions/lambda.html.";

	BindResult lambda_bind_result;
	ErrorData error;
	try {
		lambda_bind_result = BindLambdaFunction(function, func.Cast<ScalarFunctionCatalogEntry>(), depth);
	} catch (const std::exception &ex) {
		error = ErrorData(ex);
	}

	if (error.HasError() && error.Type() == ExceptionType::PARAMETER_NOT_RESOLVED && invalid_syntax) {
		ErrorData deprecation_error(ExceptionType::BINDER, msg);
		deprecation_error.Throw();
	} else if (error.HasError()) {
		error.Throw();
	}

	if (!lambda_bind_result.HasError()) {
		if (!invalid_syntax) {
			if (warn_deprecated_syntax) {
				DUCKDB_LOG_WARNING(context, msg);
			}
			return lambda_bind_result;
		}
		throw BinderException(msg);
	}

	// The lambda reading failed - the JSON reading binds the same nodes from scratch. Any child the
	// lambda attempt already bound is therefore bound twice, which repeats that bind's side effects
	// (correlated-column registration, subquery planning). Only the deprecated arrow syntax is
	// ambiguous enough to reach here, so this goes away with it rather than being worked around.
	auto json_bind_result = BindFunction(function, func.Cast<ScalarFunctionCatalogEntry>(), depth);
	if (!json_bind_result.HasError()) {
		return json_bind_result;
	}

	return BindResult("failed to bind function, either: " + lambda_bind_result.error.RawMessage() +
	                  "\n"
	                  " or: " +
	                  json_bind_result.error.RawMessage());
}

CatalogEntry &ExpressionBinder::BindFunction(FunctionExpression &function) {
	QueryErrorContext error_context(function.GetQueryLocation());

	ColumnQualifier qualifier(binder);
	auto func = qualifier.QualifyFunction(function);
	if (!func) {
		// function was not found - check if we this is a table function (to throw a more helpful error message)
		EntryLookupInfo table_function_lookup(CatalogType::TABLE_FUNCTION_ENTRY, function.GetQualifiedName(),
		                                      error_context);
		auto table_func = GetCatalogEntry(table_function_lookup, OnEntryNotFound::RETURN_NULL);
		if (table_func) {
			throw BinderException(function,
			                      "Function %s is a table function but it was used as a scalar function. This "
			                      "function has to be called in a FROM clause (similar to a table).",
			                      function.FunctionName());
		}
		// not a table function - rebind to throw an error
		EntryLookupInfo function_lookup(CatalogType::SCALAR_FUNCTION_ENTRY, function.GetQualifiedName(), error_context);
		func = GetCatalogEntry(function_lookup, OnEntryNotFound::THROW_EXCEPTION);
	}
	return *func;
}

//! Rewrite a call that resolved to the built-in has_error into the HAS_ERROR operator and bind that.
//! The operator binds its argument itself (per-row error capture, TRY transport for nested subqueries),
//! so the rewrite must happen before the children are bound.
BindResult ExpressionBinder::BindHasErrorFunction(FunctionExpression &function, idx_t depth,
                                                  unique_ptr<ParsedExpression> &expr_ptr) {
	if (function.GetArguments().size() != 1) {
		throw BinderException(function, "Wrong number of arguments provided to HAS_ERROR expression");
	}
	auto &argument = function.GetArgumentsMutable()[0];
	if (argument.HasName()) {
		throw BinderException(function, "Named arguments are not supported in HAS_ERROR expression");
	}
	auto op = make_uniq<OperatorExpression>(ExpressionType::OPERATOR_HAS_ERROR);
	op->GetChildrenMutable().push_back(std::move(argument.GetExpressionMutable()));
	op->SetQueryLocation(function.GetQueryLocation());
	op->SetAlias(function.GetAlias());
	expr_ptr = std::move(op);
	return BindExpression(expr_ptr->Cast<OperatorExpression>(), depth);
}

BindResult ExpressionBinder::BindExpression(FunctionExpression &function, idx_t depth,
                                            unique_ptr<ParsedExpression> &expr_ptr) {
	auto &func = BindFunction(function);

	switch (func.type) {
	case CatalogType::AGGREGATE_FUNCTION_ENTRY:
	case CatalogType::WINDOW_FUNCTION_ENTRY:
		break;
	default:
		if (function.Distinct() || function.Filter() || !function.OrderBy()->orders.empty()) {
			throw InvalidInputException("Function %s is a %s. \"DISTINCT\", \"FILTER\", and \"ORDER BY\" are only "
			                            "applicable to window and aggregate functions.",
			                            function.FunctionName(), CatalogTypeToString(func.type));
		}
		break;
	}

	switch (func.type) {
	case CatalogType::SCALAR_FUNCTION_ENTRY: {
		if (func.internal && func.name == "has_error" && func.ParentCatalog().IsSystemCatalog()) {
			// the built-in has_error binds as the HAS_ERROR operator, whose executor captures errors
			// raised while evaluating the argument; a user function of the same name resolves normally
			return BindHasErrorFunction(function, depth, expr_ptr);
		}
		auto child = function.IsLambdaFunction();
		if (child) {
			auto syntax_type = child->Cast<LambdaExpression>().GetLambdaSyntaxType();
			return TryBindLambdaOrJson(function, depth, func, syntax_type);
		}
		return BindFunction(function, func.Cast<ScalarFunctionCatalogEntry>(), depth);
	}
	case CatalogType::MACRO_ENTRY:
		// macro function - a prepared statement must be re-bound when the macro changes
		if (!func.ParentCatalog().IsSystemCatalog()) {
			binder.GetStatementProperties().RegisterDBRead(func.ParentCatalog(), context);
		}
		return BindMacro(function, func.Cast<ScalarMacroCatalogEntry>(), depth, expr_ptr);
	case CatalogType::AGGREGATE_FUNCTION_ENTRY: {
		// an aggregate belongs to the innermost query level in which one of its arguments resolves a
		// column of that level, so that the level collecting it is the one the arguments refer to
		if (!binder.GetEnclosingScopes().empty()) {
			auto owner = ResolveAggregateOwner(function, 0);
			if (owner.IsValid() && owner.GetIndex() != 0) {
				return BindAggregateInEnclosingScope(function, owner.GetIndex(), depth, expr_ptr);
			}
		}
		return BindAggregate(function, func.Cast<AggregateFunctionCatalogEntry>(), depth);
	}
	case CatalogType::WINDOW_FUNCTION_ENTRY:
		// window function
		return BindWindow(function, func.Cast<WindowFunctionCatalogEntry>(), depth);
	default:
		throw InvalidInputException("Unsupported catalog type when binding function");
	}
}

BindResult ExpressionBinder::BindAggregateInEnclosingScope(FunctionExpression &aggregate, idx_t owner, idx_t depth,
                                                           unique_ptr<ParsedExpression> &expr_ptr) {
	// Bind a copy at each candidate scope. Binding rewrites the nodes it manages to bind - a column
	// reference becomes the qualified form of the scope that owns it - so a failed attempt must not be
	// handed to the next scope out. Moving the original aside also keeps `aggregate` valid throughout,
	// since it aliases the node `expr_ptr` held on entry.
	auto original = std::move(expr_ptr);
	ErrorData bind_error;
	for (optional_idx scope = owner; scope.IsValid(); scope = ResolveAggregateOwner(aggregate, scope.GetIndex() + 1)) {
		expr_ptr = original->Copy();
		auto result = DispatchToScope(scope.GetIndex(), expr_ptr, depth);
		if (!result.HasError()) {
			return result;
		}
		if (!bind_error.HasError()) {
			// report the innermost owner's error: that is the level the arguments actually name
			bind_error = std::move(result.error);
		}
	}
	expr_ptr = std::move(original);
	return BindResult(std::move(bind_error));
}

BindResult ExpressionBinder::BindFunction(FunctionExpression &function, ScalarFunctionCatalogEntry &func, idx_t depth) {
	// bind the children of the function expression
	ErrorData error;

	// bind each child
	vector<unique_ptr<Expression>> bound_children;
	for (idx_t i = 0; i < function.GetArguments().size(); i++) {
		bound_children.push_back(BindChild(function.GetArgumentsMutable()[i].GetExpressionMutable(), depth, error));
	}

	if (error.HasError()) {
		return BindResult(std::move(error));
	}
	if (binder.GetBindingMode() == BindingMode::EXTRACT_NAMES ||
	    binder.GetBindingMode() == BindingMode::EXTRACT_QUALIFIED_NAMES) {
		return BindResult(make_uniq<BoundConstantExpression>(Value(LogicalType::SQLNULL)));
	}

	// all children bound successfully - collect them (with their explicit names, if any) into the full argument list.
	// The positional/named split (and, for capturing functions, the alias capture) is resolved later per candidate
	// overload.
	vector<pair<Identifier, unique_ptr<Expression>>> arguments;
	// for legacy calls with names: fallback arguments that keep the names, used if binding positionally fails
	vector<pair<Identifier, unique_ptr<Expression>>> named_arguments;
	arguments.reserve(function.GetArguments().size());
	for (idx_t i = 0; i < function.GetArgumentsMutable().size(); i++) {
		auto &arg = function.GetArgumentsMutable()[i];
		auto bound_arg = std::move(bound_children[i]);

		// legacy function calls cannot have named arguments, so we ignore the names of the arguments during binding
		// and pass them all positionally. We do alias them by their name though, so that alias-capturing functions
		// (e.g. struct_pack) still work and so that re-serializing to the old format can match arguments by name.
		// Only override the alias when the argument actually carries a name, otherwise we would clobber the
		// display alias the binding assigned (e.g. clearing a column reference's name to its raw binding).
		if (!arg.GetName().empty()) {
			bound_arg->SetAlias(arg.GetName());
		}
		if (function.IsLegacyFunctionCall()) {
			if (!named_arguments.empty() || !arg.GetName().empty()) {
				// first named argument: copy the unnamed arguments bound so far
				for (idx_t prev = named_arguments.size(); prev < arguments.size(); prev++) {
					named_arguments.emplace_back(Identifier(), arguments[prev].second->Copy());
				}
				named_arguments.emplace_back(arg.GetName(), bound_arg->Copy());
			}
			arguments.emplace_back(string(), std::move(bound_arg));
		} else {
			arguments.emplace_back(arg.GetName(), std::move(bound_arg));
		}
	}

	FunctionBinder function_binder(binder);
	auto result = function_binder.BindScalarFunction(func, std::move(arguments), error, function.IsOperator(), &binder);
	if (!result && !named_arguments.empty()) {
		// a legacy call that does not bind positionally may use names the function declares (e.g. **kwargs)
		ErrorData named_error;
		result = function_binder.BindScalarFunction(func, std::move(named_arguments), named_error,
		                                            function.IsOperator(), &binder);
	}
	if (!result) {
		error.AddQueryLocation(function);
		error.Throw();
	}
	if (result->GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &bound_function = result->Cast<BoundFunctionExpression>();
		if (bound_function.Function().GetStability() == FunctionStability::CONSISTENT_WITHIN_QUERY) {
			binder.SetAlwaysRequireRebind();
		}
	}
	return BindResult(std::move(result));
}

BindResult ExpressionBinder::BindLambdaFunction(FunctionExpression &function, ScalarFunctionCatalogEntry &func,
                                                idx_t depth) {
	// get the callback function for the lambda parameter types
	auto &scalar_function = *func.functions.functions.front();
	auto bind_lambda_function = scalar_function.GetBindLambdaCallback();
	if (!bind_lambda_function) {
		return BindResult("This scalar function does not support lambdas!");
	}

	auto &args = function.GetArgumentsMutable();

	// list lambda functions use the existing (list, lambda) shape; invoke is the only lambda
	// function that accepts the lambda expression as the first argument.
	const idx_t lambda_expr_idx = func.name == "invoke" ? 0 : 1;
	if (args.size() <= lambda_expr_idx ||
	    args[lambda_expr_idx].GetExpression().GetExpressionClass() != ExpressionClass::LAMBDA) {
		return BindResult("This scalar function requires a lambda expression!");
	}

	vector<LogicalType> function_child_types;
	vector<unique_ptr<Expression>> bound_children(function.GetArguments().size());
	ErrorData error;

	for (idx_t i = 0; i < function.GetArguments().size(); i++) {
		if (i == lambda_expr_idx) {
			function_child_types.push_back(LogicalType::LAMBDA);
			continue;
		}

		if (args[i].GetExpression().GetExpressionClass() == ExpressionClass::LAMBDA) {
			return BindResult("No function matches the given name and argument types: '" + function.ToString() +
			                  "'. You might need to add explicit type casts.");
		}

		bound_children[i] = BindChild(function.GetArgumentsMutable()[i].GetExpressionMutable(), depth, error);
		if (error.HasError()) {
			return BindResult(std::move(error));
		}

		function_child_types.push_back(bound_children[i]->GetReturnType());
	}

	if (lambda_expr_idx == 1) {
		// get the logical type of the children of the list
		auto &list_child = *bound_children[0];
		if (list_child.GetReturnType().id() != LogicalTypeId::LIST &&
		    list_child.GetReturnType().id() != LogicalTypeId::ARRAY &&
		    list_child.GetReturnType().id() != LogicalTypeId::SQLNULL &&
		    list_child.GetReturnType().id() != LogicalTypeId::UNKNOWN) {
			return BindResult("Invalid LIST argument during lambda function binding!");
		}
	}

	// bind the lambda parameter
	auto &lambda_expr = args[lambda_expr_idx].GetExpressionMutable()->Cast<LambdaExpression>();

	unique_ptr<ParsedExpression> lambda_expr_copy;
	// the aliases of list_reduce (e.g. reduce) are separate catalog entries
	const bool is_list_reduce = func.name == "list_reduce" || func.alias_of == "list_reduce";
	if (is_list_reduce) {
		// We have to make this copy before binding in case we have to double-bind
		lambda_expr_copy = lambda_expr.Copy();
	}

	BindResult bind_lambda_result =
	    BindExpression(lambda_expr, depth, function_child_types, &bind_lambda_function, nullptr);
	bind_lambda_function_t override_bind_lambda_storage = nullptr;
	optional_ptr<bind_lambda_function_t> capture_bind_lambda = &bind_lambda_function;
	unique_ptr<BindLambdaContext> override_bind_lambda_context;
	auto capture_child_types = function_child_types;

	if (bind_lambda_result.HasError()) {
		return BindResult(bind_lambda_result.error);
	}
	auto bind_lambda_expression = [&](LambdaExpression &expr, idx_t lambda_depth, const vector<LogicalType> &types,
	                                  optional_ptr<bind_lambda_function_t> bind_lambda_fn,
	                                  optional_ptr<BindLambdaContext> bind_lambda_context) {
		return BindExpression(expr, lambda_depth, types, bind_lambda_fn, bind_lambda_context);
	};

	if (is_list_reduce) {
		// list_reduce requires binding the lambda function a second time as there is a cyclical dependency between its
		// acc parameter type and its return type. This is resolved by initially binding it with acc = initial, and then
		// rebinding it with acc = return type
		auto rebind_result =
		    MaybeRebindListReduceLambda(context, depth, function_child_types, bind_lambda_function, bind_lambda_result,
		                                bind_lambda_expression, nullptr, lambda_expr_copy);
		if (rebind_result.did_rebind) {
			capture_child_types = std::move(rebind_result.capture_child_types);
			override_bind_lambda_context = std::move(rebind_result.override_bind_lambda_context);
			if (rebind_result.override_bind_lambda) {
				override_bind_lambda_storage = rebind_result.override_bind_lambda;
				capture_bind_lambda = &override_bind_lambda_storage;
			} else {
				capture_bind_lambda = &bind_lambda_function;
			}
		}
	}

	// successfully bound: keep the bound lambda alongside the other bound children
	auto alias = args[lambda_expr_idx].GetExpression().GetAlias();
	bind_lambda_result.expression->SetAlias(alias);

	bound_children[lambda_expr_idx] = std::move(bind_lambda_result.expression);

	if (binder.GetBindingMode() == BindingMode::EXTRACT_NAMES) {
		return BindResult(make_uniq<BoundConstantExpression>(Value(LogicalType::SQLNULL)));
	}

	// all children bound successfully
	// extract the children and types
	auto children = std::move(bound_children);

	// capture the (lambda) columns
	auto &bound_lambda_expr = children[lambda_expr_idx]->Cast<BoundLambdaExpression>();
	CaptureLambdaColumns(bound_lambda_expr, bound_lambda_expr.LambdaExprMutable(), capture_bind_lambda,
	                     override_bind_lambda_context, capture_child_types);

	FunctionBinder function_binder(binder);
	unique_ptr<Expression> result =
	    function_binder.BindScalarFunction(func, std::move(children), error, function.IsOperator(), &binder);
	if (!result) {
		error.AddQueryLocation(function);
		error.Throw();
	}

	auto &bound_function_expr = result->Cast<BoundFunctionExpression>();

	// the lambda expression stays in the children, so that the argument positions do not shift.
	// it has no runtime value - the expression executor fills its slot with a constant placeholder
	auto &bound_lambda = bound_function_expr.GetChildrenMutable()[lambda_expr_idx]->Cast<BoundLambdaExpression>();

	// push back (in reverse order) any nested lambda parameters so that we can later use them in the lambda
	// expression (rhs). This happens after we bound the lambda expression of this depth. So it is relevant for
	// correctly binding lambdas one level 'out'. Therefore, the current parameter count does not matter here.
	// The parameters of the lambdas enclosing an aggregate are not passed: the aggregate is computed outside them
	if (lambda_bindings) {
		idx_t offset = 0;

		for (idx_t i = lambda_bindings->size(); i > inaccessible_lambda_binding_count; i--) {
			auto &binding = (*lambda_bindings)[i - 1];
			auto &column_names = binding.GetColumnNames();
			auto &column_types = binding.GetColumnTypes();
			D_ASSERT(column_names.size() == column_types.size());

			for (idx_t column_idx = column_names.size(); column_idx > 0; column_idx--) {
				auto bound_lambda_param = make_uniq<BoundReferenceExpression>(column_names[column_idx - 1],
				                                                              column_types[column_idx - 1], offset);
				offset++;
				bound_function_expr.GetChildrenMutable().push_back(std::move(bound_lambda_param));
			}
		}
	}

	// push back the captures into the children vector
	for (auto &capture : bound_lambda.CapturesMutable()) {
		bound_function_expr.GetChildrenMutable().push_back(std::move(capture));
	}
	// the lambda stays in the children, so it must not hold on to the moved-from capture slots
	bound_lambda.CapturesMutable().clear();

	return BindResult(std::move(result));
}

BindResult ExpressionBinder::BindAggregate(FunctionExpression &expr, AggregateFunctionCatalogEntry &function,
                                           idx_t depth) {
	return BindUnsupportedExpression(expr, depth, UnsupportedAggregateMessage());
}

BindResult ExpressionBinder::BindWindow(FunctionExpression &expr, WindowFunctionCatalogEntry &function, idx_t depth) {
	return BindUnsupportedExpression(expr, depth, UnsupportedWindowMessage());
}

BindResult ExpressionBinder::BindUnnest(FunctionExpression &expr, idx_t depth, bool root_expression) {
	return BindUnsupportedExpression(expr, depth, UnsupportedUnnestMessage());
}

void ExpressionBinder::ThrowIfUnnestInLambda(const ColumnBinding &column_binding) {
}

string ExpressionBinder::UnsupportedAggregateMessage() {
	return "Aggregate functions are not supported here";
}

string ExpressionBinder::UnsupportedWindowMessage() {
	return "Window functions are not supported here";
}

string ExpressionBinder::UnsupportedUnnestMessage() {
	return "UNNEST not supported here";
}

optional_ptr<CatalogEntry> ExpressionBinder::GetCatalogEntry(const Identifier &catalog, const Identifier &schema,
                                                             const EntryLookupInfo &lookup_info,
                                                             OnEntryNotFound on_entry_not_found) {
	return binder.GetCatalogEntry(catalog, schema, lookup_info, on_entry_not_found);
}

optional_ptr<CatalogEntry> ExpressionBinder::GetCatalogEntry(const EntryLookupInfo &lookup_info,
                                                             OnEntryNotFound on_entry_not_found) {
	return binder.GetCatalogEntry(lookup_info, on_entry_not_found);
}

} // namespace duckdb
