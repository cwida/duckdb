#include "duckdb/catalog/catalog.hpp"
#include "duckdb/parser/parsed_data/create_macro_info.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/parser/statement/copy_database_statement.hpp"
#include "duckdb/catalog/catalog_entry/list.hpp"
#include "duckdb/parser/parsed_data/create_macro_info.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/planner/operator/logical_copy_database.hpp"
#include "duckdb/execution/operator/persistent/physical_export.hpp"
#include "duckdb/planner/operator/logical_create_table.hpp"
#include "duckdb/planner/operator/logical_set_operation.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/statement/insert_statement.hpp"
#include "duckdb/parser/query_node/insert_query_node.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/operator/logical_dummy_scan.hpp"
#include "duckdb/planner/operator/logical_expression_get.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/catalog/dependency_manager.hpp"

namespace duckdb {

unique_ptr<LogicalOperator> Binder::BindCopyDatabaseSchema(Catalog &from_database,
                                                           const Identifier &target_database_name) {
	catalog_entry_vector_t catalog_entries;
	catalog_entries = PhysicalExport::GetNaiveExportOrder(context, from_database);

	auto info = make_uniq<CopyDatabaseInfo>(target_database_name);
	auto &target_catalog = Catalog::GetCatalog(context, target_database_name);
	for (auto &entry : catalog_entries) {
		auto create_info = entry.get().GetInfo();
		// re-root the entry (keeping its possibly nested schema path) in the target database
		create_info->SetQualifiedName(create_info->GetQualifiedName().WithCatalog(target_database_name));
		if (create_info->type == CatalogType::MACRO_ENTRY || create_info->type == CatalogType::TABLE_MACRO_ENTRY) {
			// this path bypasses the CREATE binder, enforce the storage-version gate here
			CheckMacroStorageVersion(target_catalog, create_info->Cast<CreateMacroInfo>());
		}
		auto on_conflict = create_info->type == CatalogType::SCHEMA_ENTRY ? OnCreateConflict::IGNORE_ON_CONFLICT
		                                                                  : OnCreateConflict::ERROR_ON_CONFLICT;
		// Update all the dependencies of the entry to point to the newly created entries on the target database
		LogicalDependencyList altered_dependencies;
		for (auto &dep : create_info->dependencies.Set()) {
			auto altered_dep = dep;
			altered_dep.catalog = target_database_name;
			altered_dependencies.AddDependency(altered_dep);
		}
		create_info->dependencies = altered_dependencies;
		create_info->on_conflict = on_conflict;
		info->entries.push_back(std::move(create_info));
	}

	return make_uniq<LogicalCopyDatabase>(std::move(info));
}

unique_ptr<LogicalOperator> Binder::BindCopyDatabaseData(Catalog &source_catalog,
                                                         const Identifier &target_database_name) {
	auto entries = PhysicalExport::GetNaiveExportOrder(context, source_catalog);

	vector<unique_ptr<LogicalOperator>> insert_nodes;
	for (auto &table_ref : entries) {
		if (table_ref.get().type != CatalogType::TABLE_ENTRY) {
			continue;
		}
		auto &table = table_ref.get().Cast<TableCatalogEntry>();
		// generate the insert statement
		InsertStatement insert_stmt;
		auto &insert_node = *insert_stmt.node;
		// the table can live in a nested schema - carry the full schema path on both sides
		auto source_name = table.ParentSchema().GetQualifiedName(table.name);
		insert_node.qualified_name = source_name.WithCatalog(target_database_name);

		auto from_tbl = make_uniq<BaseTableRef>();
		from_tbl->SetQualifiedName(source_name.WithCatalog(source_catalog.GetName()));

		auto select_node = make_uniq<SelectNode>();
		auto &select_list = select_node->select_list;
		for (auto &col : table.GetColumns().Physical()) {
			select_list.push_back(make_uniq<ColumnRefExpression>(col.Name(), table.name));
		}

		select_node->from_table = std::move(from_tbl);

		auto select_stmt = make_uniq<SelectStatement>();
		select_stmt->node = std::move(select_node);

		insert_node.select_statement = std::move(select_stmt);
		auto bound_insert = Bind(insert_stmt);
		auto insert_plan = std::move(bound_insert.plan);
		insert_nodes.push_back(std::move(insert_plan));
	}
	unique_ptr<LogicalOperator> result;
	if (insert_nodes.empty()) {
		vector<LogicalType> result_types;
		result_types.push_back(LogicalType::BIGINT);
		vector<unique_ptr<Expression>> expression_list;
		expression_list.push_back(make_uniq<BoundConstantExpression>(Value::BIGINT(0)));
		vector<vector<unique_ptr<Expression>>> expressions;
		expressions.push_back(std::move(expression_list));
		result = make_uniq<LogicalExpressionGet>(GenerateTableIndex(), std::move(result_types), std::move(expressions));
		result->children.push_back(make_uniq<LogicalDummyScan>(GenerateTableIndex()));
	} else {
		// use UNION ALL to combine the individual copy statements into a single node
		result = UnionOperators(std::move(insert_nodes));
	}
	return result;
}

BoundStatement Binder::Bind(CopyDatabaseStatement &stmt) {
	BoundStatement result;

	unique_ptr<LogicalOperator> plan;
	auto &source_catalog = Catalog::GetCatalog(context, stmt.from_database);
	auto &target_catalog = Catalog::GetCatalog(context, stmt.to_database);
	if (&source_catalog == &target_catalog) {
		throw BinderException("Cannot copy from %s to %s - FROM and TO databases are the same", stmt.from_database,
		                      stmt.to_database);
	}
	if (target_catalog.IsSystemCatalog()) {
		throw BinderException("Cannot copy to the system catalog");
	}
	if (stmt.copy_type == CopyDatabaseType::COPY_SCHEMA) {
		result.types = {LogicalType::BOOLEAN};
		result.names = {"Success"};

		plan = BindCopyDatabaseSchema(source_catalog, target_catalog.GetName());
	} else {
		result.types = {LogicalType::BIGINT};
		result.names = {"Count"};

		plan = BindCopyDatabaseData(source_catalog, target_catalog.GetName());
	}

	result.plan = std::move(plan);

	auto &properties = GetStatementProperties();
	properties.return_type = StatementReturnType::NOTHING;

	DatabaseModificationType modification;
	modification |= DatabaseModificationType::INSERT_DATA;
	modification |= DatabaseModificationType::CREATE_CATALOG_ENTRY;
	properties.RegisterDBModify(target_catalog, context, modification);
	return result;
}

} // namespace duckdb
