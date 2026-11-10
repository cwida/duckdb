#include "test_capi_v2.hpp"

#include <cstring>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// V2 stats callback tests. The functions here deliberately produce data their stats callback denies: a query that
// filters on the denied kind of value returns a wrong count exactly when the optimizer acted on the guarantee, which
// makes the callback's effect observable. A callback that leaves the result untouched gets the correct count.
//
// Callbacks avoid Catch assertions: a REQUIRE would throw through the C callback boundary into the engine.
// Observations are latched into file-scope statics and asserted after the query.
// ---------------------------------------------------------------------------

namespace test_capi_v2 {

namespace {

// What a stats callback does to the result statistics, passed as the function's user data.
struct StatsMode {
	bool set_null = false;
	bool can_have_null = true;
	bool set_valid = false;
	bool can_have_valid = true;
	bool fail = false;
	// Scalar exec only: write NULL instead of 7 into every row
	bool write_nulls = false;
};

// What the last stats callback saw.
struct StatsObserved {
	idx_t calls = 0;
	void *user_data = nullptr;
	idx_t arg_count = 0;
	bool arg_can_have_null = false;
	bool arg_can_have_valid = false;
	DUCKDB_V2_ERROR read_only_null_rc = DUCKDB_V2_ERROR_NONE;
	DUCKDB_V2_ERROR read_only_valid_rc = DUCKDB_V2_ERROR_NONE;
	DUCKDB_V2_ERROR out_of_bounds_rc = DUCKDB_V2_ERROR_NONE;
	bool result_can_have_null = false;
	bool result_can_have_valid = false;
	// Table only: one bit per declared column the engine asked about
	uint64_t columns_asked = 0;
};

StatsObserved g_stats_observed;

void StatsFail(duckdb_v2_error_info_handle *err, std::string_view message) {
	duckdb_v2_error_info_set_code(*err, DUCKDB_V2_ERROR_API);
	duckdb_v2_error_info_set_text(*err, message.data(), message.size());
}

// Applies the mode to the result statistics and latches what they read back as.
void StatsApply(duckdb_v2_stats_handle result, const StatsMode &mode, duckdb_v2_error_info_handle *err) {
	if (mode.fail) {
		StatsFail(err, "stats callback failed on purpose");
		return;
	}
	if (mode.set_null && duckdb_v2_stats_set_can_have_null(result, mode.can_have_null, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	if (mode.set_valid &&
	    duckdb_v2_stats_set_can_have_valid(result, mode.can_have_valid, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	if (duckdb_v2_stats_can_have_null(result, &g_stats_observed.result_can_have_null, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	duckdb_v2_stats_can_have_valid(result, &g_stats_observed.result_can_have_valid, err);
}

// Latches the statistics of the single argument, and checks they refuse writes.
void StatsObserveArg(duckdb_v2_stats_handle arg, duckdb_v2_error_info_handle *err) {
	if (duckdb_v2_stats_can_have_null(arg, &g_stats_observed.arg_can_have_null, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_stats_can_have_valid(arg, &g_stats_observed.arg_can_have_valid, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	g_stats_observed.read_only_null_rc = duckdb_v2_stats_set_can_have_null(arg, false, nullptr);
	g_stats_observed.read_only_valid_rc = duckdb_v2_stats_set_can_have_valid(arg, false, nullptr);
}

// Runs a query producing a single BIGINT cell.
int64_t StatsQueryI64(duckdb_v2_connection_handle conn, std::string_view sql) {
	duckdb_v2_result_handle result = nullptr;
	REQUIRE(Query(conn, sql, &result) == DUCKDB_V2_ERROR_NONE);
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

void StatsExec(duckdb_v2_connection_handle conn, std::string_view sql) {
	duckdb_v2_result_handle result = nullptr;
	duckdb_v2_error_info_handle err = nullptr;
	auto rc = Query(conn, sql, &result, &err);
	std::string message;
	if (err) {
		const char *text = nullptr;
		idx_t text_length = 0;
		duckdb_v2_error_info_get_text(err, &text, &text_length);
		message = std::string(text, text_length);
	}
	duckdb_v2_error_info_destroy(&err);
	INFO(sql << ": " << message);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);
	// Execution is lazy: drain the result so the statement runs to completion
	while (auto chunk = StepChunk(result)) {
		duckdb_v2_data_chunk_destroy(&chunk);
	}
	duckdb_v2_result_destroy(&result);
}

// The message of a query that fails while it is planned.
std::string StatsQueryError(duckdb_v2_connection_handle conn, std::string_view sql) {
	duckdb_v2_result_handle result = nullptr;
	duckdb_v2_error_info_handle err = nullptr;
	auto rc = Query(conn, sql, &result, &err);
	duckdb_v2_result_destroy(&result);
	std::string message;
	if (rc != DUCKDB_V2_ERROR_NONE && err) {
		const char *text = nullptr;
		idx_t text_length = 0;
		duckdb_v2_error_info_get_text(err, &text, &text_length);
		message = std::string(text, text_length);
	}
	duckdb_v2_error_info_destroy(&err);
	REQUIRE(rc != DUCKDB_V2_ERROR_NONE);
	return message;
}

// The owned value in the first cell of a query, or null when the query fails or returns no rows.
duckdb_v2_value_handle StatsQueryValue(duckdb_v2_connection_handle conn, std::string_view sql) {
	duckdb_v2_result_handle result = nullptr;
	if (Query(conn, sql, &result) != DUCKDB_V2_ERROR_NONE) {
		return nullptr;
	}
	duckdb_v2_value_handle value = nullptr;
	if (auto chunk = StepChunk(result)) {
		duckdb_v2_vector_handle vec = nullptr;
		if (duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr) == DUCKDB_V2_ERROR_NONE) {
			duckdb_v2_vector_get_value(vec, 0, &value, nullptr);
		}
		duckdb_v2_data_chunk_destroy(&chunk);
	}
	duckdb_v2_result_destroy(&result);
	return value;
}

// Tables whose column statistics the engine knows from storage: "t" has no NULLs, "tn" has one.
void StatsCreateTables(duckdb_v2_connection_handle conn) {
	StatsExec(conn, "CREATE TABLE t AS SELECT range::INTEGER AS i FROM range(3)");
	StatsExec(conn, "CREATE TABLE tn(i INTEGER)");
	StatsExec(conn, "INSERT INTO tn VALUES (1), (NULL), (3)");
}

// ---------------------------------------------------------------------------
// stats_scalar(x INTEGER) -> INTEGER: writes 7 (or NULL) into every row.
// ---------------------------------------------------------------------------

void StatsScalarExecCb(duckdb_v2_scalar_function_exec_info_handle info, duckdb_v2_context_handle,
                       duckdb_v2_error_info_handle *err) {
	void *user_data = nullptr;
	duckdb_v2_vector_handle out = nullptr;
	idx_t count = 0;
	void *raw = nullptr;
	if (duckdb_v2_scalar_function_exec_get_user_data(info, &user_data, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_scalar_function_exec_get_result(info, &out, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_scalar_function_exec_get_row_count(info, &count, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_vector_get_data_mutable(out, &raw, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto *data = static_cast<int32_t *>(raw);
	for (idx_t i = 0; i < count; i++) {
		data[i] = 7;
	}
	if (!static_cast<StatsMode *>(user_data)->write_nulls) {
		return;
	}
	uint64_t *validity = nullptr;
	if (duckdb_v2_vector_flat_get_validity_mutable(out, &validity, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	for (idx_t i = 0; i < count; i++) {
		validity[i / 64] &= ~(uint64_t(1) << (i % 64));
	}
}

void StatsScalarStatsCb(duckdb_v2_scalar_function_stats_info_handle info, duckdb_v2_context_handle,
                        duckdb_v2_error_info_handle *err) {
	g_stats_observed.calls++;
	void *user_data = nullptr;
	idx_t positional_fixed = 0;
	idx_t positional_variadic = 0;
	idx_t named_fixed = 0;
	idx_t named_variadic = 0;
	duckdb_v2_stats_handle arg = nullptr;
	duckdb_v2_stats_handle result = nullptr;
	if (duckdb_v2_scalar_function_stats_get_user_data(info, &user_data, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_scalar_function_stats_get_arg_count(info, &positional_fixed, &positional_variadic, &named_fixed,
	                                                  &named_variadic, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_scalar_function_stats_get_arg_stats(info, 0, &arg, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_scalar_function_stats_get_result_stats(info, &result, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	g_stats_observed.user_data = user_data;
	g_stats_observed.arg_count = positional_fixed + positional_variadic + named_fixed + named_variadic;

	duckdb_v2_stats_handle past_the_end = nullptr;
	g_stats_observed.out_of_bounds_rc =
	    duckdb_v2_scalar_function_stats_get_arg_stats(info, g_stats_observed.arg_count, &past_the_end, nullptr);

	StatsObserveArg(arg, err);
	StatsApply(result, *static_cast<StatsMode *>(user_data), err);
}

void RegisterStatsScalar(duckdb_v2_connection_handle conn, std::string_view name, StatsMode *mode) {
	auto integer = MakeType(FactoryOf(conn), DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	duckdb_v2_scalar_function_handle function = nullptr;
	REQUIRE(duckdb_v2_scalar_function_create(FactoryOf(conn), &function, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_scalar_function_set_name(function, name.data(), name.size(), nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_function_signature_handle sig = nullptr;
	REQUIRE(duckdb_v2_scalar_function_get_signature(function, &sig, nullptr) == DUCKDB_V2_ERROR_NONE);
	std::string_view param = "x";
	REQUIRE(duckdb_v2_function_signature_add_parameter(sig, param.data(), param.size(), integer, nullptr,
	                                                   DUCKDB_V2_FUNCTION_PARAMETER_KIND_STANDARD,
	                                                   nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_function_signature_set_return_type(sig, integer, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_opaque user_data = {mode, nullptr, nullptr};
	REQUIRE(duckdb_v2_scalar_function_set_user_data(function, &user_data, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_scalar_function_set_exec_callback(function, StatsScalarExecCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_scalar_function_set_stats_callback(function, StatsScalarStatsCb, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_register_scalar_function(conn, function, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_scalar_function_destroy(&function);
	duckdb_v2_logical_type_destroy(&integer);
}

// ---------------------------------------------------------------------------
// stats_agg(x INTEGER) -> BIGINT: finalizes every group to 7.
// ---------------------------------------------------------------------------

void StatsAggSize(duckdb_v2_aggregate_function_size_info_handle info, duckdb_v2_error_info_handle *err) {
	duckdb_v2_aggregate_function_size_set_state_size(info, sizeof(int64_t), err);
}
void StatsAggInit(duckdb_v2_aggregate_function_init_info_handle, duckdb_v2_error_info_handle *) {
}
void StatsAggUpdate(duckdb_v2_aggregate_function_update_info_handle, duckdb_v2_error_info_handle *) {
}
void StatsAggCombine(duckdb_v2_aggregate_function_combine_info_handle, duckdb_v2_error_info_handle *) {
}
void StatsAggFinalize(duckdb_v2_aggregate_function_finalize_info_handle info, duckdb_v2_error_info_handle *err) {
	idx_t count = 0;
	idx_t offset = 0;
	duckdb_v2_vector_handle result = nullptr;
	void *raw = nullptr;
	if (duckdb_v2_aggregate_function_finalize_get_state_count(info, &count, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_aggregate_function_finalize_get_result(info, &result, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_aggregate_function_finalize_get_result_offset(info, &offset, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_vector_get_data_mutable(result, &raw, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto *out = static_cast<int64_t *>(raw);
	for (idx_t i = 0; i < count; i++) {
		out[offset + i] = 7;
	}
}

void StatsAggStatsCb(duckdb_v2_aggregate_function_stats_info_handle info, duckdb_v2_context_handle,
                     duckdb_v2_error_info_handle *err) {
	g_stats_observed.calls++;
	void *user_data = nullptr;
	idx_t positional_fixed = 0;
	duckdb_v2_stats_handle arg = nullptr;
	duckdb_v2_stats_handle result = nullptr;
	if (duckdb_v2_aggregate_function_stats_get_user_data(info, &user_data, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_aggregate_function_stats_get_arg_count(info, &positional_fixed, nullptr, nullptr, nullptr, err) !=
	        DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_aggregate_function_stats_get_arg_stats(info, 0, &arg, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_aggregate_function_stats_get_result_stats(info, &result, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	g_stats_observed.user_data = user_data;
	g_stats_observed.arg_count = positional_fixed;

	duckdb_v2_stats_handle past_the_end = nullptr;
	g_stats_observed.out_of_bounds_rc =
	    duckdb_v2_aggregate_function_stats_get_arg_stats(info, positional_fixed, &past_the_end, nullptr);

	StatsObserveArg(arg, err);
	StatsApply(result, *static_cast<StatsMode *>(user_data), err);
}

void RegisterStatsAgg(duckdb_v2_connection_handle conn, std::string_view name, StatsMode *mode) {
	auto integer = MakeType(FactoryOf(conn), DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	auto bigint = MakeType(FactoryOf(conn), DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT);
	duckdb_v2_aggregate_function_handle function = nullptr;
	REQUIRE(duckdb_v2_aggregate_function_create(FactoryOf(conn), &function, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_aggregate_function_set_name(function, name.data(), name.size(), nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_function_signature_handle sig = nullptr;
	REQUIRE(duckdb_v2_aggregate_function_get_signature(function, &sig, nullptr) == DUCKDB_V2_ERROR_NONE);
	std::string_view param = "x";
	REQUIRE(duckdb_v2_function_signature_add_parameter(sig, param.data(), param.size(), integer, nullptr,
	                                                   DUCKDB_V2_FUNCTION_PARAMETER_KIND_STANDARD,
	                                                   nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_function_signature_set_return_type(sig, bigint, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_opaque user_data = {mode, nullptr, nullptr};
	REQUIRE(duckdb_v2_aggregate_function_set_user_data(function, &user_data, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_aggregate_function_set_size_callback(function, StatsAggSize, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_aggregate_function_set_init_callback(function, StatsAggInit, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_aggregate_function_set_update_callback(function, StatsAggUpdate, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_aggregate_function_set_combine_callback(function, StatsAggCombine, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_aggregate_function_set_finalize_callback(function, StatsAggFinalize, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_aggregate_function_set_stats_callback(function, StatsAggStatsCb, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_register_aggregate_function(conn, function, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_aggregate_function_destroy(&function);
	duckdb_v2_logical_type_destroy(&integer);
	duckdb_v2_logical_type_destroy(&bigint);
}

// ---------------------------------------------------------------------------
// stats_pair(): INTEGER columns "a" and "b", three rows of 7, one per batch so any vector size fits. The stats
// callback applies the mode to "b" only.
// ---------------------------------------------------------------------------

struct StatsPairGlobal {
	idx_t produced = 0;
};

void DeleteStatsPairGlobal(void *ptr) {
	delete static_cast<StatsPairGlobal *>(ptr);
}

void StatsPairBindCb(duckdb_v2_function_bind_info_handle, duckdb_v2_table_function_bind_info_handle result,
                     duckdb_v2_context_handle context, duckdb_v2_error_info_handle *err) {
	duckdb_v2_factory_handle factory = nullptr;
	duckdb_v2_logical_type_handle integer = nullptr;
	if (duckdb_v2_context_get_factory(context, &factory, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_factory_create_type_from_id(factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER, nullptr, nullptr, nullptr, 0,
	                                          &integer, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	std::string_view a = "a";
	std::string_view b = "b";
	if (duckdb_v2_table_function_bind_add_result_column(result, a.data(), a.size(), integer, err) ==
	    DUCKDB_V2_ERROR_NONE) {
		duckdb_v2_table_function_bind_add_result_column(result, b.data(), b.size(), integer, err);
	}
	duckdb_v2_logical_type_destroy(&integer);
}

void StatsPairInitGlobalCb(duckdb_v2_table_function_init_global_info_handle info, duckdb_v2_context_handle,
                           duckdb_v2_error_info_handle *err) {
	duckdb_v2_opaque state = {new StatsPairGlobal(), DeleteStatsPairGlobal, nullptr};
	duckdb_v2_table_function_init_global_set_global_state(info, &state, err);
}

void StatsPairExecCb(duckdb_v2_table_function_exec_info_handle info, duckdb_v2_context_handle,
                     duckdb_v2_error_info_handle *err) {
	void *global_ptr = nullptr;
	duckdb_v2_data_chunk_handle chunk = nullptr;
	if (duckdb_v2_table_function_exec_get_global_state(info, &global_ptr, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_table_function_exec_get_output_chunk(info, &chunk, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto &global = *static_cast<StatsPairGlobal *>(global_ptr);
	idx_t produced = global.produced < 3 ? 1 : 0;
	global.produced += produced;
	duckdb_v2_vector_handle first = nullptr;
	for (idx_t col = 0; col < 2; col++) {
		duckdb_v2_vector_handle vec = nullptr;
		void *raw = nullptr;
		if (duckdb_v2_data_chunk_get_vector(chunk, col, &vec, err) != DUCKDB_V2_ERROR_NONE ||
		    duckdb_v2_vector_get_data_mutable(vec, &raw, err) != DUCKDB_V2_ERROR_NONE) {
			return;
		}
		for (idx_t i = 0; i < produced; i++) {
			static_cast<int32_t *>(raw)[i] = 7;
		}
		if (col == 0) {
			first = vec;
		}
	}
	// The first vector's size is the batch's row count; 0 ends the scan.
	duckdb_v2_vector_set_size(first, produced, err);
}

void StatsPairStatsCb(duckdb_v2_table_function_stats_info_handle info, duckdb_v2_context_handle,
                      duckdb_v2_error_info_handle *err) {
	g_stats_observed.calls++;
	void *user_data = nullptr;
	idx_t column = 0;
	duckdb_v2_stats_handle result = nullptr;
	if (duckdb_v2_table_function_stats_get_user_data(info, &user_data, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_table_function_stats_get_column_index(info, &column, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_table_function_stats_get_result_stats(info, &result, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	g_stats_observed.user_data = user_data;
	g_stats_observed.columns_asked |= uint64_t(1) << column;
	if (column == 1) {
		StatsApply(result, *static_cast<StatsMode *>(user_data), err);
	}
}

void RegisterStatsPair(duckdb_v2_connection_handle conn, std::string_view name, StatsMode *mode) {
	duckdb_v2_table_function_handle function = nullptr;
	REQUIRE(duckdb_v2_table_function_create(FactoryOf(conn), &function, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_name(function, name.data(), name.size(), nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_opaque user_data = {mode, nullptr, nullptr};
	REQUIRE(duckdb_v2_table_function_set_user_data(function, &user_data, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_bind_callback(function, StatsPairBindCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_init_global_callback(function, StatsPairInitGlobalCb, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_exec_callback(function, StatsPairExecCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_stats_callback(function, StatsPairStatsCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_register_table_function(conn, function, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_table_function_destroy(&function);
}

} // namespace

// ---------------------------------------------------------------------------
// Scalar
// ---------------------------------------------------------------------------

TEST_CASE("V2 stats: scalar result statistics reach the optimizer", "[capi_v2][stats]") {
	EnvFixture fx;
	StatsCreateTables(fx.conn);

	// Writes NULLs but promises none, so "IS NULL" is folded to false
	static StatsMode no_null;
	no_null.set_null = true;
	no_null.can_have_null = false;
	no_null.write_nulls = true;
	RegisterStatsScalar(fx.conn, "stats_no_null", &no_null);

	// Writes 7s but promises only NULLs, so "IS NOT NULL" is folded to false
	static StatsMode all_null;
	all_null.set_valid = true;
	all_null.can_have_valid = false;
	RegisterStatsScalar(fx.conn, "stats_all_null", &all_null);

	// Leaves the result untouched: the data decides
	static StatsMode untouched;
	untouched.write_nulls = true;
	RegisterStatsScalar(fx.conn, "stats_untouched", &untouched);

	g_stats_observed = {};
	REQUIRE(StatsQueryI64(fx.conn, "SELECT count(*) FROM t WHERE stats_no_null(i) IS NULL") == 0);
	REQUIRE(g_stats_observed.calls > 0);
	REQUIRE(g_stats_observed.user_data == &no_null);
	REQUIRE(!g_stats_observed.result_can_have_null);
	REQUIRE(g_stats_observed.result_can_have_valid);

	g_stats_observed = {};
	REQUIRE(StatsQueryI64(fx.conn, "SELECT count(*) FROM t WHERE stats_all_null(i) IS NOT NULL") == 0);
	REQUIRE(g_stats_observed.result_can_have_null);
	REQUIRE(!g_stats_observed.result_can_have_valid);

	g_stats_observed = {};
	REQUIRE(StatsQueryI64(fx.conn, "SELECT count(*) FROM t WHERE stats_untouched(i) IS NULL") == 3);
	REQUIRE(g_stats_observed.calls > 0);
	REQUIRE(g_stats_observed.result_can_have_null);
	REQUIRE(g_stats_observed.result_can_have_valid);
}

TEST_CASE("V2 stats: scalar argument statistics are read-only", "[capi_v2][stats]") {
	EnvFixture fx;
	StatsCreateTables(fx.conn);
	static StatsMode untouched;
	RegisterStatsScalar(fx.conn, "stats_probe", &untouched);

	g_stats_observed = {};
	StatsExec(fx.conn, "SELECT stats_probe(i) FROM t");
	REQUIRE(g_stats_observed.calls > 0);
	REQUIRE(g_stats_observed.arg_count == 1);
	REQUIRE(!g_stats_observed.arg_can_have_null);
	REQUIRE(g_stats_observed.arg_can_have_valid);
	REQUIRE(g_stats_observed.read_only_null_rc == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(g_stats_observed.read_only_valid_rc == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(g_stats_observed.out_of_bounds_rc == DUCKDB_V2_ERROR_INPUT_INVALID);

	g_stats_observed = {};
	StatsExec(fx.conn, "SELECT stats_probe(i) FROM tn");
	REQUIRE(g_stats_observed.calls > 0);
	REQUIRE(g_stats_observed.arg_can_have_null);
	REQUIRE(g_stats_observed.arg_can_have_valid);
}

TEST_CASE("V2 stats: a failing stats callback fails the query", "[capi_v2][stats]") {
	EnvFixture fx;
	StatsCreateTables(fx.conn);
	static StatsMode failing;
	failing.fail = true;
	RegisterStatsScalar(fx.conn, "stats_failing", &failing);

	auto message = StatsQueryError(fx.conn, "SELECT stats_failing(i) FROM t");
	REQUIRE(message.find("stats callback failed on purpose") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Aggregate
// ---------------------------------------------------------------------------

TEST_CASE("V2 stats: aggregate statistics", "[capi_v2][stats]") {
	EnvFixture fx;
	StatsCreateTables(fx.conn);

	// Finalizes to 7 but promises only NULLs, so "IS NOT NULL" on the result is folded to false
	static StatsMode all_null;
	all_null.set_valid = true;
	all_null.can_have_valid = false;
	RegisterStatsAgg(fx.conn, "stats_agg_all_null", &all_null);

	static StatsMode untouched;
	RegisterStatsAgg(fx.conn, "stats_agg_untouched", &untouched);

	g_stats_observed = {};
	REQUIRE(StatsQueryI64(fx.conn,
	                      "SELECT count(*) FROM (SELECT stats_agg_all_null(i) AS a FROM t) WHERE a IS NOT NULL") == 0);
	REQUIRE(g_stats_observed.calls > 0);
	REQUIRE(g_stats_observed.user_data == &all_null);
	REQUIRE(!g_stats_observed.result_can_have_valid);

	g_stats_observed = {};
	REQUIRE(StatsQueryI64(
	            fx.conn, "SELECT count(*) FROM (SELECT stats_agg_untouched(i) AS a FROM tn) WHERE a IS NOT NULL") == 1);
	REQUIRE(g_stats_observed.calls > 0);
	REQUIRE(g_stats_observed.arg_count == 1);
	REQUIRE(g_stats_observed.arg_can_have_null);
	REQUIRE(g_stats_observed.arg_can_have_valid);
	REQUIRE(g_stats_observed.read_only_null_rc == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(g_stats_observed.read_only_valid_rc == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(g_stats_observed.out_of_bounds_rc == DUCKDB_V2_ERROR_INPUT_INVALID);
}

// ---------------------------------------------------------------------------
// Table
// ---------------------------------------------------------------------------

TEST_CASE("V2 stats: table column statistics", "[capi_v2][stats]") {
	EnvFixture fx;

	// Column "b" holds 7s but is promised to hold only NULLs
	static StatsMode all_null;
	all_null.set_valid = true;
	all_null.can_have_valid = false;
	RegisterStatsPair(fx.conn, "stats_pair", &all_null);

	static StatsMode untouched;
	RegisterStatsPair(fx.conn, "stats_pair_untouched", &untouched);

	g_stats_observed = {};
	REQUIRE(StatsQueryI64(fx.conn, "SELECT count(*) FROM stats_pair() WHERE b IS NOT NULL") == 0);
	REQUIRE(g_stats_observed.calls > 0);
	REQUIRE(g_stats_observed.user_data == &all_null);
	REQUIRE((g_stats_observed.columns_asked & 2) != 0);
	REQUIRE(!g_stats_observed.result_can_have_valid);

	// Column "a" is described as unknown, so its filter is evaluated
	REQUIRE(StatsQueryI64(fx.conn, "SELECT count(*) FROM stats_pair() WHERE a IS NOT NULL") == 3);

	g_stats_observed = {};
	REQUIRE(StatsQueryI64(fx.conn, "SELECT count(*) FROM stats_pair_untouched() WHERE b IS NOT NULL") == 3);
	REQUIRE(g_stats_observed.calls > 0);
	REQUIRE(g_stats_observed.result_can_have_valid);
}

// ---------------------------------------------------------------------------
// Null arguments
// ---------------------------------------------------------------------------

TEST_CASE("V2 stats: null arguments", "[capi_v2][stats]") {
	bool out = false;
	REQUIRE(duckdb_v2_stats_can_have_null(nullptr, &out, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_stats_can_have_valid(nullptr, &out, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_stats_set_can_have_null(nullptr, false, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_stats_set_can_have_valid(nullptr, false, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	REQUIRE(duckdb_v2_scalar_function_set_stats_callback(nullptr, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_aggregate_function_set_stats_callback(nullptr, nullptr, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_table_function_set_stats_callback(nullptr, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	duckdb_v2_stats_handle stats = nullptr;
	REQUIRE(duckdb_v2_scalar_function_stats_get_result_stats(nullptr, &stats, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_aggregate_function_stats_get_result_stats(nullptr, &stats, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_table_function_stats_get_result_stats(nullptr, &stats, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
}

// ---------------------------------------------------------------------------
// Bounds: a table function reports min/max through the stats callback as a STRUCT value, and checks predicates against
// statistics it builds itself. The function produces data its statistics exclude, so a query whose filter the
// statistics make unsatisfiable returns a wrong count exactly when the optimizer acted on them.
// ---------------------------------------------------------------------------

namespace {

// Builds a STRUCT from parallel field names and values with the value API, and sets the statistics from it.
DUCKDB_V2_ERROR SetStatsStruct(duckdb_v2_factory_handle factory, duckdb_v2_stats_handle stats,
                               const std::vector<const char *> &names,
                               const std::vector<duckdb_v2_value_handle> &children, duckdb_v2_error_info_handle *err) {
	StrArray idents;
	for (auto name : names) {
		idents.push_back(name);
	}
	duckdb_v2_value_handle value = nullptr;
	auto rc = duckdb_v2_value_create_struct(factory, idents.ptrs(), idents.lengths(), children.data(), children.size(),
	                                        &value, err);
	if (rc == DUCKDB_V2_ERROR_NONE) {
		rc = duckdb_v2_stats_set_value(stats, value, err);
	}
	duckdb_v2_value_destroy(&value);
	return rc;
}

struct ValueField;

// A value spelled as a tree, the compact way to write a nested statistics struct in a test case; BuildValue turns it
// into a value with the typed constructors, duckdb_v2_value_create_struct, duckdb_v2_value_create_tuple and
// duckdb_v2_value_create_list.
struct ValueSpec {
	enum class Kind { NULL_VALUE, BOOL, INT, DOUBLE, TEXT, STRUCT, TUPLE, LIST };
	Kind kind = Kind::NULL_VALUE;
	bool b = false;
	int32_t i = 0;
	double d = 0;
	const char *s = nullptr;
	std::vector<ValueField> fields;
	std::vector<ValueSpec> items;
};

// A named struct field of a ValueSpec, complete only here so the vector above never holds a pair of an incomplete type.
struct ValueField {
	const char *name;
	ValueSpec value;
};

ValueSpec Null() {
	return ValueSpec();
}
ValueSpec Bool(bool b) {
	ValueSpec spec;
	spec.kind = ValueSpec::Kind::BOOL;
	spec.b = b;
	return spec;
}
ValueSpec Int(int32_t i) {
	ValueSpec spec;
	spec.kind = ValueSpec::Kind::INT;
	spec.i = i;
	return spec;
}
ValueSpec Dbl(double d) {
	ValueSpec spec;
	spec.kind = ValueSpec::Kind::DOUBLE;
	spec.d = d;
	return spec;
}
ValueSpec Str(const char *s) {
	ValueSpec spec;
	spec.kind = ValueSpec::Kind::TEXT;
	spec.s = s;
	return spec;
}
ValueSpec Obj(std::initializer_list<ValueField> fields) {
	ValueSpec spec;
	spec.kind = ValueSpec::Kind::STRUCT;
	spec.fields.assign(fields.begin(), fields.end());
	return spec;
}
ValueSpec Arr(std::initializer_list<ValueSpec> items) {
	ValueSpec spec;
	spec.kind = ValueSpec::Kind::LIST;
	spec.items.assign(items.begin(), items.end());
	return spec;
}
ValueSpec Tup(std::initializer_list<ValueSpec> items) {
	ValueSpec spec;
	spec.kind = ValueSpec::Kind::TUPLE;
	spec.items.assign(items.begin(), items.end());
	return spec;
}

// The owned value a spec describes, or null when it cannot be built; a NULL is an INTEGER NULL, a list takes the type
// of its first item.
duckdb_v2_value_handle BuildValue(duckdb_v2_factory_handle factory, const ValueSpec &spec,
                                  duckdb_v2_error_info_handle *err) {
	duckdb_v2_value_handle value = nullptr;
	duckdb_v2_logical_type_handle type = nullptr;
	StrArray names;
	std::vector<duckdb_v2_value_handle> children;
	switch (spec.kind) {
	case ValueSpec::Kind::NULL_VALUE:
		if (duckdb_v2_factory_create_type_from_id(factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER, nullptr, nullptr, nullptr,
		                                          0, &type, err) == DUCKDB_V2_ERROR_NONE) {
			duckdb_v2_value_create_null(factory, type, &value, err);
		}
		break;
	case ValueSpec::Kind::BOOL:
		duckdb_v2_value_create_bool(factory, spec.b, &value, err);
		break;
	case ValueSpec::Kind::INT:
		duckdb_v2_value_create_int(factory, spec.i, &value, err);
		break;
	case ValueSpec::Kind::DOUBLE:
		duckdb_v2_value_create_double(factory, spec.d, &value, err);
		break;
	case ValueSpec::Kind::TEXT: {
		std::string_view text(spec.s);
		duckdb_v2_value_create_varchar(factory, text.data(), text.size(), &value, err);
		break;
	}
	case ValueSpec::Kind::STRUCT: {
		bool ok = true;
		for (auto &field : spec.fields) {
			names.push_back(field.name);
			children.push_back(BuildValue(factory, field.value, err));
			ok = ok && children.back();
		}
		if (ok) {
			duckdb_v2_value_create_struct(factory, names.ptrs(), names.lengths(), children.data(), children.size(),
			                              &value, err);
		}
		break;
	}
	case ValueSpec::Kind::TUPLE: {
		bool ok = true;
		for (auto &item : spec.items) {
			children.push_back(BuildValue(factory, item, err));
			ok = ok && children.back();
		}
		if (ok) {
			duckdb_v2_value_create_tuple(factory, children.data(), children.size(), &value, err);
		}
		break;
	}
	case ValueSpec::Kind::LIST: {
		bool ok = !spec.items.empty();
		for (auto &item : spec.items) {
			children.push_back(BuildValue(factory, item, err));
			ok = ok && children.back();
		}
		if (ok && duckdb_v2_value_get_logical_type(children[0], &type, err) == DUCKDB_V2_ERROR_NONE) {
			duckdb_v2_value_create_list(factory, type, children.data(), children.size(), &value, err);
		}
		break;
	}
	}
	for (auto &child : children) {
		duckdb_v2_value_destroy(&child);
	}
	duckdb_v2_logical_type_destroy(&type);
	return value;
}

// Builds the value a spec describes and sets the statistics from it.
DUCKDB_V2_ERROR SetStatsSpec(duckdb_v2_factory_handle factory, duckdb_v2_stats_handle stats, const ValueSpec &spec,
                             duckdb_v2_error_info_handle *err) {
	auto value = BuildValue(factory, spec, err);
	if (!value) {
		return DUCKDB_V2_ERROR_API;
	}
	auto rc = duckdb_v2_stats_set_value(stats, value, err);
	duckdb_v2_value_destroy(&value);
	return rc;
}

// The statistics rendered as text, "<error>" when they cannot be read.
std::string StatsText(duckdb_v2_stats_handle stats) {
	duckdb_v2_value_handle value = nullptr;
	if (duckdb_v2_stats_get_value(stats, &value, nullptr) != DUCKDB_V2_ERROR_NONE) {
		return "<error>";
	}
	auto text = Render(value);
	duckdb_v2_value_destroy(&value);
	return text;
}

// The owned value of one field of the statistics struct, found by name through the struct type; null when absent.
duckdb_v2_value_handle StatsField(duckdb_v2_stats_handle stats, const char *name) {
	duckdb_v2_value_handle value = nullptr;
	if (duckdb_v2_stats_get_value(stats, &value, nullptr) != DUCKDB_V2_ERROR_NONE) {
		return nullptr;
	}
	duckdb_v2_logical_type_handle type = nullptr;
	idx_t count = 0;
	duckdb_v2_value_handle field = nullptr;
	if (duckdb_v2_value_get_logical_type(value, &type, nullptr) == DUCKDB_V2_ERROR_NONE &&
	    duckdb_v2_logical_type_get_param_count(type, &count, nullptr) == DUCKDB_V2_ERROR_NONE) {
		for (idx_t i = 0; i < count && !field; i++) {
			const char *field_name = nullptr;
			idx_t field_name_length = 0;
			duckdb_v2_value_handle field_type = nullptr;
			if (duckdb_v2_logical_type_get_param(type, i, &field_name, &field_name_length, &field_type, nullptr) ==
			        DUCKDB_V2_ERROR_NONE &&
			    std::string_view(field_name, field_name_length) == name) {
				duckdb_v2_value_get_child(value, i, &field, nullptr);
			}
			duckdb_v2_value_destroy(&field_type);
		}
	}
	duckdb_v2_logical_type_destroy(&type);
	duckdb_v2_value_destroy(&value);
	return field;
}

// The type id of a value.
DUCKDB_V2_LOGICAL_TYPE_ID TypeIdOf(duckdb_v2_value_handle value) {
	duckdb_v2_logical_type_handle type = nullptr;
	auto id = DUCKDB_V2_LOGICAL_TYPE_ID_INVALID;
	if (duckdb_v2_value_get_logical_type(value, &type, nullptr) == DUCKDB_V2_ERROR_NONE) {
		duckdb_v2_logical_type_get_id(type, &id, nullptr);
	}
	duckdb_v2_logical_type_destroy(&type);
	return id;
}

// Owned statistics of a type given as text, or null when they cannot be built.
duckdb_v2_stats_handle StatsOfText(duckdb_v2_factory_handle factory, const char *type_text,
                                   duckdb_v2_error_info_handle *err) {
	std::string_view text(type_text);
	duckdb_v2_logical_type_handle type = nullptr;
	duckdb_v2_stats_handle stats = nullptr;
	if (duckdb_v2_factory_create_type_from_text(factory, text.data(), text.size(), &type, err) ==
	    DUCKDB_V2_ERROR_NONE) {
		duckdb_v2_stats_create(factory, type, &stats, err);
	}
	duckdb_v2_logical_type_destroy(&type);
	return stats;
}

// Owned INTEGER statistics bounded by [min, max] without NULLs, set from a STRUCT built with the value API; null when
// they cannot be built.
duckdb_v2_stats_handle IntStats(duckdb_v2_factory_handle factory, int32_t min, int32_t max,
                                duckdb_v2_error_info_handle *err) {
	auto stats = StatsOfText(factory, "INTEGER", err);
	if (!stats) {
		return nullptr;
	}
	duckdb_v2_value_handle lower = nullptr;
	duckdb_v2_value_handle upper = nullptr;
	duckdb_v2_value_handle no_nulls = nullptr;
	if (duckdb_v2_value_create_int(factory, min, &lower, err) == DUCKDB_V2_ERROR_NONE &&
	    duckdb_v2_value_create_int(factory, max, &upper, err) == DUCKDB_V2_ERROR_NONE &&
	    duckdb_v2_value_create_bool(factory, false, &no_nulls, err) == DUCKDB_V2_ERROR_NONE) {
		SetStatsStruct(factory, stats, {"min", "max", "has_null"}, {lower, upper, no_nulls}, err);
	}
	duckdb_v2_value_destroy(&lower);
	duckdb_v2_value_destroy(&upper);
	duckdb_v2_value_destroy(&no_nulls);
	return stats;
}

// Sets string bounds from two VARCHAR values built with the value API.
DUCKDB_V2_ERROR SetTextBounds(duckdb_v2_factory_handle factory, duckdb_v2_stats_handle stats, const char *min,
                              const char *max, duckdb_v2_error_info_handle *err) {
	duckdb_v2_value_handle lower = nullptr;
	duckdb_v2_value_handle upper = nullptr;
	duckdb_v2_value_handle no_nulls = nullptr;
	auto rc = duckdb_v2_value_create_varchar(factory, min, std::strlen(min), &lower, err);
	if (rc == DUCKDB_V2_ERROR_NONE) {
		rc = duckdb_v2_value_create_varchar(factory, max, std::strlen(max), &upper, err);
	}
	if (rc == DUCKDB_V2_ERROR_NONE) {
		rc = duckdb_v2_value_create_bool(factory, false, &no_nulls, err);
	}
	if (rc == DUCKDB_V2_ERROR_NONE) {
		rc = SetStatsStruct(factory, stats, {"min", "max", "has_null"}, {lower, upper, no_nulls}, err);
	}
	duckdb_v2_value_destroy(&lower);
	duckdb_v2_value_destroy(&upper);
	duckdb_v2_value_destroy(&no_nulls);
	return rc;
}

// What a function's stats / pushdown callback does, passed as its user data.
struct MinMaxMode {
	bool report = true;
	// The pushdown callback checks against statistics merged from [5, 10] and [1, 20] instead of [20, 30].
	bool merged = false;
	// The stats callback builds owned statistics and assigns them to the result instead of narrowing it in place.
	bool via_assign = false;
	// The VARCHAR column holds the five characters a\x41 (a backslash, no escape) and is bounded by exactly that.
	bool backslash = false;
};

// What the last stats / pushdown callback saw.
struct MinMaxObserved {
	idx_t stats_calls = 0;
	idx_t exec_calls = 0;
	DUCKDB_V2_ERROR check_error_rc = DUCKDB_V2_ERROR_NONE;
	DUCKDB_V2_ERROR type_mismatch_rc = DUCKDB_V2_ERROR_NONE;
	void *user_data = nullptr;
	idx_t column_index = 999;
	DUCKDB_V2_ERROR set_rc = DUCKDB_V2_ERROR_NONE;
	DUCKDB_V2_ERROR string_on_numeric_rc = DUCKDB_V2_ERROR_NONE;
	DUCKDB_V2_ERROR destroy_borrowed_rc = DUCKDB_V2_ERROR_NONE;
	bool borrowed_survives = false;
	DUCKDB_V2_ERROR assign_rc = DUCKDB_V2_ERROR_NONE;
	std::string result_text;
	idx_t pushdown_checks = 0;
	DUCKDB_V2_FILTER_PROPAGATE_RESULT check_result = DUCKDB_V2_FILTER_PROPAGATE_RESULT_NO_PRUNING_POSSIBLE;
	DUCKDB_V2_ERROR merge_rc = DUCKDB_V2_ERROR_NONE;
};

MinMaxObserved g_mm;

struct MinMaxGlobal {
	idx_t produced = 0;
};

void DeleteMinMaxGlobal(void *p) {
	delete static_cast<MinMaxGlobal *>(p);
}

// Columns: i INTEGER, j INTEGER, s VARCHAR, t TUPLE(INTEGER, VARCHAR)
void MinMaxBindCb(duckdb_v2_function_bind_info_handle, duckdb_v2_table_function_bind_info_handle result,
                  duckdb_v2_context_handle context, duckdb_v2_error_info_handle *err) {
	duckdb_v2_factory_handle factory = nullptr;
	duckdb_v2_logical_type_handle integer = nullptr;
	duckdb_v2_logical_type_handle varchar = nullptr;
	duckdb_v2_logical_type_handle pair = nullptr;
	std::string_view pair_text = "TUPLE(INTEGER, VARCHAR)";
	if (duckdb_v2_context_get_factory(context, &factory, err) == DUCKDB_V2_ERROR_NONE &&
	    duckdb_v2_factory_create_type_from_id(factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER, nullptr, nullptr, nullptr, 0,
	                                          &integer, err) == DUCKDB_V2_ERROR_NONE &&
	    duckdb_v2_factory_create_type_from_id(factory, DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR, nullptr, nullptr, nullptr, 0,
	                                          &varchar, err) == DUCKDB_V2_ERROR_NONE &&
	    duckdb_v2_factory_create_type_from_text(factory, pair_text.data(), pair_text.size(), &pair, err) ==
	        DUCKDB_V2_ERROR_NONE) {
		duckdb_v2_table_function_bind_add_result_column(result, "i", 1, integer, err);
		duckdb_v2_table_function_bind_add_result_column(result, "j", 1, integer, err);
		duckdb_v2_table_function_bind_add_result_column(result, "s", 1, varchar, err);
		duckdb_v2_table_function_bind_add_result_column(result, "t", 1, pair, err);
	}
	duckdb_v2_logical_type_destroy(&integer);
	duckdb_v2_logical_type_destroy(&varchar);
	duckdb_v2_logical_type_destroy(&pair);
}

void MinMaxInitGlobalCb(duckdb_v2_table_function_init_global_info_handle info, duckdb_v2_context_handle,
                        duckdb_v2_error_info_handle *err) {
	duckdb_v2_opaque state = {new MinMaxGlobal(), DeleteMinMaxGlobal, nullptr};
	duckdb_v2_table_function_init_global_set_global_state(info, &state, err);
}

void MinMaxExecCb(duckdb_v2_table_function_exec_info_handle info, duckdb_v2_context_handle context,
                  duckdb_v2_error_info_handle *err) {
	void *global_ptr = nullptr;
	void *user_data = nullptr;
	duckdb_v2_data_chunk_handle chunk = nullptr;
	duckdb_v2_factory_handle factory = nullptr;
	if (duckdb_v2_table_function_exec_get_global_state(info, &global_ptr, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_table_function_exec_get_user_data(info, &user_data, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_table_function_exec_get_output_chunk(info, &chunk, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_context_get_factory(context, &factory, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	g_mm.exec_calls++;
	auto *mode = static_cast<MinMaxMode *>(user_data);
	auto &global = *static_cast<MinMaxGlobal *>(global_ptr);
	idx_t produced = global.produced < 3 ? 1 : 0;
	global.produced += produced;
	for (idx_t column = 0; column < 4; column++) {
		duckdb_v2_vector_handle vec = nullptr;
		if (duckdb_v2_data_chunk_get_vector(chunk, column, &vec, err) != DUCKDB_V2_ERROR_NONE ||
		    duckdb_v2_vector_set_size(vec, produced, err) != DUCKDB_V2_ERROR_NONE) {
			return;
		}
		for (idx_t row = 0; row < produced; row++) {
			if (column < 2) {
				// Every row holds 7 - a value the reported statistics (min 20, max 30) exclude.
				void *raw = nullptr;
				if (duckdb_v2_vector_get_data_mutable(vec, &raw, err) == DUCKDB_V2_ERROR_NONE) {
					static_cast<int32_t *>(raw)[row] = 7;
				}
				continue;
			}
			// Every row holds 'zebra' - a value after the reported maximum 'pear' - or the five characters a\x41; the
			// TUPLE column holds (7, <that text>).
			duckdb_v2_value_handle text_value = nullptr;
			duckdb_v2_value_handle seven = nullptr;
			duckdb_v2_value_handle pair = nullptr;
			std::string_view text = mode && mode->backslash ? "a\\x41" : "zebra";
			if (duckdb_v2_value_create_varchar(factory, text.data(), text.size(), &text_value, err) !=
			    DUCKDB_V2_ERROR_NONE) {
				return;
			}
			if (column == 2) {
				duckdb_v2_vector_set_value(vec, row, text_value, err);
			} else if (duckdb_v2_value_create_int(factory, 7, &seven, err) == DUCKDB_V2_ERROR_NONE) {
				const duckdb_v2_value_handle elements[] = {seven, text_value};
				if (duckdb_v2_value_create_tuple(factory, elements, 2, &pair, err) == DUCKDB_V2_ERROR_NONE) {
					duckdb_v2_vector_set_value(vec, row, pair, err);
				}
			}
			duckdb_v2_value_destroy(&pair);
			duckdb_v2_value_destroy(&seven);
			duckdb_v2_value_destroy(&text_value);
		}
	}
}

void MinMaxStatsCb(duckdb_v2_table_function_stats_info_handle info, duckdb_v2_context_handle context,
                   duckdb_v2_error_info_handle *err) {
	g_mm.stats_calls++;
	void *user_data = nullptr;
	idx_t column = 999;
	duckdb_v2_stats_handle result = nullptr;
	duckdb_v2_factory_handle factory = nullptr;
	if (duckdb_v2_table_function_stats_get_user_data(info, &user_data, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_table_function_stats_get_column_index(info, &column, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_table_function_stats_get_result_stats(info, &result, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_context_get_factory(context, &factory, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	g_mm.user_data = user_data;
	g_mm.column_index = column;

	auto *mode = static_cast<MinMaxMode *>(user_data);
	if (!mode || !mode->report) {
		return;
	}

	if (column == 2) {
		// The VARCHAR column: bounds in binary order, as a file format stores them, taken as the bytes they are
		if (mode->backslash) {
			g_mm.set_rc = SetTextBounds(factory, result, "a\\x41", "a\\x41", err);
		} else {
			g_mm.set_rc = SetTextBounds(factory, result, "apple", "pear", err);
		}
		g_mm.result_text = StatsText(result);
		return;
	}
	if (column == 3) {
		// The TUPLE column: the statistics of its elements in order, as a TUPLE
		g_mm.set_rc = SetStatsSpec(
		    factory, result,
		    Obj({{"child_stats", Tup({Obj({{"min", Int(20)}, {"max", Int(30)}, {"has_null", Bool(false)}}),
		                              Obj({{"min", Str("apple")}, {"max", Str("pear")}, {"has_null", Bool(false)}})})},
		         {"has_null", Bool(false)}}),
		    err);
		g_mm.result_text = StatsText(result);
		return;
	}
	if (mode->via_assign) {
		// Statistics built by the function (as combined from its files) are copied into the result.
		auto owned = IntStats(factory, 20, 30, err);
		g_mm.assign_rc = duckdb_v2_stats_assign(result, owned, err);
		duckdb_v2_stats_destroy(&owned);
	} else {
		g_mm.set_rc =
		    SetStatsSpec(factory, result, Obj({{"min", Int(20)}, {"max", Int(30)}, {"has_null", Bool(false)}}), err);
	}
	g_mm.result_text = StatsText(result);
	// A string bound on numeric statistics is refused, and leaves the bounds as they were.
	g_mm.string_on_numeric_rc = SetStatsSpec(factory, result, Obj({{"min", Str("x")}}), nullptr);
	// The borrowed result statistics cannot be destroyed, and stay usable.
	auto borrowed = result;
	g_mm.destroy_borrowed_rc = duckdb_v2_stats_destroy(&borrowed);
	bool can_have_null = true;
	g_mm.borrowed_survives = borrowed == result &&
	                         duckdb_v2_stats_can_have_null(result, &can_have_null, nullptr) == DUCKDB_V2_ERROR_NONE &&
	                         !can_have_null;
}

void MinMaxPushdownCb(duckdb_v2_table_function_filter_pushdown_info_handle info, duckdb_v2_context_handle context,
                      duckdb_v2_error_info_handle *err) {
	idx_t count = 0;
	void *user_data = nullptr;
	duckdb_v2_factory_handle factory = nullptr;
	if (duckdb_v2_table_function_filter_pushdown_get_filter_count(info, &count, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_table_function_filter_pushdown_get_user_data(info, &user_data, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_context_get_factory(context, &factory, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto *mode = static_cast<MinMaxMode *>(user_data);
	duckdb_v2_stats_handle stats = nullptr;
	if (mode && mode->merged) {
		// Merging [1, 20] into [5, 10] widens the bounds to [1, 20].
		stats = IntStats(factory, 5, 10, err);
		auto other = IntStats(factory, 1, 20, err);
		g_mm.merge_rc = duckdb_v2_stats_merge(stats, other, err);
		duckdb_v2_stats_destroy(&other);
	} else {
		stats = IntStats(factory, 20, 30, err);
	}
	if (stats) {
		for (idx_t i = 0; i < count; i++) {
			duckdb_v2_expression_handle filter = nullptr;
			if (duckdb_v2_table_function_filter_pushdown_get_filter(info, i, &filter, err) != DUCKDB_V2_ERROR_NONE) {
				break;
			}
			DUCKDB_V2_FILTER_PROPAGATE_RESULT outcome = DUCKDB_V2_FILTER_PROPAGATE_RESULT_NO_PRUNING_POSSIBLE;
			auto rc = duckdb_v2_expression_check_statistics(context, filter, stats, &outcome, nullptr);
			if (rc == DUCKDB_V2_ERROR_NONE) {
				g_mm.pushdown_checks++;
				g_mm.check_result = outcome;
			} else {
				g_mm.check_error_rc = rc;
			}
			// Statistics of another type than the column are refused
			auto double_stats = StatsOfText(factory, "DOUBLE", nullptr);
			if (double_stats) {
				g_mm.type_mismatch_rc =
				    duckdb_v2_expression_check_statistics(context, filter, double_stats, &outcome, nullptr);
			}
			duckdb_v2_stats_destroy(&double_stats);
		}
	}
	duckdb_v2_stats_destroy(&stats);
}

void RegisterMinMax(duckdb_v2_connection_handle conn, const char *name, MinMaxMode *mode, bool with_stats,
                    bool with_pushdown) {
	duckdb_v2_table_function_handle function = nullptr;
	REQUIRE(duckdb_v2_table_function_create(FactoryOf(conn), &function, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_name(function, name, std::strlen(name), nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_opaque user_data = {mode, nullptr, nullptr};
	REQUIRE(duckdb_v2_table_function_set_user_data(function, &user_data, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_bind_callback(function, MinMaxBindCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_init_global_callback(function, MinMaxInitGlobalCb, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_exec_callback(function, MinMaxExecCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	if (with_stats) {
		REQUIRE(duckdb_v2_table_function_set_stats_callback(function, MinMaxStatsCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	}
	if (with_pushdown) {
		REQUIRE(duckdb_v2_table_function_set_filter_pushdown_callback(function, MinMaxPushdownCb, nullptr) ==
		        DUCKDB_V2_ERROR_NONE);
	}
	REQUIRE(duckdb_v2_connection_register_table_function(conn, function, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_table_function_destroy(&function);
}

} // namespace

TEST_CASE("V2 stats: table function min/max reach the optimizer", "[capi_v2][stats][statistics]") {
	EnvFixture fx;
	g_mm = MinMaxObserved {};

	static MinMaxMode report {true};
	RegisterMinMax(fx.conn, "mm_report", &report, true, false);
	static MinMaxMode silent {false};
	RegisterMinMax(fx.conn, "mm_silent", &silent, true, false);
	static MinMaxMode assigned {true, false, true};
	RegisterMinMax(fx.conn, "mm_assigned", &assigned, true, false);
	static MinMaxMode backslash {true, false, false, true};
	RegisterMinMax(fx.conn, "mm_backslash", &backslash, true, false);

	// The data is 7; the reported minimum 20 makes "i < 15" unsatisfiable, so acting on the statistics yields 0 and the
	// scan never runs.
	REQUIRE(StatsQueryI64(fx.conn, "SELECT count(*) FROM mm_report() WHERE i < 15") == 0);
	REQUIRE(g_mm.set_rc == DUCKDB_V2_ERROR_NONE);
	REQUIRE(g_mm.exec_calls == 0);
	// "i > 100" exceeds the reported maximum 30: the scan does not run for it either.
	g_mm = MinMaxObserved {};
	REQUIRE(StatsQueryI64(fx.conn, "SELECT count(*) FROM mm_report() WHERE i > 100") == 0);
	REQUIRE(g_mm.exec_calls == 0);
	REQUIRE(g_mm.user_data == &report);
	REQUIRE(g_mm.column_index == 0);
	REQUIRE(g_mm.result_text == "{'min': 20, 'max': 30, 'has_null': false, 'has_no_null': true}");
	REQUIRE(g_mm.string_on_numeric_rc == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(g_mm.destroy_borrowed_rc == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(g_mm.borrowed_survives);

	// The data is 'zebra'; the reported maximum 'pear' makes "s > 'tiger'" unsatisfiable.
	g_mm = MinMaxObserved {};
	REQUIRE(StatsQueryI64(fx.conn, "SELECT count(*) FROM mm_report() WHERE s > 'tiger'") == 0);
	REQUIRE(g_mm.set_rc == DUCKDB_V2_ERROR_NONE);
	REQUIRE(g_mm.column_index == 2);
	REQUIRE(g_mm.result_text ==
	        "{'min': apple, 'max': pear, 'has_unicode': true, 'has_null': false, 'has_no_null': true}");
	REQUIRE(g_mm.exec_calls == 0);
	// 'banana' lies within the bounds: the scan runs
	g_mm = MinMaxObserved {};
	REQUIRE(StatsQueryI64(fx.conn, "SELECT count(*) FROM mm_report() WHERE s > 'banana'") == 3);
	REQUIRE(g_mm.exec_calls > 0);

	// The TUPLE column holds (7, 'zebra'); the statistics of its elements bound them to [20, 30] and ['apple', 'pear'],
	// so a filter on either element is answered from them.
	g_mm = MinMaxObserved {};
	REQUIRE(StatsQueryI64(fx.conn, "SELECT count(*) FROM mm_report() WHERE t[1] < 15") == 0);
	REQUIRE(g_mm.set_rc == DUCKDB_V2_ERROR_NONE);
	REQUIRE(g_mm.column_index == 3);
	REQUIRE(g_mm.result_text == "{'child_stats': ({'min': 20, 'max': 30, 'has_null': false, 'has_no_null': true}, "
	                            "{'min': apple, 'max': pear, 'has_unicode': true, 'has_null': false, 'has_no_null': "
	                            "true}), 'has_null': false, 'has_no_null': true}");
	REQUIRE(g_mm.exec_calls == 0);
	g_mm = MinMaxObserved {};
	REQUIRE(StatsQueryI64(fx.conn, "SELECT count(*) FROM mm_report() WHERE t[2] > 'tiger'") == 0);
	REQUIRE(g_mm.exec_calls == 0);
	// 25 lies within the bounds of the first element: the scan runs
	g_mm = MinMaxObserved {};
	REQUIRE(StatsQueryI64(fx.conn, "SELECT count(*) FROM mm_report() WHERE t[1] < 25") == 3);
	REQUIRE(g_mm.exec_calls > 0);

	// A string bound is the bytes it is given: the backslash in a\x41 is a character, not an escape, so the rows that
	// hold exactly those five characters stay, while 'aA' (what the escape would decode to) is excluded. (The text
	// rendering quotes such a string and doubles its backslash.)
	g_mm = MinMaxObserved {};
	REQUIRE(StatsQueryI64(fx.conn, "SELECT count(*) FROM mm_backslash() WHERE s = 'a\\x41'") == 3);
	REQUIRE(g_mm.set_rc == DUCKDB_V2_ERROR_NONE);
	REQUIRE(g_mm.result_text ==
	        "{'min': 'a\\\\x41', 'max': 'a\\\\x41', 'has_unicode': true, 'has_null': false, 'has_no_null': true}");
	REQUIRE(g_mm.exec_calls > 0);
	g_mm = MinMaxObserved {};
	REQUIRE(StatsQueryI64(fx.conn, "SELECT count(*) FROM mm_backslash() WHERE s = 'aA'") == 0);
	REQUIRE(g_mm.exec_calls == 0);

	// A function whose callback leaves the statistics unknown is scanned and returns every row.
	g_mm = MinMaxObserved {};
	REQUIRE(StatsQueryI64(fx.conn, "SELECT count(*) FROM mm_silent() WHERE i < 15") == 3);
	REQUIRE(g_mm.exec_calls > 0);

	// Statistics the function built itself and assigned to the result act the same.
	g_mm = MinMaxObserved {};
	REQUIRE(StatsQueryI64(fx.conn, "SELECT count(*) FROM mm_assigned() WHERE i < 15") == 0);
	REQUIRE(g_mm.assign_rc == DUCKDB_V2_ERROR_NONE);
	REQUIRE(g_mm.result_text == "{'min': 20, 'max': 30, 'has_null': false, 'has_no_null': true}");
	REQUIRE(g_mm.exec_calls == 0);
}

TEST_CASE("V2 stats: a predicate is checked against statistics", "[capi_v2][stats][statistics]") {
	EnvFixture fx;
	g_mm = MinMaxObserved {};

	static MinMaxMode mode {false};
	RegisterMinMax(fx.conn, "mm_filter", &mode, false, true);

	// The pushdown callback checks "i < 15" against min 20 / max 30: no row can satisfy it.
	(void)StatsQueryI64(fx.conn, "SELECT count(*) FROM mm_filter() WHERE i < 15");
	REQUIRE(g_mm.pushdown_checks >= 1);
	REQUIRE(g_mm.check_result == DUCKDB_V2_FILTER_PROPAGATE_RESULT_ALWAYS_FALSE);
	// DOUBLE statistics for the INTEGER column are refused
	REQUIRE(g_mm.type_mismatch_rc == DUCKDB_V2_ERROR_INPUT_INVALID);

	// A predicate over two columns cannot be checked against the statistics of one: the rows stay.
	g_mm = MinMaxObserved {};
	REQUIRE(StatsQueryI64(fx.conn, "SELECT count(*) FROM mm_filter() WHERE i <= j") == 3);
	REQUIRE(g_mm.check_error_rc == DUCKDB_V2_ERROR_INPUT_INVALID);

	// Statistics merged from [5, 10] and [1, 20] span [1, 20]: "i < 1" cannot match, "i < 2" can.
	static MinMaxMode merged {false, true};
	RegisterMinMax(fx.conn, "mm_merged", &merged, false, true);
	g_mm = MinMaxObserved {};
	(void)StatsQueryI64(fx.conn, "SELECT count(*) FROM mm_merged() WHERE i < 1");
	REQUIRE(g_mm.merge_rc == DUCKDB_V2_ERROR_NONE);
	REQUIRE(g_mm.pushdown_checks >= 1);
	REQUIRE(g_mm.check_result == DUCKDB_V2_FILTER_PROPAGATE_RESULT_ALWAYS_FALSE);
	g_mm = MinMaxObserved {};
	(void)StatsQueryI64(fx.conn, "SELECT count(*) FROM mm_merged() WHERE i < 2");
	REQUIRE(g_mm.merge_rc == DUCKDB_V2_ERROR_NONE);
	REQUIRE(g_mm.pushdown_checks >= 1);
	REQUIRE(g_mm.check_result != DUCKDB_V2_FILTER_PROPAGATE_RESULT_ALWAYS_FALSE);
	// The merged maximum is 20: "i > 20" cannot match, "i > 19" can.
	g_mm = MinMaxObserved {};
	(void)StatsQueryI64(fx.conn, "SELECT count(*) FROM mm_merged() WHERE i > 20");
	REQUIRE(g_mm.pushdown_checks >= 1);
	REQUIRE(g_mm.check_result == DUCKDB_V2_FILTER_PROPAGATE_RESULT_ALWAYS_FALSE);
	g_mm = MinMaxObserved {};
	(void)StatsQueryI64(fx.conn, "SELECT count(*) FROM mm_merged() WHERE i > 19");
	REQUIRE(g_mm.pushdown_checks >= 1);
	REQUIRE(g_mm.check_result != DUCKDB_V2_FILTER_PROPAGATE_RESULT_ALWAYS_FALSE);
}

// ---------------------------------------------------------------------------
// Shredded VARIANT: a table function with one VARIANT column reports a shredded layout, and checks a predicate on an
// extracted field against the same statistics.
// ---------------------------------------------------------------------------

namespace {

struct ShreddedObserved {
	idx_t exec_calls = 0;
	DUCKDB_V2_ERROR malformed_rc = DUCKDB_V2_ERROR_NONE;
	DUCKDB_V2_ERROR set_rc = DUCKDB_V2_ERROR_NONE;
	std::string result_text;
	idx_t pushdown_checks = 0;
	DUCKDB_V2_ERROR check_rc = DUCKDB_V2_ERROR_NONE;
	DUCKDB_V2_FILTER_PROPAGATE_RESULT check_result = DUCKDB_V2_FILTER_PROPAGATE_RESULT_NO_PRUNING_POSSIBLE;
};

ShreddedObserved g_shredded;

// Every value is an object whose field x is an INTEGER in [1, 5], stored in the structured layout throughout.
ValueSpec ShreddedOnX() {
	auto x_stats = Obj({{"min", Int(1)}, {"max", Int(5)}, {"has_null", Bool(false)}, {"has_no_null", Bool(true)}});
	return Obj({{"has_null", Bool(false)},
	            {"shredding_state", Str("SHREDDED")},
	            {"fully_shredded", Obj({{"x", Obj({{"type", Str("INTEGER")}, {"stats", x_stats}})}})}});
}

void ShreddedBindCb(duckdb_v2_function_bind_info_handle, duckdb_v2_table_function_bind_info_handle result,
                    duckdb_v2_context_handle context, duckdb_v2_error_info_handle *err) {
	duckdb_v2_factory_handle factory = nullptr;
	duckdb_v2_logical_type_handle variant = nullptr;
	std::string_view variant_text = "VARIANT";
	if (duckdb_v2_context_get_factory(context, &factory, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_factory_create_type_from_text(factory, variant_text.data(), variant_text.size(), &variant, err) !=
	        DUCKDB_V2_ERROR_NONE) {
		return;
	}
	duckdb_v2_table_function_bind_add_result_column(result, "v", 1, variant, err);
	duckdb_v2_logical_type_destroy(&variant);
}

// Produces no rows: only the statistics matter.
void ShreddedExecCb(duckdb_v2_table_function_exec_info_handle info, duckdb_v2_context_handle,
                    duckdb_v2_error_info_handle *err) {
	g_shredded.exec_calls++;
	duckdb_v2_data_chunk_handle chunk = nullptr;
	duckdb_v2_vector_handle vec = nullptr;
	if (duckdb_v2_table_function_exec_get_output_chunk(info, &chunk, err) == DUCKDB_V2_ERROR_NONE &&
	    duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, err) == DUCKDB_V2_ERROR_NONE) {
		duckdb_v2_vector_set_size(vec, 0, err);
	}
}

void ShreddedStatsCb(duckdb_v2_table_function_stats_info_handle info, duckdb_v2_context_handle context,
                     duckdb_v2_error_info_handle *err) {
	duckdb_v2_stats_handle result = nullptr;
	duckdb_v2_factory_handle factory = nullptr;
	if (duckdb_v2_table_function_stats_get_result_stats(info, &result, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_context_get_factory(context, &factory, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	// A nested type is not shredded on as a whole: an object is described by its fields.
	auto struct_leaf = Obj({{"type", Str("STRUCT(x INTEGER)")}, {"stats", Obj({})}});
	g_shredded.malformed_rc = SetStatsSpec(
	    factory, result, Obj({{"shredding_state", Str("SHREDDED")}, {"fully_shredded", struct_leaf}}), nullptr);
	g_shredded.set_rc = SetStatsSpec(factory, result, ShreddedOnX(), err);
	g_shredded.result_text = StatsText(result);
}

void ShreddedPushdownCb(duckdb_v2_table_function_filter_pushdown_info_handle info, duckdb_v2_context_handle context,
                        duckdb_v2_error_info_handle *err) {
	idx_t count = 0;
	duckdb_v2_factory_handle factory = nullptr;
	if (duckdb_v2_table_function_filter_pushdown_get_filter_count(info, &count, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_context_get_factory(context, &factory, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto variant = StatsOfText(factory, "VARIANT", err);
	if (variant && SetStatsSpec(factory, variant, ShreddedOnX(), err) != DUCKDB_V2_ERROR_NONE) {
		duckdb_v2_stats_destroy(&variant);
	}
	for (idx_t i = 0; i < count && variant; i++) {
		duckdb_v2_expression_handle filter = nullptr;
		if (duckdb_v2_table_function_filter_pushdown_get_filter(info, i, &filter, err) != DUCKDB_V2_ERROR_NONE) {
			break;
		}
		g_shredded.pushdown_checks++;
		g_shredded.check_rc =
		    duckdb_v2_expression_check_statistics(context, filter, variant, &g_shredded.check_result, nullptr);
	}
	duckdb_v2_stats_destroy(&variant);
}

void RegisterShredded(duckdb_v2_connection_handle conn, const char *name, bool with_stats, bool with_pushdown) {
	duckdb_v2_table_function_handle function = nullptr;
	REQUIRE(duckdb_v2_table_function_create(FactoryOf(conn), &function, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_name(function, name, std::strlen(name), nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_bind_callback(function, ShreddedBindCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_exec_callback(function, ShreddedExecCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	if (with_stats) {
		REQUIRE(duckdb_v2_table_function_set_stats_callback(function, ShreddedStatsCb, nullptr) ==
		        DUCKDB_V2_ERROR_NONE);
	}
	if (with_pushdown) {
		REQUIRE(duckdb_v2_table_function_set_filter_pushdown_callback(function, ShreddedPushdownCb, nullptr) ==
		        DUCKDB_V2_ERROR_NONE);
	}
	REQUIRE(duckdb_v2_connection_register_table_function(conn, function, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_table_function_destroy(&function);
}

} // namespace

TEST_CASE("V2 stats: shredded VARIANT statistics reach variant_extract", "[capi_v2][stats][statistics]") {
	EnvFixture fx;
	g_shredded = ShreddedObserved {};

	RegisterShredded(fx.conn, "vs_report", true, false);
	RegisterShredded(fx.conn, "vs_filter", false, true);

	// x is fully shredded and never NULL: "variant_extract(v, 'x') IS NULL" is unsatisfiable, so the scan never runs.
	REQUIRE(StatsQueryI64(fx.conn, "SELECT count(*) FROM vs_report() WHERE variant_extract(v, 'x') IS NULL") == 0);
	REQUIRE(g_shredded.malformed_rc == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(g_shredded.set_rc == DUCKDB_V2_ERROR_NONE);
	REQUIRE(g_shredded.result_text ==
	        "{'shredding_state': SHREDDED, 'fully_shredded': {'x': {'type': INTEGER, 'stats': {'min': 1, 'max': 5, "
	        "'has_null': false, 'has_no_null': true}}}, 'has_null': false, 'has_no_null': true}");
	REQUIRE(g_shredded.exec_calls == 0);
	// "IS NOT NULL" always holds: nothing is pruned and the (empty) function is scanned.
	REQUIRE(StatsQueryI64(fx.conn, "SELECT count(*) FROM vs_report() WHERE variant_extract(v, 'x') IS NOT NULL") == 0);
	REQUIRE(g_shredded.exec_calls > 0);

	// The pushdown callback checks the same predicate against the statistics it built.
	g_shredded = ShreddedObserved {};
	REQUIRE(StatsQueryI64(fx.conn, "SELECT count(*) FROM vs_filter() WHERE variant_extract(v, 'x') IS NULL") == 0);
	REQUIRE(g_shredded.pushdown_checks >= 1);
	REQUIRE(g_shredded.check_rc == DUCKDB_V2_ERROR_NONE);
	REQUIRE(g_shredded.check_result == DUCKDB_V2_FILTER_PROPAGATE_RESULT_ALWAYS_FALSE);
}

// ---------------------------------------------------------------------------
// Owned statistics, driven directly.
// ---------------------------------------------------------------------------

namespace {

// Sets statistics from the value a spec describes and returns the error message, or an empty string on success.
std::string SetStatsSpecMessage(duckdb_v2_factory_handle factory, duckdb_v2_stats_handle stats, const ValueSpec &spec,
                                DUCKDB_V2_ERROR &rc) {
	duckdb_v2_error_info_handle err = nullptr;
	rc = SetStatsSpec(factory, stats, spec, &err);
	std::string message;
	if (err) {
		const char *text = nullptr;
		idx_t text_length = 0;
		duckdb_v2_error_info_get_text(err, &text, &text_length);
		message = std::string(text, text_length);
	}
	duckdb_v2_error_info_destroy(&err);
	return message;
}

// Owned statistics of the type with the same bounds as `stats`, set from what `stats` reads back as.
duckdb_v2_stats_handle CopyThroughValue(duckdb_v2_factory_handle factory, duckdb_v2_stats_handle stats,
                                        const char *type_text) {
	duckdb_v2_value_handle value = nullptr;
	REQUIRE(duckdb_v2_stats_get_value(stats, &value, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto copy = StatsOfText(factory, type_text, nullptr);
	REQUIRE(copy != nullptr);
	REQUIRE(duckdb_v2_stats_set_value(copy, value, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_value_destroy(&value);
	return copy;
}

} // namespace

TEST_CASE("V2 stats: owned statistics narrow, merge and assign", "[capi_v2][stats][statistics]") {
	EnvFixture fx;

	auto stats = StatsOfText(fx.factory, "INTEGER", nullptr);
	REQUIRE(stats != nullptr);

	// Created statistics start out unknown and are writable.
	bool can_have_null = false;
	REQUIRE(duckdb_v2_stats_can_have_null(stats, &can_have_null, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(can_have_null);
	REQUIRE(StatsText(stats) == "{'has_null': true, 'has_no_null': true}");

	// A STRUCT value built with the value API sets the parts it mentions, the rest keeps its value.
	auto five = MakeInt32Value(fx.factory, 5);
	auto ten = MakeInt32Value(fx.factory, 10);
	REQUIRE(SetStatsStruct(fx.factory, stats, {"min", "max"}, {five, ten}, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(StatsText(stats) == "{'min': 5, 'max': 10, 'has_null': true, 'has_no_null': true}");
	REQUIRE(duckdb_v2_stats_set_can_have_null(stats, false, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(SetStatsSpec(fx.factory, stats, Obj({{"approx_unique", Int(6)}}), nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(StatsText(stats) == "{'min': 5, 'max': 10, 'has_null': false, 'has_no_null': true, 'approx_unique': 6}");
	// A bound of another type is cast to the column's type
	REQUIRE(SetStatsSpec(fx.factory, stats, Obj({{"min", Dbl(4.0)}}), nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(StatsText(stats) == "{'min': 4, 'max': 10, 'has_null': false, 'has_no_null': true, 'approx_unique': 6}");
	// A NULL field leaves that statistic unchanged
	duckdb_v2_logical_type_handle integer_type = nullptr;
	duckdb_v2_value_handle null_int = nullptr;
	auto eight = MakeInt32Value(fx.factory, 8);
	REQUIRE(duckdb_v2_factory_create_type_from_id(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER, nullptr, nullptr,
	                                              nullptr, 0, &integer_type, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_value_create_null(fx.factory, integer_type, &null_int, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(SetStatsStruct(fx.factory, stats, {"min", "max"}, {null_int, eight}, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(StatsText(stats) == "{'min': 4, 'max': 8, 'has_null': false, 'has_no_null': true, 'approx_unique': 6}");
	REQUIRE(SetStatsSpec(fx.factory, stats, Obj({{"max", Null()}, {"approx_unique", Null()}, {"has_null", Bool(true)}}),
	                     nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(StatsText(stats) == "{'min': 4, 'max': 8, 'has_null': true, 'has_no_null': true, 'approx_unique': 6}");
	REQUIRE(duckdb_v2_stats_set_can_have_null(stats, false, nullptr) == DUCKDB_V2_ERROR_NONE);

	// The output of stats() is accepted too: the same struct as a VARIANT value sets the same statistics
	duckdb_v2_logical_type_handle variant_type = nullptr;
	std::string_view variant_text = "VARIANT";
	REQUIRE(duckdb_v2_factory_create_type_from_text(fx.factory, variant_text.data(), variant_text.size(), &variant_type,
	                                                nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_value_handle bounds = nullptr;
	duckdb_v2_value_handle bounds_variant = nullptr;
	StrArray bound_names {"min", "max"};
	const duckdb_v2_value_handle bound_values[] = {five, ten};
	REQUIRE(duckdb_v2_value_create_struct(fx.factory, bound_names.ptrs(), bound_names.lengths(), bound_values, 2,
	                                      &bounds, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_value_cast(fx.factory, bounds, variant_type, &bounds_variant, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_stats_set_value(stats, bounds_variant, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(StatsText(stats) == "{'min': 5, 'max': 10, 'has_null': false, 'has_no_null': true, 'approx_unique': 6}");

	// Merge in a wider part of the column: the bounds widen to [1, 20], and NULLs can occur again.
	auto other = IntStats(fx.factory, 1, 20, nullptr);
	REQUIRE(duckdb_v2_stats_set_can_have_null(other, true, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_stats_merge(stats, other, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_stats_can_have_null(stats, &can_have_null, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(can_have_null);
	REQUIRE(StatsText(stats) == "{'min': 1, 'max': 20, 'has_null': true, 'has_no_null': true, 'approx_unique': 6}");

	// Assigning replaces everything: the NULL-free [1, 20] of "other" is taken over, after narrowing it.
	REQUIRE(duckdb_v2_stats_set_can_have_null(other, false, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_stats_assign(stats, other, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(StatsText(stats) == "{'min': 1, 'max': 20, 'has_null': false, 'has_no_null': true}");

	// Statistics of another type do not merge or assign.
	auto text_stats = StatsOfText(fx.factory, "VARCHAR", nullptr);
	REQUIRE(duckdb_v2_stats_merge(stats, text_stats, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_stats_assign(stats, text_stats, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	// Refused values name the field, and leave the statistics as they were.
	DUCKDB_V2_ERROR rc;
	auto message = SetStatsSpecMessage(fx.factory, stats, Obj({{"min", Str("not an int")}}), rc);
	REQUIRE(rc == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(message.find("'min'") != std::string::npos);
	message = SetStatsSpecMessage(fx.factory, stats, Obj({{"min", Int(0)}, {"has_unicode", Bool(false)}}), rc);
	REQUIRE(rc == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(message.find("'has_unicode'") != std::string::npos);
	REQUIRE(StatsText(stats) == "{'min': 1, 'max': 20, 'has_null': false, 'has_no_null': true}");
	REQUIRE(SetStatsSpecMessage(fx.factory, stats, Obj({{"has_null", Str("maybe")}}), rc).find("'has_null'") !=
	        std::string::npos);
	REQUIRE(rc == DUCKDB_V2_ERROR_INPUT_INVALID);
	// Values that are not a struct of statistics: a number, a list, and text, which is not parsed but refused by type
	REQUIRE(duckdb_v2_stats_set_value(stats, five, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(SetStatsSpec(fx.factory, stats, Arr({Int(1), Int(2)}), nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	message = SetStatsSpecMessage(fx.factory, stats, Str("{\"min\": 20, \"max\": 30}"), rc);
	REQUIRE(rc == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(message.find("VARCHAR") != std::string::npos);
	message = SetStatsSpecMessage(fx.factory, stats, Str("{'min': 20, 'max': 30}"), rc);
	REQUIRE(rc == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(message.find("VARCHAR") != std::string::npos);
	duckdb_v2_value_handle null_variant = nullptr;
	REQUIRE(duckdb_v2_value_create_null(fx.factory, variant_type, &null_variant, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_stats_set_value(stats, null_variant, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_stats_set_value(stats, null_int, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(StatsText(stats) == "{'min': 1, 'max': 20, 'has_null': false, 'has_no_null': true}");
	// An empty struct describes nothing
	REQUIRE(SetStatsStruct(fx.factory, stats, {}, {}, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(StatsText(stats) == "{'min': 1, 'max': 20, 'has_null': false, 'has_no_null': true}");

	duckdb_v2_value_destroy(&null_variant);
	duckdb_v2_value_destroy(&null_int);
	duckdb_v2_value_destroy(&bounds_variant);
	duckdb_v2_value_destroy(&bounds);
	duckdb_v2_logical_type_destroy(&variant_type);
	duckdb_v2_logical_type_destroy(&integer_type);
	duckdb_v2_value_destroy(&five);
	duckdb_v2_value_destroy(&ten);
	duckdb_v2_value_destroy(&eight);
	duckdb_v2_stats_destroy(&text_stats);
	duckdb_v2_stats_destroy(&other);
	duckdb_v2_stats_destroy(&stats);
	REQUIRE(stats == nullptr);

	// Null-safety of destroy.
	REQUIRE(duckdb_v2_stats_destroy(nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_stats_handle already_null = nullptr;
	REQUIRE(duckdb_v2_stats_destroy(&already_null) == DUCKDB_V2_ERROR_NONE);
}

TEST_CASE("V2 stats: bounds keep the column's type", "[capi_v2][stats][statistics]") {
	EnvFixture fx;

	// A DATE bound is a DATE value, or text that casts to one; a number is not a date.
	auto dates = StatsOfText(fx.factory, "DATE", nullptr);
	REQUIRE(dates != nullptr);
	duckdb_v2_value_handle first = nullptr;
	const int32_t days_2024_01_01 = 19723;
	REQUIRE(duckdb_v2_value_create_date(fx.factory, days_2024_01_01, &first, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto last = MakeVarcharValue(fx.factory, "2024-12-31");
	REQUIRE(SetStatsStruct(fx.factory, dates, {"min", "max"}, {first, last}, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(StatsText(dates) == "{'min': 2024-01-01, 'max': 2024-12-31, 'has_null': true, 'has_no_null': true}");
	auto min_field = StatsField(dates, "min");
	REQUIRE(min_field != nullptr);
	REQUIRE(TypeIdOf(min_field) == DUCKDB_V2_LOGICAL_TYPE_ID_DATE);
	int32_t min_days = 0;
	REQUIRE(duckdb_v2_value_get_date(min_field, &min_days, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(min_days == days_2024_01_01);
	duckdb_v2_value_destroy(&min_field);
	DUCKDB_V2_ERROR rc;
	auto message = SetStatsSpecMessage(fx.factory, dates, Obj({{"min", Int(0)}}), rc);
	REQUIRE(rc == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(message.find("'min'") != std::string::npos);
	REQUIRE(StatsText(dates) == "{'min': 2024-01-01, 'max': 2024-12-31, 'has_null': true, 'has_no_null': true}");
	duckdb_v2_value_destroy(&first);
	duckdb_v2_value_destroy(&last);
	duckdb_v2_stats_destroy(&dates);

	// A DECIMAL bound is a DECIMAL value of the column's width and scale.
	auto decimals = StatsOfText(fx.factory, "DECIMAL(10, 2)", nullptr);
	REQUIRE(SetStatsSpec(fx.factory, decimals, Obj({{"min", Dbl(1.5)}, {"max", Dbl(2.25)}}), nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	auto max_field = StatsField(decimals, "max");
	REQUIRE(max_field != nullptr);
	REQUIRE(TypeIdOf(max_field) == DUCKDB_V2_LOGICAL_TYPE_ID_DECIMAL);
	REQUIRE(Render(max_field) == "2.25");
	duckdb_v2_value_destroy(&max_field);
	duckdb_v2_stats_destroy(&decimals);
}

TEST_CASE("V2 stats: string bounds are the bytes they are given", "[capi_v2][stats][statistics]") {
	EnvFixture fx;

	// A VARCHAR bound is taken verbatim: a backslash is a character, not the start of an escape.
	auto text = StatsOfText(fx.factory, "VARCHAR", nullptr);
	REQUIRE(text != nullptr);
	REQUIRE(SetTextBounds(fx.factory, text, "C:\\temp", "a\\x41", nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(StatsText(text) ==
	        "{'min': 'C:\\\\temp', 'max': 'a\\\\x41', 'has_unicode': true, 'has_null': false, 'has_no_null': true}");
	auto field = StatsField(text, "min");
	REQUIRE(field != nullptr);
	REQUIRE(TypeIdOf(field) == DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR);
	REQUIRE(ConsumeValue<std::string>(field) == "C:\\temp");
	field = StatsField(text, "max");
	REQUIRE(ConsumeValue<std::string>(field) == "a\\x41");
	// What was read back sets the same bounds again
	auto copy = CopyThroughValue(fx.factory, text, "VARCHAR");
	REQUIRE(StatsText(copy) == StatsText(text));
	duckdb_v2_stats_destroy(&copy);

	// Valid UTF-8 beyond ASCII stays a VARCHAR; a BLOB value with valid UTF-8 bytes reads back as a VARCHAR too.
	const unsigned char e_acute[] = {0xC3, 0xA9};
	auto lower = MakeBlobValue(fx.factory, e_acute, sizeof(e_acute));
	auto upper = MakeVarcharValue(fx.factory, "\xC3\xA9z");
	REQUIRE(SetStatsStruct(fx.factory, text, {"min", "max"}, {lower, upper}, nullptr) == DUCKDB_V2_ERROR_NONE);
	field = StatsField(text, "min");
	REQUIRE(TypeIdOf(field) == DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR);
	REQUIRE(ConsumeValue<std::string>(field) == "\xC3\xA9");
	field = StatsField(text, "max");
	REQUIRE(ConsumeValue<std::string>(field) == "\xC3\xA9z");
	duckdb_v2_value_destroy(&lower);
	duckdb_v2_value_destroy(&upper);

	// Bytes that are not valid UTF-8 (as a truncated prefix of a VARCHAR may be) are given and read back as a BLOB.
	const unsigned char byte_80[] = {0x61, 0x80};
	const unsigned char byte_ff[] = {0xFF};
	lower = MakeBlobValue(fx.factory, byte_80, sizeof(byte_80));
	upper = MakeBlobValue(fx.factory, byte_ff, sizeof(byte_ff));
	REQUIRE(SetStatsStruct(fx.factory, text, {"min", "max"}, {lower, upper}, nullptr) == DUCKDB_V2_ERROR_NONE);
	field = StatsField(text, "min");
	REQUIRE(TypeIdOf(field) == DUCKDB_V2_LOGICAL_TYPE_ID_BLOB);
	REQUIRE(ConsumeBlob(field) == std::string("a\x80", 2));
	field = StatsField(text, "max");
	REQUIRE(TypeIdOf(field) == DUCKDB_V2_LOGICAL_TYPE_ID_BLOB);
	REQUIRE(ConsumeBlob(field) == std::string("\xFF", 1));
	copy = CopyThroughValue(fx.factory, text, "VARCHAR");
	REQUIRE(StatsText(copy) == StatsText(text));
	field = StatsField(copy, "min");
	REQUIRE(ConsumeBlob(field) == std::string("a\x80", 2));
	duckdb_v2_stats_destroy(&copy);
	duckdb_v2_value_destroy(&lower);
	duckdb_v2_value_destroy(&upper);
	duckdb_v2_stats_destroy(&text);

	// The bounds of a BLOB column are BLOB values; a VARCHAR bound on it is its characters, escapes included.
	auto blobs = StatsOfText(fx.factory, "BLOB", nullptr);
	REQUIRE(blobs != nullptr);
	const unsigned char nul_backslash[] = {0x00, 0x5C};
	lower = MakeBlobValue(fx.factory, nul_backslash, sizeof(nul_backslash));
	upper = MakeBlobValue(fx.factory, byte_ff, sizeof(byte_ff));
	REQUIRE(SetStatsStruct(fx.factory, blobs, {"min", "max"}, {lower, upper}, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(StatsText(blobs) == "{'min': '\\\\x00\\\\x5C', 'max': '\\\\xFF', 'has_unicode': true, 'has_null': true, "
	                            "'has_no_null': true}");
	field = StatsField(blobs, "min");
	REQUIRE(TypeIdOf(field) == DUCKDB_V2_LOGICAL_TYPE_ID_BLOB);
	REQUIRE(ConsumeBlob(field) == std::string("\x00\x5C", 2));
	copy = CopyThroughValue(fx.factory, blobs, "BLOB");
	REQUIRE(StatsText(copy) == StatsText(blobs));
	duckdb_v2_stats_destroy(&copy);
	duckdb_v2_value_destroy(&lower);
	duckdb_v2_value_destroy(&upper);
	auto escaped = MakeVarcharValue(fx.factory, "\\x00");
	REQUIRE(SetStatsStruct(fx.factory, blobs, {"max"}, {escaped}, nullptr) == DUCKDB_V2_ERROR_NONE);
	field = StatsField(blobs, "max");
	REQUIRE(TypeIdOf(field) == DUCKDB_V2_LOGICAL_TYPE_ID_BLOB);
	REQUIRE(ConsumeBlob(field) == "\\x00");
	duckdb_v2_value_destroy(&escaped);
	duckdb_v2_stats_destroy(&blobs);
}

TEST_CASE("V2 stats: statistics of every kind round-trip through a value", "[capi_v2][stats][statistics]") {
	EnvFixture fx;

	// Each case: the type, the struct to set, and what the statistics read back as.
	struct RoundTrip {
		const char *type;
		ValueSpec value;
		const char *expected;
	};
	auto n_leaf =
	    Obj({{"type", Str("BIGINT")}, {"stats", Obj({{"min", Int(1)}, {"max", Int(9)}, {"has_null", Bool(false)}})}});
	auto s_leaf = Obj({{"type", Str("VARCHAR")}, {"stats", Obj({{"min", Str("a")}, {"max", Str("c")}})}});
	const RoundTrip cases[] = {
	    {"DECIMAL(10, 2)", Obj({{"min", Dbl(1.5)}, {"max", Dbl(2.25)}, {"has_null", Bool(false)}}),
	     "{'min': 1.50, 'max': 2.25, 'has_null': false, 'has_no_null': true}"},
	    {"DATE", Obj({{"min", Str("2024-01-01")}, {"max", Str("2024-12-31")}}),
	     "{'min': 2024-01-01, 'max': 2024-12-31, 'has_null': true, 'has_no_null': true}"},
	    // a single bound is kept, but only rendered once both are known
	    {"DOUBLE", Obj({{"max", Dbl(1.5)}}), "{'has_null': true, 'has_no_null': true}"},
	    {"VARCHAR",
	     Obj({{"min", Str("a")},
	          {"max", Str("z")},
	          {"has_unicode", Bool(false)},
	          {"min_string_length", Int(1)},
	          {"max_string_length", Int(8)},
	          {"total_string_length", Int(1234)}}),
	     "{'min': a, 'max': z, 'has_unicode': false, 'min_string_length': 1, 'max_string_length': 8, "
	     "'total_string_length': 1234, 'has_null': true, 'has_no_null': true}"},
	    // a backslash in a string bound is a character
	    {"VARCHAR", Obj({{"min", Str("C:\\temp")}, {"max", Str("a\\x41")}}),
	     "{'min': 'C:\\\\temp', 'max': 'a\\\\x41', 'has_unicode': true, 'has_null': true, 'has_no_null': true}"},
	    {"INTEGER[]",
	     Obj({{"child_stats", Obj({{"min", Int(1)}, {"max", Int(3)}, {"has_null", Bool(false)}})},
	          {"has_null", Bool(false)}}),
	     "{'child_stats': {'min': 1, 'max': 3, 'has_null': false, 'has_no_null': true}, 'has_null': false, "
	     "'has_no_null': true}"},
	    // list lengths are statistics of the list itself, beside the statistics of its elements
	    {"INTEGER[]",
	     Obj({{"min_list_length", Int(0)},
	          {"max_list_length", Int(4)},
	          {"total_list_length", Int(17)},
	          {"child_stats", Obj({{"min", Int(1)}, {"max", Int(3)}})}}),
	     "{'min_list_length': 0, 'max_list_length': 4, 'total_list_length': 17, 'child_stats': {'min': 1, 'max': 3, "
	     "'has_null': true, 'has_no_null': true}, 'has_null': true, 'has_no_null': true}"},
	    // a single length bound is kept as well
	    {"VARCHAR[]", Obj({{"max_list_length", Int(2)}}),
	     "{'max_list_length': 2, 'child_stats': {'has_unicode': true, 'has_null': true, 'has_no_null': true}, "
	     "'has_null': true, 'has_no_null': true}"},
	    {"INTEGER[2]", Obj({{"child_stats", Obj({{"min", Int(1)}, {"max", Int(3)}})}}),
	     "{'child_stats': {'min': 1, 'max': 3, 'has_null': true, 'has_no_null': true}, 'has_null': true, "
	     "'has_no_null': true}"},
	    {"STRUCT(a INTEGER, b VARCHAR)",
	     Obj({{"child_stats", Obj({{"b", Obj({{"min", Str("d")}, {"max", Str("m")}, {"has_null", Bool(false)}})}})}}),
	     "{'child_stats': {'a': {'has_null': true, 'has_no_null': true}, 'b': {'min': d, 'max': m, 'has_unicode': "
	     "true, "
	     "'has_null': false, 'has_no_null': true}}, 'has_null': true, 'has_no_null': true}"},
	    // a NULL child leaves that field's statistics unchanged
	    {"STRUCT(a INTEGER, b VARCHAR)",
	     Obj({{"child_stats", Obj({{"a", Null()}, {"b", Obj({{"min", Str("d")}, {"max", Str("m")}})}})}}),
	     "{'child_stats': {'a': {'has_null': true, 'has_no_null': true}, 'b': {'min': d, 'max': m, 'has_unicode': "
	     "true, "
	     "'has_null': true, 'has_no_null': true}}, 'has_null': true, 'has_no_null': true}"},
	    {"MAP(VARCHAR, INTEGER)",
	     Obj({{"child_stats", Obj({{"child_stats", Obj({{"value", Obj({{"min", Int(0)}, {"max", Int(7)}})}})}})}}),
	     "{'child_stats': {'child_stats': {'key': {'has_unicode': true, 'has_null': true, 'has_no_null': true}, "
	     "'value': {'min': 0, 'max': 7, 'has_null': true, 'has_no_null': true}}, 'has_null': true, "
	     "'has_no_null': true}, 'has_null': true, 'has_no_null': true}"},
	    // the elements of a TUPLE are positional: a TUPLE of statistics, a NULL element leaving one unchanged
	    {"TUPLE(INTEGER, VARCHAR)",
	     Obj({{"child_stats", Tup({Obj({{"min", Int(1)}, {"max", Int(9)}, {"has_null", Bool(false)}}),
	                               Obj({{"min", Str("d")}, {"max", Str("m")}})})}}),
	     "{'child_stats': ({'min': 1, 'max': 9, 'has_null': false, 'has_no_null': true}, {'min': d, 'max': m, "
	     "'has_unicode': true, 'has_null': true, 'has_no_null': true}), 'has_null': true, 'has_no_null': true}"},
	    {"TUPLE(INTEGER, VARCHAR)", Obj({{"child_stats", Tup({Null(), Obj({{"min", Str("d")}, {"max", Str("m")}})})}}),
	     "{'child_stats': ({'has_null': true, 'has_no_null': true}, {'min': d, 'max': m, 'has_unicode': true, "
	     "'has_null': true, 'has_no_null': true}), 'has_null': true, 'has_no_null': true}"},
	    // a LIST of statistics is positional as well (stats() renders the elements as an array)
	    {"TUPLE(INTEGER, INTEGER)",
	     Obj({{"child_stats",
	           Arr({Obj({{"min", Int(2)}, {"max", Int(3)}}), Obj({{"min", Int(5)}, {"max", Int(6)}})})}}),
	     "{'child_stats': ({'min': 2, 'max': 3, 'has_null': true, 'has_no_null': true}, {'min': 5, 'max': 6, "
	     "'has_null': true, 'has_no_null': true}), 'has_null': true, 'has_no_null': true}"},
	    // a TUPLE inside a STRUCT, and a LIST of single-element TUPLEs
	    {"STRUCT(a TUPLE(INTEGER, DOUBLE))",
	     Obj({{"child_stats",
	           Obj({{"a", Obj({{"child_stats", Tup({Obj({{"min", Int(1)}, {"max", Int(2)}}), Null()})}})}})}}),
	     "{'child_stats': {'a': {'child_stats': ({'min': 1, 'max': 2, 'has_null': true, 'has_no_null': true}, "
	     "{'has_null': true, 'has_no_null': true}), 'has_null': true, 'has_no_null': true}}, 'has_null': true, "
	     "'has_no_null': true}"},
	    {"TUPLE(INTEGER)[]",
	     Obj({{"child_stats",
	           Obj({{"child_stats", Tup({Obj({{"min", Int(4)}, {"max", Int(8)}, {"has_null", Bool(false)}})})}})}}),
	     "{'child_stats': {'child_stats': ({'min': 4, 'max': 8, 'has_null': false, 'has_no_null': true},), "
	     "'has_null': true, 'has_no_null': true}, 'has_null': true, 'has_no_null': true}"},
	    {"GEOMETRY",
	     Obj({{"extent", Obj({{"x_min", Int(0)},
	                          {"x_max", Int(10)},
	                          {"y_min", Int(-5)},
	                          {"y_max", Int(5)},
	                          {"z_min", Int(1)},
	                          {"z_max", Int(2)}})},
	          {"types", Arr({Str("point"), Str("polygon_z")})},
	          {"has_empty_geom", Bool(false)},
	          {"has_empty_part", Bool(false)}}),
	     "{'extent': {'x_min': 0.0, 'x_max': 10.0, 'y_min': -5.0, 'y_max': 5.0, 'z_min': 1.0, 'z_max': 2.0}, "
	     "'types': [point, polygon_z], 'has_empty_geom': false, 'has_non_empty_geom': true, 'has_empty_part': false, "
	     "'has_non_empty_part': true, 'has_null': true, 'has_no_null': true}"},
	    {"VARIANT", Obj({{"shredding_state", Str("NOT_SHREDDED")}}),
	     "{'shredding_state': NOT_SHREDDED, 'has_null': true, 'has_no_null': true}"},
	    // a list of objects whose field n is a BIGINT throughout
	    {"VARIANT",
	     Obj({{"shredding_state", Str("SHREDDED")}, {"fully_shredded", Obj({{"child_stats", Obj({{"n", n_leaf}})}})}}),
	     "{'shredding_state': SHREDDED, 'fully_shredded': {'child_stats': {'n': {'type': BIGINT, 'stats': {'min': 1, "
	     "'max': 9, 'has_null': false, 'has_no_null': true}}}}, 'has_null': true, 'has_no_null': true}"},
	    // a list of objects whose field s is a VARCHAR for some values, with others in the generic layout
	    {"VARIANT",
	     Obj({{"shredding_state", Str("SHREDDED")},
	          {"partially_shredded", Obj({{"child_stats", Obj({{"s", s_leaf}})}})}}),
	     "{'shredding_state': SHREDDED, 'partially_shredded': {'child_stats': {'s': {'type': VARCHAR, 'stats': "
	     "{'min': a, 'max': c, 'has_unicode': true, 'has_null': true, 'has_no_null': true}}}}, 'has_null': true, "
	     "'has_no_null': true}"},
	    // objects whose fields are named type and stats: their values are nodes, not a type name
	    {"VARIANT", Obj({{"fully_shredded", Obj({{"type", s_leaf}, {"stats", n_leaf}})}}),
	     "{'shredding_state': SHREDDED, 'fully_shredded': {'stats': {'type': BIGINT, 'stats': {'min': 1, 'max': 9, "
	     "'has_null': false, 'has_no_null': true}}, 'type': {'type': VARCHAR, 'stats': {'min': a, 'max': c, "
	     "'has_unicode': true, 'has_null': true, 'has_no_null': true}}}, 'has_null': true, 'has_no_null': true}"},
	};
	for (auto &test : cases) {
		auto stats = StatsOfText(fx.factory, test.type, nullptr);
		REQUIRE(stats != nullptr);
		DUCKDB_V2_ERROR rc;
		auto message = SetStatsSpecMessage(fx.factory, stats, test.value, rc);
		INFO(test.type << ": " << message);
		REQUIRE(rc == DUCKDB_V2_ERROR_NONE);
		REQUIRE(StatsText(stats) == test.expected);

		// What was read back sets the same statistics again
		auto again = CopyThroughValue(fx.factory, stats, test.type);
		REQUIRE(StatsText(again) == test.expected);
		duckdb_v2_stats_destroy(&again);
		duckdb_v2_stats_destroy(&stats);
	}

	// Unknown statistics read back as they are: a GEOMETRY extent renders its unknown bounds as infinities.
	auto geometry = StatsOfText(fx.factory, "GEOMETRY", nullptr);
	auto unknown_geometry = "{'extent': {'x_min': -inf, 'x_max': inf, 'y_min': -inf, 'y_max': inf}, 'has_empty_geom': "
	                        "true, 'has_non_empty_geom': true, 'has_empty_part': true, 'has_non_empty_part': true, "
	                        "'has_null': true, 'has_no_null': true}";
	REQUIRE(StatsText(geometry) == unknown_geometry);
	auto again = CopyThroughValue(fx.factory, geometry, "GEOMETRY");
	REQUIRE(StatsText(again) == unknown_geometry);
	duckdb_v2_stats_destroy(&again);
	// Unknown TUPLE statistics round-trip as well
	auto tuple = StatsOfText(fx.factory, "TUPLE(BIGINT, VARCHAR)", nullptr);
	auto unknown_tuple = "{'child_stats': ({'has_null': true, 'has_no_null': true}, {'has_unicode': true, 'has_null': "
	                     "true, 'has_no_null': true}), 'has_null': true, 'has_no_null': true}";
	REQUIRE(StatsText(tuple) == unknown_tuple);
	again = CopyThroughValue(fx.factory, tuple, "TUPLE(BIGINT, VARCHAR)");
	REQUIRE(StatsText(again) == unknown_tuple);
	duckdb_v2_stats_destroy(&again);
	duckdb_v2_stats_destroy(&tuple);

	// Malformed parts are refused with a message naming the field, and leave the statistics as they were.
	struct Malformed {
		const char *type;
		ValueSpec value;
		const char *field;
	};
	auto empty_int_leaf = Obj({{"type", Str("INTEGER")}, {"stats", Obj({})}});
	const Malformed malformed[] = {
	    {"INTEGER[]", Obj({{"child_stats", Obj({{"min", Str("x")}})}}), "'child_stats.min'"},
	    {"INTEGER[]", Obj({{"child_stats", Int(5)}}), "'child_stats'"},
	    {"INTEGER[]", Obj({{"min_list_length", Str("short")}}), "'min_list_length'"},
	    {"INTEGER[]", Obj({{"total_list_length", Int(-1)}}), "'total_list_length'"},
	    {"STRUCT(a INTEGER)", Obj({{"child_stats", Obj({{"b", Obj({{"min", Int(1)}})}})}}), "'child_stats.b'"},
	    {"STRUCT(a INTEGER)", Obj({{"child_stats", Obj({{"a", Obj({{"min", Int(1)}, {"other", Int(2)}})}})}}),
	     "'child_stats.a.other'"},
	    // a TUPLE takes one statistics struct per element, by position
	    {"TUPLE(INTEGER, VARCHAR)", Obj({{"child_stats", Tup({Obj({{"min", Int(1)}})})}}), "'child_stats'"},
	    {"TUPLE(INTEGER, VARCHAR)", Obj({{"child_stats", Int(5)}}), "'child_stats'"},
	    {"TUPLE(INTEGER, VARCHAR)", Obj({{"child_stats", Obj({{"element1", Obj({{"min", Int(1)}})}})}}),
	     "'child_stats'"},
	    {"TUPLE(INTEGER, VARCHAR)",
	     Obj({{"child_stats", Tup({Obj({{"min", Int(1)}}), Obj({{"min", Str("a")}, {"other", Int(2)}})})}}),
	     "'child_stats.element2.other'"},
	    {"DATE", Obj({{"min", Str("2024-01-01")}, {"max", Int(20241231)}}), "'max'"},
	    {"GEOMETRY", Obj({{"extent", Obj({{"x_min", Str("left")}})}}), "'extent.x_min'"},
	    {"GEOMETRY", Obj({{"extent", Obj({{"w_min", Int(1)}})}}), "'extent.w_min'"},
	    {"GEOMETRY", Obj({{"types", Arr({Str("circle")})}}), "'types'"},
	    {"GEOMETRY", Obj({{"types", Str("point")}}), "'types'"},
	    {"VARIANT", Obj({{"shredding_state", Str("SOMEWHAT")}}), "'shredding_state'"},
	    {"VARIANT", Obj({{"shredding_state", Str("NOT_SHREDDED")}, {"fully_shredded", empty_int_leaf}}),
	     "'fully_shredded'"},
	    {"VARIANT", Obj({{"fully_shredded", Obj({{"type", Str("NOSUCHTYPE")}, {"stats", Obj({})}})}}),
	     "'fully_shredded.type'"},
	    {"VARIANT", Obj({{"fully_shredded", Obj({{"type", Str("VARIANT")}, {"stats", Obj({})}})}}),
	     "'fully_shredded.type'"},
	    {"VARIANT", Obj({{"fully_shredded", Obj({{"type", Str("INTEGER")}, {"stats", Obj({{"min", Str("x")}})}})}}),
	     "'fully_shredded.stats.min'"},
	    {"VARIANT", Obj({{"fully_shredded", Obj({{"x", Obj({{"type", Str("INTEGER")}})}})}}), "'fully_shredded.x"},
	    {"VARIANT", Obj({{"fully_shredded", Obj({})}}), "'fully_shredded'"},
	    // a NULL node describes nothing, so the tree is empty
	    {"VARIANT", Obj({{"fully_shredded", Obj({{"x", Null()}})}}), "'fully_shredded'"},
	    {"VARIANT", Obj({{"fully_shredded", empty_int_leaf}, {"partially_shredded", empty_int_leaf}}),
	     "'partially_shredded'"},
	};
	for (auto &test : malformed) {
		auto stats = StatsOfText(fx.factory, test.type, nullptr);
		REQUIRE(stats != nullptr);
		auto before = StatsText(stats);
		DUCKDB_V2_ERROR rc;
		auto message = SetStatsSpecMessage(fx.factory, stats, test.value, rc);
		INFO(test.type << " " << test.field << ": " << message);
		REQUIRE(rc == DUCKDB_V2_ERROR_INPUT_INVALID);
		REQUIRE(message.find(test.field) != std::string::npos);
		REQUIRE(StatsText(stats) == before);
		duckdb_v2_stats_destroy(&stats);
	}
	duckdb_v2_stats_destroy(&geometry);
}

TEST_CASE("V2 stats: the output of stats() sets the statistics it shows", "[capi_v2][stats][statistics]") {
	EnvFixture fx;
	StatsExec(fx.conn,
	          "CREATE TABLE pairs AS SELECT row(i, 'x' || i) AS r, {'n': i, 'p': row(i)} AS s FROM range(10) t(i)");

	// stats() renders statistics as a VARIANT, with the elements of a TUPLE's child_stats as an array
	struct Shown {
		const char *column;
		const char *type;
		const char *expected;
	};
	const Shown cases[] = {
	    {"r", "TUPLE(INTEGER, VARCHAR)",
	     "{'child_stats': ({'min': 0, 'max': 9, 'has_null': false, 'has_no_null': true}, {'min': x0, 'max': x9, "
	     "'has_unicode': false, 'min_string_length': 2, 'max_string_length': 2, 'total_string_length': 20, "
	     "'has_null': false, 'has_no_null': true}), 'has_null': false, 'has_no_null': true}"},
	    {"s", "STRUCT(n INTEGER, p TUPLE(INTEGER))",
	     "{'child_stats': {'n': {'min': 0, 'max': 9, 'has_null': false, 'has_no_null': true}, 'p': {'child_stats': "
	     "({'min': 0, 'max': 9, 'has_null': false, 'has_no_null': true},), 'has_null': false, 'has_no_null': true}}, "
	     "'has_null': false, 'has_no_null': true}"},
	};
	for (auto &test : cases) {
		auto shown = StatsQueryValue(fx.conn, std::string("SELECT stats(") + test.column + ") FROM pairs LIMIT 1");
		REQUIRE(shown != nullptr);
		REQUIRE(TypeIdOf(shown) == DUCKDB_V2_LOGICAL_TYPE_ID_VARIANT);
		auto stats = StatsOfText(fx.factory, test.type, nullptr);
		REQUIRE(stats != nullptr);
		duckdb_v2_error_info_handle err = nullptr;
		auto rc = duckdb_v2_stats_set_value(stats, shown, &err);
		std::string message;
		if (err) {
			const char *text = nullptr;
			idx_t text_length = 0;
			duckdb_v2_error_info_get_text(err, &text, &text_length);
			message = std::string(text, text_length);
		}
		duckdb_v2_error_info_destroy(&err);
		INFO(test.column << ": " << message);
		REQUIRE(rc == DUCKDB_V2_ERROR_NONE);
		REQUIRE(StatsText(stats) == test.expected);
		duckdb_v2_stats_destroy(&stats);
		duckdb_v2_value_destroy(&shown);
	}
}

} // namespace test_capi_v2
