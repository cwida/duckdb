#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/filename_pattern.hpp"
#include "duckdb/common/sql_identifier.hpp"
#include "duckdb/parser/parsed_data/copy_info.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/parser/query_node/copy_query_node.hpp"
#include "duckdb/parser/query_node/statement_node.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/operator/logical_copy_to_file.hpp"
#include "duckdb/planner/operator/logical_create_table.hpp"
#include "duckdb/planner/sql_export/logical_plan_sql_exporter_internal.hpp"

namespace duckdb {

static LogicalPlanVerificationIssue UnsupportedStatement(const LogicalPlanVerificationPath &path, string feature,
                                                         string message) {
	return LogicalPlanSQLExportHelpers::PlanUnsupportedFeature(path, std::move(feature), std::move(message));
}

static optional<LogicalPlanVerificationIssue> StatementBoundaryIssue(const LogicalPlanSQLExportContext &context,
                                                                     const LogicalPlanVerificationPath &path) {
	if (context.Ancestors().size() != 1) {
		return UnsupportedStatement(path, "dml_subplan",
		                            "A data-modifying operator below the plan root cannot be exported");
	}
	return optional<LogicalPlanVerificationIssue>();
}

//! The filename pattern string the bound segments were parsed from, empty for the default pattern.
static optional<string> FilenamePatternOption(const FilenamePattern &pattern) {
	// the legacy serialization helpers drop legacy-compatible segments, read the segments directly
	auto &segments = pattern.Segments();
	string result;
	for (auto &segment : segments) {
		switch (segment.type) {
		case FileNameSegmentType::LITERAL:
			result += segment.data;
			break;
		case FileNameSegmentType::OFFSET:
			result += "{i}";
			break;
		case FileNameSegmentType::UUID_V4:
			result += "{uuid}";
			break;
		case FileNameSegmentType::UUID_V7:
			result += "{uuidv7}";
			break;
		default:
			return optional<string>();
		}
	}
	return result;
}

LogicalPlanSQLExportResult SQLExportStatementHelpers::ExportCopyToNode(LogicalCopyToFile &op,
                                                                       LogicalPlanSQLExportContext &context,
                                                                       const LogicalPlanVerificationPath &path) {
	D_ASSERT(op.copy_info);
	if (op.copy_info->is_from || op.children.size() != 1) {
		return LogicalPlanSQLExportResult::Failure(
		    {UnsupportedStatement(path, "copy_shape", "COPY without a source query cannot be exported")});
	}
	if (op.copy_info->file_path_expression) {
		return LogicalPlanSQLExportResult::Failure(
		    {UnsupportedStatement(path, "copy_dynamic_path", "COPY with a computed file path cannot be exported")});
	}
	if (op.partition_path_expression) {
		return LogicalPlanSQLExportResult::Failure(
		    {UnsupportedStatement(path, "copy_partition_path", "COPY with PARTITION_PATH cannot be exported")});
	}
	auto pattern = FilenamePatternOption(op.filename_pattern);
	if (!pattern) {
		return LogicalPlanSQLExportResult::Failure({UnsupportedStatement(
		    path, "copy_filename_pattern", "The COPY filename pattern cannot be reconstructed as an option")});
	}
	auto fields = LogicalPlanSQLExportHelpers::CreateFields(op, path);
	if (fields.HasError()) {
		return LogicalPlanSQLExportResult::Failure(fields);
	}
	auto child = context.Export(*op.children[0], LogicalPlanSQLExportHelpers::PlanChildPath(path, 0));
	if (child.HasError()) {
		return child;
	}
	// the written file schema uses the source's output names
	if (child.GetValue().fields.size() != op.names.size()) {
		return LogicalPlanSQLExportResult::Failure(
		    {UnsupportedStatement(path, "copy_shape", "The COPY source does not cover the written columns")});
	}
	child = LogicalPlanSQLExportHelpers::ApplyOutputNames(std::move(child), op.names);
	if (child.HasError()) {
		return child;
	}
	auto info = make_uniq<CopyInfo>();
	info->is_from = false;
	// the bound format may be auto-detected or fallen back, the selected function is authoritative
	info->format = op.function.name.GetIdentifierName();
	info->is_format_auto_detected = false;
	info->file_path = op.file_path;
	// format-specific options stay in the bound info, sink options re-emit from the operator's resolved state
	for (auto &entry : op.copy_info->options) {
		info->options[entry.first] = entry.second;
	}
	auto set_option = [&](const char *name, Value value) {
		info->options[Identifier(name)] = {std::move(value)};
	};
	// binding rejects an explicit USE_TMP_FILE next to these modes and resolves it to false
	const bool use_tmp_file_implied =
	    op.per_thread_output || op.rotate || op.partition_output || !op.order_columns.empty();
	if (!use_tmp_file_implied) {
		set_option("use_tmp_file", Value::BOOLEAN(op.use_tmp_file));
	}
	switch (op.overwrite_mode) {
	case CopyOverwriteMode::COPY_OVERWRITE_OR_IGNORE:
		set_option("overwrite_or_ignore", Value::BOOLEAN(true));
		break;
	case CopyOverwriteMode::COPY_OVERWRITE:
		set_option("overwrite", Value::BOOLEAN(true));
		break;
	case CopyOverwriteMode::COPY_APPEND:
		set_option("append", Value::BOOLEAN(true));
		break;
	default:
		break;
	}
	if (*pattern != "data_{i}") {
		// everything but the default renders, including an explicitly empty pattern
		set_option("filename_pattern", Value(*pattern));
	}
	// binding defaults the extension from the function, so any difference was set explicitly,
	// including an explicitly empty extension
	if (op.file_extension != op.function.extension) {
		set_option("file_extension", Value(op.file_extension));
	}
	if (op.per_thread_output) {
		set_option("per_thread_output", Value::BOOLEAN(true));
	}
	if (op.batch_size.IsValid()) {
		// the binder backfills an unset batch size from the function's desired batch size, which a
		// rebind re-derives from the format options, so only an overriding value needs to render
		auto rederived = op.function.desired_batch_size && op.bind_data
		                     ? op.function.desired_batch_size(context.GetClientContext(), *op.bind_data)
		                     : optional_idx();
		if (op.batch_size != rederived) {
			// the SQL surface names these the row group options, batch_size is not a bindable name
			set_option("row_group_size", Value::UBIGINT(op.batch_size.GetIndex()));
		}
	}
	if (op.batch_size_bytes.IsValid()) {
		set_option("row_group_size_bytes", Value::UBIGINT(op.batch_size_bytes.GetIndex()));
	}
	if (op.batches_per_file.IsValid()) {
		set_option("row_groups_per_file", Value::UBIGINT(op.batches_per_file.GetIndex()));
	}
	if (op.file_size_bytes.IsValid()) {
		set_option("file_size_bytes", Value::UBIGINT(op.file_size_bytes.GetIndex()));
	}
	// ordered writes set partition_output without columns, which rebinding re-derives from order_by
	if (op.partition_output && !op.partition_columns.empty()) {
		vector<Value> partition_names;
		for (auto column_index : op.partition_columns) {
			partition_names.emplace_back(op.names[column_index].GetIdentifierName());
		}
		info->options[Identifier("partition_by")] = std::move(partition_names);
		if (op.write_partition_columns) {
			set_option("write_partition_columns", Value::BOOLEAN(true));
		}
		if (!op.hive_file_pattern) {
			set_option("hive_file_pattern", Value::BOOLEAN(false));
		}
	}
	if (!op.write_empty_file) {
		set_option("write_empty_file", Value::BOOLEAN(false));
	}
	if (!op.order_columns.empty()) {
		vector<Value> order_values;
		for (auto &order : op.order_columns) {
			if (order.expression->GetExpressionClass() != ExpressionClass::BOUND_REF) {
				return LogicalPlanSQLExportResult::Failure({UnsupportedStatement(
				    path, "copy_order_expression", "The COPY ordering is not a plain written column")});
			}
			string entry = SQLIdentifier::ToString(order.expression->GetAlias());
			entry += order.type == OrderType::DESCENDING ? " DESC" : " ASC";
			entry += order.null_order == OrderByNullType::NULLS_FIRST ? " NULLS FIRST" : " NULLS LAST";
			order_values.emplace_back(std::move(entry));
		}
		info->options[Identifier("order_by")] = std::move(order_values);
	}
	if (op.preserve_order != PreserveOrderType::AUTOMATIC) {
		set_option("preserve_order", Value::BOOLEAN(op.preserve_order == PreserveOrderType::PRESERVE_ORDER));
	}
	switch (GetCopyFunctionBaseReturnType(op.return_type)) {
	case CopyFunctionReturnType::CHANGED_ROWS_AND_FILE_LIST:
		set_option("return_files", Value::BOOLEAN(true));
		break;
	case CopyFunctionReturnType::WRITTEN_FILE_STATISTICS:
		set_option("return_stats", Value::BOOLEAN(true));
		break;
	default:
		break;
	}
	if (CopyFunctionReturnsColumnTypes(op.return_type)) {
		set_option("return_column_types", Value::BOOLEAN(true));
	}
	info->select_statement = std::move(child.GetValue().query);
	auto node = make_uniq<CopyQueryNode>(std::move(info));
	return LogicalPlanSQLExportResult::Success({std::move(node), std::move(fields.GetValue())});
}

LogicalPlanSQLExportResult SQLExportStatementHelpers::ExportCreateTableAsNode(LogicalCreateTable &op,
                                                                              LogicalPlanSQLExportContext &context,
                                                                              const LogicalPlanVerificationPath &path) {
	D_ASSERT(op.info && op.info->base);
	auto &base = op.info->base->Cast<CreateTableInfo>();
	if (op.children.size() != 1) {
		return LogicalPlanSQLExportResult::Failure({UnsupportedStatement(
		    path, "create_table_plain", "CREATE TABLE without a source query cannot be exported")});
	}
	auto fields = LogicalPlanSQLExportHelpers::CreateFields(op, path);
	if (fields.HasError()) {
		return LogicalPlanSQLExportResult::Failure(fields);
	}
	auto child = context.Export(*op.children[0], LogicalPlanSQLExportHelpers::PlanChildPath(path, 0));
	if (child.HasError()) {
		return child;
	}
	// the created columns take the source's output names
	vector<Identifier> column_names;
	for (auto &column : base.columns.Logical()) {
		column_names.push_back(column.Name());
	}
	if (child.GetValue().fields.size() != column_names.size()) {
		return LogicalPlanSQLExportResult::Failure({UnsupportedStatement(
		    path, "create_table_shape", "The CREATE TABLE source does not cover the created columns")});
	}
	child = LogicalPlanSQLExportHelpers::ApplyOutputNames(std::move(child), column_names);
	if (child.HasError()) {
		return child;
	}
	auto info = make_uniq<CreateTableInfo>(base.GetQualifiedName());
	info->temporary = base.temporary;
	info->on_conflict = base.on_conflict;
	for (auto &partition : base.partition_keys) {
		info->partition_keys.push_back(partition->Copy());
	}
	for (auto &order : base.sort_keys) {
		info->sort_keys.push_back(order->Copy());
	}
	for (auto &entry : base.options) {
		info->options[entry.first] = entry.second->Copy();
	}
	info->query = make_uniq<SelectStatement>();
	info->query->node = std::move(child.GetValue().query);
	auto statement = make_uniq<CreateStatement>();
	statement->info = std::move(info);
	auto node = make_uniq<StatementNode>(std::move(statement));
	return LogicalPlanSQLExportResult::Success({std::move(node), std::move(fields.GetValue())});
}

QueryNode &SQLExportStatementHelpers::SourceDefinitionNode(QueryNode &query) {
	switch (query.type) {
	case QueryNodeType::COPY_QUERY_NODE: {
		auto &info = *query.Cast<CopyQueryNode>().info;
		D_ASSERT(info.select_statement);
		return SourceDefinitionNode(*info.select_statement);
	}
	case QueryNodeType::STATEMENT_NODE: {
		auto &statement = query.Cast<StatementNode>().stmt;
		D_ASSERT(statement.type == StatementType::CREATE_STATEMENT);
		auto &info = statement.Cast<CreateStatement>().info->Cast<CreateTableInfo>();
		D_ASSERT(info.query && info.query->node);
		return *info.query->node;
	}
	default:
		return query;
	}
}

LogicalPlanSQLExportResult LogicalCopyToFile::ToSQL(LogicalPlanSQLExportContext &context,
                                                    const LogicalPlanVerificationPath &path) {
	if (auto issue = StatementBoundaryIssue(context, path)) {
		return LogicalPlanSQLExportResult::Failure({std::move(*issue)});
	}
	return SQLExportStatementHelpers::ExportCopyToNode(*this, context, path);
}

LogicalPlanSQLExportResult LogicalCreateTable::ToSQL(LogicalPlanSQLExportContext &context,
                                                     const LogicalPlanVerificationPath &path) {
	if (auto issue = StatementBoundaryIssue(context, path)) {
		return LogicalPlanSQLExportResult::Failure({std::move(*issue)});
	}
	return SQLExportStatementHelpers::ExportCreateTableAsNode(*this, context, path);
}

} // namespace duckdb
