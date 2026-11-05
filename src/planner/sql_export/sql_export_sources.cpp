#include "duckdb/planner/operator/logical_extension_operator.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/sql_export/logical_plan_sql_exporter_internal.hpp"
#include "duckdb/planner/logical_plan_sql_exporter.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/tableref/at_clause.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/planner/bound_expression_sql_exporter.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/operator/logical_limit.hpp"
#include "duckdb/planner/operator/logical_top_n.hpp"
#include "duckdb/planner/operator/logical_extension_operator.hpp"
#include "duckdb/planner/sql_export_helpers.hpp"
#include "duckdb/planner/operator/logical_secure_view.hpp"

namespace duckdb {

static LogicalPlanVerificationIssue ExtensionIssue(LogicalPlanVerificationIssueCode code,
                                                   const LogicalPlanVerificationPath &path, const string &identifier,
                                                   string message) {
	return SQLExportHelpers::MakeIssue(code, LogicalPlanVerificationPhase::PLAN_EXPORT, path,
	                                   LogicalPlanVerificationConstructIdentity::Extension(identifier),
	                                   std::move(message));
}

LogicalPlanSQLExportResult LogicalGet::ExportSQLSource(LogicalPlanSQLExportContext &export_context,
                                                       const LogicalPlanVerificationPath &path,
                                                       optional_ptr<const LogicalPlanSQLExportField> ordinality) {
	auto &get = *this;
	D_ASSERT(get.children.size() <= 1);
	if (get.has_pushed_projection) {
		return LogicalPlanSQLExportResult::Failure({LogicalPlanSQLExportHelpers::UnsupportedSource(
		    path, LogicalPlanSQLExportHelpers::LogicalSourceIdentity(get), "pushed_projection")});
	}
	auto fields = LogicalPlanSQLExportHelpers::CreateFields(get, path);
	if (fields.HasError()) {
		return LogicalPlanSQLExportResult::Failure(fields);
	}
	if ((!get.extra_info.file_filters.empty() || get.extra_info.total_files.IsValid()) &&
	    !get.extra_info.file_filter_expressions) {
		return LogicalPlanSQLExportResult::Failure({LogicalPlanSQLExportHelpers::PlanUnsupportedFeature(
		    path, "file_filter_residual", "The source does not retain the SQL predicate used for file pruning")});
	}
	if (get.row_group_order_options &&
	    (get.row_group_order_options->row_group_offset || get.row_group_order_options->leading_null_group_offset)) {
		bool has_unpruned_offset = false;
		for (idx_t i = export_context.Ancestors().size(); i > 0; i--) {
			auto &ancestor = export_context.Ancestors()[i - 1].get();
			if (ancestor.type == LogicalOperatorType::LOGICAL_LIMIT) {
				has_unpruned_offset = ancestor.Cast<LogicalLimit>().unpruned_offset.IsValid();
				break;
			} else if (ancestor.type == LogicalOperatorType::LOGICAL_TOP_N) {
				has_unpruned_offset = ancestor.Cast<LogicalTopN>().unpruned_offset.IsValid();
				break;
			}
		}
		if (!has_unpruned_offset) {
			return LogicalPlanSQLExportResult::Failure({LogicalPlanSQLExportHelpers::PlanUnsupportedFeature(
			    path, "pruned_offset", "The row group pruning does not retain its original SQL offset")});
		}
	}

	vector<LogicalPlanSQLExportField> scan_fields;
	for (idx_t i = 0; i < get.GetColumnIds().size(); i++) {
		auto &type = get.GetColumnType(get.GetColumnIds()[i]);
		if (!SQLExportHelpers::IsSQLValueType(type)) {
			return LogicalPlanSQLExportResult::Failure({LogicalPlanSQLExportHelpers::PlanUnsupportedFeature(
			    path, "scan_type", "Scan type cannot be represented in SQL")});
		}
		scan_fields.push_back({ColumnBinding(get.table_index, ProjectionIndex(i)), type});
	}
	unique_ptr<TableRef> input;
	if (!get.children.empty()) {
		auto child = export_context.ExportChild(*get.children[0], LogicalPlanSQLExportHelpers::PlanChildPath(path, 0));
		if (child.HasError()) {
			return LogicalPlanSQLExportResult::Failure(child);
		}
		for (auto index : get.projected_input) {
			scan_fields.push_back(child.GetValue().relation.fields[index]);
		}
		input = LogicalPlanSQLExportHelpers::CreateSubquery(std::move(child.GetValue()));
	}
	if (ordinality) {
		fields.GetValue().push_back(*ordinality);
		scan_fields.push_back(*ordinality);
	}
	auto relation_alias = export_context.NextRelationAlias();
	auto source_sql = LogicalPlanSQLExportHelpers::ReconstructSQLSource(
	    export_context.GetClientContext(), get, std::move(input), relation_alias, bool(ordinality));
	if (!source_sql.query) {
		auto guard = source_sql.unsupported_reason.empty() ? "to_sql_callback_declined" : source_sql.unsupported_reason;
		return LogicalPlanSQLExportResult::Failure({LogicalPlanSQLExportHelpers::UnsupportedSource(
		    path, LogicalPlanSQLExportHelpers::LogicalSourceIdentity(get), std::move(guard))});
	}
	auto query = std::move(source_sql.query);
	if (get.extra_info.sample_options) {
		auto sampling = get.extra_info.sample_options->Copy();
		if (!sampling->repeatable && sampling->seed.IsValid()) {
			sampling->seed = optional_idx::Invalid();
		}
		if (sampling->repeatable && !sampling->seed.IsValid()) {
			return LogicalPlanSQLExportResult::Failure({LogicalPlanSQLExportHelpers::PlanUnsupportedFeature(
			    path, "sample_repeatability", "SQL sampling seeds imply repeatable sampling")});
		}
		if (sampling->seed.IsValid() && sampling->seed.GetIndex() > idx_t(NumericLimits<int64_t>::Maximum())) {
			return LogicalPlanSQLExportResult::Failure({LogicalPlanSQLExportHelpers::PlanUnsupportedFeature(
			    path, "sample_seed", "The sampling seed has no SQL spelling")});
		}
		sampling->sample_rate = -1.0;
		LogicalPlanSQLExportedChild unsampled {{std::move(query), scan_fields}, export_context.NextRelationAlias()};
		auto sampled = export_context.ForwardFields(unsampled, scan_fields);
		sampled->sample = std::move(sampling);
		sampled->from_table = LogicalPlanSQLExportHelpers::CreateSubquery(std::move(unsampled));
		query = std::move(sampled);
	}
	LogicalPlanSQLExportedChild source {{std::move(query), std::move(scan_fields)}, std::move(relation_alias)};
	auto plain = LogicalPlanSQLExportHelpers::PlainScope(*source.relation.query);
	if (plain && (plain->where_clause || plain->select_list.size() != source.relation.fields.size())) {
		plain = nullptr;
	}
	auto binding_context =
	    LogicalPlanSQLExportHelpers::CreateBindingContext(export_context.GetClientContext(), {source}, {plain});
	auto select = export_context.ForwardFields(source, fields.GetValue(), plain);
	vector<unique_ptr<Expression>> predicates;
	for (auto &entry : get.table_filters) {
		if (ExpressionFilter::IsOptionalFilter(entry.Filter())) {
			continue;
		}
		auto &field = source.relation.fields[entry.GetIndex().GetIndex()];
		BoundColumnRefExpression column(field.type, field.source_binding);
		predicates.push_back(entry.Filter().ToExpression(column));
	}
	auto conjunction = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
	auto ordinal = LogicalPlanSQLExportHelpers::CollectExpressions(get).size();
	for (auto &predicate : predicates) {
		auto exported = BoundExpressionSQLExporter::ExportAtPath(
		    *predicate, binding_context, LogicalPlanSQLExportHelpers::PlanExpressionPath(path, ordinal++));
		if (exported.HasError()) {
			return LogicalPlanSQLExportResult::Failure(exported);
		}
		conjunction->AddExpression(std::move(exported.GetValue()));
	}
	if (conjunction->GetChildren().size() == 1) {
		select->where_clause = std::move(conjunction->GetChildrenMutable()[0]);
	} else if (!conjunction->GetChildren().empty()) {
		select->where_clause = std::move(conjunction);
	}
	LogicalPlanSQLExportHelpers::SetChildScope(*select, std::move(source), plain);
	return LogicalPlanSQLExportResult::Success({std::move(select), std::move(fields.GetValue())});
}

LogicalPlanSQLExportResult LogicalSecureView::ToSQL(LogicalPlanSQLExportContext &export_context,
                                                    const LogicalPlanVerificationPath &path) {
	auto &view = *this;
	auto fields = LogicalPlanSQLExportHelpers::CreateFields(view, path);
	if (fields.HasError()) {
		return LogicalPlanSQLExportResult::Failure(fields);
	}
	if (!view.has_source || view.source_name.Path().empty() || view.source_types.empty()) {
		return LogicalPlanSQLExportResult::Failure({LogicalPlanSQLExportHelpers::PlanUnsupportedFeature(
		    path, "secure_view_source", "The secure view does not retain its qualified source metadata")});
	}
	for (auto &component : view.source_name.Path()) {
		if (component.empty()) {
			return LogicalPlanSQLExportResult::Failure({LogicalPlanSQLExportHelpers::PlanUnsupportedFeature(
			    path, "secure_view_source", "The secure view source name cannot be represented in SQL")});
		}
	}
	if (view.output_bindings.size() != fields.GetValue().size() ||
	    view.output_expressions.size() != fields.GetValue().size()) {
		return LogicalPlanSQLExportResult::Failure({LogicalPlanSQLExportHelpers::PlanUnsupportedFeature(
		    path, "secure_view_output", "The secure view output mapping is incomplete")});
	}
	for (auto &type : view.source_types) {
		if (!SQLExportHelpers::IsSQLRepresentableType(type)) {
			return LogicalPlanSQLExportResult::Failure({LogicalPlanSQLExportHelpers::PlanUnsupportedFeature(
			    path, "secure_view_source", "A secure view source type cannot be represented in SQL")});
		}
	}

	if (view.source_filters.size() != view.pushed_filters.size()) {
		return LogicalPlanSQLExportResult::Failure({LogicalPlanSQLExportHelpers::PlanUnsupportedFeature(
		    path, "secure_view_filter", "The secure view does not retain every caller predicate")});
	}

	auto source_alias = export_context.NextRelationAlias();
	auto table = make_uniq<BaseTableRef>();
	table->SetQualifiedName(view.source_name);
	table->alias = source_alias;
	for (idx_t i = 0; i < view.source_types.size(); i++) {
		table->column_name_alias.push_back(LogicalPlanSQLExportHelpers::FieldIdentifier(i));
	}
	if (view.has_at_clause) {
		table->at_clause = make_uniq<AtClause>(view.at_unit, ConstantExpression::FromValue(view.at_value));
	}

	BoundExpressionSQLExportContext expression_context;
	expression_context.client_context = &export_context.GetClientContext();
	expression_context.resolve_binding = [source_alias, source_types = view.source_types](
	                                         const ColumnBinding &binding) -> optional<ResolvedSQLColumnReference> {
		if (binding.table_index != TableIndex(0) || binding.column_index.GetIndex() >= source_types.size()) {
			return {};
		}
		auto index = binding.column_index.GetIndex();
		return ResolvedSQLColumnReference {{source_alias, LogicalPlanSQLExportHelpers::FieldIdentifier(index)},
		                                   source_types[index]};
	};

	auto select = make_uniq<SelectNode>();
	select->from_table = std::move(table);
	for (idx_t i = 0; i < fields.GetValue().size(); i++) {
		if (view.output_bindings[i] != fields.GetValue()[i].source_binding ||
		    !view.output_expressions[i]->GetReturnType().EqualsIncludingCollation(fields.GetValue()[i].type)) {
			return LogicalPlanSQLExportResult::Failure({LogicalPlanSQLExportHelpers::PlanUnsupportedFeature(
			    path, "secure_view_output", "The secure view output mapping does not match its current schema")});
		}
		auto expression = BoundExpressionSQLExporter::ExportAtPath(
		    *view.output_expressions[i], expression_context, LogicalPlanSQLExportHelpers::PlanExpressionPath(path, i));
		if (expression.HasError()) {
			return LogicalPlanSQLExportResult::Failure(expression);
		}
		expression.GetValue()->SetAlias(LogicalPlanSQLExportHelpers::FieldIdentifier(i));
		select->select_list.push_back(std::move(expression.GetValue()));
	}
	for (idx_t i = 0; i < view.source_filters.size(); i++) {
		if (!view.source_filters[i]) {
			return LogicalPlanSQLExportResult::Failure({LogicalPlanSQLExportHelpers::PlanUnsupportedFeature(
			    path, "secure_view_filter", "The secure view caller predicate has no complete source mapping")});
		}
		auto predicate = BoundExpressionSQLExporter::ExportAtPath(
		    *view.source_filters[i], expression_context,
		    LogicalPlanSQLExportHelpers::PlanExpressionPath(path, fields.GetValue().size() + i));
		if (predicate.HasError()) {
			return LogicalPlanSQLExportResult::Failure(predicate);
		}
		select->where_clause =
		    SQLExportHelpers::Conjoin(std::move(select->where_clause), std::move(predicate.GetValue()));
	}

	return LogicalPlanSQLExportResult::Success({std::move(select), std::move(fields.GetValue())});
}

LogicalPlanVerificationResult<LogicalPlanSQLExportRelation> LogicalGet::ToSQL(LogicalPlanSQLExportContext &context,
                                                                              const LogicalPlanVerificationPath &path) {
	return ExportSQLSource(context, path);
}

LogicalPlanVerificationResult<LogicalPlanSQLExportRelation>
LogicalExtensionOperator::ToSQL(LogicalPlanSQLExportContext &context, const LogicalPlanVerificationPath &path) {
	return LogicalPlanSQLExportResult::Failure(
	    {ExtensionIssue(LogicalPlanVerificationIssueCode::UNSUPPORTED_EXTENSION, path, GetExtensionName(),
	                    "The extension operator does not implement SQL reconstruction")});
}

} // namespace duckdb
