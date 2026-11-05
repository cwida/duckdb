#include "duckdb/catalog/catalog_search_path.hpp"
#include "duckdb/catalog/default/default_schemas.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/constants.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/sql_identifier.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/extension_callback_manager.hpp"

#include "duckdb/common/exception/parser_exception.hpp"

namespace duckdb {

CatalogSearchEntry::CatalogSearchEntry(Identifier catalog_p, Identifier schema_p, bool default_schema_precedence_p)
    : catalog(std::move(catalog_p)), schema(std::move(schema_p)),
      default_schema_precedence(default_schema_precedence_p) {
}

CatalogSearchEntry CatalogSearchEntry::FromSchemaPath(Identifier catalog, vector<Identifier> schema_path,
                                                      bool default_schema_precedence) {
	D_ASSERT(!schema_path.empty());
	auto schema = std::move(schema_path.back());
	schema_path.pop_back();
	CatalogSearchEntry result(std::move(catalog), std::move(schema), default_schema_precedence);
	result.parent_schemas = std::move(schema_path);
	return result;
}

vector<Identifier> CatalogSearchEntry::GetSchemaPath() const {
	auto result = parent_schemas;
	result.push_back(schema);
	return result;
}

string CatalogSearchEntry::ToString() const {
	string result;
	if (!catalog.empty()) {
		result = WriteOptionallyQuoted(catalog) + ".";
	}
	for (auto &parent : parent_schemas) {
		result += WriteOptionallyQuoted(parent) + ".";
	}
	return result + WriteOptionallyQuoted(schema);
}

string CatalogSearchEntry::WriteOptionallyQuoted(const Identifier &input) {
	auto &name = input.GetIdentifierName();
	for (idx_t i = 0; i < name.size(); i++) {
		if (name[i] == '.' || name[i] == ',' || name[i] == '"') {
			return SQLQuotedIdentifier::ToString(name);
		}
	}
	return name;
}

string CatalogSearchEntry::ListToString(const vector<CatalogSearchEntry> &input) {
	string result;
	for (auto &entry : input) {
		if (!result.empty()) {
			result += ",";
		}
		result += entry.ToString();
	}
	return result;
}

CatalogSearchEntry CatalogSearchEntry::ParseInternal(const string &input, idx_t &idx) {
	string catalog;
	string schema;
	string entry;
	bool finished = false;
normal:
	for (; idx < input.size();) {
		if (input[idx] == '"') {
			string quoted;
			if (!StringUtil::TryParseQuotedString(input, idx, quoted)) {
				throw ParserException("Unterminated quote in qualified name!");
			}
			entry += quoted;
		} else if (input[idx] == '.') {
			goto separator;
		} else if (input[idx] == ',') {
			finished = true;
			goto separator;
		} else {
			entry += input[idx++];
		}
	}
	finished = true;
	goto separator;
separator:
	if (entry.empty()) {
		throw ParserException("Unexpected dot - empty CatalogSearchEntry");
	}
	if (schema.empty()) {
		// if we parse one entry it is the schema
		schema = std::move(entry);
	} else if (catalog.empty()) {
		// if we parse two entries it is [catalog.schema]
		catalog = std::move(schema);
		schema = std::move(entry);
	} else {
		throw ParserException("Too many dots - expected [schema] or [catalog.schema] for CatalogSearchEntry");
	}
	entry = "";
	idx++;
	if (finished) {
		goto final;
	}
	goto normal;
final:
	if (schema.empty()) {
		throw ParserException("Unexpected end of entry - empty CatalogSearchEntry");
	}
	return CatalogSearchEntry(Identifier(std::move(catalog)), Identifier(std::move(schema)));
}

CatalogSearchEntry CatalogSearchEntry::Parse(const string &input) {
	idx_t pos = 0;
	auto result = ParseInternal(input, pos);
	if (pos < input.size()) {
		throw ParserException("Failed to convert entry \"%s\" to CatalogSearchEntry - expected a single entry", input);
	}
	return result;
}

vector<CatalogSearchEntry> CatalogSearchEntry::ParseList(const string &input) {
	idx_t pos = 0;
	vector<CatalogSearchEntry> result;
	while (pos < input.size()) {
		auto entry = ParseInternal(input, pos);
		result.push_back(entry);
	}
	return result;
}

CatalogSearchPath::CatalogSearchPath(ClientContext &context_p, vector<CatalogSearchEntry> entries)
    : context(context_p) {
	SetPathsInternal(std::move(entries));
}

CatalogSearchPath::CatalogSearchPath(ClientContext &context_p) : CatalogSearchPath(context_p, {}) {
}

void CatalogSearchPath::Reset() {
	vector<CatalogSearchEntry> empty;
	SetPathsInternal(empty);
}

void CatalogSearchPath::RefreshSetPaths() {
	SetPathsInternal(set_paths);
}

string CatalogSearchPath::GetSetName(CatalogSetPathType set_type) {
	switch (set_type) {
	case CatalogSetPathType::SET_SCHEMA:
		return "SET schema";
	case CatalogSetPathType::SET_SCHEMAS:
		return "SET search_path";
	default:
		throw InternalException("Unrecognized CatalogSetPathType");
	}
}

void CatalogSearchPath::Set(vector<CatalogSearchEntry> new_paths, CatalogSetPathType set_type) {
	if (set_type == CatalogSetPathType::SET_SCHEMA && new_paths.size() != 1) {
		throw CatalogException("%s can set only 1 schema. This has %d", GetSetName(set_type), new_paths.size());
	}
	for (auto &path : new_paths) {
		if (set_type == CatalogSetPathType::SET_DIRECTLY) {
			if (path.GetCatalog().empty() || path.GetSchema().empty()) {
				throw InternalException("SET_WITHOUT_VERIFICATION requires a fully qualified set path");
			}
			continue;
		}
		auto schema_entry =
		    Catalog::GetSchema(context, path.GetCatalog(), path.GetSchema(), OnEntryNotFound::RETURN_NULL);
		if (schema_entry) {
			// we are setting a schema - update the catalog and schema
			if (path.GetCatalog().empty()) {
				path.SetCatalog(GetDefault().GetCatalog());
			}
			continue;
		}
		// only schema supplied - check if this is a catalog instead
		if (path.GetCatalog().empty()) {
			auto catalog = Catalog::GetCatalogEntry(context, path.GetSchema());
			if (catalog) {
				auto default_schema = catalog->GetDefaultSchema();
				if (default_schema) {
					auto schema = catalog->GetSchema(context, *default_schema, OnEntryNotFound::RETURN_NULL);
					if (schema) {
						path.SetCatalog(path.GetSchema());
						path.SetSchema(schema->name);
						continue;
					}
				}
			}
		}
		if (!path.GetCatalog().empty()) {
			// "a.b" can also name a nested schema - give a clearer error in that case
			vector<Identifier> nested_path {path.GetCatalog(), path.GetSchema()};
			if (Catalog::GetSchema(context, Identifier(), nested_path, OnEntryNotFound::RETURN_NULL)) {
				throw NotImplementedException("%s: \"%s\" is a nested schema - nested schemas cannot be used in the "
				                              "search path",
				                              GetSetName(set_type), path.ToString());
			}
		}
		throw CatalogException("%s: No catalog + schema named \"%s\" found.", GetSetName(set_type), path.ToString());
	}
	if (set_type == CatalogSetPathType::SET_SCHEMA) {
		if (new_paths[0].GetCatalog() == TEMP_CATALOG || new_paths[0].GetCatalog() == SYSTEM_CATALOG) {
			throw CatalogException("%s cannot be set to internal schema %s", GetSetName(set_type),
			                       new_paths[0].GetCatalog());
		}
	}
	SetPathsInternal(std::move(new_paths));
}

void CatalogSearchPath::Set(CatalogSearchEntry new_value, CatalogSetPathType set_type) {
	vector<CatalogSearchEntry> new_paths {std::move(new_value)};
	Set(std::move(new_paths), set_type);
}

vector<CatalogSearchEntry> CatalogSearchPath::Get() const {
	vector<CatalogSearchEntry> res;
	for (auto &path : paths) {
		if (path.GetSchema().empty()) {
			continue;
		}
		res.emplace_back(path);
	}
	return res;
}

Identifier CatalogSearchPath::GetDefaultSchema(const Identifier &catalog) const {
	for (auto &path : paths) {
		if (path.GetCatalog() == TEMP_CATALOG || path.IsNested()) {
			continue;
		}
		if (path.GetCatalog() == catalog) {
			return path.GetSchema();
		}
	}
	return DEFAULT_SCHEMA;
}

optional<Identifier> CatalogSearchPath::GetDefaultSchema(ClientContext &context, const Identifier &catalog) const {
	for (auto &path : paths) {
		if (path.GetCatalog() == TEMP_CATALOG || path.IsNested()) {
			continue;
		}
		if (path.GetCatalog() == catalog) {
			return path.GetSchema();
		}
	}
	auto catalog_entry = Catalog::GetCatalogEntry(context, catalog);
	if (catalog_entry) {
		return catalog_entry->GetDefaultSchema();
	}
	return Identifier(DEFAULT_SCHEMA);
}

Identifier CatalogSearchPath::GetDefaultCatalog(const Identifier &schema) const {
	if (DefaultSchemaGenerator::IsDefaultSchema(schema)) {
		return Identifier::SystemCatalog();
	}
	for (auto &path : paths) {
		if (path.GetCatalog() == TEMP_CATALOG || path.IsNested()) {
			continue;
		}
		if (path.GetSchema() == schema) {
			return path.GetCatalog();
		}
	}
	return Identifier::InvalidCatalog();
}

Identifier CatalogSearchPath::ResolveCatalog(const Identifier &schema) const {
	auto catalog = schema.empty() ? Identifier::InvalidCatalog() : GetDefaultCatalog(schema);
	if (IsInvalidCatalog(catalog)) {
		catalog = GetDefault().GetCatalog();
	}
	if (IsInvalidCatalog(catalog)) {
		catalog = DatabaseManager::GetDefaultDatabase(context);
	}
	return catalog;
}

vector<Identifier> CatalogSearchPath::GetCatalogsForSchema(const Identifier &schema) const {
	vector<Identifier> catalogs;
	if (DefaultSchemaGenerator::IsDefaultSchema(schema)) {
		catalogs.push_back(Identifier::SystemCatalog());
	} else {
		for (auto &path : paths) {
			if (path.IsNested()) {
				continue;
			}
			if (path.GetSchema() == schema || path.GetSchema().empty()) {
				catalogs.emplace_back(path.GetCatalog());
			}
		}
	}
	return catalogs;
}

vector<Identifier> CatalogSearchPath::GetSchemasForCatalog(const Identifier &catalog) const {
	vector<Identifier> schemas;
	for (auto &path : paths) {
		if (!path.GetSchema().empty() && !path.IsNested() && path.GetCatalog() == catalog) {
			schemas.emplace_back(path.GetSchema());
		}
	}
	return schemas;
}

vector<CatalogSearchEntry> CatalogSearchPath::GetEntriesForCatalog(const Identifier &catalog) const {
	vector<CatalogSearchEntry> entries;
	for (auto &path : paths) {
		if (!path.GetSchema().empty() && path.GetCatalog() == catalog) {
			entries.push_back(path);
		}
	}
	return entries;
}

vector<CatalogSearchEntry> CatalogSearchPath::GetImplicitSearchCatalogs() const {
	// Get the implicit entries that were not already resolved by the precedence flag.
	vector<CatalogSearchEntry> catalogs;
	for (auto &path : paths) {
		if (path.GetSchema().empty() && !path.GetCatalog().empty() && !path.DefaultSchemaPrecedence()) {
			catalogs.push_back(path);
		}
	}
	return catalogs;
}

vector<CatalogSearchEntry> CatalogSearchPath::GetWithPrecedenceSchemas(ClientContext &context) const {
	vector<CatalogSearchEntry> res;
	for (auto &path : paths) {
		// Resolve implicit entries with default_schema_precedence to the catalog's default schema.
		if (path.GetSchema().empty()) {
			if (path.GetCatalog().empty() || !path.DefaultSchemaPrecedence()) {
				continue;
			}
			auto catalog_entry = Catalog::GetCatalogEntry(context, path.GetCatalog());
			if (!catalog_entry) {
				continue;
			}
			auto default_schema = catalog_entry->GetDefaultSchema();
			if (!default_schema) {
				continue;
			}
			res.emplace_back(path.GetCatalog(), *default_schema);
		} else {
			res.emplace_back(path);
		}
	}
	return res;
}

const CatalogSearchEntry &CatalogSearchPath::GetDefault() const {
	D_ASSERT(paths.size() >= 2);
	D_ASSERT(!paths[1].GetSchema().empty());
	return paths[1];
}

void CatalogSearchPath::SetPathsInternal(vector<CatalogSearchEntry> new_paths) {
	this->set_paths = std::move(new_paths);

	paths.clear();
	paths.reserve(set_paths.size() + 4);
	paths.emplace_back(TEMP_CATALOG, DEFAULT_SCHEMA);
	for (auto &path : set_paths) {
		paths.push_back(path);
	}
	paths.emplace_back(INVALID_CATALOG, DEFAULT_SCHEMA);
	paths.emplace_back(SYSTEM_CATALOG, DEFAULT_SCHEMA);
	paths.emplace_back(SYSTEM_CATALOG, "pg_catalog");
	// set extension schemas on the search path, if any
	for (auto &schema : ExtensionCallbackManager::Get(context).GetExtensionSchemas()) {
		paths.emplace_back(Identifier(SYSTEM_CATALOG), Identifier(schema));
	}
}

bool CatalogSearchPath::SchemaInSearchPath(ClientContext &context, const Identifier &catalog_name,
                                           const Identifier &schema_name) const {
	for (auto &path : paths) {
		if (path.GetSchema() != schema_name || path.IsNested()) {
			continue;
		}
		if (path.GetCatalog() == catalog_name) {
			return true;
		}
		if (IsInvalidCatalog(path.GetCatalog()) && catalog_name == DatabaseManager::TryGetDefaultDatabase(context)) {
			return true;
		}
	}
	return false;
}

} // namespace duckdb
