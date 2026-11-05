//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/scalar_function.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/vector_operations/binary_executor.hpp"
#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/common/vector_operations/ternary_executor.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/execution/expression_executor_state.hpp"
#include "duckdb/function/arg_properties.hpp"
#include "duckdb/function/function.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/enums/filter_propagate_result.hpp"

namespace duckdb {
class BaseStatistics;
class FunctionBinder;
struct ScalarFunctionInfo {
	DUCKDB_API virtual ~ScalarFunctionInfo();

	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}
	template <class TARGET>
	const TARGET &Cast() const {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<const TARGET &>(*this);
	}
};

//! Optional context passed to lambda bind callbacks
struct BindLambdaContext {
	virtual ~BindLambdaContext() = default;

	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}
	template <class TARGET>
	const TARGET &Cast() const {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<const TARGET &>(*this);
	}
};

class Binder;
class BoundFunctionExpression;
class BoundScalarFunction;
class ParsedExpression;
class ScalarFunctionCatalogEntry;

struct StatementProperties;

struct FunctionStatisticsPruneInput {
	FunctionStatisticsPruneInput(const BoundFunctionExpression &function_p, optional_ptr<FunctionData> bind_data_p,
	                             const vector<optional_ptr<const BaseStatistics>> &child_stats_p)
	    : function(function_p), bind_data(bind_data_p), child_stats(child_stats_p) {
	}

	//! The bound function expression being checked (gives access to the argument expressions)
	const BoundFunctionExpression &function;
	optional_ptr<FunctionData> bind_data;

	//! Statistics for each function argument (an entry is null if it could not be derived for that argument)
	const vector<optional_ptr<const BaseStatistics>> &child_stats;

	//! Convenience accessor: statistics of the i-th argument, or null if absent / not derivable
	optional_ptr<const BaseStatistics> ChildStats(idx_t i) const {
		return i < child_stats.size() ? child_stats[i] : optional_ptr<const BaseStatistics>();
	}
};

struct FunctionStatisticsInput {
	FunctionStatisticsInput(BoundFunctionExpression &expr_p, optional_ptr<FunctionData> bind_data_p,
	                        vector<BaseStatistics> &child_stats_p, unique_ptr<Expression> *expr_ptr_p)
	    : expr(expr_p), bind_data(bind_data_p), child_stats(child_stats_p), expr_ptr(expr_ptr_p) {
	}

	BoundFunctionExpression &expr;
	optional_ptr<FunctionData> bind_data;
	vector<BaseStatistics> &child_stats;
	unique_ptr<Expression> *expr_ptr;
};

struct FunctionModifiedDatabasesInput {
	FunctionModifiedDatabasesInput(optional_ptr<FunctionData> bind_data_p, StatementProperties &properties)
	    : bind_data(bind_data_p), properties(properties) {
	}

	optional_ptr<FunctionData> bind_data;
	StatementProperties &properties;
};

struct FunctionBindExpressionInput {
	FunctionBindExpressionInput(ClientContext &context_p, BoundScalarFunction &bound_function,
	                            optional_ptr<FunctionData> bind_data_p, vector<unique_ptr<Expression>> &children_p)
	    : context(context_p), bound_function(bound_function), bind_data(bind_data_p), children(children_p) {
	}

	ClientContext &context;
	BoundScalarFunction &bound_function;
	optional_ptr<FunctionData> bind_data;
	vector<unique_ptr<Expression>> &children;
};

struct FunctionUnbindInput {
	FunctionUnbindInput(const BoundFunctionExpression &expression_p, vector<unique_ptr<ParsedExpression>> children_p);
	~FunctionUnbindInput();

	const BoundFunctionExpression &expression;
	vector<unique_ptr<ParsedExpression>> children;
};

struct FunctionToStringInput {
	FunctionToStringInput(const BoundScalarFunction &bound_function, optional_ptr<FunctionData> bind_data_p,
	                      const vector<unique_ptr<Expression>> &children_p)
	    : bound_function(bound_function), bind_data(bind_data_p), children(children_p) {
	}

	const BoundScalarFunction &bound_function;
	optional_ptr<FunctionData> bind_data;
	const vector<unique_ptr<Expression>> &children;

	const Expression &GetChild(idx_t i) const {
		return *children[i];
	}
};

class BindScalarFunctionInput;
class ResolveScalarFunctionTypesInput;

//! The scalar function type
typedef std::function<void(DataChunk &, ExpressionState &, Vector &)> scalar_function_t;
//! The type to bind the scalar function and to create the function data
typedef unique_ptr<FunctionData> (*bind_scalar_function_t)(BindScalarFunctionInput &input);
//! The type to resolve the argument and return types of the scalar function, before casts are added to the arguments
typedef void (*resolve_scalar_types_t)(ResolveScalarFunctionTypesInput &input);
//! The type to initialize a thread local state for the scalar function
typedef unique_ptr<FunctionLocalState> (*init_local_state_t)(ExpressionState &state,
                                                             const BoundFunctionExpression &expr,
                                                             FunctionData *bind_data);
//! The type to directly access the selection vector of a scalar function
typedef idx_t (*scalar_function_select_t)(DataChunk &args, ExpressionState &state,
                                          optional_ptr<const SelectionVector> sel,
                                          optional_ptr<SelectionVector> true_sel,
                                          optional_ptr<SelectionVector> false_sel);
//! The type to propagate statistics for this scalar function
typedef unique_ptr<BaseStatistics> (*function_statistics_t)(ClientContext &context, FunctionStatisticsInput &input);

//! The type to bind lambda-specific parameter types
typedef LogicalType (*bind_lambda_function_t)(ClientContext &context, const vector<LogicalType> &function_child_types,
                                              idx_t parameter_idx, optional_ptr<BindLambdaContext> bind_lambda_context);

//! The type to bind lambda-specific parameter types
typedef void (*get_modified_databases_t)(ClientContext &context, FunctionModifiedDatabasesInput &input);

typedef void (*function_serialize_t)(Serializer &serializer, const optional_ptr<FunctionData> bind_data,
                                     const BoundScalarFunction &function);
typedef unique_ptr<FunctionData> (*function_deserialize_t)(Deserializer &deserializer, BoundScalarFunction &function);

//! The type to prune row groups based on statistics
typedef FilterPropagateResult (*propagate_filter_t)(const FunctionStatisticsPruneInput &input);

//! The type to bind lambda-specific parameter types
typedef unique_ptr<Expression> (*function_bind_expression_t)(FunctionBindExpressionInput &input);

//! Convert a scalar function to string
typedef string (*function_to_string_t)(FunctionToStringInput &input);

//! Reconstruct an invocation from children exported in the current binding scope
typedef unique_ptr<ParsedExpression> (*scalar_function_unbind_t)(FunctionUnbindInput &input);

//! Get the expression type of a function
typedef ExpressionType (*function_get_expression_type_t)(FunctionToStringInput &input);

//! Legacy serialize function for expressions that were converted into functions
typedef unique_ptr<Expression> (*function_legacy_serialize_t)(FunctionToStringInput &input);

class ScalarFunctionCallbacks {
public:
	//! The main scalar function to execute
	scalar_function_t function = nullptr;
	//! Direct selection callback (if any)
	scalar_function_select_t select_function = nullptr;
	//! Resolves the argument / return types (if any)
	resolve_scalar_types_t resolve_types = nullptr;
	//! The bind function (if any)
	bind_scalar_function_t bind = nullptr;
	//! Init thread local state for the function (if any)
	init_local_state_t init_local_state = nullptr;
	//! The statistics propagation function (if any)
	function_statistics_t statistics = nullptr;
	//! The lambda bind function (if any)
	bind_lambda_function_t bind_lambda = nullptr;
	//! Function to bind the result function expression directly (if any)
	function_bind_expression_t bind_expression = nullptr;
	//! Gets the modified databases (if any)
	get_modified_databases_t get_modified_databases = nullptr;
	//! Convert a scalar function to string
	function_to_string_t to_string = nullptr;
	//! Reconstruct the bound SQL invocation
	scalar_function_unbind_t unbind = nullptr;
	//! Get the expression type
	function_get_expression_type_t get_expression_type = nullptr;

	function_serialize_t serialize = nullptr;
	function_deserialize_t deserialize = nullptr;

	function_legacy_serialize_t legacy_serialize = nullptr;

	//! The filter prune function (if any)
	propagate_filter_t filter_prune = nullptr;

	bool operator==(const ScalarFunctionCallbacks &rhs) const;
	bool operator!=(const ScalarFunctionCallbacks &rhs) const;
};

//! Functions that can throw runtime errors must be marked as fallible using SetFallible(), since many parts of the
//! system rely on this (e.g. dictionary expression caching, filter pushdown, TRY)
//! Rethrows an execution error thrown by a function that is not marked as fallible as an internal error
[[noreturn]] DUCKDB_API void ThrowNonFallibleFunctionError(const Identifier &name, std::exception &ex);

template <class IMPL>
class BaseScalarFunction {
	friend IMPL; // Only allow the derived class to access the protected members of this class.
	BaseScalarFunction() = default;

public:
	// clang-format off
	auto GetProperties() const -> const FunctionProperties & { return properties; }
	auto GetProperties() -> FunctionProperties & { return properties; }
	auto SetProperties(const FunctionProperties &properties_p) -> void { properties = properties_p; }

	auto GetCallbacks() const -> const ScalarFunctionCallbacks & { return callbacks; }
	auto GetCallbacks() -> ScalarFunctionCallbacks & { return callbacks; }
	auto SetCallbacks(const ScalarFunctionCallbacks &callbacks_p) -> void { callbacks = callbacks_p; }

public: // Properties

	auto GetStability() const -> FunctionStability { return properties.stability; }
	auto SetStability(FunctionStability value) -> void { properties.stability = value; }

	auto GetNullHandling() const -> FunctionNullHandling { return properties.null_handling; }
	auto SetNullHandling(FunctionNullHandling value) -> void { properties.null_handling = value; }

	auto GetErrorMode() const -> FunctionErrors { return properties.errors; }
	auto SetErrorMode(FunctionErrors value) -> void { properties.errors = value; }

	auto GetCollationHandling() const -> FunctionCollationHandling { return properties.collation_handling; }
	auto SetCollationHandling(FunctionCollationHandling value) -> void { properties.collation_handling = value; }

	auto GetCaptureArgumentAliases() const -> bool { return properties.capture_argument_aliases; }
	auto SetCaptureArgumentAliases(bool value) -> void { properties.capture_argument_aliases = value; }

	auto RequiresOrderedExecution() const -> bool { return properties.requires_ordered_execution; }
	auto SetRequiresOrderedExecution(bool value) -> void { properties.requires_ordered_execution = value; }

	//! Set this functions error-mode as fallible (can throw runtime errors)
	void SetFallible() { properties.errors = FunctionErrors::CAN_THROW_RUNTIME_ERROR; }
	//! Set this functions stability as volatile (can not be cached per row)
	void SetVolatile() { properties.stability = FunctionStability::VOLATILE; }

public: // Callbacks

	auto HasFunctionCallback() const -> bool { return callbacks.function != nullptr; }
	auto GetFunctionCallback() const -> scalar_function_t { return callbacks.function; }
	auto SetFunctionCallback(scalar_function_t callback) -> void { callbacks.function = std::move(callback); }

	auto HasSelectCallback() const -> bool { return callbacks.select_function != nullptr; }
	auto GetSelectCallback() const -> scalar_function_select_t { return callbacks.select_function; }
	auto SetSelectCallback(scalar_function_select_t callback) -> void { callbacks.select_function = callback; }

	auto HasResolveTypesCallback() const -> bool { return callbacks.resolve_types != nullptr; }
	auto GetResolveTypesCallback() const -> resolve_scalar_types_t { return callbacks.resolve_types; }
	auto SetResolveTypesCallback(resolve_scalar_types_t callback) -> void { callbacks.resolve_types = callback; }

	auto HasBindCallback() const -> bool { return callbacks.bind != nullptr; };
	auto GetBindCallback() const -> bind_scalar_function_t { return callbacks.bind; };
	auto SetBindCallback(bind_scalar_function_t callback) -> void { callbacks.bind = callback; }

	auto HasBindLambdaCallback() const -> bool { return callbacks.bind_lambda != nullptr; }
	auto GetBindLambdaCallback() const -> bind_lambda_function_t { return callbacks.bind_lambda; }
	auto SetBindLambdaCallback(bind_lambda_function_t callback) -> void { callbacks.bind_lambda = callback; }

	auto HasBindExpressionCallback() const -> bool { return callbacks.bind_expression != nullptr; }
	auto GetBindExpressionCallback() const -> function_bind_expression_t { return callbacks.bind_expression; }
	auto SetBindExpressionCallback(function_bind_expression_t callback) -> void { callbacks.bind_expression = callback; }

	auto HasInitStateCallback() const -> bool { return callbacks.init_local_state != nullptr; }
	auto GetInitStateCallback() const -> init_local_state_t { return callbacks.init_local_state; }
	auto SetInitStateCallback(init_local_state_t callback) -> void { callbacks.init_local_state = callback; }

	auto HasStatisticsCallback() const -> bool { return callbacks.statistics != nullptr; }
	auto GetStatisticsCallback() const -> function_statistics_t { return callbacks.statistics; }
	auto SetStatisticsCallback(function_statistics_t callback) -> void { callbacks.statistics = callback; }

	auto HasModifiedDatabasesCallback() const -> bool { return callbacks.get_modified_databases != nullptr; }
	auto GetModifiedDatabasesCallback() const -> get_modified_databases_t { return callbacks.get_modified_databases; }
	auto SetModifiedDatabasesCallback(get_modified_databases_t callback) -> void { callbacks.get_modified_databases = callback; }

	auto HasSerializationCallbacks() const -> bool { return callbacks.serialize != nullptr && callbacks.deserialize != nullptr; }
	auto SetSerializeCallback(function_serialize_t callback) -> void { callbacks.serialize = callback; }
	auto SetDeserializeCallback(function_deserialize_t callback) -> void { callbacks.deserialize = callback; }
	auto GetSerializeCallback() const -> function_serialize_t { return callbacks.serialize; }
	auto GetDeserializeCallback() const -> function_deserialize_t { return callbacks.deserialize; }

	auto HasFilterPruneCallback() const -> bool { return callbacks.filter_prune != nullptr; }
	auto SetFilterPruneCallback(propagate_filter_t callback) -> void { callbacks.filter_prune = callback; }
	auto GetFilterPruneCallback() const -> propagate_filter_t { return callbacks.filter_prune; }

	auto HasToStringCallback() const -> bool { return callbacks.to_string != nullptr; }
	auto SetToStringCallback(function_to_string_t callback) -> void { callbacks.to_string = callback; }
	auto FunctionToString(FunctionToStringInput &input) const -> string { return callbacks.to_string(input); }

	auto HasUnbindCallback() const -> bool { return callbacks.unbind != nullptr; }
	auto SetUnbindCallback(scalar_function_unbind_t callback) -> void { callbacks.unbind = callback; }
	auto GetUnbindCallback() const -> scalar_function_unbind_t { return callbacks.unbind; }

	auto HasLegacySerializeCallback() const -> bool { return callbacks.legacy_serialize != nullptr; }
	auto SetLegacySerializeCallback(function_legacy_serialize_t callback) -> void { callbacks.legacy_serialize = callback; }
	auto GetLegacySerializeCallback() const -> function_legacy_serialize_t { return callbacks.legacy_serialize; }

	auto SetGetExpressionTypeCallback(function_get_expression_type_t callback) -> void { callbacks.get_expression_type = callback; }
	auto GetExpressionType(FunctionToStringInput &input) const -> ExpressionType {
		if (callbacks.get_expression_type) {
			return callbacks.get_expression_type(input);
		}
		return ExpressionType::BOUND_FUNCTION;
	}
	// clang-format on

public: // Execution
	//! Execute the function, verifying that functions that are not marked as fallible do not throw execution errors
	void Execute(DataChunk &args, ExpressionState &state, Vector &result) const {
		if (properties.errors != FunctionErrors::CANNOT_ERROR) {
			callbacks.function(args, state, result);
			return;
		}
		try {
			callbacks.function(args, state, result);
		} catch (std::exception &ex) {
			ThrowNonFallibleFunctionError(Name(), ex);
		}
	}

	//! Execute the select callback of the function, verifying that non-fallible functions do not throw
	idx_t Select(DataChunk &args, ExpressionState &state, const SelectionVector *sel, SelectionVector *true_sel,
	             SelectionVector *false_sel) const {
		if (properties.errors != FunctionErrors::CANNOT_ERROR) {
			return callbacks.select_function(args, state, sel, true_sel, false_sel);
		}
		try {
			return callbacks.select_function(args, state, sel, true_sel, false_sel);
		} catch (std::exception &ex) {
			ThrowNonFallibleFunctionError(Name(), ex);
		}
	}

public:
	bool HasExtraFunctionInfo() const {
		return function_info != nullptr;
	}
	ScalarFunctionInfo &GetExtraFunctionInfo() const {
		D_ASSERT(function_info.get());
		return *function_info;
	}
	void SetExtraFunctionInfo(shared_ptr<ScalarFunctionInfo> info) {
		function_info = std::move(info);
	}
	template <class T, class... ARGS>
	void SetExtraFunctionInfo(ARGS &&... args) {
		function_info = make_shared_ptr<T>(std::forward<ARGS>(args)...);
	}
	shared_ptr<ScalarFunctionInfo> GetFunctionInfo() const {
		return function_info;
	}

private:
	const Identifier &Name() const {
		return static_cast<const IMPL &>(*this).GetName();
	}

protected:
	FunctionProperties properties;

private:
	ScalarFunctionCallbacks callbacks;

protected:
	shared_ptr<ScalarFunctionInfo> function_info;

	//! Per-argument declarative properties (monotonicity). Empty = no claims made.
	vector<ArgProperties> arg_props;

public:
	//! Per-argument declarative properties. Returns a default ArgProperties when no annotation exists.
	const ArgProperties &GetArgProperties(idx_t arg_idx) const {
		static const ArgProperties unknown;
		return arg_idx < arg_props.size() ? arg_props[arg_idx] : unknown;
	}
	const vector<ArgProperties> &GetAllArgProperties() const {
		return arg_props;
	}
	bool HasArgProperties() const {
		return !arg_props.empty();
	}
	IMPL &SetArgProperties(idx_t arg_idx, ArgProperties props) {
		if (arg_props.size() <= arg_idx) {
			arg_props.resize(arg_idx + 1);
		}
		arg_props[arg_idx] = props;
		return *static_cast<IMPL *>(this);
	}
	IMPL &SetArgProperties(vector<ArgProperties> props) {
		arg_props = std::move(props);
		return *static_cast<IMPL *>(this);
	}
	ScalarFunction &SetUnaryArgProperties(ArgProperties props) {
		return SetArgProperties(0, props);
	}
};

class ScalarFunction : public BaseScalarFunction<ScalarFunction>,
                       public SimpleFunction { // NOLINT: work-around bug in clang-tidy
public:
	static constexpr const FunctionKind KIND = FunctionKind::SCALAR;
	FunctionKind GetKind() const override {
		return KIND;
	}
	//! The properties of the function, for code that only knows it as a SimpleFunction
	auto GetFunctionProperties() const -> optional_ptr<const FunctionProperties> override {
		return GetProperties();
	}
	DUCKDB_API ScalarFunction(Identifier name, FunctionSignature sig, scalar_function_t function);
	DUCKDB_API ScalarFunction(Identifier name, vector<LogicalType> arguments, LogicalType return_type,
	                          scalar_function_t function, bind_scalar_function_t bind = nullptr,
	                          function_statistics_t statistics = nullptr, init_local_state_t init_local_state = nullptr,
	                          LogicalType varargs = LogicalType(LogicalTypeId::INVALID),
	                          FunctionStability stability = FunctionStability::CONSISTENT,
	                          FunctionNullHandling null_handling = FunctionNullHandling::DEFAULT_NULL_HANDLING,
	                          bind_lambda_function_t bind_lambda = nullptr);

	DUCKDB_API ScalarFunction(vector<LogicalType> arguments, LogicalType return_type, scalar_function_t function,
	                          bind_scalar_function_t bind = nullptr, function_statistics_t statistics = nullptr,
	                          init_local_state_t init_local_state = nullptr,
	                          LogicalType varargs = LogicalType(LogicalTypeId::INVALID),
	                          FunctionStability stability = FunctionStability::CONSISTENT,
	                          FunctionNullHandling null_handling = FunctionNullHandling::DEFAULT_NULL_HANDLING,
	                          bind_lambda_function_t bind_lambda = nullptr);

	DUCKDB_API ScalarFunction(Identifier name, std::initializer_list<FunctionParameter> params, LogicalType return_type,
	                          scalar_function_t function, bind_scalar_function_t bind = nullptr,
	                          function_statistics_t statistics = nullptr, init_local_state_t init_local_state = nullptr,
	                          LogicalType varargs = LogicalType(LogicalTypeId::INVALID),
	                          FunctionStability stability = FunctionStability::CONSISTENT,
	                          FunctionNullHandling null_handling = FunctionNullHandling::DEFAULT_NULL_HANDLING,
	                          bind_lambda_function_t bind_lambda = nullptr);

	DUCKDB_API ScalarFunction(std::initializer_list<FunctionParameter> params, LogicalType return_type,
	                          scalar_function_t function, bind_scalar_function_t bind = nullptr,
	                          function_statistics_t statistics = nullptr, init_local_state_t init_local_state = nullptr,
	                          LogicalType varargs = LogicalType(LogicalTypeId::INVALID),
	                          FunctionStability stability = FunctionStability::CONSISTENT,
	                          FunctionNullHandling null_handling = FunctionNullHandling::DEFAULT_NULL_HANDLING,
	                          bind_lambda_function_t bind_lambda = nullptr);

	DUCKDB_API bool operator==(const ScalarFunction &rhs) const;
	DUCKDB_API bool operator!=(const ScalarFunction &rhs) const;

public:
	unique_ptr<BoundFunctionExpression> Bind(ClientContext &context, vector<unique_ptr<Expression>> arguments,
	                                         optional_ptr<Binder> binder = nullptr) const;

public:
	DUCKDB_API static void NopFunction(DataChunk &input, ExpressionState &state, Vector &result);

	template <class TA, class TR, class OP>
	static void UnaryFunction(DataChunk &input, ExpressionState &state, Vector &result) {
		D_ASSERT(input.ColumnCount() >= 1);
		UnaryExecutor::Execute<TA, TR, OP>(input.data[0], result);
	}

	template <class TA, class TB, class TR, class OP>
	static void BinaryFunction(DataChunk &input, ExpressionState &state, Vector &result) {
		D_ASSERT(input.ColumnCount() == 2);
		BinaryExecutor::ExecuteStandard<TA, TB, TR, OP>(input.data[0], input.data[1], result);
	}

	template <class TA, class TB, class TC, class TR, class OP>
	static void TernaryFunction(DataChunk &input, ExpressionState &state, Vector &result) {
		D_ASSERT(input.ColumnCount() == 3);
		TernaryExecutor::ExecuteStandard<TA, TB, TC, TR, OP>(input.data[0], input.data[1], input.data[2], result);
	}

public:
	template <class OP>
	static scalar_function_t GetScalarUnaryFunction(const LogicalType &type) {
		scalar_function_t function;
		switch (type.id()) {
		case LogicalTypeId::TINYINT:
			function = &ScalarFunction::UnaryFunction<int8_t, int8_t, OP>;
			break;
		case LogicalTypeId::SMALLINT:
			function = &ScalarFunction::UnaryFunction<int16_t, int16_t, OP>;
			break;
		case LogicalTypeId::INTEGER:
			function = &ScalarFunction::UnaryFunction<int32_t, int32_t, OP>;
			break;
		case LogicalTypeId::BIGINT:
			function = &ScalarFunction::UnaryFunction<int64_t, int64_t, OP>;
			break;
		case LogicalTypeId::UTINYINT:
			function = &ScalarFunction::UnaryFunction<uint8_t, uint8_t, OP>;
			break;
		case LogicalTypeId::USMALLINT:
			function = &ScalarFunction::UnaryFunction<uint16_t, uint16_t, OP>;
			break;
		case LogicalTypeId::UINTEGER:
			function = &ScalarFunction::UnaryFunction<uint32_t, uint32_t, OP>;
			break;
		case LogicalTypeId::UBIGINT:
			function = &ScalarFunction::UnaryFunction<uint64_t, uint64_t, OP>;
			break;
		case LogicalTypeId::HUGEINT:
			function = &ScalarFunction::UnaryFunction<hugeint_t, hugeint_t, OP>;
			break;
		case LogicalTypeId::UHUGEINT:
			function = &ScalarFunction::UnaryFunction<uhugeint_t, uhugeint_t, OP>;
			break;
		case LogicalTypeId::FLOAT:
			function = &ScalarFunction::UnaryFunction<float, float, OP>;
			break;
		case LogicalTypeId::DOUBLE:
			function = &ScalarFunction::UnaryFunction<double, double, OP>;
			break;
		default:
			throw InternalException("Unimplemented type for GetScalarUnaryFunction");
		}
		return function;
	}

	template <class TR, class OP>
	static scalar_function_t GetScalarUnaryFunctionFixedReturn(const LogicalType &type) {
		scalar_function_t function;
		switch (type.id()) {
		case LogicalTypeId::TINYINT:
			function = &ScalarFunction::UnaryFunction<int8_t, TR, OP>;
			break;
		case LogicalTypeId::SMALLINT:
			function = &ScalarFunction::UnaryFunction<int16_t, TR, OP>;
			break;
		case LogicalTypeId::INTEGER:
			function = &ScalarFunction::UnaryFunction<int32_t, TR, OP>;
			break;
		case LogicalTypeId::BIGINT:
			function = &ScalarFunction::UnaryFunction<int64_t, TR, OP>;
			break;
		case LogicalTypeId::UTINYINT:
			function = &ScalarFunction::UnaryFunction<uint8_t, TR, OP>;
			break;
		case LogicalTypeId::USMALLINT:
			function = &ScalarFunction::UnaryFunction<uint16_t, TR, OP>;
			break;
		case LogicalTypeId::UINTEGER:
			function = &ScalarFunction::UnaryFunction<uint32_t, TR, OP>;
			break;
		case LogicalTypeId::UBIGINT:
			function = &ScalarFunction::UnaryFunction<uint64_t, TR, OP>;
			break;
		case LogicalTypeId::HUGEINT:
			function = &ScalarFunction::UnaryFunction<hugeint_t, TR, OP>;
			break;
		case LogicalTypeId::UHUGEINT:
			function = &ScalarFunction::UnaryFunction<uhugeint_t, TR, OP>;
			break;
		case LogicalTypeId::FLOAT:
			function = &ScalarFunction::UnaryFunction<float, TR, OP>;
			break;
		case LogicalTypeId::DOUBLE:
			function = &ScalarFunction::UnaryFunction<double, TR, OP>;
			break;
		default:
			throw InternalException("Unimplemented type for GetScalarUnaryFunctionFixedReturn");
		}
		return function;
	}
};

class BoundScalarFunction : public BaseScalarFunction<BoundScalarFunction>, public BoundSimpleFunction {
public:
	explicit BoundScalarFunction(const ScalarFunction &function);
	explicit BoundScalarFunction(shared_ptr<const ScalarFunction> function);

	bool operator==(const BoundScalarFunction &rhs) const;
	bool operator!=(const BoundScalarFunction &rhs) const;

public:
	//! The function this was bound from. Unaffected by later mutation of the bound function, e.g. statistics
	//! propagation swapping in a specialized implementation. For a function bound from a ScalarFunctionSet this is
	//! the set's own overload, so it compares equal by pointer across binds. Functions bound outside of a set are
	//! copied into a definition of their own.
	//! Only null in a moved-from bound function.
	const shared_ptr<const ScalarFunction> &GetDefinition() const {
		return definition;
	}
	//! The number of arguments that were received by the standard and positional-only parameters, they come first
	idx_t GetStandardArgumentCount() const {
		return BoundSimpleFunction::GetStandardArgumentCount(definition->GetSignature());
	}
	//! The number of arguments that were received by "*args", they directly follow the standard parameters
	idx_t GetVarArgsCount() const {
		return BoundSimpleFunction::GetVarArgsCount(definition->GetSignature());
	}
	//! The number of arguments that were received by the keyword-only parameters, they follow "*args"
	idx_t GetKeywordOnlyArgumentCount() const {
		return BoundSimpleFunction::GetKeywordOnlyArgumentCount(definition->GetSignature());
	}
	//! The number of arguments that were received by "**kwargs", they are the last arguments
	idx_t GetKwargsCount() const {
		return BoundSimpleFunction::GetKwargsCount(definition->GetSignature());
	}
	//! The kind of the parameter that received the argument at the given index
	FunctionParameterKind GetArgumentParameterKind(idx_t argument_index) const {
		return BoundSimpleFunction::GetArgumentParameterKind(definition->GetSignature(), argument_index);
	}
	//! Restore the definition after the bound function has been replaced wholesale, together with the
	//! qualification it carries - the replacement is a specialized implementation, not a different function
	void SetDefinition(shared_ptr<const ScalarFunction> definition_p) {
		definition = std::move(definition_p);
		if (definition) {
			qualified_name = definition->GetQualifiedName().WithName(GetName());
		}
	}
	const vector<LogicalType> &GetLogicalArguments() const {
		return logical_arguments;
	}
	const LogicalType &GetLogicalReturnType() const {
		return logical_return_type;
	}

private:
	void SetLogicalArguments(vector<LogicalType> arguments_p) {
		logical_arguments = std::move(arguments_p);
	}
	void SetLogicalReturnType(LogicalType return_type_p) {
		logical_return_type = std::move(return_type_p);
	}
	shared_ptr<const ScalarFunction> definition;
	vector<LogicalType> logical_arguments;
	LogicalType logical_return_type;

	friend class FunctionSerializer;
	friend class FunctionBinder;
};

class BindScalarFunctionInput : public BindFunctionInput {
public:
	BindScalarFunctionInput(ClientContext &context_p, BoundScalarFunction &bound_function_p,
	                        vector<unique_ptr<Expression>> &arguments_p, optional_ptr<Binder> binder_p = nullptr)
	    : BindFunctionInput(context_p, bound_function_p, bound_function_p.GetDefinition()->GetSignature(), arguments_p),
	      bound_function(bound_function_p), binder(binder_p) {
	}

	BoundScalarFunction &GetBoundFunction() const {
		return bound_function;
	}
	bool HasBinder() const {
		return binder != nullptr;
	}
	Binder &GetBinder() {
		if (binder == nullptr) {
			throw InternalException("Function %s has cannot be bound without a Binder!", bound_function.GetName());
		}
		return *binder;
	}

private:
	BoundScalarFunction &bound_function;
	optional_ptr<Binder> binder;
};

//! Input of the resolve_types callback - the arguments are read-only, the bound function can be modified
class ResolveScalarFunctionTypesInput : private BindFunctionInput {
public:
	ResolveScalarFunctionTypesInput(ClientContext &context_p, BoundScalarFunction &bound_function_p,
	                                vector<unique_ptr<Expression>> &arguments_p)
	    : BindFunctionInput(context_p, bound_function_p, bound_function_p.GetDefinition()->GetSignature(), arguments_p),
	      bound_function(bound_function_p), arguments(arguments_p) {
	}

	using BindFunctionInput::GetArgumentName;
	using BindFunctionInput::GetClientContext;
	using BindFunctionInput::GetConstant;
	using BindFunctionInput::GetNonNullConstant;
	using BindFunctionInput::TryGetConstant;

	BoundScalarFunction &GetBoundFunction() const {
		return bound_function;
	}
	//! The argument expressions - these must not be modified
	const vector<unique_ptr<Expression>> &GetArguments() const {
		return arguments;
	}
	idx_t GetArgumentCount() const {
		return arguments.size();
	}
	DUCKDB_API const Expression &GetArgument(idx_t arg_idx) const;
	//! The type of the argument expression, before it is cast to the argument type of the bound function
	DUCKDB_API const LogicalType &GetArgumentType(idx_t arg_idx) const;
	DUCKDB_API vector<LogicalType> GetArgumentTypes() const;

private:
	BoundScalarFunction &bound_function;
	const vector<unique_ptr<Expression>> &arguments;
};

} // namespace duckdb
