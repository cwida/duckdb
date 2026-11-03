#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/query_node/delete_query_node.hpp"
#include "duckdb/parser/query_node/insert_query_node.hpp"
#include "duckdb/parser/query_node/merge_query_node.hpp"
#include "duckdb/parser/query_node/update_query_node.hpp"
#include "duckdb/parser/statement/insert_statement.hpp"
#include "duckdb/parser/statement/merge_into_statement.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_delete.hpp"
#include "duckdb/planner/operator/logical_insert.hpp"
#include "duckdb/planner/operator/logical_copy_to_file.hpp"
#include "duckdb/planner/operator/logical_merge_into.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_update.hpp"
#include "duckdb/planner/sql_export/logical_plan_sql_exporter_internal.hpp"

namespace duckdb {

static LogicalPlanVerificationIssue UnsupportedDML(const LogicalPlanVerificationPath &path, string feature,
                                                   string message) {
	return LogicalPlanSQLExportHelpers::PlanUnsupportedFeature(path, std::move(feature), std::move(message));
}

struct ExportedRowId {
	Identifier name;
	//! Index of the key column within the row-id columns the binder appends to the source
	idx_t ordinal;
	//! Number of appended row-id columns
	idx_t count;
};

//! The row-id column SQL can reference on the modified table. A single row-id column is the key by
//! itself. Among several, only the logical row identifier can key the generated statement: the
//! remaining columns are physical coordinates, not identity. Row-id sets without such a key have no
//! SQL form, and a physical column with the row-id name would capture the generated references.
static LogicalPlanVerificationResult<ExportedRowId> ExportedRowIdName(const TableCatalogEntry &table,
                                                                      const LogicalPlanVerificationPath &path) {
	auto row_id_columns = table.GetRowIdColumns();
	auto virtual_columns = table.GetVirtualColumns();
	optional<column_t> selected;
	idx_t ordinal = 0;
	if (row_id_columns.size() == 1) {
		selected = row_id_columns[0];
	} else {
		for (idx_t i = 0; i < row_id_columns.size(); i++) {
			if (row_id_columns[i] == COLUMN_IDENTIFIER_ROW_ID) {
				selected = row_id_columns[i];
				ordinal = i;
				break;
			}
		}
	}
	optional_ptr<const TableColumn> column;
	if (selected) {
		auto entry = virtual_columns.find(*selected);
		if (entry != virtual_columns.end()) {
			column = &entry->second;
		}
	}
	if (!column || table.ColumnExists(column->name)) {
		return LogicalPlanVerificationResult<ExportedRowId>::Failure(
		    {UnsupportedDML(path, "dml_row_id", "The modified table does not expose an exportable row-id column")});
	}
	return LogicalPlanVerificationResult<ExportedRowId>::Success(
	    {Identifier(column->name), ordinal, row_id_columns.size()});
}

static unique_ptr<TableRef> ModifiedTableRef(const TableCatalogEntry &table) {
	auto reference = make_uniq<BaseTableRef>();
	reference->SetQualifiedName(table.schema.GetQualifiedName(table.name));
	return std::move(reference);
}

static LogicalPlanSQLExportResult ExportInsertNode(LogicalInsert &op, LogicalPlanSQLExportContext &context,
                                                   const LogicalPlanVerificationPath &path) {
	if (op.on_conflict_info.action_type != OnConflictAction::THROW || !op.insert_values.empty()) {
		return LogicalPlanSQLExportResult::Failure(
		    {UnsupportedDML(path, "insert_on_conflict", "INSERT ... ON CONFLICT cannot be exported")});
	}
	if (op.children.size() != 1) {
		return LogicalPlanSQLExportResult::Failure(
		    {UnsupportedDML(path, "dml_source", "INSERT without a source relation cannot be exported")});
	}
	auto fields = LogicalPlanSQLExportHelpers::CreateFields(op, path);
	if (fields.HasError()) {
		return LogicalPlanSQLExportResult::Failure(fields);
	}
	auto child = context.Export(*op.children[0], LogicalPlanSQLExportHelpers::PlanChildPath(path, 0));
	if (child.HasError()) {
		return child;
	}
	// The bound source produces every physical column in declaration order, with defaults filled in.
	vector<Identifier> target_columns;
	for (auto &column : op.table.GetColumns().Physical()) {
		target_columns.push_back(column.Name());
	}
	if (child.GetValue().fields.size() != target_columns.size()) {
		return LogicalPlanSQLExportResult::Failure(
		    {UnsupportedDML(path, "insert_source", "The INSERT source does not cover the table's physical columns")});
	}
	auto node = make_uniq<InsertQueryNode>();
	node->qualified_name = op.table.schema.GetQualifiedName(op.table.name);
	node->columns = std::move(target_columns);
	node->select_statement = make_uniq<SelectStatement>();
	node->select_statement->node = std::move(child.GetValue().query);
	return LogicalPlanSQLExportResult::Success({std::move(node), std::move(fields.GetValue())});
}

static LogicalPlanSQLExportResult ExportDeleteNode(LogicalDelete &op, LogicalPlanSQLExportContext &context,
                                                   const LogicalPlanVerificationPath &path) {
	if (op.children.size() != 1) {
		return LogicalPlanSQLExportResult::Failure(
		    {UnsupportedDML(path, "dml_source", "DELETE without a source relation cannot be exported")});
	}
	auto row_id = ExportedRowIdName(op.table, path);
	if (row_id.HasError()) {
		return LogicalPlanSQLExportResult::Failure(row_id);
	}
	auto fields = LogicalPlanSQLExportHelpers::CreateFields(op, path);
	if (fields.HasError()) {
		return LogicalPlanSQLExportResult::Failure(fields);
	}
	// The row-id reference the operator deletes by carries the row-id column's name as its alias.
	optional_ptr<const BoundColumnRefExpression> row_id_reference;
	bool ambiguous = false;
	for (auto &expression : op.expressions) {
		if (expression->GetAlias() != row_id.GetValue().name) {
			continue;
		}
		ambiguous |= static_cast<bool>(row_id_reference);
		if (expression->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
			row_id_reference = &expression->Cast<BoundColumnRefExpression>();
		}
	}
	if (!row_id_reference || ambiguous || row_id_reference->Depth() != 0) {
		return LogicalPlanSQLExportResult::Failure(
		    {UnsupportedDML(path, "dml_row_id", "The deleted row-id column was not found in the plan")});
	}
	auto child = context.ExportChild(*op.children[0], LogicalPlanSQLExportHelpers::PlanChildPath(path, 0));
	if (child.HasError()) {
		return LogicalPlanSQLExportResult::Failure(child);
	}
	auto &child_fields = child.GetValue().relation.fields;
	optional_idx row_id_field;
	for (idx_t i = 0; i < child_fields.size(); i++) {
		if (child_fields[i].source_binding == row_id_reference->Binding()) {
			row_id_field = i;
			break;
		}
	}
	if (!row_id_field.IsValid()) {
		return LogicalPlanSQLExportResult::Failure(
		    {UnsupportedDML(path, "dml_row_id", "The deleted row-id column was not found in the exported source")});
	}
	auto plain = LogicalPlanSQLExportHelpers::PlainScope(*child.GetValue().relation.query);
	auto select = make_uniq<SelectNode>();
	select->select_list.push_back(
	    LogicalPlanSQLExportHelpers::ChildColumn(child.GetValue(), row_id_field.GetIndex(), plain));
	LogicalPlanSQLExportHelpers::SetChildScope(*select, std::move(child.GetValue()), plain);
	auto subquery = make_uniq<SubqueryExpression>();
	subquery->SubqueryMutable() = make_uniq<SelectStatement>();
	subquery->SubqueryMutable()->node = std::move(select);
	subquery->GetSubqueryTypeMutable() = SubqueryType::ANY;
	subquery->GetComparisonTypeMutable() = ExpressionType::COMPARE_EQUAL;
	subquery->GetChildMutable() = make_uniq<ColumnRefExpression>(row_id.GetValue().name);
	auto node = make_uniq<DeleteQueryNode>();
	node->table = ModifiedTableRef(op.table);
	node->condition = std::move(subquery);
	return LogicalPlanSQLExportResult::Success({std::move(node), std::move(fields.GetValue())});
}

static LogicalPlanSQLExportResult ExportUpdateNode(LogicalUpdate &op, LogicalPlanSQLExportContext &context,
                                                   const LogicalPlanVerificationPath &path) {
	if (op.children.size() != 1) {
		return LogicalPlanSQLExportResult::Failure(
		    {UnsupportedDML(path, "dml_source", "UPDATE without a source relation cannot be exported")});
	}
	auto row_id = ExportedRowIdName(op.table, path);
	if (row_id.HasError()) {
		return LogicalPlanSQLExportResult::Failure(row_id);
	}
	auto fields = LogicalPlanSQLExportHelpers::CreateFields(op, path);
	if (fields.HasError()) {
		return LogicalPlanSQLExportResult::Failure(fields);
	}
	// Keep the table's own name reserved so the generated source alias cannot capture it.
	context.NextRelationAlias(op.table.name);
	auto child = context.ExportChild(*op.children[0], LogicalPlanSQLExportHelpers::PlanChildPath(path, 0));
	if (child.HasError()) {
		return LogicalPlanSQLExportResult::Failure(child);
	}
	auto &child_fields = child.GetValue().relation.fields;
	auto row_id_count = row_id.GetValue().count;
	if (op.expressions.size() != op.columns.size() || child_fields.size() < op.columns.size() + row_id_count) {
		return LogicalPlanSQLExportResult::Failure(
		    {UnsupportedDML(path, "dml_row_id", "The updated row-id column was not found in the exported source")});
	}
	// The bound source keeps every row-id column as its final columns, the selected key among them.
	auto row_id_field = child_fields.size() - row_id_count + row_id.GetValue().ordinal;
	auto expression_context =
	    LogicalPlanSQLExportHelpers::CreateBindingContext(context.GetClientContext(), {child.GetValue()});
	vector<reference<const Expression>> set_expressions;
	for (auto &expression : op.expressions) {
		set_expressions.push_back(*expression);
	}
	auto set_info = make_uniq<UpdateSetInfo>();
	for (idx_t i = 0; i < op.columns.size(); i++) {
		auto exported = context.ExportExpression(op, set_expressions, i, expression_context, path);
		if (exported.HasError()) {
			return LogicalPlanSQLExportResult::Failure(exported);
		}
		set_info->columns.push_back(op.table.GetColumns().GetColumn(op.columns[i]).Name());
		set_info->expressions.push_back(std::move(exported.GetValue()));
	}
	set_info->condition = make_uniq<ComparisonExpression>(
	    ExpressionType::COMPARE_EQUAL, make_uniq<ColumnRefExpression>(row_id.GetValue().name, op.table.name),
	    LogicalPlanSQLExportHelpers::ChildColumn(child.GetValue(), row_id_field));
	auto node = make_uniq<UpdateQueryNode>();
	node->table = ModifiedTableRef(op.table);
	node->from_table = LogicalPlanSQLExportHelpers::CreateSubquery(std::move(child.GetValue()));
	node->set_info = std::move(set_info);
	return LogicalPlanSQLExportResult::Success({std::move(node), std::move(fields.GetValue())});
}

//! The merged rows are keyed back to the target by its row id: a MATCHED source row carries the
//! matched row id, a NOT MATCHED (by target) row carries NULL and never rejoins. A target row the
//! source never matched does not appear as unmatched under this key, so WHEN NOT MATCHED BY SOURCE
//! actions cannot be reconstructed.
static LogicalPlanSQLExportResult ExportMergeNode(LogicalMergeInto &op, LogicalPlanSQLExportContext &context,
                                                  const LogicalPlanVerificationPath &path) {
	if (op.children.size() != 1) {
		return LogicalPlanSQLExportResult::Failure(
		    {UnsupportedDML(path, "dml_source", "MERGE INTO without a source relation cannot be exported")});
	}
	if (op.actions.count(MergeActionCondition::WHEN_NOT_MATCHED_BY_SOURCE)) {
		return LogicalPlanSQLExportResult::Failure(
		    {UnsupportedDML(path, "merge_not_matched_by_source",
		                    "WHEN NOT MATCHED BY SOURCE actions cannot be keyed by the exported row id")});
	}
	auto row_id = ExportedRowIdName(op.table, path);
	if (row_id.HasError()) {
		return LogicalPlanSQLExportResult::Failure(row_id);
	}
	auto fields = LogicalPlanSQLExportHelpers::CreateFields(op, path);
	if (fields.HasError()) {
		return LogicalPlanSQLExportResult::Failure(fields);
	}
	// Keep the table's own name reserved so the generated source alias cannot capture it.
	context.NextRelationAlias(op.table.name);
	auto child = context.ExportChild(*op.children[0], LogicalPlanSQLExportHelpers::PlanChildPath(path, 0));
	if (child.HasError()) {
		return LogicalPlanSQLExportResult::Failure(child);
	}
	// The bound source keeps every row-id column as its final columns, after every action input.
	if (op.row_id_start + row_id.GetValue().count != child.GetValue().relation.fields.size()) {
		return LogicalPlanSQLExportResult::Failure(
		    {UnsupportedDML(path, "dml_row_id", "The merged row-id column was not found in the exported source")});
	}
	auto expression_context =
	    LogicalPlanSQLExportHelpers::CreateBindingContext(context.GetClientContext(), {child.GetValue()});
	// The action inputs are exported in a stable order: per action its condition, then its expressions.
	vector<reference<const Expression>> action_inputs;
	for (auto &entry : op.actions) {
		for (auto &action : entry.second) {
			if (action->condition) {
				action_inputs.push_back(*action->condition);
			}
			for (auto &expression : action->expressions) {
				action_inputs.push_back(*expression);
			}
		}
	}
	auto node = make_uniq<MergeQueryNode>();
	node->target = ModifiedTableRef(op.table);
	node->join_condition = make_uniq<ComparisonExpression>(
	    ExpressionType::COMPARE_EQUAL, make_uniq<ColumnRefExpression>(row_id.GetValue().name, op.table.name),
	    LogicalPlanSQLExportHelpers::ChildColumn(child.GetValue(), op.row_id_start + row_id.GetValue().ordinal));
	idx_t input_ordinal = 0;
	auto export_input = [&]() {
		return context.ExportExpression(op, action_inputs, input_ordinal++, expression_context, path);
	};
	for (auto &entry : op.actions) {
		vector<unique_ptr<MergeIntoAction>> actions;
		for (auto &bound_action : entry.second) {
			auto action = make_uniq<MergeIntoAction>();
			action->action_type = bound_action->action_type;
			if (bound_action->condition) {
				auto condition = export_input();
				if (condition.HasError()) {
					return LogicalPlanSQLExportResult::Failure(condition);
				}
				action->condition = std::move(condition.GetValue());
			}
			switch (bound_action->action_type) {
			case MergeActionType::MERGE_UPDATE: {
				if (bound_action->columns.size() != bound_action->expressions.size()) {
					return LogicalPlanSQLExportResult::Failure({UnsupportedDML(
					    path, "merge_update", "The MERGE update action does not pair its columns and expressions")});
				}
				auto set_info = make_uniq<UpdateSetInfo>();
				for (idx_t i = 0; i < bound_action->columns.size(); i++) {
					auto value = export_input();
					if (value.HasError()) {
						return LogicalPlanSQLExportResult::Failure(value);
					}
					set_info->columns.push_back(op.table.GetColumns().GetColumn(bound_action->columns[i]).Name());
					set_info->expressions.push_back(std::move(value.GetValue()));
				}
				action->update_info = std::move(set_info);
				break;
			}
			case MergeActionType::MERGE_INSERT: {
				// the bound action provides every physical column in declaration order, with defaults filled in
				for (auto &column : op.table.GetColumns().Physical()) {
					action->insert_columns.push_back(column.Name());
				}
				if (bound_action->expressions.size() != action->insert_columns.size()) {
					return LogicalPlanSQLExportResult::Failure({UnsupportedDML(
					    path, "merge_insert", "The MERGE insert action does not cover the table's physical columns")});
				}
				for (idx_t i = 0; i < bound_action->expressions.size(); i++) {
					auto value = export_input();
					if (value.HasError()) {
						return LogicalPlanSQLExportResult::Failure(value);
					}
					action->expressions.push_back(std::move(value.GetValue()));
				}
				break;
			}
			case MergeActionType::MERGE_ERROR: {
				for (idx_t i = 0; i < bound_action->expressions.size(); i++) {
					auto message = export_input();
					if (message.HasError()) {
						return LogicalPlanSQLExportResult::Failure(message);
					}
					action->expressions.push_back(std::move(message.GetValue()));
				}
				break;
			}
			default:
				// DELETE and DO NOTHING carry no inputs
				break;
			}
			actions.push_back(std::move(action));
		}
		node->actions.emplace(entry.first, std::move(actions));
	}
	node->source = LogicalPlanSQLExportHelpers::CreateSubquery(std::move(child.GetValue()));
	return LogicalPlanSQLExportResult::Success({std::move(node), std::move(fields.GetValue())});
}

static optional<LogicalPlanVerificationIssue> DMLStatementIssue(const LogicalOperator &op,
                                                                const LogicalPlanVerificationPath &path) {
	if (op.type == LogicalOperatorType::LOGICAL_UPDATE && op.Cast<LogicalUpdate>().capture_old_rows) {
		return UnsupportedDML(path, "dml_old_rows", "UPDATE with OLD row capture cannot be exported");
	}
	// A bare operator never returns modified rows; that shape only exists under a RETURNING projection.
	bool return_chunk;
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_INSERT:
		return_chunk = op.Cast<LogicalInsert>().return_chunk;
		break;
	case LogicalOperatorType::LOGICAL_UPDATE:
		return_chunk = op.Cast<LogicalUpdate>().return_chunk;
		break;
	case LogicalOperatorType::LOGICAL_MERGE_INTO:
		return_chunk = op.Cast<LogicalMergeInto>().return_chunk;
		break;
	case LogicalOperatorType::LOGICAL_COPY_TO_FILE:
		return_chunk = false;
		break;
	default:
		return_chunk = op.Cast<LogicalDelete>().return_chunk;
		break;
	}
	if (return_chunk) {
		return UnsupportedDML(path, "dml_returning", "A data-modifying operator returning rows cannot be exported");
	}
	return optional<LogicalPlanVerificationIssue>();
}

static optional<LogicalPlanVerificationIssue> DMLBoundaryIssue(const LogicalPlanSQLExportContext &context,
                                                               const LogicalOperator &op,
                                                               const LogicalPlanVerificationPath &path) {
	if (context.Ancestors().size() != 1) {
		return UnsupportedDML(path, "dml_subplan", "A data-modifying operator below the plan root cannot be exported");
	}
	return DMLStatementIssue(op, path);
}

LogicalPlanSQLExportResult LogicalInsert::ToSQL(LogicalPlanSQLExportContext &context,
                                                const LogicalPlanVerificationPath &path) {
	if (auto issue = DMLBoundaryIssue(context, *this, path)) {
		return LogicalPlanSQLExportResult::Failure({std::move(*issue)});
	}
	return ExportInsertNode(*this, context, path);
}

LogicalPlanSQLExportResult LogicalDelete::ToSQL(LogicalPlanSQLExportContext &context,
                                                const LogicalPlanVerificationPath &path) {
	if (auto issue = DMLBoundaryIssue(context, *this, path)) {
		return LogicalPlanSQLExportResult::Failure({std::move(*issue)});
	}
	return ExportDeleteNode(*this, context, path);
}

LogicalPlanSQLExportResult LogicalUpdate::ToSQL(LogicalPlanSQLExportContext &context,
                                                const LogicalPlanVerificationPath &path) {
	if (auto issue = DMLBoundaryIssue(context, *this, path)) {
		return LogicalPlanSQLExportResult::Failure({std::move(*issue)});
	}
	return ExportUpdateNode(*this, context, path);
}

LogicalPlanSQLExportResult LogicalMergeInto::ToSQL(LogicalPlanSQLExportContext &context,
                                                   const LogicalPlanVerificationPath &path) {
	if (auto issue = DMLBoundaryIssue(context, *this, path)) {
		return LogicalPlanSQLExportResult::Failure({std::move(*issue)});
	}
	return ExportMergeNode(*this, context, path);
}

//! An identity projection over a data-modifying body only forwards its columns under new bindings.
static bool IsIdentityProjectionOverDML(LogicalOperator &op) {
	if (op.type != LogicalOperatorType::LOGICAL_PROJECTION || op.children.size() != 1 ||
	    !SQLExportDMLHelpers::MatchDMLBody(*op.children[0])) {
		return false;
	}
	auto child_fields = LogicalPlanSQLExportHelpers::CreateFields(*op.children[0], LogicalPlanVerificationPath());
	return child_fields.IsSuccess() &&
	       LogicalPlanSQLExportHelpers::IsIdentityProjection(op.Cast<LogicalProjection>(), child_fields.GetValue());
}

bool SQLExportDMLHelpers::MatchDMLBody(LogicalOperator &op) {
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_INSERT:
	case LogicalOperatorType::LOGICAL_UPDATE:
	case LogicalOperatorType::LOGICAL_DELETE:
	case LogicalOperatorType::LOGICAL_MERGE_INTO:
	case LogicalOperatorType::LOGICAL_COPY_TO_FILE:
		return true;
	default:
		return MatchReturningRoot(op) || IsIdentityProjectionOverDML(op);
	}
}

bool SQLExportDMLHelpers::ContainsReplacedBoundary(const LogicalPlanSQLExportContext &context, LogicalOperator &op) {
	reference<LogicalOperator> current(op);
	while (true) {
		if (context.FindSource(current)) {
			return true;
		}
		auto &node = current.get();
		if (node.type == LogicalOperatorType::LOGICAL_PROJECTION && node.children.size() == 1 &&
		    MatchDMLBody(*node.children[0])) {
			current = *node.children[0];
			continue;
		}
		return false;
	}
}

bool SQLExportDMLHelpers::IsDMLNode(const QueryNode &query) {
	switch (query.type) {
	case QueryNodeType::INSERT_QUERY_NODE:
	case QueryNodeType::UPDATE_QUERY_NODE:
	case QueryNodeType::DELETE_QUERY_NODE:
	case QueryNodeType::MERGE_QUERY_NODE:
	case QueryNodeType::COPY_QUERY_NODE:
	case QueryNodeType::STATEMENT_NODE:
		return true;
	default:
		return false;
	}
}

LogicalPlanSQLExportResult SQLExportDMLHelpers::ExportBody(LogicalPlanSQLExportContext &context, LogicalOperator &op,
                                                           const LogicalPlanVerificationPath &path) {
	if (auto returning = MatchReturningRoot(op)) {
		return ExportReturning(context, *returning, optional<vector<Identifier>>(), path);
	}
	if (IsIdentityProjectionOverDML(op)) {
		auto fields = LogicalPlanSQLExportHelpers::CreateFields(op, path);
		if (fields.HasError()) {
			return LogicalPlanSQLExportResult::Failure(fields);
		}
		LogicalPlanSQLExportContext::AncestorScope projection_scope(context, op);
		auto exported = ExportBody(context, *op.children[0], LogicalPlanSQLExportHelpers::PlanChildPath(path, 0));
		if (exported.HasError()) {
			return exported;
		}
		// the projection only renames the forwarded columns' bindings
		D_ASSERT(exported.GetValue().fields.size() == fields.GetValue().size());
		exported.GetValue().fields = std::move(fields.GetValue());
		return exported;
	}
	if (auto issue = DMLStatementIssue(op, path)) {
		return LogicalPlanSQLExportResult::Failure({std::move(*issue)});
	}
	LogicalPlanSQLExportContext::AncestorScope op_scope(context, op);
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_INSERT:
		return ExportInsertNode(op.Cast<LogicalInsert>(), context, path);
	case LogicalOperatorType::LOGICAL_UPDATE:
		return ExportUpdateNode(op.Cast<LogicalUpdate>(), context, path);
	case LogicalOperatorType::LOGICAL_MERGE_INTO:
		return ExportMergeNode(op.Cast<LogicalMergeInto>(), context, path);
	case LogicalOperatorType::LOGICAL_COPY_TO_FILE:
		return SQLExportStatementHelpers::ExportCopyToNode(op.Cast<LogicalCopyToFile>(), context, path);
	default:
		return ExportDeleteNode(op.Cast<LogicalDelete>(), context, path);
	}
}

optional_ptr<LogicalProjection> SQLExportDMLHelpers::MatchReturningRoot(LogicalOperator &root) {
	if (root.type != LogicalOperatorType::LOGICAL_PROJECTION || root.children.size() != 1) {
		return nullptr;
	}
	auto &child = *root.children[0];
	switch (child.type) {
	case LogicalOperatorType::LOGICAL_INSERT:
		if (!child.Cast<LogicalInsert>().return_chunk) {
			return nullptr;
		}
		break;
	case LogicalOperatorType::LOGICAL_UPDATE:
		if (!child.Cast<LogicalUpdate>().return_chunk) {
			return nullptr;
		}
		break;
	case LogicalOperatorType::LOGICAL_DELETE:
		if (!child.Cast<LogicalDelete>().return_chunk) {
			return nullptr;
		}
		break;
	case LogicalOperatorType::LOGICAL_MERGE_INTO:
		if (!child.Cast<LogicalMergeInto>().return_chunk) {
			return nullptr;
		}
		break;
	default:
		return nullptr;
	}
	return &root.Cast<LogicalProjection>();
}

//! Binding context resolving the modified rows a RETURNING projection references: the table's physical
//! columns bind positionally, and each referenced virtual column (e.g. rowid) is appended behind them
//! carrying the column name it was bound from.
static LogicalPlanVerificationResult<BoundExpressionSQLExportContext>
ReturningBindingContext(ClientContext &client, const LogicalProjection &projection, const TableCatalogEntry &table,
                        TableIndex table_index, const LogicalPlanVerificationPath &path,
                        vector<TableColumn> statement_columns = {}) {
	column_binding_map_t<ResolvedSQLColumnReference> entries;
	idx_t physical_count = 0;
	for (auto &column : table.GetColumns().Physical()) {
		entries.emplace(ColumnBinding(table_index, ProjectionIndex(physical_count++)),
		                ResolvedSQLColumnReference {{column.Name()}, column.Type()});
	}
	for (auto &entry : table.GetVirtualColumns()) {
		statement_columns.push_back(entry.second);
	}
	optional<LogicalPlanVerificationIssue> unsupported;
	for (auto &expression : projection.expressions) {
		ExpressionIterator::VisitExpression<BoundColumnRefExpression>(
		    *expression, [&](const BoundColumnRefExpression &column) {
			    if (unsupported || column.Binding().table_index != table_index ||
			        column.Binding().column_index < physical_count) {
				    return;
			    }
			    for (auto &entry : statement_columns) {
				    if (entry.name != column.GetAlias() || table.ColumnExists(entry.name) ||
				        !entry.type.EqualsIncludingCollation(column.GetReturnType())) {
					    continue;
				    }
				    entries.emplace(column.Binding(), ResolvedSQLColumnReference {{entry.name}, entry.type});
				    return;
			    }
			    unsupported = UnsupportedDML(path, "dml_returning_reference",
			                                 "RETURNING references a column SQL cannot name on the modified table");
		    });
	}
	if (unsupported) {
		return LogicalPlanVerificationResult<BoundExpressionSQLExportContext>::Failure({std::move(*unsupported)});
	}
	BoundExpressionSQLExportContext result;
	result.client_context = &client;
	result.discard_optimizer_metadata = true;
	result.resolve_binding =
	    [entries = std::move(entries)](const ColumnBinding &binding) -> optional<ResolvedSQLColumnReference> {
		auto entry = entries.find(binding);
		if (entry != entries.end()) {
			return entry->second;
		}
		return {};
	};
	return LogicalPlanVerificationResult<BoundExpressionSQLExportContext>::Success(std::move(result));
}

LogicalPlanSQLExportResult SQLExportDMLHelpers::ExportReturning(LogicalPlanSQLExportContext &context,
                                                                LogicalProjection &root,
                                                                const optional<vector<Identifier>> &output_names,
                                                                const LogicalPlanVerificationPath &path) {
	auto fields = LogicalPlanSQLExportHelpers::CreateFields(root, path);
	if (fields.HasError()) {
		return LogicalPlanSQLExportResult::Failure(fields);
	}
	auto &op = *root.children[0];
	auto child_path = LogicalPlanSQLExportHelpers::PlanChildPath(path, 0);
	LogicalPlanSQLExportContext::AncestorScope root_scope(context, root);
	LogicalPlanSQLExportContext::AncestorScope op_scope(context, op);
	optional_ptr<const TableCatalogEntry> table;
	TableIndex table_index;
	vector<TableColumn> statement_columns;
	auto exported = [&]() -> LogicalPlanSQLExportResult {
		switch (op.type) {
		case LogicalOperatorType::LOGICAL_INSERT: {
			auto &insert = op.Cast<LogicalInsert>();
			table = &insert.table;
			table_index = insert.table_index;
			return ExportInsertNode(insert, context, child_path);
		}
		case LogicalOperatorType::LOGICAL_UPDATE: {
			auto &update = op.Cast<LogicalUpdate>();
			if (update.capture_old_rows) {
				return LogicalPlanSQLExportResult::Failure(
				    {UnsupportedDML(child_path, "dml_old_rows", "UPDATE with OLD row capture cannot be exported")});
			}
			table = &update.table;
			table_index = update.table_index;
			return ExportUpdateNode(update, context, child_path);
		}
		case LogicalOperatorType::LOGICAL_MERGE_INTO: {
			auto &merge = op.Cast<LogicalMergeInto>();
			table = &merge.table;
			table_index = merge.table_index;
			// MERGE RETURNING binds the statement-provided merge_action column instead of the table's virtuals
			statement_columns.emplace_back(Identifier("merge_action"), LogicalType::VARCHAR);
			return ExportMergeNode(merge, context, child_path);
		}
		default: {
			auto &del = op.Cast<LogicalDelete>();
			table = &del.table;
			table_index = del.table_index;
			return ExportDeleteNode(del, context, child_path);
		}
		}
	}();
	if (exported.HasError()) {
		return exported;
	}
	auto expression_context = ReturningBindingContext(context.GetClientContext(), root, *table, table_index, path,
	                                                  std::move(statement_columns));
	if (expression_context.HasError()) {
		return LogicalPlanSQLExportResult::Failure(expression_context);
	}
	auto expressions = LogicalPlanSQLExportHelpers::CollectExpressions(root);
	vector<unique_ptr<ParsedExpression>> returning_list;
	for (idx_t i = 0; i < root.expressions.size(); i++) {
		auto expression = context.ExportExpression(root, expressions, i, expression_context.GetValue(), path);
		if (expression.HasError()) {
			return LogicalPlanSQLExportResult::Failure(expression);
		}
		if (output_names && output_names->size() == root.expressions.size()) {
			expression.GetValue()->SetAlias((*output_names)[i]);
		} else if (!root.expressions[i]->GetAlias().empty()) {
			expression.GetValue()->SetAlias(root.expressions[i]->GetAlias());
		}
		returning_list.push_back(std::move(expression.GetValue()));
	}
	auto &query = *exported.GetValue().query;
	switch (query.type) {
	case QueryNodeType::INSERT_QUERY_NODE:
		query.Cast<InsertQueryNode>().returning_list = std::move(returning_list);
		break;
	case QueryNodeType::UPDATE_QUERY_NODE:
		query.Cast<UpdateQueryNode>().returning_list = std::move(returning_list);
		break;
	case QueryNodeType::MERGE_QUERY_NODE:
		query.Cast<MergeQueryNode>().returning_list = std::move(returning_list);
		break;
	default:
		query.Cast<DeleteQueryNode>().returning_list = std::move(returning_list);
		break;
	}
	return LogicalPlanSQLExportResult::Success({std::move(exported.GetValue().query), std::move(fields.GetValue())});
}

} // namespace duckdb
