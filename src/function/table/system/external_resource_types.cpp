#include "duckdb/function/table/system_functions.hpp"
#include "duckdb/catalog/catalog_search_path.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/external_resource_type_registry.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// register_external_resource_type(name, kind := ..., create_function := ...
//                                 [, status_function, destroy_function, resolve_function])
//===--------------------------------------------------------------------===//

struct RegisterExternalResourceTypeBindData : public TableFunctionData {
	ExternalResourceType type;
};

struct RegisterExternalResourceTypeState : public GlobalTableFunctionState {
	bool done = false;
};

static unique_ptr<FunctionData> RegisterExternalResourceTypeBind(ClientContext &context, TableFunctionBindInput &input,
                                                                 vector<LogicalType> &return_types,
                                                                 vector<Identifier> &names) {
	auto result = make_uniq<RegisterExternalResourceTypeBindData>();
	auto &type = result->type;

	// Only `name` is positional; kind/create (required) and status/destroy/resolve (optional) are named.
	if (input.inputs[0].IsNull() || StringValue::Get(input.inputs[0]).empty()) {
		throw InvalidInputException("register_external_resource_type: name must not be NULL or empty");
	}
	type.name = StringValue::Get(input.inputs[0]);
	type.origin = "user";
	// Capture the registration-time search path so the (by-name, lazily-resolved) callbacks keep resolving
	// from create_external_resource's internal connection — the same reason views/macros record theirs.
	type.search_path = CatalogSearchEntry::ListToString(ClientData::Get(context).catalog_search_path->GetSetPaths());

	for (auto &np : input.named_parameters) {
		auto key = StringUtil::Lower(np.first.GetIdentifierName());
		auto value = np.second.IsNull() ? string() : StringValue::Get(np.second);
		if (key == "kind") {
			type.kind = value;
		} else if (key == "create_function") {
			type.create_function = value;
		} else if (key == "status_function") {
			type.status_function = value;
		} else if (key == "destroy_function") {
			type.destroy_function = value;
		} else if (key == "resolve_function") {
			type.resolve_function = value;
		} else if (key == "list_function") {
			type.list_function = value;
		}
	}
	// The entry itself (required fields, well-formed callback names) is validated by the registry, which
	// enforces the same invariants for extensions registering types directly.

	return_types.emplace_back(LogicalType::BOOLEAN);
	names.emplace_back("Success");
	return std::move(result);
}

static unique_ptr<GlobalTableFunctionState> RegisterExternalResourceTypeInit(ClientContext &context,
                                                                             TableFunctionInitInput &input) {
	return make_uniq<RegisterExternalResourceTypeState>();
}

static void RegisterExternalResourceTypeFunction(ClientContext &context, TableFunctionInput &data_p,
                                                 DataChunk &output) {
	auto &state = data_p.global_state->Cast<RegisterExternalResourceTypeState>();
	if (state.done) {
		return;
	}
	auto &bind_data = data_p.bind_data->Cast<RegisterExternalResourceTypeBindData>();
	// Append-only: registration never removes or updates, only adds.
	ExternalResourceTypeRegistry::Get(context).Add(bind_data.type);
	output.data[0].Append(Value::BOOLEAN(true));
	state.done = true;
}

//===--------------------------------------------------------------------===//
// duckdb_external_resource_types()
//===--------------------------------------------------------------------===//

struct ExternalResourceTypesData : public GlobalTableFunctionState {
	ExternalResourceTypesData() : offset(0) {
	}
	vector<ExternalResourceType> types;
	idx_t offset;
};

static unique_ptr<FunctionData> ExternalResourceTypesBind(ClientContext &context, TableFunctionBindInput &input,
                                                          vector<LogicalType> &return_types,
                                                          vector<Identifier> &names) {
	names.emplace_back("name");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("kind");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("create_function");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("status_function");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("destroy_function");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("resolve_function");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("list_function");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("origin");
	return_types.emplace_back(LogicalType::VARCHAR);
	return nullptr;
}

static unique_ptr<GlobalTableFunctionState> ExternalResourceTypesInit(ClientContext &context,
                                                                      TableFunctionInitInput &input) {
	auto result = make_uniq<ExternalResourceTypesData>();
	result->types = ExternalResourceTypeRegistry::Get(context).List();
	return std::move(result);
}

static Value OptionalString(const string &s) {
	return s.empty() ? Value(LogicalType::VARCHAR) : Value(s);
}

static void ExternalResourceTypesFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<ExternalResourceTypesData>();
	auto &name = output.data[0];
	auto &kind = output.data[1];
	auto &create = output.data[2];
	auto &status = output.data[3];
	auto &destroy = output.data[4];
	auto &resolve = output.data[5];
	auto &list = output.data[6];
	auto &origin = output.data[7];
	idx_t count = 0;
	while (data.offset < data.types.size() && count < STANDARD_VECTOR_SIZE) {
		auto &t = data.types[data.offset++];
		name.Append(Value(t.name));
		kind.Append(Value(t.kind));
		create.Append(Value(t.create_function));
		status.Append(OptionalString(t.status_function));
		destroy.Append(OptionalString(t.destroy_function));
		resolve.Append(OptionalString(t.resolve_function));
		list.Append(OptionalString(t.list_function));
		origin.Append(Value(t.origin));
		count++;
	}
}

//===--------------------------------------------------------------------===//
// Registration
//===--------------------------------------------------------------------===//

void RegisterExternalResourceTypeFun::RegisterFunction(BuiltinFunctions &set) {
	TableFunction fn(
	    "register_external_resource_type", FunctionSignature().AddPositionalOnly("name", LogicalType::VARCHAR),
	    RegisterExternalResourceTypeFunction, RegisterExternalResourceTypeBind, RegisterExternalResourceTypeInit);
	fn.GetSignature()
	    .AddKeywordOnly("kind", LogicalType::VARCHAR)
	    .AddKeywordOnly("create_function", LogicalType::VARCHAR)
	    .WithTypedKwargs("options", [](TypedKwargs &kwargs) {
		    kwargs.Add("status_function", LogicalType::VARCHAR)
		        .Add("destroy_function", LogicalType::VARCHAR)
		        .Add("resolve_function", LogicalType::VARCHAR)
		        .Add("list_function", LogicalType::VARCHAR);
	    });

	set.AddFunction(fn);
}

void DuckDBExternalResourceTypesFun::RegisterFunction(BuiltinFunctions &set) {
	set.AddFunction(TableFunction("duckdb_external_resource_types", {}, ExternalResourceTypesFunction,
	                              ExternalResourceTypesBind, ExternalResourceTypesInit));
}

} // namespace duckdb
