#include "duckdb/function/table/system_functions.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/external_resource_type_registry.hpp"
#include "duckdb/main/external_resources_manager.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/qualified_name.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// duckdb_external_resources(discover := false, type := NULL)
//
// Lists the external resources this instance knows about. By default that is the local registry: the
// resources registered in this instance (managed = true). `type` restricts the listing to one type.
//
// `discover := true` also lists what exists externally. The call is then rewritten (bind_replace) into
// plain SQL: the registry UNION ALL one `SELECT ... FROM <list callback>()` per type with a list callback,
// each tagged with its type name and anti-joined against the registry, so a resource registered here is
// shown once, as managed. A list callback returns one row per resource: handle (MAP), name, reference and
// state. The name is the provider's own name for the resource (unique within the type), so a discovered
// row can be referred to by it; a registered row shows its local name instead. A `type` restriction leaves out the
// other types' branches up front, and a `WHERE type = ...` on the result is pushed into the branches by the optimizer,
// where it folds to a constant and drops the branch - so only the callbacks of the types asked for run.
//===--------------------------------------------------------------------===//

struct ExternalResourcesParameters {
	bool discover = false;
	//! The type to list; empty lists every type.
	string type;
};

static ExternalResourcesParameters ReadParameters(TableFunctionBindInput &input) {
	ExternalResourcesParameters result;
	for (auto &np : input.named_parameters) {
		if (np.second.IsNull()) {
			continue;
		}
		if (np.first == "discover") {
			result.discover = BooleanValue::Get(np.second);
		} else if (np.first == "type") {
			result.type = StringValue::Get(np.second);
		}
	}
	return result;
}

static bool MatchesType(const string &type, const string &type_name) {
	return type.empty() || type == type_name;
}

//! The registry scan restricted to `type`, as SQL.
static string RegistryScan(const string &type) {
	if (type.empty()) {
		return "duckdb_external_resources()";
	}
	return "duckdb_external_resources(type := " + Value(type).ToSQLString() + ")";
}

static bool RegistryHasType(ClientContext &context, const string &type) {
	for (auto &instance : ExternalResourcesManager::Get(context).List()) {
		if (instance.type == type) {
			return true;
		}
	}
	return false;
}

static unique_ptr<TableRef> ExternalResourcesBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	auto params = ReadParameters(input);
	if (!params.discover) {
		return nullptr; // the registry scan below
	}
	vector<string> branches;
	branches.push_back("SELECT name, type, managed, NULL::VARCHAR AS state, NULL::VARCHAR AS reference, uri, "
	                   "attached_db_type, handle FROM " +
	                   RegistryScan(params.type));
	if (!params.type.empty()) {
		// An unknown type autoloads the extension known to provide it, like an unknown function does
		ExternalResourceTypeRegistry::LookupOrAutoload(DatabaseInstance::GetDatabase(context), params.type);
	}
	bool matched = false;
	for (auto &type : ExternalResourceTypeRegistry::Get(context).List()) {
		if (!MatchesType(params.type, type.name)) {
			continue;
		}
		matched = true;
		if (type.list_function.empty()) {
			continue; // a type without a list callback has nothing to discover
		}
		// Resolved here, in the caller's search path, so the generated SQL does not depend on it.
		auto callback = QualifyTableCallback(context, type.list_function);
		if (callback.empty()) {
			throw InvalidInputException(
			    "external resource discovery for type \"%s\": the list callback \"%s\" does not exist", type.name,
			    type.list_function);
		}
		// The callback returns handle (MAP), name, reference and state; the rows registered here are left out by
		// handle. The callback's rows are aliased with its name, so a binder error about a missing column names it.
		auto type_literal = Value(type.name).ToSQLString();
		auto alias = KeywordHelper::WriteQuotedAndEscaped(type.list_function, '"');
		branches.push_back(StringUtil::Format(
		    "SELECT %s.name::VARCHAR AS name, %s AS type, false AS managed, %s.state::VARCHAR AS state, "
		    "%s.reference::VARCHAR AS reference, NULL::VARCHAR AS uri, NULL::VARCHAR AS attached_db_type, "
		    "%s.handle::MAP(VARCHAR, VARCHAR) AS handle FROM %s(MAP {}::MAP(VARCHAR, VARCHAR)) AS %s "
		    "ANTI JOIN %s AS registry ON %s.handle = registry.handle",
		    alias, type_literal, alias, alias, alias, callback, alias, RegistryScan(type.name), alias));
	}
	// A type nothing provides but the registry knows (its extension is not loaded, or it was registered as an
	// opaque type) has nothing to discover, like a type without a list callback. A type neither knows is most
	// likely a typo, or a missing extension.
	if (!matched && !params.type.empty() && !RegistryHasType(context, params.type)) {
		auto extension = ExternalResourceTypeRegistry::ProvidingExtension(params.type);
		if (!extension.empty()) {
			throw InvalidInputException(
			    "duckdb_external_resources: unknown resource type \"%s\": it is provided by the "
			    "%s extension, which could not be autoloaded. Try: INSTALL AND LOAD %s;",
			    params.type, extension, extension);
		}
		throw InvalidInputException("duckdb_external_resources: unknown resource type \"%s\"", params.type);
	}
	Parser parser(context);
	parser.ParseQuery(StringUtil::Join(branches, " UNION ALL "));
	auto select = unique_ptr_cast<SQLStatement, SelectStatement>(std::move(parser.statements[0]));
	return make_uniq<SubqueryRef>(std::move(select));
}

struct ExternalResourcesBindData : public TableFunctionData {
	string type;
};

struct ExternalResourcesGlobalState : public GlobalTableFunctionState {
	vector<ExternalResource> rows;
	idx_t offset = 0;
};

static unique_ptr<FunctionData> ExternalResourcesBind(ClientContext &context, TableFunctionBindInput &input,
                                                      vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto result = make_uniq<ExternalResourcesBindData>();
	result->type = ReadParameters(input).type;

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

static unique_ptr<GlobalTableFunctionState> ExternalResourcesInit(ClientContext &context,
                                                                  TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<ExternalResourcesBindData>();
	auto result = make_uniq<ExternalResourcesGlobalState>();
	for (auto &instance : ExternalResourcesManager::Get(context).List()) {
		if (MatchesType(bind_data.type, instance.type)) {
			result->rows.push_back(std::move(instance));
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
	auto null_string = Value(LogicalType::VARCHAR);
	idx_t count = 0;
	while (state.offset < state.rows.size() && count < STANDARD_VECTOR_SIZE) {
		auto &r = state.rows[state.offset++];
		output.data[0].Append(Value(r.name));
		output.data[1].Append(Value(r.type));
		output.data[2].Append(Value::BOOLEAN(true));
		output.data[3].Append(null_string);
		output.data[4].Append(null_string);
		output.data[5].Append(NullableString(r.uri));
		output.data[6].Append(NullableString(r.attached_db_type));
		output.data[7].Append(r.handle.IsNull() ? empty_handle : r.handle);
		count++;
	}
}

void DuckDBExternalResourcesFun::RegisterFunction(BuiltinFunctions &set) {
	TableFunction fn("duckdb_external_resources", {}, ExternalResourcesFunction, ExternalResourcesBind,
	                 ExternalResourcesInit);
	fn.bind_replace = ExternalResourcesBindReplace;
	fn.GetSignature().WithTypedKwargs("options", [](TypedKwargs &options) {
		options.Add("discover", LogicalType::BOOLEAN).Add("type", LogicalType::VARCHAR);
	});
	// reads the live external resource registry, and discovery reaches out to external systems
	fn.stability = FunctionStability::VOLATILE;
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
	// writes the external resource registry per execution
	fn.stability = FunctionStability::VOLATILE;
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
	TableFunction deregister_fun(
	    "deregister_external_resource", FunctionSignature().AddPositionalOnly("name", LogicalType::VARCHAR),
	    DeregisterExternalResourceFunction, DeregisterExternalResourceBind, DeregisterExternalResourceInit);
	// writes the external resource registry per execution
	deregister_fun.stability = FunctionStability::VOLATILE;
	set.AddFunction(std::move(deregister_fun));
}

} // namespace duckdb
