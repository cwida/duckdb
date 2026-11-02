#include "duckdb_v2.h"
#include "test_capi_v2.hpp"

#include <atomic>
#include <cstring>
#include <string>

// ---------------------------------------------------------------------------
// V2 copy function tests: build a function on a connection, register it, and
// drive it through COPY ... TO and COPY ... FROM.
//
// Callbacks avoid Catch assertions: a REQUIRE would throw through the C
// callback boundary into the engine. On failure they populate the provided
// error slot and return; the failure then surfaces as a query error that the
// test asserts on. Cross-callback observations are latched into file-scope
// statics and asserted after the query. The batch and exec callbacks may run
// on several threads at once, so the latches they write are atomic.
// ---------------------------------------------------------------------------

namespace test_capi_v2 {

namespace {

// Create a copy function on the connection with the given name.
duckdb_v2_copy_function_handle MakeCopy(duckdb_v2_connection_handle conn, const char *name) {
	duckdb_v2_copy_function_handle function = nullptr;
	REQUIRE(duckdb_v2_copy_function_create(FactoryOf(conn), &function, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto str = Convert(name);
	REQUIRE(duckdb_v2_copy_function_set_name(function, &str, nullptr) == DUCKDB_V2_ERROR_NONE);
	return function;
}

// COPY the rows of `source` to `path` with the given format and extra options. The target never exists beforehand,
// so the engine writes it in place; USE_TMP_FILE is pinned anyway so a leftover file from an earlier run cannot
// change that.
std::string CopyToStatement(const std::string &source, const std::string &path, const char *format,
                            const std::string &options = "") {
	return "COPY (" + source + ") TO '" + path + "' (FORMAT " + format + ", USE_TMP_FILE FALSE" + options + ")";
}

std::string CopyFromStatement(const std::string &table, const std::string &path, const char *format,
                              const std::string &options = "") {
	return "COPY " + table + " FROM '" + path + "' (FORMAT " + format + options + ")";
}

// Runs a COPY statement and returns the row count it reports.
int64_t RunCopy(duckdb_v2_connection_handle conn, const std::string &sql) {
	duckdb_v2_result_handle result = nullptr;
	REQUIRE(Query(conn, sql.c_str(), &result) == DUCKDB_V2_ERROR_NONE);
	auto rows = DrainChangedRows(result);
	duckdb_v2_result_destroy(&result);
	return rows;
}

// Runs a statement expected to fail, returning the code it fails with: either straight from the execute call, or
// while stepping the result when the failure only surfaces during execution.
DUCKDB_V2_ERROR RunFailingCopy(duckdb_v2_connection_handle conn, const std::string &sql) {
	duckdb_v2_result_handle result = nullptr;
	auto rc = Query(conn, sql.c_str(), &result);
	if (rc != DUCKDB_V2_ERROR_NONE) {
		duckdb_v2_result_destroy(&result);
		return rc;
	}
	auto status = DUCKDB_V2_RESULT_STEP_STATUS_WAITING;
	for (int i = 0; i < 100000 && rc == DUCKDB_V2_ERROR_NONE && status != DUCKDB_V2_RESULT_STEP_STATUS_FINISHED; i++) {
		duckdb_v2_data_chunk_handle chunk = nullptr;
		rc = duckdb_v2_result_step(result, &chunk, &status, nullptr);
		if (chunk) {
			duckdb_v2_data_chunk_destroy(&chunk);
		}
		if (rc == DUCKDB_V2_ERROR_NONE && status == DUCKDB_V2_RESULT_STEP_STATUS_WAITING) {
			rc = duckdb_v2_result_wait(result, nullptr);
		}
	}
	duckdb_v2_result_destroy(&result);
	return rc;
}

// Runs a query producing a single BIGINT cell.
int64_t QueryI64(duckdb_v2_connection_handle conn, const std::string &sql) {
	duckdb_v2_result_handle result = nullptr;
	REQUIRE(Query(conn, sql.c_str(), &result) == DUCKDB_V2_ERROR_NONE);
	auto chunk = StepChunk(result);
	REQUIRE(chunk != nullptr);
	duckdb_v2_vector_handle vec = nullptr;
	duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr);
	duckdb_v2_vector_view view {};
	duckdb_v2_vector_get_view(vec, &view, nullptr);
	auto out = static_cast<const int64_t *>(view.data)[SelAt(view.sel, 0)];
	duckdb_v2_data_chunk_destroy(&chunk);
	duckdb_v2_result_destroy(&result);
	return out;
}

std::string Lower(std::string s) {
	for (auto &c : s) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return s;
}

// ---------------------------------------------------------------------------
// The COPY TO probe: every callback latches what it saw. The user, bind, init
// and batch data are file-scope markers with counting destructors.
// ---------------------------------------------------------------------------

int copy_user_marker = 0;
int copy_bind_marker = 0;
int copy_init_marker = 0;
int copy_batch_marker = 0;

enum ToPhase { TO_BIND = 0, TO_BATCH_SIZE, TO_INIT, TO_BATCH, TO_FLUSH, TO_FINALIZE, TO_PHASE_COUNT };

struct CopyToProbe {
	// What each phase's data accessors returned.
	std::atomic<void *> user_data[TO_PHASE_COUNT];
	std::atomic<void *> bind_data[TO_PHASE_COUNT];
	std::atomic<void *> init_data[TO_PHASE_COUNT];
	std::atomic<void *> batch_data_in_flush;
	std::atomic<idx_t> calls[TO_PHASE_COUNT];
	// Taking the batch a second time is refused.
	std::atomic<DUCKDB_V2_ERROR> second_take_rc;
	// Rows seen by the batch callback, summed over every batch.
	std::atomic<idx_t> rows;
	std::atomic<int> user_data_destroys;
	std::atomic<int> bind_data_destroys;
	std::atomic<int> init_data_destroys;
	std::atomic<int> batch_data_destroys;

	// Written by the bind and init callbacks only, which run once per statement.
	std::string bind_file_path;
	idx_t column_count = 0;
	std::string column_names[2];
	DUCKDB_V2_LOGICAL_TYPE_ID column_types[2] = {DUCKDB_V2_LOGICAL_TYPE_ID_INVALID, DUCKDB_V2_LOGICAL_TYPE_ID_INVALID};
	// Out-of-range probes, latched with their own (null) error slot.
	DUCKDB_V2_ERROR oob_type_rc = DUCKDB_V2_ERROR_NONE;
	DUCKDB_V2_ERROR oob_name_rc = DUCKDB_V2_ERROR_NONE;
	DUCKDB_V2_ERROR oob_option_rc = DUCKDB_V2_ERROR_NONE;
	// The statement's options as "name=rendered value" lines, in order.
	std::string options;
	std::string file_path;

	void Reset() {
		for (int phase = 0; phase < TO_PHASE_COUNT; phase++) {
			user_data[phase] = nullptr;
			bind_data[phase] = nullptr;
			init_data[phase] = nullptr;
			calls[phase] = 0;
		}
		batch_data_in_flush = nullptr;
		second_take_rc = DUCKDB_V2_ERROR_NONE;
		rows = 0;
		user_data_destroys = 0;
		bind_data_destroys = 0;
		init_data_destroys = 0;
		batch_data_destroys = 0;
		bind_file_path.clear();
		column_count = 0;
		column_names[0].clear();
		column_names[1].clear();
		column_types[0] = DUCKDB_V2_LOGICAL_TYPE_ID_INVALID;
		column_types[1] = DUCKDB_V2_LOGICAL_TYPE_ID_INVALID;
		oob_type_rc = DUCKDB_V2_ERROR_NONE;
		oob_name_rc = DUCKDB_V2_ERROR_NONE;
		oob_option_rc = DUCKDB_V2_ERROR_NONE;
		options.clear();
		file_path.clear();
	}
} copy_to_probe;

void DestroyUserData(void *) {
	copy_to_probe.user_data_destroys++;
}
void DestroyBindData(void *) {
	copy_to_probe.bind_data_destroys++;
}
void DestroyInitData(void *) {
	copy_to_probe.init_data_destroys++;
}
void DestroyBatchData(void *) {
	copy_to_probe.batch_data_destroys++;
}

// Renders an option value without Catch assertions; the caller owns the value.
std::string RenderOption(duckdb_v2_value_handle value) {
	idx_t len = 0;
	if (duckdb_v2_value_to_string(value, nullptr, 0, &len, nullptr) != DUCKDB_V2_ERROR_NONE) {
		return "<error>";
	}
	std::string out(len + 1, '\0');
	if (duckdb_v2_value_to_string(value, &out[0], out.size(), &len, nullptr) != DUCKDB_V2_ERROR_NONE) {
		return "<error>";
	}
	out.resize(len);
	return out;
}

void ProbeToBind(duckdb_v2_copy_to_bind_info_handle info, duckdb_v2_context_handle, duckdb_v2_error_info_handle *err) {
	copy_to_probe.calls[TO_BIND]++;
	void *user_data = nullptr;
	duckdb_v2_str path = {nullptr, 0};
	if (duckdb_v2_copy_to_bind_get_user_data(info, &user_data, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_copy_to_bind_get_file_path(info, &path, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	copy_to_probe.user_data[TO_BIND] = user_data;
	copy_to_probe.bind_file_path = Convert(path);

	if (duckdb_v2_copy_to_bind_get_column_count(info, &copy_to_probe.column_count, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	for (idx_t i = 0; i < copy_to_probe.column_count && i < 2; i++) {
		duckdb_v2_identifier_t name = {nullptr, 0};
		if (duckdb_v2_copy_to_bind_get_column_name(info, i, &name, err) != DUCKDB_V2_ERROR_NONE) {
			return;
		}
		copy_to_probe.column_names[i] = Convert(name);
		duckdb_v2_logical_type_handle type = nullptr;
		if (duckdb_v2_copy_to_bind_get_column_type(info, i, &type, err) != DUCKDB_V2_ERROR_NONE) {
			return;
		}
		auto rc = duckdb_v2_logical_type_get_id(type, &copy_to_probe.column_types[i], err);
		duckdb_v2_logical_type_destroy(&type);
		if (rc != DUCKDB_V2_ERROR_NONE) {
			return;
		}
	}

	idx_t option_count = 0;
	if (duckdb_v2_copy_to_bind_get_option_count(info, &option_count, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	for (idx_t i = 0; i < option_count; i++) {
		duckdb_v2_identifier_t name = {nullptr, 0};
		duckdb_v2_value_handle value = nullptr;
		if (duckdb_v2_copy_to_bind_get_option_name(info, i, &name, err) != DUCKDB_V2_ERROR_NONE ||
		    duckdb_v2_copy_to_bind_get_option_value(info, i, &value, err) != DUCKDB_V2_ERROR_NONE) {
			return;
		}
		copy_to_probe.options += Lower(Convert(name)) + "=" + RenderOption(value) + "\n";
		duckdb_v2_value_destroy(&value);
	}

	// An index past the last column or option is an input error.
	duckdb_v2_logical_type_handle oob_type = nullptr;
	duckdb_v2_identifier_t oob_name = {nullptr, 0};
	duckdb_v2_value_handle oob_value = nullptr;
	copy_to_probe.oob_type_rc =
	    duckdb_v2_copy_to_bind_get_column_type(info, copy_to_probe.column_count, &oob_type, nullptr);
	copy_to_probe.oob_name_rc =
	    duckdb_v2_copy_to_bind_get_column_name(info, copy_to_probe.column_count, &oob_name, nullptr);
	copy_to_probe.oob_option_rc = duckdb_v2_copy_to_bind_get_option_value(info, option_count, &oob_value, nullptr);

	duckdb_v2_opaque bind_data = {&copy_bind_marker, DestroyBindData, nullptr};
	duckdb_v2_copy_to_bind_set_bind_data(info, &bind_data, err);
}

// Asks for batches larger than any input in these tests, so a whole copy arrives as one batch per thread.
void ProbeToBatchSize(duckdb_v2_copy_to_batch_size_info_handle info, duckdb_v2_context_handle,
                      duckdb_v2_error_info_handle *err) {
	copy_to_probe.calls[TO_BATCH_SIZE]++;
	void *user_data = nullptr;
	void *bind_data = nullptr;
	if (duckdb_v2_copy_to_batch_size_get_user_data(info, &user_data, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_copy_to_batch_size_get_bind_data(info, &bind_data, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	copy_to_probe.user_data[TO_BATCH_SIZE] = user_data;
	copy_to_probe.bind_data[TO_BATCH_SIZE] = bind_data;
	duckdb_v2_copy_to_batch_size_set_target(info, 100000, err);
}

void ProbeToInit(duckdb_v2_copy_to_init_info_handle info, duckdb_v2_context_handle, duckdb_v2_error_info_handle *err) {
	copy_to_probe.calls[TO_INIT]++;
	void *user_data = nullptr;
	void *bind_data = nullptr;
	duckdb_v2_str path = {nullptr, 0};
	if (duckdb_v2_copy_to_init_get_user_data(info, &user_data, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_copy_to_init_get_bind_data(info, &bind_data, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_copy_to_init_get_file_path(info, &path, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	copy_to_probe.user_data[TO_INIT] = user_data;
	copy_to_probe.bind_data[TO_INIT] = bind_data;
	copy_to_probe.file_path = Convert(path);

	duckdb_v2_opaque init_data = {&copy_init_marker, DestroyInitData, nullptr};
	duckdb_v2_copy_to_init_set_init_data(info, &init_data, err);
}

void ProbeToBatch(duckdb_v2_copy_to_batch_info_handle info, duckdb_v2_context_handle,
                  duckdb_v2_error_info_handle *err) {
	copy_to_probe.calls[TO_BATCH]++;
	void *user_data = nullptr;
	void *bind_data = nullptr;
	void *init_data = nullptr;
	duckdb_v2_column_data_collection_handle input = nullptr;
	if (duckdb_v2_copy_to_batch_get_user_data(info, &user_data, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_copy_to_batch_get_bind_data(info, &bind_data, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_copy_to_batch_get_init_data(info, &init_data, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_copy_to_batch_take_input(info, &input, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	copy_to_probe.user_data[TO_BATCH] = user_data;
	copy_to_probe.bind_data[TO_BATCH] = bind_data;
	copy_to_probe.init_data[TO_BATCH] = init_data;

	// The batch is ours now: count its rows and release it. A second take has nothing left to hand out.
	idx_t count = 0;
	auto rc = duckdb_v2_column_data_collection_row_count(input, &count, err);
	duckdb_v2_column_data_collection_destroy(&input);
	if (rc != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	copy_to_probe.rows += count;
	duckdb_v2_column_data_collection_handle again = nullptr;
	copy_to_probe.second_take_rc = duckdb_v2_copy_to_batch_take_input(info, &again, nullptr);

	duckdb_v2_opaque batch_data = {&copy_batch_marker, DestroyBatchData, nullptr};
	duckdb_v2_copy_to_batch_set_batch_data(info, &batch_data, err);
}

void ProbeToFlush(duckdb_v2_copy_to_flush_info_handle info, duckdb_v2_context_handle,
                  duckdb_v2_error_info_handle *err) {
	copy_to_probe.calls[TO_FLUSH]++;
	void *user_data = nullptr;
	void *bind_data = nullptr;
	void *init_data = nullptr;
	void *batch_data = nullptr;
	if (duckdb_v2_copy_to_flush_get_user_data(info, &user_data, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_copy_to_flush_get_bind_data(info, &bind_data, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_copy_to_flush_get_init_data(info, &init_data, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_copy_to_flush_get_batch_data(info, &batch_data, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	copy_to_probe.user_data[TO_FLUSH] = user_data;
	copy_to_probe.bind_data[TO_FLUSH] = bind_data;
	copy_to_probe.init_data[TO_FLUSH] = init_data;
	copy_to_probe.batch_data_in_flush = batch_data;
}

void ProbeToFinalize(duckdb_v2_copy_to_finalize_info_handle info, duckdb_v2_context_handle,
                     duckdb_v2_error_info_handle *err) {
	copy_to_probe.calls[TO_FINALIZE]++;
	void *user_data = nullptr;
	void *bind_data = nullptr;
	void *init_data = nullptr;
	if (duckdb_v2_copy_to_finalize_get_user_data(info, &user_data, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_copy_to_finalize_get_bind_data(info, &bind_data, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_copy_to_finalize_get_init_data(info, &init_data, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	copy_to_probe.user_data[TO_FINALIZE] = user_data;
	copy_to_probe.bind_data[TO_FINALIZE] = bind_data;
	copy_to_probe.init_data[TO_FINALIZE] = init_data;
}

// Registers the COPY TO probe under the given name, with every callback and user data set, and destroys the
// handle. Without the batch size callback, the batch size is left to the statement.
void RegisterProbeCopyTo(duckdb_v2_connection_handle conn, const char *name = "probe_copy",
                         bool with_batch_size = true) {
	auto function = MakeCopy(conn, name);
	if (with_batch_size) {
		REQUIRE(duckdb_v2_copy_to_set_batch_size_callback(function, ProbeToBatchSize, nullptr) == DUCKDB_V2_ERROR_NONE);
	}
	duckdb_v2_opaque user_data = {&copy_user_marker, DestroyUserData, nullptr};
	REQUIRE(duckdb_v2_copy_function_set_user_data(function, &user_data, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_copy_to_set_bind_callback(function, ProbeToBind, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_copy_to_set_init_callback(function, ProbeToInit, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_copy_to_set_batch_callback(function, ProbeToBatch, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_copy_to_set_flush_callback(function, ProbeToFlush, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_copy_to_set_finalize_callback(function, ProbeToFinalize, nullptr) == DUCKDB_V2_ERROR_NONE);

	REQUIRE(duckdb_v2_connection_register_copy_function(conn, function, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_copy_function_destroy(&function) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(function == nullptr);
}

// Noop callbacks for registration-refusal tests.
void ToNoopBatch(duckdb_v2_copy_to_batch_info_handle, duckdb_v2_context_handle, duckdb_v2_error_info_handle *) {
}
void ToNoopFlush(duckdb_v2_copy_to_flush_info_handle, duckdb_v2_context_handle, duckdb_v2_error_info_handle *) {
}

// Fail the query through a callback's error slot.
void FailingToBind(duckdb_v2_copy_to_bind_info_handle, duckdb_v2_context_handle, duckdb_v2_error_info_handle *err) {
	duckdb_v2_error_info_set_code(*err, DUCKDB_V2_ERROR_IO_GENERAL);
	auto text_str = Convert("copy to bind failed on purpose");
	duckdb_v2_error_info_set_text(*err, &text_str);
}
void FailingToBatch(duckdb_v2_copy_to_batch_info_handle, duckdb_v2_context_handle, duckdb_v2_error_info_handle *err) {
	duckdb_v2_error_info_set_code(*err, DUCKDB_V2_ERROR_IO_GENERAL);
	auto text_str = Convert("copy to batch failed on purpose");
	duckdb_v2_error_info_set_text(*err, &text_str);
}
void FailingToBatchSize(duckdb_v2_copy_to_batch_size_info_handle, duckdb_v2_context_handle,
                        duckdb_v2_error_info_handle *err) {
	duckdb_v2_error_info_set_code(*err, DUCKDB_V2_ERROR_IO_GENERAL);
	auto text_str = Convert("copy to batch size failed on purpose");
	duckdb_v2_error_info_set_text(*err, &text_str);
}
// Leaves the target unset, which the engine refuses.
void EmptyToBatchSize(duckdb_v2_copy_to_batch_size_info_handle, duckdb_v2_context_handle,
                      duckdb_v2_error_info_handle *) {
}

// Sets the target to an invalid (0) value.
void ZeroToBatchSize(duckdb_v2_copy_to_batch_size_info_handle info, duckdb_v2_context_handle,
                     duckdb_v2_error_info_handle *) {
	duckdb_v2_copy_to_batch_size_set_target(info, 0, NULL);
}

// ---------------------------------------------------------------------------
// The COPY FROM probe: a reader producing (a BIGINT, b INTEGER) rows with a = i
// and b = 2 * i for i in [0, rows), where `rows` comes from the ROWS option.
// ---------------------------------------------------------------------------

enum FromPhase { FROM_BIND = 0, FROM_INIT_GLOBAL, FROM_INIT_LOCAL, FROM_EXEC, FROM_PROGRESS, FROM_PHASE_COUNT };

int reader_bind_marker = 0;
int reader_local_marker = 0;

struct ReaderGlobal {
	std::atomic<int64_t> position {0};
	int64_t rows = 0;
};

struct CopyFromProbe {
	std::atomic<void *> user_data[FROM_PHASE_COUNT];
	std::atomic<void *> bind_data[FROM_PHASE_COUNT];
	std::atomic<void *> global_state[FROM_PHASE_COUNT];
	std::atomic<void *> local_state_in_exec;
	std::atomic<idx_t> calls[FROM_PHASE_COUNT];
	std::atomic<int> bind_data_destroys;
	std::atomic<int> global_state_destroys;
	std::atomic<int> local_state_destroys;

	// Written by the bind callback only.
	std::string file_path;
	idx_t column_count = 0;
	std::string column_names[2];
	DUCKDB_V2_LOGICAL_TYPE_ID column_types[2] = {DUCKDB_V2_LOGICAL_TYPE_ID_INVALID, DUCKDB_V2_LOGICAL_TYPE_ID_INVALID};
	std::string options;
	int32_t rows = 0;

	void Reset() {
		for (int phase = 0; phase < FROM_PHASE_COUNT; phase++) {
			user_data[phase] = nullptr;
			bind_data[phase] = nullptr;
			global_state[phase] = nullptr;
			calls[phase] = 0;
		}
		local_state_in_exec = nullptr;
		bind_data_destroys = 0;
		global_state_destroys = 0;
		local_state_destroys = 0;
		file_path.clear();
		column_count = 0;
		column_names[0].clear();
		column_names[1].clear();
		column_types[0] = DUCKDB_V2_LOGICAL_TYPE_ID_INVALID;
		column_types[1] = DUCKDB_V2_LOGICAL_TYPE_ID_INVALID;
		options.clear();
		rows = 0;
	}
} copy_from_probe;

void DestroyReaderBindData(void *) {
	copy_from_probe.bind_data_destroys++;
}
void DestroyReaderGlobal(void *ptr) {
	copy_from_probe.global_state_destroys++;
	delete static_cast<ReaderGlobal *>(ptr);
}
void DestroyReaderLocal(void *) {
	copy_from_probe.local_state_destroys++;
}

void ProbeFromBind(duckdb_v2_copy_from_bind_info_handle info, duckdb_v2_context_handle,
                   duckdb_v2_error_info_handle *err) {
	copy_from_probe.calls[FROM_BIND]++;
	void *user_data = nullptr;
	duckdb_v2_str path = {nullptr, 0};
	if (duckdb_v2_copy_from_bind_get_user_data(info, &user_data, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_copy_from_bind_get_file_path(info, &path, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	copy_from_probe.user_data[FROM_BIND] = user_data;
	copy_from_probe.file_path = Convert(path);

	if (duckdb_v2_copy_from_bind_get_column_count(info, &copy_from_probe.column_count, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	for (idx_t i = 0; i < copy_from_probe.column_count && i < 2; i++) {
		duckdb_v2_identifier_t name = {nullptr, 0};
		if (duckdb_v2_copy_from_bind_get_column_name(info, i, &name, err) != DUCKDB_V2_ERROR_NONE) {
			return;
		}
		copy_from_probe.column_names[i] = Convert(name);
		duckdb_v2_logical_type_handle type = nullptr;
		if (duckdb_v2_copy_from_bind_get_column_type(info, i, &type, err) != DUCKDB_V2_ERROR_NONE) {
			return;
		}
		auto rc = duckdb_v2_logical_type_get_id(type, &copy_from_probe.column_types[i], err);
		duckdb_v2_logical_type_destroy(&type);
		if (rc != DUCKDB_V2_ERROR_NONE) {
			return;
		}
	}

	// The ROWS option decides how many rows to produce; everything else is latched as text.
	idx_t option_count = 0;
	if (duckdb_v2_copy_from_bind_get_option_count(info, &option_count, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	for (idx_t i = 0; i < option_count; i++) {
		duckdb_v2_identifier_t name = {nullptr, 0};
		duckdb_v2_value_handle value = nullptr;
		if (duckdb_v2_copy_from_bind_get_option_name(info, i, &name, err) != DUCKDB_V2_ERROR_NONE ||
		    duckdb_v2_copy_from_bind_get_option_value(info, i, &value, err) != DUCKDB_V2_ERROR_NONE) {
			return;
		}
		auto lower = Lower(Convert(name));
		copy_from_probe.options += lower + "=" + RenderOption(value) + "\n";
		auto rc = DUCKDB_V2_ERROR_NONE;
		if (lower == "rows") {
			rc = duckdb_v2_value_get_int(value, &copy_from_probe.rows, err);
		}
		duckdb_v2_value_destroy(&value);
		if (rc != DUCKDB_V2_ERROR_NONE) {
			return;
		}
	}

	duckdb_v2_copy_from_bind_set_cardinality(info, static_cast<idx_t>(copy_from_probe.rows), true, err);
	duckdb_v2_opaque bind_data = {&reader_bind_marker, DestroyReaderBindData, nullptr};
	duckdb_v2_copy_from_bind_set_bind_data(info, &bind_data, err);
}

void ProbeFromInitGlobal(duckdb_v2_copy_from_init_global_info_handle info, duckdb_v2_context_handle,
                         duckdb_v2_error_info_handle *err) {
	copy_from_probe.calls[FROM_INIT_GLOBAL]++;
	void *user_data = nullptr;
	void *bind_data = nullptr;
	if (duckdb_v2_copy_from_init_global_get_user_data(info, &user_data, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_copy_from_init_global_get_bind_data(info, &bind_data, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	copy_from_probe.user_data[FROM_INIT_GLOBAL] = user_data;
	copy_from_probe.bind_data[FROM_INIT_GLOBAL] = bind_data;

	auto global = new ReaderGlobal();
	global->rows = copy_from_probe.rows;
	duckdb_v2_opaque state = {global, DestroyReaderGlobal, nullptr};
	if (duckdb_v2_copy_from_init_global_set_global_state(info, &state, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	duckdb_v2_copy_from_init_global_set_max_threads(info, 2, err);
}

void ProbeFromInitLocal(duckdb_v2_copy_from_init_local_info_handle info, duckdb_v2_context_handle,
                        duckdb_v2_error_info_handle *err) {
	copy_from_probe.calls[FROM_INIT_LOCAL]++;
	void *user_data = nullptr;
	void *bind_data = nullptr;
	void *global_state = nullptr;
	if (duckdb_v2_copy_from_init_local_get_user_data(info, &user_data, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_copy_from_init_local_get_bind_data(info, &bind_data, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_copy_from_init_local_get_global_state(info, &global_state, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	copy_from_probe.user_data[FROM_INIT_LOCAL] = user_data;
	copy_from_probe.bind_data[FROM_INIT_LOCAL] = bind_data;
	copy_from_probe.global_state[FROM_INIT_LOCAL] = global_state;

	duckdb_v2_opaque state = {&reader_local_marker, DestroyReaderLocal, nullptr};
	duckdb_v2_copy_from_init_local_set_local_state(info, &state, err);
}

// How many rows a batch produces: within the smallest vector size the build can be configured with.
constexpr int64_t READER_BATCH_ROWS = 2;

void ProbeFromExec(duckdb_v2_copy_from_exec_info_handle info, duckdb_v2_context_handle,
                   duckdb_v2_error_info_handle *err) {
	copy_from_probe.calls[FROM_EXEC]++;
	void *user_data = nullptr;
	void *bind_data = nullptr;
	void *global_ptr = nullptr;
	void *local_state = nullptr;
	duckdb_v2_data_chunk_handle chunk = nullptr;
	if (duckdb_v2_copy_from_exec_get_user_data(info, &user_data, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_copy_from_exec_get_bind_data(info, &bind_data, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_copy_from_exec_get_global_state(info, &global_ptr, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_copy_from_exec_get_local_state(info, &local_state, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_copy_from_exec_get_output_chunk(info, &chunk, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	copy_from_probe.user_data[FROM_EXEC] = user_data;
	copy_from_probe.bind_data[FROM_EXEC] = bind_data;
	copy_from_probe.global_state[FROM_EXEC] = global_ptr;
	copy_from_probe.local_state_in_exec = local_state;
	auto &global = *static_cast<ReaderGlobal *>(global_ptr);

	duckdb_v2_vector_handle a = nullptr;
	duckdb_v2_vector_handle b = nullptr;
	void *raw_a = nullptr;
	void *raw_b = nullptr;
	if (duckdb_v2_data_chunk_get_vector(chunk, 0, &a, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_data_chunk_get_vector(chunk, 1, &b, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_vector_get_data_mutable(a, &raw_a, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_vector_get_data_mutable(b, &raw_b, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}

	// Claim the next slice of rows from the shared position; several threads may be reading.
	const auto start = global.position.fetch_add(READER_BATCH_ROWS);
	auto produced = global.rows - start;
	if (produced > READER_BATCH_ROWS) {
		produced = READER_BATCH_ROWS;
	}
	if (produced < 0) {
		produced = 0;
	}
	auto *out_a = static_cast<int64_t *>(raw_a);
	auto *out_b = static_cast<int32_t *>(raw_b);
	for (int64_t i = 0; i < produced; i++) {
		out_a[i] = start + i;
		out_b[i] = static_cast<int32_t>(2 * (start + i));
	}

	// The first vector's size is the batch's row count; 0 ends the read.
	duckdb_v2_vector_set_size(a, static_cast<idx_t>(produced), err);
}

void ProbeFromProgress(duckdb_v2_copy_from_progress_info_handle info, duckdb_v2_context_handle,
                       duckdb_v2_error_info_handle *err) {
	copy_from_probe.calls[FROM_PROGRESS]++;
	void *user_data = nullptr;
	void *bind_data = nullptr;
	void *global_state = nullptr;
	if (duckdb_v2_copy_from_progress_get_user_data(info, &user_data, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_copy_from_progress_get_bind_data(info, &bind_data, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_copy_from_progress_get_global_state(info, &global_state, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	copy_from_probe.user_data[FROM_PROGRESS] = user_data;
	copy_from_probe.bind_data[FROM_PROGRESS] = bind_data;
	copy_from_probe.global_state[FROM_PROGRESS] = global_state;
	duckdb_v2_copy_from_progress_set_progress(info, 0.5, err);
}

// Registers the COPY FROM probe under the given name, with every callback and user data set, and destroys the
// handle.
void RegisterProbeCopyFrom(duckdb_v2_connection_handle conn, const char *name = "probe_reader") {
	auto function = MakeCopy(conn, name);
	duckdb_v2_opaque user_data = {&copy_user_marker, DestroyUserData, nullptr};
	REQUIRE(duckdb_v2_copy_function_set_user_data(function, &user_data, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_copy_from_set_bind_callback(function, ProbeFromBind, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_copy_from_set_init_global_callback(function, ProbeFromInitGlobal, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_copy_from_set_init_local_callback(function, ProbeFromInitLocal, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_copy_from_set_exec_callback(function, ProbeFromExec, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_copy_from_set_progress_callback(function, ProbeFromProgress, nullptr) == DUCKDB_V2_ERROR_NONE);

	REQUIRE(duckdb_v2_connection_register_copy_function(conn, function, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_copy_function_destroy(&function) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(function == nullptr);
}

void FromNoopBind(duckdb_v2_copy_from_bind_info_handle, duckdb_v2_context_handle, duckdb_v2_error_info_handle *) {
}
void FromNoopExec(duckdb_v2_copy_from_exec_info_handle, duckdb_v2_context_handle, duckdb_v2_error_info_handle *) {
}
void FailingFromBind(duckdb_v2_copy_from_bind_info_handle, duckdb_v2_context_handle, duckdb_v2_error_info_handle *err) {
	duckdb_v2_error_info_set_code(*err, DUCKDB_V2_ERROR_IO_GENERAL);
	auto text_str = Convert("copy from bind failed on purpose");
	duckdb_v2_error_info_set_text(*err, &text_str);
}
void FailingFromExec(duckdb_v2_copy_from_exec_info_handle, duckdb_v2_context_handle, duckdb_v2_error_info_handle *err) {
	duckdb_v2_error_info_set_code(*err, DUCKDB_V2_ERROR_IO_GENERAL);
	auto text_str = Convert("copy from exec failed on purpose");
	duckdb_v2_error_info_set_text(*err, &text_str);
}

} // namespace

// ===========================================================================
// COPY TO: register on a connection and drive through COPY ... TO.
// ===========================================================================

TEST_CASE("V2 copy: COPY TO on a connection", "[capi_v2][copy_function]") {
	EnvFixture fx;
	RegisterProbeCopyTo(fx.conn);

	// More rows than one chunk, so the batch callback sees real batches. Two options of the function's own; the
	// engine's USE_TMP_FILE is consumed before bind and not reported.
	const auto path = duckdb::TestCreatePath("v2_copy_probe.out");
	copy_to_probe.Reset();
	REQUIRE(RunCopy(fx.conn, CopyToStatement("SELECT r AS a, r::VARCHAR AS b FROM range(5000) t(r)", path, "probe_copy",
	                                         ", TAG 'x', FLAG")) == 5000);

	// Bind saw the columns of the SELECT and the target path; an index past the columns is refused.
	REQUIRE(copy_to_probe.calls[TO_BIND].load() == 1);
	REQUIRE(copy_to_probe.bind_file_path == path);
	REQUIRE(copy_to_probe.column_count == 2);
	REQUIRE(copy_to_probe.column_names[0] == "a");
	REQUIRE(copy_to_probe.column_names[1] == "b");
	REQUIRE(copy_to_probe.column_types[0] == DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT);
	REQUIRE(copy_to_probe.column_types[1] == DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR);
	REQUIRE(copy_to_probe.oob_type_rc == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(copy_to_probe.oob_name_rc == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(copy_to_probe.oob_option_rc == DUCKDB_V2_ERROR_INPUT_INVALID);
	// Options arrive by name: a bare option reads as true.
	REQUIRE(copy_to_probe.options == "flag=true\ntag=x\n");

	// The batch size callback ran once, after bind.
	REQUIRE(copy_to_probe.calls[TO_BATCH_SIZE].load() == 1);

	// One file: init and finalize ran once, with the COPY target as the path.
	REQUIRE(copy_to_probe.calls[TO_INIT].load() == 1);
	REQUIRE(copy_to_probe.file_path == path);
	REQUIRE(copy_to_probe.calls[TO_FINALIZE].load() == 1);

	// Every row went through a batch, and every batch was flushed.
	REQUIRE(copy_to_probe.calls[TO_BATCH].load() >= 1);
	REQUIRE(copy_to_probe.calls[TO_FLUSH].load() == copy_to_probe.calls[TO_BATCH].load());
	REQUIRE(copy_to_probe.rows.load() == 5000);
	REQUIRE(copy_to_probe.second_take_rc.load() == DUCKDB_V2_ERROR_INPUT_INVALID);

	// The data slots reached every phase they are defined for.
	for (int phase = 0; phase < TO_PHASE_COUNT; phase++) {
		REQUIRE(copy_to_probe.user_data[phase].load() == &copy_user_marker);
	}
	for (int phase = TO_BATCH_SIZE; phase < TO_PHASE_COUNT; phase++) {
		REQUIRE(copy_to_probe.bind_data[phase].load() == &copy_bind_marker);
	}
	for (int phase = TO_BATCH; phase < TO_PHASE_COUNT; phase++) {
		REQUIRE(copy_to_probe.init_data[phase].load() == &copy_init_marker);
	}
	REQUIRE(copy_to_probe.batch_data_in_flush.load() == &copy_batch_marker);

	// Each batch's data was released after its flush, and the file's init data and the statement's bind data with
	// the statement.
	REQUIRE(copy_to_probe.batch_data_destroys.load() == copy_to_probe.calls[TO_BATCH].load());
	REQUIRE(copy_to_probe.init_data_destroys.load() == 1);
	REQUIRE(copy_to_probe.bind_data_destroys.load() == 1);
	// The user data lives with the registered function.
	REQUIRE(copy_to_probe.user_data_destroys.load() == 0);
}

// ===========================================================================
// The batch size callback decides where batches are cut when the statement sets no BATCH_SIZE.
// ===========================================================================

// The batch counts below follow from the default chunk size, so the test only holds for the default vector size.
#if (STANDARD_VECTOR_SIZE == DEFAULT_STANDARD_VECTOR_SIZE)
TEST_CASE("V2 copy: COPY TO batch size callback", "[capi_v2][copy_function]") {
	EnvFixture fx;
	// One sink thread, so the batch count follows from the chunk sizes alone.
	ExecSQL(fx.conn, "SET threads = 1");
	RegisterProbeCopyTo(fx.conn, "chunked_copy", false);
	RegisterProbeCopyTo(fx.conn, "whole_copy", true);

	// Without a batch size every sunk chunk is a batch: 2048, 2048 and 904 rows.
	const auto path = duckdb::TestCreatePath("v2_copy_batch_size.out");
	copy_to_probe.Reset();
	REQUIRE(RunCopy(fx.conn, CopyToStatement("SELECT r FROM range(5000) t(r)", path, "chunked_copy")) == 5000);
	REQUIRE(copy_to_probe.calls[TO_BATCH_SIZE].load() == 0);
	REQUIRE(copy_to_probe.calls[TO_BATCH].load() == 3);
	REQUIRE(copy_to_probe.rows.load() == 5000);

	// The callback asks for batches larger than the input: everything arrives as one batch.
	copy_to_probe.Reset();
	REQUIRE(RunCopy(fx.conn, CopyToStatement("SELECT r FROM range(5000) t(r)", path, "whole_copy")) == 5000);
	REQUIRE(copy_to_probe.calls[TO_BATCH_SIZE].load() == 1);
	REQUIRE(copy_to_probe.calls[TO_BATCH].load() == 1);
	REQUIRE(copy_to_probe.rows.load() == 5000);

	// The statement's own BATCH_SIZE wins, and the callback is not consulted.
	copy_to_probe.Reset();
	REQUIRE(RunCopy(fx.conn, CopyToStatement("SELECT r FROM range(5000) t(r)", path, "whole_copy",
	                                         ", BATCH_SIZE 4096")) == 5000);
	REQUIRE(copy_to_probe.calls[TO_BATCH_SIZE].load() == 0);
	REQUIRE(copy_to_probe.calls[TO_BATCH].load() == 2);
	REQUIRE(copy_to_probe.rows.load() == 5000);
}
#endif

// ===========================================================================
// Only the batch and flush callbacks are required on the COPY TO side; the others leave their slots empty.
// ===========================================================================

TEST_CASE("V2 copy: COPY TO optional callbacks and empty data slots", "[capi_v2][copy_function]") {
	EnvFixture fx;

	auto function = MakeCopy(fx.conn, "bare_copy");
	REQUIRE(duckdb_v2_copy_to_set_batch_callback(function, ProbeToBatch, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_copy_to_set_flush_callback(function, ProbeToFlush, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_register_copy_function(fx.conn, function, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_copy_function_destroy(&function);

	const auto path = duckdb::TestCreatePath("v2_copy_bare.out");
	copy_to_probe.Reset();
	REQUIRE(RunCopy(fx.conn, CopyToStatement("SELECT r FROM range(10) t(r)", path, "bare_copy")) == 10);

	REQUIRE(copy_to_probe.calls[TO_BIND].load() == 0);
	REQUIRE(copy_to_probe.calls[TO_INIT].load() == 0);
	REQUIRE(copy_to_probe.calls[TO_FINALIZE].load() == 0);
	REQUIRE(copy_to_probe.calls[TO_BATCH].load() >= 1);
	REQUIRE(copy_to_probe.calls[TO_FLUSH].load() == copy_to_probe.calls[TO_BATCH].load());
	REQUIRE(copy_to_probe.rows.load() == 10);

	// Nothing was set, so every slot reads as null.
	REQUIRE(copy_to_probe.user_data[TO_BATCH].load() == nullptr);
	REQUIRE(copy_to_probe.bind_data[TO_BATCH].load() == nullptr);
	REQUIRE(copy_to_probe.init_data[TO_BATCH].load() == nullptr);
	REQUIRE(copy_to_probe.user_data[TO_FLUSH].load() == nullptr);
	REQUIRE(copy_to_probe.bind_data[TO_FLUSH].load() == nullptr);
	REQUIRE(copy_to_probe.init_data[TO_FLUSH].load() == nullptr);
	// The batch data was set by the batch callback, so it did reach the flush.
	REQUIRE(copy_to_probe.batch_data_in_flush.load() == &copy_batch_marker);
	REQUIRE(copy_to_probe.batch_data_destroys.load() == copy_to_probe.calls[TO_BATCH].load());

	// A batch callback that never takes its input leaves the engine to release it.
	auto untaken = MakeCopy(fx.conn, "untaken_copy");
	REQUIRE(duckdb_v2_copy_to_set_batch_callback(untaken, ToNoopBatch, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_copy_to_set_flush_callback(untaken, ToNoopFlush, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_register_copy_function(fx.conn, untaken, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_copy_function_destroy(&untaken);
	REQUIRE(RunCopy(fx.conn, CopyToStatement("SELECT r FROM range(10) t(r)", path, "untaken_copy")) == 10);

	// A COPY TO only function cannot be read from.
	ExecSQL(fx.conn, "CREATE TABLE t(a BIGINT)");
	REQUIRE(RunFailingCopy(fx.conn, CopyFromStatement("t", "nowhere", "untaken_copy")) ==
	        DUCKDB_V2_ERROR_QUERY_NOT_IMPLEMENTED);
}

// ===========================================================================
// COPY FROM: register on a connection and drive through COPY ... FROM.
// ===========================================================================

TEST_CASE("V2 copy: COPY FROM on a connection", "[capi_v2][copy_function]") {
	EnvFixture fx;
	RegisterProbeCopyFrom(fx.conn);
	ExecSQL(fx.conn, "CREATE TABLE target(a BIGINT, b INTEGER)");

	// ROWS drives the reader; TAG and FLAG are along for the ride, and a parenthesized list arrives as a tuple.
	copy_from_probe.Reset();
	REQUIRE(RunCopy(fx.conn, CopyFromStatement("target", "some/where.probe", "probe_reader",
	                                           ", ROWS 5000, TAG 'x', FLAG, PAIR (1, 'two')")) == 5000);

	// Bind saw the target table's columns, the path as written, and the options by name.
	REQUIRE(copy_from_probe.calls[FROM_BIND].load() == 1);
	REQUIRE(copy_from_probe.file_path == "some/where.probe");
	REQUIRE(copy_from_probe.column_count == 2);
	REQUIRE(copy_from_probe.column_names[0] == "a");
	REQUIRE(copy_from_probe.column_names[1] == "b");
	REQUIRE(copy_from_probe.column_types[0] == DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT);
	REQUIRE(copy_from_probe.column_types[1] == DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	REQUIRE(copy_from_probe.options == "flag=true\npair=(1, two)\nrows=5000\ntag=x\n");

	// The read ran with the requested state and produced the rows into the table.
	REQUIRE(copy_from_probe.calls[FROM_INIT_GLOBAL].load() == 1);
	REQUIRE(copy_from_probe.calls[FROM_INIT_LOCAL].load() >= 1);
	REQUIRE(copy_from_probe.calls[FROM_EXEC].load() >= 1);
	REQUIRE(QueryI64(fx.conn, "SELECT count(*) FROM target") == 5000);
	REQUIRE(QueryI64(fx.conn, "SELECT sum(a) FROM target") == 5000LL * 4999 / 2);
	REQUIRE(QueryI64(fx.conn, "SELECT sum(b)::BIGINT FROM target") == 5000LL * 4999);

	// The data slots reached every phase they are defined for; progress only runs when the engine asks for it.
	for (int phase = 0; phase < FROM_PHASE_COUNT; phase++) {
		if (copy_from_probe.calls[phase].load() > 0) {
			REQUIRE(copy_from_probe.user_data[phase].load() == &copy_user_marker);
		}
	}
	for (int phase = FROM_INIT_GLOBAL; phase < FROM_PHASE_COUNT; phase++) {
		if (copy_from_probe.calls[phase].load() > 0) {
			REQUIRE(copy_from_probe.bind_data[phase].load() == &reader_bind_marker);
		}
	}
	REQUIRE(copy_from_probe.global_state[FROM_INIT_LOCAL].load() != nullptr);
	REQUIRE(copy_from_probe.global_state[FROM_EXEC].load() == copy_from_probe.global_state[FROM_INIT_LOCAL].load());
	REQUIRE(copy_from_probe.local_state_in_exec.load() == &reader_local_marker);

	// The states were released with the read, the bind data with the statement.
	REQUIRE(copy_from_probe.global_state_destroys.load() == 1);
	REQUIRE(copy_from_probe.local_state_destroys.load() == copy_from_probe.calls[FROM_INIT_LOCAL].load());
	REQUIRE(copy_from_probe.bind_data_destroys.load() == 1);

	// A COPY FROM only function cannot be written to.
	REQUIRE(RunFailingCopy(fx.conn, CopyToStatement("SELECT 1", "nowhere", "probe_reader")) ==
	        DUCKDB_V2_ERROR_QUERY_NOT_IMPLEMENTED);
}

// ===========================================================================
// Errors
// ===========================================================================

// An error set in a callback's slot fails the statement with its code.
TEST_CASE("V2 copy: callback errors propagate to the result", "[capi_v2][copy_function]") {
	EnvFixture fx;
	ExecSQL(fx.conn, "CREATE TABLE target(a BIGINT)");

	// Registers a COPY TO function with the given bind, batch size and batch callbacks.
	auto register_to = [&](const char *name, duckdb_v2_copy_to_bind_callback_fn bind,
	                       duckdb_v2_copy_to_batch_size_callback_fn batch_size,
	                       duckdb_v2_copy_to_batch_callback_fn batch) {
		auto function = MakeCopy(fx.conn, name);
		if (bind) {
			REQUIRE(duckdb_v2_copy_to_set_bind_callback(function, bind, nullptr) == DUCKDB_V2_ERROR_NONE);
		}
		if (batch_size) {
			REQUIRE(duckdb_v2_copy_to_set_batch_size_callback(function, batch_size, nullptr) == DUCKDB_V2_ERROR_NONE);
		}
		REQUIRE(duckdb_v2_copy_to_set_batch_callback(function, batch, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(duckdb_v2_copy_to_set_flush_callback(function, ToNoopFlush, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(duckdb_v2_connection_register_copy_function(fx.conn, function, nullptr) == DUCKDB_V2_ERROR_NONE);
		duckdb_v2_copy_function_destroy(&function);
	};
	register_to("to_batch_fails", nullptr, nullptr, FailingToBatch);
	register_to("to_bind_fails", FailingToBind, nullptr, ToNoopBatch);
	register_to("to_batch_size_fails", nullptr, FailingToBatchSize, ToNoopBatch);
	register_to("to_batch_size_zero", nullptr, ZeroToBatchSize, ToNoopBatch);
	register_to("to_batch_size_empty", nullptr, EmptyToBatchSize, ToNoopBatch);

	// Registers a COPY FROM function with the given bind and exec callbacks.
	auto register_from = [&](const char *name, duckdb_v2_copy_from_bind_callback_fn bind,
	                         duckdb_v2_copy_from_exec_callback_fn exec) {
		auto function = MakeCopy(fx.conn, name);
		REQUIRE(duckdb_v2_copy_from_set_bind_callback(function, bind, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(duckdb_v2_copy_from_set_exec_callback(function, exec, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(duckdb_v2_connection_register_copy_function(fx.conn, function, nullptr) == DUCKDB_V2_ERROR_NONE);
		duckdb_v2_copy_function_destroy(&function);
	};
	register_from("from_bind_fails", FailingFromBind, FromNoopExec);
	register_from("from_exec_fails", FromNoopBind, FailingFromExec);

	// The callback's code round-trips through the engine's exception machinery.
	const auto path = duckdb::TestCreatePath("v2_copy_fails.out");
	const std::string source = "SELECT r FROM range(10) t(r)";
	REQUIRE(RunFailingCopy(fx.conn, CopyToStatement(source, path, "to_batch_fails")) == DUCKDB_V2_ERROR_IO_GENERAL);
	REQUIRE(RunFailingCopy(fx.conn, CopyToStatement(source, path, "to_bind_fails")) == DUCKDB_V2_ERROR_IO_GENERAL);
	REQUIRE(RunFailingCopy(fx.conn, CopyToStatement(source, path, "to_batch_size_fails")) ==
	        DUCKDB_V2_ERROR_IO_GENERAL);
	// A batch size of 0 should fail.
	REQUIRE(RunFailingCopy(fx.conn, CopyToStatement(source, path, "to_batch_size_zero")) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(RunFailingCopy(fx.conn, CopyFromStatement("target", path, "from_bind_fails")) ==
	        DUCKDB_V2_ERROR_IO_GENERAL);
	REQUIRE(RunFailingCopy(fx.conn, CopyFromStatement("target", path, "from_exec_fails")) ==
	        DUCKDB_V2_ERROR_IO_GENERAL);

	// A batch size callback that sets no target does not fail.
	REQUIRE(RunFailingCopy(fx.conn, CopyToStatement(source, path, "to_batch_size_empty")) == DUCKDB_V2_ERROR_NONE);
}

TEST_CASE("V2 copy: registration refusals", "[capi_v2][copy_function]") {
	EnvFixture fx;

	// No name.
	{
		duckdb_v2_copy_function_handle function = nullptr;
		REQUIRE(duckdb_v2_copy_function_create(fx.factory, &function, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(duckdb_v2_copy_to_set_batch_callback(function, ToNoopBatch, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(duckdb_v2_copy_to_set_flush_callback(function, ToNoopFlush, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(duckdb_v2_connection_register_copy_function(fx.conn, function, nullptr) ==
		        DUCKDB_V2_ERROR_INPUT_INVALID);
		duckdb_v2_copy_function_destroy(&function);
	}

	// Neither side set.
	{
		auto function = MakeCopy(fx.conn, "copy_no_side");
		REQUIRE(duckdb_v2_connection_register_copy_function(fx.conn, function, nullptr) ==
		        DUCKDB_V2_ERROR_INPUT_INVALID);
		duckdb_v2_copy_function_destroy(&function);
	}

	// A COPY TO side without its batch, then without its flush callback.
	{
		auto function = MakeCopy(fx.conn, "copy_to_no_batch");
		REQUIRE(duckdb_v2_copy_to_set_flush_callback(function, ToNoopFlush, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(duckdb_v2_connection_register_copy_function(fx.conn, function, nullptr) ==
		        DUCKDB_V2_ERROR_INPUT_INVALID);
		duckdb_v2_copy_function_destroy(&function);
	}
	{
		auto function = MakeCopy(fx.conn, "copy_to_no_flush");
		REQUIRE(duckdb_v2_copy_to_set_batch_callback(function, ToNoopBatch, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(duckdb_v2_connection_register_copy_function(fx.conn, function, nullptr) ==
		        DUCKDB_V2_ERROR_INPUT_INVALID);
		duckdb_v2_copy_function_destroy(&function);
	}

	// A COPY FROM side without its bind, then without its exec callback.
	{
		auto function = MakeCopy(fx.conn, "copy_from_no_bind");
		REQUIRE(duckdb_v2_copy_from_set_exec_callback(function, FromNoopExec, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(duckdb_v2_connection_register_copy_function(fx.conn, function, nullptr) ==
		        DUCKDB_V2_ERROR_INPUT_INVALID);
		duckdb_v2_copy_function_destroy(&function);
	}
	{
		auto function = MakeCopy(fx.conn, "copy_from_no_exec");
		REQUIRE(duckdb_v2_copy_from_set_bind_callback(function, FromNoopBind, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(duckdb_v2_connection_register_copy_function(fx.conn, function, nullptr) ==
		        DUCKDB_V2_ERROR_INPUT_INVALID);
		duckdb_v2_copy_function_destroy(&function);
	}

	// A complete COPY TO side alongside an incomplete COPY FROM side is still refused.
	{
		auto function = MakeCopy(fx.conn, "copy_mixed");
		REQUIRE(duckdb_v2_copy_to_set_batch_callback(function, ToNoopBatch, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(duckdb_v2_copy_to_set_flush_callback(function, ToNoopFlush, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(duckdb_v2_copy_from_set_bind_callback(function, FromNoopBind, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(duckdb_v2_connection_register_copy_function(fx.conn, function, nullptr) ==
		        DUCKDB_V2_ERROR_INPUT_INVALID);
		duckdb_v2_copy_function_destroy(&function);
	}
}

TEST_CASE("V2 copy: null arguments and destroy null-safety", "[capi_v2][copy_function]") {
	EnvFixture fx;

	duckdb_v2_copy_function_handle function = nullptr;
	REQUIRE(duckdb_v2_copy_function_create(nullptr, &function, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(function == nullptr);
	REQUIRE(duckdb_v2_copy_function_create(fx.factory, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	REQUIRE(duckdb_v2_copy_function_create(fx.factory, &function, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_copy_function_set_name(function, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_copy_function_set_user_data(function, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_copy_to_set_batch_callback(nullptr, ToNoopBatch, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_copy_from_set_exec_callback(nullptr, FromNoopExec, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_connection_register_copy_function(fx.conn, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	idx_t count = 0;
	duckdb_v2_logical_type_handle type = nullptr;
	duckdb_v2_identifier_t name = {nullptr, 0};
	duckdb_v2_str path = {nullptr, 0};
	duckdb_v2_value_handle value = nullptr;
	duckdb_v2_column_data_collection_handle input = nullptr;
	duckdb_v2_data_chunk_handle chunk = nullptr;
	void *data = nullptr;
	REQUIRE(duckdb_v2_copy_to_bind_get_column_count(nullptr, &count, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_copy_to_bind_get_column_type(nullptr, 0, &type, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_copy_to_bind_get_column_name(nullptr, 0, &name, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_copy_to_bind_get_option_value(nullptr, 0, &value, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_copy_to_batch_size_set_target(nullptr, 1, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_copy_to_init_get_file_path(nullptr, &path, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_copy_to_batch_take_input(nullptr, &input, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_copy_to_flush_get_batch_data(nullptr, &data, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_copy_to_finalize_get_init_data(nullptr, &data, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_copy_from_bind_get_file_path(nullptr, &path, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_copy_from_bind_set_cardinality(nullptr, 1, true, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_copy_from_init_global_set_max_threads(nullptr, 1, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_copy_from_exec_get_output_chunk(nullptr, &chunk, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_copy_from_progress_set_progress(nullptr, 0.5, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	duckdb_v2_copy_function_destroy(&function);

	REQUIRE(duckdb_v2_copy_function_destroy(nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_copy_function_handle null_function = nullptr;
	REQUIRE(duckdb_v2_copy_function_destroy(&null_function) == DUCKDB_V2_ERROR_NONE);
}

TEST_CASE("V2 copy: set_file_extension", "[capi_v2][copy_function]") {
	EnvFixture fx;
	auto function = MakeCopy(fx.conn, "v2_copy_ext");
	auto extension = Convert("summary");
	REQUIRE(duckdb_v2_copy_function_set_file_extension(function, &extension, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto dotted = Convert(".summary");
	REQUIRE(duckdb_v2_copy_function_set_file_extension(function, &dotted, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_copy_function_set_file_extension(function, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_copy_function_set_file_extension(nullptr, &extension, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	duckdb_v2_copy_function_destroy(&function);
}

} // namespace test_capi_v2
