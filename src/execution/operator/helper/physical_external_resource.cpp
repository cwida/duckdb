#include "duckdb/execution/operator/helper/physical_external_resource.hpp"

#include "duckdb/execution/operator/helper/launch_external_resource.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/external_resource_type_registry.hpp"
#include "duckdb/main/external_resources_manager.hpp"

namespace duckdb {

//! REGISTER records (name, type, handle) and nothing else: no type lookup, no status call, no autoload. The
//! type is resolved, and the handle checked, when the registration is used (ATTACH / CONNECT / DESTROY), so a
//! registration works before the providing extension is available. Add() rejects a duplicate name.
static void RecordExternalResource(ClientContext &client, const BoundExternalResource &data) {
	ExternalResource resource;
	resource.name = data.name;
	resource.type = data.type;
	resource.handle = data.handle;
	resource.deleter_payload = data.handle;
	ExternalResourcesManager::Get(client).Add(std::move(resource));
}

//! Provision it (CREATE), then record it under its name.
static void CreateExternalResource(ClientContext &client, const BoundExternalResource &data) {
	auto &manager = ExternalResourcesManager::Get(client);
	// Reject a duplicate name before provisioning, so a name clash does not spin up (and tear down) a
	// resource needlessly. Add() below re-checks under its lock for the rare concurrent race.
	if (manager.Lookup(data.name)) {
		throw InvalidInputException("external resource \"%s\" is already registered", data.name);
	}
	auto launched = ProvisionExternalResource(client, data.type, data.params, data.name, data.handle);

	ExternalResource resource;
	resource.name = data.name;
	resource.type = data.type;
	resource.handle = launched.deleter_payload; // the create handle (== deleter payload)
	resource.uri = launched.uri;
	resource.attached_db_type = launched.attached_db_type;
	resource.deleter_function = launched.deleter_function;
	resource.deleter_payload = launched.deleter_payload;

	try {
		manager.Add(std::move(resource));
	} catch (...) {
		// Registration failed (e.g. duplicate name): the resource was provisioned but nothing owns it,
		// so tear it down best-effort. The registration error takes precedence.
		ResourceDeleter(DatabaseInstance::GetDatabase(client), launched.deleter_function, launched.deleter_payload,
		                data.type, data.name)
		    .TryDelete();
		throw;
	}
}

//! The deleter of a registration that recorded none: the type's destroy function, qualified against the type's
//! search path so it resolves on the deleter's own connection. Resolving the type autoloads its extension, and
//! an unknown type is an error: the resource cannot be torn down without it.
static string DeriveDeleterFunction(ClientContext &client, const ExternalResource &instance) {
	auto &db = DatabaseInstance::GetDatabase(client);
	auto type = ExternalResourceTypeRegistry::Require(db, instance.type, "DESTROY EXTERNAL RESOURCE");
	if (type->destroy_function.empty() || type->search_path.empty()) {
		return type->destroy_function;
	}
	auto con = CreateExternalResourceCallbackConnection(db, &client);
	auto set_res = con->Query("SET search_path = " + Value(type->search_path).ToSQLString());
	if (set_res->HasError()) {
		throw InvalidInputException("external resource type \"%s\": could not apply its search path '%s': %s",
		                            instance.type, type->search_path, set_res->GetError());
	}
	con->BeginTransaction();
	auto qualified = QualifyTableCallback(*con->context, type->destroy_function);
	con->Commit();
	return qualified.empty() ? type->destroy_function : qualified;
}

//! Tear down and deregister a registered resource. Teardown-first: on a failed teardown the resource
//! stays registered so DESTROY can be retried.
static void DestroyExternalResource(ClientContext &client, const BoundExternalResource &data) {
	auto &manager = ExternalResourcesManager::Get(client);
	auto instance = manager.Lookup(data.name);
	if (!instance) {
		throw InvalidInputException("external resource \"%s\" is not registered", data.name);
	}
	auto deleter_function = instance->deleter_function;
	if (deleter_function.empty()) {
		deleter_function = DeriveDeleterFunction(client, *instance);
	}
	// Never blocked by a borrower: DESTROY is what stops the meter, so it cannot depend on a DETACH
	// that may itself be refused. A stranded resource costs money; a dangling attachment costs nothing.
	auto &db = DatabaseInstance::GetDatabase(client);
	ResourceDeleter(db, deleter_function, instance->deleter_payload, instance->type, instance->name).Delete();
	manager.Remove(data.name);
	// Then clean up after it: the borrowing attachments now point at nothing, so detach them best-effort.
	// Runs only after a successful teardown, so the scan never sits in front of the remote call.
	DatabaseManager::Get(client).DetachResourceBorrowers(client, instance->name);
}

SourceResultType PhysicalExternalResource::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                           OperatorSourceInput &input) const {
	switch (data.operation) {
	case ExternalResourceOperation::CREATE:
		CreateExternalResource(context.client, data);
		break;
	case ExternalResourceOperation::REGISTER:
		RecordExternalResource(context.client, data);
		break;
	case ExternalResourceOperation::DESTROY:
		DestroyExternalResource(context.client, data);
		break;
	default:
		throw InternalException("Unsupported operation in PhysicalExternalResource");
	}
	return SourceResultType::FINISHED;
}

} // namespace duckdb
