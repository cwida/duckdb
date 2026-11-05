#include "test_capi_v2.hpp"

#include <cstring>
#include <string>

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

duckdb_v2_identifier_t StatsIdent(const char *s) {
	return duckdb_v2_identifier_t {s, std::strlen(s)};
}

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

void StatsFail(duckdb_v2_error_info_handle *err, const char *message) {
	duckdb_v2_error_info_set_code(*err, DUCKDB_V2_ERROR_API);
	auto text = Convert(message);
	duckdb_v2_error_info_set_text(*err, &text);
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
int64_t StatsQueryI64(duckdb_v2_connection_handle conn, const char *sql) {
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

void StatsExec(duckdb_v2_connection_handle conn, const char *sql) {
	duckdb_v2_result_handle result = nullptr;
	duckdb_v2_error_info_handle err = nullptr;
	auto rc = Query(conn, sql, &result, &err);
	std::string message;
	if (err) {
		duckdb_v2_str text = {nullptr, 0};
		duckdb_v2_error_info_get_text(err, &text);
		message = Convert(text);
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
std::string StatsQueryError(duckdb_v2_connection_handle conn, const char *sql) {
	duckdb_v2_result_handle result = nullptr;
	duckdb_v2_error_info_handle err = nullptr;
	auto rc = Query(conn, sql, &result, &err);
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

void RegisterStatsScalar(duckdb_v2_connection_handle conn, const char *name, StatsMode *mode) {
	auto integer = MakeType(FactoryOf(conn), DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	duckdb_v2_scalar_function_handle function = nullptr;
	REQUIRE(duckdb_v2_scalar_function_create(FactoryOf(conn), &function, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto name_str = Convert(name);
	REQUIRE(duckdb_v2_scalar_function_set_name(function, &name_str, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_function_signature_handle sig = nullptr;
	REQUIRE(duckdb_v2_scalar_function_get_signature(function, &sig, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto param = StatsIdent("x");
	REQUIRE(duckdb_v2_function_signature_add_parameter(sig, &param, integer, nullptr,
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

void RegisterStatsAgg(duckdb_v2_connection_handle conn, const char *name, StatsMode *mode) {
	auto integer = MakeType(FactoryOf(conn), DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	auto bigint = MakeType(FactoryOf(conn), DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT);
	duckdb_v2_aggregate_function_handle function = nullptr;
	REQUIRE(duckdb_v2_aggregate_function_create(FactoryOf(conn), &function, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto name_str = Convert(name);
	REQUIRE(duckdb_v2_aggregate_function_set_name(function, &name_str, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_function_signature_handle sig = nullptr;
	REQUIRE(duckdb_v2_aggregate_function_get_signature(function, &sig, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto param = StatsIdent("x");
	REQUIRE(duckdb_v2_function_signature_add_parameter(sig, &param, integer, nullptr,
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
	    duckdb_v2_factory_create_type_from_id(factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER, nullptr, nullptr, 0, &integer,
	                                          err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto a = StatsIdent("a");
	auto b = StatsIdent("b");
	if (duckdb_v2_table_function_bind_add_result_column(result, &a, integer, err) == DUCKDB_V2_ERROR_NONE) {
		duckdb_v2_table_function_bind_add_result_column(result, &b, integer, err);
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

void RegisterStatsPair(duckdb_v2_connection_handle conn, const char *name, StatsMode *mode) {
	duckdb_v2_table_function_handle function = nullptr;
	REQUIRE(duckdb_v2_table_function_create(FactoryOf(conn), &function, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto name_str = Convert(name);
	REQUIRE(duckdb_v2_table_function_set_name(function, &name_str, nullptr) == DUCKDB_V2_ERROR_NONE);
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

} // namespace test_capi_v2
