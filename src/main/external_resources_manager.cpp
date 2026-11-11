#include "duckdb/main/external_resources_manager.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/common/enums/on_entry_not_found.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/logging/log_type.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parser/qualified_name.hpp"

namespace duckdb {

//! Callbacks can call external resource functions themselves - bound the nesting before it exhausts the stack
static constexpr idx_t MAX_EXTERNAL_RESOURCE_CALLBACK_DEPTH = 16;

unique_ptr<Connection> CreateExternalResourceCallbackConnection(DatabaseInstance &db,
                                                                optional_ptr<ClientContext> caller) {
	if (!caller) {
		return make_uniq<Connection>(Connection::CreateInternal(db));
	}
	if (caller->internal_depth + 1 > MAX_EXTERNAL_RESOURCE_CALLBACK_DEPTH) {
		throw InvalidInputException("External resource callbacks are nested too deeply (maximum depth %llu) - does a "
		                            "callback call itself recursively?",
		                            MAX_EXTERNAL_RESOURCE_CALLBACK_DEPTH);
	}
	return make_uniq<Connection>(Connection::CreateInternal(*caller));
}

ResourceDeleter::ResourceDeleter(DatabaseInstance &db, string deleter_function_p, Value deleter_payload_p,
                                 string resource_type_p, string resource_name_p)
    : db(db), deleter_function(std::move(deleter_function_p)), deleter_payload(std::move(deleter_payload_p)),
      resource_type(std::move(resource_type_p)), resource_name(std::move(resource_name_p)) {
}

string ResourceDeleter::DeleteSQL() const {
	if (deleter_function.empty()) {
		return string();
	}
	// Render the (possibly schema-qualified) function name with each component properly quoted, so a
	// name needing quoting is valid SQL and a registry-supplied string cannot inject SQL.
	auto function_name = QualifiedName::Parse(deleter_function).ToString();
	return "SELECT * FROM " + function_name + "(" + deleter_payload.ToSQLString() + ")";
}

void ResourceDeleter::Delete() {
	auto sql = DeleteSQL();
	if (sql.empty()) {
		return;
	}
	// On a separate internal connection, since the current connection's context lock is held.
	auto con = CreateExternalResourceCallbackConnection(db, nullptr);
	auto result = con->Query(sql);
	DUCKDB_LOG(db, ExternalResourceLogType, resource_type, resource_name, string("destroy"),
	           result->HasError() ? result->GetError() : string(),
	           Value::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR, vector<Value>(), vector<Value>()));
	if (result->HasError()) {
		// A failed teardown is a leak, so fail loudly and say how to retry it manually.
		throw IOException("external resource teardown failed: %s. The resource was NOT torn down; run `%s;` "
		                  "to retry the teardown",
		                  result->GetError(), sql);
	}
}

void ResourceDeleter::TryDelete() {
	try {
		Delete();
	} catch (std::exception &ex) {
		DUCKDB_LOG_WARNING(db, ErrorData(ex).Message());
	}
}

string QualifyTableCallback(ClientContext &context, const string &name) {
	EntryLookupInfo table_fn(CatalogType::TABLE_FUNCTION_ENTRY, QualifiedName::Parse(name));
	auto entry = Catalog::GetEntry(context, table_fn, OnEntryNotFound::RETURN_NULL);
	if (!entry) {
		EntryLookupInfo table_macro(CatalogType::TABLE_MACRO_ENTRY, QualifiedName::Parse(name));
		entry = Catalog::GetEntry(context, table_macro, OnEntryNotFound::RETURN_NULL);
	}
	if (!entry) {
		return string();
	}
	return QualifiedName(entry->ParentCatalog().GetName(), entry->ParentSchema().name, entry->name).ToString();
}

ExternalResourcesManager &ExternalResourcesManager::Get(DatabaseInstance &db) {
	return db.GetExternalResourcesManager();
}

ExternalResourcesManager &ExternalResourcesManager::Get(ClientContext &context) {
	return ExternalResourcesManager::Get(*context.db);
}

void ExternalResourcesManager::Add(ExternalResource instance) {
	if (instance.name.empty()) {
		throw InvalidInputException("an external resource instance must have a name");
	}
	if (instance.type.empty()) {
		throw InvalidInputException("external resource \"%s\": a type is required", instance.name);
	}
	lock_guard<mutex> guard(lock);
	for (auto &existing : instances) {
		if (existing.name == instance.name) {
			throw InvalidInputException("external resource \"%s\" is already registered", instance.name);
		}
	}
	instances.push_back(std::move(instance));
}

unique_ptr<ExternalResource> ExternalResourcesManager::Remove(const string &name) {
	lock_guard<mutex> guard(lock);
	for (idx_t i = 0; i < instances.size(); i++) {
		if (instances[i].name == name) {
			auto result = make_uniq<ExternalResource>(std::move(instances[i]));
			instances.erase(instances.begin() + static_cast<int64_t>(i));
			return result;
		}
	}
	return nullptr;
}

unique_ptr<ExternalResource> ExternalResourcesManager::Lookup(const string &name) const {
	lock_guard<mutex> guard(lock);
	for (auto &instance : instances) {
		if (instance.name == name) {
			return make_uniq<ExternalResource>(instance);
		}
	}
	return nullptr;
}

vector<ExternalResource> ExternalResourcesManager::List() const {
	lock_guard<mutex> guard(lock);
	return instances;
}

} // namespace duckdb
