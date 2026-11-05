#include "duckdb/catalog/catalog_entry/index_catalog_entry.hpp"

#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"

namespace duckdb {

IndexCatalogEntry::IndexCatalogEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateIndexInfo &info)
    : StandardEntry(CatalogType::INDEX_ENTRY, schema, catalog, info.GetIndexName()), sql(info.sql),
      options(info.options), index_type(info.index_type), index_constraint_type(info.constraint_type),
      column_ids(info.column_ids) {
	this->temporary = info.temporary;
	this->dependencies = info.dependencies;
	this->comment = info.comment;
	this->tags = info.tags;
	for (auto &expr : expressions) {
		D_ASSERT(expr);
		expressions.push_back(expr->Copy());
	}
	for (auto &parsed_expr : info.parsed_expressions) {
		D_ASSERT(parsed_expr);
		parsed_expressions.push_back(parsed_expr->Copy());
	}
}

unique_ptr<CreateInfo> IndexCatalogEntry::GetInfo() const {
	auto result = make_uniq<CreateIndexInfo>();
	result->SetQualifiedName(schema.GetQualifiedName(name));
	result->table = GetTableName();

	result->temporary = temporary;
	result->sql = sql;
	result->index_type = index_type;
	result->constraint_type = index_constraint_type;
	result->column_ids = column_ids;
	result->dependencies = dependencies;

	for (auto &expr : expressions) {
		result->expressions.push_back(expr->Copy());
	}
	for (auto &expr : parsed_expressions) {
		result->parsed_expressions.push_back(expr->Copy());
	}

	result->comment = comment;
	result->tags = tags;
	result->options = options;

	return std::move(result);
}

string IndexCatalogEntry::ToSQL() const {
	auto info = GetInfo();
	info->StripCatalogQualification();
	return info->ToString();
}

bool IndexCatalogEntry::IsUnique() const {
	return (index_constraint_type == IndexConstraintType::UNIQUE ||
	        index_constraint_type == IndexConstraintType::PRIMARY);
}

bool IndexCatalogEntry::IsPrimary() const {
	return (index_constraint_type == IndexConstraintType::PRIMARY);
}

} // namespace duckdb
