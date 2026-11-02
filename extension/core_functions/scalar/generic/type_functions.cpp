#include "core_functions/scalar/generic_functions.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/parser/expression/type_expression.hpp"
#include "duckdb/execution/expression_executor.hpp"

namespace duckdb {

//----------------------------------------------------------------------------------------------------------------------
// typeof function
//----------------------------------------------------------------------------------------------------------------------

static void TypeOfFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	Value v(args.data[0].GetType().ToString());
	result.Reference(v, count_t(args.size()));
}

static unique_ptr<Expression> BindTypeOfFunctionExpression(FunctionBindExpressionInput &input) {
	auto &return_type = input.children[0]->GetReturnType();
	if (return_type.id() == LogicalTypeId::UNKNOWN || return_type.id() == LogicalTypeId::SQLNULL) {
		// parameter - unknown return type
		return nullptr;
	}
	// emit a constant expression
	return make_uniq<BoundConstantExpression>(Value(return_type.ToString()));
}

ScalarFunction TypeOfFun::GetFunction() {
	auto fun = ScalarFunction({}, LogicalType::VARCHAR, TypeOfFunction);
	fun.GetSignature().AddParameter("expression", LogicalType::ANY);
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	fun.SetBindExpressionCallback(BindTypeOfFunctionExpression);
	return fun;
}

//----------------------------------------------------------------------------------------------------------------------
// get_type	function
//----------------------------------------------------------------------------------------------------------------------
// This is like "typeof", except returns LogicalType::TYPE instead of VARCHAR

static void GetTypeFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto v = Value::TYPE(args.data[0].GetType());
	result.Reference(v, count_t(args.size()));
}

static unique_ptr<FunctionData> BindGetTypeFunction(BindScalarFunctionInput &input) {
	auto &bound_function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	if (arguments[0]->HasParameter()) {
		throw ParameterNotResolvedException();
	}
	bound_function.GetArguments()[0] = arguments[0]->GetReturnType();
	return nullptr;
}

static unique_ptr<Expression> BindGetTypeFunctionExpression(FunctionBindExpressionInput &input) {
	auto &return_type = input.children[0]->GetReturnType();
	if (return_type.id() == LogicalTypeId::UNKNOWN || return_type.id() == LogicalTypeId::SQLNULL) {
		// parameter - unknown return type
		return nullptr;
	}
	// emit a constant expression
	return make_uniq<BoundConstantExpression>(Value::TYPE(return_type));
}

ScalarFunction GetTypeFun::GetFunction() {
	auto fun = ScalarFunction({}, LogicalType::TYPE(), GetTypeFunction, BindGetTypeFunction);
	fun.GetSignature().AddParameter("expression", LogicalType::ANY);
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	fun.SetBindExpressionCallback(BindGetTypeFunctionExpression);
	return fun;
}

//----------------------------------------------------------------------------------------------------------------------
// make_type function
//----------------------------------------------------------------------------------------------------------------------
static void MakeTypeFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	throw InvalidInputException("make_type function can only be used in constant expressions");
}

static unique_ptr<Expression> BindMakeTypeFunctionExpression(FunctionBindExpressionInput &input) {
	vector<pair<string, Value>> args;

	// Evaluate all arguments to constant values
	for (auto &child : input.children) {
		string name = child->GetAlias().GetIdentifierName();
		if (!child->IsFoldable()) {
			throw BinderException("make_type function arguments must be constant expressions");
		}
		auto val = ExpressionExecutor::EvaluateScalar(input.context, *child);
		args.emplace_back(name, val);
	}

	if (args.empty()) {
		throw BinderException("make_type function requires at least one argument");
	}

	if (args.front().second.type() != LogicalType::VARCHAR) {
		throw BinderException("make_type function first argument must be the type name as VARCHAR");
	}

	vector<unique_ptr<ParsedExpression>> type_args;
	for (idx_t i = 1; i < args.size(); i++) {
		auto &arg = args[i];
		auto result = ConstantExpression::FromValue(arg.second);
		result->SetAlias(Identifier(arg.first));

		type_args.push_back(std::move(result));
	}

	auto type_name = args.front().second.GetValue<string>();
	QualifiedName qualified_name;
	string error;
	if (!QualifiedName::TryParse(type_name, qualified_name, error)) {
		throw ParserException(error);
	}

	auto unbound_type =
	    LogicalType::UNBOUND(make_uniq<TypeExpression>(std::move(qualified_name), std::move(type_args)));

	// Bind the unbound type
	auto binder = Binder::CreateBinder(input.context);
	binder->BindLogicalType(unbound_type);
	return make_uniq<BoundConstantExpression>(Value::TYPE(unbound_type));
}

ScalarFunction MakeTypeFun::GetFunction() {
	auto fun = ScalarFunction({}, LogicalType::TYPE(), MakeTypeFunction);
	fun.GetSignature().AddParameter("name", LogicalType::VARCHAR);
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	fun.SetBindExpressionCallback(BindMakeTypeFunctionExpression);
	fun.GetProperties().SetRequiresExpressionNames(true);
	fun.GetSignature().AddArgs("args", LogicalType::ANY).AddKwargs("kwargs", LogicalType::ANY);
	return fun;
}

} // namespace duckdb
