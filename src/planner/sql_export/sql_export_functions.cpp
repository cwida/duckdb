#include "duckdb/planner/sql_export/bound_expression_sql_exporter_internal.hpp"
#include "duckdb/function/scalar/compressed_materialization_utils.hpp"
#include "duckdb/planner/bound_expression_sql_exporter.hpp"
#include "duckdb/planner/sql_export_helpers.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/lambda_expression.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_lambda_expression.hpp"
#include "duckdb/planner/filter/table_filter_functions.hpp"

namespace duckdb {

template <class FUNCTION>
static bool IsOptimizerFunctionQualification(const FUNCTION &function) {
	if (function.GetCatalogName().empty() && function.GetSchemaName().empty()) {
		return true;
	}
	return function.GetCatalogName() == "system" && function.GetSchemaName() == "main";
}

BoundExpressionSQLExportResult BoundExpressionSQLExportState::ExportLambda(const BoundLambdaExpression &lambda,
                                                                           const BoundFunctionExpression &function,
                                                                           idx_t logical_argument_count,
                                                                           const LogicalPlanVerificationPath &path) {
	const bool has_lambda_body = lambda.GetExpressionType() == ExpressionType::LAMBDA &&
	                             lambda.GetReturnType() == LogicalType::LAMBDA && lambda.LambdaExpr();
	const bool has_parameter_names =
	    lambda.ParameterCount() > 0 && lambda.ParameterNames().size() == lambda.ParameterCount();
	if (!has_lambda_body || !lambda.Captures().empty() || !has_parameter_names) {
		return BoundExpressionSQLExportResult::Failure({BoundExpressionSQLExportState::UnsupportedFeature(
		    path, "lambda_binding", "The bound lambda does not retain its SQL parameter binding")});
	}

	vector<unique_ptr<ParsedExpression>> references;
	for (idx_t index = 0; index < lambda.ParameterCount(); index++) {
		auto &parameter = lambda.ParameterNames()[lambda.ParameterCount() - index - 1];
		references.push_back(make_uniq<ColumnRefExpression>(parameter));
	}
	for (idx_t index = logical_argument_count; index < function.GetChildren().size(); index++) {
		auto reference = Export(*function.GetChildren()[index], SQLExportHelpers::ChildPath(path, index));
		if (reference.HasError()) {
			return reference;
		}
		references.push_back(std::move(reference.GetValue()));
	}

	lambda_reference_scopes.push_back(std::move(references));
	try {
		auto body = Export(*lambda.LambdaExpr(), SQLExportHelpers::ChildPath(path, 0));
		lambda_reference_scopes.pop_back();
		if (body.HasError()) {
			return body;
		}
		vector<string> parameter_names;
		for (auto &parameter : lambda.ParameterNames()) {
			parameter_names.push_back(parameter.GetIdentifierName());
		}
		return BoundExpressionSQLExportResult::Success(
		    make_uniq<LambdaExpression>(std::move(parameter_names), std::move(body.GetValue())));
	} catch (...) {
		lambda_reference_scopes.pop_back();
		throw;
	}
}

BoundExpressionSQLExportResult BoundExpressionSQLExportState::CompressedMaterializationFailure(
    const BoundFunctionExpression &expression, const LogicalPlanVerificationPath &path, string message) {
	auto &function = expression.Function();
	auto &definition = function.GetDefinition();
	D_ASSERT(definition);
	return BoundExpressionSQLExportResult::Failure({BoundExpressionSQLExportState::UnsupportedFunction(
	    path,
	    BoundExpressionSQLExportState::DefinitionFunctionIdentity(*definition, function.GetLogicalArguments(),
	                                                              function.GetLogicalReturnType()),
	    std::move(message))});
}

optional<BoundExpressionSQLExportResult>
BoundExpressionSQLExportState::TryExportCompressedMaterialization(const BoundFunctionExpression &expression,
                                                                  const LogicalPlanVerificationPath &path) {
	auto &function = expression.Function();
	auto &definition = function.GetDefinition();
	if (!definition) {
		return {};
	}
	auto compress = CMUtils::GetExpressionType(expression) == CMExpressionType::COMPRESS;
	auto decompress = CMUtils::GetExpressionType(expression) == CMExpressionType::DECOMPRESS;
	if (!compress && !decompress) {
		return {};
	}
	if (!IsOptimizerFunctionQualification(*definition) || !IsOptimizerFunctionQualification(function) ||
	    expression.GetChildren().empty()) {
		return CompressedMaterializationFailure(expression, path,
		                                        "The compressed materialization expression is malformed");
	}
	if (!context.discard_optimizer_metadata) {
		return CompressedMaterializationFailure(expression, path,
		                                        "Compressed materialization requires its complete logical plan");
	}
	if (decompress && expression.GetChildren()[0]->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &column = expression.GetChildren()[0]->Cast<BoundColumnRefExpression>();
		auto resolved = context.resolve_binding ? context.resolve_binding(column.Binding())
		                                        : optional<ResolvedSQLColumnReference>();
		if (!resolved || resolved->type != expression.GetReturnType()) {
			return CompressedMaterializationFailure(expression, path,
			                                        "The decompression input has no equivalent SQL representation");
		}
	}
	return ExportChild(*expression.GetChildren()[0], path, 0);
}

BoundExpressionSQLExportResult
BoundExpressionSQLExportState::ExportScalarFunction(const BoundFunctionExpression &expression,
                                                    const LogicalPlanVerificationPath &path) {
	auto &function = expression.Function();
	auto &definition = function.GetDefinition();
	D_ASSERT(definition);
	if (function.GetBindCallback() == TableFilterFunctions::Bind &&
	    TableFilterFunctions::IsTableFilterFunction(function.GetName())) {
		auto identity = BoundExpressionSQLExportState::DefinitionFunctionIdentity(
		    *definition, function.GetLogicalArguments(), function.GetLogicalReturnType());
		if (!context.discard_optimizer_metadata) {
			return BoundExpressionSQLExportResult::Failure({BoundExpressionSQLExportState::UnsupportedFunction(
			    path, std::move(identity), "Internal table filters require their complete logical plan")});
		}
		D_ASSERT(expression.GetReturnType() == LogicalType::BOOLEAN && expression.GetChildren().size() == 1);
		return BoundExpressionSQLExportResult::Success(ConstantExpression::FromValue(Value::BOOLEAN(true)));
	}
	auto compressed = TryExportCompressedMaterialization(expression, path);
	if (compressed) {
		return std::move(*compressed);
	}
	auto identity = BoundExpressionSQLExportState::DefinitionFunctionIdentity(
	    *definition, function.GetLogicalArguments(), function.GetLogicalReturnType());
	if (!identity.IsValid()) {
		identity.arguments.clear();
		for (auto &child : expression.GetChildren()) {
			identity.arguments.push_back(child->GetReturnType());
		}
		identity.return_type = expression.GetReturnType();
		if (!identity.IsValid()) {
			return BoundExpressionSQLExportResult::Failure({BoundExpressionSQLExportState::InternalExpressionInvariant(
			    path, expression, "Bound scalar function identity is incomplete")});
		}
	}
	optional_idx lambda_index;
	for (idx_t index = 0; index < MinValue(expression.GetChildren().size(), function.GetLogicalArguments().size());
	     index++) {
		if (expression.GetChildren()[index]->GetExpressionClass() == ExpressionClass::BOUND_LAMBDA) {
			if (lambda_index.IsValid()) {
				return BoundExpressionSQLExportResult::Failure({BoundExpressionSQLExportState::UnsupportedFunction(
				    path, std::move(identity), "The scalar function retains multiple SQL lambda arguments")});
			}
			lambda_index = index;
		}
	}
	if (function.HasBindLambdaCallback() != lambda_index.IsValid()) {
		return BoundExpressionSQLExportResult::Failure({BoundExpressionSQLExportState::UnsupportedFunction(
		    path, std::move(identity), "The scalar function does not retain its SQL lambda argument")});
	}
	const auto logical_argument_count = function.GetLogicalArguments().size();
	const auto child_count = expression.GetChildren().size();
	const bool retained_variadic_arguments =
	    definition->GetSignature().GetArgs() && child_count >= logical_argument_count;
	const bool has_scalar_arguments =
	    child_count == logical_argument_count || retained_variadic_arguments || definition->HasUnbindCallback();
	const bool has_expected_arguments =
	    lambda_index.IsValid() ? child_count >= logical_argument_count : has_scalar_arguments;
	if (!has_expected_arguments) {
		return BoundExpressionSQLExportResult::Failure({BoundExpressionSQLExportState::UnsupportedFunction(
		    path, std::move(identity), "The scalar function does not retain every SQL argument")});
	}
	auto name = BoundExpressionSQLExportState::RebindableFunctionName(*definition);
	if (!name || !SQLExportHelpers::IsSQLValueType(expression.GetReturnType())) {
		return BoundExpressionSQLExportResult::Failure({BoundExpressionSQLExportState::UnsupportedFunction(
		    path, std::move(identity), "The retained scalar function definition is not representable as SQL")});
	}
	const bool can_reconstruct_argument_names = definition->HasUnbindCallback();
	if (definition->GetProperties().GetCaptureArgumentAliases() && !can_reconstruct_argument_names) {
		return BoundExpressionSQLExportResult::Failure({BoundExpressionSQLExportState::UnsupportedFunction(
		    path, std::move(identity), "The bound scalar function does not expose its SQL argument names")});
	}
	if (definition->GetProperties().RequiresExpressionNames() && !can_reconstruct_argument_names) {
		return BoundExpressionSQLExportResult::Failure({BoundExpressionSQLExportState::UnsupportedFunction(
		    path, std::move(identity), "The bound scalar function requires expression names that are not retained")});
	}
	vector<Identifier> argument_names;
	if (!definition->HasUnbindCallback() && !function.GetNamedArguments().empty()) {
		auto positional_count = function.GetPositionalArgumentCount();
		if (positional_count + function.GetNamedArguments().size() != expression.GetChildren().size()) {
			return BoundExpressionSQLExportResult::Failure({BoundExpressionSQLExportState::UnsupportedFunction(
			    path, std::move(identity), "The named SQL arguments are incomplete")});
		}
		argument_names.resize(expression.GetChildren().size());
		for (idx_t i = 0; i < function.GetNamedArguments().size(); i++) {
			argument_names[positional_count + i] = function.GetNamedArguments()[i];
		}
	}
	vector<unique_ptr<ParsedExpression>> children;
	auto sql_argument_count =
	    lambda_index.IsValid() ? function.GetLogicalArguments().size() : expression.GetChildren().size();
	for (idx_t child_index = 0; child_index < sql_argument_count; child_index++) {
		auto child =
		    lambda_index == child_index
		        ? ExportLambda(expression.GetChildren()[child_index]->Cast<BoundLambdaExpression>(), expression,
		                       sql_argument_count, SQLExportHelpers::ChildPath(path, child_index))
		        : Export(*expression.GetChildren()[child_index], SQLExportHelpers::ChildPath(path, child_index));
		if (child.HasError()) {
			return child;
		}
		children.push_back(std::move(child.GetValue()));
	}
	unique_ptr<ParsedExpression> result;
	if (definition->HasUnbindCallback()) {
		FunctionUnbindInput input(expression, std::move(children));
		result = definition->GetUnbindCallback()(input);
		if (!result) {
			return BoundExpressionSQLExportResult::Failure({BoundExpressionSQLExportState::UnsupportedFunction(
			    path, std::move(identity), "The function cannot reconstruct its bound invocation")});
		}
	} else if (!argument_names.empty()) {
		vector<FunctionArgument> arguments;
		for (idx_t argument_index = 0; argument_index < children.size(); argument_index++) {
			arguments.emplace_back(argument_names[argument_index], std::move(children[argument_index]));
		}
		result = make_uniq<FunctionExpression>(*name, std::move(arguments), nullptr, nullptr, false, false, false);
	} else {
		result = make_uniq<FunctionExpression>(*name, std::move(children), nullptr, nullptr, false, false, false);
	}
	// Restore result types when binding or optimization changed argument types.
	const bool can_restore_result_type = SQLExportHelpers::IsSQLRepresentableType(expression.GetReturnType()) &&
	                                     !expression.GetReturnType().IsAggregateState();
	const bool has_specialized_result_type = (definition->HasBindCallback() || definition->HasResolveTypesCallback() ||
	                                          definition->GetReturnType().id() == LogicalTypeId::SQLNULL) &&
	                                         definition->GetReturnType() != expression.GetReturnType();
	if (can_restore_result_type && has_specialized_result_type) {
		return RestoreResultType(expression.GetReturnType(), std::move(result), path);
	}
	return BoundExpressionSQLExportResult::Success(std::move(result));
}

BoundAggregateSQLExportResult
BoundExpressionSQLExportState::BuildAggregateCall(const BoundAggregateExpression &expression,
                                                  const LogicalPlanVerificationPath &path) {
	D_ASSERT(expression.GetExpressionType() == ExpressionType::BOUND_AGGREGATE);
	auto &function = expression.Function();
	auto &definition = function.GetDefinition();
	D_ASSERT(definition);
	auto identity = BoundExpressionSQLExportState::DefinitionFunctionIdentity(
	    *definition, function.GetLogicalArguments(), function.GetLogicalReturnType());
	if (!identity.IsValid()) {
		return BoundAggregateSQLExportResult::Failure({BoundExpressionSQLExportState::InternalExpressionInvariant(
		    path, expression, "Bound aggregate function identity is incomplete")});
	}
	auto name = BoundExpressionSQLExportState::RebindableFunctionName(*definition);
	if (!definition->HasUnbindCallback() && expression.GetChildren().size() != function.GetLogicalArguments().size()) {
		return BoundAggregateSQLExportResult::Failure({BoundExpressionSQLExportState::UnsupportedFunction(
		    path, std::move(identity), "The aggregate does not retain every SQL argument")});
	}
	if (!name || !SQLExportHelpers::IsSQLValueType(expression.GetReturnType())) {
		return BoundAggregateSQLExportResult::Failure({BoundExpressionSQLExportState::UnsupportedFunction(
		    path, std::move(identity), "The retained aggregate function definition is not representable as SQL")});
	}
	D_ASSERT(expression.GetAggregateType() == AggregateType::NON_DISTINCT ||
	         expression.GetAggregateType() == AggregateType::DISTINCT);
	D_ASSERT(expression.StateExportMode() == AggregateStateExportMode::NONE ||
	         expression.StateExportMode() == AggregateStateExportMode::STATE_EXPORT);
	if (!definition->HasUnbindCallback() && (definition->GetProperties().GetCaptureArgumentAliases() ||
	                                         definition->GetProperties().RequiresExpressionNames())) {
		return BoundAggregateSQLExportResult::Failure({BoundExpressionSQLExportState::UnsupportedFunction(
		    path, std::move(identity), "The bound aggregate requires argument aliases that are not retained")});
	}
	vector<ChildExpression> source_children;
	for (idx_t child_index = 0; child_index < expression.GetChildren().size(); child_index++) {
		source_children.emplace_back(expression.GetChildren()[child_index].get());
	}
	if (expression.GetFilter()) {
		source_children.emplace_back(expression.GetFilter().get(), LogicalType::BOOLEAN);
	}
	if (expression.GetOrderBys()) {
		for (auto &order : expression.GetOrderBys()->orders) {
			D_ASSERT(order.type == OrderType::ASCENDING || order.type == OrderType::DESCENDING);
			D_ASSERT(order.null_order == OrderByNullType::NULLS_FIRST ||
			         order.null_order == OrderByNullType::NULLS_LAST);
			source_children.emplace_back(order.expression.get());
		}
	}

	vector<unique_ptr<ParsedExpression>> children;
	vector<LogicalPlanVerificationIssue> issues;
	ExportChildren(source_children, path, children, issues);
	if (!issues.empty()) {
		return BoundAggregateSQLExportResult::Failure(std::move(issues));
	}
	unique_ptr<FunctionExpression> result;
	if (definition->HasUnbindCallback()) {
		vector<unique_ptr<ParsedExpression>> arguments;
		for (idx_t i = 0; i < expression.GetChildren().size(); i++) {
			arguments.push_back(std::move(children[i]));
		}
		AggregateFunctionUnbindInput input(expression, std::move(arguments));
		result = definition->GetUnbindCallback()(input);
		if (!result) {
			return BoundAggregateSQLExportResult::Failure({BoundExpressionSQLExportState::UnsupportedFunction(
			    path, std::move(identity), "The aggregate cannot reconstruct its bound invocation")});
		}
	} else {
		auto &named_arguments = function.GetNamedArguments();
		auto positional_count = function.GetPositionalArgumentCount();
		if (!named_arguments.empty() && positional_count + named_arguments.size() != expression.GetChildren().size()) {
			return BoundAggregateSQLExportResult::Failure({BoundExpressionSQLExportState::UnsupportedFunction(
			    path, std::move(identity), "The named SQL arguments are incomplete")});
		}
		vector<FunctionArgument> arguments;
		for (idx_t i = 0; i < expression.GetChildren().size(); i++) {
			auto argument_name = !named_arguments.empty() && i >= positional_count
			                         ? named_arguments[i - positional_count]
			                         : Identifier();
			arguments.emplace_back(std::move(argument_name), std::move(children[i]));
		}
		result = make_uniq<FunctionExpression>(*name, std::move(arguments));
	}
	idx_t child_index = expression.GetChildren().size();
	unique_ptr<ParsedExpression> filter;
	if (expression.GetFilter()) {
		filter = std::move(children[child_index++]);
	}
	auto order_bys = make_uniq<OrderModifier>();
	if (expression.GetOrderBys()) {
		for (auto &order : expression.GetOrderBys()->orders) {
			auto &child = children[child_index++];
			if (SQLExportHelpers::IsConstantOrderKey(*order.expression, *child)) {
				continue;
			}
			order_bys->orders.emplace_back(
			    order.type, order.null_order,
			    SQLExportHelpers::OrderExpression(order.expression->GetReturnType(), std::move(child)));
		}
	}
	result->FilterMutable() = std::move(filter);
	result->OrderByMutable() = std::move(order_bys);
	result->DistinctMutable() = expression.IsDistinct();
	result->ExportStateMutable() = expression.StateExportMode() == AggregateStateExportMode::STATE_EXPORT;
	return BoundAggregateSQLExportResult::Success(std::move(result));
}

BoundExpressionSQLExportResult
BoundExpressionSQLExportState::ExportAggregate(const BoundAggregateExpression &expression,
                                               const LogicalPlanVerificationPath &path) {
	auto call = BuildAggregateCall(expression, path);
	if (call.HasError()) {
		return BoundExpressionSQLExportResult::Failure(call);
	}
	auto &function = expression.Function();
	auto &definition = function.GetDefinition();
	unique_ptr<ParsedExpression> result = std::move(call.GetValue());
	const bool can_restore_result_type = expression.StateExportMode() == AggregateStateExportMode::NONE &&
	                                     SQLExportHelpers::IsSQLRepresentableType(expression.GetReturnType()) &&
	                                     !expression.GetReturnType().IsAggregateState();
	const bool has_specialized_result_type =
	    definition->HasBindCallback() && definition->GetReturnType() != expression.GetReturnType();
	if (can_restore_result_type && has_specialized_result_type) {
		return RestoreResultType(expression.GetReturnType(), std::move(result), path);
	}
	return BoundExpressionSQLExportResult::Success(std::move(result));
}

BoundAggregateSQLExportResult
BoundExpressionSQLExportState::ExportAggregateCall(const BoundAggregateExpression &expression,
                                                   const LogicalPlanVerificationPath &path) {
	auto call = BuildAggregateCall(expression, path);
	if (call.HasError()) {
		return call;
	}
	if (!expression.GetReturnType().EqualsIncludingCollation(expression.Function().GetLogicalReturnType())) {
		return BoundAggregateSQLExportResult::Failure({BoundExpressionSQLExportState::UnsupportedFeature(
		    path, "aggregate_call_result_type",
		    "A bare aggregate call cannot preserve the bound expression's logical result type")});
	}
	return call;
}

} // namespace duckdb
