#include "test_capi_v2.hpp"

#include <string>
#include <vector>

#include "duckdb/parallel/pipeline.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

// ---------------------------------------------------------------------------
// V2 table function tests: build a function on a connection, configure its
// signature, register it, and scan it through SQL.
//
// Callbacks avoid Catch assertions: a REQUIRE would throw through the C
// callback boundary into the engine. On failure they populate the provided
// error slot and return; the failure then surfaces as a query error that the
// test asserts on. Cross-callback observations are latched into file-scope
// statics and asserted after the query.
// ---------------------------------------------------------------------------

namespace test_capi_v2 {

namespace {

duckdb_v2_identifier_t TableIdent(const char *s) {
	return duckdb_v2_identifier_t {s, std::strlen(s)};
}

// Create a table function on the connection with the given name.
duckdb_v2_table_function_handle MakeTable(duckdb_v2_connection_handle conn, const char *name) {
	duckdb_v2_table_function_handle function = nullptr;
	REQUIRE(duckdb_v2_table_function_create(FactoryOf(conn), &function, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto str = Convert(name);
	REQUIRE(duckdb_v2_table_function_set_name(function, &str, nullptr) == DUCKDB_V2_ERROR_NONE);
	return function;
}

// The function's borrowed signature.
duckdb_v2_function_signature_handle SigOf(duckdb_v2_table_function_handle function) {
	duckdb_v2_function_signature_handle sig = nullptr;
	REQUIRE(duckdb_v2_table_function_get_signature(function, &sig, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(sig != nullptr);
	return sig;
}

void TableSigParam(duckdb_v2_function_signature_handle sig, const char *name, duckdb_v2_logical_type_handle type,
                   duckdb_v2_value_handle default_value = nullptr) {
	auto name_str = TableIdent(name);
	REQUIRE(duckdb_v2_function_signature_add_parameter(sig, &name_str, type, default_value,
	                                                   DUCKDB_V2_FUNCTION_PARAMETER_KIND_STANDARD,
	                                                   nullptr) == DUCKDB_V2_ERROR_NONE);
}

// Run a query producing a single BIGINT cell.
int64_t QueryI64(duckdb_v2_connection_handle conn, const char *sql) {
	duckdb_v2_result_handle result = nullptr;
	REQUIRE(Query(conn, sql, &result) == DUCKDB_V2_ERROR_NONE);
	auto chunk = StepChunk(result);
	REQUIRE(chunk != nullptr);
	duckdb_v2_vector_handle vec = nullptr;
	duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr);
	duckdb_v2_logical_type_handle type = nullptr;
	REQUIRE(duckdb_v2_vector_get_logical_type(vec, &type, nullptr) == DUCKDB_V2_ERROR_NONE);
	DUCKDB_V2_LOGICAL_TYPE_ID type_id = DUCKDB_V2_LOGICAL_TYPE_ID_INVALID;
	REQUIRE(duckdb_v2_logical_type_get_id(type, &type_id, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_logical_type_destroy(&type);
	REQUIRE(type_id == DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT);
	duckdb_v2_vector_view view {};
	duckdb_v2_vector_get_view(vec, &view, nullptr);
	auto out = static_cast<const int64_t *>(view.data)[SelAt(view.sel, 0)];
	duckdb_v2_data_chunk_destroy(&chunk);
	duckdb_v2_result_destroy(&result);
	return out;
}

// Builds a type from inside a callback, where a context rather than a connection is in hand.
// Returns null after populating the error slot, so the caller can bail out.
duckdb_v2_logical_type_handle MakeTypeInCallback(duckdb_v2_context_handle context, DUCKDB_V2_LOGICAL_TYPE_ID id,
                                                 duckdb_v2_error_info_handle *err) {
	duckdb_v2_factory_handle factory = nullptr;
	duckdb_v2_context_get_factory(context, &factory, nullptr);
	duckdb_v2_logical_type_handle type = nullptr;
	if (duckdb_v2_factory_create_type_from_id(factory, id, nullptr, nullptr, 0, &type, err) != DUCKDB_V2_ERROR_NONE) {
		return nullptr;
	}
	return type;
}

// The message of a failing query, or the empty string when it succeeded.
std::string QueryError(duckdb_v2_connection_handle conn, const char *sql) {
	duckdb_v2_result_handle result = nullptr;
	duckdb_v2_error_info_handle err = nullptr;
	auto rc = Query(conn, sql, &result, &err);
	// Streaming execution is lazy: the failure only surfaces once stepping reaches it, which may
	// take several rounds. Drain until something fails or the stream ends.
	while (rc == DUCKDB_V2_ERROR_NONE) {
		duckdb_v2_data_chunk_handle chunk = nullptr;
		DUCKDB_V2_RESULT_STEP_STATUS status = DUCKDB_V2_RESULT_STEP_STATUS_WAITING;
		rc = duckdb_v2_result_step(result, &chunk, &status, &err);
		duckdb_v2_data_chunk_destroy(&chunk);
		if (rc != DUCKDB_V2_ERROR_NONE || status == DUCKDB_V2_RESULT_STEP_STATUS_FINISHED ||
		    status == DUCKDB_V2_RESULT_STEP_STATUS_CANCELLED) {
			break;
		}
		if (status == DUCKDB_V2_RESULT_STEP_STATUS_WAITING) {
			rc = duckdb_v2_result_wait(result, &err);
		}
	}
	duckdb_v2_result_destroy(&result);
	std::string message;
	if (rc != DUCKDB_V2_ERROR_NONE && err) {
		duckdb_v2_str text = {nullptr, 0};
		duckdb_v2_error_info_get_text(err, &text);
		message = Convert(text);
	}
	duckdb_v2_error_info_destroy(&err);
	REQUIRE(rc != DUCKDB_V2_ERROR_NONE);
	return message;
}

// ---------------------------------------------------------------------------
// my_range(n): one BIGINT column "i" carrying 0..n-1.
// ---------------------------------------------------------------------------

struct RangeBind {
	int64_t count = 0;
};
struct RangeGlobal {
	int64_t position = 0;
};

void DeleteRangeBind(void *ptr) {
	delete static_cast<RangeBind *>(ptr);
}
void DeleteRangeGlobal(void *ptr) {
	delete static_cast<RangeGlobal *>(ptr);
}

void RangeBindCb(duckdb_v2_function_bind_info_handle info, duckdb_v2_table_function_bind_info_handle result,
                 duckdb_v2_context_handle context, duckdb_v2_error_info_handle *err) {
	duckdb_v2_factory_handle factory = nullptr;
	duckdb_v2_context_get_factory(context, &factory, nullptr);
	duckdb_v2_value_handle value = nullptr;
	if (duckdb_v2_function_bind_get_arg_value(info, 0, &value, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	int64_t count = 0;
	auto rc = duckdb_v2_value_get_bigint(value, &count, err);
	duckdb_v2_value_destroy(&value);
	if (rc != DUCKDB_V2_ERROR_NONE) {
		return;
	}

	duckdb_v2_logical_type_handle bigint = nullptr;
	if (duckdb_v2_factory_create_type_from_id(factory, DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT, nullptr, nullptr, 0, &bigint,
	                                          err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto name_str = TableIdent("i");
	rc = duckdb_v2_table_function_bind_add_result_column(result, &name_str, bigint, err);
	duckdb_v2_logical_type_destroy(&bigint);
	if (rc != DUCKDB_V2_ERROR_NONE) {
		return;
	}

	// An exact row count is known here, so hand it to the optimizer directly.
	if (duckdb_v2_table_function_bind_set_cardinality(result, static_cast<idx_t>(count), true, err) !=
	    DUCKDB_V2_ERROR_NONE) {
		return;
	}

	duckdb_v2_opaque bind_data = {new RangeBind {count}, DeleteRangeBind, nullptr};
	duckdb_v2_function_bind_set_bind_data(info, &bind_data, err);
}

void RangeInitGlobalCb(duckdb_v2_table_function_init_global_info_handle info, duckdb_v2_context_handle,
                       duckdb_v2_error_info_handle *err) {
	duckdb_v2_opaque state = {new RangeGlobal {0}, DeleteRangeGlobal, nullptr};
	duckdb_v2_table_function_init_global_set_global_state(info, &state, err);
}

void RangeExecCb(duckdb_v2_table_function_exec_info_handle info, duckdb_v2_context_handle,
                 duckdb_v2_error_info_handle *err) {
	void *bind_ptr = nullptr;
	void *global_ptr = nullptr;
	duckdb_v2_data_chunk_handle chunk = nullptr;
	if (duckdb_v2_table_function_exec_get_bind_data(info, &bind_ptr, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_table_function_exec_get_global_state(info, &global_ptr, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_table_function_exec_get_output_chunk(info, &chunk, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto &bind = *static_cast<RangeBind *>(bind_ptr);
	auto &global = *static_cast<RangeGlobal *>(global_ptr);

	duckdb_v2_vector_handle vec = nullptr;
	void *raw = nullptr;
	if (duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_vector_get_data_mutable(vec, &raw, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}

	auto remaining = bind.count - global.position;
	auto produced =
	    remaining < static_cast<int64_t>(STANDARD_VECTOR_SIZE) ? remaining : static_cast<int64_t>(STANDARD_VECTOR_SIZE);
	if (produced < 0) {
		produced = 0;
	}
	auto *out = static_cast<int64_t *>(raw);
	for (int64_t i = 0; i < produced; i++) {
		out[i] = global.position + i;
	}
	global.position += produced;

	// The first vector's size is the batch's row count; 0 ends the scan.
	duckdb_v2_vector_set_size(vec, static_cast<idx_t>(produced), err);
}

// Registers my_range on the connection, optionally with partition callbacks.
void RegisterRange(EnvFixture &fx, const char *name = "my_range",
                   duckdb_v2_table_function_partitioning_callback_fn info_cb = nullptr,
                   duckdb_v2_table_function_partition_data_callback_fn data_cb = nullptr) {
	auto bigint = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT);
	auto function = MakeTable(fx.conn, name);
	TableSigParam(SigOf(function), "n", bigint);
	REQUIRE(duckdb_v2_table_function_set_bind_callback(function, RangeBindCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_init_global_callback(function, RangeInitGlobalCb, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_exec_callback(function, RangeExecCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	if (info_cb) {
		REQUIRE(duckdb_v2_table_function_set_partitioning_callback(function, info_cb, nullptr) == DUCKDB_V2_ERROR_NONE);
	}
	if (data_cb) {
		REQUIRE(duckdb_v2_table_function_set_partition_data_callback(function, data_cb, nullptr) ==
		        DUCKDB_V2_ERROR_NONE);
	}
	REQUIRE(duckdb_v2_connection_register_table_function(fx.conn, function, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_table_function_destroy(&function);
	duckdb_v2_logical_type_destroy(&bigint);
}

// ---------------------------------------------------------------------------
// my_pairs(n): two columns, "a" INTEGER and "b" VARCHAR, produced in one batch.
// Pins that the row count taken from the first vector reaches the others.
// ---------------------------------------------------------------------------

#if (STANDARD_VECTOR_SIZE > 2)
struct PairsBind {
	int32_t count = 0;
};

void DeletePairsBind(void *ptr) {
	delete static_cast<PairsBind *>(ptr);
}

void PairsBindCb(duckdb_v2_function_bind_info_handle info, duckdb_v2_table_function_bind_info_handle result,
                 duckdb_v2_context_handle context, duckdb_v2_error_info_handle *err) {
	duckdb_v2_value_handle value = nullptr;
	if (duckdb_v2_function_bind_get_arg_value(info, 0, &value, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	int32_t count = 0;
	auto rc = duckdb_v2_value_get_int(value, &count, err);
	duckdb_v2_value_destroy(&value);
	if (rc != DUCKDB_V2_ERROR_NONE) {
		return;
	}

	auto integer = MakeTypeInCallback(context, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER, err);
	if (!integer) {
		return;
	}
	auto name_str = TableIdent("a");
	rc = duckdb_v2_table_function_bind_add_result_column(result, &name_str, integer, err);
	duckdb_v2_logical_type_destroy(&integer);
	if (rc != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto varchar = MakeTypeInCallback(context, DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR, err);
	if (!varchar) {
		return;
	}
	auto name_str2 = TableIdent("b");
	rc = duckdb_v2_table_function_bind_add_result_column(result, &name_str2, varchar, err);
	duckdb_v2_logical_type_destroy(&varchar);
	if (rc != DUCKDB_V2_ERROR_NONE) {
		return;
	}

	duckdb_v2_opaque bind_data = {new PairsBind {count}, DeletePairsBind, nullptr};
	duckdb_v2_function_bind_set_bind_data(info, &bind_data, err);
}

void PairsExecCb(duckdb_v2_table_function_exec_info_handle info, duckdb_v2_context_handle,
                 duckdb_v2_error_info_handle *err) {
	void *bind_ptr = nullptr;
	duckdb_v2_data_chunk_handle chunk = nullptr;
	if (duckdb_v2_table_function_exec_get_bind_data(info, &bind_ptr, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_table_function_exec_get_output_chunk(info, &chunk, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto &bind = *static_cast<PairsBind *>(bind_ptr);

	// Everything is produced in the first batch; the next call ends the scan.
	auto produced = bind.count;
	bind.count = 0;

	duckdb_v2_vector_handle a = nullptr;
	duckdb_v2_vector_handle b = nullptr;
	void *raw = nullptr;
	if (duckdb_v2_data_chunk_get_vector(chunk, 0, &a, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_data_chunk_get_vector(chunk, 1, &b, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_vector_get_data_mutable(a, &raw, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto *values = static_cast<int32_t *>(raw);
	for (int32_t i = 0; i < produced; i++) {
		values[i] = i;
		char text[16] = {};
		auto len = std::snprintf(text, sizeof(text), "row%d", i);
		if (V2VectorAssignString(b, static_cast<idx_t>(i), text, static_cast<idx_t>(len), err) !=
		    DUCKDB_V2_ERROR_NONE) {
			return;
		}
	}
	// Only the first vector is sized; the engine propagates the count to "b".
	duckdb_v2_vector_set_size(a, static_cast<idx_t>(produced), err);
}
#endif

// ---------------------------------------------------------------------------
// my_args(a, b := 7, ...): latches the argument slots the bind callback sees.
// ---------------------------------------------------------------------------

struct TableArgProbe {
	idx_t arg_count = 0;
	idx_t arg_variadic_count = 0;
	int64_t values[8] = {};
	DUCKDB_V2_LOGICAL_TYPE_ID types[8] = {};
	DUCKDB_V2_ERROR oob_type_rc = DUCKDB_V2_ERROR_NONE;
	DUCKDB_V2_ERROR oob_value_rc = DUCKDB_V2_ERROR_NONE;
	bool oob_type_cleared = false;
	bool oob_value_cleared = false;
};
TableArgProbe table_arg_probe;

void ArgsBindCb(duckdb_v2_function_bind_info_handle info, duckdb_v2_table_function_bind_info_handle result,
                duckdb_v2_context_handle context, duckdb_v2_error_info_handle *err) {
	table_arg_probe = TableArgProbe {};
	idx_t variadic_count = 0;
	if (duckdb_v2_function_bind_get_arg_count(info, &table_arg_probe.arg_count, &variadic_count, nullptr, nullptr,
	                                          err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	table_arg_probe.arg_variadic_count = variadic_count;
	table_arg_probe.arg_count += variadic_count;
	for (idx_t i = 0; i < table_arg_probe.arg_count && i < 8; i++) {
		duckdb_v2_logical_type_handle type = nullptr;
		if (duckdb_v2_function_bind_get_arg_type(info, i, &type, err) != DUCKDB_V2_ERROR_NONE) {
			return;
		}
		auto rc = duckdb_v2_logical_type_get_id(type, &table_arg_probe.types[i], err);
		duckdb_v2_logical_type_destroy(&type);
		if (rc != DUCKDB_V2_ERROR_NONE) {
			return;
		}
		duckdb_v2_value_handle value = nullptr;
		if (duckdb_v2_function_bind_get_arg_value(info, i, &value, err) != DUCKDB_V2_ERROR_NONE) {
			return;
		}
		rc = duckdb_v2_value_get_bigint(value, &table_arg_probe.values[i], err);
		duckdb_v2_value_destroy(&value);
		if (rc != DUCKDB_V2_ERROR_NONE) {
			return;
		}
	}

	// An index past the last argument is an input error, and leaves the out-parameter cleared.
	duckdb_v2_logical_type_handle oob_type = nullptr;
	duckdb_v2_value_handle oob_value = nullptr;
	table_arg_probe.oob_type_rc = duckdb_v2_function_bind_get_arg_type(info, 99, &oob_type, nullptr);
	table_arg_probe.oob_value_rc = duckdb_v2_function_bind_get_arg_value(info, 99, &oob_value, nullptr);
	table_arg_probe.oob_type_cleared = oob_type == nullptr;
	table_arg_probe.oob_value_cleared = oob_value == nullptr;

	auto bigint = MakeTypeInCallback(context, DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT, err);
	if (!bigint) {
		return;
	}
	auto name_str = TableIdent("n");
	auto rc = duckdb_v2_table_function_bind_add_result_column(result, &name_str, bigint, err);
	duckdb_v2_logical_type_destroy(&bigint);
	if (rc != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	duckdb_v2_opaque bind_data = {new RangeBind {static_cast<int64_t>(table_arg_probe.arg_count)}, DeleteRangeBind,
	                              nullptr};
	duckdb_v2_function_bind_set_bind_data(info, &bind_data, err);
}

// Emits a single row carrying the argument count, then ends the scan.
void ArgsExecCb(duckdb_v2_table_function_exec_info_handle info, duckdb_v2_context_handle,
                duckdb_v2_error_info_handle *err) {
	void *bind_ptr = nullptr;
	duckdb_v2_data_chunk_handle chunk = nullptr;
	if (duckdb_v2_table_function_exec_get_bind_data(info, &bind_ptr, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_table_function_exec_get_output_chunk(info, &chunk, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto &bind = *static_cast<RangeBind *>(bind_ptr);

	duckdb_v2_vector_handle vec = nullptr;
	void *raw = nullptr;
	if (duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_vector_get_data_mutable(vec, &raw, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	idx_t produced = 0;
	if (bind.count >= 0) {
		static_cast<int64_t *>(raw)[0] = bind.count;
		produced = 1;
		bind.count = -1;
	}
	duckdb_v2_vector_set_size(vec, produced, err);
}

// ---------------------------------------------------------------------------
// State plumbing: a global counter and a per-thread local state, both read back
// in exec.
// ---------------------------------------------------------------------------

struct StateProbe {
	idx_t init_global_calls = 0;
	idx_t init_local_calls = 0;
	bool local_saw_global = false;
	bool exec_saw_global = false;
	bool exec_saw_local = false;
	bool exec_saw_user_data = false;
};
StateProbe state_probe;

struct GlobalState {
	int64_t remaining = 0;
};
struct LocalState {
	int64_t tag = 0;
};

void DeleteGlobalState(void *ptr) {
	delete static_cast<GlobalState *>(ptr);
}
void DeleteLocalState(void *ptr) {
	delete static_cast<LocalState *>(ptr);
}

void StateBindCb(duckdb_v2_function_bind_info_handle info, duckdb_v2_table_function_bind_info_handle result,
                 duckdb_v2_context_handle context, duckdb_v2_error_info_handle *err) {
	state_probe = StateProbe {};
	auto bigint = MakeTypeInCallback(context, DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT, err);
	if (!bigint) {
		return;
	}
	auto name_str = TableIdent("v");
	auto rc = duckdb_v2_table_function_bind_add_result_column(result, &name_str, bigint, err);
	duckdb_v2_logical_type_destroy(&bigint);
	if (rc != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	duckdb_v2_opaque bind_data = {new RangeBind {3}, DeleteRangeBind, nullptr};
	duckdb_v2_function_bind_set_bind_data(info, &bind_data, err);
}

void StateInitGlobalCb(duckdb_v2_table_function_init_global_info_handle info, duckdb_v2_context_handle,
                       duckdb_v2_error_info_handle *err) {
	state_probe.init_global_calls++;
	void *bind_ptr = nullptr;
	if (duckdb_v2_table_function_init_global_get_bind_data(info, &bind_ptr, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto &bind = *static_cast<RangeBind *>(bind_ptr);
	duckdb_v2_opaque state = {new GlobalState {bind.count}, DeleteGlobalState, nullptr};
	if (duckdb_v2_table_function_init_global_set_global_state(info, &state, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	duckdb_v2_table_function_init_global_set_max_threads(info, 1, err);
}

void StateInitLocalCb(duckdb_v2_table_function_init_local_info_handle info, duckdb_v2_context_handle,
                      duckdb_v2_error_info_handle *err) {
	state_probe.init_local_calls++;
	void *global_ptr = nullptr;
	if (duckdb_v2_table_function_init_local_get_global_state(info, &global_ptr, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	state_probe.local_saw_global = global_ptr != nullptr;
	duckdb_v2_opaque state = {new LocalState {42}, DeleteLocalState, nullptr};
	duckdb_v2_table_function_init_local_set_local_state(info, &state, err);
}

void StateExecCb(duckdb_v2_table_function_exec_info_handle info, duckdb_v2_context_handle,
                 duckdb_v2_error_info_handle *err) {
	void *global_ptr = nullptr;
	void *local_ptr = nullptr;
	void *user_ptr = nullptr;
	duckdb_v2_data_chunk_handle chunk = nullptr;
	if (duckdb_v2_table_function_exec_get_global_state(info, &global_ptr, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_table_function_exec_get_local_state(info, &local_ptr, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_table_function_exec_get_user_data(info, &user_ptr, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_table_function_exec_get_output_chunk(info, &chunk, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	state_probe.exec_saw_global = global_ptr != nullptr;
	state_probe.exec_saw_local = local_ptr != nullptr;
	state_probe.exec_saw_user_data = user_ptr != nullptr;

	auto &global = *static_cast<GlobalState *>(global_ptr);
	auto &local = *static_cast<LocalState *>(local_ptr);

	duckdb_v2_vector_handle vec = nullptr;
	void *raw = nullptr;
	if (duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_vector_get_data_mutable(vec, &raw, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	idx_t produced = 0;
	if (global.remaining > 0) {
		global.remaining--;
		static_cast<int64_t *>(raw)[0] = local.tag;
		produced = 1;
	}
	duckdb_v2_vector_set_size(vec, produced, err);
}

// ---------------------------------------------------------------------------
// Failure callbacks
// ---------------------------------------------------------------------------

void FailingBindCb(duckdb_v2_function_bind_info_handle, duckdb_v2_table_function_bind_info_handle,
                   duckdb_v2_context_handle, duckdb_v2_error_info_handle *err) {
	duckdb_v2_error_info_set_code(*err, DUCKDB_V2_ERROR_INPUT_INVALID);
	auto text_str = Convert("bind refused");
	duckdb_v2_error_info_set_text(*err, &text_str);
}

void FailingExecCb(duckdb_v2_table_function_exec_info_handle, duckdb_v2_context_handle,
                   duckdb_v2_error_info_handle *err) {
	duckdb_v2_error_info_set_code(*err, DUCKDB_V2_ERROR_INPUT_OUT_OF_RANGE);
	auto text_str = Convert("exec refused");
	duckdb_v2_error_info_set_text(*err, &text_str);
}

// A bind callback that declares nothing: registration cannot catch this, the scan must.
void NoColumnsBindCb(duckdb_v2_function_bind_info_handle, duckdb_v2_table_function_bind_info_handle,
                     duckdb_v2_context_handle, duckdb_v2_error_info_handle *) {
}

// A bind callback that tries to declare an ANY column.
void AnyColumnBindCb(duckdb_v2_function_bind_info_handle info, duckdb_v2_table_function_bind_info_handle result,
                     duckdb_v2_context_handle context, duckdb_v2_error_info_handle *err) {
	auto any = MakeTypeInCallback(context, DUCKDB_V2_LOGICAL_TYPE_ID_ANY, err);
	if (!any) {
		return;
	}
	auto name_str = TableIdent("x");
	auto rc = duckdb_v2_table_function_bind_add_result_column(result, &name_str, any, err);
	duckdb_v2_logical_type_destroy(&any);
	(void)rc;
}

// ---------------------------------------------------------------------------
// Progress
// ---------------------------------------------------------------------------

struct HookProbe {
	idx_t progress_calls = 0;
	bool progress_saw_global_state = false;
};
HookProbe hook_probe;

// Only the progress test uses this, and that test needs a full-size vector to stay quick.
#if (STANDARD_VECTOR_SIZE == DEFAULT_STANDARD_VECTOR_SIZE)
void ProgressCb(duckdb_v2_table_function_progress_info_handle info, duckdb_v2_context_handle,
                duckdb_v2_error_info_handle *err) {
	hook_probe.progress_calls++;
	void *global_ptr = nullptr;
	if (duckdb_v2_table_function_progress_get_global_state(info, &global_ptr, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	hook_probe.progress_saw_global_state = global_ptr != nullptr;
	duckdb_v2_table_function_progress_set_progress(info, 0.5, err);
}
#endif

} // namespace

// ===========================================================================
// Register on a connection and scan through SQL.
// ===========================================================================

TEST_CASE("V2 table: register on connection and scan", "[capi_v2][table_function]") {
	EnvFixture fx;
	RegisterRange(fx);

	REQUIRE(QueryI64(fx.conn, "SELECT count(*) FROM my_range(10)") == 10);
	REQUIRE(QueryI64(fx.conn, "SELECT sum(i)::BIGINT FROM my_range(10)") == 45);
	// A scan spanning several batches, and one producing nothing at all.
	REQUIRE(QueryI64(fx.conn, "SELECT count(*) FROM my_range(5000)") == 5000);
	REQUIRE(QueryI64(fx.conn, "SELECT count(*) FROM my_range(0)") == 0);

	// The bind callback's column declaration is the function's schema.
	duckdb_v2_result_handle result = nullptr;
	REQUIRE(Query(fx.conn, "SELECT * FROM my_range(3)", &result) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(ColumnCount(result) == 1);
	RequireColumn(result, 0, "i", DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT);
	REQUIRE(DrainRowCount(result) == 3);
	duckdb_v2_result_destroy(&result);
}

// my_pairs writes its whole result in one batch, so it needs a vector that holds more than two rows.
#if (STANDARD_VECTOR_SIZE > 2)
TEST_CASE("V2 table: multiple result columns share the batch row count", "[capi_v2][table_function]") {
	EnvFixture fx;
	auto integer = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);

	auto function = MakeTable(fx.conn, "my_pairs");
	TableSigParam(SigOf(function), "n", integer);
	REQUIRE(duckdb_v2_table_function_set_bind_callback(function, PairsBindCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_exec_callback(function, PairsExecCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_register_table_function(fx.conn, function, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_table_function_destroy(&function);
	duckdb_v2_logical_type_destroy(&integer);

	duckdb_v2_result_handle result = nullptr;
	REQUIRE(Query(fx.conn, "SELECT * FROM my_pairs(4)", &result) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(ColumnCount(result) == 2);
	RequireColumn(result, 0, "a", DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	RequireColumn(result, 1, "b", DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR);
	REQUIRE(DrainRowCount(result) == 4);
	duckdb_v2_result_destroy(&result);

	// Both columns carry all four rows: the count set on "a" reached "b".
	REQUIRE(QueryI64(fx.conn, "SELECT count(b) FROM my_pairs(4)") == 4);
	REQUIRE(QueryI64(fx.conn, "SELECT sum(a)::BIGINT FROM my_pairs(4)") == 6);

	duckdb_v2_result_handle text = nullptr;
	REQUIRE(Query(fx.conn, "SELECT string_agg(b, ',' ORDER BY a) FROM my_pairs(3)", &text) == DUCKDB_V2_ERROR_NONE);
	auto chunk = StepChunk(text);
	REQUIRE(chunk != nullptr);
	duckdb_v2_vector_handle vec = nullptr;
	duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr);
	duckdb_v2_vector_view view {};
	duckdb_v2_vector_get_view(vec, &view, nullptr);
	auto bytes = static_cast<const duckdb_v2_bytes *>(view.data)[SelAt(view.sel, 0)];
	REQUIRE(Convert(Convert(bytes)) == "row0,row1,row2");
	duckdb_v2_data_chunk_destroy(&chunk);
	duckdb_v2_result_destroy(&text);
}
#endif

// ===========================================================================
// Argument routing: every parameter can be passed by position or by name, and a defaulted one can be omitted.
// ===========================================================================

TEST_CASE("V2 table: parameter defaults, named arguments and varargs", "[capi_v2][table_function]") {
	EnvFixture fx;
	auto bigint = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT);
	auto seven = MakeInt64Value(fx.factory, 7);

	auto function = MakeTable(fx.conn, "my_args");
	auto sig = SigOf(function);
	TableSigParam(sig, "a", bigint);
	TableSigParam(sig, "b", bigint, seven);
	auto args_ident = TableIdent("args");
	REQUIRE(duckdb_v2_function_signature_add_parameter(sig, &args_ident, bigint, nullptr,
	                                                   DUCKDB_V2_FUNCTION_PARAMETER_KIND_POSITIONAL_VARIADIC,
	                                                   nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_bind_callback(function, ArgsBindCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_exec_callback(function, ArgsExecCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_register_table_function(fx.conn, function, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_table_function_destroy(&function);
	duckdb_v2_value_destroy(&seven);
	duckdb_v2_logical_type_destroy(&bigint);

	// Only the required parameter: the defaulted one is still present, carrying its default.
	REQUIRE(QueryI64(fx.conn, "SELECT * FROM my_args(1)") == 2);
	REQUIRE(table_arg_probe.arg_count == 2);
	REQUIRE(table_arg_probe.values[0] == 1);
	REQUIRE(table_arg_probe.values[1] == 7);
	REQUIRE(table_arg_probe.types[0] == DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT);
	// An out-of-range index fails and leaves the out-parameter cleared.
	REQUIRE(table_arg_probe.oob_type_rc == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(table_arg_probe.oob_value_rc == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(table_arg_probe.oob_type_cleared);
	REQUIRE(table_arg_probe.oob_value_cleared);

	// A defaulted parameter is passed by name.
	REQUIRE(QueryI64(fx.conn, "SELECT * FROM my_args(1, b => 5)") == 2);
	REQUIRE(table_arg_probe.values[0] == 1);
	REQUIRE(table_arg_probe.values[1] == 5);

	// A defaulted parameter is filled by position before the variadic tail, which follows in call order.
	REQUIRE(QueryI64(fx.conn, "SELECT * FROM my_args(1, 30, 40)") == 3);
	REQUIRE(table_arg_probe.arg_count == 3);
	REQUIRE(table_arg_probe.arg_variadic_count == 1);
	REQUIRE(table_arg_probe.values[0] == 1);
	REQUIRE(table_arg_probe.values[1] == 30);
	REQUIRE(table_arg_probe.values[2] == 40);

	// Without "**kwargs", a name no parameter declares is rejected rather than dropped.
	duckdb_v2_result_handle result = nullptr;
	REQUIRE(Query(fx.conn, "SELECT * FROM my_args(1, 30, x := 40)", &result) != DUCKDB_V2_ERROR_NONE);
	duckdb_v2_result_destroy(&result);
}

// ===========================================================================
// Global and local state.
// ===========================================================================

TEST_CASE("V2 table: user data, global state and local state reach exec", "[capi_v2][table_function]") {
	EnvFixture fx;

	auto function = MakeTable(fx.conn, "my_state");
	int payload = 99;
	duckdb_v2_opaque user_data = {&payload, nullptr, nullptr};
	REQUIRE(duckdb_v2_table_function_set_user_data(function, &user_data, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_bind_callback(function, StateBindCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_init_global_callback(function, StateInitGlobalCb, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_init_local_callback(function, StateInitLocalCb, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_exec_callback(function, StateExecCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_register_table_function(fx.conn, function, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_table_function_destroy(&function);

	// The bind data seeded the global counter with 3 rows, each carrying the local state's tag.
	REQUIRE(QueryI64(fx.conn, "SELECT sum(v)::BIGINT FROM my_state()") == 126);
	REQUIRE(state_probe.init_global_calls == 1);
	REQUIRE(state_probe.init_local_calls >= 1);
	REQUIRE(state_probe.local_saw_global);
	REQUIRE(state_probe.exec_saw_global);
	REQUIRE(state_probe.exec_saw_local);
	REQUIRE(state_probe.exec_saw_user_data);
}

// ===========================================================================
// Errors reported from the callbacks.
// ===========================================================================

TEST_CASE("V2 table: bind and exec errors propagate to the result", "[capi_v2][table_function]") {
	EnvFixture fx;

	auto failing_bind = MakeTable(fx.conn, "bad_bind");
	REQUIRE(duckdb_v2_table_function_set_bind_callback(failing_bind, FailingBindCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_exec_callback(failing_bind, RangeExecCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_register_table_function(fx.conn, failing_bind, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_table_function_destroy(&failing_bind);

	auto failing_exec = MakeTable(fx.conn, "bad_exec");
	REQUIRE(duckdb_v2_table_function_set_bind_callback(failing_exec, StateBindCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_exec_callback(failing_exec, FailingExecCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_register_table_function(fx.conn, failing_exec, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_table_function_destroy(&failing_exec);

	auto no_columns = MakeTable(fx.conn, "no_columns");
	REQUIRE(duckdb_v2_table_function_set_bind_callback(no_columns, NoColumnsBindCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_exec_callback(no_columns, RangeExecCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_register_table_function(fx.conn, no_columns, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_table_function_destroy(&no_columns);

	auto any_column = MakeTable(fx.conn, "any_column");
	REQUIRE(duckdb_v2_table_function_set_bind_callback(any_column, AnyColumnBindCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_exec_callback(any_column, RangeExecCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_register_table_function(fx.conn, any_column, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_table_function_destroy(&any_column);

	REQUIRE(QueryError(fx.conn, "SELECT * FROM bad_bind()").find("bind refused") != std::string::npos);
	REQUIRE(QueryError(fx.conn, "SELECT * FROM bad_exec()").find("exec refused") != std::string::npos);
	REQUIRE(QueryError(fx.conn, "SELECT * FROM no_columns()").find("result columns") != std::string::npos);
	// The ANY column is rejected where it is declared, and the bind callback ignores the failure,
	// so the scan fails for having declared nothing.
	REQUIRE_FALSE(QueryError(fx.conn, "SELECT * FROM any_column()").empty());
}

// ===========================================================================
// Registration refusals.
// ===========================================================================

TEST_CASE("V2 table: registration refusals", "[capi_v2][table_function]") {
	EnvFixture fx;
	auto bigint = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT);

	// No name.
	{
		duckdb_v2_table_function_handle function = nullptr;
		REQUIRE(duckdb_v2_table_function_create(fx.factory, &function, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(duckdb_v2_table_function_set_bind_callback(function, StateBindCb, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(duckdb_v2_table_function_set_exec_callback(function, RangeExecCb, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(duckdb_v2_connection_register_table_function(fx.conn, function, nullptr) ==
		        DUCKDB_V2_ERROR_INPUT_INVALID);
		duckdb_v2_table_function_destroy(&function);
	}

	// No bind callback: a table function has no other way to declare its columns.
	{
		auto function = MakeTable(fx.conn, "no_bind");
		REQUIRE(duckdb_v2_table_function_set_exec_callback(function, RangeExecCb, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(duckdb_v2_connection_register_table_function(fx.conn, function, nullptr) ==
		        DUCKDB_V2_ERROR_INPUT_INVALID);
		duckdb_v2_table_function_destroy(&function);
	}

	// No exec callback.
	{
		auto function = MakeTable(fx.conn, "no_exec");
		REQUIRE(duckdb_v2_table_function_set_bind_callback(function, StateBindCb, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(duckdb_v2_connection_register_table_function(fx.conn, function, nullptr) ==
		        DUCKDB_V2_ERROR_INPUT_INVALID);
		duckdb_v2_table_function_destroy(&function);
	}

	// A return type on the signature: the columns come from bind instead.
	{
		auto function = MakeTable(fx.conn, "has_return_type");
		REQUIRE(duckdb_v2_function_signature_set_return_type(SigOf(function), bigint, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(duckdb_v2_table_function_set_bind_callback(function, StateBindCb, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(duckdb_v2_table_function_set_exec_callback(function, RangeExecCb, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(duckdb_v2_connection_register_table_function(fx.conn, function, nullptr) ==
		        DUCKDB_V2_ERROR_INPUT_INVALID);
		duckdb_v2_table_function_destroy(&function);
	}

	// A parameter without a default following one with a default.
	{
		auto function = MakeTable(fx.conn, "bad_defaults");
		auto sig = SigOf(function);
		auto seven = MakeInt64Value(fx.factory, 7);
		TableSigParam(sig, "a", bigint, seven);
		TableSigParam(sig, "b", bigint);
		duckdb_v2_value_destroy(&seven);
		REQUIRE(duckdb_v2_table_function_set_bind_callback(function, StateBindCb, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(duckdb_v2_table_function_set_exec_callback(function, RangeExecCb, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(duckdb_v2_connection_register_table_function(fx.conn, function, nullptr) ==
		        DUCKDB_V2_ERROR_INPUT_INVALID);
		duckdb_v2_table_function_destroy(&function);
	}

	duckdb_v2_logical_type_destroy(&bigint);
}

// ===========================================================================
// Progress.
// ===========================================================================

#if (STANDARD_VECTOR_SIZE == DEFAULT_STANDARD_VECTOR_SIZE)
TEST_CASE("V2 table: progress callback reports scan progress", "[capi_v2][table_function]") {
	EnvFixture fx;
	hook_probe = HookProbe {};

	// Single-threaded so all execution happens in our steps, and the progress
	// bar enabled so the engine polls the scan for progress at all.
	for (const char *setup_sql :
	     {"SET threads=1", "SET enable_progress_bar=true", "SET enable_progress_bar_print=false"}) {
		ExecSQL(fx.conn, setup_sql);
	}

	auto bigint = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT);
	auto function = MakeTable(fx.conn, "my_progress");
	TableSigParam(SigOf(function), "n", bigint);
	REQUIRE(duckdb_v2_table_function_set_bind_callback(function, RangeBindCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_init_global_callback(function, RangeInitGlobalCb, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_exec_callback(function, RangeExecCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_progress_callback(function, ProgressCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_register_table_function(fx.conn, function, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_table_function_destroy(&function);
	duckdb_v2_logical_type_destroy(&bigint);

	duckdb_v2_result_handle r = nullptr;
	REQUIRE(Query(fx.conn, "SELECT * FROM my_progress(1000000)", &r, nullptr) == DUCKDB_V2_ERROR_NONE);

	// The round count is timing-dependent, so the loop latches instead of asserting.
	auto step_rc = DUCKDB_V2_ERROR_NONE;
	idx_t rows = 0;
	while (true) {
		duckdb_v2_data_chunk_handle chunk = nullptr;
		DUCKDB_V2_RESULT_STEP_STATUS status = DUCKDB_V2_RESULT_STEP_STATUS_WAITING;
		step_rc = duckdb_v2_result_step(r, &chunk, &status, nullptr);
		if (step_rc != DUCKDB_V2_ERROR_NONE) {
			break;
		}
		if (chunk) {
			idx_t size = 0;
			duckdb_v2_data_chunk_get_size(chunk, &size, nullptr);
			rows += size;
			duckdb_v2_data_chunk_destroy(&chunk);
		}
		if (status == DUCKDB_V2_RESULT_STEP_STATUS_FINISHED) {
			break;
		}
	}
	REQUIRE(step_rc == DUCKDB_V2_ERROR_NONE);
	REQUIRE(rows == 1000000);
	REQUIRE(hook_probe.progress_calls >= 1);
	REQUIRE(hook_probe.progress_saw_global_state);

	duckdb_v2_result_destroy(&r);
}
#endif

// ===========================================================================
// Null argument handling and destroy null-safety.
// ===========================================================================

TEST_CASE("V2 table: null arguments and destroy null-safety", "[capi_v2][table_function]") {
	EnvFixture fx;

	duckdb_v2_table_function_handle function = nullptr;
	REQUIRE(duckdb_v2_table_function_create(nullptr, &function, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_table_function_create(fx.factory, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_connection_register_table_function(nullptr, function, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	auto str = Convert("x");
	REQUIRE(duckdb_v2_table_function_set_name(nullptr, &str, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_table_function_set_bind_callback(nullptr, StateBindCb, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_table_function_set_exec_callback(nullptr, RangeExecCb, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_table_function_get_signature(nullptr, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_connection_register_table_function(fx.conn, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	REQUIRE(duckdb_v2_table_function_exec_get_output_chunk(nullptr, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_table_function_progress_set_progress(nullptr, 0.5, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	// A thread count of zero would starve the scan.
	REQUIRE(duckdb_v2_table_function_init_global_set_max_threads(nullptr, 0, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	// Destroy is null-safe and clears the slot.
	REQUIRE(duckdb_v2_table_function_destroy(nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_table_function_handle already_null = nullptr;
	REQUIRE(duckdb_v2_table_function_destroy(&already_null) == DUCKDB_V2_ERROR_NONE);
	auto live = MakeTable(fx.conn, "unused");
	REQUIRE(duckdb_v2_table_function_destroy(&live) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(live == nullptr);
}

// ---------------------------------------------------------------------------
// proj_probe(): three BIGINT columns x, y, z, three rows, cell = declared column * 100 + row. The exec callback
// fills whatever the scan asks for through the column mapping, so it serves with and without projection pushdown.
// ---------------------------------------------------------------------------

namespace {

struct ProjGlobal {
	idx_t position = 0;
};

constexpr idx_t PROJ_ROWS = 3;

void DeleteProjGlobal(void *ptr) {
	delete static_cast<ProjGlobal *>(ptr);
}

// The column mapping each init callback observed for the last scan.
std::vector<idx_t> proj_global_columns;
std::vector<idx_t> proj_local_columns;

void ProjBindCb(duckdb_v2_function_bind_info_handle info, duckdb_v2_table_function_bind_info_handle result,
                duckdb_v2_context_handle context, duckdb_v2_error_info_handle *err) {
	auto bigint = MakeTypeInCallback(context, DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT, err);
	if (!bigint) {
		return;
	}
	for (auto name : {"x", "y", "z"}) {
		auto name_str = TableIdent(name);
		if (duckdb_v2_table_function_bind_add_result_column(result, &name_str, bigint, err) != DUCKDB_V2_ERROR_NONE) {
			break;
		}
	}
	duckdb_v2_logical_type_destroy(&bigint);
}

void ProjInitGlobalCb(duckdb_v2_table_function_init_global_info_handle info, duckdb_v2_context_handle,
                      duckdb_v2_error_info_handle *err) {
	proj_global_columns.clear();
	idx_t count = 0;
	if (duckdb_v2_table_function_init_global_get_column_count(info, &count, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	for (idx_t i = 0; i < count; i++) {
		idx_t column = 0;
		if (duckdb_v2_table_function_init_global_get_column_index(info, i, &column, err) != DUCKDB_V2_ERROR_NONE) {
			return;
		}
		proj_global_columns.push_back(column);
	}
	duckdb_v2_opaque state = {new ProjGlobal {}, DeleteProjGlobal, nullptr};
	duckdb_v2_table_function_init_global_set_global_state(info, &state, err);
}

void ProjInitLocalCb(duckdb_v2_table_function_init_local_info_handle info, duckdb_v2_context_handle,
                     duckdb_v2_error_info_handle *err) {
	proj_local_columns.clear();
	idx_t count = 0;
	if (duckdb_v2_table_function_init_local_get_column_count(info, &count, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	for (idx_t i = 0; i < count; i++) {
		idx_t column = 0;
		if (duckdb_v2_table_function_init_local_get_column_index(info, i, &column, err) != DUCKDB_V2_ERROR_NONE) {
			return;
		}
		proj_local_columns.push_back(column);
	}
}

void ProjExecCb(duckdb_v2_table_function_exec_info_handle info, duckdb_v2_context_handle,
                duckdb_v2_error_info_handle *err) {
	void *global_ptr = nullptr;
	duckdb_v2_data_chunk_handle chunk = nullptr;
	idx_t count = 0;
	if (duckdb_v2_table_function_exec_get_global_state(info, &global_ptr, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_table_function_exec_get_output_chunk(info, &chunk, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_table_function_exec_get_column_count(info, &count, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto &global = *static_cast<ProjGlobal *>(global_ptr);
	// Batches are capped by the vector size so the probe also serves builds with a tiny STANDARD_VECTOR_SIZE.
	idx_t rows = PROJ_ROWS - global.position;
	if (rows > STANDARD_VECTOR_SIZE) {
		rows = STANDARD_VECTOR_SIZE;
	}

	for (idx_t i = 0; i < count; i++) {
		idx_t column = 0;
		duckdb_v2_vector_handle vec = nullptr;
		void *raw = nullptr;
		if (duckdb_v2_table_function_exec_get_column_index(info, i, &column, err) != DUCKDB_V2_ERROR_NONE ||
		    duckdb_v2_data_chunk_get_vector(chunk, i, &vec, err) != DUCKDB_V2_ERROR_NONE ||
		    duckdb_v2_vector_get_data_mutable(vec, &raw, err) != DUCKDB_V2_ERROR_NONE) {
			return;
		}
		for (idx_t row = 0; row < rows; row++) {
			static_cast<int64_t *>(raw)[row] = static_cast<int64_t>(column * 100 + global.position + row);
		}
		if (i == 0) {
			duckdb_v2_vector_set_size(vec, rows, err);
		}
	}
	global.position += rows;
}

void RegisterProj(duckdb_v2_connection_handle conn, const char *name, bool projection_pushdown) {
	auto function = MakeTable(conn, name);
	REQUIRE(duckdb_v2_table_function_set_bind_callback(function, ProjBindCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_init_global_callback(function, ProjInitGlobalCb, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_init_local_callback(function, ProjInitLocalCb, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_exec_callback(function, ProjExecCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_projection_pushdown(function, projection_pushdown, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_register_table_function(conn, function, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_table_function_destroy(&function);
}

// Runs a query and calls fn(view, row) for every cell, row-major.
template <class FN>
void ForEachCell(duckdb_v2_connection_handle conn, const char *sql, FN fn) {
	duckdb_v2_result_handle result = nullptr;
	REQUIRE(Query(conn, sql, &result) == DUCKDB_V2_ERROR_NONE);
	while (auto chunk = StepChunk(result)) {
		idx_t columns = 0;
		idx_t rows = 0;
		duckdb_v2_data_chunk_get_vector_count(chunk, &columns, nullptr);
		duckdb_v2_data_chunk_get_size(chunk, &rows, nullptr);
		for (idx_t row = 0; row < rows; row++) {
			for (idx_t col = 0; col < columns; col++) {
				duckdb_v2_vector_handle vec = nullptr;
				duckdb_v2_data_chunk_get_vector(chunk, col, &vec, nullptr);
				duckdb_v2_vector_view view {};
				duckdb_v2_vector_get_view(vec, &view, nullptr);
				fn(view, SelAt(view.sel, row));
			}
		}
		duckdb_v2_data_chunk_destroy(&chunk);
	}
	duckdb_v2_result_destroy(&result);
}

// Runs a query and collects its BIGINT columns row-major.
std::vector<int64_t> QueryCells(duckdb_v2_connection_handle conn, const char *sql) {
	std::vector<int64_t> cells;
	ForEachCell(conn, sql, [&](const duckdb_v2_vector_view &view, idx_t row) {
		cells.push_back(static_cast<const int64_t *>(view.data)[row]);
	});
	return cells;
}

// ---------------------------------------------------------------------------
// claim_probe(n): column i BIGINT with 0..n-1. Its pushdown callback claims every "i < constant" predicate by
// recording the bound in the bind data; the scan then deliberately produces two rows past the bound, which only
// reach the result if the engine really stopped applying the claimed predicate.
// ---------------------------------------------------------------------------

struct ClaimBind {
	int64_t count = 0;
	int64_t bound = -1;
};
struct ClaimGlobal {
	int64_t position = 0;
};

void DeleteClaimBind(void *ptr) {
	delete static_cast<ClaimBind *>(ptr);
}
void DeleteClaimGlobal(void *ptr) {
	delete static_cast<ClaimGlobal *>(ptr);
}

// Whether the pushdown callback saw the user data it was registered with.
bool claim_saw_user_data = false;
int claim_user_data_marker = 0;

void ClaimBindCb(duckdb_v2_function_bind_info_handle info, duckdb_v2_table_function_bind_info_handle result,
                 duckdb_v2_context_handle context, duckdb_v2_error_info_handle *err) {
	duckdb_v2_value_handle value = nullptr;
	if (duckdb_v2_function_bind_get_arg_value(info, 0, &value, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	int64_t count = 0;
	auto rc = duckdb_v2_value_get_bigint(value, &count, err);
	duckdb_v2_value_destroy(&value);
	if (rc != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto bigint = MakeTypeInCallback(context, DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT, err);
	if (!bigint) {
		return;
	}
	auto name_str = TableIdent("i");
	rc = duckdb_v2_table_function_bind_add_result_column(result, &name_str, bigint, err);
	duckdb_v2_logical_type_destroy(&bigint);
	if (rc != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	duckdb_v2_opaque bind_data = {new ClaimBind {count, -1}, DeleteClaimBind, nullptr};
	duckdb_v2_function_bind_set_bind_data(info, &bind_data, err);
}

void ClaimPushdownCb(duckdb_v2_table_function_filter_pushdown_info_handle info, duckdb_v2_context_handle,
                     duckdb_v2_error_info_handle *err) {
	void *user_ptr = nullptr;
	void *bind_ptr = nullptr;
	idx_t count = 0;
	if (duckdb_v2_table_function_filter_pushdown_get_user_data(info, &user_ptr, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_table_function_filter_pushdown_get_bind_data(info, &bind_ptr, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_table_function_filter_pushdown_get_filter_count(info, &count, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	claim_saw_user_data = user_ptr == &claim_user_data_marker;
	auto &bind = *static_cast<ClaimBind *>(bind_ptr);

	for (idx_t i = 0; i < count; i++) {
		duckdb_v2_expression_handle filter = nullptr;
		DUCKDB_V2_EXPRESSION_TYPE type = DUCKDB_V2_EXPRESSION_TYPE_INVALID;
		if (duckdb_v2_table_function_filter_pushdown_get_filter(info, i, &filter, err) != DUCKDB_V2_ERROR_NONE ||
		    duckdb_v2_expression_get_type(filter, &type, err) != DUCKDB_V2_ERROR_NONE) {
			return;
		}
		if (type != DUCKDB_V2_EXPRESSION_TYPE_COMPARE_LESSTHAN) {
			continue;
		}
		duckdb_v2_expression_handle left = nullptr;
		duckdb_v2_expression_handle right = nullptr;
		DUCKDB_V2_EXPRESSION_TYPE left_type = DUCKDB_V2_EXPRESSION_TYPE_INVALID;
		DUCKDB_V2_EXPRESSION_TYPE right_type = DUCKDB_V2_EXPRESSION_TYPE_INVALID;
		if (duckdb_v2_expression_get_child(filter, 0, &left, err) != DUCKDB_V2_ERROR_NONE ||
		    duckdb_v2_expression_get_child(filter, 1, &right, err) != DUCKDB_V2_ERROR_NONE ||
		    duckdb_v2_expression_get_type(left, &left_type, err) != DUCKDB_V2_ERROR_NONE ||
		    duckdb_v2_expression_get_type(right, &right_type, err) != DUCKDB_V2_ERROR_NONE) {
			return;
		}
		if (left_type != DUCKDB_V2_EXPRESSION_TYPE_BOUND_COLUMN_REF ||
		    right_type != DUCKDB_V2_EXPRESSION_TYPE_VALUE_CONSTANT) {
			continue;
		}
		idx_t column = 0;
		idx_t declared = 0;
		duckdb_v2_value_handle value = nullptr;
		if (duckdb_v2_expression_column_ref_get_index(left, &column, err) != DUCKDB_V2_ERROR_NONE ||
		    duckdb_v2_table_function_filter_pushdown_get_column_index(info, column, &declared, err) !=
		        DUCKDB_V2_ERROR_NONE ||
		    duckdb_v2_expression_constant_get_value(right, &value, err) != DUCKDB_V2_ERROR_NONE) {
			return;
		}
		int64_t bound = 0;
		auto rc = duckdb_v2_value_get_bigint(value, &bound, err);
		duckdb_v2_value_destroy(&value);
		if (rc != DUCKDB_V2_ERROR_NONE) {
			return;
		}
		if (declared != 0) {
			continue;
		}
		bind.bound = bound;
		if (duckdb_v2_table_function_filter_pushdown_accept(info, i, err) != DUCKDB_V2_ERROR_NONE) {
			return;
		}
	}
}

void ClaimInitGlobalCb(duckdb_v2_table_function_init_global_info_handle info, duckdb_v2_context_handle,
                       duckdb_v2_error_info_handle *err) {
	duckdb_v2_opaque state = {new ClaimGlobal {0}, DeleteClaimGlobal, nullptr};
	duckdb_v2_table_function_init_global_set_global_state(info, &state, err);
}

void ClaimExecCb(duckdb_v2_table_function_exec_info_handle info, duckdb_v2_context_handle,
                 duckdb_v2_error_info_handle *err) {
	void *bind_ptr = nullptr;
	void *global_ptr = nullptr;
	duckdb_v2_data_chunk_handle chunk = nullptr;
	if (duckdb_v2_table_function_exec_get_bind_data(info, &bind_ptr, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_table_function_exec_get_global_state(info, &global_ptr, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_table_function_exec_get_output_chunk(info, &chunk, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto &bind = *static_cast<ClaimBind *>(bind_ptr);
	auto &global = *static_cast<ClaimGlobal *>(global_ptr);

	duckdb_v2_vector_handle vec = nullptr;
	void *raw = nullptr;
	if (duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_vector_get_data_mutable(vec, &raw, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}

	// Two rows past a claimed bound: they surface only if the engine dropped the predicate.
	auto limit = bind.bound < 0 ? bind.count : bind.bound + 2;
	if (limit > bind.count) {
		limit = bind.count;
	}
	auto remaining = limit - global.position;
	auto produced =
	    remaining < static_cast<int64_t>(STANDARD_VECTOR_SIZE) ? remaining : static_cast<int64_t>(STANDARD_VECTOR_SIZE);
	if (produced < 0) {
		produced = 0;
	}
	auto *out = static_cast<int64_t *>(raw);
	for (int64_t i = 0; i < produced; i++) {
		out[i] = global.position + i;
	}
	global.position += produced;
	duckdb_v2_vector_set_size(vec, static_cast<idx_t>(produced), err);
}

void FailingPushdownCb(duckdb_v2_table_function_filter_pushdown_info_handle, duckdb_v2_context_handle,
                       duckdb_v2_error_info_handle *err) {
	SetErrorInfo(err, DUCKDB_V2_ERROR_INPUT_INVALID, "pushdown refused");
}

void RegisterClaim(EnvFixture &fx, const char *name, duckdb_v2_table_function_filter_pushdown_callback_fn pushdown) {
	auto bigint = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT);
	auto function = MakeTable(fx.conn, name);
	TableSigParam(SigOf(function), "n", bigint);
	duckdb_v2_opaque user_data = {&claim_user_data_marker, nullptr, nullptr};
	REQUIRE(duckdb_v2_table_function_set_user_data(function, &user_data, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_bind_callback(function, ClaimBindCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_init_global_callback(function, ClaimInitGlobalCb, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_exec_callback(function, ClaimExecCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_filter_pushdown_callback(function, pushdown, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_register_table_function(fx.conn, function, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_table_function_destroy(&function);
	duckdb_v2_logical_type_destroy(&bigint);
}

} // namespace

TEST_CASE("V2 table: projection pushdown narrows the output chunk", "[capi_v2][table_function]") {
	EnvFixture fx;
	RegisterProj(fx.conn, "proj_probe", true);
	RegisterProj(fx.conn, "proj_plain", false);

	using Cells = std::vector<int64_t>;
	using Columns = std::vector<idx_t>;

	// Two of three columns, in query order: the chunk holds exactly those, and the values prove the mapping.
	REQUIRE(QueryCells(fx.conn, "SELECT z, x FROM proj_probe()") == Cells {200, 0, 201, 1, 202, 2});
	REQUIRE(proj_global_columns == Columns {2, 0});
	REQUIRE(proj_local_columns == Columns {2, 0});

	// A query that needs no column at all still scans one.
	REQUIRE(QueryCells(fx.conn, "SELECT count(*) FROM proj_probe()") == Cells {3});
	REQUIRE(proj_global_columns.size() == 1);

	// Without projection pushdown the chunk always holds every declared column, in declaration order.
	REQUIRE(QueryCells(fx.conn, "SELECT z, x FROM proj_plain()") == Cells {200, 0, 201, 1, 202, 2});
	REQUIRE(proj_global_columns == Columns {0, 1, 2});
	REQUIRE(proj_local_columns == Columns {0, 1, 2});
}

TEST_CASE("V2 table: claimed filters are no longer applied by the engine", "[capi_v2][table_function]") {
	EnvFixture fx;
	RegisterClaim(fx, "claim_probe", ClaimPushdownCb);

	// Nothing to claim: every row comes through.
	claim_saw_user_data = false;
	REQUIRE(QueryI64(fx.conn, "SELECT count(*) FROM claim_probe(100) WHERE i % 2 = 0") == 50);
	REQUIRE(claim_saw_user_data);

	// The claimed bound plus the two rows the scan sneaks past it: the engine no longer filters them out.
	REQUIRE(QueryI64(fx.conn, "SELECT count(*) FROM claim_probe(100) WHERE i < 10") == 12);
	// The unclaimed half of the conjunction is still applied above the scan.
	REQUIRE(QueryI64(fx.conn, "SELECT count(*) FROM claim_probe(100) WHERE i < 10 AND i % 2 = 0") == 6);
	// A comparison the callback does not recognize stays with the engine.
	REQUIRE(QueryI64(fx.conn, "SELECT count(*) FROM claim_probe(100) WHERE i > 90") == 9);
}

TEST_CASE("V2 table: filter pushdown errors fail the query", "[capi_v2][table_function]") {
	EnvFixture fx;
	RegisterClaim(fx, "claim_fail", FailingPushdownCb);

	// Without a predicate there is nothing to offer, so the callback never runs.
	REQUIRE(QueryI64(fx.conn, "SELECT count(*) FROM claim_fail(5)") == 5);
	auto message = QueryError(fx.conn, "SELECT count(*) FROM claim_fail(5) WHERE i < 3");
	REQUIRE(message.find("pushdown refused") != std::string::npos);
}

TEST_CASE("V2 table: pushdown null arguments", "[capi_v2][table_function]") {
	idx_t count = 0;
	void *data = nullptr;
	REQUIRE(duckdb_v2_table_function_set_projection_pushdown(nullptr, true, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_table_function_set_filter_pushdown_callback(nullptr, ClaimPushdownCb, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_table_function_init_global_get_column_count(nullptr, &count, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_table_function_init_global_get_column_index(nullptr, 0, &count, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_table_function_init_local_get_column_count(nullptr, &count, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_table_function_init_local_get_column_index(nullptr, 0, &count, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_table_function_exec_get_column_count(nullptr, &count, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_table_function_exec_get_column_index(nullptr, 0, &count, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_table_function_filter_pushdown_get_user_data(nullptr, &data, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_table_function_filter_pushdown_get_bind_data(nullptr, &data, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
}

// ===========================================================================
// partition_data / partitioning.
// ===========================================================================

namespace {

// Runs sql (an EXPLAIN statement) and reports whether any VARCHAR cell of the output contains needle.
bool ExplainContains(duckdb_v2_connection_handle conn, const char *sql, const char *needle) {
	bool found = false;
	ForEachCell(conn, sql, [&](const duckdb_v2_vector_view &view, idx_t row) {
		auto bytes = static_cast<const duckdb_v2_bytes *>(view.data)[row];
		if (Convert(Convert(bytes)).find(needle) != std::string::npos) {
			found = true;
		}
	});
	return found;
}

// ---------------------------------------------------------------------------
// batch_order_probe(): one BIGINT column "b" over two batches raced so that batch 1 reaches the sink first;
// only partition_data lets the BATCH_INDEX_ORDERED result sink restore insertion order.
// ---------------------------------------------------------------------------

struct BatchOrderGlobal {
	std::atomic<int32_t> claimed {0};
	std::atomic<int32_t> started {0};
};
struct BatchOrderLocal {
	int32_t id = 0;
	bool done = false;
};

void DeleteBatchOrderGlobal(void *ptr) {
	delete static_cast<BatchOrderGlobal *>(ptr);
}
void DeleteBatchOrderLocal(void *ptr) {
	delete static_cast<BatchOrderLocal *>(ptr);
}

void BatchOrderBindCb(duckdb_v2_function_bind_info_handle info, duckdb_v2_table_function_bind_info_handle result,
                      duckdb_v2_context_handle context, duckdb_v2_error_info_handle *err) {
	auto bigint = MakeTypeInCallback(context, DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT, err);
	if (!bigint) {
		return;
	}
	auto name_str = TableIdent("b");
	duckdb_v2_table_function_bind_add_result_column(result, &name_str, bigint, err);
	duckdb_v2_logical_type_destroy(&bigint);
}

void BatchOrderInitGlobalCb(duckdb_v2_table_function_init_global_info_handle info, duckdb_v2_context_handle,
                            duckdb_v2_error_info_handle *err) {
	duckdb_v2_opaque state = {new BatchOrderGlobal {}, DeleteBatchOrderGlobal, nullptr};
	if (duckdb_v2_table_function_init_global_set_global_state(info, &state, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	duckdb_v2_table_function_init_global_set_max_threads(info, 2, err);
}

void BatchOrderInitLocalCb(duckdb_v2_table_function_init_local_info_handle info, duckdb_v2_context_handle,
                           duckdb_v2_error_info_handle *err) {
	void *global_ptr = nullptr;
	if (duckdb_v2_table_function_init_local_get_global_state(info, &global_ptr, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto &global = *static_cast<BatchOrderGlobal *>(global_ptr);
	auto id = global.claimed.fetch_add(1);
	global.started.fetch_add(1);
	// Bounded barrier: both local states must exist before either races ahead in exec, or the completion order
	// below would not be deterministic. Bounded so a scheduler that never launches the second task cannot hang.
	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (global.started.load() < 2 && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

	duckdb_v2_opaque state = {new BatchOrderLocal {id, false}, DeleteBatchOrderLocal, nullptr};
	duckdb_v2_table_function_init_local_set_local_state(info, &state, err);
}

void BatchOrderExecCb(duckdb_v2_table_function_exec_info_handle info, duckdb_v2_context_handle,
                      duckdb_v2_error_info_handle *err) {
	void *local_ptr = nullptr;
	duckdb_v2_data_chunk_handle chunk = nullptr;
	if (duckdb_v2_table_function_exec_get_local_state(info, &local_ptr, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_table_function_exec_get_output_chunk(info, &chunk, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto &local = *static_cast<BatchOrderLocal *>(local_ptr);
	duckdb_v2_vector_handle vec = nullptr;
	void *raw = nullptr;
	if (duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_vector_get_data_mutable(vec, &raw, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	if (local.done) {
		duckdb_v2_vector_set_size(vec, 0, err);
		return;
	}
	// Batch 0 finishes later than batch 1 despite being claimed first: only batch-index ordering restores order.
	if (local.id == 0) {
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	static_cast<int64_t *>(raw)[0] = local.id;
	local.done = true;
	duckdb_v2_vector_set_size(vec, 1, err);
}

// Set when any call observed requires_batch_index() true, so the ordered-result test can tell a real exercise of
// the callback apart from a vacuous pass on a scheduler that ran everything on one thread.
std::atomic<bool> batch_order_requires_batch_index_seen {false};

void BatchOrderGetPartitionDataCb(duckdb_v2_table_function_partition_data_info_handle info, duckdb_v2_context_handle,
                                  duckdb_v2_error_info_handle *err) {
	bool requires_batch_index = false;
	if (duckdb_v2_table_function_partition_data_requires_batch_index(info, &requires_batch_index, err) !=
	    DUCKDB_V2_ERROR_NONE) {
		return;
	}
	if (requires_batch_index) {
		batch_order_requires_batch_index_seen = true;
	}
	void *local_ptr = nullptr;
	if (duckdb_v2_table_function_partition_data_get_local_state(info, &local_ptr, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto &local = *static_cast<BatchOrderLocal *>(local_ptr);
	duckdb_v2_table_function_partition_data_set_batch_index(info, static_cast<idx_t>(local.id), err);
}

void RegisterBatchOrder(duckdb_v2_connection_handle conn, const char *name, bool with_partition_data) {
	auto function = MakeTable(conn, name);
	REQUIRE(duckdb_v2_table_function_set_bind_callback(function, BatchOrderBindCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_init_global_callback(function, BatchOrderInitGlobalCb, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_init_local_callback(function, BatchOrderInitLocalCb, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_exec_callback(function, BatchOrderExecCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	if (with_partition_data) {
		REQUIRE(duckdb_v2_table_function_set_partition_data_callback(function, BatchOrderGetPartitionDataCb, nullptr) ==
		        DUCKDB_V2_ERROR_NONE);
	}
	REQUIRE(duckdb_v2_connection_register_table_function(conn, function, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_table_function_destroy(&function);
}

// ---------------------------------------------------------------------------
// part_probe(n): BIGINT "part_col" and "val", n groups of 3 rows with one group per exec call, partitioned by
// "part_col" alone; its callbacks also probe the out-of-range setter paths along the way.
// ---------------------------------------------------------------------------

constexpr int64_t PART_PROBE_ROWS_PER_GROUP = 3;
constexpr idx_t PART_PROBE_PART_COL_INDEX = 0;

struct PartProbeBind {
	int64_t groups = 0;
};
struct PartProbeGlobal {
	int64_t position = 0;
	int64_t last_group = -1;
};

void DeletePartProbeBind(void *ptr) {
	delete static_cast<PartProbeBind *>(ptr);
}
void DeletePartProbeGlobal(void *ptr) {
	delete static_cast<PartProbeGlobal *>(ptr);
}

// Latched by the last call to each callback, for the out-of-range/type assertions.
std::atomic<DUCKDB_V2_ERROR> part_probe_oob_set_partition_value_rc {DUCKDB_V2_ERROR_NONE};
std::atomic<DUCKDB_V2_ERROR> part_probe_oob_set_partition_info_rc {DUCKDB_V2_ERROR_NONE};
std::atomic<DUCKDB_V2_ERROR> part_probe_oob_set_batch_index_rc {DUCKDB_V2_ERROR_NONE};
std::atomic<DUCKDB_V2_ERROR> part_probe_wrong_type_partition_value_rc {DUCKDB_V2_ERROR_NONE};

void PartProbeBindCb(duckdb_v2_function_bind_info_handle info, duckdb_v2_table_function_bind_info_handle result,
                     duckdb_v2_context_handle context, duckdb_v2_error_info_handle *err) {
	duckdb_v2_value_handle value = nullptr;
	if (duckdb_v2_function_bind_get_arg_value(info, 0, &value, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	int64_t groups = 0;
	auto rc = duckdb_v2_value_get_bigint(value, &groups, err);
	duckdb_v2_value_destroy(&value);
	if (rc != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto bigint = MakeTypeInCallback(context, DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT, err);
	if (!bigint) {
		return;
	}
	auto name_str = TableIdent("part_col");
	rc = duckdb_v2_table_function_bind_add_result_column(result, &name_str, bigint, err);
	if (rc == DUCKDB_V2_ERROR_NONE) {
		auto name_str2 = TableIdent("val");
		rc = duckdb_v2_table_function_bind_add_result_column(result, &name_str2, bigint, err);
	}
	duckdb_v2_logical_type_destroy(&bigint);
	if (rc != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	duckdb_v2_opaque bind_data = {new PartProbeBind {groups}, DeletePartProbeBind, nullptr};
	duckdb_v2_function_bind_set_bind_data(info, &bind_data, err);
}

void PartProbeInitGlobalCb(duckdb_v2_table_function_init_global_info_handle info, duckdb_v2_context_handle,
                           duckdb_v2_error_info_handle *err) {
	duckdb_v2_opaque state = {new PartProbeGlobal {}, DeletePartProbeGlobal, nullptr};
	duckdb_v2_table_function_init_global_set_global_state(info, &state, err);
}

void PartProbeExecCb(duckdb_v2_table_function_exec_info_handle info, duckdb_v2_context_handle,
                     duckdb_v2_error_info_handle *err) {
	void *bind_ptr = nullptr;
	void *global_ptr = nullptr;
	duckdb_v2_data_chunk_handle chunk = nullptr;
	if (duckdb_v2_table_function_exec_get_bind_data(info, &bind_ptr, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_table_function_exec_get_global_state(info, &global_ptr, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_table_function_exec_get_output_chunk(info, &chunk, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto &bind = *static_cast<PartProbeBind *>(bind_ptr);
	auto &global = *static_cast<PartProbeGlobal *>(global_ptr);

	duckdb_v2_vector_handle part_vec = nullptr;
	duckdb_v2_vector_handle val_vec = nullptr;
	void *part_raw = nullptr;
	void *val_raw = nullptr;
	if (duckdb_v2_data_chunk_get_vector(chunk, 0, &part_vec, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_data_chunk_get_vector(chunk, 1, &val_vec, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_vector_get_data_mutable(part_vec, &part_raw, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_vector_get_data_mutable(val_vec, &val_raw, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}

	if (global.position >= bind.groups * PART_PROBE_ROWS_PER_GROUP) {
		duckdb_v2_vector_set_size(part_vec, 0, err);
		return;
	}
	// A group spans several chunks when the vector size is smaller than a group.
	auto group = global.position / PART_PROBE_ROWS_PER_GROUP;
	auto offset = global.position % PART_PROBE_ROWS_PER_GROUP;
	auto rows = std::min<int64_t>(PART_PROBE_ROWS_PER_GROUP - offset, STANDARD_VECTOR_SIZE);
	global.last_group = group;
	for (int64_t i = 0; i < rows; i++) {
		static_cast<int64_t *>(part_raw)[i] = group;
		static_cast<int64_t *>(val_raw)[i] = group * 10 + offset + i;
	}
	global.position += rows;
	duckdb_v2_vector_set_size(part_vec, static_cast<idx_t>(rows), err);
}

void PartProbeGetPartitionDataCb(duckdb_v2_table_function_partition_data_info_handle info,
                                 duckdb_v2_context_handle context, duckdb_v2_error_info_handle *err) {
	duckdb_v2_factory_handle factory = nullptr;
	duckdb_v2_context_get_factory(context, &factory, nullptr);
	void *global_ptr = nullptr;
	if (duckdb_v2_table_function_partition_data_get_global_state(info, &global_ptr, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto &global = *static_cast<PartProbeGlobal *>(global_ptr);

	// Out of range: duckdb::PipelineBuildState::BATCH_INCREMENT - 2 is the first value the engine rejects. Latched,
	// not asserted here: a REQUIRE inside a callback would throw through the C callback boundary.
	part_probe_oob_set_batch_index_rc = duckdb_v2_table_function_partition_data_set_batch_index(
	    info, duckdb::PipelineBuildState::BATCH_INCREMENT - 2, nullptr);

	// A constant batch index would silently fold every group into the first partition: NextBatch no-ops otherwise.
	if (duckdb_v2_table_function_partition_data_set_batch_index(info, static_cast<idx_t>(global.last_group), err) !=
	    DUCKDB_V2_ERROR_NONE) {
		return;
	}
	bool needs_columns = false;
	if (duckdb_v2_table_function_partition_data_requires_partition_columns(info, &needs_columns, err) !=
	    DUCKDB_V2_ERROR_NONE) {
		return;
	}
	if (!needs_columns) {
		return;
	}
	idx_t column = 0;
	if (duckdb_v2_table_function_partition_data_get_partition_column_index(info, 0, &column, err) !=
	    DUCKDB_V2_ERROR_NONE) {
		return;
	}
	if (column != PART_PROBE_PART_COL_INDEX) {
		return;
	}

	duckdb_v2_value_handle wrong_type = nullptr;
	if (duckdb_v2_value_create_int(factory, static_cast<int32_t>(global.last_group), &wrong_type, nullptr) ==
	    DUCKDB_V2_ERROR_NONE) {
		part_probe_wrong_type_partition_value_rc =
		    duckdb_v2_table_function_partition_data_set_partition_value(info, 0, wrong_type, nullptr);
		duckdb_v2_value_destroy(&wrong_type);
	}

	duckdb_v2_value_handle value = nullptr;
	if (duckdb_v2_value_create_bigint(factory, global.last_group, &value, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	// Exactly one column is ever requested, so index 1 is always out of range.
	part_probe_oob_set_partition_value_rc =
	    duckdb_v2_table_function_partition_data_set_partition_value(info, 1, value, nullptr);
	duckdb_v2_table_function_partition_data_set_partition_value(info, 0, value, err);
	duckdb_v2_value_destroy(&value);
}

void PartProbeGetPartitionInfoCb(duckdb_v2_table_function_partitioning_info_handle info, duckdb_v2_context_handle,
                                 duckdb_v2_error_info_handle *err) {
	// DUCKDB_V2_TABLE_PARTITION_INFO only declares values 0-3.
	part_probe_oob_set_partition_info_rc = duckdb_v2_table_function_partitioning_set_partition_info(
	    info, static_cast<DUCKDB_V2_TABLE_PARTITION_INFO>(99), nullptr);

	idx_t count = 0;
	if (duckdb_v2_table_function_partitioning_get_partition_column_count(info, &count, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	if (count != 1) {
		return;
	}
	idx_t column = 0;
	if (duckdb_v2_table_function_partitioning_get_partition_column_index(info, 0, &column, err) !=
	    DUCKDB_V2_ERROR_NONE) {
		return;
	}
	if (column != PART_PROBE_PART_COL_INDEX) {
		return;
	}
	duckdb_v2_table_function_partitioning_set_partition_info(
	    info, DUCKDB_V2_TABLE_PARTITION_INFO_SINGLE_VALUE_PARTITIONS, err);
}

// Registers part_probe's bind/init_global/exec with the given partition callbacks.
void RegisterPartProbeVariant(EnvFixture &fx, const char *name,
                              duckdb_v2_table_function_partitioning_callback_fn info_cb,
                              duckdb_v2_table_function_partition_data_callback_fn data_cb) {
	auto bigint = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT);
	auto function = MakeTable(fx.conn, name);
	TableSigParam(SigOf(function), "n", bigint);
	REQUIRE(duckdb_v2_table_function_set_bind_callback(function, PartProbeBindCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_init_global_callback(function, PartProbeInitGlobalCb, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_exec_callback(function, PartProbeExecCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_partition_data_callback(function, data_cb, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_partitioning_callback(function, info_cb, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_register_table_function(fx.conn, function, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_table_function_destroy(&function);
	duckdb_v2_logical_type_destroy(&bigint);
}

void RegisterPartProbe(EnvFixture &fx, const char *name) {
	RegisterPartProbeVariant(fx, name, PartProbeGetPartitionInfoCb, PartProbeGetPartitionDataCb);
}

// Reports batch index 0, 1, 0: decreasing on the third group, to trip the "batch index must not decrease" guard.
void DecreasingBatchIndexDataCb(duckdb_v2_table_function_partition_data_info_handle info,
                                duckdb_v2_context_handle context, duckdb_v2_error_info_handle *err) {
	duckdb_v2_factory_handle factory = nullptr;
	duckdb_v2_context_get_factory(context, &factory, nullptr);
	void *global_ptr = nullptr;
	if (duckdb_v2_table_function_partition_data_get_global_state(info, &global_ptr, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto &global = *static_cast<PartProbeGlobal *>(global_ptr);
	idx_t reported_batch = global.last_group == 2 ? 0 : static_cast<idx_t>(global.last_group);
	if (duckdb_v2_table_function_partition_data_set_batch_index(info, reported_batch, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	bool needs_columns = false;
	if (duckdb_v2_table_function_partition_data_requires_partition_columns(info, &needs_columns, err) !=
	        DUCKDB_V2_ERROR_NONE ||
	    !needs_columns) {
		return;
	}
	duckdb_v2_value_handle value = nullptr;
	if (duckdb_v2_value_create_bigint(factory, global.last_group, &value, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	duckdb_v2_table_function_partition_data_set_partition_value(info, 0, value, err);
	duckdb_v2_value_destroy(&value);
}

// Reports batch index 0 for every group, while the partition value legitimately changes per group: trips the
// "value changed without the batch index changing" guard.
void ConstantBatchIndexDataCb(duckdb_v2_table_function_partition_data_info_handle info,
                              duckdb_v2_context_handle context, duckdb_v2_error_info_handle *err) {
	duckdb_v2_factory_handle factory = nullptr;
	duckdb_v2_context_get_factory(context, &factory, nullptr);
	void *global_ptr = nullptr;
	if (duckdb_v2_table_function_partition_data_get_global_state(info, &global_ptr, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto &global = *static_cast<PartProbeGlobal *>(global_ptr);
	if (duckdb_v2_table_function_partition_data_set_batch_index(info, 0, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	bool needs_columns = false;
	if (duckdb_v2_table_function_partition_data_requires_partition_columns(info, &needs_columns, err) !=
	        DUCKDB_V2_ERROR_NONE ||
	    !needs_columns) {
		return;
	}
	duckdb_v2_value_handle value = nullptr;
	if (duckdb_v2_value_create_bigint(factory, global.last_group, &value, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	duckdb_v2_table_function_partition_data_set_partition_value(info, 0, value, err);
	duckdb_v2_value_destroy(&value);
}

// ---------------------------------------------------------------------------
// Completeness and error-propagation failures on my_range, forced into a partitioned aggregate.
// ---------------------------------------------------------------------------

void AlwaysPartitionedInfoCb(duckdb_v2_table_function_partitioning_info_handle info, duckdb_v2_context_handle,
                             duckdb_v2_error_info_handle *err) {
	duckdb_v2_table_function_partitioning_set_partition_info(
	    info, DUCKDB_V2_TABLE_PARTITION_INFO_SINGLE_VALUE_PARTITIONS, err);
}

// Never reports a batch index: exercises the "batch index required, even when unrequested" completeness check.
void NoBatchIndexDataCb(duckdb_v2_table_function_partition_data_info_handle, duckdb_v2_context_handle,
                        duckdb_v2_error_info_handle *) {
}

// Reports the batch index but never the requested partitioning column value.
void NoPartitionValueDataCb(duckdb_v2_table_function_partition_data_info_handle info, duckdb_v2_context_handle,
                            duckdb_v2_error_info_handle *err) {
	duckdb_v2_table_function_partition_data_set_batch_index(info, 0, err);
}

void FailingGetPartitionDataCb(duckdb_v2_table_function_partition_data_info_handle, duckdb_v2_context_handle,
                               duckdb_v2_error_info_handle *err) {
	SetErrorInfo(err, DUCKDB_V2_ERROR_INPUT_INVALID, "partition data refused");
}

void FailingGetPartitionInfoCb(duckdb_v2_table_function_partitioning_info_handle, duckdb_v2_context_handle,
                               duckdb_v2_error_info_handle *err) {
	SetErrorInfo(err, DUCKDB_V2_ERROR_INPUT_INVALID, "partition info refused");
}

// ---------------------------------------------------------------------------
// proj_part_probe(n): part_probe's groups behind projection pushdown, with an INTEGER "pad" declared first so
// "part_col" (declared index 1) sits at scan position 0 whenever "pad" is pruned.
// ---------------------------------------------------------------------------

constexpr idx_t PROJ_PART_PROBE_PART_COL_INDEX = 1;

// Latched by partition_data: the declared column it was asked to report.
std::atomic<idx_t> proj_part_probe_reported_column {0};

void ProjPartProbeBindCb(duckdb_v2_function_bind_info_handle info, duckdb_v2_table_function_bind_info_handle result,
                         duckdb_v2_context_handle context, duckdb_v2_error_info_handle *err) {
	duckdb_v2_value_handle value = nullptr;
	if (duckdb_v2_function_bind_get_arg_value(info, 0, &value, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	int64_t groups = 0;
	auto rc = duckdb_v2_value_get_bigint(value, &groups, err);
	duckdb_v2_value_destroy(&value);
	if (rc != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto integer = MakeTypeInCallback(context, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER, err);
	if (!integer) {
		return;
	}
	auto bigint = MakeTypeInCallback(context, DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT, err);
	if (!bigint) {
		duckdb_v2_logical_type_destroy(&integer);
		return;
	}
	auto name_str = TableIdent("pad");
	rc = duckdb_v2_table_function_bind_add_result_column(result, &name_str, integer, err);
	if (rc == DUCKDB_V2_ERROR_NONE) {
		auto name_str2 = TableIdent("part_col");
		rc = duckdb_v2_table_function_bind_add_result_column(result, &name_str2, bigint, err);
	}
	if (rc == DUCKDB_V2_ERROR_NONE) {
		auto name_str3 = TableIdent("val");
		rc = duckdb_v2_table_function_bind_add_result_column(result, &name_str3, bigint, err);
	}
	duckdb_v2_logical_type_destroy(&integer);
	duckdb_v2_logical_type_destroy(&bigint);
	if (rc != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	duckdb_v2_opaque bind_data = {new PartProbeBind {groups}, DeletePartProbeBind, nullptr};
	duckdb_v2_function_bind_set_bind_data(info, &bind_data, err);
}

void ProjPartProbeExecCb(duckdb_v2_table_function_exec_info_handle info, duckdb_v2_context_handle,
                         duckdb_v2_error_info_handle *err) {
	void *bind_ptr = nullptr;
	void *global_ptr = nullptr;
	duckdb_v2_data_chunk_handle chunk = nullptr;
	idx_t count = 0;
	if (duckdb_v2_table_function_exec_get_bind_data(info, &bind_ptr, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_table_function_exec_get_global_state(info, &global_ptr, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_table_function_exec_get_output_chunk(info, &chunk, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_table_function_exec_get_column_count(info, &count, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto &bind = *static_cast<PartProbeBind *>(bind_ptr);
	auto &global = *static_cast<PartProbeGlobal *>(global_ptr);

	int64_t rows = 0;
	auto group = global.position / PART_PROBE_ROWS_PER_GROUP;
	auto offset = global.position % PART_PROBE_ROWS_PER_GROUP;
	if (global.position < bind.groups * PART_PROBE_ROWS_PER_GROUP) {
		rows = std::min<int64_t>(PART_PROBE_ROWS_PER_GROUP - offset, STANDARD_VECTOR_SIZE);
		global.last_group = group;
		global.position += rows;
	}
	for (idx_t i = 0; i < count; i++) {
		idx_t column = 0;
		duckdb_v2_vector_handle vec = nullptr;
		void *raw = nullptr;
		if (duckdb_v2_table_function_exec_get_column_index(info, i, &column, err) != DUCKDB_V2_ERROR_NONE ||
		    duckdb_v2_data_chunk_get_vector(chunk, i, &vec, err) != DUCKDB_V2_ERROR_NONE ||
		    duckdb_v2_vector_get_data_mutable(vec, &raw, err) != DUCKDB_V2_ERROR_NONE) {
			return;
		}
		for (int64_t row = 0; row < rows; row++) {
			if (column == 0) {
				static_cast<int32_t *>(raw)[row] = -1;
			} else if (column == PROJ_PART_PROBE_PART_COL_INDEX) {
				static_cast<int64_t *>(raw)[row] = group;
			} else {
				static_cast<int64_t *>(raw)[row] = group * 10 + offset + row;
			}
		}
		if (i == 0) {
			duckdb_v2_vector_set_size(vec, static_cast<idx_t>(rows), err);
		}
	}
}

void ProjPartProbeGetPartitionDataCb(duckdb_v2_table_function_partition_data_info_handle info,
                                     duckdb_v2_context_handle context, duckdb_v2_error_info_handle *err) {
	duckdb_v2_factory_handle factory = nullptr;
	duckdb_v2_context_get_factory(context, &factory, nullptr);
	void *global_ptr = nullptr;
	if (duckdb_v2_table_function_partition_data_get_global_state(info, &global_ptr, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto &global = *static_cast<PartProbeGlobal *>(global_ptr);
	if (duckdb_v2_table_function_partition_data_set_batch_index(info, static_cast<idx_t>(global.last_group), err) !=
	    DUCKDB_V2_ERROR_NONE) {
		return;
	}
	bool needs_columns = false;
	if (duckdb_v2_table_function_partition_data_requires_partition_columns(info, &needs_columns, err) !=
	        DUCKDB_V2_ERROR_NONE ||
	    !needs_columns) {
		return;
	}
	idx_t column = 0;
	if (duckdb_v2_table_function_partition_data_get_partition_column_index(info, 0, &column, err) !=
	    DUCKDB_V2_ERROR_NONE) {
		return;
	}
	proj_part_probe_reported_column = column;
	duckdb_v2_value_handle value = nullptr;
	if (duckdb_v2_value_create_bigint(factory, global.last_group, &value, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	duckdb_v2_table_function_partition_data_set_partition_value(info, 0, value, err);
	duckdb_v2_value_destroy(&value);
}

void ProjPartProbeGetPartitionInfoCb(duckdb_v2_table_function_partitioning_info_handle info, duckdb_v2_context_handle,
                                     duckdb_v2_error_info_handle *err) {
	idx_t count = 0;
	idx_t column = 0;
	if (duckdb_v2_table_function_partitioning_get_partition_column_count(info, &count, err) != DUCKDB_V2_ERROR_NONE ||
	    count != 1 ||
	    duckdb_v2_table_function_partitioning_get_partition_column_index(info, 0, &column, err) !=
	        DUCKDB_V2_ERROR_NONE ||
	    column != PROJ_PART_PROBE_PART_COL_INDEX) {
		return;
	}
	duckdb_v2_table_function_partitioning_set_partition_info(
	    info, DUCKDB_V2_TABLE_PARTITION_INFO_SINGLE_VALUE_PARTITIONS, err);
}

void RegisterProjPartProbe(EnvFixture &fx, const char *name) {
	auto bigint = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT);
	auto function = MakeTable(fx.conn, name);
	TableSigParam(SigOf(function), "n", bigint);
	REQUIRE(duckdb_v2_table_function_set_bind_callback(function, ProjPartProbeBindCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_init_global_callback(function, PartProbeInitGlobalCb, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_exec_callback(function, ProjPartProbeExecCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_projection_pushdown(function, true, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_partition_data_callback(function, ProjPartProbeGetPartitionDataCb, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_partitioning_callback(function, ProjPartProbeGetPartitionInfoCb, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_register_table_function(fx.conn, function, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_table_function_destroy(&function);
	duckdb_v2_logical_type_destroy(&bigint);
}

} // namespace

TEST_CASE("V2 table: partition_data reports declared columns under projection pushdown", "[capi_v2][table_function]") {
	EnvFixture fx;
	RegisterProjPartProbe(fx, "proj_part_probe");

	// Only "part_col" is scanned, so declared index 1 is scan position 0; reporting 0 would name the INTEGER "pad"
	// and the BIGINT partition value would be refused.
	REQUIRE(ExplainContains(fx.conn, "EXPLAIN SELECT part_col, count(*) FROM proj_part_probe(3) GROUP BY part_col",
	                        "Partitioned Aggregate"));
	auto cells =
	    QueryCells(fx.conn, "SELECT part_col, count(*) FROM proj_part_probe(3) GROUP BY part_col ORDER BY part_col");
	REQUIRE(cells == std::vector<int64_t> {0, 3, 1, 3, 2, 3});
	REQUIRE(proj_part_probe_reported_column.load() == PROJ_PART_PROBE_PART_COL_INDEX);

	// Two columns scanned, "part_col" still first among them.
	proj_part_probe_reported_column = 0;
	cells =
	    QueryCells(fx.conn, "SELECT part_col, max(val) FROM proj_part_probe(3) GROUP BY part_col ORDER BY part_col");
	REQUIRE(cells == std::vector<int64_t> {0, 2, 1, 12, 2, 22});
	REQUIRE(proj_part_probe_reported_column.load() == PROJ_PART_PROBE_PART_COL_INDEX);

	// Grouping by "val" is not a partitioning the probe claims, so the plain path still works under pushdown.
	REQUIRE_FALSE(ExplainContains(fx.conn, "EXPLAIN SELECT val, count(*) FROM proj_part_probe(3) GROUP BY val",
	                              "Partitioned Aggregate"));
	REQUIRE(QueryI64(fx.conn, "SELECT count(*) FROM (SELECT val FROM proj_part_probe(3) GROUP BY val)") == 9);
}

TEST_CASE("V2 table: partitioning requires partition_data", "[capi_v2][table_function]") {
	EnvFixture fx;
	auto bigint = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT);
	auto function = MakeTable(fx.conn, "info_only_probe");
	TableSigParam(SigOf(function), "n", bigint);
	REQUIRE(duckdb_v2_table_function_set_bind_callback(function, RangeBindCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_exec_callback(function, RangeExecCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_partitioning_callback(function, AlwaysPartitionedInfoCb, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_register_table_function(fx.conn, function, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	REQUIRE(duckdb_v2_table_function_set_partition_data_callback(function, NoBatchIndexDataCb, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_register_table_function(fx.conn, function, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_table_function_destroy(&function);
	duckdb_v2_logical_type_destroy(&bigint);
}

TEST_CASE("V2 table: partition_data restores batch order", "[capi_v2][table_function]") {
	EnvFixture fx;
	ExecSQL(fx.conn, "SET threads=2");
	RegisterBatchOrder(fx.conn, "batch_order_probe", true);
	RegisterBatchOrder(fx.conn, "batch_order_probe_nopart", false);

	batch_order_requires_batch_index_seen = false;
	REQUIRE(QueryCells(fx.conn, "SELECT * FROM batch_order_probe()") == std::vector<int64_t> {0, 1});
	// Otherwise the scheduler ran everything on one thread and never actually asked for a batch index, which would
	// make the ordering assertion above a vacuous pass rather than a real exercise of the callback.
	REQUIRE(batch_order_requires_batch_index_seen.load());

	// Without the callback the scan cannot support batch ordering, so the sink falls back to SOURCE_ORDERED; the
	// query still succeeds and produces the same rows, with no order guaranteed.
	REQUIRE(QueryI64(fx.conn, "SELECT count(*) FROM batch_order_probe_nopart()") == 2);
	REQUIRE(QueryI64(fx.conn, "SELECT sum(b)::BIGINT FROM batch_order_probe_nopart()") == 1);
}

TEST_CASE("V2 table: partition_data and partitioning feed a partitioned aggregate", "[capi_v2][table_function]") {
	EnvFixture fx;
	RegisterPartProbe(fx, "part_probe");

	// partitioning claims the requested column set only for "part_col": GROUP BY part_col unlocks the
	// partitioned aggregate, GROUP BY val does not.
	REQUIRE(ExplainContains(fx.conn, "EXPLAIN SELECT part_col, count(*) FROM part_probe(3) GROUP BY part_col",
	                        "Partitioned Aggregate"));
	REQUIRE_FALSE(ExplainContains(fx.conn, "EXPLAIN SELECT val, count(*) FROM part_probe(3) GROUP BY val",
	                              "Partitioned Aggregate"));

	auto cells =
	    QueryCells(fx.conn, "SELECT part_col, count(*) FROM part_probe(3) GROUP BY part_col ORDER BY part_col");
	REQUIRE(cells == std::vector<int64_t> {0, 3, 1, 3, 2, 3});

	// The out-of-range/wrong-type setter calls made along the way returned INPUT_INVALID synchronously, and did
	// not stop the correct calls right after them from keeping the query working.
	REQUIRE(part_probe_oob_set_partition_value_rc == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(part_probe_oob_set_partition_info_rc == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(part_probe_oob_set_batch_index_rc == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(part_probe_wrong_type_partition_value_rc == DUCKDB_V2_ERROR_INPUT_INVALID);
}

TEST_CASE("V2 table: partition_data batch index must not decrease on the same thread", "[capi_v2][table_function]") {
	EnvFixture fx;
	RegisterPartProbeVariant(fx, "decreasing_batch_probe", AlwaysPartitionedInfoCb, DecreasingBatchIndexDataCb);

	auto message = QueryError(fx.conn, "SELECT part_col, count(*) FROM decreasing_batch_probe(3) GROUP BY part_col");
	REQUIRE(message.find("decreasing_batch_probe") != std::string::npos);

	// The connection stays usable: the guard reports a clear, function-named error instead of the engine-internal
	// one PipelineExecutor::NextBatch would otherwise raise for the same decreasing index.
	REQUIRE(QueryI64(fx.conn, "SELECT 1::BIGINT") == 1);
}

TEST_CASE("V2 table: partition_data partition value must not change without the batch index",
          "[capi_v2][table_function]") {
	EnvFixture fx;
	RegisterPartProbeVariant(fx, "constant_batch_probe", AlwaysPartitionedInfoCb, ConstantBatchIndexDataCb);

	auto message = QueryError(fx.conn, "SELECT part_col, count(*) FROM constant_batch_probe(3) GROUP BY part_col");
	REQUIRE(message.find("constant_batch_probe") != std::string::npos);
}

TEST_CASE("V2 table: partition_data completeness and error failures", "[capi_v2][table_function]") {
	EnvFixture fx;
	RegisterRange(fx, "no_batch_index_probe", AlwaysPartitionedInfoCb, NoBatchIndexDataCb);
	RegisterRange(fx, "no_partition_value_probe", AlwaysPartitionedInfoCb, NoPartitionValueDataCb);
	RegisterRange(fx, "failing_partition_data_probe", AlwaysPartitionedInfoCb, FailingGetPartitionDataCb);

	auto missing_batch = QueryError(fx.conn, "SELECT i, count(*) FROM no_batch_index_probe(3) GROUP BY i");
	REQUIRE(missing_batch.find("no_batch_index_probe") != std::string::npos);
	REQUIRE(missing_batch.find("batch index") != std::string::npos);

	auto missing_value = QueryError(fx.conn, "SELECT i, count(*) FROM no_partition_value_probe(3) GROUP BY i");
	REQUIRE(missing_value.find("index 0") != std::string::npos);

	auto failed = QueryError(fx.conn, "SELECT i, count(*) FROM failing_partition_data_probe(3) GROUP BY i");
	REQUIRE(failed.find("partition data refused") != std::string::npos);
}

TEST_CASE("V2 table: partitioning failures only surface for a partitioned aggregate", "[capi_v2][table_function]") {
	EnvFixture fx;
	RegisterRange(fx, "failing_partition_info_probe", FailingGetPartitionInfoCb, NoBatchIndexDataCb);

	// The callback only runs while the optimizer is considering a partitioned aggregate for this scan; a plain
	// scan never reaches it.
	REQUIRE(QueryI64(fx.conn, "SELECT count(*) FROM failing_partition_info_probe(3)") == 3);

	auto message = QueryError(fx.conn, "SELECT i, count(*) FROM failing_partition_info_probe(3) GROUP BY i");
	REQUIRE(message.find("partition info refused") != std::string::npos);
}

TEST_CASE("V2 table: partition_data and partitioning null arguments", "[capi_v2][table_function]") {
	idx_t count = 0;
	idx_t index = 0;
	void *data = nullptr;
	bool flag = false;

	REQUIRE(duckdb_v2_table_function_set_partition_data_callback(nullptr, PartProbeGetPartitionDataCb, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_table_function_set_partitioning_callback(nullptr, PartProbeGetPartitionInfoCb, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);

	REQUIRE(duckdb_v2_table_function_partition_data_get_user_data(nullptr, &data, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_table_function_partition_data_get_bind_data(nullptr, &data, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_table_function_partition_data_get_global_state(nullptr, &data, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_table_function_partition_data_get_local_state(nullptr, &data, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_table_function_partition_data_requires_batch_index(nullptr, &flag, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_table_function_partition_data_requires_partition_columns(nullptr, &flag, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_table_function_partition_data_get_partition_column_count(nullptr, &count, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_table_function_partition_data_get_partition_column_index(nullptr, 0, &index, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_table_function_partition_data_set_batch_index(nullptr, 0, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_table_function_partition_data_set_partition_value(nullptr, 0, nullptr, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);

	REQUIRE(duckdb_v2_table_function_partitioning_get_user_data(nullptr, &data, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_table_function_partitioning_get_bind_data(nullptr, &data, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_table_function_partitioning_get_partition_column_count(nullptr, &count, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_table_function_partitioning_get_partition_column_index(nullptr, 0, &index, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_table_function_partitioning_set_partition_info(
	            nullptr, DUCKDB_V2_TABLE_PARTITION_INFO_NOT_PARTITIONED, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
}

namespace {

// ---------------------------------------------------------------------------
// my_kw(n, *, step := 1, **opts): latches the argument list the bind callback sees.
// ---------------------------------------------------------------------------

struct TableKindProbe {
	idx_t counts[4] = {};
	std::vector<std::string> names;
	// Owned; rendered and destroyed by the test, since rendering asserts
	std::vector<duckdb_v2_value_handle> values;
	std::vector<DUCKDB_V2_LOGICAL_TYPE_ID> types;
	bool n_found = false;
	idx_t n_index = 0;
	bool step_found = false;
	idx_t step_index = 0;
};
TableKindProbe table_kind_probe;

void KwargsBindCb(duckdb_v2_function_bind_info_handle info, duckdb_v2_table_function_bind_info_handle result,
                  duckdb_v2_context_handle context, duckdb_v2_error_info_handle *err) {
	for (auto &value : table_kind_probe.values) {
		duckdb_v2_value_destroy(&value);
	}
	table_kind_probe = TableKindProbe {};
	auto &counts = table_kind_probe.counts;
	auto n_ident = TableIdent("n");
	auto step_ident = TableIdent("step");
	if (duckdb_v2_function_bind_get_arg_count(info, &counts[0], &counts[1], &counts[2], &counts[3], err) !=
	        DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_function_bind_get_arg_index(info, &n_ident, &table_kind_probe.n_index, &table_kind_probe.n_found,
	                                          err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_function_bind_get_arg_index(info, &step_ident, &table_kind_probe.step_index,
	                                          &table_kind_probe.step_found, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	const auto arg_count = counts[0] + counts[1] + counts[2] + counts[3];
	for (idx_t i = 0; i < arg_count; i++) {
		duckdb_v2_identifier_t name {};
		duckdb_v2_logical_type_handle type = nullptr;
		duckdb_v2_value_handle value = nullptr;
		DUCKDB_V2_LOGICAL_TYPE_ID type_id = DUCKDB_V2_LOGICAL_TYPE_ID_INVALID;
		if (duckdb_v2_function_bind_get_arg_name(info, i, &name, err) != DUCKDB_V2_ERROR_NONE ||
		    duckdb_v2_function_bind_get_arg_type(info, i, &type, err) != DUCKDB_V2_ERROR_NONE) {
			return;
		}
		auto rc = duckdb_v2_logical_type_get_id(type, &type_id, err);
		duckdb_v2_logical_type_destroy(&type);
		if (rc != DUCKDB_V2_ERROR_NONE ||
		    duckdb_v2_function_bind_get_arg_value(info, i, &value, err) != DUCKDB_V2_ERROR_NONE) {
			return;
		}
		table_kind_probe.names.push_back(Convert(name));
		table_kind_probe.types.push_back(type_id);
		table_kind_probe.values.push_back(value);
	}

	auto bigint = MakeTypeInCallback(context, DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT, err);
	if (!bigint) {
		return;
	}
	auto rc = duckdb_v2_table_function_bind_add_result_column(result, &n_ident, bigint, err);
	duckdb_v2_logical_type_destroy(&bigint);
	if (rc != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	duckdb_v2_opaque bind_data = {new RangeBind {static_cast<int64_t>(arg_count)}, DeleteRangeBind, nullptr};
	duckdb_v2_function_bind_set_bind_data(info, &bind_data, err);
}

// Renders the latched argument values and releases them.
std::vector<std::string> TakeArgValues() {
	std::vector<std::string> result;
	for (auto &value : table_kind_probe.values) {
		result.push_back(Render(value));
		duckdb_v2_value_destroy(&value);
	}
	table_kind_probe.values.clear();
	return result;
}

std::vector<idx_t> TableArgCounts() {
	auto &counts = table_kind_probe.counts;
	return {counts[0], counts[1], counts[2], counts[3]};
}

} // namespace

// ===========================================================================
// Named-only parameters and "**kwargs" follow the positional arguments in the argument list.
// ===========================================================================

TEST_CASE("V2 table: named-only parameters and kwargs", "[capi_v2][table_function]") {
	EnvFixture fx;
	auto bigint = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT);
	auto any = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_ANY);
	auto one = MakeInt64Value(fx.factory, 1);

	auto function = MakeTable(fx.conn, "my_kw");
	auto sig = SigOf(function);
	TableSigParam(sig, "n", bigint);
	auto opts_ident = TableIdent("opts");
	REQUIRE(duckdb_v2_function_signature_add_parameter(sig, &opts_ident, any, nullptr,
	                                                   DUCKDB_V2_FUNCTION_PARAMETER_KIND_NAMED_VARIADIC,
	                                                   nullptr) == DUCKDB_V2_ERROR_NONE);
	auto step_ident = TableIdent("step");
	REQUIRE(duckdb_v2_function_signature_add_parameter(sig, &step_ident, bigint, one,
	                                                   DUCKDB_V2_FUNCTION_PARAMETER_KIND_NAMED_ONLY,
	                                                   nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_bind_callback(function, KwargsBindCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_exec_callback(function, ArgsExecCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_register_table_function(fx.conn, function, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_table_function_destroy(&function);
	duckdb_v2_value_destroy(&one);
	duckdb_v2_logical_type_destroy(&any);
	duckdb_v2_logical_type_destroy(&bigint);

	// The omitted named-only parameter carries its default. A standard parameter is found by name too.
	REQUIRE(QueryI64(fx.conn, "SELECT * FROM my_kw(3)") == 2);
	REQUIRE(TableArgCounts() == std::vector<idx_t> {1, 0, 1, 0});
	REQUIRE(table_kind_probe.names == std::vector<std::string> {"n", "step"});
	REQUIRE(TakeArgValues() == std::vector<std::string> {"3", "1"});
	REQUIRE(table_kind_probe.n_found);
	REQUIRE(table_kind_probe.n_index == 0);
	REQUIRE(table_kind_probe.step_found);
	REQUIRE(table_kind_probe.step_index == 1);

	// A standard parameter passed by name is at its declared position; unmatched names reach "**kwargs" in call
	// order, with the types they were passed with.
	REQUIRE(QueryI64(fx.conn, "SELECT * FROM my_kw(fmt => 'csv', n => 4, step => 5, level => 3)") == 4);
	REQUIRE(TableArgCounts() == std::vector<idx_t> {1, 0, 1, 2});
	REQUIRE(table_kind_probe.names == std::vector<std::string> {"n", "step", "fmt", "level"});
	REQUIRE(TakeArgValues() == std::vector<std::string> {"4", "5", "csv", "3"});
	REQUIRE(table_kind_probe.types[0] == DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT);
	REQUIRE(table_kind_probe.types[1] == DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT);
	REQUIRE(table_kind_probe.types[2] == DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR);
	REQUIRE(table_kind_probe.step_index == 1);

	// Without "*args", a surplus positional argument is rejected.
	duckdb_v2_result_handle result = nullptr;
	REQUIRE(Query(fx.conn, "SELECT * FROM my_kw(1, 2)", &result) != DUCKDB_V2_ERROR_NONE);
	duckdb_v2_result_destroy(&result);
}

} // namespace test_capi_v2
