#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/parser/parsed_expression.hpp"

namespace duckdb {

FunctionUnbindInput::FunctionUnbindInput(const BoundFunctionExpression &expression_p,
                                         vector<unique_ptr<ParsedExpression>> children_p)
    : expression(expression_p), children(std::move(children_p)) {
}

FunctionUnbindInput::~FunctionUnbindInput() {
}

void ThrowNonFallibleFunctionError(const Identifier &name, std::exception &ex) {
	ErrorData error(ex);
	if (!Exception::IsExecutionError(error.Type())) {
		throw;
	}
	throw InternalException("Scalar function %s threw an execution error, but the function is not marked as "
	                        "fallible - the function must call SetFallible(). Error: %s",
	                        name, error.RawMessage());
}

const Expression &ResolveScalarFunctionTypesInput::GetArgument(idx_t arg_idx) const {
	D_ASSERT(arg_idx < arguments.size());
	return *arguments[arg_idx];
}

const LogicalType &ResolveScalarFunctionTypesInput::GetArgumentType(idx_t arg_idx) const {
	return GetArgument(arg_idx).GetReturnType();
}

vector<LogicalType> ResolveScalarFunctionTypesInput::GetArgumentTypes() const {
	vector<LogicalType> result;
	result.reserve(arguments.size());
	for (auto &argument : arguments) {
		result.push_back(argument->GetReturnType());
	}
	return result;
}

bool ScalarFunctionCallbacks::operator==(const ScalarFunctionCallbacks &rhs) const {
	return resolve_types == rhs.resolve_types && bind == rhs.bind && init_local_state == rhs.init_local_state &&
	       statistics == rhs.statistics && bind_lambda == rhs.bind_lambda && bind_expression == rhs.bind_expression &&
	       get_modified_databases == rhs.get_modified_databases && serialize == rhs.serialize &&
	       deserialize == rhs.deserialize && filter_prune == rhs.filter_prune && unbind == rhs.unbind;
}

bool ScalarFunctionCallbacks::operator!=(const ScalarFunctionCallbacks &rhs) const {
	return !(*this == rhs);
}

FunctionLocalState::~FunctionLocalState() {
}

ScalarFunctionInfo::~ScalarFunctionInfo() {
}

ScalarFunction::ScalarFunction(Identifier name, FunctionSignature sig, scalar_function_t function)
    : SimpleFunction(std::move(name), std::move(sig)) {
	SetFunctionCallback(std::move(function));
}

ScalarFunction::ScalarFunction(Identifier name, vector<LogicalType> arguments, LogicalType return_type,
                               scalar_function_t function, bind_scalar_function_t bind,
                               function_statistics_t statistics, init_local_state_t init_local_state,
                               LogicalType varargs, FunctionStability side_effects, FunctionNullHandling null_handling,
                               bind_lambda_function_t bind_lambda)
    : SimpleFunction(std::move(name), std::move(arguments), std::move(return_type), std::move(varargs)) {
	properties.stability = side_effects;
	properties.null_handling = null_handling;

	SetFunctionCallback(std::move(function));
	callbacks.bind = bind;
	callbacks.init_local_state = init_local_state;
	callbacks.statistics = statistics;
	callbacks.bind_lambda = bind_lambda;
}

ScalarFunction::ScalarFunction(vector<LogicalType> arguments, LogicalType return_type, scalar_function_t function,
                               bind_scalar_function_t bind, function_statistics_t statistics,
                               init_local_state_t init_local_state, LogicalType varargs, FunctionStability side_effects,
                               FunctionNullHandling null_handling, bind_lambda_function_t bind_lambda)
    : ScalarFunction(Identifier(), std::move(arguments), std::move(return_type), std::move(function), bind, statistics,
                     init_local_state, std::move(varargs), side_effects, null_handling, bind_lambda) {
}

// (we take an initializer list to ensure that the function is not ambiguous with the other constructor)
ScalarFunction::ScalarFunction(Identifier name, std::initializer_list<FunctionParameter> params,
                               LogicalType return_type, scalar_function_t function, bind_scalar_function_t bind,
                               function_statistics_t statistics, init_local_state_t init_local_state,
                               LogicalType varargs, FunctionStability side_effects, FunctionNullHandling null_handling,
                               bind_lambda_function_t bind_lambda)
    : SimpleFunction(std::move(name), FunctionSignature(params, std::move(return_type))) {
	if (varargs.id() != LogicalTypeId::INVALID) {
		signature.AddArgs("args", varargs).AddKwargs("kwargs", std::move(varargs));
	}
	properties.stability = side_effects;
	properties.null_handling = null_handling;

	SetFunctionCallback(std::move(function));
	callbacks.bind = bind;
	callbacks.init_local_state = init_local_state;
	callbacks.statistics = statistics;
	callbacks.bind_lambda = bind_lambda;
}

ScalarFunction::ScalarFunction(std::initializer_list<FunctionParameter> params, LogicalType return_type,
                               scalar_function_t function, bind_scalar_function_t bind,
                               function_statistics_t statistics, init_local_state_t init_local_state,
                               LogicalType varargs, FunctionStability side_effects, FunctionNullHandling null_handling,
                               bind_lambda_function_t bind_lambda)
    : ScalarFunction(Identifier(), params, std::move(return_type), std::move(function), bind, statistics,
                     init_local_state, std::move(varargs), side_effects, null_handling, bind_lambda) {
}

bool ScalarFunction::operator==(const ScalarFunction &rhs) const {
	return name == rhs.name && signature == rhs.signature && callbacks == rhs.callbacks && properties == rhs.properties;
}

bool ScalarFunction::operator!=(const ScalarFunction &rhs) const {
	return !(*this == rhs);
}

void ScalarFunction::NopFunction(DataChunk &input, ExpressionState &state, Vector &result) {
	D_ASSERT(input.ColumnCount() >= 1);
	result.Reference(input.data[0]);
}

unique_ptr<BoundFunctionExpression> ScalarFunction::Bind(ClientContext &context,
                                                         vector<unique_ptr<Expression>> arguments,
                                                         optional_ptr<Binder> binder) const {
	FunctionBinder func_binder(context);
	auto expr = func_binder.BindScalarFunction(*this, std::move(arguments), binder);

	if (expr->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		throw InvalidInputException("BindScalarFunction did not return a BoundFunctionExpression");
	}

	return unique_ptr_cast<Expression, BoundFunctionExpression>(std::move(expr));
}

BoundScalarFunction::BoundScalarFunction(const ScalarFunction &function)
    // the function does not come from a function set - copy it into a definition of its own
    : BoundScalarFunction(make_shared_ptr<ScalarFunction>(function)) {
}

BoundScalarFunction::BoundScalarFunction(shared_ptr<const ScalarFunction> function_p)
    : definition(std::move(function_p)) {
	auto &function = *definition;
	qualified_name = function.GetQualifiedName();
	extra_info = function.extra_info;
	return_type = function.GetReturnType();
	callbacks = function.GetCallbacks();
	properties = function.GetProperties();
	function_info = function.GetFunctionInfo();
	arg_props = function.GetAllArgProperties();

	// Try to default bind the function, to fill in any missing information in the BoundScalarFunction (e.g. from the
	// "bind" callback)
	for (auto &param : function.GetSignature().GetParameters()) {
		if (!param.IsVariadic()) {
			arguments.push_back(param.GetType());
		}
	}
	positional_arguments = arguments.size();
	logical_arguments = arguments;
	logical_return_type = return_type;
}

bool BoundScalarFunction::operator==(const BoundScalarFunction &rhs) const {
	return callbacks == rhs.callbacks && properties == rhs.properties && GetName() == rhs.GetName() &&
	       return_type == rhs.return_type && arguments == rhs.arguments &&
	       positional_arguments == rhs.positional_arguments && named_arguments == rhs.named_arguments;
}
bool BoundScalarFunction::operator!=(const BoundScalarFunction &rhs) const {
	return !(*this == rhs);
}

} // namespace duckdb
