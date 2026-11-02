//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/function_binder.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/function/function.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/aggregate_function.hpp"
#include "duckdb/function/window_function.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/enums/order_type.hpp"
#include "duckdb/common/optional.hpp"
#include "duckdb/common/query_location.hpp"

#include <variant>

namespace duckdb {
class AggregateFunctionCatalogEntry;

class WindowFunctionCatalogEntry;
struct BoundPragmaInfo;

//! One ORDER BY key of an exported ordered aggregate state: the buffered struct column it sorts on and the modifiers.
struct SortedAggregateStateOrder {
	idx_t column;
	OrderType order_type;
	OrderByNullType null_order;
};

//! A scalar function with its argument and return types resolved, before the bind callback is invoked
struct ResolvedScalarFunctionTypes {
	BoundScalarFunction bound_function;
	//! The collation pushed into the arguments, to be propagated to the return type after binding
	string pushed_collation;
};

//! A table function bound to the constants it is called with, before its bind callback is invoked
struct ResolvedTableFunction {
	explicit ResolvedTableFunction(BoundTableFunction function_p, vector<Value> parameters_p = {},
	                               named_argument_map_t named_parameters_p = {})
	    : function(std::move(function_p)), parameters(std::move(parameters_p)),
	      named_parameters(std::move(named_parameters_p)) {
	}

	BoundTableFunction function;
	vector<Value> parameters;
	named_argument_map_t named_parameters;
};

//! An overload that matches a call, and how the arguments of the call are passed to it
struct FunctionBinding {
	//! The index of the overload in the function set
	idx_t GetOverloadIndex() const {
		return overload;
	}
	//! The overload the call binds to
	const SimpleFunction &GetFunction() const {
		return *function;
	}
	//! Whether the call binds to this overload at a lower cost - an order of how well the overloads match, not of
	//! which overload they are
	bool operator<(const FunctionBinding &other) const {
		return cost < other.cost;
	}
	template <class T>
	shared_ptr<const T> GetFunction() const {
		D_ASSERT(function->GetKind() == T::KIND);
		return shared_ptr_cast<const SimpleFunction, const T>(function);
	}

	//! The cost of casting the arguments to the parameters of the overload
	idx_t cost = 0;
	//! The parameter of the overload that receives each argument - the positional arguments, followed by the named
	//! ones. Arguments received by "*args" or "**kwargs" map to that parameter
	vector<idx_t> argument_to_parameters;
	//! The type of every parameter of the overload, with the templates the arguments and defaults determine
	//! substituted
	vector<LogicalType> parameter_types;
	//! The return type of the overload, with the templates substituted
	LogicalType return_type;

private:
	friend class FunctionBinder;
	idx_t overload = 0;
	shared_ptr<const SimpleFunction> function;
};

//! One argument of a call - a positional one if it has no name
struct FunctionCallArgument {
	FunctionCallArgument(LogicalType type, Identifier name = Identifier(), // NOLINT: allow implicit conversion
	                     QueryLocation location = QueryLocation())
	    : type(std::move(type)), name(std::move(name)), location(location) {
	}
	//! For the type constants, e.g. LogicalType::INTEGER
	FunctionCallArgument(LogicalTypeId id)
	    : FunctionCallArgument(LogicalType(id)) { // NOLINT: allow implicit conversion
	}

	LogicalType type;
	Identifier name;
	QueryLocation location;
};

//! The arguments of a call to select an overload for - the positional arguments, followed by the named ones
class FunctionCallArguments {
public:
	FunctionCallArguments() = default;
	//! E.g. {LogicalType::INTEGER, {LogicalType::BIGINT, "opt"}} - the positional arguments come first
	FunctionCallArguments(std::initializer_list<FunctionCallArgument> arguments); // NOLINT: allow implicit conversion
	FunctionCallArguments(vector<LogicalType> positional,                         // NOLINT: allow implicit conversion
	                      vector<pair<Identifier, LogicalType>> named = {});
	//! The types and locations of bound arguments
	FunctionCallArguments(const vector<unique_ptr<Expression>> &positional, // NOLINT: allow implicit conversion
	                      const vector<pair<Identifier, unique_ptr<Expression>>> &named = {});

public:
	idx_t GetArgumentCount() const {
		return arguments.size();
	}
	idx_t GetPositionalCount() const {
		return positional_count;
	}
	//! An argument - the positional arguments, followed by the named ones
	const FunctionCallArgument &GetArgument(idx_t argument) const {
		return arguments[argument];
	}
	//! The types of the positional arguments
	vector<LogicalType> GetPositionalTypes() const;
	//! The names and types of the named arguments
	vector<pair<Identifier, LogicalType>> GetNamedTypes() const;

private:
	void AddArgument(FunctionCallArgument argument);

private:
	vector<FunctionCallArgument> arguments;
	idx_t positional_count = 0;
};

class FunctionBindError;

//! Why an overload does not match a call. Arguments are numbered as the positional arguments, followed by the named
//! ones - what the call itself says about an argument, its type and name, is not repeated here
class FunctionMismatch {
public:
	enum class Type : uint8_t {
		MISSING_PARAMETER,
		TOO_MANY_ARGUMENTS,
		UNKNOWN_NAMED_ARGUMENT,
		ALREADY_PASSED_BY_POSITION,
		INCOMPATIBLE_ARGUMENT,
		TEMPLATE_DEDUCTION,
		TEMPLATE_UNRESOLVED
	};

	//! A parameter without a default is not filled by the call
	struct MissingParameter {
		static constexpr auto TYPE = Type::MISSING_PARAMETER;
		idx_t parameter;
		//! The name of the parameter - empty if it has none
		Identifier name;

		string ToString(const SimpleFunction &function, const FunctionBindError &error) const;
	};
	//! The call passes more arguments than the overload takes, starting at this one
	struct TooManyArguments {
		static constexpr auto TYPE = Type::TOO_MANY_ARGUMENTS;
		idx_t argument;
		//! The number of arguments the overload accepts at most
		idx_t accepted;
		//! Whether that is the number of arguments it accepts by position
		bool positional;

		string ToString(const SimpleFunction &function, const FunctionBindError &error) const;
	};
	//! A named argument names no parameter, and the overload has no "**kwargs" to receive it
	struct UnknownNamedArgument {
		static constexpr auto TYPE = Type::UNKNOWN_NAMED_ARGUMENT;
		idx_t argument;

		string ToString(const SimpleFunction &function, const FunctionBindError &error) const;
	};
	//! A named argument names a parameter that a positional argument already fills
	struct AlreadyPassedByPosition {
		static constexpr auto TYPE = Type::ALREADY_PASSED_BY_POSITION;
		idx_t argument;

		string ToString(const SimpleFunction &function, const FunctionBindError &error) const;
	};
	//! An argument cannot be implicitly cast to the type of the parameter that receives it
	struct IncompatibleArgument {
		static constexpr auto TYPE = Type::INCOMPATIBLE_ARGUMENT;
		idx_t argument;
		//! The type of the parameter that receives the argument
		LogicalType parameter_type;

		string ToString(const SimpleFunction &function, const FunctionBindError &error) const;
	};
	//! The arguments determine conflicting types for a template of the overload
	struct TemplateDeduction {
		static constexpr auto TYPE = Type::TEMPLATE_DEDUCTION;
		//! The argument that conflicts - invalid if it is the default of a parameter
		optional_idx argument;
		string template_name;
		//! The types the template was inferred to be, in the order they were inferred, followed by the type that
		//! conflicts with them
		vector<LogicalType> inferred_types;

		string ToString(const SimpleFunction &function, const FunctionBindError &error) const;
	};
	//! Neither the arguments nor the defaults determine a template of the types the call uses
	struct TemplateUnresolved {
		static constexpr auto TYPE = Type::TEMPLATE_UNRESOLVED;
		string template_name;

		string ToString(const SimpleFunction &function, const FunctionBindError &error) const;
	};

	//! The alternatives are in the order of Type
	using Reason = std::variant<MissingParameter, TooManyArguments, UnknownNamedArgument, AlreadyPassedByPosition,
	                            IncompatibleArgument, TemplateDeduction, TemplateUnresolved>;

	FunctionMismatch(idx_t overload, shared_ptr<const SimpleFunction> function, idx_t distance, Reason reason)
	    : overload(overload), function(std::move(function)), distance(distance), reason(std::move(reason)) {
	}

public:
	//! The index of the overload in the function set
	idx_t GetOverloadIndex() const {
		return overload;
	}
	//! The overload the call does not match
	const SimpleFunction &GetFunction() const {
		return *function;
	}
	//! How far the call is from matching the overload, lower is closer. Any number of arguments that cannot be cast
	//! is closer than a call of the wrong shape - missing parameters, extra arguments or unknown names
	idx_t GetDistance() const {
		return distance;
	}
	//! Whether the call is closer to matching this overload - an order of how close the overloads are, not of which
	//! overload they are
	bool operator<(const FunctionMismatch &other) const {
		return distance < other.distance;
	}
	Type GetType() const {
		return static_cast<Type>(reason.index());
	}
	template <class T>
	const T &Cast() const {
		D_ASSERT(GetType() == T::TYPE);
		return std::get<T>(reason);
	}
	bool IsTemplateMismatch() const {
		return GetType() == Type::TEMPLATE_DEDUCTION || GetType() == Type::TEMPLATE_UNRESOLVED;
	}
	//! Why the call of the error does not match, e.g. "argument 1 of type DATE cannot be implicitly cast to VARCHAR"
	string ToString(const FunctionBindError &error) const;

private:
	idx_t overload;
	shared_ptr<const SimpleFunction> function;
	idx_t distance;
	Reason reason;
};

//! Why no overload of a function set could be selected for a call
class FunctionBindError {
public:
	enum class Type : uint8_t { NO_MATCH, AMBIGUOUS_MATCH, DUPLICATE_ARGUMENT };

	//! No overload matches the call
	struct NoMatch {
		static constexpr auto TYPE = Type::NO_MATCH;
		//! Why every overload does not match, closest first
		vector<FunctionMismatch> mismatches;

		//! The closest mismatch, if no other is as close
		optional_ptr<const FunctionMismatch> GetClosest() const;
		ErrorData ToError(const FunctionBindError &error) const;
	};
	//! Several overloads match at the lowest cost
	struct AmbiguousMatch {
		static constexpr auto TYPE = Type::AMBIGUOUS_MATCH;
		vector<FunctionBinding> candidates;

		ErrorData ToError(const FunctionBindError &error) const;
	};
	//! A named argument repeats the name of an earlier one, so no overload can match
	struct DuplicateArgument {
		static constexpr auto TYPE = Type::DUPLICATE_ARGUMENT;
		idx_t argument;
		//! The kind of the functions of the set - the call matches none of them, so none of them is at hand
		FunctionKind function_kind;

		ErrorData ToError(const FunctionBindError &error) const;
	};

	//! The alternatives are in the order of Type
	using Kind = std::variant<NoMatch, AmbiguousMatch, DuplicateArgument>;

	//! No overload of the set matches the call - the mismatches are sorted, closest first
	static FunctionBindError CreateNoMatch(Identifier name, FunctionCallArguments call,
	                                       vector<FunctionMismatch> mismatches) {
		return FunctionBindError(std::move(name), std::move(call), NoMatch {std::move(mismatches)});
	}
	//! Several overloads match the call at the lowest cost
	static FunctionBindError CreateAmbiguousMatch(Identifier name, FunctionCallArguments call,
	                                              vector<FunctionBinding> candidates) {
		return FunctionBindError(std::move(name), std::move(call), AmbiguousMatch {std::move(candidates)});
	}
	//! A named argument of the call repeats the name of an earlier one
	static FunctionBindError CreateDuplicateArgument(Identifier name, FunctionCallArguments call, idx_t argument,
	                                                 FunctionKind function_kind) {
		return FunctionBindError(std::move(name), std::move(call), DuplicateArgument {argument, function_kind});
	}

public:
	//! The name of the function set the selection was made from
	const Identifier &GetName() const {
		return name;
	}
	//! The call the selection was made for
	const FunctionCallArguments &GetCall() const {
		return call;
	}
	Type GetType() const {
		return static_cast<Type>(kind.index());
	}
	template <class T>
	const T &Cast() const {
		D_ASSERT(GetType() == T::TYPE);
		return std::get<T>(kind);
	}
	//! The error to report for the call, pointing at the argument it is about if its location is known
	operator ErrorData() const; // NOLINT: allow implicit conversion
	[[noreturn]] void Throw() const;
	//! The messages of the error, as ErrorData::Message() and ErrorData::RawMessage()
	string Message() const;
	string RawMessage() const;

private:
	FunctionBindError(Identifier name, FunctionCallArguments call, Kind kind)
	    : name(std::move(name)), call(std::move(call)), kind(std::move(kind)) {
	}

private:
	Identifier name;
	FunctionCallArguments call;
	Kind kind;
};

//! The outcome of selecting the overload of a function set that a call binds to
class FunctionBindResult {
public:
	FunctionBindResult(FunctionBinding binding) : result(std::move(binding)) { // NOLINT: allow implicit conversion
	}
	FunctionBindResult(FunctionBindError error) : result(std::move(error)) { // NOLINT: allow implicit conversion
	}

public:
	bool IsSuccess() const {
		return std::holds_alternative<FunctionBinding>(result);
	}
	explicit operator bool() const {
		return IsSuccess();
	}
	//! The overload the call binds to - only if the selection succeeded
	const FunctionBinding &GetBinding() const {
		D_ASSERT(IsSuccess());
		return std::get<FunctionBinding>(result);
	}
	//! Why the selection failed - only if it did not succeed
	const FunctionBindError &GetError() const {
		D_ASSERT(!IsSuccess());
		return std::get<FunctionBindError>(result);
	}

private:
	std::variant<FunctionBinding, FunctionBindError> result;
};

//! The FunctionBinder class is responsible for binding functions
class FunctionBinder {
public:
	DUCKDB_API explicit FunctionBinder(Binder &binder);
	DUCKDB_API explicit FunctionBinder(ClientContext &context);

	optional_ptr<Binder> binder;
	ClientContext &context;
	//! Set when the arguments were bound before, so their collations are not pushed again
	bool arguments_collated = false;

public:
	//! Overload selection: the cheapest overload of a function set that a call with the given arguments binds to.
	//! Selection needs no binder - without a ClientContext only the built-in implicit casts are considered. With
	//! explicit casts, an argument that cannot be implicitly cast to its parameter is accepted too
	DUCKDB_API static FunctionBindResult SelectFunction(optional_ptr<ClientContext> context,
	                                                    const BaseFunctionSet &functions,
	                                                    const FunctionCallArguments &call, bool explicit_casts = false);

	//! Bind a table function: select the overload, check the named arguments against its signature, and fold the
	//! arguments to the constants it is called with, cast to the types it declares. Returns nullptr and sets the error
	//! if no overload matches
	DUCKDB_API unique_ptr<ResolvedTableFunction>
	BindTableFunction(const TableFunctionSet &functions, vector<unique_ptr<Expression>> &positional_arguments,
	                  vector<pair<Identifier, unique_ptr<Expression>>> &named_arguments, ErrorData &error);
	//! Bind a pragma function, as BindTableFunction binds a table function
	DUCKDB_API unique_ptr<BoundPragmaInfo>
	BindPragmaFunction(const PragmaFunctionSet &functions, vector<unique_ptr<Expression>> &positional_arguments,
	                   vector<pair<Identifier, unique_ptr<Expression>>> &named_arguments, ErrorData &error);

	//! Casts a constant to the type of the parameter it fills, as an argument of any function is cast - a parameter
	//! whose type is not concrete, such as ANY, takes it as it is
	DUCKDB_API static Value CastToParameterType(ClientContext &context, Value value, const LogicalType &parameter_type);

	DUCKDB_API unique_ptr<Expression> BindScalarFunction(const Identifier &schema, const Identifier &name,
	                                                     vector<unique_ptr<Expression>> children, ErrorData &error,
	                                                     bool is_operator = false,
	                                                     optional_ptr<Binder> binder = nullptr);

	//! Throws the binding error instead of returning nullptr
	DUCKDB_API unique_ptr<Expression> BindScalarFunction(const Identifier &schema, const Identifier &name,
	                                                     vector<unique_ptr<Expression>> children,
	                                                     bool is_operator = false,
	                                                     optional_ptr<Binder> binder = nullptr);

	DUCKDB_API unique_ptr<Expression> BindScalarFunction(const ScalarFunctionCatalogEntry &function,
	                                                     vector<unique_ptr<Expression>> children, ErrorData &error,
	                                                     bool is_operator = false,
	                                                     optional_ptr<Binder> binder = nullptr);

	DUCKDB_API unique_ptr<Expression> BindScalarFunction(const shared_ptr<const ScalarFunction> &function,
	                                                     vector<unique_ptr<Expression>> children,
	                                                     bool is_operator = false,
	                                                     optional_ptr<Binder> binder = nullptr);

	//! Bind a function that does not come from a function set - the function is copied into a definition of its own
	DUCKDB_API unique_ptr<Expression> BindScalarFunction(const ScalarFunction &function,
	                                                     vector<unique_ptr<Expression>> children,
	                                                     bool is_operator = false,
	                                                     optional_ptr<Binder> binder = nullptr);

	//! Bind a scalar function from a catalog entry given the full list of (maybe-named) bound arguments. The
	//! positional/named split is resolved per candidate overload (overloads flagged to capture argument aliases
	//! treat every argument as positional and keep its alias).
	DUCKDB_API unique_ptr<Expression> BindScalarFunction(const ScalarFunctionCatalogEntry &function,
	                                                     vector<pair<Identifier, unique_ptr<Expression>>> arguments,
	                                                     ErrorData &error, bool is_operator = false,
	                                                     optional_ptr<Binder> binder = nullptr);

	DUCKDB_API unique_ptr<Expression> BindScalarFunction(const shared_ptr<const ScalarFunction> &function,
	                                                     vector<unique_ptr<Expression>> children,
	                                                     vector<pair<Identifier, unique_ptr<Expression>>> keyword_args,
	                                                     bool is_operator = false,
	                                                     optional_ptr<Binder> binder = nullptr);

	DUCKDB_API unique_ptr<Expression> BindScalarFunction(const ScalarFunction &function,
	                                                     vector<unique_ptr<Expression>> children,
	                                                     vector<pair<Identifier, unique_ptr<Expression>>> keyword_args,
	                                                     bool is_operator = false,
	                                                     optional_ptr<Binder> binder = nullptr);

	DUCKDB_API unique_ptr<BoundAggregateExpression>
	BindAggregateFunction(const shared_ptr<const AggregateFunction> &function, vector<unique_ptr<Expression>> children,
	                      unique_ptr<Expression> filter = nullptr,
	                      AggregateType aggr_type = AggregateType::NON_DISTINCT);

	//! Bind a function that does not come from a function set - the function is copied into a definition of its own
	DUCKDB_API unique_ptr<BoundAggregateExpression>
	BindAggregateFunction(const AggregateFunction &function, vector<unique_ptr<Expression>> children,
	                      unique_ptr<Expression> filter = nullptr,
	                      AggregateType aggr_type = AggregateType::NON_DISTINCT);

	DUCKDB_API unique_ptr<BoundAggregateExpression>
	BindAggregateFunction(const shared_ptr<const AggregateFunction> &function, vector<unique_ptr<Expression>> children,
	                      vector<pair<Identifier, unique_ptr<Expression>>> keyword_args, unique_ptr<Expression> filter,
	                      AggregateType aggr_type);

	DUCKDB_API unique_ptr<BoundAggregateExpression>
	BindAggregateFunction(const AggregateFunction &function, vector<unique_ptr<Expression>> children,
	                      vector<pair<Identifier, unique_ptr<Expression>>> keyword_args, unique_ptr<Expression> filter,
	                      AggregateType aggr_type);

	DUCKDB_API unique_ptr<BoundAggregateExpression>
	BindAggregateFunction(const AggregateFunctionCatalogEntry &function,
	                      vector<pair<Identifier, unique_ptr<Expression>>> arguments, ErrorData &error,
	                      unique_ptr<Expression> filter = nullptr,
	                      AggregateType aggr_type = AggregateType::NON_DISTINCT);
	DUCKDB_API unique_ptr<BoundAggregateExpression>
	BindAggregateFunction(const AggregateFunctionCatalogEntry &function, vector<unique_ptr<Expression>> children,
	                      ErrorData &error, unique_ptr<Expression> filter = nullptr,
	                      AggregateType aggr_type = AggregateType::NON_DISTINCT);

	DUCKDB_API static void BindSortedAggregate(ClientContext &context, BoundAggregateExpression &expr,
	                                           const vector<unique_ptr<Expression>> &groups,
	                                           optional_ptr<vector<GroupingSet>> grouping_sets);
	DUCKDB_API static void BindSortedAggregate(ClientContext &context, BoundWindowExpression &expr);

	//! Computes the exported buffer layout of an ordered aggregate: the struct of buffered columns (arguments first,
	//! then any appended sort keys), the per-key column + modifiers, and the number of leading argument columns.
	//! Mirrors the matching done by the sorted aggregate bind data so the export type matches the runtime buffer.
	DUCKDB_API static void GetSortedAggregateStateLayout(const BoundAggregateExpression &expr,
	                                                     LogicalType &buffer_struct,
	                                                     vector<SortedAggregateStateOrder> &orders,
	                                                     idx_t &argument_count);
	//! Reconstructs a sorted aggregate wrapper from an exported buffer state so finalize/combine operate on the buffer:
	//! finalize sorts by the keys and runs the (already re-bound) inner aggregate, combine concatenates buffers.
	//! Returns the wrapper function and its bind data.
	DUCKDB_API static pair<AggregateFunction, unique_ptr<FunctionData>>
	BindSortedAggregateState(ClientContext &context, const BoundAggregateFunction &inner_function,
	                         unique_ptr<FunctionData> inner_bind_info, const LogicalType &buffer_struct,
	                         const vector<SortedAggregateStateOrder> &orders, idx_t argument_count);

	DUCKDB_API unique_ptr<BoundWindowExpression>
	BindWindowFunction(const shared_ptr<const WindowFunction> &function, vector<unique_ptr<Expression>> children,
	                   vector<pair<Identifier, unique_ptr<Expression>>> keyword_args, vector<LogicalType> &order_types,
	                   vector<LogicalType> &arg_order_types);

	DUCKDB_API unique_ptr<BoundWindowExpression>
	BindWindowFunction(const WindowFunctionCatalogEntry &function,
	                   vector<pair<Identifier, unique_ptr<Expression>>> arguments, ErrorData &error,
	                   vector<LogicalType> &order_types, vector<LogicalType> &arg_order_types);

	DUCKDB_API unique_ptr<Expression> BindScalarWindowFunction(BoundWindowExpression &wexpr,
	                                                           const vector<LogicalType> &input_types);

	//! Turn the overload a call binds to into a bound function. The arguments are passed as they were to the
	//! selection, positional ones first. The overload is kept as the definition of the bound function
	DUCKDB_API pair<BoundScalarFunction, unique_ptr<FunctionData>>
	ResolveScalarFunction(const FunctionBinding &binding, vector<unique_ptr<Expression>> &children,
	                      vector<pair<Identifier, unique_ptr<Expression>>> &keyword_args);
	DUCKDB_API pair<BoundAggregateFunction, unique_ptr<FunctionData>>
	ResolveAggregateFunction(const FunctionBinding &binding, vector<unique_ptr<Expression>> &children,
	                         vector<pair<Identifier, unique_ptr<Expression>>> &keyword_args);
	//! Also resolves the types of the ORDER BY arguments, if they are passed
	DUCKDB_API pair<BoundWindowFunction, unique_ptr<FunctionData>>
	ResolveWindowFunction(const FunctionBinding &binding, vector<unique_ptr<Expression>> &children,
	                      vector<pair<Identifier, unique_ptr<Expression>>> &keyword_args,
	                      optional_ptr<vector<LogicalType>> order_types = nullptr,
	                      optional_ptr<vector<LogicalType>> arg_order_types = nullptr);

private:
	//! Resolves the argument / return types of the function and casts the children to them - everything that happens
	//! before the bind callback is invoked
	ResolvedScalarFunctionTypes
	ResolveScalarFunctionTypes(const FunctionBinding &binding, vector<unique_ptr<Expression>> &children,
	                           vector<pair<Identifier, unique_ptr<Expression>>> &keyword_args);

	unique_ptr<Expression> BindScalarFunction(const FunctionBinding &binding, vector<unique_ptr<Expression>> children,
	                                          vector<pair<Identifier, unique_ptr<Expression>>> keyword_args,
	                                          bool is_operator, optional_ptr<Binder> binder);
	unique_ptr<BoundAggregateExpression>
	BindAggregateFunction(FunctionBinding binding, vector<unique_ptr<Expression>> children,
	                      vector<pair<Identifier, unique_ptr<Expression>>> keyword_args, unique_ptr<Expression> filter,
	                      AggregateType aggr_type);
	unique_ptr<BoundWindowExpression> BindWindowFunction(const FunctionBinding &binding,
	                                                     vector<unique_ptr<Expression>> children,
	                                                     vector<pair<Identifier, unique_ptr<Expression>>> keyword_args,
	                                                     vector<LogicalType> &order_types,
	                                                     vector<LogicalType> &arg_order_types);
	//! Cast the arguments of a call to the types of the parameters they fill
	void CastToFunctionArguments(const Identifier &function_name, vector<LogicalType> &argument_types,
	                             vector<unique_ptr<Expression>> &children);

	//! Select the best matching overload for the given full (maybe-named) argument list, and split it into the
	//! positional and named arguments. Names positional arguments by their alias if that is the only way the call
	//! matches an overload that captures argument aliases
	optional<FunctionBinding> SelectFunctionFromArguments(const BaseFunctionSet &functions,
	                                                      vector<pair<Identifier, unique_ptr<Expression>>> arguments,
	                                                      vector<unique_ptr<Expression>> &positional,
	                                                      vector<pair<Identifier, unique_ptr<Expression>>> &named,
	                                                      ErrorData &error);

	//! Whether an overload matches a call - with how the call binds to it if it does, and why not if it does not. With
	//! explicit casts, an argument that cannot be implicitly cast to its parameter is accepted too
	static std::variant<FunctionBinding, FunctionMismatch>
	MatchOverload(optional_ptr<ClientContext> context, idx_t overload, shared_ptr<const SimpleFunction> function,
	              const FunctionCallArguments &call, bool explicit_casts);
	//! How a call binds to a function it is known to call - the arguments are cast explicitly. Throws if the call does
	//! not fit the signature of the function
	template <class T>
	FunctionBinding MatchFunction(const shared_ptr<const T> &function, const vector<unique_ptr<Expression>> &arguments,
	                              const vector<pair<Identifier, unique_ptr<Expression>>> &named_arguments);
};

} // namespace duckdb
