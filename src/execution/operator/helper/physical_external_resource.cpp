#include "duckdb/execution/operator/helper/physical_external_resource.hpp"

#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/execution/operator/helper/launch_external_resource.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/external_resource_type_registry.hpp"
#include "duckdb/main/external_resources_manager.hpp"

namespace duckdb {

//! What a statement did to a record, reported in its `status` column.
static constexpr const char *STATUS_CREATED = "created";
static constexpr const char *STATUS_REGISTERED = "registered";
static constexpr const char *STATUS_PRE_EXISTING = "pre-existing";
static constexpr const char *STATUS_DESTROYED = "destroyed";

//! REGISTER records (name, type, handle) and nothing else: no type lookup, no status call, no autoload. The
//! type is resolved, and the handle checked, when the registration is used (ATTACH / CONNECT / DESTROY), so a
//! registration works before the providing extension is available. An identical record is a no-op.
static const char *RecordExternalResource(ClientContext &client, const string &name, const string &type,
                                          const Value &handle) {
	ExternalResource resource;
	resource.name = name;
	resource.type = type;
	resource.handle = handle;
	resource.deleter_payload = handle;
	return ExternalResourcesManager::Get(client).Record(std::move(resource)) ? STATUS_REGISTERED : STATUS_PRE_EXISTING;
}

//! Provision it (CREATE), then record it under its name.
static ExternalResource CreateExternalResource(ClientContext &client, const BoundExternalResource &data) {
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
		manager.Add(resource);
	} catch (...) {
		// Registration failed (e.g. duplicate name): the resource was provisioned but nothing owns it,
		// so tear it down best-effort. The registration error takes precedence.
		ResourceDeleter(DatabaseInstance::GetDatabase(client), launched.deleter_function, launched.deleter_payload,
		                data.type, data.name)
		    .TryDelete();
		throw;
	}
	return resource;
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
static ExternalResource DestroyExternalResource(ClientContext &client, const BoundExternalResource &data) {
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
	manager.Remove(data.name, instance->handle);
	// Then clean up after it: the borrowing attachments now point at nothing, so detach them best-effort.
	// Runs only after a successful teardown, so the scan never sits in front of the remote call.
	DatabaseManager::Get(client).DetachResourceBorrowers(client, instance->name);
	return *instance;
}

//===--------------------------------------------------------------------===//
// REGISTER EXTERNAL RESOURCES FROM <source>
//===--------------------------------------------------------------------===//
class RegisterExternalResourcesSinkState : public GlobalSinkState {
public:
	RegisterExternalResourcesSinkState(ClientContext &context, const vector<LogicalType> &types)
	    : rows(context, types) {
	}

	//! The source rows: type, handle, name.
	ColumnDataCollection rows;
};

class RegisterExternalResourcesSourceState : public GlobalSourceState {
public:
	bool done = false;
	//! One (status, name, type, handle) row per source row.
	unique_ptr<ColumnDataCollection> result;
	ColumnDataScanState scan;
};

unique_ptr<GlobalSinkState> PhysicalExternalResource::GetGlobalSinkState(ClientContext &context) const {
	return make_uniq<RegisterExternalResourcesSinkState>(context, children[0].get().GetTypes());
}

SinkResultType PhysicalExternalResource::Sink(ExecutionContext &context, DataChunk &chunk,
                                              OperatorSinkInput &input) const {
	input.global_state.Cast<RegisterExternalResourcesSinkState>().rows.Append(chunk);
	return SinkResultType::NEED_MORE_INPUT;
}

unique_ptr<GlobalSourceState> PhysicalExternalResource::GetGlobalSourceState(ClientContext &context) const {
	return make_uniq<RegisterExternalResourcesSourceState>();
}

//! Register the rows: every row is checked before any is recorded, so a malformed row registers nothing. Then
//! each row is recorded (an identical record is left in place) and returned with what happened to it.
static void RegisterExternalResourcesFromRows(ClientContext &client, ColumnDataCollection &rows,
                                              ColumnDataCollection &result) {
	D_ASSERT(rows.ColumnCount() == 3);
	const idx_t type_column = 0, handle_column = 1, name_column = 2;

	auto describe = [](const string &type, const Value &handle) {
		return StringUtil::Format("'%s' %s", type, handle.ToString());
	};
	DataChunk chunk;
	rows.InitializeScanChunk(chunk);
	ColumnDataScanState scan;
	rows.InitializeScan(scan);
	idx_t row_number = 0;
	while (rows.Scan(scan, chunk)) {
		for (idx_t i = 0; i < chunk.size(); i++) {
			row_number++;
			auto type = chunk.GetValue(type_column, i);
			auto handle = chunk.GetValue(handle_column, i);
			auto name = chunk.GetValue(name_column, i);
			if (type.IsNull() || StringValue::Get(type).empty()) {
				throw InvalidInputException("REGISTER EXTERNAL RESOURCES: row %llu has no type. Nothing was registered",
				                            row_number);
			}
			if (handle.IsNull()) {
				throw InvalidInputException(
				    "REGISTER EXTERNAL RESOURCES: row %llu (type '%s') has a NULL handle. Nothing was registered",
				    row_number, StringValue::Get(type));
			}
			if (name.IsNull() || StringValue::Get(name).empty()) {
				throw InvalidInputException(
				    "REGISTER EXTERNAL RESOURCES: row %llu (%s) has no name. Nothing was registered", row_number,
				    describe(StringValue::Get(type), handle));
			}
		}
	}

	DataChunk output;
	output.Initialize(Allocator::Get(client), result.Types());
	rows.InitializeScan(scan);
	while (rows.Scan(scan, chunk)) {
		for (idx_t i = 0; i < chunk.size(); i++) {
			auto name = chunk.GetValue(name_column, i);
			auto type = chunk.GetValue(type_column, i);
			auto handle = chunk.GetValue(handle_column, i);
			auto recorded = RecordExternalResource(client, StringValue::Get(name), StringValue::Get(type), handle);
			output.data[0].Append(Value(recorded));
			output.data[1].Append(name);
			output.data[2].Append(type);
			output.data[3].Append(handle);
		}
		output.CheckCardinality(chunk.size());
		result.Append(output);
		output.Reset();
	}
}

SourceResultType PhysicalExternalResource::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                           OperatorSourceInput &input) const {
	if (!children.empty()) {
		auto &gsource = input.global_state.Cast<RegisterExternalResourcesSourceState>();
		if (!gsource.done) {
			auto &gsink = sink_state->Cast<RegisterExternalResourcesSinkState>();
			gsource.result = make_uniq<ColumnDataCollection>(context.client, types);
			RegisterExternalResourcesFromRows(context.client, gsink.rows, *gsource.result);
			gsource.result->InitializeScan(gsource.scan);
			gsource.done = true;
		}
		gsource.result->Scan(gsource.scan, chunk);
		return chunk.size() == 0 ? SourceResultType::FINISHED : SourceResultType::HAVE_MORE_OUTPUT;
	}

	// One row: status, name, type, handle.
	string status;
	ExternalResource record;
	switch (data.operation) {
	case ExternalResourceOperation::CREATE:
		record = CreateExternalResource(context.client, data);
		status = STATUS_CREATED;
		break;
	case ExternalResourceOperation::REGISTER:
		status = RecordExternalResource(context.client, data.name, data.type, data.handle);
		record.name = data.name;
		record.type = data.type;
		record.handle = data.handle;
		break;
	case ExternalResourceOperation::DESTROY:
		record = DestroyExternalResource(context.client, data);
		status = STATUS_DESTROYED;
		break;
	default:
		throw InternalException("Unsupported operation in PhysicalExternalResource");
	}
	chunk.data[0].Append(Value(status));
	chunk.data[1].Append(Value(record.name));
	chunk.data[2].Append(Value(record.type));
	chunk.data[3].Append(record.handle);
	chunk.CheckCardinality(1);
	return SourceResultType::FINISHED;
}

} // namespace duckdb
