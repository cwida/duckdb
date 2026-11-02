#include "duckdb/function/function_binder.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/aggregate_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/scalar_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/window_function_catalog_entry.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/type_visitor.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/aggregate_function.hpp"
#include "duckdb/function/cast/cast_function_set.hpp"
#include "duckdb/function/cast_rules.hpp"
#include "duckdb/function/type_constructor.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/expression_binder.hpp"
#include "duckdb/planner/binder.hpp"

namespace duckdb {

FunctionBinder::FunctionBinder(ClientContext &context_p) : binder(nullptr), context(context_p) {
}
FunctionBinder::FunctionBinder(Binder &binder_p) : binder(&binder_p), context(binder_p.context) {
}

// Split the full (maybe-named) argument list into positional types + named (name, type) pairs.
// Returns false if a positional argument follows a named one, which is not allowed.
static bool TrySplitArgumentTypes(const vector<pair<Identifier, unique_ptr<Expression>>> &arguments,
                                  vector<LogicalType> &positional, vector<pair<Identifier, LogicalType>> &named) {
	for (auto &arg : arguments) {
		auto type = ExpressionBinder::GetExpressionReturnType(*arg.second);
		if (!arg.first.empty()) {
			named.emplace_back(arg.first, std::move(type));
			continue;
		}
		if (named.empty()) {
			positional.push_back(std::move(type));
			continue;
		}
		// a positional argument cannot follow a named one
		return false;
	}
	return true;
}

// Split the full (maybe-named) argument list into the positional + named (keyword) children.
static auto SplitArguments(vector<pair<Identifier, unique_ptr<Expression>>> arguments)
    -> pair<vector<unique_ptr<Expression>>, vector<pair<Identifier, unique_ptr<Expression>>>> {
	vector<unique_ptr<Expression>> regular_args;
	vector<pair<Identifier, unique_ptr<Expression>>> keyword_args;

	for (auto &arg : arguments) {
		if (!arg.first.empty()) {
			keyword_args.push_back(std::move(arg));
			continue;
		}
		if (keyword_args.empty()) {
			regular_args.push_back(std::move(arg.second));
			continue;
		}
		// Defensive: a positional argument following a named one should already have been rejected during binding
		// (see TrySplitArgumentTypes / BindFunctionWithImplicitNaming).
		throw BinderException(arg.second->GetQueryLocation(),
		                      "Positional argument '%s' cannot follow named arguments in a function call.",
		                      arg.second->ToString());
	}

	return {std::move(regular_args), std::move(keyword_args)};
}

static int64_t ImplicitCastCost(optional_ptr<ClientContext> context, const LogicalType &source,
                                const LogicalType &target) {
	if (context) {
		return CastFunctionSet::ImplicitCastCost(*context, source, target);
	}
	// without a context we can only consider the built-in cast rules - no registered casts are reachable
	return CastRules::ImplicitCast(source, target);
}

optional_idx FunctionOverloads::Cost(optional_ptr<ClientContext> context, const SimpleFunction &func,
                                     const vector<LogicalType> &arguments,
                                     const vector<pair<Identifier, LogicalType>> &named_arguments) {
	const auto &sig = func.GetSignature();

	// Compute total number of arguments passed
	const auto received_arg_count = static_cast<idx_t>(arguments.size() + named_arguments.size());

	// And the maximum number of arguments the function can accept
	const auto positional_count = sig.GetPositionalParameterCount();
	const auto args_param = sig.GetArgs();
	const auto kwargs_param = sig.GetKwargs();

	idx_t maximum_arg_count = NumericLimits<idx_t>::Maximum();
	if (!args_param && !kwargs_param) {
		maximum_arg_count = sig.GetParameterCount();
	}

	// Every parameter without a default has to be filled by the call, either from its position or by its name.
	// Counting the arguments instead would let a named argument stand in for a required positional parameter, so
	// that f(a, *, opt := NULL) matched a call f(opt := 1) that cannot fill "a" just as well as f(*, opt := NULL).
	for (idx_t i = 0; i < sig.GetParameterCount(); i++) {
		auto &param = sig.GetParameter(i);
		if (param.IsVariadic() || param.HasDefaultValue()) {
			continue;
		}
		if (param.AcceptsPosition() && i < arguments.size()) {
			continue;
		}
		bool filled = false;
		if (param.AcceptsName()) {
			for (auto &named_arg : named_arguments) {
				if (named_arg.first == param.GetName()) {
					filled = true;
					break;
				}
			}
		}
		if (!filled) {
			// This parameter cannot be filled by the call, so this function cannot be a match.
			return optional_idx();
		}
	}

	if (received_arg_count > maximum_arg_count) {
		// We have more arguments than the function can take, so this function cannot be a match.
		return optional_idx();
	}

	if (arguments.size() > positional_count && !args_param) {
		// There is no "*args" to receive the extra positional arguments
		return optional_idx();
	}

	idx_t cost = 0;
	bool has_parameter = false;

	for (idx_t i = 0; i < arguments.size(); i++) {
		if (arguments[i].id() == LogicalTypeId::UNKNOWN) {
			has_parameter = true;
			continue;
		}

		auto &arg_type = i < positional_count ? sig.GetParameter(i).GetType() : args_param->GetType();

		int64_t cast_cost = ImplicitCastCost(context, arguments[i], arg_type);
		if (cast_cost >= 0) {
			// we can implicitly cast, add the cost to the total cost
			cost += idx_t(cast_cost);
		} else {
			// we can't implicitly cast: throw an error
			return optional_idx();
		}
	}

	// Now check the named arguments
	for (idx_t i = 0; i < named_arguments.size(); i++) {
		auto &named_arg = named_arguments[i];
		auto opt_param_idx = sig.GetParameterIndexByName(named_arg.first);

		if (!opt_param_idx.IsValid()) {
			if (!kwargs_param) {
				// no parameter with this name, and no "**kwargs" to receive it
				return optional_idx();
			}
			// the options of "**kwargs" do not select the overload - they are checked once it is chosen
			int64_t cast_cost = ImplicitCastCost(context, named_arg.second, kwargs_param->GetType());
			if (cast_cost >= 0) {
				// we can implicitly cast, add the cost to the total cost
				cost += static_cast<idx_t>(cast_cost);
			} else {
				// we can't implicitly cast: throw an error
				return optional_idx();
			}
		} else {
			// This parameter might technically be invalid, in that it may point to a parameter that has already
			// been filled by a positional argument. However, we will catch that later when we actually bind the
			// argument expressions to construct the bound function expression. At that point we will also have more
			// context so we can give a better error message.
			const auto param_idx = opt_param_idx.GetIndex();
			auto &param = sig.GetParameter(param_idx);

			if (param.GetKind() == FunctionParameterKind::STANDARD && param_idx < arguments.size()) {
				// If already covered by a positional argument, skip the cost check here.
				continue;
			}

			int64_t cast_cost = ImplicitCastCost(context, named_arg.second, param.GetType());
			if (cast_cost >= 0) {
				cost += idx_t(cast_cost);
			} else {
				// A named argument reaches its parameter only by an implicit cast, exactly like a positional one
				return optional_idx();
			}
		}
	}

	if (has_parameter) {
		// all arguments are implicitly castable and there is a parameter - return 0 as cost
		return 0;
	}
	return cost;
}

template <class T>
vector<idx_t> FunctionOverloads::Candidates(optional_ptr<ClientContext> context, const Identifier &name,
                                            const FunctionSet<T> &functions, const vector<LogicalType> &arguments,
                                            const vector<pair<Identifier, LogicalType>> &named_arguments,
                                            ErrorData &error) {
	optional_idx best_function;
	idx_t lowest_cost = NumericLimits<idx_t>::Maximum();
	vector<idx_t> candidate_functions;
	for (idx_t f_idx = 0; f_idx < functions.functions.size(); f_idx++) {
		auto &func = *functions.functions[f_idx];
		// check the arguments of the function
		auto bind_cost = Cost(context, func, arguments, named_arguments);
		if (!bind_cost.IsValid()) {
			// auto casting was not possible
			continue;
		}
		auto cost = bind_cost.GetIndex();
		if (cost == lowest_cost) {
			candidate_functions.push_back(f_idx);
			continue;
		}
		if (cost > lowest_cost) {
			continue;
		}
		candidate_functions.clear();
		lowest_cost = cost;
		best_function = f_idx;
	}
	if (!best_function.IsValid()) {
		// no matching function was found, throw an error
		vector<string> candidates;
		Identifier catalog_name;
		Identifier schema_name;
		for (auto &f : functions.functions) {
			if (catalog_name.empty() && !f->GetCatalogName().empty()) {
				catalog_name = f->GetCatalogName();
			}
			if (schema_name.empty() && !f->GetSchemaName().empty()) {
				schema_name = f->GetSchemaName();
			}
			candidates.push_back(f->ToString());
		}
		error = ErrorData(BinderException::NoMatchingFunction(catalog_name, schema_name, name, arguments,
		                                                      named_arguments, candidates));
		return candidate_functions;
	}
	candidate_functions.push_back(best_function.GetIndex());
	return candidate_functions;
}

template <class T>
static optional_idx
MultipleCandidateException(const Identifier &catalog_name, const Identifier &schema_name, const Identifier &name,
                           const FunctionSet<T> &functions, const vector<idx_t> &candidate_functions,
                           const vector<LogicalType> &arguments,
                           const vector<pair<Identifier, LogicalType>> &named_arguments, ErrorData &error) {
	D_ASSERT(functions.functions.size() > 1);
	// there are multiple possible function definitions
	// throw an exception explaining which overloads are there
	string call_str = Function::CallToString(catalog_name, schema_name, name, arguments, named_arguments);
	string candidate_str;
	for (auto &conf : candidate_functions) {
		const auto &f = functions.GetFunctionByOffset(conf);
		candidate_str += "\t" + f->ToString() + "\n";
	}
	error = ErrorData(
	    ExceptionType::BINDER,
	    StringUtil::Format("Could not choose a best candidate function for the function call \"%s\". In order to "
	                       "select one, please add explicit type casts.\n\tCandidate functions:\n%s",
	                       call_str, candidate_str));
	return optional_idx();
}

template <class T>
optional_idx FunctionOverloads::Select(optional_ptr<ClientContext> context, const Identifier &name,
                                       const FunctionSet<T> &functions, const vector<LogicalType> &arguments,
                                       const vector<pair<Identifier, LogicalType>> &named_arguments, ErrorData &error) {
	auto candidate_functions = Candidates(context, name, functions, arguments, named_arguments, error);
	if (candidate_functions.empty()) {
		// No candidates, return an invalid index.
		return optional_idx();
	}
	if (candidate_functions.size() > 1) {
		// Multiple candidates, check if there are any unknown arguments.
		for (auto &arg_type : arguments) {
			if (arg_type.IsUnknown()) {
				// We cannot resolve the parameters to a function.
				throw ParameterNotResolvedException();
			}
		}
		auto catalog_name = functions.functions.size() > 0 ? functions.functions[0]->GetCatalogName() : Identifier();
		auto schema_name = functions.functions.size() > 0 ? functions.functions[0]->GetSchemaName() : Identifier();
		return MultipleCandidateException(catalog_name, schema_name, name, functions, candidate_functions, arguments,
		                                  named_arguments, error);
	}
	return candidate_functions[0];
}

template <class T>
optional_idx FunctionBinder::BindFunctionFromArguments(const Identifier &name, const FunctionSet<T> &functions,
                                                       const vector<LogicalType> &arguments,
                                                       const vector<pair<Identifier, LogicalType>> &named_arguments,
                                                       ErrorData &error) {
	return FunctionOverloads::Select(context, name, functions, arguments, named_arguments, error);
}

//! Type constructors select overloads outside of FunctionBinder, as they can be bound without a ClientContext
template optional_idx FunctionOverloads::Select<TypeConstructor>(optional_ptr<ClientContext>, const Identifier &,
                                                                 const FunctionSet<TypeConstructor> &,
                                                                 const vector<LogicalType> &,
                                                                 const vector<pair<Identifier, LogicalType>> &,
                                                                 ErrorData &);

template <class T>
static bool AnyOverloadSupportsImplicitArgumentNames(const FunctionSet<T> &functions) {
	for (auto &func : functions.functions) {
		if (func->GetProperties().GetCaptureArgumentAliases()) {
			return true;
		}
	}
	return false;
}

static optional_idx PositionalAfterNamedArgumentError(const vector<pair<Identifier, unique_ptr<Expression>>> &arguments,
                                                      ErrorData &error) {
	bool seen_named = false;
	for (const auto &[name, expr] : arguments) {
		if (!name.empty()) {
			seen_named = true;
			continue;
		}
		if (seen_named) {
			error = ErrorData(BinderException(
			    expr->GetQueryLocation(), "Positional argument '%s' cannot follow named arguments in function call.",
			    expr->ToString()));
			return optional_idx();
		}
	}
	throw InternalException("ThrowPositionalAfterNamed called without a positional-after-named argument");
}

template <class T>
optional_idx FunctionBinder::BindFunctionFromArguments(const Identifier &name, const FunctionSet<T> &functions,
                                                       vector<pair<Identifier, unique_ptr<Expression>>> &arguments,
                                                       ErrorData &error) {
	// First, attempt a regular bind, splitting the arguments into positional + named just once for all overloads.
	vector<LogicalType> positional;
	vector<pair<Identifier, LogicalType>> named;

	// Does this call have a valid positional -> named argument shape?
	const auto is_valid_call = TrySplitArgumentTypes(arguments, positional, named);
	if (is_valid_call) {
		// Yes. Let's try to bind
		const auto result = BindFunctionFromArguments(name, functions, positional, named, error);

		// We failed binding - does any other overload support implicit argument names?
		// If not, this is a hard failure.
		if (result.IsValid() || !AnyOverloadSupportsImplicitArgumentNames(functions)) {
			return result;
		}
	}

	// Either a positional argument follows a named one, or no overload takes the positional arguments.
	// Check if there is any overload that supports implicit argument names.
	if (AnyOverloadSupportsImplicitArgumentNames(functions)) {
		// If so, we can attempt to salvage the call by implicitly naming the positional arguments and retrying again
		for (auto &[name, expr] : arguments) {
			if (name.empty()) {
				name = expr->GetAlias();
			}
		}

		positional.clear();
		named.clear();

		if (TrySplitArgumentTypes(arguments, positional, named)) {
			ErrorData retry_error;
			auto result = BindFunctionFromArguments(name, functions, positional, named, retry_error);
			if (result.IsValid() || !is_valid_call) {
				error = std::move(retry_error);
				return result;
			}
		}
	}
	if (is_valid_call) {
		// the call only failed to match an overload, which is reported in the error
		return optional_idx();
	}

	// No overload could rescue the positional-after-named call, give a clear error.
	return PositionalAfterNamedArgumentError(arguments, error);
}

optional_idx FunctionBinder::BindFunction(const Identifier &name, const ScalarFunctionSet &functions,
                                          const vector<LogicalType> &regular_args,
                                          const vector<pair<Identifier, LogicalType>> &keyword_args, ErrorData &error) {
	return BindFunctionFromArguments(name, functions, regular_args, keyword_args, error);
}

optional_idx FunctionBinder::BindFunction(const Identifier &name, const AggregateFunctionSet &functions,
                                          const vector<LogicalType> &regular_args,
                                          const vector<pair<Identifier, LogicalType>> &keyword_args, ErrorData &error) {
	return BindFunctionFromArguments(name, functions, regular_args, keyword_args, error);
}

optional_idx FunctionBinder::BindFunction(const Identifier &name, const WindowFunctionSet &functions,
                                          const vector<LogicalType> &regular_args,
                                          const vector<pair<Identifier, LogicalType>> &keyword_args, ErrorData &error) {
	return BindFunctionFromArguments(name, functions, regular_args, keyword_args, error);
}

optional_idx FunctionBinder::BindFunction(const Identifier &name, const TableFunctionSet &functions,
                                          const vector<LogicalType> &regular_args,
                                          const vector<pair<Identifier, LogicalType>> &keyword_args, ErrorData &error) {
	return BindFunctionFromArguments(name, functions, regular_args, keyword_args, error);
}

//! Reject a named argument that no overload of the set accepts, naming the options that were available. Overload
//! selection would otherwise report only that nothing matched the call, which hides which name was wrong
template <class T>
static void VerifyNamedArgumentsAccepted(const Identifier &name, const FunctionSet<T> &functions,
                                         const vector<pair<Identifier, unique_ptr<Expression>>> &named_arguments) {
	for (auto &named_argument : named_arguments) {
		bool accepted = false;
		for (idx_t i = 0; i < functions.functions.size() && !accepted; i++) {
			auto &signature = functions.functions[i]->GetSignature();
			// the options of a "**kwargs" are checked once the overload is chosen
			accepted =
			    signature.GetParameterIndexByName(named_argument.first).IsValid() || signature.GetKwargs() != nullptr;
		}
		if (accepted) {
			continue;
		}
		// list every option any overload accepts, in name order
		map<string, string> candidates;
		for (idx_t i = 0; i < functions.functions.size(); i++) {
			for (auto &param : functions.functions[i]->GetSignature().GetParameters()) {
				if (!param.IsVariadic() && param.HasDefaultValue()) {
					candidates[param.GetName().GetIdentifierName()] = param.GetType().ToString();
				}
			}
		}
		string candidate_list;
		for (auto &candidate : candidates) {
			candidate_list += "    " + candidate.first + " " + candidate.second + "\n";
		}
		throw BinderException("Invalid named parameter %s for function %s\n%s", named_argument.first,
		                      name.GetIdentifierName(),
		                      candidate_list.empty() ? "Function does not accept any named parameters."
		                                             : "Candidates:\n" + candidate_list);
	}
}

enum class LogicalTypeComparisonResult : uint8_t { IDENTICAL_TYPE, TARGET_IS_ANY, DIFFERENT_TYPES };

static LogicalTypeComparisonResult RequiresCast(const LogicalType &source_type, const LogicalType &target_type) {
	if (target_type.id() == LogicalTypeId::ANY) {
		return LogicalTypeComparisonResult::TARGET_IS_ANY;
	}
	if (source_type == target_type) {
		return LogicalTypeComparisonResult::IDENTICAL_TYPE;
	}
	if (source_type.id() == LogicalTypeId::LIST && target_type.id() == LogicalTypeId::LIST) {
		return RequiresCast(ListType::GetChildType(source_type), ListType::GetChildType(target_type));
	}
	if (source_type.id() == LogicalTypeId::ARRAY && target_type.id() == LogicalTypeId::ARRAY) {
		auto target_size = ArrayType::GetSize(target_type);
		if (target_size != 0 && target_size != ArrayType::GetSize(source_type)) {
			return LogicalTypeComparisonResult::DIFFERENT_TYPES;
		}
		return RequiresCast(ArrayType::GetChildType(source_type), ArrayType::GetChildType(target_type));
	}
	return LogicalTypeComparisonResult::DIFFERENT_TYPES;
}

//! Resolve any ANY in the target type by substituting the matching part of the source, so we never cast a value to a
//! type that still contains ANY - ANY has no physical representation, so building such a vector throws. This happens
//! e.g. when an ARRAY argument fills a LIST(ANY) parameter (the file list of the multi-file readers).
static LogicalType ResolveAnyInParameterType(const LogicalType &target, const LogicalType &source) {
	if (target.id() == LogicalTypeId::ANY) {
		return source;
	}
	if (target.id() == LogicalTypeId::LIST &&
	    (source.id() == LogicalTypeId::LIST || source.id() == LogicalTypeId::ARRAY)) {
		auto &source_child =
		    source.id() == LogicalTypeId::LIST ? ListType::GetChildType(source) : ArrayType::GetChildType(source);
		return LogicalType::LIST(ResolveAnyInParameterType(ListType::GetChildType(target), source_child));
	}
	return target;
}

Value FunctionBinder::CastToParameterType(ClientContext &context, Value value, const LogicalType &parameter_type) {
	if (RequiresCast(value.type(), parameter_type) == LogicalTypeComparisonResult::DIFFERENT_TYPES) {
		return value.CastAs(context, ResolveAnyInParameterType(parameter_type, value.type()));
	}
	return value;
}

//! Fold an argument to the constant it was bound to. A constant expression carries its value directly - evaluating
//! one is both wasteful and impossible for the types that have no vector representation, such as TABLE
static Value FoldArgument(ClientContext &context, Expression &expr) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
		return expr.Cast<BoundConstantExpression>().GetValue();
	}
	return ExpressionExecutor::EvaluateScalar(context, expr, true);
}

//! Fold an argument to a constant and cast it to the type of the parameter it fills
static Value PlaceArgument(ClientContext &context, Expression &expr, const LogicalType &target_type) {
	if (target_type.id() == LogicalTypeId::TABLE) {
		// A TABLE parameter is filled by the subquery itself rather than by a value, so its slot holds a plain
		// NULL. A NULL of type TABLE would not do: TABLE has no physical type, so such a value cannot be read
		// back when the plan is deserialized.
		return Value();
	}
	return FunctionBinder::CastToParameterType(context, FoldArgument(context, expr), target_type);
}

//! Fold an option to a constant and cast it to its type like an explicit cast - overload selection never checked it.
//! A failed cast names the option, as the cast alone cannot tell which one it was
static Value PlaceOption(ClientContext &context, Expression &expr, const TypedKwarg &option) {
	auto value = FoldArgument(context, expr);
	if (RequiresCast(value.type(), option.type) != LogicalTypeComparisonResult::DIFFERENT_TYPES) {
		return value;
	}
	string error_message;
	auto result = value.TryCastAs(context, option.type, &error_message);
	if (!result) {
		throw InvalidInputException(expr, "Could not cast value %s to named parameter %s of type %s: %s",
		                            value.ToSQLString(), option.name, option.type.ToString(), error_message);
	}
	return std::move(*result);
}

//! Place the arguments of a call onto the parameters of the chosen overload: fold each to a constant and cast it to
//! the type that parameter declares. Every parameter that accepts a position gets its slot in "parameters", whether
//! the caller passed it by position, by name or not at all, so "range(1, col1 := 5)" reaches the callback exactly as
//! "range(1, 5)" does. Only keyword-only and "**kwargs" arguments are handed back in "named_parameters", keyed by the
//! name the caller wrote - or, for an option "**kwargs" declares, by the name of the option, cast to its type. A
//! parameter the call leaves out receives its default; an option the call leaves out is not passed. Overload selection
//! has already established that every parameter's cast is one the implicit rules allow. An option takes no part in
//! selection, so it is cast to its type like an explicit cast.
template <class T>
static void PlaceArguments(ClientContext &context, const T &function,
                           vector<unique_ptr<Expression>> &positional_arguments,
                           vector<pair<Identifier, unique_ptr<Expression>>> &named_arguments, vector<Value> &parameters,
                           named_argument_map_t &named_parameters) {
	auto &signature = function.GetSignature();
	const auto positional_count = signature.GetPositionalParameterCount();
	const auto passed_count = positional_arguments.size();

	// the positional parameters the caller filled by name, by parameter index
	vector<optional_ptr<Expression>> named_slots(positional_count);
	identifier_set_t seen_names;
	for (auto &named_argument : named_arguments) {
		auto &argument_name = named_argument.first;
		if (!seen_names.insert(argument_name).second) {
			throw BinderException("Duplicate named argument %s for function %s", argument_name,
			                      function.GetName().GetIdentifierName());
		}
		auto param_idx = signature.GetParameterIndexByName(argument_name);
		if (!param_idx.IsValid()) {
			auto option_schema = signature.GetTypedKwargs();
			if (!option_schema) {
				// received by "**kwargs" - cast to its type, as overload selection checked, unless that is ANY
				auto &kwargs_type = signature.GetKwargs()->GetType();
				named_parameters.insert(
				    make_pair(argument_name, PlaceArgument(context, *named_argument.second, kwargs_type)));
				continue;
			}
			auto &expr = *named_argument.second;
			auto option_ptr = option_schema->Find(argument_name);
			if (!option_ptr) {
				// only the options that resemble the name - listing every option buries the one that was meant
				const double similarity_threshold = 0.8;
				auto lower_name = StringUtil::Lower(argument_name.GetIdentifierName());
				vector<pair<string, double>> scores;
				for (auto &option_name : option_schema->GetNames()) {
					auto score = StringUtil::SimilarityRating(option_name.GetIdentifierName(), lower_name);
					if (score >= similarity_threshold) {
						scores.emplace_back(option_name.GetIdentifierName(), score);
					}
				}
				auto similar = StringUtil::TopNStrings(scores, 3, similarity_threshold);
				throw BinderException(expr.GetQueryLocation(), "Invalid named parameter %s for function %s%s",
				                      argument_name, function.GetName().GetIdentifierName(),
				                      StringUtil::CandidatesMessage(similar, "Did you mean"));
			}
			auto &option = *option_ptr;
			if (named_parameters.find(option.name) != named_parameters.end()) {
				throw BinderException(named_argument.second->GetQueryLocation(),
				                      "Named parameter %s was passed more than once in function call to %s",
				                      option.name, function.GetName().GetIdentifierName());
			}
			named_parameters.insert(make_pair(option.name, PlaceOption(context, *named_argument.second, option)));
			continue;
		}
		auto &param = signature.GetParameter(param_idx.GetIndex());
		if (!param.AcceptsPosition()) {
			named_parameters.insert(
			    make_pair(argument_name, PlaceArgument(context, *named_argument.second, param.GetType())));
			continue;
		}
		if (param_idx.GetIndex() < passed_count) {
			throw BinderException(named_argument.second->GetQueryLocation(),
			                      "Named argument '%s' cannot be used for parameter '%s' because it has already "
			                      "been provided as a positional argument in function call to '%s'",
			                      named_argument.second->ToString(), argument_name,
			                      function.GetName().GetIdentifierName());
		}
		named_slots[param_idx.GetIndex()] = *named_argument.second;
	}

	for (idx_t i = 0; i < positional_count; i++) {
		auto &param = signature.GetParameter(i);
		if (i < passed_count) {
			parameters.push_back(PlaceArgument(context, *positional_arguments[i], param.GetType()));
		} else if (named_slots[i]) {
			parameters.push_back(PlaceArgument(context, *named_slots[i], param.GetType()));
		} else if (param.HasDefaultValue()) {
			parameters.push_back(
			    FunctionBinder::CastToParameterType(context, *param.GetDefaultValue(), param.GetType()));
		} else {
			// overload selection only picks an overload whose required parameters the call fills
			throw InternalException("Missing value for parameter %s in function call to %s", param.GetName(),
			                        function.GetName().GetIdentifierName());
		}
	}
	if (passed_count > positional_count) {
		// overload selection only picks an overload with "*args" for surplus positional arguments
		auto &args_type = signature.GetArgs()->GetType();
		for (idx_t i = positional_count; i < passed_count; i++) {
			parameters.push_back(PlaceArgument(context, *positional_arguments[i], args_type));
		}
	}
	signature.FillNamedDefaults(context, named_parameters);
}

optional_idx FunctionBinder::BindFunction(const Identifier &name, const TableFunctionSet &functions,
                                          vector<unique_ptr<Expression>> &positional_arguments,
                                          vector<pair<Identifier, unique_ptr<Expression>>> &named_arguments,
                                          vector<Value> &parameters, named_argument_map_t &named_parameters,
                                          ErrorData &error) {
	VerifyNamedArgumentsAccepted(name, functions, named_arguments);
	auto entry = BindFunction(name, functions, positional_arguments, named_arguments, error);
	if (!entry.IsValid()) {
		return entry;
	}
	PlaceArguments(context, *functions.GetFunctionByOffset(entry.GetIndex()), positional_arguments, named_arguments,
	               parameters, named_parameters);
	return entry;
}

optional_idx FunctionBinder::BindTableInOutFunction(const Identifier &name, const TableFunctionSet &functions,
                                                    const vector<LogicalType> &input_types, ErrorData &error) {
	return BindFunctionFromArguments(name, functions, input_types, {}, error);
}

optional_idx FunctionBinder::BindFunction(const Identifier &name, const PragmaFunctionSet &functions,
                                          vector<unique_ptr<Expression>> &positional_arguments,
                                          vector<pair<Identifier, unique_ptr<Expression>>> &named_arguments,
                                          vector<Value> &parameters, named_argument_map_t &named_parameters,
                                          ErrorData &error) {
	VerifyNamedArgumentsAccepted(name, functions, named_arguments);
	auto [args, kwargs] = GetArgumentsFromExpressions(positional_arguments, named_arguments);
	auto entry = BindFunctionFromArguments(name, functions, args, kwargs, error);
	if (!entry.IsValid()) {
		error.Throw();
	}
	PlaceArguments(context, *functions.GetFunctionByOffset(entry.GetIndex()), positional_arguments, named_arguments,
	               parameters, named_parameters);
	return entry;
}

pair<vector<LogicalType>, vector<pair<Identifier, LogicalType>>>
FunctionBinder::GetArgumentsFromExpressions(const vector<unique_ptr<Expression>> &regular_args,
                                            const vector<pair<Identifier, unique_ptr<Expression>>> &keyword_args) {
	vector<LogicalType> regular_arg_types;
	vector<pair<Identifier, LogicalType>> keyword_arg_types;

	for (auto &arg : regular_args) {
		regular_arg_types.push_back(ExpressionBinder::GetExpressionReturnType(*arg));
	}
	for (auto &kwarg : keyword_args) {
		keyword_arg_types.emplace_back(kwarg.first, ExpressionBinder::GetExpressionReturnType(*kwarg.second));
	}
	return {std::move(regular_arg_types), std::move(keyword_arg_types)};
}

optional_idx FunctionBinder::BindFunction(const Identifier &name, const ScalarFunctionSet &functions,
                                          const vector<unique_ptr<Expression>> &regular_args,
                                          const vector<pair<Identifier, unique_ptr<Expression>>> &keyword_args,
                                          ErrorData &error) {
	auto [args, kwargs] = GetArgumentsFromExpressions(regular_args, keyword_args);
	return BindFunctionFromArguments(name, functions, args, kwargs, error);
}

optional_idx FunctionBinder::BindFunction(const Identifier &name, const AggregateFunctionSet &functions,
                                          const vector<unique_ptr<Expression>> &regular_args,
                                          const vector<pair<Identifier, unique_ptr<Expression>>> &keyword_args,
                                          ErrorData &error) {
	auto [args, kwargs] = GetArgumentsFromExpressions(regular_args, keyword_args);
	return BindFunctionFromArguments(name, functions, args, kwargs, error);
}

optional_idx FunctionBinder::BindFunction(const Identifier &name, const TableFunctionSet &functions,
                                          const vector<unique_ptr<Expression>> &regular_args,
                                          const vector<pair<Identifier, unique_ptr<Expression>>> &keyword_args,
                                          ErrorData &error) {
	auto [args, kwargs] = GetArgumentsFromExpressions(regular_args, keyword_args);
	return BindFunctionFromArguments(name, functions, args, kwargs, error);
}

static bool TypeRequiresPrepare(const LogicalType &type) {
	if (type.id() == LogicalTypeId::ANY) {
		return true;
	}
	if (type.id() == LogicalTypeId::LIST) {
		return TypeRequiresPrepare(ListType::GetChildType(type));
	}
	return false;
}

static LogicalType PrepareTypeForCastRecursive(const LogicalType &type) {
	if (type.id() == LogicalTypeId::ANY) {
		return AnyType::GetTargetType(type);
	}
	if (type.id() == LogicalTypeId::LIST) {
		return LogicalType::LIST(PrepareTypeForCastRecursive(ListType::GetChildType(type)));
	}
	return type;
}

static void PrepareTypeForCast(LogicalType &type) {
	if (!TypeRequiresPrepare(type)) {
		return;
	}
	type = PrepareTypeForCastRecursive(type);
}

//! Whether the type has no concrete type information (e.g. DECIMAL without width / scale) - ANY, UNKNOWN and INVALID
//! are excluded, as casts to these have special meaning (e.g. for prepared statement parameters)
static bool IsIncompleteCastTarget(const LogicalType &type) {
	if (type.IsComplete()) {
		return false;
	}
	return !TypeVisitor::Contains(type, [](const LogicalType &child) {
		return child.id() == LogicalTypeId::ANY || child.id() == LogicalTypeId::UNKNOWN ||
		       child.id() == LogicalTypeId::INVALID;
	});
}

void FunctionBinder::CastToFunctionArguments(BoundSimpleFunction &function, vector<unique_ptr<Expression>> &children) {
	for (auto &arg : function.GetArguments()) {
		PrepareTypeForCast(arg);
	}

	// Varargs should be expanded by this point
	// If not, the function has somehow added more argument expressions during binding, which is not allowed.
	// There is one exception, count(*) which can be invoked internally with fewer arguments than the function
	// definition. That should be fixed separately though.
	D_ASSERT(children.size() <= function.GetArguments().size());

	for (idx_t i = 0; i < children.size(); i++) {
		auto &target_type = function.GetArguments()[i];
		if (target_type.id() == LogicalTypeId::STRING_LITERAL || target_type.id() == LogicalTypeId::INTEGER_LITERAL) {
			throw InternalException(
			    "Function %s returned a STRING_LITERAL or INTEGER_LITERAL type - return an explicit type instead",
			    function.GetName());
		}
		target_type.Verify();
		// don't cast lambda children, their slot only ever holds a placeholder
		if (children[i]->GetReturnType().id() == LogicalTypeId::LAMBDA) {
			continue;
		}
		const bool target_has_any = TypeVisitor::Contains(target_type, LogicalTypeId::ANY);
		if (target_has_any && children[i]->GetReturnType().id() == LogicalTypeId::ARRAY &&
		    target_type.id() == LogicalTypeId::LIST) {
			// an array passed to a LIST(ANY) parameter is cast to a list of its child type
			children[i] = BoundCastExpression::AddArrayCastToList(context, std::move(children[i]));
		}
		// check if the type of child matches the type of function argument
		// if not we need to add a cast
		auto cast_result = RequiresCast(children[i]->GetReturnType(), target_type);
		// except for one special case: if the function accepts ANY argument
		// in that case we don't add a cast
		if (cast_result == LogicalTypeComparisonResult::DIFFERENT_TYPES) {
			if (target_has_any && children[i]->GetReturnType().id() != LogicalTypeId::UNKNOWN) {
				// a type that is not concrete cannot be cast to (e.g. a NULL or a VARIANT passed to LIST(ANY)) - the
				// function has to handle the argument as-is. Unresolved parameters are still assigned the type.
				continue;
			}
			if (IsIncompleteCastTarget(target_type)) {
				throw InternalException("Function %s has incomplete argument type %s - concrete argument types must "
				                        "be resolved in the resolve_types callback",
				                        function.GetName(), target_type);
			}
			children[i] = BoundCastExpression::AddCastToType(context, std::move(children[i]), target_type);
		}
	}
}

unique_ptr<Expression> FunctionBinder::BindScalarFunction(const Identifier &schema, const Identifier &name,
                                                          vector<unique_ptr<Expression>> children, ErrorData &error,
                                                          bool is_operator, optional_ptr<Binder> binder) {
	// bind the function
	auto &function = Catalog::GetSystemCatalog(context).GetEntry<ScalarFunctionCatalogEntry>(
	    context, QualifiedName(Catalog::GetSystemCatalog(context).GetName(), schema, name));
	D_ASSERT(function.type == CatalogType::SCALAR_FUNCTION_ENTRY);
	return BindScalarFunction(function, std::move(children), error, is_operator, binder);
}

unique_ptr<Expression> FunctionBinder::BindScalarFunction(const Identifier &schema, const Identifier &name,
                                                          vector<unique_ptr<Expression>> children, bool is_operator,
                                                          optional_ptr<Binder> binder) {
	ErrorData error;
	auto result = BindScalarFunction(schema, name, std::move(children), error, is_operator, binder);
	if (!result) {
		error.Throw();
	}
	return result;
}

unique_ptr<Expression> FunctionBinder::BindScalarFunction(const ScalarFunctionCatalogEntry &func,
                                                          vector<unique_ptr<Expression>> children, ErrorData &error,
                                                          bool is_operator, optional_ptr<Binder> binder) {
	vector<pair<Identifier, unique_ptr<Expression>>> arguments;
	arguments.reserve(children.size());
	for (auto &child : children) {
		arguments.emplace_back(string(), std::move(child));
	}
	return BindScalarFunction(func, std::move(arguments), error, is_operator, binder);
}

//! Returns true if any argument is an untyped NULL, i.e. has type SQLNULL
static bool HasUntypedNullArgument(const vector<unique_ptr<Expression>> &regular_args,
                                   const vector<pair<Identifier, unique_ptr<Expression>>> &keyword_args) {
	for (auto &child : regular_args) {
		if (child->GetReturnType() == LogicalTypeId::SQLNULL) {
			return true;
		}
	}
	for (auto &child : keyword_args) {
		if (child.second->GetReturnType() == LogicalTypeId::SQLNULL) {
			return true;
		}
	}
	return false;
}

//! Evaluates a constant argument and returns true if it is NULL. A non-NULL result replaces the argument with a
//! constant: the argument has been evaluated anyway, and keeping the result means that it is not evaluated again by
//! every enclosing function call - which made binding nested constant expressions quadratic in their depth.
static bool FoldConstantArgument(ClientContext &context, unique_ptr<Expression> &child) {
	if (child->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
		return child->Cast<BoundConstantExpression>().GetValue().IsNull();
	}
	// a lambda has no value of its own, the function it is passed to executes its body
	if (child->GetExpressionClass() == ExpressionClass::BOUND_LAMBDA || !child->IsFoldable()) {
		return false;
	}
	Value result;
	if (!ExpressionExecutor::TryEvaluateScalar(context, *child, result)) {
		// the argument cannot be evaluated: leave it in place so that the error surfaces at execution
		return false;
	}
	if (result.IsNull()) {
		return true;
	}
	auto constant = make_uniq<BoundConstantExpression>(std::move(result));
	constant->SetAlias(child->GetAlias());
	constant->SetQueryLocation(child->GetQueryLocation());
	child = std::move(constant);
	return false;
}

//! Returns true if any argument is a constant expression that evaluates to NULL. Arguments that evaluate to a non-NULL
//! value are folded to constants along the way.
static bool HasNullConstantArgument(ClientContext &context, vector<unique_ptr<Expression>> &regular_args,
                                    vector<pair<Identifier, unique_ptr<Expression>>> &keyword_args) {
	for (auto &child : regular_args) {
		if (FoldConstantArgument(context, child)) {
			return true;
		}
	}
	for (auto &child : keyword_args) {
		if (FoldConstantArgument(context, child.second)) {
			return true;
		}
	}
	return false;
}

static void PropagateReturnCollation(BoundSimpleFunction &bound_function, const FunctionProperties &props,
                                     const vector<unique_ptr<Expression>> &children, const string &pushed_collation);

//! The type of the NULL constant a function call is folded to - SQLNULL if the return type could not be determined
static LogicalType NullReturnType(const LogicalType &return_type) {
	return return_type.IsComplete() ? return_type : LogicalType::SQLNULL;
}

unique_ptr<Expression> FunctionBinder::BindScalarFunction(const ScalarFunctionCatalogEntry &func,
                                                          vector<pair<Identifier, unique_ptr<Expression>>> arguments,
                                                          ErrorData &error, bool is_operator,
                                                          optional_ptr<Binder> binder) {
	// select the best matching overload (this may name positional arguments by their alias for functions that opt
	// into implicit argument naming, e.g. struct_pack/row)
	auto best_function = BindFunctionFromArguments(func.name, func.functions, arguments, error);
	if (!best_function.IsValid()) {
		return nullptr;
	}

	// found a matching function!
	auto selected_function = func.functions.GetFunctionByOffset(best_function.GetIndex());
	auto &bound_function = *selected_function;

	// now that the overload is fixed, split the arguments into their final positional/named children
	auto [regular_args, keyword_args] = SplitArguments(std::move(arguments));

	// If any of the parameters are NULL, the function will just be replaced with a NULL constant.
	// Untyped NULLs never go through type resolution. For typed NULL constants, resolve the function's types only
	// when needed to determine the return type. Preserve result collation without invoking bind callbacks.
	if (bound_function.GetNullHandling() == FunctionNullHandling::DEFAULT_NULL_HANDLING) {
		const auto &return_type = bound_function.GetReturnType();
		if (HasUntypedNullArgument(regular_args, keyword_args)) {
			return make_uniq<BoundConstantExpression>(Value(NullReturnType(return_type)));
		}
		if (HasNullConstantArgument(context, regular_args, keyword_args)) {
			if (return_type.IsComplete()) {
				BoundScalarFunction null_function(std::move(selected_function));
				for (auto &child : keyword_args) {
					regular_args.push_back(std::move(child.second));
				}
				PropagateReturnCollation(null_function, null_function.GetProperties(), regular_args, string());
				return make_uniq<BoundConstantExpression>(Value(null_function.GetReturnType()));
			}
			auto resolved = ResolveScalarFunctionTypes(std::move(selected_function), regular_args, keyword_args);
			PropagateReturnCollation(resolved.bound_function, resolved.bound_function.GetProperties(), regular_args,
			                         resolved.pushed_collation);
			return make_uniq<BoundConstantExpression>(Value(NullReturnType(resolved.bound_function.GetReturnType())));
		}
	}
	return BindScalarFunction(std::move(selected_function), std::move(regular_args), std::move(keyword_args),
	                          is_operator, binder);
}

static bool RequiresCollationPropagation(const LogicalType &type) {
	return type.id() == LogicalTypeId::VARCHAR && !type.HasAlias();
}

struct FunctionCollations {
	string collation;
	identifier_map_t<unique_ptr<FunctionCollations>> fields;

	FunctionCollations &GetField(const Identifier &name) {
		auto &field = fields[name];
		if (!field) {
			field = make_uniq<FunctionCollations>();
		}
		return *field;
	}
};

static Identifier CollationFieldName(const LogicalType &type, idx_t index) {
	return StructType::IsUnnamed(type) ? Identifier(to_string(index)) : StructType::GetChildName(type, index);
}

//! Recursively extracts the collation of a (possibly nested) type, e.g. the element collation of a LIST(VARCHAR).
//! target is the resolved argument type and may include fields from other arguments.
static void ExtractCollationFromType(const LogicalType &source_type, const LogicalType &target,
                                     FunctionCollations &collations) {
	// Leave collation of aliased types to their registered callbacks.
	if (source_type.HasAlias() || target.HasAlias()) {
		return;
	}
	switch (source_type.id()) {
	case LogicalTypeId::VARCHAR: {
		auto collation = StringType::GetCollation(source_type);
		if (collations.collation.empty()) {
			collations.collation = std::move(collation);
		} else if (!collation.empty() && collations.collation != collation) {
			throw BinderException("Cannot combine types with different collation");
		}
		break;
	}
	case LogicalTypeId::LIST:
	case LogicalTypeId::ARRAY: {
		auto &source_child = source_type.id() == LogicalTypeId::LIST ? ListType::GetChildType(source_type)
		                                                             : ArrayType::GetChildType(source_type);
		auto &target_child = target.id() == LogicalTypeId::LIST    ? ListType::GetChildType(target)
		                     : target.id() == LogicalTypeId::ARRAY ? ArrayType::GetChildType(target)
		                                                           : source_child;
		ExtractCollationFromType(source_child, target_child, collations);
		break;
	}
	case LogicalTypeId::STRUCT:
	case LogicalTypeId::TUPLE: {
		auto &target_type = StructType::IsStruct(target) && target.HasParameters() ? target : source_type;
		auto &source_children = StructType::GetChildTypes(source_type);
		auto &target_children = StructType::GetChildTypes(target_type);

		// If BOTH source and target_type are named, we can use named indexing, else we use positional.
		if (!StructType::IsUnnamed(source_type) && !StructType::IsUnnamed(target_type)) {
			// Named indexing, so build a map to map source fields to their position
			identifier_map_t<idx_t> src_child_indices;
			for (idx_t i = 0; i < source_children.size(); i++) {
				src_child_indices.emplace(source_children[i].first, i);
			}

			for (idx_t i = 0; i < target_children.size(); i++) {
				// Find index in the source fields, or skip if non existent.
				auto entry = src_child_indices.find(target_children[i].first);
				if (entry == src_child_indices.end()) {
					continue;
				}
				const auto child_index = entry->second;
				auto &field = collations.GetField(CollationFieldName(target_type, i));
				ExtractCollationFromType(source_children[child_index].second, target_children[i].second, field);
			}
		} else {
			// Use positional indexing, as not both source_type and target_type are named.
			for (idx_t i = 0; i < MinValue(target_children.size(), source_children.size()); i++) {
				auto &field = collations.GetField(CollationFieldName(target_type, i));
				ExtractCollationFromType(source_children[i].second, target_children[i].second, field);
			}
		}
		break;
	}
	default:
		break;
	}
}

//! Returns a copy of the type with the collected collations applied to its VARCHAR fields.
static LogicalType ApplyCollationToType(const LogicalType &type, const FunctionCollations &collations) {
	// Rebuilding an aliased type would discard its alias and bypass its collation callbacks
	if (type.HasAlias()) {
		return type;
	}
	switch (type.id()) {
	case LogicalTypeId::VARCHAR:
		return collations.collation.empty() ? type : LogicalType::VARCHAR_COLLATION(collations.collation);
	case LogicalTypeId::LIST:
		return LogicalType::LIST(ApplyCollationToType(ListType::GetChildType(type), collations));
	case LogicalTypeId::ARRAY:
		return LogicalType::ARRAY(ApplyCollationToType(ArrayType::GetChildType(type), collations),
		                          ArrayType::GetSize(type));
	case LogicalTypeId::STRUCT:
	case LogicalTypeId::TUPLE: {
		auto children = StructType::GetChildTypes(type);
		for (idx_t i = 0; i < children.size(); i++) {
			auto entry = collations.fields.find(CollationFieldName(type, i));
			if (entry != collations.fields.end()) {
				children[i].second = ApplyCollationToType(children[i].second, *entry->second);
			}
		}
		return StructType::IsUnnamed(type) ? LogicalType::TUPLE(std::move(children))
		                                   : LogicalType::STRUCT(std::move(children));
	}
	default:
		return type;
	}
}

static string ExtractCollation(const vector<unique_ptr<Expression>> &children) {
	string collation;
	for (auto &arg : children) {
		if (!RequiresCollationPropagation(arg->GetReturnType())) {
			// not a varchar column
			continue;
		}
		auto child_collation = StringType::GetCollation(arg->GetReturnType());
		if (collation.empty()) {
			collation = child_collation;
		} else if (!child_collation.empty() && collation != child_collation) {
			throw BinderException("Cannot combine types with different collation!");
		}
	}
	return collation;
}

//! Pushes the collations of the arguments into the children, returns the collation to propagate to the return type
static string PushCollations(ClientContext &context, BoundSimpleFunction &bound_function,
                             vector<unique_ptr<Expression>> &children, CollationType type) {
	FunctionCollations collations;
	auto &argument_types = bound_function.GetArguments();
	for (idx_t i = 0; i < children.size(); i++) {
		auto &source_type = children[i]->GetReturnType();
		auto &target_type = argument_types[i].IsComplete() ? argument_types[i] : source_type;
		ExtractCollationFromType(source_type, target_type, collations);
	}
	if (collations.collation.empty() && collations.fields.empty()) {
		// no collation to push
		return string();
	}
	// push collations to the children
	for (idx_t i = 0; i < children.size(); i++) {
		auto &arg = children[i];
		auto &arg_type = argument_types[i];
		auto &target_type = arg_type.IsComplete() ? arg_type : arg->GetReturnType();
		// apply the collations to the (possibly nested) varchar leaves of the argument type
		auto collated_type = ApplyCollationToType(target_type, collations);
		arg = BoundCastExpression::AddCastToType(context, std::move(arg), collated_type);
		if (RequiresCollationPropagation(arg->GetReturnType())) {
			// if this is a varchar type - propagate the collation
			arg->SetReturnType(collated_type);
		}
		// now push the actual collation handling
		if (ExpressionBinder::PushCollation(context, arg, collated_type, type)) {
			// Normalization can change field types, so do not cast back to the original argument type.
			arg_type = arg->GetReturnType();
		}
	}
	return collations.collation;
}

//! Pushes collations into the children (if required), returns the collation to propagate to the return type
static string PushArgumentCollations(ClientContext &context, BoundSimpleFunction &bound_function,
                                     const FunctionProperties &props, vector<unique_ptr<Expression>> &children) {
	switch (props.GetCollationHandling()) {
	case FunctionCollationHandling::IGNORE_COLLATIONS:
	case FunctionCollationHandling::PROPAGATE_COLLATIONS:
		return string();
	case FunctionCollationHandling::PUSH_COMBINABLE_COLLATIONS:
		return PushCollations(context, bound_function, children, CollationType::COMBINABLE_COLLATIONS);
	case FunctionCollationHandling::PUSH_DISTINCT_COLLATIONS:
		throw InternalException("PUSH_DISTINCT_COLLATIONS is only supported for aggregate functions");
	default:
		throw InternalException("Unrecognized collation handling");
	}
}

//! Propagates the collation of the arguments to the return type of the function (if it returns a varchar)
static void PropagateReturnCollation(BoundSimpleFunction &bound_function, const FunctionProperties &props,
                                     const vector<unique_ptr<Expression>> &children, const string &pushed_collation) {
	if (!RequiresCollationPropagation(bound_function.GetReturnType())) {
		return;
	}
	auto collation = props.GetCollationHandling() == FunctionCollationHandling::PROPAGATE_COLLATIONS
	                     ? ExtractCollation(children)
	                     : pushed_collation;
	if (collation.empty()) {
		return;
	}
	bound_function.SetReturnType(LogicalType::VARCHAR_COLLATION(std::move(collation)));
}

static void InferTemplateType(ClientContext &context, const LogicalType &source, const LogicalType &target,
                              case_insensitive_map_t<vector<LogicalType>> &bindings, const Expression &current_expr,
                              const BoundSimpleFunction &function) {
	if (target.id() == LogicalTypeId::UNKNOWN || target.id() == LogicalTypeId::SQLNULL) {
		// If the actual type is unknown, we cannot infer anything more.
		// Therefore, we map all remaining templates in the source to UNKNOWN or SQLNULL, if not already inferred to
		// something else

		// This might seem a bit strange, why not just not set the binding and error out later when we try to substitute
		// all templates? Well, this is how bindings for most nested functions already work, they simply propagate the
		// UNKNOWN/SQLNULL. The binder will later check for UNKNOWN/SQLNULL in return types and if it finds one, insert
		// a dummy cast to INT32 so that the function can be executed without errors (and just return NULLs).

		TypeVisitor::Contains(source, [&](const LogicalType &child) {
			if (child.id() == LogicalTypeId::TEMPLATE) {
				const auto index = TemplateType::GetName(child);
				if (bindings.find(index) == bindings.end()) {
					// not found, add the binding
					bindings[index] = {target.id()};
				}
			}
			return false; // continue visiting
		});
		return;
	}

	// If the source is a template type, we bind it, or try to unify its existing binding with the target type.
	if (source.id() == LogicalTypeId::TEMPLATE) {
		const auto &index = TemplateType::GetName(source);
		auto it = bindings.find(index);
		if (it == bindings.end()) {
			// not found, add the binding
			bindings[index] = {target};
			return;
		}
		if (it->second.back() == target) {
			// already bound to the same type
			return;
		}

		// Try to unify (promote) the type candidates
		LogicalType result;
		if (LogicalType::TryGetMaxLogicalType(context, it->second.back(), target, result)) {
			// Type unification was successful
			if (it->second.back() != result) {
				// update the binding
				it->second.push_back(target);
				it->second.push_back(std::move(result)); // Push the new promoted type
			}
			return;
		}

		// If we reach here, it means the types are incompatible
		string msg =
		    StringUtil::Format("Cannot deduce template type '%s' in function: '%s'\nType '%s' was inferred to be:\n",
		                       TemplateType::GetName(source), function.ToString(), TemplateType::GetName(source));
		const auto &steps = it->second;

		for (idx_t i = 0; i < steps.size(); i += 2) {
			if (i == 0) {
				// Normalize the first step to ensure it is a valid type
				msg += StringUtil::Format(" - '%s', from first occurrence\n", steps[i].ToString());
			} else {
				msg += StringUtil::Format(" - '%s', by promoting '%s' + '%s'\n", steps[i].ToString(),
				                          steps[i - 2].ToString(), steps[i - 1]);
			}
		}
		msg += StringUtil::Format(" - '%s', which is incompatible with previously inferred type!", target.ToString());
		throw BinderException(current_expr.GetQueryLocation(), msg);
	}

	// Otherwise, recurse downwards into nested types, and try to infer nested type members
	// This only works if the source and target types are completely defined (excluding templates),
	// i.e. they have aux info.
	if (!(source.IsNested() && target.IsNested() && source.HasParameters() && target.HasParameters())) {
		return;
	}

	switch (source.id()) {
	case LogicalTypeId::LIST:
	case LogicalTypeId::ARRAY: {
		if ((source.id() == LogicalTypeId::ARRAY || source.id() == LogicalTypeId::LIST) &&
		    (target.id() == LogicalTypeId::LIST || target.id() == LogicalTypeId::ARRAY)) {
			const auto &source_child =
			    source.id() == LogicalTypeId::LIST ? ListType::GetChildType(source) : ArrayType::GetChildType(source);
			const auto &target_child =
			    target.id() == LogicalTypeId::LIST ? ListType::GetChildType(target) : ArrayType::GetChildType(target);
			InferTemplateType(context, source_child, target_child, bindings, current_expr, function);
		}
	} break;
	case LogicalTypeId::MAP: {
		// Map is only implicitly castable to map, so we only need to handle this case here/
		if (target.id() == LogicalTypeId::MAP) {
			const auto &source_key = MapType::KeyType(source);
			const auto &source_val = MapType::ValueType(source);
			const auto &target_key = MapType::KeyType(target);
			const auto &target_val = MapType::ValueType(target);

			InferTemplateType(context, source_key, target_key, bindings, current_expr, function);
			InferTemplateType(context, source_val, target_val, bindings, current_expr, function);
		}
	} break;
	case LogicalTypeId::UNION: {
		// TODO: Support union types with template member types.
		throw NotImplementedException("Union types cannot infer templated member types yet!");
	} break;
	case LogicalTypeId::STRUCT:
	case LogicalTypeId::TUPLE: {
		// Structs are only implicitly castable to structs, so we only need to handle this case here.
		if (StructType::IsStruct(target) && StructType::IsUnnamed(source)) {
			const auto &source_children = StructType::GetChildTypes(source);
			const auto &target_children = StructType::GetChildTypes(target);

			const auto common_children = MinValue(source_children.size(), target_children.size());
			for (idx_t i = 0; i < common_children; i++) {
				const auto &source_child_type = source_children[i].second;
				const auto &target_child_type = target_children[i].second;
				InferTemplateType(context, source_child_type, target_child_type, bindings, current_expr, function);
			}
		} else {
			// TODO: Support named structs with template child types.
			throw NotImplementedException("Named structs cannot infer templated child types yet!");
		}
	} break;
	default:
		break; // no template type to infer
	}
}

static void SubstituteTemplateType(LogicalType &type, case_insensitive_map_t<vector<LogicalType>> &bindings,
                                   const Identifier &function_name) {
	// Replace all template types in with their bound concrete types.
	type = TypeVisitor::VisitReplace(type, [&](const LogicalType &t) -> LogicalType {
		if (t.id() == LogicalTypeId::TEMPLATE) {
			const auto index = TemplateType::GetName(t);
			auto it = bindings.find(index);
			if (it != bindings.end()) {
				// found a binding, return the concrete type
				return LogicalType::NormalizeType(it->second.back());
			}

			// If we reach here, the template type was not bound to any concrete type.
			// We dont throw an error here, but give users a chance to handle unresolved template type later in the
			// "bind_scalar_function_t" callback. We then throw an error if the template type is still not bound
			// in the "CheckTemplateTypesResolved" method afterwards.
		}
		return t;
	});
}

void FunctionBinder::ResolveTemplateTypes(BoundSimpleFunction &bound_function,
                                          const vector<unique_ptr<Expression>> &children) {
	case_insensitive_map_t<vector<LogicalType>> bindings;
	vector<reference<LogicalType>> to_substitute;

	// First, we need to infer the template types from the children.
	for (idx_t i = 0; i < bound_function.GetArguments().size(); i++) {
		auto &param = bound_function.GetArguments()[i];

		// If the parameter is not templated, we can skip it.
		if (param.IsTemplated()) {
			auto actual = ExpressionBinder::GetExpressionReturnType(*children[i]);
			InferTemplateType(context, param, actual, bindings, *children[i], bound_function);

			to_substitute.emplace_back(param);
		}
	}

	// If the return type is templated, we need to substitute it as well
	if (bound_function.GetReturnType().IsTemplated()) {
		to_substitute.emplace_back(bound_function.GetReturnType());
	}

	// Finally, substitute all template types in the bound function with their concrete types.
	for (auto &templated_type : to_substitute) {
		SubstituteTemplateType(templated_type, bindings, bound_function.GetName());
	}
}

static void VerifyTemplateType(const LogicalType &type, const Identifier &function_name) {
	TypeVisitor::Contains(type, [&](const LogicalType &type) {
		if (type.id() == LogicalTypeId::TEMPLATE) {
			const auto msg =
			    "Function %s has a template parameter type '%s' that could not be resolved to a concrete type";
			throw BinderException(msg, function_name, TemplateType::GetName(type));
		}
		return false; // continue visiting
	});
}

// Verify that all template types are bound to concrete types.
void FunctionBinder::CheckTemplateTypesResolved(const BoundSimpleFunction &bound_function) {
	for (const auto &arg : bound_function.GetArguments()) {
		VerifyTemplateType(arg, bound_function.GetName());
	}
	VerifyTemplateType(bound_function.GetReturnType(), bound_function.GetName());
}

// Drain all named arguments and lay out the arguments in the order of the function signature:
// [standard parameters | *args | keyword-only parameters | **kwargs], inserting default values where needed.
// The types of the variadic arguments are added to the arguments of the bound function.
// Returns the resolved name of every argument: the parameter name for standard and keyword-only parameters, the
// caller-provided name for "**kwargs" and an empty identifier for "*args".
static vector<Identifier> ResolveArguments(const SimpleFunction &function, BoundSimpleFunction &bound_function,
                                           vector<unique_ptr<Expression>> &arguments,
                                           vector<pair<Identifier, unique_ptr<Expression>>> &named_arguments) {
	const auto &sig = function.GetSignature();
	const auto positional_count = sig.GetPositionalParameterCount();
	const auto args_param = sig.GetArgs();
	const auto kwargs_param = sig.GetKwargs();

	// The arguments that were passed by position
	auto positional_arguments = std::move(arguments);
	const auto passed_count = positional_arguments.size();

	// The arguments that were passed by name, by the index of their parameter
	vector<unique_ptr<Expression>> keyword_arguments(sig.GetParameterCount());
	vector<pair<Identifier, unique_ptr<Expression>>> kwargs;

	identifier_set_t seen_names;
	for (auto &[name, arg] : named_arguments) {
		const auto location = arg->GetQueryLocation();

		if (name.empty()) {
			// Somehow this was not a named argument, throw an error.
			throw BinderException(location,
			                      "Positional arguments cannot follow named arguments in a function call to %s",
			                      function.GetName());
		}

		if (seen_names.count(name)) {
			// This should also not really happen when invoked through SQL
			throw BinderException(location, "Duplicate named argument %s in function call to %s", name,
			                      function.GetName());
		}
		seen_names.insert(name);

		const auto opt_param_idx = sig.GetParameterIndexByName(name);
		if (!opt_param_idx.IsValid()) {
			if (!kwargs_param) {
				throw BinderException(location, "Function %s does not have a parameter named %s", function.GetName(),
				                      name);
			}
			kwargs.emplace_back(name, std::move(arg));
			continue;
		}

		const auto param_idx = opt_param_idx.GetIndex();
		if (sig.GetParameter(param_idx).AcceptsPosition() && param_idx < passed_count) {
			throw BinderException(location,
			                      "Named argument '%s' cannot be used for parameter %s because it has already "
			                      "been provided as a positional argument in function call to %s",
			                      arg->ToString(), name, function.GetName());
		}
		keyword_arguments[param_idx] = std::move(arg);
	}

	// the arguments in the order of the signature, moved back into "arguments" once they are all resolved
	vector<unique_ptr<Expression>> resolved_arguments;
	vector<Identifier> resolved_names;
	vector<Identifier> named_arguments_names;
	auto &bound_arguments = bound_function.GetArguments();

	auto add_argument = [&](unique_ptr<Expression> arg, Identifier resolved_name) {
		resolved_arguments.push_back(std::move(arg));
		resolved_names.push_back(std::move(resolved_name));
	};

	for (idx_t param_idx = 0; param_idx < sig.GetParameterCount(); param_idx++) {
		const auto &param = sig.GetParameter(param_idx);
		switch (param.GetKind()) {
		case FunctionParameterKind::VAR_POSITIONAL:
			for (idx_t i = positional_count; i < passed_count; i++) {
				bound_arguments.insert(bound_arguments.begin() + NumericCast<int64_t>(resolved_arguments.size()),
				                       param.GetType());
				add_argument(std::move(positional_arguments[i]), Identifier());
			}
			continue;
		case FunctionParameterKind::VAR_KEYWORD:
			for (auto &[name, arg] : kwargs) {
				bound_arguments.push_back(param.GetType());
				named_arguments_names.push_back(name);
				add_argument(std::move(arg), name);
			}
			continue;
		default:
			break;
		}

		if (param.AcceptsPosition() && param_idx < passed_count) {
			add_argument(std::move(positional_arguments[param_idx]), param.GetName());
		} else if (keyword_arguments[param_idx]) {
			add_argument(std::move(keyword_arguments[param_idx]), param.GetName());
		} else if (param.HasDefaultValue()) {
			auto default_arg = make_uniq<BoundConstantExpression>(*param.GetDefaultValue());
			default_arg->SetAlias(param.GetName());
			add_argument(std::move(default_arg), param.GetName());
		} else {
			throw BinderException("Missing value for parameter %s in function call to %s", param.GetName(),
			                      function.GetName());
		}
		if (param.GetKind() == FunctionParameterKind::KEYWORD_ONLY) {
			named_arguments_names.push_back(param.GetName());
		}
	}

	if (!args_param) {
		// Without "*args", positional arguments beyond the signature stay behind the resolved arguments
		for (idx_t i = positional_count; i < passed_count; i++) {
			add_argument(std::move(positional_arguments[i]), Identifier());
		}
	}

	const auto positional_argument_count = resolved_arguments.size() - named_arguments_names.size();
	bound_function.SetNamedArguments(positional_argument_count, std::move(named_arguments_names));
	arguments = std::move(resolved_arguments);

	return resolved_names;
}

// The names of the named arguments have to stay parallel to the last arguments
static void VerifyArgumentCount(const BoundSimpleFunction &bound_function, idx_t resolved_count, idx_t argument_count) {
	if (resolved_count != argument_count && !bound_function.GetNamedArguments().empty()) {
		throw InternalException("Function %s cannot add or remove arguments in its bind callback when it is "
		                        "called with keyword-only or '**kwargs' arguments",
		                        bound_function.GetName());
	}
}

//! The arguments are cast before the bind callback - verify that it did not change the argument types
static void VerifyBindArgumentTypes(const BoundScalarFunction &bound_function,
                                    const vector<unique_ptr<Expression>> &arguments) {
	auto &argument_types = bound_function.GetArguments();
	if (arguments.size() > argument_types.size()) {
		throw InternalException("Function %s added arguments in its bind callback - argument types must be "
		                        "resolved in the resolve_types callback",
		                        bound_function.GetName());
	}
	for (idx_t i = 0; i < arguments.size(); i++) {
		auto &source_type = arguments[i]->GetReturnType();
		if (source_type.id() == LogicalTypeId::LAMBDA) {
			continue;
		}
		auto target_type = argument_types[i];
		PrepareTypeForCast(target_type);
		if (target_type.id() == LogicalTypeId::UNKNOWN || target_type.id() == LogicalTypeId::INVALID) {
			continue;
		}
		if (RequiresCast(source_type, target_type) == LogicalTypeComparisonResult::DIFFERENT_TYPES) {
			throw InternalException("Function %s changed the type of argument %llu from %s to %s in its bind "
			                        "callback - argument types must be resolved in the resolve_types callback",
			                        bound_function.GetName(), i + 1, source_type, target_type);
		}
	}
}

//! Incomplete argument types (e.g. a DECIMAL without width and scale) accept any type of that kind - resolve them to
//! the type of the argument
static void ResolveIncompleteArgumentTypes(BoundSimpleFunction &bound_function,
                                           const vector<unique_ptr<Expression>> &arguments) {
	auto &argument_types = bound_function.GetArguments();
	for (idx_t i = 0; i < arguments.size() && i < argument_types.size(); i++) {
		auto &source_type = arguments[i]->GetReturnType();
		if (!IsIncompleteCastTarget(argument_types[i]) || source_type.id() != argument_types[i].id() ||
		    !source_type.IsComplete()) {
			continue;
		}
		argument_types[i] = source_type;
	}
}

static vector<LogicalType> CaptureLogicalArguments(const BoundSimpleFunction &function,
                                                   const vector<unique_ptr<Expression>> &arguments) {
	D_ASSERT(function.GetArguments().size() == arguments.size());
	vector<LogicalType> result;
	result.reserve(arguments.size());
	for (idx_t argument_index = 0; argument_index < arguments.size(); argument_index++) {
		auto target_type = function.GetArguments()[argument_index];
		PrepareTypeForCast(target_type);
		result.push_back(target_type.IsComplete() ? target_type : arguments[argument_index]->GetReturnType());
	}
	return result;
}

ResolvedScalarFunctionTypes
FunctionBinder::ResolveScalarFunctionTypes(shared_ptr<const ScalarFunction> function_p,
                                           vector<unique_ptr<Expression>> &arguments,
                                           vector<pair<Identifier, unique_ptr<Expression>>> &named_arguments) {
	auto &function = *function_p;
	// Make a BoundScalarFunction out of the ScalarFunction, so we can store bind info and other properties in it.
	ResolvedScalarFunctionTypes result {BoundScalarFunction(std::move(function_p)), {}, {}};
	auto &bound_function = result.bound_function;

	// Reorder named args and expand the variadic arguments
	result.argument_names = ResolveArguments(function, bound_function, arguments, named_arguments);

	// Attempt to resolve template types, before we call the "resolve_types" callback.
	ResolveTemplateTypes(bound_function, arguments);
	bound_function.SetLogicalArguments(CaptureLogicalArguments(bound_function, arguments));
	ResolveIncompleteArgumentTypes(bound_function, arguments);

	if (bound_function.HasResolveTypesCallback()) {
		ResolveScalarFunctionTypesInput input(context, bound_function, arguments, result.argument_names);
		bound_function.GetResolveTypesCallback()(input);
		VerifyArgumentCount(bound_function, result.argument_names.size(), arguments.size());
	}

	// The argument types are final - all templates must be resolved before we add casts
	for (auto &argument_type : bound_function.GetArguments()) {
		VerifyTemplateType(argument_type, bound_function.GetName());
	}
	result.pushed_collation =
	    PushArgumentCollations(context, bound_function, bound_function.GetProperties(), arguments);
	CastToFunctionArguments(bound_function, arguments);
	return result;
}

pair<BoundScalarFunction, unique_ptr<FunctionData>>
FunctionBinder::ResolveFunction(shared_ptr<const ScalarFunction> function_p, vector<unique_ptr<Expression>> &arguments,
                                vector<pair<Identifier, unique_ptr<Expression>>> &named_arguments) {
	auto resolved = ResolveScalarFunctionTypes(std::move(function_p), arguments, named_arguments);
	auto &bound_function = resolved.bound_function;
	auto &argument_names = resolved.argument_names;

	// The bind callback is called with arguments that are cast to the argument types of the bound function
	unique_ptr<FunctionData> bind_info;
	if (bound_function.HasBindCallback()) {
		BindScalarFunctionInput input(context, bound_function, arguments, argument_names, binder);
		bind_info = bound_function.GetBindCallback()(input);
		VerifyArgumentCount(bound_function, argument_names.size(), arguments.size());
		VerifyBindArgumentTypes(bound_function, arguments);
	}
	VerifyTemplateType(bound_function.GetReturnType(), bound_function.GetName());

	if (bound_function.HasModifiedDatabasesCallback() && binder) {
		auto &properties = binder->GetStatementProperties();
		FunctionModifiedDatabasesInput input(bind_info, properties);
		bound_function.GetModifiedDatabasesCallback()(context, input);
	}

	PropagateReturnCollation(bound_function, bound_function.GetProperties(), arguments, resolved.pushed_collation);
	bound_function.SetLogicalReturnType(bound_function.GetReturnType());

	return {std::move(bound_function), std::move(bind_info)};
}

unique_ptr<Expression> FunctionBinder::BindScalarFunction(shared_ptr<const ScalarFunction> function,
                                                          vector<unique_ptr<Expression>> children, bool is_operator,
                                                          optional_ptr<Binder> binder) {
	return BindScalarFunction(std::move(function), std::move(children), {}, is_operator, binder);
}

unique_ptr<Expression> FunctionBinder::BindScalarFunction(const ScalarFunction &function,
                                                          vector<unique_ptr<Expression>> children, bool is_operator,
                                                          optional_ptr<Binder> binder) {
	return BindScalarFunction(make_shared_ptr<ScalarFunction>(function), std::move(children), {}, is_operator, binder);
}

unique_ptr<Expression> FunctionBinder::BindScalarFunction(const ScalarFunction &function,
                                                          vector<unique_ptr<Expression>> children,
                                                          vector<pair<Identifier, unique_ptr<Expression>>> keyword_args,
                                                          bool is_operator, optional_ptr<Binder> binder) {
	return BindScalarFunction(make_shared_ptr<ScalarFunction>(function), std::move(children), std::move(keyword_args),
	                          is_operator, binder);
}

unique_ptr<Expression> FunctionBinder::BindScalarFunction(shared_ptr<const ScalarFunction> function,
                                                          vector<unique_ptr<Expression>> children,
                                                          vector<pair<Identifier, unique_ptr<Expression>>> keyword_args,
                                                          bool is_operator, optional_ptr<Binder> binder) {
	auto [bound_function, bind_info] = ResolveFunction(std::move(function), children, keyword_args);

	unique_ptr<Expression> result;

	auto result_func = make_uniq<BoundFunctionExpression>(std::move(bound_function), std::move(children),
	                                                      std::move(bind_info), is_operator);

	if (binder && result_func->Function().GetStability() == FunctionStability::CONSISTENT_WITHIN_QUERY) {
		// the result depends on the query it is bound in, also when the bind_expression callback folds it
		binder->SetAlwaysRequireRebind();
	}
	if (result_func->Function().HasBindExpressionCallback()) {
		// if a bind_expression callback is registered - call it and emit the resulting expression
		FunctionBindExpressionInput input(context, result_func->FunctionMutable(), result_func->BindInfoMutable().get(),
		                                  result_func->GetChildrenMutable());
		result = result_func->Function().GetBindExpressionCallback()(input);
	}

	if (!result) {
		result = std::move(result_func);
	}

	return result;
}

pair<BoundAggregateFunction, unique_ptr<FunctionData>>
FunctionBinder::ResolveFunction(shared_ptr<const AggregateFunction> function_p,
                                vector<unique_ptr<Expression>> &children,
                                vector<pair<Identifier, unique_ptr<Expression>>> &named_arguments) {
	auto &function = *function_p;
	// Make a BoundFunction out of the func
	BoundAggregateFunction bound_function(std::move(function_p));

	// Reorder named args and expand the variadic arguments
	auto argument_names = ResolveArguments(function, bound_function, children, named_arguments);

	ResolveTemplateTypes(bound_function, children);
	bound_function.SetLogicalArguments(CaptureLogicalArguments(bound_function, children));

	unique_ptr<FunctionData> bind_info;

	if (bound_function.GetCallbacks().HasBindCallback()) {
		BindAggregateFunctionInput input(context, bound_function, children, argument_names);
		bind_info = bound_function.GetCallbacks().GetBindCallback()(input);

		// we may have lost some arguments in the bind
		children.resize(MinValue(bound_function.GetArguments().size(), children.size()));
		VerifyArgumentCount(bound_function, argument_names.size(), children.size());
	}

	CheckTemplateTypesResolved(bound_function);
	bound_function.SetLogicalReturnType(bound_function.GetReturnType());

	// check if we need to add casts to the children
	CastToFunctionArguments(bound_function, children);

	return {std::move(bound_function), std::move(bind_info)};
}

unique_ptr<BoundAggregateExpression> FunctionBinder::BindAggregateFunction(shared_ptr<const AggregateFunction> function,
                                                                           vector<unique_ptr<Expression>> children,
                                                                           unique_ptr<Expression> filter,
                                                                           AggregateType aggr_type) {
	return BindAggregateFunction(std::move(function), std::move(children), {}, std::move(filter), aggr_type);
}

unique_ptr<BoundAggregateExpression> FunctionBinder::BindAggregateFunction(const AggregateFunction &function,
                                                                           vector<unique_ptr<Expression>> children,
                                                                           unique_ptr<Expression> filter,
                                                                           AggregateType aggr_type) {
	return BindAggregateFunction(make_shared_ptr<AggregateFunction>(function), std::move(children), {},
	                             std::move(filter), aggr_type);
}

unique_ptr<BoundAggregateExpression>
FunctionBinder::BindAggregateFunction(const AggregateFunction &function, vector<unique_ptr<Expression>> children,
                                      vector<pair<Identifier, unique_ptr<Expression>>> keyword_args,
                                      unique_ptr<Expression> filter, AggregateType aggr_type) {
	return BindAggregateFunction(make_shared_ptr<AggregateFunction>(function), std::move(children),
	                             std::move(keyword_args), std::move(filter), aggr_type);
}

unique_ptr<BoundAggregateExpression>
FunctionBinder::BindAggregateFunction(shared_ptr<const AggregateFunction> function,
                                      vector<unique_ptr<Expression>> children,
                                      vector<pair<Identifier, unique_ptr<Expression>>> keyword_args,
                                      unique_ptr<Expression> filter, AggregateType aggr_type) {
	if (aggr_type == AggregateType::DISTINCT &&
	    function->GetCollationHandling() == FunctionCollationHandling::PUSH_DISTINCT_COLLATIONS) {
		for (auto &child : children) {
			ExpressionBinder::PushCollation(context, child, child->GetReturnType());
		}
		for (auto &argument : keyword_args) {
			ExpressionBinder::PushCollation(context, argument.second, argument.second->GetReturnType());
		}
	}
	auto [bound_function, bind_info] = ResolveFunction(std::move(function), children, keyword_args);

	return make_uniq<BoundAggregateExpression>(std::move(bound_function), std::move(children), std::move(filter),
	                                           std::move(bind_info), aggr_type);
}

unique_ptr<BoundAggregateExpression>
FunctionBinder::BindAggregateFunction(const AggregateFunctionCatalogEntry &func,
                                      vector<pair<Identifier, unique_ptr<Expression>>> arguments, ErrorData &error,
                                      unique_ptr<Expression> filter, AggregateType aggr_type) {
	// select the best matching overload (this may name positional arguments by their alias for functions that opt
	// into implicit argument naming, e.g. struct_pack/row)
	auto best_function = BindFunctionFromArguments(func.name, func.functions, arguments, error);
	if (!best_function.IsValid()) {
		return nullptr;
	}

	// found a matching function!
	auto bound_function = func.functions.GetFunctionByOffset(best_function.GetIndex());

	// now that the overload is fixed, split the arguments into their final positional/named children
	auto [regular_args, keyword_args] = SplitArguments(std::move(arguments));

	return BindAggregateFunction(std::move(bound_function), std::move(regular_args), std::move(keyword_args),
	                             std::move(filter), aggr_type);
}

pair<BoundWindowFunction, unique_ptr<FunctionData>>
FunctionBinder::ResolveFunction(shared_ptr<const WindowFunction> function_p, vector<unique_ptr<Expression>> &children,
                                vector<pair<Identifier, unique_ptr<Expression>>> &named_arguments,
                                optional_ptr<vector<LogicalType>> order_types,
                                optional_ptr<vector<LogicalType>> arg_order_types) {
	auto &function = *function_p;
	BoundWindowFunction bound_function(std::move(function_p));

	// Reorder named args and expand the variadic arguments
	auto argument_names = ResolveArguments(function, bound_function, children, named_arguments);

	ResolveTemplateTypes(bound_function, children);
	auto logical_arguments = CaptureLogicalArguments(bound_function, children);
	auto argument_types = bound_function.GetArguments();

	unique_ptr<FunctionData> bind_info;

	if (bound_function.HasBindCallback()) {
		BindWindowFunctionInput input(context, bound_function, children, argument_names, order_types, arg_order_types);
		bind_info = bound_function.GetBindCallback()(input);
		// we may have lost some arguments in the bind
		children.resize(MinValue(bound_function.GetArguments().size(), children.size()));
		VerifyArgumentCount(bound_function, argument_names.size(), children.size());
	}

	CheckTemplateTypesResolved(bound_function);
	for (idx_t i = 0; i < argument_types.size() && i < bound_function.GetArguments().size(); i++) {
		if (!argument_types[i].IsComplete() && bound_function.GetArguments()[i].IsComplete()) {
			logical_arguments[i] = bound_function.GetArguments()[i];
		}
	}
	bound_function.SetLogicalArguments(std::move(logical_arguments));
	bound_function.SetLogicalReturnType(bound_function.GetReturnType());

	// check if we need to add casts to the children
	CastToFunctionArguments(bound_function, children);

	return {std::move(bound_function), std::move(bind_info)};
}

unique_ptr<BoundWindowExpression>
FunctionBinder::BindWindowFunction(shared_ptr<const WindowFunction> function, vector<unique_ptr<Expression>> children,
                                   vector<pair<Identifier, unique_ptr<Expression>>> keyword_args,
                                   vector<LogicalType> &order_types, vector<LogicalType> &arg_order_types) {
	auto [bound_function, bind_info] =
	    ResolveFunction(std::move(function), children, keyword_args, order_types, arg_order_types);
	auto return_type = bound_function.GetReturnType();

	auto window = make_uniq<BoundWindowFunction>(std::move(bound_function));
	auto result = make_uniq<BoundWindowExpression>(return_type, nullptr, std::move(window), std::move(bind_info));
	result->GetChildrenMutable() = std::move(children);

	return result;
}

unique_ptr<BoundWindowExpression>
FunctionBinder::BindWindowFunction(const WindowFunction &function, vector<unique_ptr<Expression>> children,
                                   vector<pair<Identifier, unique_ptr<Expression>>> keyword_args,
                                   vector<LogicalType> &order_types, vector<LogicalType> &arg_order_types) {
	return BindWindowFunction(make_shared_ptr<WindowFunction>(function), std::move(children), std::move(keyword_args),
	                          order_types, arg_order_types);
}

unique_ptr<BoundWindowExpression> FunctionBinder::BindWindowFunction(shared_ptr<const WindowFunction> function,
                                                                     vector<unique_ptr<Expression>> children,
                                                                     vector<LogicalType> &order_types,
                                                                     vector<LogicalType> &arg_order_types) {
	vector<pair<Identifier, unique_ptr<Expression>>> empty_keyword_args;
	return BindWindowFunction(std::move(function), std::move(children), std::move(empty_keyword_args), order_types,
	                          arg_order_types);
}

unique_ptr<BoundWindowExpression> FunctionBinder::BindWindowFunction(const WindowFunction &function,
                                                                     vector<unique_ptr<Expression>> children,
                                                                     vector<LogicalType> &order_types,
                                                                     vector<LogicalType> &arg_order_types) {
	vector<pair<Identifier, unique_ptr<Expression>>> empty_keyword_args;
	return BindWindowFunction(make_shared_ptr<WindowFunction>(function), std::move(children),
	                          std::move(empty_keyword_args), order_types, arg_order_types);
}

unique_ptr<BoundWindowExpression>
FunctionBinder::BindWindowFunction(const WindowFunctionCatalogEntry &func,
                                   vector<pair<Identifier, unique_ptr<Expression>>> arguments, ErrorData &error,
                                   vector<LogicalType> &order_types, vector<LogicalType> &arg_order_types) {
	// select the best matching overload
	auto best_function = BindFunctionFromArguments(func.name, func.functions, arguments, error);
	if (!best_function.IsValid()) {
		return nullptr;
	}

	// found a matching function!
	auto bound_function = func.functions.GetFunctionByOffset(best_function.GetIndex());

	// now that the overload is fixed, split the arguments into their final positional/named children
	auto [regular_args, keyword_args] = SplitArguments(std::move(arguments));

	return BindWindowFunction(std::move(bound_function), std::move(regular_args), std::move(keyword_args), order_types,
	                          arg_order_types);
}

} // namespace duckdb
