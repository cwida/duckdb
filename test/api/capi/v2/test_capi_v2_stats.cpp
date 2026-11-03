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

// ---------------------------------------------------------------------------
// Bounds: a table function reports min/max through the stats callback, and checks predicates against statistics it
// builds itself. The function produces data its statistics exclude, so a query whose filter the statistics make
// unsatisfiable returns a wrong count exactly when the optimizer acted on them.
// ---------------------------------------------------------------------------

namespace {

// What a function's stats / pushdown callback does, passed as its user data.
struct MinMaxMode {
	bool report = true;
	// The pushdown callback checks against statistics merged from [5, 10] and [1, 20] instead of [20, 30].
	bool merged = false;
	// The stats callback builds owned statistics and assigns them to the result instead of narrowing it in place.
	bool via_assign = false;
};

// What the last stats / pushdown callback saw.
struct MinMaxObserved {
	idx_t stats_calls = 0;
	idx_t exec_calls = 0;
	DUCKDB_V2_ERROR check_error_rc = DUCKDB_V2_ERROR_NONE;
	DUCKDB_V2_ERROR type_mismatch_rc = DUCKDB_V2_ERROR_NONE;
	void *user_data = nullptr;
	idx_t column_index = 999;
	DUCKDB_V2_LOGICAL_TYPE_ID stats_type = DUCKDB_V2_LOGICAL_TYPE_ID_INVALID;
	DUCKDB_V2_ERROR string_on_numeric_rc = DUCKDB_V2_ERROR_NONE;
	DUCKDB_V2_ERROR destroy_borrowed_rc = DUCKDB_V2_ERROR_NONE;
	bool borrowed_survives = false;
	DUCKDB_V2_ERROR assign_rc = DUCKDB_V2_ERROR_NONE;
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

void MinMaxBindCb(duckdb_v2_function_bind_info_handle, duckdb_v2_table_function_bind_info_handle result,
                  duckdb_v2_context_handle context, duckdb_v2_error_info_handle *err) {
	duckdb_v2_factory_handle factory = nullptr;
	duckdb_v2_logical_type_handle integer = nullptr;
	if (duckdb_v2_context_get_factory(context, &factory, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_factory_create_type_from_id(factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER, nullptr, nullptr, 0, &integer,
	                                          err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto i = StatsIdent("i");
	auto j = StatsIdent("j");
	duckdb_v2_table_function_bind_add_result_column(result, &i, integer, err);
	duckdb_v2_table_function_bind_add_result_column(result, &j, integer, err);
	duckdb_v2_logical_type_destroy(&integer);
}

void MinMaxInitGlobalCb(duckdb_v2_table_function_init_global_info_handle info, duckdb_v2_context_handle,
                        duckdb_v2_error_info_handle *err) {
	duckdb_v2_opaque state = {new MinMaxGlobal(), DeleteMinMaxGlobal, nullptr};
	duckdb_v2_table_function_init_global_set_global_state(info, &state, err);
}

void MinMaxExecCb(duckdb_v2_table_function_exec_info_handle info, duckdb_v2_context_handle,
                  duckdb_v2_error_info_handle *err) {
	void *global_ptr = nullptr;
	duckdb_v2_data_chunk_handle chunk = nullptr;
	if (duckdb_v2_table_function_exec_get_global_state(info, &global_ptr, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_table_function_exec_get_output_chunk(info, &chunk, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	g_mm.exec_calls++;
	auto &global = *static_cast<MinMaxGlobal *>(global_ptr);
	idx_t produced = global.produced < 3 ? 1 : 0;
	global.produced += produced;
	for (idx_t column = 0; column < 2; column++) {
		duckdb_v2_vector_handle vec = nullptr;
		void *raw = nullptr;
		if (duckdb_v2_data_chunk_get_vector(chunk, column, &vec, err) != DUCKDB_V2_ERROR_NONE ||
		    duckdb_v2_vector_get_data_mutable(vec, &raw, err) != DUCKDB_V2_ERROR_NONE) {
			return;
		}
		// Every row holds 7 - a value the reported statistics (min 20, max 30) exclude.
		for (idx_t i = 0; i < produced; i++) {
			static_cast<int32_t *>(raw)[i] = 7;
		}
		duckdb_v2_vector_set_size(vec, produced, err);
	}
}

// Narrows INTEGER statistics to [min, max] without NULLs.
void IntBounds(duckdb_v2_factory_handle factory, duckdb_v2_stats_handle stats, int32_t min, int32_t max,
               duckdb_v2_error_info_handle *err) {
	duckdb_v2_value_handle min_value = nullptr;
	duckdb_v2_value_handle max_value = nullptr;
	if (duckdb_v2_value_create_int(factory, min, &min_value, err) == DUCKDB_V2_ERROR_NONE &&
	    duckdb_v2_value_create_int(factory, max, &max_value, err) == DUCKDB_V2_ERROR_NONE) {
		duckdb_v2_stats_set_min(stats, min_value, err);
		duckdb_v2_stats_set_max(stats, max_value, err);
		duckdb_v2_stats_set_can_have_null(stats, false, err);
	}
	duckdb_v2_value_destroy(&min_value);
	duckdb_v2_value_destroy(&max_value);
}

// Owned INTEGER statistics bounded by [min, max] without NULLs, or null when they cannot be built.
duckdb_v2_stats_handle IntStats(duckdb_v2_factory_handle factory, duckdb_v2_logical_type_handle integer, int32_t min,
                                int32_t max, duckdb_v2_error_info_handle *err) {
	duckdb_v2_stats_handle stats = nullptr;
	if (duckdb_v2_stats_create(factory, integer, &stats, err) == DUCKDB_V2_ERROR_NONE) {
		IntBounds(factory, stats, min, max, err);
	}
	return stats;
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

	// The result statistics are of the declared column type.
	duckdb_v2_logical_type_handle stats_type = nullptr;
	if (duckdb_v2_stats_get_type(result, &stats_type, err) == DUCKDB_V2_ERROR_NONE) {
		duckdb_v2_logical_type_get_id(stats_type, &g_mm.stats_type, err);
	}

	auto *mode = static_cast<MinMaxMode *>(user_data);
	if (!mode || !mode->report) {
		duckdb_v2_logical_type_destroy(&stats_type);
		return;
	}

	if (mode->via_assign) {
		// Statistics built by the function (as combined from its files) are copied into the result.
		auto owned = IntStats(factory, stats_type, 20, 30, err);
		g_mm.assign_rc = duckdb_v2_stats_assign(result, owned, err);
		duckdb_v2_stats_destroy(&owned);
	} else {
		IntBounds(factory, result, 20, 30, err);
	}
	// A string bound on numeric statistics is refused.
	auto text = Convert("x");
	g_mm.string_on_numeric_rc = duckdb_v2_stats_set_string_min(result, &text, true, nullptr);
	// The borrowed result statistics cannot be destroyed, and stay usable.
	auto borrowed = result;
	g_mm.destroy_borrowed_rc = duckdb_v2_stats_destroy(&borrowed);
	bool can_have_null = true;
	g_mm.borrowed_survives = borrowed == result &&
	                         duckdb_v2_stats_can_have_null(result, &can_have_null, nullptr) == DUCKDB_V2_ERROR_NONE &&
	                         !can_have_null;
	duckdb_v2_logical_type_destroy(&stats_type);
}

void MinMaxPushdownCb(duckdb_v2_table_function_filter_pushdown_info_handle info, duckdb_v2_context_handle context,
                      duckdb_v2_error_info_handle *err) {
	idx_t count = 0;
	void *user_data = nullptr;
	duckdb_v2_factory_handle factory = nullptr;
	duckdb_v2_logical_type_handle integer = nullptr;
	if (duckdb_v2_table_function_filter_pushdown_get_filter_count(info, &count, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_table_function_filter_pushdown_get_user_data(info, &user_data, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_context_get_factory(context, &factory, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_factory_create_type_from_id(factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER, nullptr, nullptr, 0, &integer,
	                                          err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto *mode = static_cast<MinMaxMode *>(user_data);
	duckdb_v2_stats_handle stats = nullptr;
	if (mode && mode->merged) {
		// Merging [1, 20] into [5, 10] widens the bounds to [1, 20].
		stats = IntStats(factory, integer, 5, 10, err);
		auto other = IntStats(factory, integer, 1, 20, err);
		g_mm.merge_rc = duckdb_v2_stats_merge(stats, other, err);
		duckdb_v2_stats_destroy(&other);
	} else {
		stats = IntStats(factory, integer, 20, 30, err);
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
			duckdb_v2_logical_type_handle double_type = nullptr;
			duckdb_v2_stats_handle double_stats = nullptr;
			if (duckdb_v2_factory_create_type_from_id(factory, DUCKDB_V2_LOGICAL_TYPE_ID_DOUBLE, nullptr, nullptr, 0,
			                                          &double_type, nullptr) == DUCKDB_V2_ERROR_NONE &&
			    duckdb_v2_stats_create(factory, double_type, &double_stats, nullptr) == DUCKDB_V2_ERROR_NONE) {
				g_mm.type_mismatch_rc =
				    duckdb_v2_expression_check_statistics(context, filter, double_stats, &outcome, nullptr);
			}
			duckdb_v2_stats_destroy(&double_stats);
			duckdb_v2_logical_type_destroy(&double_type);
		}
	}
	duckdb_v2_stats_destroy(&stats);
	duckdb_v2_logical_type_destroy(&integer);
}

void RegisterMinMax(duckdb_v2_connection_handle conn, const char *name, MinMaxMode *mode, bool with_stats,
                    bool with_pushdown) {
	duckdb_v2_table_function_handle function = nullptr;
	REQUIRE(duckdb_v2_table_function_create(FactoryOf(conn), &function, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto name_str = Convert(name);
	REQUIRE(duckdb_v2_table_function_set_name(function, &name_str, nullptr) == DUCKDB_V2_ERROR_NONE);
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

	// The data is 7; the reported minimum 20 makes "i < 15" unsatisfiable, so acting on the statistics yields 0 and the
	// scan never runs.
	REQUIRE(StatsQueryI64(fx.conn, "SELECT count(*) FROM mm_report() WHERE i < 15") == 0);
	REQUIRE(g_mm.exec_calls == 0);
	// "i > 100" exceeds the maximum 30 as well: 0 again, whichever bound is acted on.
	REQUIRE(StatsQueryI64(fx.conn, "SELECT count(*) FROM mm_report() WHERE i > 100") == 0);
	REQUIRE(g_mm.user_data == &report);
	REQUIRE(g_mm.column_index == 0);
	REQUIRE(g_mm.stats_type == DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	REQUIRE(g_mm.string_on_numeric_rc == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(g_mm.destroy_borrowed_rc == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(g_mm.borrowed_survives);

	// A function whose callback leaves the statistics unknown is scanned and returns every row.
	REQUIRE(StatsQueryI64(fx.conn, "SELECT count(*) FROM mm_silent() WHERE i < 15") == 3);
	REQUIRE(g_mm.exec_calls > 0);

	// Statistics the function built itself and assigned to the result act the same.
	g_mm = MinMaxObserved {};
	REQUIRE(StatsQueryI64(fx.conn, "SELECT count(*) FROM mm_assigned() WHERE i < 15") == 0);
	REQUIRE(g_mm.assign_rc == DUCKDB_V2_ERROR_NONE);
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
}

TEST_CASE("V2 stats: owned statistics narrow, merge and assign", "[capi_v2][stats][statistics]") {
	EnvFixture fx;

	duckdb_v2_logical_type_handle integer = nullptr;
	REQUIRE(duckdb_v2_factory_create_type_from_id(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER, nullptr, nullptr, 0,
	                                              &integer, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_stats_handle stats = nullptr;
	REQUIRE(duckdb_v2_stats_create(fx.factory, integer, &stats, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_logical_type_handle stats_type = nullptr;
	REQUIRE(duckdb_v2_stats_get_type(stats, &stats_type, nullptr) == DUCKDB_V2_ERROR_NONE);
	DUCKDB_V2_LOGICAL_TYPE_ID id = DUCKDB_V2_LOGICAL_TYPE_ID_INVALID;
	duckdb_v2_logical_type_get_id(stats_type, &id, nullptr);
	REQUIRE(id == DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	duckdb_v2_logical_type_destroy(&stats_type);

	// Created statistics start out unknown and are writable.
	bool can_have_null = false;
	REQUIRE(duckdb_v2_stats_can_have_null(stats, &can_have_null, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(can_have_null);
	duckdb_v2_value_handle five = nullptr;
	duckdb_v2_value_handle ten = nullptr;
	REQUIRE(duckdb_v2_value_create_int(fx.factory, 5, &five, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_value_create_int(fx.factory, 10, &ten, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_stats_set_min(stats, five, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_stats_set_max(stats, ten, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_stats_set_can_have_null(stats, false, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_stats_set_distinct_count(stats, 6, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_stats_can_have_null(stats, &can_have_null, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(!can_have_null);

	// Merge in a wider part of the column: the bounds widen to [1, 20], and NULLs can occur again.
	duckdb_v2_stats_handle other = nullptr;
	REQUIRE(duckdb_v2_stats_create(fx.factory, integer, &other, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_value_handle one = nullptr;
	duckdb_v2_value_handle twenty = nullptr;
	REQUIRE(duckdb_v2_value_create_int(fx.factory, 1, &one, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_value_create_int(fx.factory, 20, &twenty, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_stats_set_min(other, one, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_stats_set_max(other, twenty, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_stats_merge(stats, other, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_stats_can_have_null(stats, &can_have_null, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(can_have_null);

	// Assigning replaces everything: the NULL-free [1, 20] of "other" is taken over, after narrowing it.
	REQUIRE(duckdb_v2_stats_set_can_have_null(other, false, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_stats_assign(stats, other, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_stats_can_have_null(stats, &can_have_null, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(!can_have_null);

	// A VARCHAR value does not cast to the INTEGER statistics, and VARCHAR statistics do not merge or assign.
	duckdb_v2_value_handle text_value = nullptr;
	auto text = Convert("not an int");
	REQUIRE(duckdb_v2_value_create_varchar(fx.factory, &text, &text_value, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_stats_set_min(stats, text_value, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	duckdb_v2_logical_type_handle varchar = nullptr;
	duckdb_v2_stats_handle text_stats = nullptr;
	REQUIRE(duckdb_v2_factory_create_type_from_id(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR, nullptr, nullptr, 0,
	                                              &varchar, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_stats_create(fx.factory, varchar, &text_stats, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_stats_set_string_min(text_stats, &text, true, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_stats_set_min(text_stats, five, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_stats_merge(stats, text_stats, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_stats_assign(stats, text_stats, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	duckdb_v2_value_destroy(&text_value);
	duckdb_v2_value_destroy(&five);
	duckdb_v2_value_destroy(&ten);
	duckdb_v2_value_destroy(&one);
	duckdb_v2_value_destroy(&twenty);
	duckdb_v2_stats_destroy(&text_stats);
	duckdb_v2_stats_destroy(&other);
	duckdb_v2_stats_destroy(&stats);
	REQUIRE(stats == nullptr);
	duckdb_v2_logical_type_destroy(&varchar);
	duckdb_v2_logical_type_destroy(&integer);

	// Null-safety of destroy.
	REQUIRE(duckdb_v2_stats_destroy(nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_stats_handle already_null = nullptr;
	REQUIRE(duckdb_v2_stats_destroy(&already_null) == DUCKDB_V2_ERROR_NONE);
}

TEST_CASE("V2 stats: nested, geometry and variant statistics", "[capi_v2][stats][statistics]") {
	EnvFixture fx;

	duckdb_v2_logical_type_handle integer = nullptr;
	REQUIRE(duckdb_v2_factory_create_type_from_id(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER, nullptr, nullptr, 0,
	                                              &integer, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_value_handle five = nullptr;
	REQUIRE(duckdb_v2_value_create_int(fx.factory, 5, &five, nullptr) == DUCKDB_V2_ERROR_NONE);

	// LIST(INTEGER): the element statistics are child 0 and must be INTEGER statistics.
	auto list_text = Convert("INTEGER[]");
	duckdb_v2_logical_type_handle list = nullptr;
	REQUIRE(duckdb_v2_factory_create_type_from_text(fx.factory, &list_text, &list, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_stats_handle list_stats = nullptr;
	duckdb_v2_stats_handle element_stats = nullptr;
	REQUIRE(duckdb_v2_stats_create(fx.factory, list, &list_stats, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_stats_create(fx.factory, integer, &element_stats, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_stats_set_constant(element_stats, five, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_stats_set_child(list_stats, 0, element_stats, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_stats_set_child(list_stats, 1, element_stats, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_stats_set_child(list_stats, 0, list_stats, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_stats_set_child(list_stats, 0, nullptr, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_stats_set_child(element_stats, 0, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_stats_set_min(list_stats, five, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	// GEOMETRY: an extent whose minimum exceeds its maximum is refused, as is an unknown WKB type code.
	auto geometry_text = Convert("GEOMETRY");
	duckdb_v2_logical_type_handle geometry = nullptr;
	REQUIRE(duckdb_v2_factory_create_type_from_text(fx.factory, &geometry_text, &geometry, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	duckdb_v2_stats_handle geometry_stats = nullptr;
	REQUIRE(duckdb_v2_stats_create(fx.factory, geometry, &geometry_stats, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_stats_set_geometry_extent(geometry_stats, 0, 10, 0, 10, true, 1, 2, false, 0, 0, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_stats_set_geometry_extent(geometry_stats, 10, 0, 0, 10, false, 0, 0, false, 0, 0, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	const int32_t point_and_polygon[] = {1, 2003};
	REQUIRE(duckdb_v2_stats_set_geometry_types(geometry_stats, point_and_polygon, 2, nullptr) == DUCKDB_V2_ERROR_NONE);
	// codes outside the ISO WKB table are refused, also a well-formed list holding one
	const int32_t rejected_codes[] = {9999, 0, 1000, -1, 8};
	for (auto code : rejected_codes) {
		const int32_t with_bogus[] = {1, code};
		REQUIRE(duckdb_v2_stats_set_geometry_types(geometry_stats, &code, 1, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
		REQUIRE(duckdb_v2_stats_set_geometry_types(geometry_stats, with_bogus, 2, nullptr) ==
		        DUCKDB_V2_ERROR_INPUT_INVALID);
	}
	const int32_t all_vertex_types[] = {1, 1001, 2001, 3007};
	REQUIRE(duckdb_v2_stats_set_geometry_types(geometry_stats, all_vertex_types, 4, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_stats_set_geometry_types(geometry_stats, nullptr, 0, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_stats_set_geometry_extent(list_stats, 0, 1, 0, 1, false, 0, 0, false, 0, 0, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);

	// VARIANT shredded on INTEGER: the shredded layout is STRUCT(typed_value INTEGER, untyped_value_index UINTEGER).
	duckdb_v2_logical_type_handle shredding = nullptr;
	REQUIRE(duckdb_v2_stats_variant_shredding_type(fx.factory, integer, &shredding, nullptr) == DUCKDB_V2_ERROR_NONE);
	DUCKDB_V2_LOGICAL_TYPE_ID shredding_id = DUCKDB_V2_LOGICAL_TYPE_ID_INVALID;
	duckdb_v2_logical_type_get_id(shredding, &shredding_id, nullptr);
	REQUIRE(shredding_id == DUCKDB_V2_LOGICAL_TYPE_ID_STRUCT);
	duckdb_v2_stats_handle shredded_stats = nullptr;
	duckdb_v2_stats_handle variant_stats = nullptr;
	REQUIRE(duckdb_v2_stats_create(fx.factory, shredding, &shredded_stats, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_stats_set_child(shredded_stats, 0, element_stats, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_stats_create_variant_shredded(fx.factory, shredded_stats, &variant_stats, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	duckdb_v2_logical_type_handle variant_type = nullptr;
	REQUIRE(duckdb_v2_stats_get_type(variant_stats, &variant_type, nullptr) == DUCKDB_V2_ERROR_NONE);
	DUCKDB_V2_LOGICAL_TYPE_ID variant_id = DUCKDB_V2_LOGICAL_TYPE_ID_INVALID;
	duckdb_v2_logical_type_get_id(variant_type, &variant_id, nullptr);
	REQUIRE(variant_id == DUCKDB_V2_LOGICAL_TYPE_ID_VARIANT);
	// Shredded VARIANT statistics claim nothing about NULLs until narrowed.
	bool can_have_null = false;
	bool can_have_valid = false;
	REQUIRE(duckdb_v2_stats_can_have_null(variant_stats, &can_have_null, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_stats_can_have_valid(variant_stats, &can_have_valid, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(can_have_null);
	REQUIRE(can_have_valid);
	REQUIRE(duckdb_v2_stats_set_can_have_null(variant_stats, false, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_stats_can_have_null(variant_stats, &can_have_null, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(!can_have_null);

	duckdb_v2_logical_type_destroy(&variant_type);
	duckdb_v2_stats_destroy(&variant_stats);
	duckdb_v2_stats_destroy(&shredded_stats);
	duckdb_v2_logical_type_destroy(&shredding);
	duckdb_v2_stats_destroy(&geometry_stats);
	duckdb_v2_logical_type_destroy(&geometry);
	duckdb_v2_stats_destroy(&element_stats);
	duckdb_v2_stats_destroy(&list_stats);
	duckdb_v2_logical_type_destroy(&list);
	duckdb_v2_value_destroy(&five);
	duckdb_v2_logical_type_destroy(&integer);
}

} // namespace test_capi_v2
