#include "duckdb/planner/sql_export/logical_plan_sql_exporter_internal.hpp"
#include "duckdb/planner/logical_plan_sql_exporter.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/planner/bound_expression_sql_exporter.hpp"
#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/planner/logical_operator_visitor.hpp"
#include "duckdb/planner/sql_export_helpers.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"

namespace duckdb {

LogicalPlanVerificationPath LogicalPlanSQLExportHelpers::PlanChildPath(const LogicalPlanVerificationPath &path,
                                                                       idx_t ordinal) {
	return SQLExportHelpers::ChildPath(path, ordinal, LogicalPlanVerificationPathComponentType::OPERATOR_CHILD);
}

LogicalPlanVerificationPath LogicalPlanSQLExportHelpers::PlanExpressionPath(const LogicalPlanVerificationPath &path,
                                                                            idx_t ordinal) {
	return SQLExportHelpers::ChildPath(path, ordinal, LogicalPlanVerificationPathComponentType::OPERATOR_EXPRESSION);
}

LogicalPlanVerificationResult<unique_ptr<ParsedExpression>>
LogicalPlanSQLExportHelpers::ExportTypedNull(const LogicalType &type, const LogicalPlanVerificationPath &path) {
	auto result = BoundExpressionSQLExporter::Export(BoundConstantExpression(Value(type)), {});
	if (!result.HasError()) {
		return result;
	}
	auto issues = result.GetIssues();
	for (auto &issue : issues) {
		issue.path = path;
		issue.phase = LogicalPlanVerificationPhase::PLAN_EXPORT;
	}
	return LogicalPlanVerificationResult<unique_ptr<ParsedExpression>>::Failure(std::move(issues));
}

LogicalPlanVerificationIssue
LogicalPlanSQLExportHelpers::PlanUnsupportedFeature(const LogicalPlanVerificationPath &path, string feature,
                                                    string message) {
	return SQLExportHelpers::MakeIssue(
	    LogicalPlanVerificationIssueCode::UNSUPPORTED_EXPORT_FEATURE, LogicalPlanVerificationPhase::PLAN_EXPORT, path,
	    LogicalPlanVerificationConstructIdentity::ExportFeature(std::move(feature)), std::move(message));
}

LogicalPlanVerificationFunctionIdentity LogicalPlanSQLExportHelpers::LogicalSourceIdentity() {
	LogicalPlanVerificationFunctionIdentity source;
	source.name = "logical_source";
	source.return_type = LogicalType::TABLE;
	return source;
}

LogicalPlanVerificationFunctionIdentity LogicalPlanSQLExportHelpers::LogicalSourceIdentity(const LogicalGet &get) {
	LogicalPlanVerificationFunctionIdentity source;
	source.catalog = get.function.GetCatalogName().GetIdentifierName();
	source.schema = get.function.GetSchemaName().GetIdentifierName();
	source.name = get.function.GetName().GetIdentifierName();
	for (auto &parameter : get.parameters) {
		source.arguments.push_back(parameter.type());
	}
	for (auto &parameter : get.named_parameters) {
		source.arguments.push_back(parameter.second.type());
	}
	source.return_type = LogicalType::TABLE;
	return source;
}

LogicalPlanVerificationIssue
LogicalPlanSQLExportHelpers::UnsupportedSource(const LogicalPlanVerificationPath &path,
                                               LogicalPlanVerificationFunctionIdentity source, string guard) {
	auto issue = SQLExportHelpers::MakeIssue(
	    LogicalPlanVerificationIssueCode::UNSUPPORTED_SOURCE, LogicalPlanVerificationPhase::PLAN_EXPORT, path,
	    LogicalPlanVerificationConstructIdentity::SourceFunction(std::move(source)),
	    "The logical source does not expose structural SQL export semantics");
	issue.facts.emplace_back("guard", Value(std::move(guard)));
	return issue;
}

Identifier LogicalPlanSQLExportHelpers::FieldIdentifier(idx_t ordinal) {
	return Identifier("c" + to_string(ordinal));
}

LogicalPlanSQLFieldResult LogicalPlanSQLExportHelpers::CreateFields(LogicalOperator &op,
                                                                    const LogicalPlanVerificationPath &path) {
	auto bindings = op.GetColumnBindings();
	D_ASSERT(bindings.size() == op.types.size());
	vector<LogicalPlanSQLExportField> fields;
	for (idx_t i = 0; i < bindings.size(); i++) {
		D_ASSERT(bindings[i].table_index.IsValid() && bindings[i].column_index.IsValid());
		if (!SQLExportHelpers::IsSQLValueType(op.types[i])) {
			auto issue = LogicalPlanSQLExportHelpers::PlanUnsupportedFeature(
			    path, "output_type", "Output type cannot be represented in SQL");
			issue.facts.emplace_back("column_index", Value::UBIGINT(i));
			issue.facts.emplace_back("logical_type", Value(op.types[i].ToString()));
			issue.facts.emplace_back("varchar_collations",
			                         Value(SQLExportHelpers::TypeCollationSignature(op.types[i])));
			return LogicalPlanSQLFieldResult::Failure({std::move(issue)});
		}
		fields.push_back({bindings[i], op.types[i]});
	}
	return LogicalPlanSQLFieldResult::Success(std::move(fields));
}

BoundExpressionSQLExportContext
LogicalPlanSQLExportHelpers::CreateBindingContext(ClientContext &context,
                                                  const vector<reference<const LogicalPlanSQLExportedChild>> &children,
                                                  const vector<optional_ptr<const SelectNode>> &plain_scopes) {
	D_ASSERT(plain_scopes.empty() || plain_scopes.size() == children.size());
	column_binding_map_t<ResolvedSQLColumnReference> entries;
	for (idx_t child_index = 0; child_index < children.size(); child_index++) {
		auto &child = children[child_index].get();
		auto plain = plain_scopes.empty() ? nullptr : plain_scopes[child_index];
		for (idx_t i = 0; i < child.relation.fields.size(); i++) {
			auto &field = child.relation.fields[i];
			auto names =
			    plain ? plain->select_list[i]->Cast<ColumnRefExpression>().ColumnNames()
			          : vector<Identifier> {child.relation_alias, LogicalPlanSQLExportHelpers::FieldIdentifier(i)};
			entries.emplace(field.source_binding,
			                ResolvedSQLColumnReference {std::move(names), field.type, field.optimizer_type});
		}
	}
	BoundExpressionSQLExportContext result;
	result.client_context = &context;
	result.discard_optimizer_metadata = true;
	result.resolve_binding =
	    [entries = std::move(entries)](const ColumnBinding &binding) -> optional<ResolvedSQLColumnReference> {
		auto entry = entries.find(binding);
		if (entry != entries.end()) {
			return entry->second;
		}
		return {};
	};
	return result;
}

void LogicalPlanSQLExportHelpers::PropagateSemanticTypes(
    vector<LogicalPlanSQLExportField> &fields, const vector<reference<const LogicalPlanSQLExportedChild>> &children) {
	for (auto &field : fields) {
		for (auto &child : children) {
			for (auto &child_field : child.get().relation.fields) {
				if (field.source_binding != child_field.source_binding || field.type == child_field.type) {
					continue;
				}
				field.optimizer_type = field.type;
				field.type = child_field.type;
			}
		}
	}
}

unique_ptr<TableRef> LogicalPlanSQLExportHelpers::CreateSubquery(LogicalPlanSQLExportedChild child) {
	auto statement = make_uniq<SelectStatement>();
	statement->node = std::move(child.relation.query);
	auto result = make_uniq<SubqueryRef>(std::move(statement), std::move(child.relation_alias));
	for (idx_t i = 0; i < child.relation.fields.size(); i++) {
		result->column_name_alias.push_back(LogicalPlanSQLExportHelpers::FieldIdentifier(i));
	}
	return std::move(result);
}

unique_ptr<ParsedExpression> LogicalPlanSQLExportHelpers::ChildColumn(const LogicalPlanSQLExportedChild &child,
                                                                      idx_t field_index,
                                                                      optional_ptr<const SelectNode> plain) {
	D_ASSERT(field_index < child.relation.fields.size());
	if (plain) {
		return plain->select_list[field_index]->Copy();
	}
	return make_uniq<ColumnRefExpression>(LogicalPlanSQLExportHelpers::FieldIdentifier(field_index),
	                                      child.relation_alias);
}

vector<reference<const Expression>> LogicalPlanSQLExportHelpers::CollectExpressions(const LogicalOperator &op) {
	vector<reference<const Expression>> expressions;
	LogicalOperatorVisitor::EnumerateExpressions(op, [&](const unique_ptr<Expression> *expression) {
		D_ASSERT(expression && *expression);
		expressions.push_back(reference<const Expression>(**expression));
	});
	return expressions;
}

bool LogicalPlanSQLExportHelpers::HasEffectfulExpressions(const LogicalOperator &op) {
	for (auto &expression : LogicalPlanSQLExportHelpers::CollectExpressions(op)) {
		if (expression.get().IsVolatile() || expression.get().CanThrow()) {
			return true;
		}
	}
	return false;
}

bool LogicalPlanSQLExportHelpers::CollectScopeAliases(const TableRef &table, identifier_set_t &aliases) {
	if (table.sample) {
		return false;
	}
	if (!table.alias.empty()) {
		return aliases.insert(table.alias).second;
	}
	if (table.type != TableReferenceType::JOIN) {
		return false;
	}
	auto &join = table.Cast<JoinRef>();
	return join.left && join.right && LogicalPlanSQLExportHelpers::CollectScopeAliases(*join.left, aliases) &&
	       LogicalPlanSQLExportHelpers::CollectScopeAliases(*join.right, aliases);
}

optional_ptr<const SelectNode> LogicalPlanSQLExportHelpers::PlainScope(const QueryNode &query) {
	if (query.type != QueryNodeType::SELECT_NODE || !query.modifiers.empty() || !query.cte_map.map.empty()) {
		return nullptr;
	}
	auto &select = query.Cast<SelectNode>();
	if (!select.from_table || select.sample || select.from_table->sample) {
		return nullptr;
	}
	const bool has_groups = !select.groups.group_expressions.empty() || !select.groups.grouping_sets.empty();
	const bool has_aggregate_handling = select.aggregate_handling != AggregateHandling::STANDARD_HANDLING;
	if (has_groups || select.having || select.qualify || has_aggregate_handling) {
		return nullptr;
	}
	identifier_set_t aliases;
	if (!LogicalPlanSQLExportHelpers::CollectScopeAliases(*select.from_table, aliases)) {
		return nullptr;
	}
	for (auto &expression : select.select_list) {
		if (expression->GetExpressionClass() != ExpressionClass::COLUMN_REF) {
			return nullptr;
		}
		auto &names = expression->Cast<ColumnRefExpression>().ColumnNames();
		if (names.size() != 2 || !aliases.count(names[0])) {
			return nullptr;
		}
	}
	return select;
}

void LogicalPlanSQLExportHelpers::SetChildScope(SelectNode &select, LogicalPlanSQLExportedChild child,
                                                optional_ptr<const SelectNode> plain) {
	if (!plain) {
		select.from_table = LogicalPlanSQLExportHelpers::CreateSubquery(std::move(child));
		return;
	}
	auto &source = child.relation.query->Cast<SelectNode>();
	select.from_table = std::move(source.from_table);
	select.where_clause = SQLExportHelpers::Conjoin(std::move(source.where_clause), std::move(select.where_clause));
}

bool LogicalPlanSQLExportHelpers::IsIdentityProjection(const LogicalProjection &projection,
                                                       const vector<LogicalPlanSQLExportField> &fields) {
	if (projection.expressions.size() != fields.size()) {
		return false;
	}
	for (idx_t i = 0; i < fields.size(); i++) {
		auto &expression = *projection.expressions[i];
		if (expression.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
			return false;
		}
		auto &column = expression.Cast<BoundColumnRefExpression>();
		if (column.Depth() != 0 || column.Binding() != fields[i].source_binding ||
		    !column.GetReturnType().EqualsIncludingCollation(fields[i].type)) {
			return false;
		}
	}
	return true;
}

} // namespace duckdb
