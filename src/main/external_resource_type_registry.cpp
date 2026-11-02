#include "duckdb/main/external_resource_type_registry.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parser/qualified_name.hpp"

namespace duckdb {

ExternalResourceTypeRegistry &ExternalResourceTypeRegistry::Get(DatabaseInstance &db) {
	return db.GetExternalResourceTypeRegistry();
}

ExternalResourceTypeRegistry &ExternalResourceTypeRegistry::Get(ClientContext &context) {
	return ExternalResourceTypeRegistry::Get(*context.db);
}

//! The callbacks are stored as names and only resolved when the resource type is invoked, so reject an unusable
//! name here - the registry is append-only, and a bad entry would squat its name until restart.
static void ValidateFunctionName(const ExternalResourceType &type, const string &key, const string &value) {
	QualifiedName qname;
	string error;
	if (!QualifiedName::TryParse(value, qname, error)) {
		throw InvalidInputException("external resource type \"%s\": '%s' is not a valid function name: %s", type.name,
		                            key, error);
	}
}

//! Invariants of a registry entry, enforced for every caller (SQL and extensions alike).
static void ValidateType(const ExternalResourceType &type) {
	if (type.name.empty()) {
		throw InvalidInputException("an external resource type must have a name");
	}
	if (type.kind.empty()) {
		throw InvalidInputException("external resource type \"%s\": 'kind' is required", type.name);
	}
	if (type.create_function.empty()) {
		throw InvalidInputException("external resource type \"%s\": 'create_function' is required", type.name);
	}
	// The optional callbacks are only validated when set - leaving one out is how a type opts out of it.
	ValidateFunctionName(type, "create_function", type.create_function);
	if (!type.status_function.empty()) {
		ValidateFunctionName(type, "status_function", type.status_function);
	}
	if (!type.destroy_function.empty()) {
		ValidateFunctionName(type, "destroy_function", type.destroy_function);
	}
	if (!type.resolve_function.empty()) {
		ValidateFunctionName(type, "resolve_function", type.resolve_function);
	}
	if (!type.list_function.empty()) {
		ValidateFunctionName(type, "list_function", type.list_function);
	}
}

void ExternalResourceTypeRegistry::Add(ExternalResourceType type) {
	ValidateType(type);
	lock_guard<mutex> guard(lock);
	// First-wins: a name is claimed by its first registration and cannot be redefined.
	for (auto &existing : types) {
		if (existing.name == type.name) {
			throw InvalidInputException("external resource type \"%s\" is already registered", type.name);
		}
	}
	types.push_back(std::move(type));
}

vector<ExternalResourceType> ExternalResourceTypeRegistry::List() const {
	lock_guard<mutex> guard(lock);
	return types;
}

unique_ptr<ExternalResourceType> ExternalResourceTypeRegistry::Lookup(const string &name) const {
	lock_guard<mutex> guard(lock);
	// Names are unique (see Add), so there is at most one match.
	for (auto &type : types) {
		if (type.name == name) {
			return make_uniq<ExternalResourceType>(type);
		}
	}
	return nullptr;
}

} // namespace duckdb
