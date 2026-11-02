#include "catch.hpp"
#include "test_helpers.hpp"
#include "tpcds_extension.hpp"

using namespace duckdb;

TEST_CASE("Test TPC-DS dsdgen progress", "[tpcds][progress-bar][.]") {
	DuckDB db(nullptr);
	Connection con(db);
	if (!db.ExtensionIsLoaded("tpcds")) {
		return;
	}

	REQUIRE_NO_FAIL(con.Query("PRAGMA threads=1"));
	REQUIRE_NO_FAIL(con.Query("PRAGMA progress_bar_time=1"));
	REQUIRE_NO_FAIL(con.Query("PRAGMA disable_print_progress_bar"));

	auto pending = con.Submit("CALL dsdgen(sf=0.01, suffix='_progress')");
	double previous_percentage = -1;
	bool saw_intermediate_progress = false;
	bool saw_progress_before_ready = false;

	while (true) {
		auto state = pending->ExecuteTask();
		auto result_ready = IsObservable(state);
		auto query_progress = con.context->GetQueryProgress();
		auto percentage = query_progress.GetPercentage();
		if (percentage >= 0) {
			REQUIRE(percentage >= previous_percentage);
			REQUIRE(percentage <= 100);
			previous_percentage = percentage;
			if (percentage > 0 && percentage < 100) {
				saw_intermediate_progress = true;
			}
			if (!result_ready) {
				REQUIRE(percentage < 100);
			}
			if (!result_ready && percentage > 0) {
				saw_progress_before_ready = true;
			}
		}
		if (result_ready) {
			break;
		}
		if (state == QueryResultState::BLOCKED) {
			pending->WaitForTask();
		}
	}

	pending->Complete();
	auto &result = pending;
	REQUIRE_NO_FAIL(*result);
	REQUIRE(saw_intermediate_progress);
	REQUIRE(saw_progress_before_ready);
}

TEST_CASE("Test TPC-DS dsdgen parallel progress starts gradually", "[tpcds][progress-bar][.]") {
#ifdef DUCKDB_NO_THREADS
	return;
#else
	DuckDB db(nullptr);
	Connection con(db);
	if (!db.ExtensionIsLoaded("tpcds")) {
		return;
	}

	REQUIRE_NO_FAIL(con.Query("PRAGMA threads=4"));
	REQUIRE_NO_FAIL(con.Query("PRAGMA progress_bar_time=1"));
	REQUIRE_NO_FAIL(con.Query("PRAGMA disable_print_progress_bar"));

	auto pending = con.Submit("CALL dsdgen(sf=1, suffix='_parallel_progress')");
	double previous_percentage = -1;
	double first_positive_percentage = 101;
	double max_percentage_before_ready = 0;
	bool saw_intermediate_progress = false;
	bool saw_progress_before_ready = false;

	while (true) {
		auto state = pending->ExecuteTask();
		auto result_ready = IsObservable(state);
		auto query_progress = con.context->GetQueryProgress();
		auto percentage = query_progress.GetPercentage();
		if (percentage >= 0) {
			if (percentage < previous_percentage) {
				FAIL("TPC-DS parallel progress moved backwards");
			}
			if (percentage > 100) {
				FAIL("TPC-DS parallel progress exceeded 100%");
			}
			previous_percentage = percentage;
			if (percentage > 0 && percentage < 100) {
				saw_intermediate_progress = true;
				if (first_positive_percentage > 100) {
					first_positive_percentage = percentage;
				}
			}
			if (!result_ready && percentage >= 100) {
				FAIL("TPC-DS parallel progress completed before the query result was ready");
			}
			if (!result_ready && percentage > 0) {
				saw_progress_before_ready = true;
				if (percentage > max_percentage_before_ready) {
					max_percentage_before_ready = percentage;
				}
			}
		}
		if (result_ready) {
			break;
		}
		if (state == QueryResultState::BLOCKED) {
			pending->WaitForTask();
		}
	}

	pending->Complete();
	auto &result = pending;
	REQUIRE_NO_FAIL(*result);
	REQUIRE(saw_intermediate_progress);
	REQUIRE(saw_progress_before_ready);
	REQUIRE(first_positive_percentage < 30);
	REQUIRE(max_percentage_before_ready > 80);
#endif
}

TEST_CASE("Test TPC-DS dsdgen parallel output matches sequential output", "[tpcds][.]") {
#ifdef DUCKDB_NO_THREADS
	return;
#else
	DuckDB db(nullptr);
	Connection con(db);
	if (!db.ExtensionIsLoaded("tpcds")) {
		return;
	}

	REQUIRE_NO_FAIL(con.Query("PRAGMA threads=1"));
	REQUIRE_NO_FAIL(con.Query("CALL dsdgen(sf=1, suffix='_seq')"));
	REQUIRE_NO_FAIL(con.Query("PRAGMA threads=4"));
	REQUIRE_NO_FAIL(con.Query("CALL dsdgen(sf=1, suffix='_par')"));

	const vector<string> tables = {"call_center",
	                               "catalog_page",
	                               "catalog_returns",
	                               "catalog_sales",
	                               "customer",
	                               "customer_address",
	                               "customer_demographics",
	                               "date_dim",
	                               "household_demographics",
	                               "income_band",
	                               "inventory",
	                               "item",
	                               "promotion",
	                               "reason",
	                               "ship_mode",
	                               "store",
	                               "store_returns",
	                               "store_sales",
	                               "time_dim",
	                               "warehouse",
	                               "web_page",
	                               "web_returns",
	                               "web_sales",
	                               "web_site"};

	for (auto &table : tables) {
		auto diff =
		    con.Query("SELECT count(*) FROM ((SELECT * FROM " + table + "_seq EXCEPT ALL SELECT * FROM " + table +
		              "_par) UNION ALL (SELECT * FROM " + table + "_par EXCEPT ALL SELECT * FROM " + table + "_seq))");
		REQUIRE_NO_FAIL(*diff);
		REQUIRE(diff->Collection().GetValue(0, 0).GetValue<int64_t>() == 0);
	}
#endif
}

TEST_CASE("Test TPC-DS dsdgen rollback after interrupted optimistic write", "[tpcds][.]") {
#ifdef DUCKDB_NO_THREADS
	return;
#else
	auto db_path = TestCreatePath("tpcds_dsdgen_interrupted_optimistic_write.db");
	DuckDB db(db_path);
	Connection con(db);
	if (!db.ExtensionIsLoaded("tpcds")) {
		return;
	}

	REQUIRE_NO_FAIL(con.Query("PRAGMA threads=4"));
	REQUIRE_NO_FAIL(con.Query("SET write_buffer_row_group_count=1"));

	auto pending = con.Submit("CALL dsdgen(sf=1, suffix='_interrupted')");
	auto state = pending->ExecuteTask();
	REQUIRE(!IsObservable(state));

	con.Interrupt();
	pending->Complete();
	auto &result = pending;
	REQUIRE(result->HasError());
	con.context->ClearInterrupt();

	REQUIRE_NO_FAIL(con.Query("CHECKPOINT"));
	REQUIRE_NO_FAIL(con.Query("CALL dsdgen(sf=0.01, suffix='_after_interrupt')"));
	REQUIRE_NO_FAIL(con.Query("CHECKPOINT"));
#endif
}
