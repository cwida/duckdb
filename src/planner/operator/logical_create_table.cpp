#include "duckdb/planner/operator/logical_create_table.hpp"

#include "duckdb/planner/binder.hpp"

namespace duckdb {

LogicalCreateTable::LogicalCreateTable(SchemaCatalogEntry &schema, unique_ptr<BoundCreateTableInfo> info)
    : LogicalOperator(LogicalOperatorType::LOGICAL_CREATE_TABLE), schema(schema), info(std::move(info)) {
}

// resolve the (possibly nested) schema [catalog, schema_path..., name] the table will be created in
static SchemaCatalogEntry &ResolveCreateSchema(ClientContext &context, CreateInfo &info) {
	auto &path = info.GetQualifiedName().Path();
	vector<Identifier> schema_path(path.begin() + 1, path.end() - 1);
	return *Catalog::GetSchema(context, path.front(), schema_path, OnEntryNotFound::THROW_EXCEPTION);
}

LogicalCreateTable::LogicalCreateTable(ClientContext &context, unique_ptr<CreateInfo> unbound_info)
    : LogicalOperator(LogicalOperatorType::LOGICAL_CREATE_TABLE), schema(ResolveCreateSchema(context, *unbound_info)) {
	D_ASSERT(unbound_info->type == CatalogType::TABLE_ENTRY);
	// the column types arrive bound, so binding them looks up no user types: keep the dependencies on those types
	vector<LogicalDependency> type_dependencies;
	for (auto &dependency : unbound_info->dependencies.Set()) {
		if (dependency.entry.type == CatalogType::TYPE_ENTRY) {
			type_dependencies.push_back(dependency);
		}
	}
	auto binder = Binder::CreateBinder(context);
	info = binder->BindCreateTableInfo(unique_ptr_cast<CreateInfo, CreateTableInfo>(std::move(unbound_info)));
	for (auto &dependency : type_dependencies) {
		info->Base().dependencies.AddDependency(dependency);
	}
}

idx_t LogicalCreateTable::EstimateCardinality(ClientContext &context) {
	return 1;
}

void LogicalCreateTable::ResolveTypes() {
	types.emplace_back(LogicalType::BIGINT);
}

} // namespace duckdb
