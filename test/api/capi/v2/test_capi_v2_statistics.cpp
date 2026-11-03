#include "test_capi_v2.hpp"

#include <cstring>

// ---------------------------------------------------------------------------
// V2 statistics tests. A table function reports per-column min/max statistics to the optimizer through its
// "statistics" callback. The function deliberately produces data its statistics exclude, so a query whose filter the
// statistics make unsatisfiable returns a wrong count exactly when the optimizer acted on them - which makes their
// effect observable. The statistics object and duckdb_v2_expression_check_statistics are exercised directly.
//
// Callbacks avoid Catch assertions: a REQUIRE would throw through the C callback boundary into the engine.
// Observations are latched into file-scope statics and asserted after the query.
// ---------------------------------------------------------------------------

namespace test_capi_v2 {

namespace {

duckdb_v2_identifier_t StatIdent(const char *s) {
	return duckdb_v2_identifier_t {s, std::strlen(s)};
}

int64_t CountI64(duckdb_v2_connection_handle conn, const char *sql) {
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

// What a function's statistics callback does, passed as its user data.
struct MinMaxMode {
	bool report = true;
	// The pushdown callback checks against statistics merged from [5, 10] and [1, 20] instead of [20, 30].
	bool merged = false;
};

// What the last statistics / pushdown callback saw.
struct MinMaxObserved {
	idx_t statistics_calls = 0;
	idx_t exec_calls = 0;
	DUCKDB_V2_ERROR check_error_rc = DUCKDB_V2_ERROR_NONE;
	DUCKDB_V2_ERROR type_mismatch_rc = DUCKDB_V2_ERROR_NONE;
	void *user_data = nullptr;
	idx_t column_index = 999;
	DUCKDB_V2_LOGICAL_TYPE_ID column_type = DUCKDB_V2_LOGICAL_TYPE_ID_INVALID;
	DUCKDB_V2_LOGICAL_TYPE_ID stats_type = DUCKDB_V2_LOGICAL_TYPE_ID_INVALID;
	DUCKDB_V2_ERROR string_on_numeric_rc = DUCKDB_V2_ERROR_NONE;
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
	auto i = StatIdent("i");
	auto j = StatIdent("j");
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

void MinMaxStatisticsCb(duckdb_v2_table_function_statistics_info_handle info, duckdb_v2_context_handle context,
                        duckdb_v2_error_info_handle *err) {
	g_mm.statistics_calls++;
	void *user_data = nullptr;
	idx_t column = 999;
	duckdb_v2_logical_type_handle column_type = nullptr;
	if (duckdb_v2_table_function_statistics_get_user_data(info, &user_data, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_table_function_statistics_get_column_index(info, &column, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_table_function_statistics_get_column_type(info, &column_type, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	g_mm.user_data = user_data;
	g_mm.column_index = column;
	duckdb_v2_logical_type_get_id(column_type, &g_mm.column_type, err);

	auto *mode = static_cast<MinMaxMode *>(user_data);
	if (!mode || !mode->report) {
		duckdb_v2_logical_type_destroy(&column_type);
		return;
	}

	duckdb_v2_factory_handle factory = nullptr;
	duckdb_v2_statistics_handle statistics = nullptr;
	duckdb_v2_value_handle min_value = nullptr;
	duckdb_v2_value_handle max_value = nullptr;
	if (duckdb_v2_context_get_factory(context, &factory, err) == DUCKDB_V2_ERROR_NONE &&
	    duckdb_v2_statistics_create(factory, column_type, &statistics, err) == DUCKDB_V2_ERROR_NONE &&
	    duckdb_v2_value_create_int(factory, 20, &min_value, err) == DUCKDB_V2_ERROR_NONE &&
	    duckdb_v2_value_create_int(factory, 30, &max_value, err) == DUCKDB_V2_ERROR_NONE) {
		duckdb_v2_statistics_set_min(statistics, min_value, err);
		duckdb_v2_statistics_set_max(statistics, max_value, err);
		duckdb_v2_statistics_set_has_no_null(statistics, true, err);

		duckdb_v2_logical_type_handle got_type = nullptr;
		if (duckdb_v2_statistics_get_type(statistics, &got_type, err) == DUCKDB_V2_ERROR_NONE) {
			duckdb_v2_logical_type_get_id(got_type, &g_mm.stats_type, err);
			duckdb_v2_logical_type_destroy(&got_type);
		}
		// A string bound on numeric statistics is refused.
		auto text = Convert("x");
		g_mm.string_on_numeric_rc = duckdb_v2_statistics_set_string_min(statistics, &text, true, nullptr);

		duckdb_v2_table_function_statistics_set_result(info, statistics, err);
	}
	duckdb_v2_statistics_destroy(&statistics);
	duckdb_v2_value_destroy(&min_value);
	duckdb_v2_value_destroy(&max_value);
	duckdb_v2_logical_type_destroy(&column_type);
}

// INTEGER statistics bounded by [min, max] without NULLs, or null when they cannot be built.
duckdb_v2_statistics_handle IntStatistics(duckdb_v2_factory_handle factory, duckdb_v2_logical_type_handle integer,
                                          int32_t min, int32_t max, duckdb_v2_error_info_handle *err) {
	duckdb_v2_statistics_handle statistics = nullptr;
	duckdb_v2_value_handle min_value = nullptr;
	duckdb_v2_value_handle max_value = nullptr;
	if (duckdb_v2_statistics_create(factory, integer, &statistics, err) == DUCKDB_V2_ERROR_NONE &&
	    duckdb_v2_value_create_int(factory, min, &min_value, err) == DUCKDB_V2_ERROR_NONE &&
	    duckdb_v2_value_create_int(factory, max, &max_value, err) == DUCKDB_V2_ERROR_NONE) {
		duckdb_v2_statistics_set_min(statistics, min_value, err);
		duckdb_v2_statistics_set_max(statistics, max_value, err);
		duckdb_v2_statistics_set_has_no_null(statistics, true, err);
	} else {
		duckdb_v2_statistics_destroy(&statistics);
	}
	duckdb_v2_value_destroy(&min_value);
	duckdb_v2_value_destroy(&max_value);
	return statistics;
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
	duckdb_v2_statistics_handle statistics = nullptr;
	if (mode && mode->merged) {
		// Merging [1, 20] into [5, 10] widens the bounds to [1, 20].
		statistics = IntStatistics(factory, integer, 5, 10, err);
		auto other = IntStatistics(factory, integer, 1, 20, err);
		g_mm.merge_rc = duckdb_v2_statistics_merge(statistics, other, err);
		duckdb_v2_statistics_destroy(&other);
	} else {
		statistics = IntStatistics(factory, integer, 20, 30, err);
	}
	if (statistics) {
		for (idx_t i = 0; i < count; i++) {
			duckdb_v2_expression_handle filter = nullptr;
			if (duckdb_v2_table_function_filter_pushdown_get_filter(info, i, &filter, err) != DUCKDB_V2_ERROR_NONE) {
				break;
			}
			DUCKDB_V2_FILTER_PROPAGATE_RESULT outcome = DUCKDB_V2_FILTER_PROPAGATE_RESULT_NO_PRUNING_POSSIBLE;
			auto rc = duckdb_v2_expression_check_statistics(context, filter, statistics, &outcome, nullptr);
			if (rc == DUCKDB_V2_ERROR_NONE) {
				g_mm.pushdown_checks++;
				g_mm.check_result = outcome;
			} else {
				g_mm.check_error_rc = rc;
			}
			// Statistics of another type than the column are refused
			duckdb_v2_logical_type_handle double_type = nullptr;
			duckdb_v2_statistics_handle double_statistics = nullptr;
			if (duckdb_v2_factory_create_type_from_id(factory, DUCKDB_V2_LOGICAL_TYPE_ID_DOUBLE, nullptr, nullptr, 0,
			                                          &double_type, nullptr) == DUCKDB_V2_ERROR_NONE &&
			    duckdb_v2_statistics_create(factory, double_type, &double_statistics, nullptr) ==
			        DUCKDB_V2_ERROR_NONE) {
				g_mm.type_mismatch_rc =
				    duckdb_v2_expression_check_statistics(context, filter, double_statistics, &outcome, nullptr);
			}
			duckdb_v2_statistics_destroy(&double_statistics);
			duckdb_v2_logical_type_destroy(&double_type);
		}
	}
	duckdb_v2_statistics_destroy(&statistics);
	duckdb_v2_logical_type_destroy(&integer);
}

void RegisterMinMax(duckdb_v2_connection_handle conn, const char *name, MinMaxMode *mode, bool with_statistics,
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
	if (with_statistics) {
		REQUIRE(duckdb_v2_table_function_set_statistics_callback(function, MinMaxStatisticsCb, nullptr) ==
		        DUCKDB_V2_ERROR_NONE);
	}
	if (with_pushdown) {
		REQUIRE(duckdb_v2_table_function_set_filter_pushdown_callback(function, MinMaxPushdownCb, nullptr) ==
		        DUCKDB_V2_ERROR_NONE);
	}
	REQUIRE(duckdb_v2_connection_register_table_function(conn, function, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_table_function_destroy(&function);
}

} // namespace

TEST_CASE("V2 statistics: table function min/max reach the optimizer", "[capi_v2][statistics]") {
	EnvFixture fx;
	g_mm = MinMaxObserved {};

	MinMaxMode report {true};
	RegisterMinMax(fx.conn, "mm_report", &report, true, false);
	MinMaxMode silent {false};
	RegisterMinMax(fx.conn, "mm_silent", &silent, true, false);

	// The data is 7; the reported minimum 20 makes "i < 15" unsatisfiable, so acting on the statistics yields 0 and the
	// scan never runs.
	REQUIRE(CountI64(fx.conn, "SELECT count(*) FROM mm_report() WHERE i < 15") == 0);
	REQUIRE(g_mm.exec_calls == 0);
	// And "i > 25" keeps rows the maximum 30 allows, but the data has none: a wrong 0 only if max were acted on
	// wrongly.
	REQUIRE(CountI64(fx.conn, "SELECT count(*) FROM mm_report() WHERE i > 100") == 0);
	REQUIRE(g_mm.user_data == &report);
	REQUIRE(g_mm.column_index == 0);
	REQUIRE(g_mm.column_type == DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	REQUIRE(g_mm.stats_type == DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	REQUIRE(g_mm.string_on_numeric_rc == DUCKDB_V2_ERROR_INPUT_INVALID);

	// A function whose callback reports nothing leaves the statistics unknown: the scan runs and returns every row.
	REQUIRE(CountI64(fx.conn, "SELECT count(*) FROM mm_silent() WHERE i < 15") == 3);
	REQUIRE(g_mm.exec_calls > 0);
}

TEST_CASE("V2 statistics: a predicate is checked against statistics", "[capi_v2][statistics]") {
	EnvFixture fx;
	g_mm = MinMaxObserved {};

	MinMaxMode mode {false};
	RegisterMinMax(fx.conn, "mm_filter", &mode, false, true);

	// The pushdown callback checks "i < 15" against min 20 / max 30: no row can satisfy it.
	(void)CountI64(fx.conn, "SELECT count(*) FROM mm_filter() WHERE i < 15");
	REQUIRE(g_mm.pushdown_checks >= 1);
	REQUIRE(g_mm.check_result == DUCKDB_V2_FILTER_PROPAGATE_RESULT_ALWAYS_FALSE);
	// DOUBLE statistics for the INTEGER column are refused
	REQUIRE(g_mm.type_mismatch_rc == DUCKDB_V2_ERROR_INPUT_INVALID);

	// A predicate over two columns cannot be checked against the statistics of one: the rows stay.
	g_mm = MinMaxObserved {};
	REQUIRE(CountI64(fx.conn, "SELECT count(*) FROM mm_filter() WHERE i <= j") == 3);
	REQUIRE(g_mm.check_error_rc == DUCKDB_V2_ERROR_INPUT_INVALID);

	// Statistics merged from [5, 10] and [1, 20] span [1, 20]: "i < 1" cannot match, "i < 2" can.
	MinMaxMode merged {false, true};
	RegisterMinMax(fx.conn, "mm_merged", &merged, false, true);
	g_mm = MinMaxObserved {};
	(void)CountI64(fx.conn, "SELECT count(*) FROM mm_merged() WHERE i < 1");
	REQUIRE(g_mm.merge_rc == DUCKDB_V2_ERROR_NONE);
	REQUIRE(g_mm.pushdown_checks >= 1);
	REQUIRE(g_mm.check_result == DUCKDB_V2_FILTER_PROPAGATE_RESULT_ALWAYS_FALSE);
	g_mm = MinMaxObserved {};
	(void)CountI64(fx.conn, "SELECT count(*) FROM mm_merged() WHERE i < 2");
	REQUIRE(g_mm.merge_rc == DUCKDB_V2_ERROR_NONE);
	REQUIRE(g_mm.pushdown_checks >= 1);
	REQUIRE(g_mm.check_result != DUCKDB_V2_FILTER_PROPAGATE_RESULT_ALWAYS_FALSE);
}

TEST_CASE("V2 statistics: the statistics object narrows and merges", "[capi_v2][statistics]") {
	EnvFixture fx;

	duckdb_v2_logical_type_handle integer = nullptr;
	REQUIRE(duckdb_v2_factory_create_type_from_id(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER, nullptr, nullptr, 0,
	                                              &integer, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_statistics_handle stats = nullptr;
	REQUIRE(duckdb_v2_statistics_create(fx.factory, integer, &stats, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_logical_type_handle stats_type = nullptr;
	REQUIRE(duckdb_v2_statistics_get_type(stats, &stats_type, nullptr) == DUCKDB_V2_ERROR_NONE);
	DUCKDB_V2_LOGICAL_TYPE_ID id = DUCKDB_V2_LOGICAL_TYPE_ID_INVALID;
	duckdb_v2_logical_type_get_id(stats_type, &id, nullptr);
	REQUIRE(id == DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	duckdb_v2_logical_type_destroy(&stats_type);

	duckdb_v2_value_handle five = nullptr;
	duckdb_v2_value_handle ten = nullptr;
	REQUIRE(duckdb_v2_value_create_int(fx.factory, 5, &five, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_value_create_int(fx.factory, 10, &ten, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_statistics_set_min(stats, five, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_statistics_set_max(stats, ten, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_statistics_set_has_no_null(stats, true, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_statistics_set_distinct_count(stats, 6, nullptr) == DUCKDB_V2_ERROR_NONE);

	// Merge in a wider part of the column: the bounds widen to [1, 20].
	duckdb_v2_statistics_handle other = nullptr;
	REQUIRE(duckdb_v2_statistics_create(fx.factory, integer, &other, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_value_handle one = nullptr;
	duckdb_v2_value_handle twenty = nullptr;
	REQUIRE(duckdb_v2_value_create_int(fx.factory, 1, &one, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_value_create_int(fx.factory, 20, &twenty, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_statistics_set_min(other, one, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_statistics_set_max(other, twenty, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_statistics_merge(stats, other, nullptr) == DUCKDB_V2_ERROR_NONE);

	// A VARCHAR value does not cast to the INTEGER statistics.
	duckdb_v2_value_handle text_value = nullptr;
	auto text = Convert("not an int");
	REQUIRE(duckdb_v2_value_create_varchar(fx.factory, &text, &text_value, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_statistics_set_min(stats, text_value, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	duckdb_v2_value_destroy(&text_value);
	duckdb_v2_value_destroy(&five);
	duckdb_v2_value_destroy(&ten);
	duckdb_v2_value_destroy(&one);
	duckdb_v2_value_destroy(&twenty);
	duckdb_v2_statistics_destroy(&other);
	duckdb_v2_statistics_destroy(&stats);
	duckdb_v2_logical_type_destroy(&integer);

	// Null-safety of destroy.
	REQUIRE(duckdb_v2_statistics_destroy(nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_statistics_handle already_null = nullptr;
	REQUIRE(duckdb_v2_statistics_destroy(&already_null) == DUCKDB_V2_ERROR_NONE);
}

} // namespace test_capi_v2
