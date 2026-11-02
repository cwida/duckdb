#include "duckdb/function/scalar/compressed_materialization_utils.hpp"
#include "duckdb/function/scalar/compressed_materialization_functions.hpp"
#include "duckdb/function/scalar/operator_functions.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

namespace duckdb {

ScalarFunction InternalCompressedMaterializationCastFun::GetFunction() {
	auto function = CastFun::GetFunction();
	function.SetQualifiedName(QualifiedName("system", "main", Name));
	function.SetBindCallback(CMUtils::Bind);
	return function;
}

void CMUtils::MarkCast(BoundFunctionExpression &expression) {
	D_ASSERT(BoundCastExpression::IsCast(expression));
	D_ASSERT(BoundCastExpression::IsDefaultCast(expression));
	auto definition = make_shared_ptr<ScalarFunction>(InternalCompressedMaterializationCastFun::GetFunction());
	auto &function = expression.FunctionMutable();
	function.SetName(definition->GetName());
	function.SetBindCallback(CMUtils::Bind);
	function.SetDefinition(std::move(definition));
}

CMExpressionType CMUtils::GetExpressionType(const BoundFunctionExpression &expression) {
	auto &function = expression.Function();
	if (function.GetBindCallback() != Bind) {
		return CMExpressionType::NONE;
	}
	if (function.GetName() == InternalCompressedMaterializationCastFun::Name &&
	    function.GetDeserializeCallback() == CastFun::GetFunction().GetDeserializeCallback()) {
		return CMExpressionType::CAST;
	}
	auto type = GetIntegralType(function);
	if (type == CMExpressionType::NONE) {
		type = GetStringType(function);
	}
	if (type == CMExpressionType::NONE) {
		type = GetGeometryType(function);
	}
	return type;
}

optional_ptr<const Expression> CMUtils::GetWrappedInput(const Expression &expression) {
	if (expression.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return nullptr;
	}
	auto &function = expression.Cast<BoundFunctionExpression>();
	if (function.GetChildren().empty() || GetExpressionType(function) == CMExpressionType::NONE) {
		return nullptr;
	}
	return function.GetChildren()[0].get();
}

const vector<LogicalType> CMUtils::IntegralTypes() {
	return {LogicalType::UTINYINT, LogicalType::USMALLINT, LogicalType::UINTEGER, LogicalType::UBIGINT};
}

const vector<LogicalType> CMUtils::StringTypes() {
	return {LogicalType::UTINYINT, LogicalType::USMALLINT, LogicalType::UINTEGER, LogicalType::UBIGINT,
	        LogicalType::UHUGEINT};
}

// LCOV_EXCL_START
unique_ptr<FunctionData> CMUtils::Bind(BindScalarFunctionInput &input) {
	throw BinderException("Compressed materialization functions are for internal use only!");
}
// LCOV_EXCL_STOP

} // namespace duckdb
