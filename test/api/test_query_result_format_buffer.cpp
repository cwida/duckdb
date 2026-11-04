#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/main/query_result_stream.hpp"

using namespace duckdb;

#ifndef DUCKDB_NO_THREADS

#include "duckdb/main/buffered_data/batched_buffered_data.hpp"
#include "duckdb/main/buffered_data/simple_buffered_data.hpp"
#include "duckdb/main/query_profiler.hpp"
#include "result_wait_helpers.hpp"
#include "test_result_format.hpp"

namespace {

struct DrainReport {
	idx_t row_count = 0;
	idx_t unit_count = 0;
	idx_t largest_unit_bytes = 0;
	bool saw_blocked_sink = false;
	vector<int64_t> rows;
};

DrainReport Drain(QueryResultStream<TestFormat> &stream) {
	DrainReport report;
	while (true) {
		report.saw_blocked_sink |= stream.GetBufferedData().HasBlockedSink();
		auto unit = stream.Fetch();
		if (!unit) {
			break;
		}
		report.largest_unit_bytes = MaxValue<idx_t>(report.largest_unit_bytes, unit->byte_size);
		report.row_count += unit->row_count;
		report.unit_count++;
		for (auto &value : UnitValues(*unit, 0)) {
			report.rows.push_back(value.GetValue<int64_t>());
		}
	}
	REQUIRE(!stream.HasError());
	return report;
}

} // namespace

TEST_CASE("A unit larger than the cap is admitted into an empty queue", "[api][query_result_format]") {
	DuckDB db(nullptr);
	Connection con(db);
	// Every unit is far past the cap, so each admission is the oversized-into-an-empty-queue case
	REQUIRE_NO_FAIL(con.Query("SET max_streaming_buffer_size='1000b'"));

	auto handle = SubmitFormatted(con, "SELECT i FROM range(100000) t(i)", 20000);
	DrainWatchdog watchdog(con);
	QueryResultStream<TestFormat> stream(std::move(handle));
	auto report = Drain(stream);

	RequireAscending(report.rows, 100000);
	REQUIRE(report.largest_unit_bytes > 1000);
	// One oversized unit at a time, so the peak is a single unit and no more
	REQUIRE(stream.GetBufferedData().PeakBufferedBytes() == report.largest_unit_bytes);
}

TEST_CASE("Queued bytes stay under the cap plus one unit", "[api][query_result_format]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t AS SELECT range i, repeat('p', 100) s FROM range(200000)"));
	REQUIRE_NO_FAIL(con.Query("SET max_streaming_buffer_size='250KB'"));

	SECTION("the simple store") {
		auto handle = SubmitFormatted(con, "SELECT i FROM range(200000) t(i)", 8192);
		DrainWatchdog watchdog(con);
		QueryResultStream<TestFormat> stream(std::move(handle));
		auto report = Drain(stream);
		RequireAscending(report.rows, 200000);
		REQUIRE(stream.GetBufferedData().PeakBufferedBytes() <= 250000 + report.largest_unit_bytes);
	}
	SECTION("the batched store, with the minimum batch admitted ahead of blocked later batches") {
		auto handle = SubmitFormatted(con, "SELECT i FROM t", 8192);
		DrainWatchdog watchdog(con);
		QueryResultStream<TestFormat> stream(std::move(handle));
		auto &buffered = stream.GetBufferedData().Cast<BatchedBufferedData>();
		auto report = Drain(stream);
		// Order is restored even though later batches finished units first and parked for space
		RequireAscending(report.rows, 200000);
		REQUIRE(buffered.PeakBufferedBytes() <= 250000 + report.largest_unit_bytes);
		REQUIRE(report.saw_blocked_sink);
	}
}

TEST_CASE("A producer parks while several sliced units are still pending in its local state",
          "[api][query_result_format]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t AS SELECT range i FROM range(400000)"));
	// Below one 300-row unit, so no unit fits beside another that is still queued
	REQUIRE_NO_FAIL(con.Query("SET max_streaming_buffer_size='2KB'"));

	SECTION("the simple store") {
		auto handle = SubmitFormatted(con, "SELECT i FROM range(20000) t(i)", 300);
		DrainWatchdog watchdog(con);
		QueryResultStream<TestFormat> stream(std::move(handle));
		auto report = Drain(stream);
		RequireAscending(report.rows, 20000);
		REQUIRE(report.unit_count >= 20000 / 300);
		// A chunk of at least two units makes every append finish several units, so below that a park is down to timing
		if (STANDARD_VECTOR_SIZE >= 2 * 300) {
			REQUIRE(report.saw_blocked_sink);
			REQUIRE(stream.FormatState().max_pending_units > 1);
		}
		// A re-delivered chunk resumes the drain, so a producer never carries one append's units into the next
		REQUIRE(stream.FormatState().max_pending_units <= STANDARD_VECTOR_SIZE / 300 + 1);
	}
	SECTION("the batched store, with several producers") {
		REQUIRE_NO_FAIL(con.Query("SET threads=4"));
		auto handle = SubmitFormatted(con, "SELECT i FROM t", 300);
		DrainWatchdog watchdog(con);
		QueryResultStream<TestFormat> stream(std::move(handle));
		auto report = Drain(stream);
		RequireAscending(report.rows, 400000);
		REQUIRE(report.saw_blocked_sink);
		REQUIRE(stream.FormatState().local_states > 1);
		// No row group is a multiple of 300 rows, so each batch boundary flushes exactly one partial unit
		REQUIRE(stream.FormatState().partial_units == 4);
		REQUIRE(stream.FormatState().max_pending_units <= STANDARD_VECTOR_SIZE / 300 + 1);
	}
}

TEST_CASE("Several producers build units while the others park holding theirs", "[api][query_result_format]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t AS SELECT range i FROM range(400000)"));
	REQUIRE_NO_FAIL(con.Query("SET preserve_insertion_order=false"));
	REQUIRE_NO_FAIL(con.Query("SET threads=4"));
	// A cap of roughly one unit, so a producer that finishes a second unit has to park holding it
	REQUIRE_NO_FAIL(con.Query("SET max_streaming_buffer_size='128KB'"));

	// A row cap that no whole number of row groups is a multiple of, so every producer ends partial
	auto handle = SubmitFormatted(con, "SELECT i FROM t", 14336);
	DrainWatchdog watchdog(con);
	QueryResultStream<TestFormat> stream(std::move(handle));
	auto report = Drain(stream);

	std::sort(report.rows.begin(), report.rows.end());
	REQUIRE(report.rows.size() == 400000);
	for (idx_t i = 0; i < report.rows.size(); i++) {
		REQUIRE(report.rows[i] == NumericCast<int64_t>(i));
	}
	REQUIRE(report.saw_blocked_sink);
	REQUIRE(stream.FormatState().local_states > 1);
	// Every producer ends on a partial unit, and every hand-over of one competes for a full buffer
	REQUIRE(stream.FormatState().partial_units == stream.FormatState().local_states);
}

TEST_CASE("A producer parked at Combine is deposited on the consumer's pop", "[api][query_result_format]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t AS SELECT range i FROM range(300001)"));
	REQUIRE_NO_FAIL(con.Query("SET threads=4"));
	REQUIRE_NO_FAIL(con.Query("SET max_streaming_buffer_size='128KB'"));

	SECTION("the simple store") {
		REQUIRE_NO_FAIL(con.Query("SET preserve_insertion_order=false"));
		// The simple store runs no NextBatch, so Combine finishes and hands over every producer's partial unit
		// No whole number of row groups is a multiple of the row cap, so every producer ends partial
		auto handle = SubmitFormatted(con, "SELECT i FROM t", 14336);
		DrainWatchdog watchdog(con);
		QueryResultStream<TestFormat> stream(std::move(handle));
		REQUIRE_NOTHROW(stream.GetBufferedData().Cast<SimpleBufferedData>());
		auto report = Drain(stream);

		std::sort(report.rows.begin(), report.rows.end());
		REQUIRE(report.rows.size() == 300001);
		for (idx_t i = 0; i < report.rows.size(); i++) {
			REQUIRE(report.rows[i] == NumericCast<int64_t>(i));
		}
		REQUIRE(report.saw_blocked_sink);
		REQUIRE(stream.FormatState().partial_units == stream.FormatState().local_states);
		stream.GetBufferedData().AssertNoBlockedSinks();
	}
	SECTION("the batched store") {
		// preserve_insertion_order stays at its default (true), so the plan is batch ordered and Combine's
		// final flush parks a non-minimum batch producer in the batched store's own blocked_sinks map
		auto handle = SubmitFormatted(con, "SELECT i FROM t", 14336);
		DrainWatchdog watchdog(con);
		QueryResultStream<TestFormat> stream(std::move(handle));
		REQUIRE_NOTHROW(stream.GetBufferedData().Cast<BatchedBufferedData>());
		auto report = Drain(stream);

		RequireAscending(report.rows, 300001);
		REQUIRE(report.saw_blocked_sink);
		REQUIRE(stream.FormatState().partial_units == stream.FormatState().local_states);
		stream.GetBufferedData().AssertNoBlockedSinks();
	}
}

TEST_CASE("The peak metric reports the bytes a formatted stream held", "[api][query_result_format]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t AS SELECT range i FROM range(200000)"));
	REQUIRE_NO_FAIL(con.Query("SET max_streaming_buffer_size='100KB'"));
	REQUIRE_NO_FAIL(con.Query("PRAGMA enable_profiling='no_output'"));

	auto handle = SubmitFormatted(con, "SELECT i FROM t", 8192);
	DrainWatchdog watchdog(con);
	QueryResultStream<TestFormat> stream(std::move(handle));
	auto report = Drain(stream);
	RequireAscending(report.rows, 200000);

	auto peak = stream.GetBufferedData().PeakStreamingBytes();
	REQUIRE(peak > 0);
	auto profile = QueryProfiler::Get(*con.context).ToJSON();
	auto key_pos = profile.find("\"peak_streaming_buffer_size\"");
	REQUIRE(key_pos != string::npos);
	auto colon_pos = profile.find(':', key_pos);
	REQUIRE(colon_pos != string::npos);
	REQUIRE(std::stoull(profile.substr(colon_pos + 1)) == peak);
}

TEST_CASE("An interrupt while a producer holds a unit ends the stream", "[api][query_result_format]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t AS SELECT range i FROM range(400000)"));
	REQUIRE_NO_FAIL(con.Query("SET threads=4"));
	REQUIRE_NO_FAIL(con.Query("SET max_streaming_buffer_size='128KB'"));

	auto handle = SubmitFormatted(con, "SELECT i FROM t", 14336);
	QueryResultStream<TestFormat> stream(std::move(handle));

	// Cancel once a producer is parked holding a finished unit it could not hand over
	Deadline deadline;
	while (!stream.GetBufferedData().HasBlockedSink()) {
		REQUIRE(!IsTerminal(stream.ExecuteTask()));
		REQUIRE(!deadline.Passed());
	}
	con.Interrupt();

	while (stream.Fetch()) {
	}
	REQUIRE(stream.HasError());
	REQUIRE(stream.GetErrorType() == ExceptionType::INTERRUPT);
	auto next = con.Query("SELECT 42");
	REQUIRE(CHECK_COLUMN(next, 0, {42}));
}

#endif
