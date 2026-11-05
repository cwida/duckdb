#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/common/exception/catalog_exception.hpp"

namespace duckdb {

CreateSchemaInfo::CreateSchemaInfo() : CreateInfo(CatalogType::SCHEMA_ENTRY) {
}

const Identifier &CreateSchemaInfo::SchemaName() const {
	// the new schema is stored in the Schema() slot (the element before the empty trailing name)
	return GetQualifiedName().Schema();
}

const Identifier &CreateSchemaInfo::SchemaCatalog() const {
	// the catalog is the leading component once the path carries [catalog, schema, <empty name>]
	return GetQualifiedName().Catalog();
}

vector<Identifier> CreateSchemaInfo::ParentSchemas() const {
	auto &path = GetQualifiedName().Path();
	vector<Identifier> result;
	// everything between the catalog and the new schema is a parent schema (the last two slots are
	// the new schema and the empty trailing name)
	for (idx_t i = 1; i + 2 < path.size(); i++) {
		result.push_back(path[i]);
	}
	return result;
}

bool CreateSchemaInfo::IsNested() const {
	return GetQualifiedName().Path().size() > 3;
}

bool CreateSchemaInfo::ShouldReplaceOnConflict() const {
	switch (on_conflict) {
	case OnCreateConflict::ERROR_ON_CONFLICT:
		throw CatalogException::EntryAlreadyExists(CatalogType::SCHEMA_ENTRY, SchemaName());
	case OnCreateConflict::IGNORE_ON_CONFLICT:
		return false;
	case OnCreateConflict::REPLACE_ON_CONFLICT:
		return true;
	default:
		throw InternalException("Unsupported OnCreateConflict for CreateSchema");
	}
}

unique_ptr<CreateInfo> CreateSchemaInfo::Copy() const {
	auto result = make_uniq<CreateSchemaInfo>();
	CopyProperties(*result);
	for (auto &option : options) {
		result->options.emplace(option.first, option.second->Copy());
	}
	return std::move(result);
}

string CreateSchemaInfo::ToString() const {
	auto qualified = GetQualifiedName().Parent().ToString();

	string temp = temporary ? "TEMPORARY " : "";
	if (!options.empty()) {
		qualified += " WITH (";
		idx_t i = 0;
		for (auto &entry : options) {
			if (i > 0) {
				qualified += ", ";
			}
			qualified += SQLString(entry.first) + "=" + entry.second->ToString();
			i++;
		}
		qualified += ")";
	}

	string ret = "";
	switch (on_conflict) {
	case OnCreateConflict::ALTER_ON_CONFLICT: {
		ret += "CREATE " + temp + "SCHEMA " + qualified + " ON CONFLICT INSERT OR REPLACE;";
		break;
	}
	case OnCreateConflict::IGNORE_ON_CONFLICT: {
		ret += "CREATE " + temp + "SCHEMA IF NOT EXISTS " + qualified + ";";
		break;
	}
	case OnCreateConflict::REPLACE_ON_CONFLICT: {
		ret += "CREATE OR REPLACE " + temp + "SCHEMA " + qualified + ";";
		break;
	}
	case OnCreateConflict::ERROR_ON_CONFLICT: {
		ret += "CREATE " + temp + "SCHEMA " + qualified + ";";
		break;
	}
	}
	return ret;
}

} // namespace duckdb
