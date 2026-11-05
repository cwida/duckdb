#include "duckdb/planner/sql_export/logical_plan_sql_exporter_internal.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/common_table_expression_info.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/planner/bound_expression_sql_exporter.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_materialized_cte.hpp"
#include "duckdb/planner/operator/logical_cteref.hpp"
#include "duckdb/planner/operator/logical_limit.hpp"
#include "duckdb/planner/operator/logical_expression_get.hpp"

namespace duckdb {

namespace {

struct LimitSQLExport {
	unique_ptr<LimitModifier> modifier;
	vector<LogicalPlanSQLExportSource> sources;
};

class LimitSQLExporter {
public:
	explicit LimitSQLExporter(LogicalPlanSQLExportContext &context_p) : context(context_p) {
	}
	LogicalPlanVerificationResult<LimitSQLExport> Export(LogicalLimit &limit, const LogicalPlanVerificationPath &path);

private:
	using LimitExpressionResult = LogicalPlanVerificationResult<unique_ptr<ParsedExpression>>;
	static bool ProducesOneRow(const LogicalOperator &op, const vector<TableIndex> &single_row_ctes = {});
	static LimitExpressionResult LimitBindingFailure(const LogicalPlanVerificationPath &path);
	LimitExpressionResult ResolveLimitColumn(const ColumnBinding &binding, LogicalOperator &input,
	                                         const LogicalPlanVerificationPath &path);
	LimitExpressionResult ExportLimitExpression(const Expression &expression, LogicalOperator &input,
	                                            const LogicalPlanVerificationPath &path,
	                                            const LogicalPlanVerificationPath &expression_path);

private:
	LogicalPlanSQLExportContext &context;
	vector<LogicalPlanSQLExportSource> sources;
};

bool LimitSQLExporter::ProducesOneRow(const LogicalOperator &op, const vector<TableIndex> &single_row_ctes) {
	if (op.type == LogicalOperatorType::LOGICAL_DUMMY_SCAN) {
		return true;
	}
	if (op.type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
		auto &aggregate = op.Cast<LogicalAggregate>();
		return aggregate.groups.empty() && aggregate.grouping_sets.size() <= 1;
	}
	if (op.type == LogicalOperatorType::LOGICAL_PROJECTION) {
		return ProducesOneRow(*op.children[0], single_row_ctes);
	}
	if (op.type == LogicalOperatorType::LOGICAL_EXPRESSION_GET) {
		return op.Cast<LogicalExpressionGet>().expressions.size() == 1 &&
		       ProducesOneRow(*op.children[0], single_row_ctes);
	}
	if (op.type == LogicalOperatorType::LOGICAL_CROSS_PRODUCT) {
		return ProducesOneRow(*op.children[0], single_row_ctes) && ProducesOneRow(*op.children[1], single_row_ctes);
	}
	if (op.type == LogicalOperatorType::LOGICAL_MATERIALIZED_CTE) {
		auto ctes = single_row_ctes;
		if (ProducesOneRow(*op.children[0], ctes)) {
			ctes.push_back(op.Cast<LogicalMaterializedCTE>().table_index);
		}
		return ProducesOneRow(*op.children[1], ctes);
	}
	if (op.type == LogicalOperatorType::LOGICAL_CTE_REF) {
		auto index = op.Cast<LogicalCTERef>().cte_index;
		return std::find(single_row_ctes.begin(), single_row_ctes.end(), index) != single_row_ctes.end();
	}
	return false;
}

LimitSQLExporter::LimitExpressionResult LimitSQLExporter::LimitBindingFailure(const LogicalPlanVerificationPath &path) {
	return LimitExpressionResult::Failure({LogicalPlanSQLExportHelpers::PlanUnsupportedFeature(
	    path, "limit_binding", "SQL LIMIT requires an independent, single-row scalar input")});
}

LimitSQLExporter::LimitExpressionResult LimitSQLExporter::ResolveLimitColumn(const ColumnBinding &binding,
                                                                             LogicalOperator &input,
                                                                             const LogicalPlanVerificationPath &path) {
	if (context.FindSource(input)) {
		// A row-stream replacement exposes columns, not the original expressions
		// that made this LIMIT input independent of the rows being limited.
		return LimitBindingFailure(path);
	}
	auto bindings = input.GetColumnBindings();
	auto found = std::find(bindings.begin(), bindings.end(), binding);
	if (found == bindings.end()) {
		return LimitBindingFailure(path);
	}
	auto column = NumericCast<idx_t>(found - bindings.begin());
	if (input.type == LogicalOperatorType::LOGICAL_PROJECTION) {
		return ExportLimitExpression(*input.expressions[column], *input.children[0],
		                             LogicalPlanSQLExportHelpers::PlanChildPath(path, 0),
		                             LogicalPlanSQLExportHelpers::PlanExpressionPath(path, column));
	}
	if (input.type == LogicalOperatorType::LOGICAL_ORDER_BY) {
		return ResolveLimitColumn(binding, *input.children[0], LogicalPlanSQLExportHelpers::PlanChildPath(path, 0));
	}
	if (input.type != LogicalOperatorType::LOGICAL_CROSS_PRODUCT) {
		return LimitBindingFailure(path);
	}
	for (idx_t child_index = 0; child_index < input.children.size(); child_index++) {
		auto &child = *input.children[child_index];
		auto child_bindings = child.GetColumnBindings();
		auto child_column = std::find(child_bindings.begin(), child_bindings.end(), binding);
		if (child_column == child_bindings.end()) {
			continue;
		}
		auto child_path = LogicalPlanSQLExportHelpers::PlanChildPath(path, child_index);
		if (!ProducesOneRow(child)) {
			return ResolveLimitColumn(binding, child, child_path);
		}
		idx_t source_index = 0;
		for (; source_index < sources.size(); source_index++) {
			if (&sources[source_index].op.get() == &child) {
				break;
			}
		}
		if (source_index == sources.size()) {
			auto exported = context.ExportChild(child, child_path, sources);
			if (exported.HasError()) {
				return LimitExpressionResult::Failure(exported);
			}
			sources.push_back(
			    {child, std::move(exported.GetValue().relation_alias), std::move(exported.GetValue().relation)});
		}
		auto scalar = make_uniq<SelectNode>();
		auto table = make_uniq<BaseTableRef>();
		table->SetTable(sources[source_index].name);
		scalar->from_table = std::move(table);
		scalar->select_list.push_back(make_uniq<ColumnRefExpression>(
		    LogicalPlanSQLExportHelpers::FieldIdentifier(NumericCast<idx_t>(child_column - child_bindings.begin()))));
		auto subquery = make_uniq<SubqueryExpression>();
		subquery->SubqueryMutable() = make_uniq<SelectStatement>();
		subquery->SubqueryMutable()->node = std::move(scalar);
		subquery->GetSubqueryTypeMutable() = SubqueryType::SCALAR;
		return LimitExpressionResult::Success(std::move(subquery));
	}
	return LimitBindingFailure(path);
}

LimitSQLExporter::LimitExpressionResult
LimitSQLExporter::ExportLimitExpression(const Expression &expression, LogicalOperator &input,
                                        const LogicalPlanVerificationPath &path,
                                        const LogicalPlanVerificationPath &expression_path) {
	if (expression.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &column = expression.Cast<BoundColumnRefExpression>();
		return column.Depth() == 0 ? ResolveLimitColumn(column.Binding(), input, path) : LimitBindingFailure(path);
	}
	if (expression.IsVolatile()) {
		return LimitBindingFailure(path);
	}
	vector<pair<vector<Identifier>, unique_ptr<ParsedExpression>>> replacements;
	vector<LogicalPlanVerificationIssue> issues;
	BoundExpressionSQLExportContext expression_context;
	expression_context.client_context = context.GetClientContext();
	expression_context.resolve_binding = [&](const ColumnBinding &binding) -> optional<ResolvedSQLColumnReference> {
		auto resolved = ResolveLimitColumn(binding, input, path);
		if (resolved.HasError()) {
			issues = resolved.GetIssues();
			return {};
		}
		auto bindings = input.GetColumnBindings();
		auto found = std::find(bindings.begin(), bindings.end(), binding);
		D_ASSERT(found != bindings.end());
		vector<Identifier> names {context.NextRelationAlias(), LogicalPlanSQLExportHelpers::FieldIdentifier(0)};
		replacements.emplace_back(names, std::move(resolved.GetValue()));
		return ResolvedSQLColumnReference {
		    std::move(names), input.types[NumericCast<idx_t>(found - bindings.begin())], {}};
	};
	auto result = BoundExpressionSQLExporter::ExportAtPath(expression, expression_context, expression_path);
	if (!issues.empty()) {
		return LimitExpressionResult::Failure(std::move(issues));
	}
	if (result.HasError()) {
		return result;
	}
	std::function<void(unique_ptr<ParsedExpression> &)> replace = [&](unique_ptr<ParsedExpression> &expr) {
		if (expr->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
			for (auto &entry : replacements) {
				if (expr->Cast<ColumnRefExpression>().ColumnNames() == entry.first) {
					expr = entry.second->Copy();
					return;
				}
			}
		}
		ParsedExpressionIterator::EnumerateChildren(*expr, replace);
	};
	replace(result.GetValue());
	return result;
}

LogicalPlanVerificationResult<LimitSQLExport> LimitSQLExporter::Export(LogicalLimit &limit,
                                                                       const LogicalPlanVerificationPath &path) {
	auto modifier = make_uniq<LimitModifier>();
	idx_t expression_ordinal = 0;
	for (idx_t i = 0; i < 2; i++) {
		auto &value = i == 0 ? limit.limit_val : limit.offset_val;
		auto &target = i == 0 ? modifier->limit : modifier->offset;
		switch (value.Type()) {
		case LimitNodeType::UNSET:
			break;
		case LimitNodeType::CONSTANT_VALUE:
			target = ConstantExpression::FromValue(Value::BIGINT(NumericCast<int64_t>(value.GetConstantValue())));
			break;
		case LimitNodeType::CONSTANT_PERCENTAGE:
			modifier->limit_type = LimitValueType::PERCENTAGE;
			target = ConstantExpression::FromValue(Value::DOUBLE(value.GetConstantPercentage()));
			break;
		case LimitNodeType::EXPRESSION_VALUE:
		case LimitNodeType::EXPRESSION_PERCENTAGE: {
			if (!value.GetExpression()->IsScalar()) {
				auto expression = ExportLimitExpression(
				    *value.GetExpression(), *limit.children[0], LogicalPlanSQLExportHelpers::PlanChildPath(path, 0),
				    LogicalPlanSQLExportHelpers::PlanExpressionPath(path, expression_ordinal++));
				if (expression.HasError()) {
					return LogicalPlanVerificationResult<LimitSQLExport>::Failure(expression);
				}
				target = std::move(expression.GetValue());
				if (value.Type() == LimitNodeType::EXPRESSION_PERCENTAGE) {
					modifier->limit_type = LimitValueType::PERCENTAGE;
				}
				break;
			}
			if (value.Type() == LimitNodeType::EXPRESSION_PERCENTAGE) {
				modifier->limit_type = LimitValueType::PERCENTAGE;
			}
			auto expression = BoundExpressionSQLExporter::ExportAtPath(
			    *value.GetExpression(), {},
			    LogicalPlanSQLExportHelpers::PlanExpressionPath(path, expression_ordinal++));
			if (expression.HasError()) {
				return LogicalPlanVerificationResult<LimitSQLExport>::Failure(expression);
			}
			target = std::move(expression.GetValue());
			break;
		}
		default:
			D_ASSERT(false);
		}
	}
	return LogicalPlanVerificationResult<LimitSQLExport>::Success({std::move(modifier), std::move(sources)});
}

} // namespace

LogicalPlanSQLExportResult LogicalLimit::ToSQL(LogicalPlanSQLExportContext &export_context,
                                               const LogicalPlanVerificationPath &path) {
	auto &limit = *this;
	D_ASSERT(limit.children.size() == 1);
	auto fields = LogicalPlanSQLExportHelpers::CreateFields(limit, path);
	if (fields.HasError()) {
		return LogicalPlanSQLExportResult::Failure(fields);
	}
	LimitSQLExporter modifier_exporter(export_context);
	auto exported = modifier_exporter.Export(limit, path);
	if (exported.HasError()) {
		return LogicalPlanSQLExportResult::Failure(exported);
	}
	auto &modifier = exported.GetValue().modifier;
	auto &sources = exported.GetValue().sources;
	auto child =
	    export_context.ExportChild(*limit.children[0], LogicalPlanSQLExportHelpers::PlanChildPath(path, 0), sources);
	if (child.HasError()) {
		return LogicalPlanSQLExportResult::Failure(child);
	}
	LogicalPlanSQLExportHelpers::PropagateSemanticTypes(fields.GetValue(), {child.GetValue()});
	if (limit.unpruned_offset.IsValid()) {
		modifier->offset =
		    ConstantExpression::FromValue(Value::BIGINT(NumericCast<int64_t>(limit.unpruned_offset.GetIndex())));
	}
	// Keep the ordering and its LIMIT consumer in the same query scope.
	bool has_limit = false;
	for (auto &existing : child.GetValue().relation.query->modifiers) {
		has_limit |= existing->type == ResultModifierType::LIMIT_MODIFIER;
	}
	unique_ptr<QueryNode> query;
	if (has_limit) {
		auto select = export_context.ForwardFields(child.GetValue(), fields.GetValue());
		select->from_table = LogicalPlanSQLExportHelpers::CreateSubquery(std::move(child.GetValue()));
		query = std::move(select);
	} else {
		query = std::move(child.GetValue().relation.query);
	}
	query->modifiers.push_back(std::move(modifier));
	for (auto &source : sources) {
		auto info = make_uniq<CommonTableExpressionInfo>();
		for (idx_t i = 0; i < source.relation.fields.size(); i++) {
			info->aliases.push_back(LogicalPlanSQLExportHelpers::FieldIdentifier(i));
		}
		info->query_node = std::move(source.relation.query);
		info->materialized = CTEMaterialize::CTE_MATERIALIZE_ALWAYS;
		query->cte_map.map.insert(source.name, std::move(info));
	}
	return LogicalPlanSQLExportResult::Success({std::move(query), std::move(fields.GetValue())});
}

} // namespace duckdb
