#include "duckdb/planner/sql_export/bound_expression_sql_exporter_internal.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/sql_export_helpers.hpp"

namespace duckdb {

bool BoundExpressionSQLExportState::RequiresConstantConstructor(const LogicalType &type) {
	return ConstantExpression::RequiresTypeWitness(type);
}

BoundExpressionSQLExportResult
BoundExpressionSQLExportState::CastToConstructedType(const LogicalType &type, unique_ptr<ParsedExpression> child,
                                                     const LogicalPlanVerificationPath &path) {
	vector<unique_ptr<ParsedExpression>> arguments;
	arguments.push_back(std::move(child));
	try {
		arguments.push_back(ConstantExpression::FromValue(Value(type)));
	} catch (const NotImplementedException &ex) {
		return BoundExpressionSQLExportResult::Failure(
		    {BoundExpressionSQLExportState::UnsupportedFeature(path, "constant_type", ErrorData(ex).RawMessage())});
	}
	return BoundExpressionSQLExportResult::Success(
	    SQLExportHelpers::SystemFunction("cast_to_type", std::move(arguments)));
}

BoundExpressionSQLExportResult
BoundExpressionSQLExportState::RestoreResultType(const LogicalType &type, unique_ptr<ParsedExpression> result,
                                                 const LogicalPlanVerificationPath &path) {
	if (RequiresConstantConstructor(type)) {
		return CastToConstructedType(type, std::move(result), path);
	}
	return BoundExpressionSQLExportResult::Success(BoundExpressionSQLExportState::SQLCast(type, std::move(result)));
}

BoundExpressionSQLExportResult BoundExpressionSQLExportState::ExportConstant(const BoundConstantExpression &expression,
                                                                             const LogicalPlanVerificationPath &path) {
	D_ASSERT(expression.GetExpressionType() == ExpressionType::VALUE_CONSTANT);
	auto &return_type = expression.GetReturnType();
	auto &value = expression.GetValue();
	D_ASSERT(return_type == value.type());
	if (!SQLExportHelpers::IsSQLValueType(return_type)) {
		return BoundExpressionSQLExportResult::Failure({BoundExpressionSQLExportState::InternalExpressionInvariant(
		    path, expression, "Bound constant has an unexportable type")});
	}
	unique_ptr<ParsedExpression> result;
	try {
		result = ConstantExpression::FromValue(value);
	} catch (const NotImplementedException &ex) {
		return BoundExpressionSQLExportResult::Failure(
		    {BoundExpressionSQLExportState::UnsupportedFeature(path, "constant_value", ErrorData(ex).RawMessage())});
	}
	if (RequiresConstantConstructor(return_type) || !SQLExportHelpers::IsSQLRepresentableType(return_type)) {
		return BoundExpressionSQLExportResult::Success(std::move(result));
	}
	if (return_type.id() != LogicalTypeId::SQLNULL) {
		const bool has_result_cast =
		    result->GetExpressionClass() == ExpressionClass::CAST &&
		    result->Cast<CastExpression>().TargetType().Equals(
		        *TypeExpression::FromLogicalType(BoundExpressionSQLExportState::SQLCastType(return_type)));
		if (!has_result_cast) {
			result = BoundExpressionSQLExportState::SQLCast(return_type, std::move(result));
		}
	}
	return BoundExpressionSQLExportResult::Success(std::move(result));
}

} // namespace duckdb
