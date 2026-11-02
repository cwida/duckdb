#include "duckdb/function/table/system_functions.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/external_resource_type_registry.hpp"
#include "duckdb/main/external_resources_manager.hpp"
#include "duckdb/logging/log_type.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/parser/qualified_name.hpp"

#include <chrono>
#include <thread>

namespace duckdb {

//! Empty `extra_info` map for an ExternalResource log entry.
static Value NoExtraInfo() {
	return Value::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR, vector<Value>(), vector<Value>());
}

//! Single-entry `extra_info` map for an ExternalResource log entry.
static Value ExtraInfo(const string &key, const string &value) {
	return Value::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR, {Value(key)}, {Value(value)});
}

//! Log one recipe callback invocation (on response). `resource_type` may be empty (rendered NULL) when
//! the callsite does not know it; `error` is empty on success.
static void LogExternalResourceOperation(ClientContext &context, const string &resource_type,
                                         const string &resource_name, const char *operation, const string &error,
                                         const Value &extra_info) {
	DUCKDB_LOG(context, ExternalResourceLogType, resource_type, resource_name, string(operation), error, extra_info);
}

//! Defaults for the readiness poll loop, both overridable per call via named parameters. A timeout of 0
//! means wait indefinitely (rely on a terminal status or query cancellation).
static constexpr int64_t DEFAULT_READINESS_TIMEOUT_SECONDS = 300;
static constexpr int64_t DEFAULT_POLL_INTERVAL_SECONDS = 5;
//! Upper bound for both. Far longer than any plausible provisioning wait, and far below the point where
//! adding the duration to a steady_clock time point would overflow.
static constexpr int64_t MAX_READINESS_SECONDS = 30LL * 24 * 60 * 60;

//! Look up a key in a MAP(VARCHAR, VARCHAR) Value. Returns false if absent (or the map is NULL).
static bool MapTryGet(const Value &map_value, const string &key, string &out) {
	if (map_value.IsNull()) {
		return false;
	}
	for (auto &entry : MapValue::GetChildren(map_value)) {
		auto &kv = StructValue::GetChildren(entry);
		if (!kv[0].IsNull() && StringValue::Get(kv[0]) == key) {
			out = kv[1].IsNull() ? string() : StringValue::Get(kv[1]);
			return true;
		}
	}
	return false;
}

Value RequireResourceMap(const Value &value, const string &function_name, const string &column) {
	if (value.IsNull()) {
		return value;
	}
	if (value.type().id() != LogicalTypeId::MAP) {
		throw InvalidInputException("external resource callback \"%s\" must return a MAP in its '%s' column, got %s",
		                            function_name, column, value.type().ToString());
	}
	return value.DefaultCastAs(LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR));
}

//===--------------------------------------------------------------------===//
// create_external_resource(type_name [, params := MAP])
//
// Resolves the resource type from the registry, invokes its `create` callback, blocks polling `status`
// until the resource is 'ready', enforces the catalog baseline (uri + attached_db_type), and returns the
// endpoint + deleter binding. Because the current connection's context lock is held during execution,
// the callbacks are run on a SEPARATE internal Connection (its own lock + transaction). Callbacks must
// therefore be non-TEMP (visible instance-wide).
//===--------------------------------------------------------------------===//

//! Read a seconds-valued named parameter, rejecting anything outside [minimum, MAX_READINESS_SECONDS]. The
//! upper bound keeps the poll loop's steady_clock arithmetic from overflowing.
static int64_t ReadSecondsParameter(const string &key, const Value &value, int64_t minimum) {
	auto seconds = value.GetValue<int64_t>();
	if (seconds < minimum || seconds > MAX_READINESS_SECONDS) {
		throw InvalidInputException("create_external_resource: '%s' must be between %lld and %lld, got %lld", key,
		                            minimum, MAX_READINESS_SECONDS, seconds);
	}
	return seconds;
}

struct CreateExternalResourceBindData : public TableFunctionData {
	string type_name;
	string resource_name;
	Value params;
	//! When set (non-NULL), adopt this existing handle instead of calling `create` to provision a new one.
	Value adopt_handle;
	bool teardown_on_failure = true;
	int64_t timeout_seconds = DEFAULT_READINESS_TIMEOUT_SECONDS;
	int64_t poll_interval_seconds = DEFAULT_POLL_INTERVAL_SECONDS;
};

struct CreateExternalResourceState : public GlobalTableFunctionState {
	bool done = false;
};

static unique_ptr<FunctionData> CreateExternalResourceBind(ClientContext &context, TableFunctionBindInput &input,
                                                           vector<LogicalType> &return_types,
                                                           vector<Identifier> &names) {
	auto result = make_uniq<CreateExternalResourceBindData>();
	if (input.inputs[0].IsNull()) {
		throw InvalidInputException("create_external_resource: the type name must not be NULL");
	}
	result->type_name = StringValue::Get(input.inputs[0]);
	result->params = Value::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR, vector<Value>(), vector<Value>());
	for (auto &np : input.named_parameters) {
		auto key = StringUtil::Lower(np.first.GetIdentifierName());
		if (key == "params" && !np.second.IsNull()) {
			result->params = np.second;
		} else if (key == "resource_name" && !np.second.IsNull()) {
			result->resource_name = StringValue::Get(np.second);
		} else if (key == "handle" && !np.second.IsNull()) {
			// declared ANY so that a STRUCT of the handle's fields is accepted as readily as a MAP - anything
			// that is not convertible to the handle's shape is reported here
			result->adopt_handle =
			    np.second.type().id() == LogicalTypeId::MAP
			        ? np.second
			        : np.second.DefaultCastAs(LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR));
		} else if (key == "teardown_on_failure" && !np.second.IsNull()) {
			result->teardown_on_failure = BooleanValue::Get(np.second);
		} else if (key == "timeout_seconds" && !np.second.IsNull()) {
			result->timeout_seconds = ReadSecondsParameter(key, np.second, 0);
		} else if (key == "poll_interval_seconds" && !np.second.IsNull()) {
			result->poll_interval_seconds = ReadSecondsParameter(key, np.second, 1);
		}
	}

	auto map_vv = LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR);
	names.emplace_back("handle");
	return_types.emplace_back(map_vv);
	// Ready-state fields (from the status result), surfaced for direct use by ATTACH.
	names.emplace_back("uri");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("attached_db_type");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("result");
	return_types.emplace_back(map_vv);
	// The deleter binding: a (function name, payload) pair. Tearing the resource down is
	// `CALL <deleter_function>(<deleter_payload>)`. deleter_payload is the create handle.
	names.emplace_back("deleter_function");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("deleter_payload");
	return_types.emplace_back(map_vv);
	return std::move(result);
}

static unique_ptr<GlobalTableFunctionState> CreateExternalResourceInit(ClientContext &context,
                                                                       TableFunctionInitInput &input) {
	return make_uniq<CreateExternalResourceState>();
}

static void CreateExternalResourceFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = data_p.global_state->Cast<CreateExternalResourceState>();
	if (state.done) {
		return;
	}
	auto &bind_data = data_p.bind_data->Cast<CreateExternalResourceBindData>();

	auto type = ExternalResourceTypeRegistry::Get(context).Lookup(bind_data.type_name);
	if (!type) {
		throw InvalidInputException("create_external_resource: unknown resource type \"%s\"", bind_data.type_name);
	}
	// Separate internal connection: the current connection's context lock is held here.
	auto callback_con = CreateExternalResourceCallbackConnection(DatabaseInstance::GetDatabase(context), &context);
	auto &con = *callback_con;
	// Resolve the callbacks in the search path captured at registration, so unqualified names in a non-default
	// schema/catalog keep resolving here (not against this fresh connection's default, nor the current path).
	if (!type->search_path.empty()) {
		auto set_res = con.Query("SET search_path = " + Value(type->search_path).ToSQLString());
		if (set_res->HasError()) {
			throw InvalidInputException(
			    "create_external_resource: could not apply resource type \"%s\" registered search path (%s): %s",
			    bind_data.type_name, type->search_path, set_res->GetError());
		}
	}

	// Provision (call `create`) or adopt (use the caller-supplied handle). A `create` returns a single
	// 'handle' column (a MAP; opaque to us, passed back to status/destroy).
	const bool adopting = !bind_data.adopt_handle.IsNull();
	Value handle;
	if (adopting) {
		handle = RequireResourceMap(bind_data.adopt_handle, bind_data.type_name, "handle");
	} else {
		if (type->create_function.empty()) {
			throw InvalidInputException("create_external_resource: resource type \"%s\" has no create function",
			                            bind_data.type_name);
		}
		auto sql = "SELECT * FROM " + QualifiedName::Parse(type->create_function).ToString() + "(" +
		           bind_data.params.ToSQLString() + ")";
		// Log the operation's real outcome: the callback query returning without error is not yet success, the
		// result still has to satisfy the contract below. Logging from the catch also covers throws added later.
		try {
			auto result = con.Query(sql);
			if (result->HasError()) {
				throw IOException("create_external_resource: create function \"%s\" failed: %s", type->create_function,
				                  result->GetError());
			}
			if (result->RowCount() == 0) {
				throw InvalidInputException("create_external_resource: create function \"%s\" returned no rows",
				                            type->create_function);
			}
			handle = RequireResourceMap(result->Collection().GetRows().GetValue(0, 0), type->create_function, "handle");
			if (handle.IsNull()) {
				// the handle identifies the resource for status and teardown - without one it can never be destroyed
				throw InvalidInputException("create_external_resource: create function \"%s\" returned a NULL handle",
				                            type->create_function);
			}
			LogExternalResourceOperation(context, bind_data.type_name, bind_data.resource_name, "create", string(),
			                             NoExtraInfo());
		} catch (std::exception &ex) {
			LogExternalResourceOperation(context, bind_data.type_name, bind_data.resource_name, "create",
			                             ErrorData(ex).RawMessage(), NoExtraInfo());
			throw;
		}
	}

	// Ownership: when we provisioned the resource, we own its teardown until we successfully hand back the
	// deleter binding below; on any failure in between (status 'failed', timeout, cancellation, malformed
	// result, baseline violation) tear it down best-effort. When a handle was supplied the resource
	// already existed and is not ours to destroy on failure, so the guard stays disarmed regardless of
	// teardown_on_failure.
	bool teardown_on_failure = !adopting && bind_data.teardown_on_failure;
	bool deleter_returned = false;
	struct TeardownGuard {
		ClientContext &context;
		const string &resource_type;
		const string &resource_name;
		Connection &con;
		const string &destroy_function;
		const Value &handle;
		bool teardown;
		const bool &done;
		~TeardownGuard() {
			if (done || !teardown || destroy_function.empty()) {
				return;
			}
			try {
				auto sql = "SELECT * FROM " + QualifiedName::Parse(destroy_function).ToString() + "(" +
				           handle.ToSQLString() + ")";
				auto res = con.Query(sql);
				LogExternalResourceOperation(context, resource_type, resource_name, "destroy",
				                             res->HasError() ? res->GetError() : string(), NoExtraInfo());
			} catch (std::exception &ex) {
				// best-effort: never mask the original failure with a teardown error, but do record it - a
				// throwing Query() would otherwise leave the failed teardown invisible.
				try {
					LogExternalResourceOperation(context, resource_type, resource_name, "destroy",
					                             ErrorData(ex).RawMessage(), NoExtraInfo());
				} catch (...) { // logging must not throw out of a destructor
				}
			} catch (...) {
			}
		}
	} teardown_guard {context, bind_data.type_name, bind_data.resource_name, con, type->destroy_function,
	                  handle,  teardown_on_failure, deleter_returned};

	if (type->status_function.empty()) {
		throw InvalidInputException(
		    "create_external_resource: resource type \"%s\" has no status function (required to await readiness)",
		    bind_data.type_name);
	}

	// Blocking: poll status(handle) until `state` is terminal ('ready' / 'failed'). A synchronous type's
	// status simply returns 'ready' on the first poll.
	Value status_result;
	bool has_deadline = bind_data.timeout_seconds > 0;
	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(bind_data.timeout_seconds);
	while (true) {
		// Cooperative cancellation: this blocking loop never yields back to the executor, so surface a
		// pending Ctrl-C / max_execution_time ourselves (InterruptCheck throws InterruptException on either).
		context.InterruptCheck();
		auto status_sql = "SELECT state, result FROM " + QualifiedName::Parse(type->status_function).ToString() + "(" +
		                  handle.ToSQLString() + ")";
		// One log entry per poll, written once the poll's outcome is known, so a poll is never recorded as 'ok'
		// only to fail validation a few lines later. The catch also covers the 'failed' and timeout throws.
		bool ready = false;
		try {
			auto sres = con.Query(status_sql);
			if (sres->HasError()) {
				throw IOException("create_external_resource: status function \"%s\" failed: %s", type->status_function,
				                  sres->GetError());
			}
			if (sres->RowCount() == 0) {
				throw InvalidInputException("create_external_resource: status function \"%s\" returned no rows",
				                            type->status_function);
			}
			auto status_rows = sres->Collection().GetRows();
			auto state_val = status_rows.GetValue(0, 0);
			auto status_state = state_val.IsNull() ? string() : state_val.ToString();
			if (status_state == "failed") {
				throw IOException("create_external_resource: resource \"%s\" reported state 'failed'",
				                  bind_data.type_name);
			}
			if (status_state == "ready") {
				status_result = RequireResourceMap(status_rows.GetValue(1, 0), type->status_function, "result");
				ready = true;
			} else if (has_deadline && std::chrono::steady_clock::now() >= deadline) {
				throw IOException("create_external_resource: timed out awaiting readiness for \"%s\" (last state '%s')",
				                  bind_data.type_name, status_state);
			}
			LogExternalResourceOperation(context, bind_data.type_name, bind_data.resource_name, "status", string(),
			                             status_state.empty() ? NoExtraInfo() : ExtraInfo("state", status_state));
		} catch (std::exception &ex) {
			LogExternalResourceOperation(context, bind_data.type_name, bind_data.resource_name, "status",
			                             ErrorData(ex).RawMessage(), NoExtraInfo());
			throw;
		}
		if (ready) {
			break;
		}
		// Interruptible wait: sleep the poll interval in short slices, checking for cancellation between them.
		// The slice is small so InterruptCheck() is called often enough to honor max_execution_time promptly
		// (its deadline check is throttled to run every Nth call).
		auto poll_until = std::chrono::steady_clock::now() + std::chrono::seconds(bind_data.poll_interval_seconds);
		while (std::chrono::steady_clock::now() < poll_until) {
			context.InterruptCheck();
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
	}

	// Baseline enforcement: an catalog resource must expose 'uri' + 'attached_db_type' in its result.
	string uri, attached_db_type;
	bool has_uri = MapTryGet(status_result, "uri", uri);
	bool has_type = MapTryGet(status_result, "attached_db_type", attached_db_type);
	if (type->kind == "catalog") {
		if (!has_uri || uri.empty()) {
			throw InvalidInputException(
			    "create_external_resource: catalog resource \"%s\" is missing 'uri' in its status result",
			    bind_data.type_name);
		}
		if (!has_type || attached_db_type.empty()) {
			throw InvalidInputException("create_external_resource: catalog resource \"%s\" is missing "
			                            "'attached_db_type' in its status result",
			                            bind_data.type_name);
		}
	}

	// If the callbacks live in a non-default search path, hand back the destroy function fully qualified so the
	// deleter binding stays resolvable from any other connection later (e.g. when DETACH fires it). Callbacks on
	// the default path are left as-is so the returned name is stable across catalogs/configs.
	string deleter_function = type->destroy_function;
	if (!deleter_function.empty() && !type->search_path.empty()) {
		con.BeginTransaction();
		auto qualified = QualifyTableCallback(*con.context, deleter_function);
		con.Commit();
		if (!qualified.empty()) {
			deleter_function = qualified;
		}
	}

	// Emit: handle, uri, attached_db_type, result, deleter_function, deleter_payload.
	output.data[0].Append(handle);
	output.data[1].Append(has_uri ? Value(uri) : Value(LogicalType::VARCHAR));
	output.data[2].Append(has_type ? Value(attached_db_type) : Value(LogicalType::VARCHAR));
	output.data[3].Append(status_result);
	output.data[4].Append(deleter_function.empty() ? Value(LogicalType::VARCHAR) : Value(deleter_function));
	output.data[5].Append(handle);
	// The deleter binding is now handed to the caller: ownership transfers, so the guard must not tear down.
	deleter_returned = true;
	state.done = true;
}

void CreateExternalResourceFun::RegisterFunction(BuiltinFunctions &set) {
	TableFunction fn("create_external_resource", FunctionSignature().AddPositionalOnly("type", LogicalType::VARCHAR),
	                 CreateExternalResourceFunction, CreateExternalResourceBind, CreateExternalResourceInit);
	fn.GetSignature().WithTypedKwargs("options", [](TypedKwargs &options) {
		options.Add("params", LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR))
		    .Add("resource_name", LogicalType::VARCHAR)
		    .Add("handle", LogicalType::ANY)
		    .Add("teardown_on_failure", LogicalType::BOOLEAN)
		    .Add("timeout_seconds", LogicalType::BIGINT)
		    .Add("poll_interval_seconds", LogicalType::BIGINT);
	});
	set.AddFunction(fn);
}

//===--------------------------------------------------------------------===//
// destroy_external_resource(deleter_function, deleter_payload)
//
// Invokes a deleter binding produced by create_external_resource: CALL <deleter_function>(<payload>).
// Runs on a separate internal Connection (same reasoning as create). Generic: any (string, Value).
//===--------------------------------------------------------------------===//

struct DestroyExternalResourceBindData : public TableFunctionData {
	string deleter_function;
	Value deleter_payload;
};

struct DestroyExternalResourceState : public GlobalTableFunctionState {
	bool done = false;
};

static unique_ptr<FunctionData> DestroyExternalResourceBind(ClientContext &context, TableFunctionBindInput &input,
                                                            vector<LogicalType> &return_types,
                                                            vector<Identifier> &names) {
	auto result = make_uniq<DestroyExternalResourceBindData>();
	if (input.inputs[0].IsNull() || StringValue::Get(input.inputs[0]).empty()) {
		throw InvalidInputException("destroy_external_resource: the deleter function must not be NULL or empty");
	}
	result->deleter_function = StringValue::Get(input.inputs[0]);
	result->deleter_payload = input.inputs[1];

	return_types.emplace_back(LogicalType::BOOLEAN);
	names.emplace_back("Success");
	return std::move(result);
}

static unique_ptr<GlobalTableFunctionState> DestroyExternalResourceInit(ClientContext &context,
                                                                        TableFunctionInitInput &input) {
	return make_uniq<DestroyExternalResourceState>();
}

static void DestroyExternalResourceFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = data_p.global_state->Cast<DestroyExternalResourceState>();
	if (state.done) {
		return;
	}
	auto &bind_data = data_p.bind_data->Cast<DestroyExternalResourceBindData>();

	// CALL <deleter_function>(<deleter_payload>) on a separate internal connection.
	auto callback_con = CreateExternalResourceCallbackConnection(DatabaseInstance::GetDatabase(context), &context);
	auto &con = *callback_con;
	auto sql = "SELECT * FROM " + QualifiedName::Parse(bind_data.deleter_function).ToString() + "(" +
	           bind_data.deleter_payload.ToSQLString() + ")";
	// resource_type/name are unknown at this callsite (the deleter binding does not carry them).
	try {
		auto result = con.Query(sql);
		if (result->HasError()) {
			throw IOException("destroy_external_resource: deleter function \"%s\" failed: %s",
			                  bind_data.deleter_function, result->GetError());
		}
		LogExternalResourceOperation(context, string(), string(), "destroy", string(), NoExtraInfo());
	} catch (std::exception &ex) {
		LogExternalResourceOperation(context, string(), string(), "destroy", ErrorData(ex).RawMessage(), NoExtraInfo());
		throw;
	}
	output.data[0].Append(Value::BOOLEAN(true));
	state.done = true;
}

void DestroyExternalResourceFun::RegisterFunction(BuiltinFunctions &set) {
	set.AddFunction(TableFunction("destroy_external_resource",
	                              FunctionSignature()
	                                  .AddPositionalOnly("deleter_function", LogicalType::VARCHAR)
	                                  .AddPositionalOnly("payload", LogicalType::ANY),
	                              DestroyExternalResourceFunction, DestroyExternalResourceBind,
	                              DestroyExternalResourceInit));
}

} // namespace duckdb
