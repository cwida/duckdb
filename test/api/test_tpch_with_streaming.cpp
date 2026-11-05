#include "catch.hpp"
#include "test_helpers.hpp"
#include "tpch_extension.hpp"

#include <chrono>
#include <iostream>
#include "duckdb/common/string_util.hpp"

using namespace duckdb;

TEST_CASE("Test TPC-H SF0.01 using streaming api", "[tpch][.]") {
	DuckDB db(nullptr);
	Connection con(db);
	double sf = 0.01;
	if (!db.ExtensionIsLoaded("tpch")) {
		return;
	}

	REQUIRE_NO_FAIL(con.Query("CALL dbgen(sf=" + to_string(sf) + ")"));

	for (idx_t tpch_num = 1; tpch_num <= 22; tpch_num++) {
		auto stream = OpenStream(con, "pragma tpch(" + to_string(tpch_num) + ");");

		duckdb::ColumnDataCollection collection(duckdb::Allocator::DefaultAllocator(), stream->GetTypes());

		while (true) {
			auto chunk = stream->Fetch();
			if (chunk) {
				collection.Append(*chunk);
			} else {
				break;
			}
		}

		COMPARE_CSV_COLLECTION(collection, TpchExtension::GetAnswer(sf, tpch_num), true);
	}
}

TEST_CASE("Test TPC-H dbgen progress", "[tpch][progress-bar][.]") {
	DuckDB db(nullptr);
	Connection con(db);
	if (!db.ExtensionIsLoaded("tpch")) {
		return;
	}

	REQUIRE_NO_FAIL(con.Query("PRAGMA threads=1"));
	REQUIRE_NO_FAIL(con.Query("PRAGMA progress_bar_time=1"));
	REQUIRE_NO_FAIL(con.Query("PRAGMA disable_print_progress_bar"));

	auto pending = con.Submit("CALL dbgen(sf=0.01, suffix='_progress')");
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

TEST_CASE("Test TPC-H dbgen parallel progress does not finish early", "[tpch][progress-bar][.]") {
	DuckDB db(nullptr);
	Connection con(db);
	if (!db.ExtensionIsLoaded("tpch")) {
		return;
	}

	REQUIRE_NO_FAIL(con.Query("PRAGMA threads=2"));
	REQUIRE_NO_FAIL(con.Query("PRAGMA progress_bar_time=1"));
	REQUIRE_NO_FAIL(con.Query("PRAGMA disable_print_progress_bar"));

	auto pending = con.Submit("CALL dbgen(sf=0.01, suffix='_progress_parallel')");
	bool saw_progress_before_ready = false;
	bool saw_finished_progress_before_ready = false;

	while (true) {
		auto state = pending->ExecuteTask();
		auto result_ready = IsObservable(state);
		auto query_progress = con.context->GetQueryProgress();
		auto percentage = query_progress.GetPercentage();
		if (percentage >= 0 && !result_ready) {
			if (percentage >= 100) {
				saw_finished_progress_before_ready = true;
			}
			saw_progress_before_ready = true;
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
	REQUIRE(!saw_finished_progress_before_ready);
	REQUIRE(saw_progress_before_ready);
}

TEST_CASE("Test TPC-H dbgen rollback after interrupted optimistic write", "[tpch][.]") {
	auto db_path = TestCreatePath("tpch_dbgen_interrupted_optimistic_write.db");
	DuckDB db(db_path);
	Connection con(db);
	if (!db.ExtensionIsLoaded("tpch")) {
		return;
	}

	REQUIRE_NO_FAIL(con.Query("PRAGMA threads=4"));
	REQUIRE_NO_FAIL(con.Query("SET write_buffer_row_group_count=1"));

	auto pending = con.Submit("CALL dbgen(sf=1, suffix='_interrupted')");
	auto state = pending->ExecuteTask();
	REQUIRE(!IsObservable(state));

	con.Interrupt();
	pending->Complete();
	auto &result = pending;
	REQUIRE(result->HasError());
	con.context->ClearInterrupt();

	REQUIRE_NO_FAIL(con.Query("CHECKPOINT"));
	REQUIRE_NO_FAIL(con.Query("CALL dbgen(sf=0.01, suffix='_after_interrupt')"));
	REQUIRE_NO_FAIL(con.Query("CHECKPOINT"));
}
