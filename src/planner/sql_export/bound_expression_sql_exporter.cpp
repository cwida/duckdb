#include "duckdb/planner/sql_export/bound_expression_sql_exporter_internal.hpp"
#include "duckdb/common/extension_type_info.hpp"
#include "duckdb/function/scalar/compressed_materialization_utils.hpp"
#include "duckdb/planner/bound_expression_sql_exporter.hpp"
#include "duckdb/planner/sql_export_helpers.hpp"
#include "duckdb/parser/expression/between_expression.hpp"
#include "duckdb/parser/expression/case_expression.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/collate_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_between_expression.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/function/cast/cast_function_set.hpp"
#include "duckdb/function/scalar/generic_common.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/expression/bound_unnest_expression.hpp"

namespace duckdb {

static inline bool IsExpressionRootPath(const LogicalPlanVerificationPath &path) {
	if (!path.IsValid()) {
		return false;
	}
	if (path.root == LogicalPlanVerificationPathRoot::STANDALONE_EXPRESSION) {
		return true;
	}
	for (auto &component : path.components) {
		if (component.type == LogicalPlanVerificationPathComponentType::OPERATOR_EXPRESSION) {
			return true;
		}
	}
	return false;
}

static LogicalPlanVerificationIssue UnsupportedExpression(const LogicalPlanVerificationPath &path,
                                                          ExpressionClass expression_class) {
	return SQLExportHelpers::MakeIssue(
	    LogicalPlanVerificationIssueCode::UNSUPPORTED_EXPRESSION, LogicalPlanVerificationPhase::EXPRESSION_EXPORT, path,
	    LogicalPlanVerificationConstructIdentity::Expression(expression_class),
	    "The bound expression class does not have a SQL AST representation in this exporter");
}

static inline bool ChildrenAreConsistentWithArguments(const vector<unique_ptr<Expression>> &children,
                                                      const vector<LogicalType> &arguments) {
	if (children.size() != arguments.size()) {
		return false;
	}
	for (idx_t child_index = 0; child_index < children.size(); child_index++) {
		if (!children[child_index] || !children[child_index]->GetReturnType().IsComplete()) {
			return false;
		}
		if (arguments[child_index].IsComplete() && children[child_index]->GetReturnType() != arguments[child_index]) {
			return false;
		}
	}
	return true;
}

LogicalPlanVerificationIssue
BoundExpressionSQLExportState::InternalInvariant(optional<LogicalPlanVerificationPath> path, string message,
                                                 optional<LogicalPlanVerificationConstructIdentity> construct) {
	return SQLExportHelpers::MakeIssue(LogicalPlanVerificationIssueCode::INTERNAL_INVARIANT,
	                                   LogicalPlanVerificationPhase::EXPRESSION_EXPORT, std::move(path),
	                                   std::move(construct), std::move(message));
}

LogicalPlanVerificationIssue
BoundExpressionSQLExportState::InternalExpressionInvariant(const LogicalPlanVerificationPath &path,
                                                           const Expression &expression, string message) {
	return BoundExpressionSQLExportState::InternalInvariant(
	    path, std::move(message),
	    LogicalPlanVerificationConstructIdentity::Expression(expression.GetExpressionClass()));
}

LogicalPlanVerificationIssue BoundExpressionSQLExportState::UnsupportedFeature(const LogicalPlanVerificationPath &path,
                                                                               string feature, string message) {
	return SQLExportHelpers::MakeIssue(
	    LogicalPlanVerificationIssueCode::UNSUPPORTED_EXPORT_FEATURE, LogicalPlanVerificationPhase::EXPRESSION_EXPORT,
	    path, LogicalPlanVerificationConstructIdentity::ExportFeature(std::move(feature)), std::move(message));
}

LogicalPlanVerificationIssue
BoundExpressionSQLExportState::SensitiveParameterIssue(const LogicalPlanVerificationPath &path,
                                                       const Identifier &function_name, const Identifier &parameter) {
	auto issue = UnsupportedFeature(
	    path, "sensitive_parameter",
	    StringUtil::Format("The invocation of \"%s\" passes a value to the sensitive parameter \"%s\", "
	                       "which must not appear in generated SQL",
	                       function_name.GetIdentifierName(), parameter.GetIdentifierName()));
	issue.facts.emplace_back("function", Value(function_name.GetIdentifierName()));
	issue.facts.emplace_back("parameter", Value(parameter.GetIdentifierName()));
	return issue;
}

LogicalPlanVerificationIssue
BoundExpressionSQLExportState::StateSensitivityIssue(const LogicalPlanVerificationPath &path,
                                                     const Identifier &function_name, const Identifier &parameter) {
	if (!parameter.empty()) {
		return SensitiveParameterIssue(path, function_name, parameter);
	}
	auto issue = UnsupportedFeature(
	    path, "sensitive_parameter",
	    StringUtil::Format("The exported state of aggregate \"%s\" retains constant parameters whose "
	                       "declarations cannot be resolved",
	                       function_name.GetIdentifierName()));
	issue.facts.emplace_back("function", Value(function_name.GetIdentifierName()));
	return issue;
}

bool BoundExpressionSQLExportState::ValueRendersSensitiveState(ClientContext &client, const Value &value,
                                                               Identifier &function_name, Identifier &parameter) {
	auto state_check = [&](const LogicalType &child) {
		return child.IsAggregateState() &&
		       ExportAggregateFunction::StateRendersSensitiveParameter(client, child, function_name, parameter);
	};
	// states reachable through the value's own type
	if (TypeVisitor::Contains(value.type(), state_check)) {
		return true;
	}
	if (value.IsNull()) {
		return false;
	}
	// a TYPE value renders its payload type, which can itself describe a state
	if (value.type().id() == LogicalTypeId::TYPE) {
		return TypeVisitor::Contains(TypeValue::GetType(value), state_check);
	}
	// nested TYPE values hide behind containers
	if (!TypeVisitor::Contains(value.type(),
	                           [](const LogicalType &child) { return child.id() == LogicalTypeId::TYPE; })) {
		return false;
	}
	auto check_children = [&](const vector<Value> &children) {
		for (auto &child : children) {
			if (ValueRendersSensitiveState(client, child, function_name, parameter)) {
				return true;
			}
		}
		return false;
	};
	switch (value.type().InternalType()) {
	case PhysicalType::LIST:
		return check_children(ListValue::GetChildren(value));
	case PhysicalType::ARRAY:
		return check_children(ArrayValue::GetChildren(value));
	case PhysicalType::STRUCT:
		return check_children(StructValue::GetChildren(value));
	default:
		return false;
	}
}

LogicalPlanVerificationIssue
BoundExpressionSQLExportState::UnsupportedFunction(const LogicalPlanVerificationPath &path,
                                                   LogicalPlanVerificationFunctionIdentity identity, string message) {
	return SQLExportHelpers::MakeIssue(
	    LogicalPlanVerificationIssueCode::UNSUPPORTED_FUNCTION, LogicalPlanVerificationPhase::EXPRESSION_EXPORT, path,
	    LogicalPlanVerificationConstructIdentity::Function(std::move(identity)), std::move(message));
}

bool BoundExpressionSQLExportState::HasNestedCollation(const LogicalType &type) {
	return type.id() != LogicalTypeId::VARCHAR && TypeVisitor::Contains(type, [](const LogicalType &child) {
		       return child.id() == LogicalTypeId::VARCHAR && !StringType::GetCollation(child).empty();
	       });
}

BoundExpressionSQLExportResult
BoundExpressionSQLExportState::PreserveCollation(const LogicalType &type, BoundExpressionSQLExportResult result,
                                                 const LogicalPlanVerificationPath &path) {
	if (result.HasError()) {
		return result;
	}
	if (type.id() == LogicalTypeId::VARCHAR) {
		auto collation = StringType::GetCollation(type);
		if (!collation.empty()) {
			result.GetValue() = make_uniq<CollateExpression>(std::move(collation), std::move(result.GetValue()));
		}
	} else if (BoundExpressionSQLExportState::HasNestedCollation(type)) {
		return CastToConstructedType(type, std::move(result.GetValue()), path);
	}
	return result;
}

LogicalType BoundExpressionSQLExportState::SQLCastType(const LogicalType &type) {
	// Scalar collations are applied by COLLATE, outside the cast's type expression.
	return type.id() == LogicalTypeId::VARCHAR && !type.HasAlias() ? LogicalType::VARCHAR : type;
}

unique_ptr<ParsedExpression> BoundExpressionSQLExportState::SQLCast(const LogicalType &type,
                                                                    unique_ptr<ParsedExpression> child, bool try_cast) {
	return make_uniq<CastExpression>(BoundExpressionSQLExportState::SQLCastType(type), std::move(child), try_cast);
}

BoundExpressionSQLExportState::BoundExpressionSQLExportState(const BoundExpressionSQLExportContext &context_p)
    : context(context_p) {
}

BoundExpressionSQLExportResult BoundExpressionSQLExportState::Export(const Expression &expression,
                                                                     const LogicalPlanVerificationPath &path) {
	auto result = ExportInternal(expression, path);
	const bool preserves_collation = expression.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT ||
	                                 (expression.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION &&
	                                  expression.GetReturnType().id() != LogicalTypeId::VARCHAR) ||
	                                 (expression.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE &&
	                                  expression.GetReturnType().IsAggregateState());
	if (expression.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF || preserves_collation) {
		return result;
	}
	return BoundExpressionSQLExportState::PreserveCollation(expression.GetReturnType(), std::move(result), path);
}

BoundExpressionSQLExportResult BoundExpressionSQLExportState::ExportInternal(const Expression &expression,
                                                                             const LogicalPlanVerificationPath &path) {
	switch (expression.GetExpressionClass()) {
	case ExpressionClass::BOUND_CONSTANT:
		return ExportConstant(expression.Cast<BoundConstantExpression>(), path);
	case ExpressionClass::BOUND_COLUMN_REF:
		return ExportColumnRef(expression.Cast<BoundColumnRefExpression>(), path);
	case ExpressionClass::BOUND_REF:
		return ExportReference(expression.Cast<BoundReferenceExpression>(), path);
	case ExpressionClass::BOUND_FUNCTION:
		return ExportFunction(expression.Cast<BoundFunctionExpression>(), path);
	case ExpressionClass::BOUND_CONJUNCTION:
		return ExportConjunction(expression.Cast<BoundConjunctionExpression>(), path);
	case ExpressionClass::BOUND_CASE:
		return ExportCase(expression.Cast<BoundCaseExpression>(), path);
	case ExpressionClass::BOUND_OPERATOR:
		return ExportOperator(expression.Cast<BoundOperatorExpression>(), path);
	case ExpressionClass::BOUND_AGGREGATE:
		return ExportAggregate(expression.Cast<BoundAggregateExpression>(), path);
	case ExpressionClass::BOUND_DEFAULT:
	case ExpressionClass::BOUND_PARAMETER:
	case ExpressionClass::BOUND_SUBQUERY:
	case ExpressionClass::BOUND_WINDOW:
	case ExpressionClass::BOUND_UNNEST:
	case ExpressionClass::BOUND_LAMBDA:
	case ExpressionClass::BOUND_LAMBDA_REF:
	case ExpressionClass::LEGACY_BOUND_CAST:
	case ExpressionClass::LEGACY_BOUND_COMPARISON:
	case ExpressionClass::LEGACY_BOUND_BETWEEN:
		return BoundExpressionSQLExportResult::Failure({UnsupportedExpression(path, expression.GetExpressionClass())});
	case ExpressionClass::BOUND_EXPANDED:
	case ExpressionClass::AGGREGATE:
	case ExpressionClass::CASE:
	case ExpressionClass::CAST:
	case ExpressionClass::COLUMN_REF:
	case ExpressionClass::COMPARISON:
	case ExpressionClass::CONJUNCTION:
	case ExpressionClass::CONSTANT:
	case ExpressionClass::DEFAULT:
	case ExpressionClass::FUNCTION:
	case ExpressionClass::OPERATOR:
	case ExpressionClass::STAR:
	case ExpressionClass::SUBQUERY:
	case ExpressionClass::WINDOW:
	case ExpressionClass::PARAMETER:
	case ExpressionClass::COLLATE:
	case ExpressionClass::LAMBDA:
	case ExpressionClass::POSITIONAL_REFERENCE:
	case ExpressionClass::BETWEEN:
	case ExpressionClass::LAMBDA_REF:
	case ExpressionClass::PATTERN:
	case ExpressionClass::TYPE:
		return BoundExpressionSQLExportResult::Failure({BoundExpressionSQLExportState::InternalExpressionInvariant(
		    path, expression, "Expression export requires a final bound class")});
	case ExpressionClass::INVALID:
		return BoundExpressionSQLExportResult::Failure({BoundExpressionSQLExportState::InternalInvariant(
		    path, "Expression export received an invalid expression class")});
	}
	return BoundExpressionSQLExportResult::Failure({BoundExpressionSQLExportState::InternalInvariant(
	    path, "Expression export received an unknown expression class")});
}

BoundExpressionSQLExportResult BoundExpressionSQLExportState::ExportUnnest(const BoundUnnestExpression &expression,
                                                                           const LogicalPlanVerificationPath &path) {
	D_ASSERT(expression.Child());
	auto child = ExportChild(*expression.Child(), path, 0);
	if (child.HasError()) {
		return child;
	}
	vector<unique_ptr<ParsedExpression>> arguments;
	arguments.push_back(std::move(child.GetValue()));
	return BoundExpressionSQLExportState::PreserveCollation(
	    expression.GetReturnType(),
	    BoundExpressionSQLExportResult::Success(make_uniq<FunctionExpression>("unnest", std::move(arguments))), path);
}

BoundExpressionSQLExportResult
BoundExpressionSQLExportState::ExportReference(const BoundReferenceExpression &expression,
                                               const LogicalPlanVerificationPath &path) {
	if (expression.GetExpressionType() != ExpressionType::BOUND_REF || lambda_reference_scopes.empty() ||
	    expression.Index() >= lambda_reference_scopes.back().size()) {
		return BoundExpressionSQLExportResult::Failure({UnsupportedExpression(path, expression.GetExpressionClass())});
	}
	return BoundExpressionSQLExportResult::Success(lambda_reference_scopes.back()[expression.Index()]->Copy());
}

BoundExpressionSQLExportResult
BoundExpressionSQLExportState::ExportColumnRef(const BoundColumnRefExpression &expression,
                                               const LogicalPlanVerificationPath &path) {
	D_ASSERT(expression.GetExpressionType() == ExpressionType::BOUND_COLUMN_REF);
	auto &binding = expression.Binding();
	if (!binding.table_index.IsValid() || !binding.column_index.IsValid()) {
		return BoundExpressionSQLExportResult::Failure(
		    {InvalidBinding(path, binding, "Bound column reference has an incomplete binding")});
	}
	if (!SQLExportHelpers::IsSQLValueType(expression.GetReturnType())) {
		return BoundExpressionSQLExportResult::Failure({BoundExpressionSQLExportState::InternalExpressionInvariant(
		    path, expression, "Bound column reference has an incomplete type")});
	}
	if (expression.Depth() != 0) {
		auto issue = BoundExpressionSQLExportState::UnsupportedFeature(
		    path, "correlated_column_reference", "Correlated column references require an owning query export context");
		issue.facts.emplace_back("depth", Value::UBIGINT(expression.Depth()));
		return BoundExpressionSQLExportResult::Failure({std::move(issue)});
	}
	if (!context.resolve_binding) {
		return BoundExpressionSQLExportResult::Failure(
		    {InvalidBinding(path, binding, "No SQL column binding resolver was provided")});
	}
	auto resolved = context.resolve_binding(binding);
	if (!resolved) {
		return BoundExpressionSQLExportResult::Failure(
		    {InvalidBinding(path, binding, "The SQL column binding resolver has no matching entry")});
	}
	if (resolved->names.empty()) {
		return BoundExpressionSQLExportResult::Failure(
		    {InvalidBinding(path, binding, "The resolved SQL column name is empty")});
	}
	for (auto &name : resolved->names) {
		if (!SQLExportHelpers::IsValidIdentifier(name)) {
			return BoundExpressionSQLExportResult::Failure(
			    {InvalidBinding(path, binding, "The resolved SQL column name contains an invalid identifier")});
		}
	}
	if (!SQLExportHelpers::IsSQLValueType(resolved->type)) {
		return BoundExpressionSQLExportResult::Failure({BoundExpressionSQLExportState::InternalExpressionInvariant(
		    path, expression, "The resolved SQL column type is incomplete")});
	}
	auto optimizer_type_match = resolved->optimizer_type && *resolved->optimizer_type == expression.GetReturnType();
	if (resolved->type != expression.GetReturnType() && !optimizer_type_match) {
		LogicalPlanVerificationIssue issue;
		issue.code = LogicalPlanVerificationIssueCode::TYPE_MISMATCH;
		issue.phase = LogicalPlanVerificationPhase::EXPRESSION_EXPORT;
		issue.path = path;
		issue.construct =
		    LogicalPlanVerificationConstructIdentity::BindingTypeMismatch(resolved->type, expression.GetReturnType());
		issue.message = "The resolved SQL column type differs from the bound expression type";
		return BoundExpressionSQLExportResult::Failure({std::move(issue)});
	}
	auto replacement = substitutions.find(binding);
	auto result = BoundExpressionSQLExportResult::Success(
	    replacement == substitutions.end() ? make_uniq<ColumnRefExpression>(std::move(resolved->names))
	                                       : replacement->second.get().Copy());
	result.GetValue()->ClearAlias();
	if (optimizer_type_match) {
		return result;
	}
	if (!expression.GetReturnType().EqualsIncludingCollation(resolved->type)) {
		if (expression.GetReturnType().id() != LogicalTypeId::VARCHAR) {
			auto issue = BoundExpressionSQLExportState::UnsupportedFeature(
			    path, "nested_result_collation",
			    "Changing nested input collations requires a typed SQL representation");
			issue.facts.emplace_back("logical_type", Value(expression.GetReturnType().ToString()));
			issue.facts.emplace_back("input_logical_type", Value(resolved->type.ToString()));
			issue.facts.emplace_back("input_varchar_collations",
			                         Value(SQLExportHelpers::TypeCollationSignature(resolved->type)));
			issue.facts.emplace_back("varchar_collations",
			                         Value(SQLExportHelpers::TypeCollationSignature(expression.GetReturnType())));
			return BoundExpressionSQLExportResult::Failure({std::move(issue)});
		}
		if (StringType::GetCollation(expression.GetReturnType()).empty()) {
			return BoundExpressionSQLExportResult::Failure({BoundExpressionSQLExportState::UnsupportedFeature(
			    path, "column_collation_reset", "Clearing an input collation requires a SQL representation")});
		}
		return BoundExpressionSQLExportState::PreserveCollation(expression.GetReturnType(), std::move(result), path);
	}
	return result;
}

LogicalPlanVerificationIssue BoundExpressionSQLExportState::InvalidBinding(const LogicalPlanVerificationPath &path,
                                                                           const ColumnBinding &binding,
                                                                           string message) {
	LogicalPlanVerificationIssue issue;
	issue.code = LogicalPlanVerificationIssueCode::INVALID_BINDING;
	issue.phase = LogicalPlanVerificationPhase::EXPRESSION_EXPORT;
	issue.path = path;
	issue.facts.emplace_back("column_index", Value::UBIGINT(binding.column_index.GetIndexUnsafe()));
	issue.facts.emplace_back("table_index", Value::UBIGINT(binding.table_index.index));
	issue.message = std::move(message);
	return issue;
}

BoundExpressionSQLExportResult BoundExpressionSQLExportState::ExportFunction(const BoundFunctionExpression &expression,
                                                                             const LogicalPlanVerificationPath &path) {
	switch (expression.GetExpressionType()) {
	case ExpressionType::BOUND_FUNCTION:
		return ExportScalarFunction(expression, path);
	case ExpressionType::OPERATOR_CAST:
		return ExportCast(expression, path);
	case ExpressionType::COMPARE_EQUAL:
	case ExpressionType::COMPARE_NOTEQUAL:
	case ExpressionType::COMPARE_LESSTHAN:
	case ExpressionType::COMPARE_GREATERTHAN:
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
	case ExpressionType::COMPARE_DISTINCT_FROM:
	case ExpressionType::COMPARE_NOT_DISTINCT_FROM:
		return ExportComparison(expression, path);
	case ExpressionType::COMPARE_BETWEEN:
		return ExportBetween(expression, path);
	default:
		return BoundExpressionSQLExportResult::Failure({BoundExpressionSQLExportState::InternalExpressionInvariant(
		    path, expression, "Bound function has an invalid expression type")});
	}
}

BoundExpressionSQLExportResult BoundExpressionSQLExportState::ExportCast(const BoundFunctionExpression &expression,
                                                                         const LogicalPlanVerificationPath &path) {
	D_ASSERT(expression.GetExpressionType() == ExpressionType::OPERATOR_CAST);
	D_ASSERT(expression.GetChildren().size() == 1 && expression.GetChildren()[0]);
	D_ASSERT(BoundCastExpression::HasValidBindData(expression));
	D_ASSERT(ChildrenAreConsistentWithArguments(expression.GetChildren(), expression.Function().GetArguments()));
	D_ASSERT(expression.GetReturnType() == expression.Function().GetReturnType());
	if (!SQLExportHelpers::IsSQLRepresentableType(expression.GetReturnType()) ||
	    !SQLExportHelpers::IsSQLRepresentableType(expression.GetChildren()[0]->GetReturnType())) {
		return BoundExpressionSQLExportResult::Failure({BoundExpressionSQLExportState::UnsupportedFeature(
		    path, "cast_type", "The cast type has no SQL type representation")});
	}
	if (context.discard_optimizer_metadata && CMUtils::GetExpressionType(expression) == CMExpressionType::CAST) {
		if (!BoundCastExpression::IsDefaultCast(expression)) {
			return BoundExpressionSQLExportResult::Failure({BoundExpressionSQLExportState::UnsupportedFeature(
			    path, "compressed_materialization_cast",
			    "The compressed materialization projection contains a non-default cast")});
		}
		return ExportChild(*expression.GetChildren()[0], path, 0);
	}
	if (BoundCastExpression::IsDefaultCast(expression)) {
		if (!context.client_context ||
		    CastFunctionSet::Get(*context.client_context)
		        .CanOverrideDefaultCast(expression.GetChildren()[0]->GetReturnType(), expression.GetReturnType())) {
			return BoundExpressionSQLExportResult::Failure({BoundExpressionSQLExportState::UnsupportedFeature(
			    path, "default_cast_binding",
			    "A default-only bound cast cannot be reconstructed through this SQL binding")});
		}
	}
	auto child = ExportChild(*expression.GetChildren()[0], path, 0);
	if (child.HasError()) {
		return child;
	}
	if (expression.GetReturnType().IsAggregateState()) {
		auto state_client = context.client_context;
		if (state_client) {
			// the target state type renders its retained constant parameters
			Identifier state_function;
			Identifier state_parameter;
			if (ExportAggregateFunction::StateRendersSensitiveParameter(*state_client, expression.GetReturnType(),
			                                                            state_function, state_parameter)) {
				return BoundExpressionSQLExportResult::Failure(
				    {StateSensitivityIssue(path, state_function, state_parameter)});
			}
		}
		auto storage_type = expression.GetReturnType().WithAlias("").WithExtensionInfo(nullptr);
		auto &source_type = expression.GetChildren()[0]->GetReturnType();
		if (BoundCastExpression::IsTryCast(expression) || !context.client_context ||
		    CastFunctionSet::Get(*context.client_context)
		        .CanOverrideDefaultCast(source_type, expression.GetReturnType()) ||
		    CastFunctionSet::Get(*context.client_context).CanOverrideDefaultCast(source_type, storage_type)) {
			return BoundExpressionSQLExportResult::Failure({BoundExpressionSQLExportState::UnsupportedFeature(
			    path, "aggregate_state_try_cast",
			    "Aggregate state TRY_CAST or custom casts require a SQL representation")});
		}
		auto storage = RestoreResultType(storage_type, std::move(child.GetValue()), path);
		if (storage.HasError()) {
			return storage;
		}
		auto result = ExportAggregateFunction::StateToSQL(expression.GetReturnType(), std::move(storage.GetValue()));
		if (!result) {
			return BoundExpressionSQLExportResult::Failure({BoundExpressionSQLExportState::UnsupportedFeature(
			    path, "aggregate_state_parameters", "Aggregate state SQL parameters are not representable")});
		}
		return BoundExpressionSQLExportResult::Success(std::move(result));
	}
	if (BoundExpressionSQLExportState::HasNestedCollation(expression.GetReturnType())) {
		if (BoundCastExpression::IsTryCast(expression) ||
		    expression.GetReturnType() == expression.GetChildren()[0]->GetReturnType()) {
			return BoundExpressionSQLExportResult::Failure({BoundExpressionSQLExportState::UnsupportedFeature(
			    path, "nested_result_collation", "This cast cannot preserve nested collations through SQL")});
		}
		return CastToConstructedType(expression.GetReturnType(), std::move(child.GetValue()), path);
	}
	if (RequiresConstantConstructor(expression.GetReturnType()) && !BoundCastExpression::IsTryCast(expression)) {
		return CastToConstructedType(expression.GetReturnType(), std::move(child.GetValue()), path);
	}
	return BoundExpressionSQLExportResult::Success(BoundExpressionSQLExportState::SQLCast(
	    expression.GetReturnType(), std::move(child.GetValue()), BoundCastExpression::IsTryCast(expression)));
}

BoundExpressionSQLExportResult
BoundExpressionSQLExportState::ExportComparison(const BoundFunctionExpression &expression,
                                                const LogicalPlanVerificationPath &path) {
	D_ASSERT(BoundComparisonExpression::IsComparison(expression.GetExpressionType()));
	D_ASSERT(expression.GetReturnType() == LogicalType::BOOLEAN);
	D_ASSERT(expression.GetChildren().size() == 2 && expression.GetChildren()[0] && expression.GetChildren()[1]);
	D_ASSERT(!expression.BindInfo());
	D_ASSERT(ChildrenAreConsistentWithArguments(expression.GetChildren(), expression.Function().GetArguments()));
	D_ASSERT(expression.GetReturnType() == expression.Function().GetReturnType());
	auto &left_type = expression.GetChildren()[0]->GetReturnType();
	auto &right_type = expression.GetChildren()[1]->GetReturnType();
	// the regex range filter compares VARCHAR values with BLOB bounds, as the bounds need not be valid UTF-8
	const bool varchar_blob = (left_type.id() == LogicalTypeId::VARCHAR && right_type.id() == LogicalTypeId::BLOB) ||
	                          (left_type.id() == LogicalTypeId::BLOB && right_type.id() == LogicalTypeId::VARCHAR);
	D_ASSERT(left_type == right_type || varchar_blob);
	vector<unique_ptr<ParsedExpression>> children;
	vector<LogicalPlanVerificationIssue> issues;
	ExportChildren(expression.GetChildren(), path, children, issues);
	if (!issues.empty()) {
		return BoundExpressionSQLExportResult::Failure(std::move(issues));
	}
	if (varchar_blob) {
		// encode reinterprets the VARCHAR bytes as a BLOB, so the bytes are compared as before
		auto &varchar_child = left_type.id() == LogicalTypeId::VARCHAR ? children[0] : children[1];
		vector<unique_ptr<ParsedExpression>> arguments;
		arguments.push_back(std::move(varchar_child));
		varchar_child = SQLExportHelpers::SystemFunction("encode", std::move(arguments));
	}
	return BoundExpressionSQLExportResult::Success(make_uniq<ComparisonExpression>(
	    expression.GetExpressionType(), std::move(children[0]), std::move(children[1])));
}

BoundExpressionSQLExportResult BoundExpressionSQLExportState::ExportBetween(const BoundFunctionExpression &expression,
                                                                            const LogicalPlanVerificationPath &path) {
	D_ASSERT(expression.GetExpressionType() == ExpressionType::COMPARE_BETWEEN);
	D_ASSERT(expression.GetReturnType() == LogicalType::BOOLEAN);
	D_ASSERT(expression.GetChildren().size() == 3 && expression.GetChildren()[0] && expression.GetChildren()[1] &&
	         expression.GetChildren()[2]);
	D_ASSERT(BoundBetweenExpression::HasValidBindData(expression));
	D_ASSERT(ChildrenAreConsistentWithArguments(expression.GetChildren(), expression.Function().GetArguments()));
	D_ASSERT(expression.GetReturnType() == expression.Function().GetReturnType());
	D_ASSERT(expression.GetChildren()[0]->GetReturnType() == expression.GetChildren()[1]->GetReturnType());
	D_ASSERT(expression.GetChildren()[0]->GetReturnType() == expression.GetChildren()[2]->GetReturnType());
	vector<unique_ptr<ParsedExpression>> children;
	vector<LogicalPlanVerificationIssue> issues;
	ExportChildren(expression.GetChildren(), path, children, issues);
	if (!issues.empty()) {
		return BoundExpressionSQLExportResult::Failure(std::move(issues));
	}
	auto lower_inclusive = BoundBetweenExpression::LowerInclusive(expression);
	auto upper_inclusive = BoundBetweenExpression::UpperInclusive(expression);
	if (lower_inclusive && upper_inclusive) {
		return BoundExpressionSQLExportResult::Success(
		    make_uniq<BetweenExpression>(std::move(children[0]), std::move(children[1]), std::move(children[2])));
	}
	if (expression.GetChildren()[0]->IsVolatile()) {
		return BoundExpressionSQLExportResult::Failure({BoundExpressionSQLExportState::UnsupportedFeature(
		    path, "exclusive_between_input_evaluation",
		    "An exclusive BETWEEN cannot duplicate a volatile input while preserving evaluation semantics")});
	}
	auto lower = make_uniq<ComparisonExpression>(BoundBetweenExpression::LowerComparisonType(expression),
	                                             children[0]->Copy(), std::move(children[1]));
	auto upper = make_uniq<ComparisonExpression>(BoundBetweenExpression::UpperComparisonType(expression),
	                                             std::move(children[0]), std::move(children[2]));
	return BoundExpressionSQLExportResult::Success(
	    make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(lower), std::move(upper)));
}

BoundExpressionSQLExportResult
BoundExpressionSQLExportState::ExportConjunction(const BoundConjunctionExpression &expression,
                                                 const LogicalPlanVerificationPath &path) {
	D_ASSERT(expression.GetExpressionType() == ExpressionType::CONJUNCTION_AND ||
	         expression.GetExpressionType() == ExpressionType::CONJUNCTION_OR);
	D_ASSERT(expression.GetReturnType() == LogicalType::BOOLEAN);
	D_ASSERT(expression.GetChildren().size() >= 2);
	vector<unique_ptr<ParsedExpression>> children;
	vector<LogicalPlanVerificationIssue> issues;
	ExportChildren(expression.GetChildren(), path, children, issues, LogicalType::BOOLEAN);
	if (!issues.empty()) {
		return BoundExpressionSQLExportResult::Failure(std::move(issues));
	}
	auto result = make_uniq<ConjunctionExpression>(expression.GetExpressionType());
	result->GetChildrenMutable() = std::move(children);
	return BoundExpressionSQLExportResult::Success(std::move(result));
}

BoundExpressionSQLExportResult BoundExpressionSQLExportState::ExportCase(const BoundCaseExpression &expression,
                                                                         const LogicalPlanVerificationPath &path) {
	D_ASSERT(expression.GetExpressionType() == ExpressionType::CASE_EXPR);
	D_ASSERT(expression.GetReturnType().IsComplete());
	D_ASSERT(!expression.CaseChecks().empty());
	vector<ChildExpression> source_children;
	for (auto &check : expression.CaseChecks()) {
		source_children.emplace_back(check.when_expr.get(), LogicalType::BOOLEAN);
		source_children.emplace_back(check.then_expr.get(), expression.GetReturnType());
	}
	source_children.emplace_back(expression.ElseExpression().get(), expression.GetReturnType());

	vector<unique_ptr<ParsedExpression>> children;
	vector<LogicalPlanVerificationIssue> issues;
	ExportChildren(source_children, path, children, issues);
	if (!issues.empty()) {
		return BoundExpressionSQLExportResult::Failure(std::move(issues));
	}
	auto result = make_uniq<CaseExpression>();
	for (idx_t check_index = 0; check_index < expression.CaseChecks().size(); check_index++) {
		CaseCheck check;
		check.when_expr = std::move(children[check_index * 2]);
		check.then_expr = std::move(children[check_index * 2 + 1]);
		result->CaseChecksMutable().push_back(std::move(check));
	}
	result->ElseMutable() = std::move(children.back());
	return BoundExpressionSQLExportResult::Success(std::move(result));
}

BoundExpressionSQLExportResult BoundExpressionSQLExportState::ExportOperator(const BoundOperatorExpression &expression,
                                                                             const LogicalPlanVerificationPath &path) {
	optional<LogicalType> expected_type;
	switch (expression.GetExpressionType()) {
	case ExpressionType::OPERATOR_NOT:
		D_ASSERT(expression.GetChildren().size() == 1 && expression.GetReturnType() == LogicalType::BOOLEAN);
		expected_type = LogicalType::BOOLEAN;
		break;
	case ExpressionType::OPERATOR_IS_NULL:
	case ExpressionType::OPERATOR_IS_NOT_NULL:
		D_ASSERT(expression.GetChildren().size() == 1 && expression.GetReturnType() == LogicalType::BOOLEAN);
		break;
	case ExpressionType::COMPARE_IN:
	case ExpressionType::COMPARE_NOT_IN:
		D_ASSERT(expression.GetChildren().size() >= 2 && expression.GetReturnType() == LogicalType::BOOLEAN &&
		         expression.GetChildren()[0]);
		expected_type = expression.GetChildren()[0]->GetReturnType();
		break;
	case ExpressionType::OPERATOR_COALESCE:
		D_ASSERT(expression.GetChildren().size() >= 2 && expression.GetReturnType().IsComplete());
		expected_type = expression.GetReturnType();
		break;
	case ExpressionType::OPERATOR_TRY:
		D_ASSERT(expression.GetChildren().size() == 1 && expression.GetChildren()[0] &&
		         expression.GetReturnType().IsComplete());
		if (expression.GetChildren()[0]->IsVolatile()) {
			return BoundExpressionSQLExportResult::Failure({BoundExpressionSQLExportState::UnsupportedFeature(
			    path, "try_volatile_child", "TRY cannot be rebound around a volatile expression")});
		}
		expected_type = expression.GetReturnType();
		break;
	case ExpressionType::OPERATOR_HAS_ERROR:
		D_ASSERT(expression.GetChildren().size() == 1 && expression.GetChildren()[0] &&
		         expression.GetReturnType() == LogicalType::BOOLEAN);
		if (expression.GetChildren()[0]->IsVolatile()) {
			return BoundExpressionSQLExportResult::Failure({BoundExpressionSQLExportState::UnsupportedFeature(
			    path, "has_error_volatile_child", "HAS_ERROR cannot be rebound around a volatile expression")});
		}
		expected_type = expression.GetChildren()[0]->GetReturnType();
		break;
	case ExpressionType::OPERATOR_UNPACK:
	case ExpressionType::OPERATOR_NULLIF:
	case ExpressionType::GROUPING_FUNCTION:
	case ExpressionType::ARRAY_EXTRACT:
	case ExpressionType::ARRAY_SLICE:
	case ExpressionType::STRUCT_EXTRACT:
	case ExpressionType::ARRAY_CONSTRUCTOR:
	case ExpressionType::ARROW:
		return BoundExpressionSQLExportResult::Failure({BoundExpressionSQLExportState::UnsupportedFeature(
		    path, "bound_operator", "The bound operator has no admitted parsed SQL AST form")});
	default:
		return BoundExpressionSQLExportResult::Failure({BoundExpressionSQLExportState::InternalExpressionInvariant(
		    path, expression, "Bound operator has an invalid expression type")});
	}
	vector<unique_ptr<ParsedExpression>> children;
	vector<LogicalPlanVerificationIssue> issues;
	ExportChildren(expression.GetChildren(), path, children, issues, expected_type);
	if (!issues.empty()) {
		return BoundExpressionSQLExportResult::Failure(std::move(issues));
	}
	if (expression.GetExpressionType() == ExpressionType::OPERATOR_HAS_ERROR) {
		// an unqualified has_error(...) resolves through the catalog, where a user function of the same
		// name can shadow the built-in, so the export spells out the system entry
		QualifiedName has_error_name(Identifier::SystemCatalog(), Identifier::DefaultSchema(), Identifier("has_error"));
		return BoundExpressionSQLExportResult::Success(
		    make_uniq<FunctionExpression>(has_error_name, std::move(children)));
	}
	return BoundExpressionSQLExportResult::Success(
	    make_uniq<OperatorExpression>(expression.GetExpressionType(), std::move(children)));
}

BoundExpressionSQLExportResult BoundExpressionSQLExportState::ExportChild(const Expression &expression,
                                                                          const LogicalPlanVerificationPath &path,
                                                                          idx_t child_index) {
	return Export(expression, SQLExportHelpers::ChildPath(path, child_index));
}

void BoundExpressionSQLExportState::ExportChildren(const vector<unique_ptr<Expression>> &source,
                                                   const LogicalPlanVerificationPath &path,
                                                   vector<unique_ptr<ParsedExpression>> &result,
                                                   vector<LogicalPlanVerificationIssue> &issues,
                                                   const optional<LogicalType> &expected_type) {
	vector<ChildExpression> source_children;
	for (auto &child : source) {
		source_children.emplace_back(child.get(), expected_type);
	}
	ExportChildren(source_children, path, result, issues);
}

void BoundExpressionSQLExportState::ExportChildren(const vector<ChildExpression> &source,
                                                   const LogicalPlanVerificationPath &path,
                                                   vector<unique_ptr<ParsedExpression>> &result,
                                                   vector<LogicalPlanVerificationIssue> &issues) {
	result.resize(source.size());
	for (idx_t child_index = 0; child_index < source.size(); child_index++) {
		auto child_path = SQLExportHelpers::ChildPath(path, child_index);
		auto &input = source[child_index];
		D_ASSERT(input.expression);
		D_ASSERT(!input.expected_type || input.expression->GetReturnType() == *input.expected_type);
		auto child = Export(*input.expression, child_path);
		if (child.HasError()) {
			for (auto &issue : child.GetIssues()) {
				issues.push_back(issue);
			}
		} else {
			result[child_index] = std::move(child.GetValue());
		}
	}
}

LogicalPlanVerificationResult<unique_ptr<ParsedExpression>>
BoundExpressionSQLExporter::Export(const Expression &expression, const BoundExpressionSQLExportContext &context) {
	LogicalPlanVerificationPath path;
	path.root = LogicalPlanVerificationPathRoot::STANDALONE_EXPRESSION;
	return ExportAtPath(expression, context, path);
}

LogicalPlanVerificationResult<unique_ptr<ParsedExpression>>
BoundExpressionSQLExporter::ExportAtPath(const Expression &expression, const BoundExpressionSQLExportContext &context,
                                         const LogicalPlanVerificationPath &path) {
	D_ASSERT(IsExpressionRootPath(path));
	BoundExpressionSQLExportState state(context);
	return state.Export(expression, path);
}

LogicalPlanVerificationResult<unique_ptr<FunctionExpression>>
BoundExpressionSQLExporter::ExportAggregateCallAtPath(const BoundAggregateExpression &expression,
                                                      const BoundExpressionSQLExportContext &context,
                                                      const LogicalPlanVerificationPath &path) {
	D_ASSERT(IsExpressionRootPath(path));
	BoundExpressionSQLExportState state(context);
	return state.ExportAggregateCall(expression, path);
}

LogicalPlanVerificationResult<unique_ptr<ParsedExpression>>
BoundExpressionSQLExporter::ExportWindowAtPath(const BoundWindowExpression &expression,
                                               const BoundExpressionSQLExportContext &context,
                                               const LogicalPlanVerificationPath &path) {
	D_ASSERT(IsExpressionRootPath(path));
	BoundExpressionSQLExportState state(context);
	return state.ExportWindow(expression, path);
}

LogicalPlanVerificationResult<unique_ptr<ParsedExpression>>
BoundExpressionSQLExporter::ExportUnnestAtPath(const BoundUnnestExpression &expression,
                                               const BoundExpressionSQLExportContext &context,
                                               const LogicalPlanVerificationPath &path) {
	D_ASSERT(IsExpressionRootPath(path));
	BoundExpressionSQLExportState state(context);
	return state.ExportUnnest(expression, path);
}

} // namespace duckdb
