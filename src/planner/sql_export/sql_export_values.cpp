#include "duckdb/planner/operator/logical_empty_result.hpp"
#include "duckdb/planner/operator/logical_dummy_scan.hpp"
#include "duckdb/planner/sql_export/logical_plan_sql_exporter_internal.hpp"
#include "duckdb/planner/operator/logical_join.hpp"
#include "duckdb/function/scalar/compressed_materialization_utils.hpp"
#include "duckdb/planner/logical_plan_sql_exporter.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/parser/expression/case_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/emptytableref.hpp"
#include "duckdb/parser/tableref/expressionlistref.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/planner/bound_expression_sql_exporter.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_subquery_expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/function/lambda_functions.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/planner/operator/logical_column_data_get.hpp"
#include "duckdb/planner/operator/logical_empty_result.hpp"
#include "duckdb/planner/operator/logical_distinct.hpp"
#include "duckdb/planner/operator/logical_order.hpp"
#include "duckdb/planner/operator/logical_top_n.hpp"
#include "duckdb/planner/operator/logical_expression_get.hpp"
#include "duckdb/planner/sql_export_helpers.hpp"

namespace duckdb {

//! Whether the expression is volatile or can throw, ignoring compressed materialization wrappers
static bool IsEffectfulSemanticExpression(const Expression &expression) {
	if (auto wrapped = CMUtils::GetWrappedInput(expression)) {
		return IsEffectfulSemanticExpression(*wrapped);
	}
	bool effectful = false;
	ExpressionIterator::EnumerateChildren(
	    expression, [&](const Expression &child) { effectful = effectful || IsEffectfulSemanticExpression(child); });
	if (effectful) {
		return true;
	}
	switch (expression.GetExpressionClass()) {
	case ExpressionClass::BOUND_FUNCTION: {
		auto &bound_function = expression.Cast<BoundFunctionExpression>();
		auto &function = bound_function.Function();
		if (function.GetStability() == FunctionStability::VOLATILE ||
		    function.GetErrorMode() == FunctionErrors::CAN_THROW_RUNTIME_ERROR) {
			return true;
		}
		if (!function.HasBindLambdaCallback()) {
			return false;
		}
		auto lambda = bound_function.BindInfo()->Cast<LambdaFunctionData>().GetLambdaExpression();
		return lambda && (lambda->IsVolatile() || lambda->CanThrow());
	}
	case ExpressionClass::BOUND_AGGREGATE:
		return expression.Cast<BoundAggregateExpression>().Function().GetStability() == FunctionStability::VOLATILE;
	case ExpressionClass::BOUND_WINDOW: {
		auto &window = expression.Cast<BoundWindowExpression>();
		auto stability = window.AggregateFunction() ? window.AggregateFunction()->GetStability()
		                                            : window.WindowFunction()->GetStability();
		return stability == FunctionStability::VOLATILE;
	}
	case ExpressionClass::BOUND_SUBQUERY: {
		auto &plan = expression.Cast<BoundSubqueryExpression>().Subquery().plan;
		return plan && plan->HasVolatileExpressions();
	}
	default:
		return false;
	}
}

static bool HasEffectfulExpressionSubtree(const LogicalOperator &op) {
	for (auto &expression : LogicalPlanSQLExportHelpers::CollectExpressions(op)) {
		if (IsEffectfulSemanticExpression(expression.get())) {
			return true;
		}
	}
	for (auto &child : op.children) {
		if (HasEffectfulExpressionSubtree(*child)) {
			return true;
		}
	}
	return false;
}

static bool OrderDistinguishesValues(ClientContext &context, const LogicalType &type) {
	auto default_collation = Settings::Get<DefaultCollationSetting>(context);
	return !TypeVisitor::Contains(type, [&](const LogicalType &child) {
		if (child.IsIntegral()) {
			return false;
		}
		switch (child.id()) {
		case LogicalTypeId::BOOLEAN:
		case LogicalTypeId::DECIMAL:
		case LogicalTypeId::DATE:
		case LogicalTypeId::TIME:
		case LogicalTypeId::TIMESTAMP:
		case LogicalTypeId::TIMESTAMP_SEC:
		case LogicalTypeId::TIMESTAMP_MS:
		case LogicalTypeId::TIMESTAMP_NS:
		case LogicalTypeId::TIMESTAMP_TZ:
		case LogicalTypeId::BLOB:
		case LogicalTypeId::BIT:
		case LogicalTypeId::UUID:
		case LogicalTypeId::ENUM:
		case LogicalTypeId::LIST:
		case LogicalTypeId::ARRAY:
		case LogicalTypeId::STRUCT:
		case LogicalTypeId::MAP:
			return false;
		case LogicalTypeId::VARCHAR:
			return !StringType::GetCollation(child).empty() || !default_collation.empty();
		default:
			return true;
		}
	});
}

static bool OrdersAllFields(ClientContext &context, const vector<BoundOrderByNode> &orders, LogicalOperator &op) {
	auto bindings = op.GetColumnBindings();
	for (idx_t i = 0; i < bindings.size(); i++) {
		if (!OrderDistinguishesValues(context, op.types[i])) {
			return false;
		}
		bool found = false;
		for (auto &order : orders) {
			if (order.expression->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF &&
			    order.expression->Cast<BoundColumnRefExpression>().Binding() == bindings[i]) {
				found = true;
				break;
			}
		}
		if (!found) {
			return false;
		}
	}
	return true;
}

static bool HasChunkSensitiveConsumer(ClientContext &context, LogicalOperator &op, bool single_join_errors,
                                      bool order_required = false) {
	if (LogicalPlanSQLExportHelpers::HasEffectfulExpressions(op)) {
		return true;
	}
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN:
	case LogicalOperatorType::LOGICAL_ANY_JOIN:
	case LogicalOperatorType::LOGICAL_ASOF_JOIN:
	case LogicalOperatorType::LOGICAL_DELIM_JOIN:
		if (order_required || (single_join_errors && op.Cast<LogicalJoin>().join_type == JoinType::SINGLE)) {
			return true;
		}
		break;
	case LogicalOperatorType::LOGICAL_CROSS_PRODUCT:
	case LogicalOperatorType::LOGICAL_POSITIONAL_JOIN:
		if (order_required) {
			return true;
		}
		break;
	case LogicalOperatorType::LOGICAL_LIMIT:
		order_required = true;
		break;
	case LogicalOperatorType::LOGICAL_TOP_N:
		order_required = !OrdersAllFields(context, op.Cast<LogicalTopN>().orders, op);
		break;
	case LogicalOperatorType::LOGICAL_ORDER_BY:
		if (OrdersAllFields(context, op.Cast<LogicalOrder>().orders, op)) {
			order_required = false;
		}
		break;
	case LogicalOperatorType::LOGICAL_DISTINCT:
		order_required |= op.Cast<LogicalDistinct>().distinct_type == DistinctType::DISTINCT_ON;
		break;
	case LogicalOperatorType::LOGICAL_GET:
		if (!op.children.empty()) {
			return true;
		}
		break;
	case LogicalOperatorType::LOGICAL_PROJECTION:
	case LogicalOperatorType::LOGICAL_FILTER:
	case LogicalOperatorType::LOGICAL_EXPRESSION_GET:
	case LogicalOperatorType::LOGICAL_UNION:
	case LogicalOperatorType::LOGICAL_EXCEPT:
	case LogicalOperatorType::LOGICAL_INTERSECT:
	case LogicalOperatorType::LOGICAL_UNNEST:
	case LogicalOperatorType::LOGICAL_CHUNK_GET:
	case LogicalOperatorType::LOGICAL_DUMMY_SCAN:
	case LogicalOperatorType::LOGICAL_EMPTY_RESULT:
		break;
	default:
		return true;
	}
	for (auto &child : op.children) {
		if (HasChunkSensitiveConsumer(context, *child, single_join_errors, order_required)) {
			return true;
		}
	}
	return false;
}

LogicalPlanSQLExportResult LogicalColumnDataGet::ToSQL(LogicalPlanSQLExportContext &export_context,
                                                       const LogicalPlanVerificationPath &path) {
	auto &get = *this;
	D_ASSERT(get.children.empty() && get.collection);
	if (!get.collection.is_owned()) {
		return LogicalPlanSQLExportResult::Failure({LogicalPlanSQLExportHelpers::UnsupportedSource(
		    path, LogicalPlanSQLExportHelpers::LogicalSourceIdentity(), "borrowed_chunk_collection")});
	}
	if (get.collection->Count() == 0) {
		LogicalEmptyResult empty(get.types, get.GetColumnBindings());
		empty.ResolveOperatorTypes();
		return empty.ToSQL(export_context, path);
	}
	auto fields = LogicalPlanSQLExportHelpers::CreateFields(get, path);
	if (fields.HasError()) {
		return LogicalPlanSQLExportResult::Failure(fields);
	}
	auto values = make_uniq<ExpressionListRef>();
	values->alias = export_context.NextRelationAlias();
	values->expected_types = get.types;
	for (idx_t i = 0; i < fields.GetValue().size(); i++) {
		values->expected_names.push_back(LogicalPlanSQLExportHelpers::FieldIdentifier(i));
	}
	idx_t remaining = get.collection->Count();
	bool repacked = false;
	for (auto &chunk : get.collection->Chunks(get.GetColumnIds())) {
		repacked |= chunk.size() != MinValue<idx_t>(remaining, STANDARD_VECTOR_SIZE);
		remaining -= chunk.size();
		for (idx_t row = 0; row < chunk.size(); row++) {
			vector<unique_ptr<ParsedExpression>> exported_row;
			if (chunk.ColumnCount() == 0) {
				exported_row.push_back(make_uniq<ConstantExpression>(Literal::Null()));
			} else {
				for (idx_t column = 0; column < chunk.ColumnCount(); column++) {
					auto value =
					    BoundExpressionSQLExporter::Export(BoundConstantExpression(chunk.GetValue(column, row)), {});
					if (value.HasError()) {
						return LogicalPlanSQLExportResult::Failure({LogicalPlanSQLExportHelpers::PlanUnsupportedFeature(
						    path, "chunk_value", "The materialized value cannot be represented in SQL")});
					}
					exported_row.push_back(std::move(value.GetValue()));
				}
			}
			values->values.push_back(std::move(exported_row));
		}
	}
	if (repacked && HasChunkSensitiveConsumer(
	                    export_context.GetClientContext(), export_context.Ancestors().front().get(),
	                    Settings::Get<ScalarSubqueryErrorOnMultipleRowsSetting>(export_context.GetClientContext()))) {
		return LogicalPlanSQLExportResult::Failure({LogicalPlanSQLExportHelpers::PlanUnsupportedFeature(
		    path, "chunk_consumer_evaluation", "SQL cannot retain source chunks for an effectful consumer")});
	}
	auto select = make_uniq<SelectNode>();
	for (idx_t i = 0; i < fields.GetValue().size(); i++) {
		select->select_list.push_back(
		    make_uniq<ColumnRefExpression>(LogicalPlanSQLExportHelpers::FieldIdentifier(i), values->alias));
	}
	select->from_table = std::move(values);
	return LogicalPlanSQLExportResult::Success({std::move(select), std::move(fields.GetValue())});
}

LogicalPlanSQLExportResult LogicalExpressionGet::ToSQL(LogicalPlanSQLExportContext &export_context,
                                                       const LogicalPlanVerificationPath &path) {
	auto &get = *this;
	D_ASSERT(get.children.size() == 1 && get.children[0]);
	D_ASSERT(!get.expressions.empty() && !get.expressions[0].empty());
#ifdef D_ASSERT_IS_ENABLED
	auto column_count = get.expressions[0].size();
	D_ASSERT(get.expr_types.size() == column_count);
	for (auto &row : get.expressions) {
		D_ASSERT(row.size() == column_count);
		for (idx_t i = 0; i < column_count; i++) {
			D_ASSERT(row[i] && row[i]->GetReturnType() == get.expr_types[i]);
		}
	}
#endif
	auto fields = LogicalPlanSQLExportHelpers::CreateFields(get, path);
	if (fields.HasError()) {
		return LogicalPlanSQLExportResult::Failure(fields);
	}
	if (get.children[0]->type != LogicalOperatorType::LOGICAL_DUMMY_SCAN ||
	    export_context.FindSource(*get.children[0])) {
		return ExportSQLInput(export_context, path, std::move(fields.GetValue()));
	}

	auto values = make_uniq<ExpressionListRef>();
	values->alias = export_context.NextRelationAlias();
	values->expected_types = get.expr_types;
	for (idx_t i = 0; i < fields.GetValue().size(); i++) {
		values->expected_names.push_back(LogicalPlanSQLExportHelpers::FieldIdentifier(i));
	}
	BoundExpressionSQLExportContext expression_context;
	expression_context.client_context = &export_context.GetClientContext();
	auto expressions = LogicalPlanSQLExportHelpers::CollectExpressions(get);
	idx_t expression_ordinal = 0;
	for (auto &row : get.expressions) {
		vector<unique_ptr<ParsedExpression>> exported_row;
		for (idx_t column_index = 0; column_index < row.size(); column_index++) {
			auto expression =
			    export_context.ExportExpression(get, expressions, expression_ordinal++, expression_context, path);
			if (expression.HasError()) {
				return LogicalPlanSQLExportResult::Failure(expression);
			}
			exported_row.push_back(std::move(expression.GetValue()));
		}
		values->values.push_back(std::move(exported_row));
	}
	auto values_alias = values->alias;
	auto select = make_uniq<SelectNode>();
	for (idx_t i = 0; i < fields.GetValue().size(); i++) {
		auto expression = make_uniq<ColumnRefExpression>(LogicalPlanSQLExportHelpers::FieldIdentifier(i), values_alias);
		select->select_list.push_back(std::move(expression));
	}
	select->from_table = std::move(values);
	LogicalPlanSQLExportRelation relation {std::move(select), std::move(fields.GetValue())};
	return LogicalPlanSQLExportResult::Success(std::move(relation));
}

LogicalPlanSQLExportResult LogicalExpressionGet::ExportSQLInput(LogicalPlanSQLExportContext &export_context,
                                                                const LogicalPlanVerificationPath &path,
                                                                vector<LogicalPlanSQLExportField> fields) {
	auto &get = *this;
	if (HasEffectfulExpressionSubtree(get)) {
		return LogicalPlanSQLExportResult::Failure({LogicalPlanSQLExportHelpers::PlanUnsupportedFeature(
		    path, "values_expression_evaluation", "VALUES with input requires nonvolatile, nonthrowing expressions")});
	}
	auto child = export_context.ExportChild(*get.children[0], LogicalPlanSQLExportHelpers::PlanChildPath(path, 0));
	if (child.HasError()) {
		return LogicalPlanSQLExportResult::Failure(child);
	}
	auto expression_context =
	    LogicalPlanSQLExportHelpers::CreateBindingContext(export_context.GetClientContext(), {child.GetValue()});
	auto expressions = LogicalPlanSQLExportHelpers::CollectExpressions(get);
	auto select = make_uniq<SelectNode>();
	select->from_table = LogicalPlanSQLExportHelpers::CreateSubquery(std::move(child.GetValue()));

	auto row_alias = export_context.NextRelationAlias();
	auto cases = make_uniq<CaseExpression>();
	auto rows = make_uniq<ExpressionListRef>();
	rows->alias = row_alias;
	rows->expected_names.push_back(LogicalPlanSQLExportHelpers::FieldIdentifier(0));
	rows->expected_types.push_back(LogicalType::BIGINT);
	for (idx_t row = 0; row < get.expressions.size(); row++) {
		// Keep all expressions of a VALUES row in one evaluation group.
		vector<FunctionArgument> arguments;
		for (idx_t column = 0; column < fields.size(); column++) {
			auto expression = export_context.ExportExpression(get, expressions, row * fields.size() + column,
			                                                  expression_context, path);
			if (expression.HasError()) {
				return LogicalPlanSQLExportResult::Failure(expression);
			}
			arguments.emplace_back(LogicalPlanSQLExportHelpers::FieldIdentifier(column),
			                       std::move(expression.GetValue()));
		}
		auto value = SQLExportHelpers::SystemFunction("struct_pack", std::move(arguments));
		if (row + 1 == get.expressions.size()) {
			cases->ElseMutable() = std::move(value);
		} else {
			CaseCheck check;
			check.when_expr = make_uniq<ComparisonExpression>(
			    ExpressionType::COMPARE_EQUAL,
			    make_uniq<ColumnRefExpression>(LogicalPlanSQLExportHelpers::FieldIdentifier(0), row_alias),
			    ConstantExpression::FromValue(Value::BIGINT(int64_t(row))));
			check.then_expr = std::move(value);
			cases->CaseChecksMutable().push_back(std::move(check));
		}
		vector<unique_ptr<ParsedExpression>> ordinal;
		ordinal.push_back(ConstantExpression::FromValue(Value::BIGINT(int64_t(row))));
		rows->values.push_back(std::move(ordinal));
	}
	unique_ptr<ParsedExpression> value;
	if (get.expressions.size() == 1) {
		value = std::move(cases->ElseMutable());
	} else {
		auto join = make_uniq<JoinRef>(JoinRefType::CROSS);
		join->left = std::move(select->from_table);
		join->right = std::move(rows);
		select->from_table = std::move(join);
		value = std::move(cases);
	}
	return export_context.ExportRow(std::move(select), std::move(value), std::move(fields));
}

LogicalPlanSQLExportResult LogicalPlanSQLExportContext::ExportRow(unique_ptr<SelectNode> select,
                                                                  unique_ptr<ParsedExpression> value,
                                                                  vector<LogicalPlanSQLExportField> fields) {
	// Keep row evaluation below consumers that can filter or limit emitted rows.
	vector<unique_ptr<ParsedExpression>> list_arguments;
	list_arguments.push_back(std::move(value));
	auto list = SQLExportHelpers::SystemFunction("list_value", std::move(list_arguments));
	vector<unique_ptr<ParsedExpression>> unnest_arguments;
	unnest_arguments.push_back(std::move(list));
	auto unnest = make_uniq<FunctionExpression>(Identifier("unnest"), std::move(unnest_arguments));
	unnest->SetAlias(LogicalPlanSQLExportHelpers::FieldIdentifier(0));
	select->select_list.push_back(std::move(unnest));

	auto statement = make_uniq<SelectStatement>();
	statement->node = std::move(select);
	auto packed_alias = NextRelationAlias();
	auto packed = make_uniq<SubqueryRef>(std::move(statement), packed_alias);
	packed->column_name_alias.push_back(LogicalPlanSQLExportHelpers::FieldIdentifier(0));
	select = make_uniq<SelectNode>();
	select->from_table = std::move(packed);
	for (idx_t column = 0; column < fields.size(); column++) {
		auto value = make_uniq<OperatorExpression>(
		    ExpressionType::STRUCT_EXTRACT,
		    make_uniq<ColumnRefExpression>(LogicalPlanSQLExportHelpers::FieldIdentifier(0), packed_alias),
		    ConstantExpression::FromValue(
		        Value(LogicalPlanSQLExportHelpers::FieldIdentifier(column).GetIdentifierName())));
		value->SetAlias(LogicalPlanSQLExportHelpers::FieldIdentifier(column));
		select->select_list.push_back(std::move(value));
	}
	return LogicalPlanSQLExportResult::Success({std::move(select), std::move(fields)});
}

static LogicalPlanSQLExportResult ExportConstantSource(LogicalOperator &op, bool has_rows,
                                                       const LogicalPlanVerificationPath &path) {
	D_ASSERT(op.children.empty() && op.expressions.empty());
	auto fields = LogicalPlanSQLExportHelpers::CreateFields(op, path);
	if (fields.HasError()) {
		return LogicalPlanSQLExportResult::Failure(fields);
	}
	auto select = make_uniq<SelectNode>();
	select->from_table = make_uniq<EmptyTableRef>();
	for (idx_t i = 0; i < fields.GetValue().size(); i++) {
		auto expression = LogicalPlanSQLExportHelpers::ExportTypedNull(fields.GetValue()[i].type, path);
		if (expression.HasError()) {
			return LogicalPlanSQLExportResult::Failure(expression);
		}
		expression.GetValue()->SetAlias(LogicalPlanSQLExportHelpers::FieldIdentifier(i));
		select->select_list.push_back(std::move(expression.GetValue()));
	}
	if (!has_rows) {
		select->where_clause = ConstantExpression::FromValue(Value::BOOLEAN(false));
	}
	return LogicalPlanSQLExportResult::Success({std::move(select), std::move(fields.GetValue())});
}

LogicalPlanVerificationResult<LogicalPlanSQLExportRelation>
LogicalDummyScan::ToSQL(LogicalPlanSQLExportContext &context, const LogicalPlanVerificationPath &path) {
	return ExportConstantSource(*this, true, path);
}

LogicalPlanVerificationResult<LogicalPlanSQLExportRelation>
LogicalEmptyResult::ToSQL(LogicalPlanSQLExportContext &context, const LogicalPlanVerificationPath &path) {
	return ExportConstantSource(*this, false, path);
}

} // namespace duckdb
