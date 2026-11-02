#include "duckdb/function/table/system_functions.hpp"

#include "duckdb/common/error_data.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/map.hpp"
#include "duckdb/common/set.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/external_resource_type_registry.hpp"
#include "duckdb/main/external_resources_manager.hpp"
#include "duckdb/parser/qualified_name.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// duckdb_external_resources(discover := false)
//
// Lists the external resources this instance knows about. `discover := false` (the default) returns just
// the locally registered resources (managed = true) and stays local. `discover := true` also calls each
// resource type's `list` callback to discover resources that exist externally, unioning in the ones not
// locally registered (managed = false) — the reconciliation view that feeds REGISTER.
//===--------------------------------------------------------------------===//

struct ExternalResourceRow {
	string name; // local registration name; empty for a discovered (unmanaged) resource
	string type;
	Value handle;
	string uri;
	string attached_db_type;
	bool managed;
	string reference; // from the type's `list` callback
	string state;     // from the type's `list` callback
};

struct ExternalResourcesBindData : public TableFunctionData {
	bool discover = false;
};

struct ExternalResourcesGlobalState : public GlobalTableFunctionState {
	ExternalResourcesGlobalState() : offset(0) {
	}
	vector<ExternalResourceRow> rows;
	idx_t offset;
};

static unique_ptr<FunctionData> ExternalResourcesBind(ClientContext &context, TableFunctionBindInput &input,
                                                      vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto result = make_uniq<ExternalResourcesBindData>();
	for (auto &np : input.named_parameters) {
		if (np.first == "discover" && !np.second.IsNull()) {
			result->discover = BooleanValue::Get(np.second);
		}
	}

	names.emplace_back("name");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("type");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("managed");
	return_types.emplace_back(LogicalType::BOOLEAN);
	names.emplace_back("state");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("reference");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("uri");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("attached_db_type");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("handle");
	return_types.emplace_back(LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR));
	return std::move(result);
}

//! A stable key for a handle, so a discovered resource can be matched against a locally registered one.
//! Handles are only ever compared within a single type: the durable identity is (type, handle), so a
//! matching handle under a different type is a different resource.
static string HandleKey(const Value &handle) {
	return handle.IsNull() ? string() : handle.ToString();
}

//! Call a type's `list` callback and append the resources it discovers that are not already locally
//! managed (keyed by handle). A misconfigured type is a hard error: a callback that fails or returns the
//! wrong schema must surface, not be silently dropped as "nothing exists externally".
static void DiscoverExternalResources(ClientContext &context, const ExternalResourceType &type,
                                      const set<string> &managed_handles, vector<ExternalResourceRow> &rows) {
	// Separate internal connection (the current connection's context lock is held here), resolving the
	// callback in the type's registration-time search path.
	auto callback_con = CreateExternalResourceCallbackConnection(DatabaseInstance::GetDatabase(context), &context);
	auto &con = *callback_con;
	if (!type.search_path.empty()) {
		auto set_res = con.Query("SET search_path = " + Value(type.search_path).ToSQLString());
		if (set_res->HasError()) {
			throw InvalidInputException("external resource discovery for type \"%s\": failed to set the type's "
			                            "registration search path: %s",
			                            type.name, set_res->GetError());
		}
	}
	auto sql =
	    "SELECT * FROM " + QualifiedName::Parse(type.list_function).ToString() + "(MAP {}::MAP(VARCHAR, VARCHAR))";
	// con.Query reports query errors in-band, but with statement verification active (a global debug setting
	// the internal connection inherits) a bind-time failure of the callback is thrown instead. Handle both so
	// the wrapped message is identical in every mode.
	unique_ptr<QueryResult> res;
	try {
		res = con.Query(sql);
	} catch (const std::exception &ex) {
		throw InvalidInputException("external resource discovery for type \"%s\": the list callback \"%s\" failed: %s",
		                            type.name, type.list_function, ErrorData(ex).RawMessage());
	}
	if (res->HasError()) {
		throw InvalidInputException("external resource discovery for type \"%s\": the list callback \"%s\" failed: %s",
		                            type.name, type.list_function, res->GetError());
	}
	// Resolve the columns we consume by name; only `handle` is required.
	idx_t handle_idx = DConstants::INVALID_INDEX, reference_idx = DConstants::INVALID_INDEX,
	      state_idx = DConstants::INVALID_INDEX;
	for (idx_t c = 0; c < res->GetNames().size(); c++) {
		auto col = StringUtil::Lower(res->ColumnName(c).GetIdentifierName());
		if (col == "handle") {
			handle_idx = c;
		} else if (col == "reference") {
			reference_idx = c;
		} else if (col == "state") {
			state_idx = c;
		}
	}
	if (handle_idx == DConstants::INVALID_INDEX) {
		throw InvalidInputException(
		    "external resource discovery for type \"%s\": the list callback \"%s\" did not return a \"handle\" column",
		    type.name, type.list_function);
	}
	auto res_rows = res->Collection().GetRows();
	for (idx_t r = 0; r < res->RowCount(); r++) {
		// The handle is a MAP (opaque to us), the same contract create/status enforce - reject anything else
		// with a clear message rather than a cast failure deep in the output append.
		auto handle = RequireResourceMap(res_rows.GetValue(handle_idx, r), type.list_function, "handle");
		if (managed_handles.count(HandleKey(handle)) > 0) {
			continue; // already shown as a locally managed resource of this type
		}
		ExternalResourceRow row;
		row.type = type.name;
		row.handle = std::move(handle);
		row.managed = false;
		if (reference_idx != DConstants::INVALID_INDEX) {
			auto ref = res_rows.GetValue(reference_idx, r);
			row.reference = ref.IsNull() ? string() : ref.ToString();
		}
		if (state_idx != DConstants::INVALID_INDEX) {
			auto st = res_rows.GetValue(state_idx, r);
			row.state = st.IsNull() ? string() : st.ToString();
		}
		rows.push_back(std::move(row));
	}
}

static unique_ptr<GlobalTableFunctionState> ExternalResourcesInit(ClientContext &context,
                                                                  TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<ExternalResourcesBindData>();
	auto result = make_uniq<ExternalResourcesGlobalState>();

	// Locally registered resources — always shown (managed = true).
	// Handles of the locally registered resources, bucketed per type - handles are only comparable within a type.
	map<string, set<string>> managed_handles;
	for (auto &instance : ExternalResourcesManager::Get(context).List()) {
		ExternalResourceRow row;
		row.name = instance.name;
		row.type = instance.type;
		row.handle = instance.handle;
		row.uri = instance.uri;
		row.attached_db_type = instance.attached_db_type;
		row.managed = true;
		managed_handles[instance.type].insert(HandleKey(instance.handle));
		result->rows.push_back(std::move(row));
	}

	// Discovery union: for each type with a `list` callback, add the externally-existing resources that
	// are not locally managed.
	if (bind_data.discover) {
		for (auto &type : ExternalResourceTypeRegistry::Get(context).List()) {
			if (type.list_function.empty()) {
				continue;
			}
			// operator[] gives an empty bucket for a type with nothing registered locally
			DiscoverExternalResources(context, type, managed_handles[type.name], result->rows);
		}
	}
	return std::move(result);
}

static Value NullableString(const string &s) {
	return s.empty() ? Value(LogicalType::VARCHAR) : Value(s);
}

static void ExternalResourcesFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = data_p.global_state->Cast<ExternalResourcesGlobalState>();
	auto empty_handle = Value(LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR));
	idx_t count = 0;
	while (state.offset < state.rows.size() && count < STANDARD_VECTOR_SIZE) {
		auto &r = state.rows[state.offset++];
		output.data[0].Append(NullableString(r.name));
		output.data[1].Append(Value(r.type));
		output.data[2].Append(Value::BOOLEAN(r.managed));
		output.data[3].Append(NullableString(r.state));
		output.data[4].Append(NullableString(r.reference));
		output.data[5].Append(NullableString(r.uri));
		output.data[6].Append(NullableString(r.attached_db_type));
		output.data[7].Append(r.handle.IsNull() ? empty_handle : r.handle);
		count++;
	}
}

void DuckDBExternalResourcesFun::RegisterFunction(BuiltinFunctions &set) {
	TableFunction fn("duckdb_external_resources", {}, ExternalResourcesFunction, ExternalResourcesBind,
	                 ExternalResourcesInit);
	fn.GetSignature().WithTypedKwargs("options",
	                                  [](TypedKwargs &options) { options.Add("discover", LogicalType::BOOLEAN); });
	set.AddFunction(fn);
}

//===--------------------------------------------------------------------===//
// register_external_resource(type, name, handle, uri := , attached_db_type := , deleter_function := )
//
// Registry write: record an already-provisioned resource in this instance's ExternalResourcesManager under
// `name`, so it shows up in duckdb_external_resources() and can be torn down later. Pure — it provisions
// nothing; feed it the result of create_external_resource(). The durable identity is (type, handle).
//===--------------------------------------------------------------------===//

struct RegisterExternalResourceBindData : public TableFunctionData {
	ExternalResource resource;
};

struct RegisterExternalResourceState : public GlobalTableFunctionState {
	bool done = false;
};

static unique_ptr<FunctionData> RegisterExternalResourceBind(ClientContext &context, TableFunctionBindInput &input,
                                                             vector<LogicalType> &return_types,
                                                             vector<Identifier> &names) {
	auto result = make_uniq<RegisterExternalResourceBindData>();
	auto &resource = result->resource;
	if (input.inputs[0].IsNull() || input.inputs[1].IsNull()) {
		throw InvalidInputException("register_external_resource: the type and name must not be NULL");
	}
	if (input.inputs[2].IsNull()) {
		throw InvalidInputException("register_external_resource: the handle must not be NULL");
	}
	resource.type = StringValue::Get(input.inputs[0]);
	resource.name = StringValue::Get(input.inputs[1]);
	// The handle is the durable identity and doubles as the deleter payload.
	resource.handle = input.inputs[2];
	resource.deleter_payload = input.inputs[2];
	for (auto &np : input.named_parameters) {
		auto key = StringUtil::Lower(np.first.GetIdentifierName());
		if (np.second.IsNull()) {
			continue;
		}
		if (key == "uri") {
			resource.uri = StringValue::Get(np.second);
		} else if (key == "attached_db_type") {
			resource.attached_db_type = StringValue::Get(np.second);
		} else if (key == "deleter_function") {
			// Qualify against the registering connection's search path: teardown runs the deleter on a separate
			// internal connection. Fall back to the name as given when it does not resolve (yet) - the defining
			// extension may not be loaded at registration time.
			auto deleter = StringValue::Get(np.second);
			auto qualified = QualifyTableCallback(context, deleter);
			resource.deleter_function = qualified.empty() ? deleter : qualified;
		}
	}
	return_types.emplace_back(LogicalType::BOOLEAN);
	names.emplace_back("Success");
	return std::move(result);
}

static unique_ptr<GlobalTableFunctionState> RegisterExternalResourceInit(ClientContext &context,
                                                                         TableFunctionInitInput &input) {
	return make_uniq<RegisterExternalResourceState>();
}

static void RegisterExternalResourceFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = data_p.global_state->Cast<RegisterExternalResourceState>();
	if (state.done) {
		return;
	}
	auto &bind_data = data_p.bind_data->Cast<RegisterExternalResourceBindData>();
	// Add() validates name/type are non-empty and rejects a duplicate name.
	ExternalResourcesManager::Get(context).Add(bind_data.resource);
	output.data[0].Append(Value::BOOLEAN(true));
	state.done = true;
}

void RegisterExternalResourceFun::RegisterFunction(BuiltinFunctions &set) {
	TableFunction fn("register_external_resource",
	                 FunctionSignature()
	                     .AddPositionalOnly("type", LogicalType::VARCHAR)
	                     .AddPositionalOnly("name", LogicalType::VARCHAR)
	                     .AddPositionalOnly("handle", LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR)),
	                 RegisterExternalResourceFunction, RegisterExternalResourceBind, RegisterExternalResourceInit);
	fn.GetSignature().WithTypedKwargs("options", [](TypedKwargs &options) {
		options.Add("uri", LogicalType::VARCHAR)
		    .Add("attached_db_type", LogicalType::VARCHAR)
		    .Add("deleter_function", LogicalType::VARCHAR);
	});
	set.AddFunction(fn);
}

//===--------------------------------------------------------------------===//
// deregister_external_resource(name)
//
// Registry write: forget a locally registered resource (ExternalResourcesManager::Remove). Does NOT tear
// the resource down — that is destroy_external_resource's job. Throws if `name` is not registered.
//===--------------------------------------------------------------------===//

struct DeregisterExternalResourceBindData : public TableFunctionData {
	string name;
};

struct DeregisterExternalResourceState : public GlobalTableFunctionState {
	bool done = false;
};

static unique_ptr<FunctionData> DeregisterExternalResourceBind(ClientContext &context, TableFunctionBindInput &input,
                                                               vector<LogicalType> &return_types,
                                                               vector<Identifier> &names) {
	auto result = make_uniq<DeregisterExternalResourceBindData>();
	if (input.inputs[0].IsNull() || StringValue::Get(input.inputs[0]).empty()) {
		throw InvalidInputException("deregister_external_resource: the name must not be NULL or empty");
	}
	result->name = StringValue::Get(input.inputs[0]);
	return_types.emplace_back(LogicalType::BOOLEAN);
	names.emplace_back("Success");
	return std::move(result);
}

static unique_ptr<GlobalTableFunctionState> DeregisterExternalResourceInit(ClientContext &context,
                                                                           TableFunctionInitInput &input) {
	return make_uniq<DeregisterExternalResourceState>();
}

static void DeregisterExternalResourceFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = data_p.global_state->Cast<DeregisterExternalResourceState>();
	if (state.done) {
		return;
	}
	auto &bind_data = data_p.bind_data->Cast<DeregisterExternalResourceBindData>();
	auto removed = ExternalResourcesManager::Get(context).Remove(bind_data.name);
	if (!removed) {
		throw InvalidInputException("deregister_external_resource: \"%s\" is not registered", bind_data.name);
	}
	output.data[0].Append(Value::BOOLEAN(true));
	state.done = true;
}

void DeregisterExternalResourceFun::RegisterFunction(BuiltinFunctions &set) {
	set.AddFunction(TableFunction(
	    "deregister_external_resource", FunctionSignature().AddPositionalOnly("name", LogicalType::VARCHAR),
	    DeregisterExternalResourceFunction, DeregisterExternalResourceBind, DeregisterExternalResourceInit));
}

} // namespace duckdb
