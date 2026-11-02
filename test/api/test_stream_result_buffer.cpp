#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/main/buffered_data/batched_buffered_data.hpp"
#include "duckdb/main/buffered_data/simple_buffered_data.hpp"
#include "duckdb/main/query_profiler.hpp"
#include "duckdb/main/result_unit.hpp"
#include "result_wait_helpers.hpp"
#include "duckdb/main/query_result_stream.hpp"

#include <atomic>
#include <chrono>
#include <thread>

using namespace duckdb;

namespace {

//! Render the physical plan text so tests can assert the plan shape
string PhysicalPlanText(Connection &con, const string &query) {
	auto explain_result = con.Query("EXPLAIN " + query);
	if (explain_result->HasError()) {
		FAIL(explain_result->GetError());
	}
	string plan;
	for (idx_t row = 0; row < explain_result->RowCount(); row++) {
		plan += explain_result->Collection().GetValue(1, row).ToString();
	}
	return plan;
}

unique_ptr<QueryResultStream<>> ExecuteStreaming(Connection &con, const string &query) {
	auto handle = con.Submit(query);
	if (handle->HasError()) {
		FAIL(handle->GetError());
	}
	return make_uniq<QueryResultStream<>>(std::move(handle));
}

//! Submit a query, leaving the retention undecided
unique_ptr<QueryResult> Submit(Connection &con, const string &query) {
	auto handle = con.Submit(query);
	if (handle->HasError()) {
		FAIL(handle->GetError());
	}
	return handle;
}

//! Drive the non-blocking API until execution reaches a terminal state
QueryResultState PollToTerminal(QueryResultStream<> &stream) {
	Deadline deadline;
	while (true) {
		auto result = stream.ExecuteTask();
		if (IsTerminal(result)) {
			return result;
		}
		REQUIRE(!deadline.Passed());
		std::this_thread::sleep_for(std::chrono::microseconds(100));
	}
}

} // namespace

#ifndef DUCKDB_NO_THREADS

TEST_CASE("A blocking fetch on a batched stream observes an interrupt with chunks buffered", "[api][stream_buffer]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t AS SELECT range i FROM range(200000)"));
	// A buffer large enough that chunks stay buffered when the interrupt arrives
	REQUIRE_NO_FAIL(con.Query("SET max_streaming_buffer_size='100MB'"));

	auto result = ExecuteStreaming(con, "SELECT i FROM t");
	auto &stream = *result;
	// Wait until at least one chunk is observably buffered, then cancel before fetching
	Deadline deadline;
	while (!stream.GetBufferedData().HasObservableUnit()) {
		REQUIRE(!IsTerminal(stream.ExecuteTask()));
		REQUIRE(!deadline.Passed());
		std::this_thread::sleep_for(std::chrono::microseconds(100));
	}
	con.Interrupt();

	// The cancel must be observed even though chunks are buffered, not after the drain
	auto chunk = stream.Fetch();
	REQUIRE(!chunk);
	REQUIRE(stream.HasError());
	REQUIRE(StringUtil::Contains(stream.GetError(), "INTERRUPT"));
}

TEST_CASE("Completing a simple undecided query retains every row", "[api][stream_buffer]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET max_streaming_buffer_size='100KB'"));

	auto handle = Submit(con, "SELECT i FROM range(500000) t(i)");
	DrainWatchdog watchdog(con);
	handle->Complete();
	REQUIRE(!handle->HasError());
	REQUIRE(handle->RowCount() == 500000);
}

TEST_CASE("Completing a batched undecided query retains every row in order", "[api][stream_buffer]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t AS SELECT range i FROM range(500000)"));
	REQUIRE_NO_FAIL(con.Query("SET max_streaming_buffer_size='100KB'"));

	auto handle = Submit(con, "SELECT i FROM t");
	DrainWatchdog watchdog(con);
	handle->Complete();
	REQUIRE(!handle->HasError());
	REQUIRE(handle->RowCount() == 500000);
	REQUIRE(handle->Collection().GetValue(0, 0).GetValue<int64_t>() == 0);
	REQUIRE(handle->Collection().GetValue(0, 499999).GetValue<int64_t>() == 499999);
}

TEST_CASE("Completing an erroring query surfaces the execution error", "[api][stream_buffer]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET max_streaming_buffer_size='100KB'"));

	// The cast fails at a late row, after the stream has produced chunks
	auto handle =
	    Submit(con, "SELECT (CASE WHEN i = 400000 THEN 'boom' ELSE i::VARCHAR END)::INT FROM range(500000) t(i)");
	DrainWatchdog watchdog(con);
	handle->Complete();
	REQUIRE(handle->HasError());
	REQUIRE(StringUtil::Contains(handle->GetError(), "boom"));
}

TEST_CASE("Completing an erroring batched query surfaces the execution error", "[api][stream_buffer]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t AS SELECT range i FROM range(500000)"));
	REQUIRE_NO_FAIL(con.Query("SET max_streaming_buffer_size='100KB'"));

	// Batched producers park under the cap before the late failing row
	auto handle = Submit(con, "SELECT (CASE WHEN i = 400000 THEN 'boom' ELSE i::VARCHAR END)::INT FROM t");
	DrainWatchdog watchdog(con);
	handle->Complete();
	REQUIRE(handle->HasError());
	REQUIRE(StringUtil::Contains(handle->GetError(), "boom"));
}

TEST_CASE("A blocking drain of an erroring stream ends with the error", "[api][stream_buffer]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET max_streaming_buffer_size='100KB'"));

	auto stream = ExecuteStreaming(
	    con, "SELECT (CASE WHEN i = 400000 THEN 'boom' ELSE i::VARCHAR END)::INT FROM range(500000) t(i)");
	DrainWatchdog watchdog(con);
	idx_t row_count = 0;
	while (auto chunk = stream->Fetch()) {
		row_count += chunk->size();
	}
	REQUIRE(stream->HasError());
	REQUIRE(StringUtil::Contains(stream->GetError(), "boom"));
	// Rows before the failure may arrive, the failing chunk and anything after it never do
	REQUIRE(row_count < 400000);
}

TEST_CASE("A simple stream reports an execution error while a chunk is still buffered", "[api][stream_buffer]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=2"));
	// A cap below one chunk: the producer parks on every chunk, so one chunk is buffered when the error lands
	REQUIRE_NO_FAIL(con.Query("SET max_streaming_buffer_size='1b'"));

	auto stream = ExecuteStreaming(con, "SELECT (CASE WHEN i = " + to_string(2 * STANDARD_VECTOR_SIZE) +
	                                        " THEN 'boom' ELSE i::VARCHAR END)::INT FROM range(" +
	                                        to_string(4 * STANDARD_VECTOR_SIZE) + ") t(i)");
	// The pop wakes the parked producer, whose next chunk fails on the worker thread
	REQUIRE(stream->Fetch());
	REQUIRE(PollToTerminal(*stream) == QueryResultState::EXECUTION_ERROR);
	REQUIRE(stream->HasError());
	REQUIRE(StringUtil::Contains(stream->GetError(), "boom"));
}

TEST_CASE("A batched stream reports an execution error while a chunk is still buffered", "[api][stream_buffer]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(
	    con.Query("CREATE TABLE t AS SELECT range i FROM range(" + to_string(4 * STANDARD_VECTOR_SIZE) + ")"));
	REQUIRE_NO_FAIL(con.Query("SET threads=2"));
	REQUIRE_NO_FAIL(con.Query("SET max_streaming_buffer_size='1b'"));

	auto stream = ExecuteStreaming(con, "SELECT (CASE WHEN i = " + to_string(2 * STANDARD_VECTOR_SIZE) +
	                                        " THEN 'boom' ELSE i::VARCHAR END)::INT FROM t");
	REQUIRE(stream->Fetch());
	REQUIRE(PollToTerminal(*stream) == QueryResultState::EXECUTION_ERROR);
	REQUIRE(stream->HasError());
	REQUIRE(StringUtil::Contains(stream->GetError(), "boom"));
}

TEST_CASE("A blocking drain crosses a streaming-fanout CTE plan", "[api][stream_buffer]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE fanout AS SELECT range i, range % 512 g FROM range(200000)"));

	// Distinct aggregates are rewritten into a shared materialized CTE: one scan fans out to both consumers
	const string query = "SELECT COUNT(DISTINCT i), SUM(DISTINCT g) FROM fanout";
	REQUIRE(StringUtil::Contains(PhysicalPlanText(con, query), "PIPELINE_DEPENDENT"));

	auto stream = ExecuteStreaming(con, query);
	DrainWatchdog watchdog(con);
	idx_t row_count = 0;
	int64_t count_distinct = -1;
	int64_t sum_distinct = -1;
	while (auto chunk = stream->Fetch()) {
		for (idx_t i = 0; i < chunk->size(); i++) {
			count_distinct = chunk->GetValue(0, i).GetValue<int64_t>();
			sum_distinct = chunk->GetValue(1, i).GetValue<int64_t>();
		}
		row_count += chunk->size();
	}
	REQUIRE(!stream->HasError());
	REQUIRE(row_count == 1);
	REQUIRE(count_distinct == 200000);
	REQUIRE(sum_distinct == 130816);
}

TEST_CASE("A stream of nested types survives a tiny buffer", "[api][stream_buffer]") {
	DuckDB db(nullptr);
	Connection con(db);
	// A cap far below the chunk size: every producer parks, every copy is exercised
	REQUIRE_NO_FAIL(con.Query("SET max_streaming_buffer_size='10KB'"));

	auto stream = ExecuteStreaming(
	    con, "SELECT [i, i + 1] l, {'a': i, 's': 'payload beyond inlining length ' || i} r FROM range(100000) t(i)");
	DrainWatchdog watchdog(con);
	idx_t row_count = 0;
	unique_ptr<DataChunk> last;
	while (auto chunk = stream->Fetch()) {
		row_count += chunk->size();
		last = std::move(chunk);
	}
	REQUIRE(!stream->HasError());
	REQUIRE(row_count == 100000);
	auto last_row = last->size() - 1;
	REQUIRE(last->GetValue(0, last_row).ToString() == "[99999, 100000]");
	REQUIRE(last->GetValue(1, last_row).ToString() == "{'a': 99999, 's': payload beyond inlining length 99999}");
}

TEST_CASE("A fanout plan streams under the cap instead of completing up front", "[api][stream_buffer]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET max_streaming_buffer_size='100KB'"));

	// A materialized CTE consumed twice: the last pipeline is fed across a pipeline dependency
	const string query = "WITH c AS MATERIALIZED (SELECT range i FROM range(500000)) "
	                     "SELECT t1.i FROM c t1 JOIN c t2 USING (i)";
	REQUIRE(StringUtil::Contains(PhysicalPlanText(con, query), "PIPELINE_DEPENDENT"));

	auto stream = ExecuteStreaming(con, query);
	DrainWatchdog watchdog(con);
	idx_t row_count = 0;
	while (auto chunk = stream->Fetch()) {
		row_count += chunk->size();
	}
	REQUIRE(!stream->HasError());
	REQUIRE(row_count == 500000);
	// A completed-before-fetch execution would have buffered all ~4MB. Staying near the
	// cap proves producers parked, so the result was handed out mid-execution
	REQUIRE(stream->GetBufferedData().PeakBufferedBytes() <= 100000 + 100000);
}

TEST_CASE("A fetched stream chunk outlives result, connection, and database", "[api][stream_buffer]") {
	unique_ptr<DataChunk> chunk;
	{
		auto db = make_uniq<DuckDB>(nullptr);
		auto con = make_uniq<Connection>(*db);
		auto result =
		    ExecuteStreaming(*con, "SELECT i, 'payload beyond inlining length ' || i AS s FROM range(3000) t(i)");
		chunk = result->Fetch();
		REQUIRE(chunk);
		REQUIRE(chunk->size() > 0);
		result.reset();
		con.reset();
		db.reset();
	}
	// The chunk's pins carry the database, so its vectors still read cleanly
	auto last_row = chunk->size() - 1;
	REQUIRE(chunk->GetValue(0, 0).GetValue<int64_t>() == 0);
	REQUIRE(chunk->GetValue(1, last_row).GetValue<string>() == "payload beyond inlining length " + to_string(last_row));
}

TEST_CASE("A batched stream result never exceeds the buffer cap", "[api][stream_buffer]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET max_streaming_buffer_size='250000b'"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE tbl AS SELECT 'padding-padding-' || i AS s FROM range(200000) t(i)"));
	REQUIRE_NO_FAIL(con.Query("PRAGMA enable_profiling='no_output'"));

	auto result = ExecuteStreaming(con, "SELECT * FROM tbl");
	auto &stream = *result;
	DrainWatchdog watchdog(con);
	idx_t row_count = 0;
	while (auto chunk = stream.Fetch()) {
		// The drain must observe exact insertion order through the park/deposit/move cycle
		for (idx_t i = 0; i < chunk->size(); i++) {
			auto value = chunk->GetValue(0, i).ToString();
			auto expected = "padding-padding-" + to_string(row_count + i);
			if (value != expected) {
				FAIL(StringUtil::Format("Out-of-order row %llu: expected %s, got %s", row_count + i, expected, value));
			}
		}
		row_count += chunk->size();
	}
	REQUIRE(row_count == 200000);
	// The guarantee is the cap plus one chunk: an admission into an empty queue may
	// exceed the cap by at most the admitted chunk
	REQUIRE(stream.GetBufferedData().Cast<BatchedBufferedData>().PeakBufferedBytes() <= 250000 + 100000);

	// The peak surfaces as a query-level profiling metric, carrying the real value
	auto peak = stream.GetBufferedData().Cast<BatchedBufferedData>().PeakStreamingBytes();
	auto profile = QueryProfiler::Get(*con.context).ToJSON();
	auto key_pos = profile.find("\"peak_streaming_buffer_size\"");
	REQUIRE(key_pos != string::npos);
	auto colon_pos = profile.find(':', key_pos);
	REQUIRE(colon_pos != string::npos);
	auto reported = std::stoull(profile.substr(colon_pos + 1));
	REQUIRE(reported == peak);
	REQUIRE(reported > 0);
}

TEST_CASE("A batched stream survives a cap below one chunk", "[api][stream_buffer]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t AS SELECT range i FROM range(100000)"));
	// Every chunk exceeds the cap: each admission is the oversized-into-empty-queue case
	REQUIRE_NO_FAIL(con.Query("SET max_streaming_buffer_size='1000b'"));

	auto result = ExecuteStreaming(con, "SELECT i FROM t");
	auto &stream = *result;
	DrainWatchdog watchdog(con);
	int64_t expected = 0;
	while (auto chunk = stream.Fetch()) {
		for (idx_t i = 0; i < chunk->size(); i++) {
			if (chunk->GetValue(0, i).GetValue<int64_t>() != expected) {
				FAIL(StringUtil::Format("Out-of-order row: expected %lld", expected));
			}
			expected++;
		}
	}
	REQUIRE(expected == 100000);
}

TEST_CASE("A simple stream result never exceeds the buffer cap", "[api][stream_buffer]") {
	DuckDB db(nullptr);
	Connection con(db);
	// The blocking consumer must stop replenishing on a saturated buffer, not only on
	// an exactly-full one, or the size-aware block spins the replenish loop
	REQUIRE_NO_FAIL(con.Query("SET max_streaming_buffer_size='100000b'"));

	auto result = ExecuteStreaming(con, "SELECT i FROM range(500000) t(i)");
	auto &stream = *result;
	DrainWatchdog watchdog(con);
	idx_t row_count = 0;
	while (auto chunk = stream.Fetch()) {
		row_count += chunk->size();
	}
	REQUIRE(row_count == 500000);
	REQUIRE(stream.GetBufferedData().Cast<SimpleBufferedData>().PeakBufferedBytes() <= 100000);
}

TEST_CASE("A parked read-ahead batch does not report the batched buffer waiting on the consumer",
          "[api][stream_buffer]") {
	DuckDB db(nullptr);
	Connection con(db);
	DataChunk chunk;
	chunk.Initialize(Allocator::DefaultAllocator(), {LogicalType::BIGINT});
	auto writer = FlatVector::Writer<int64_t>(chunk.data[0], STANDARD_VECTOR_SIZE);
	for (idx_t i = 0; i < STANDARD_VECTOR_SIZE; i++) {
		writer.WriteValue(0);
	}
	// The cap counts the buffered copy. Four chunks fit and the reserve for the minimum batch is one
	// chunk, so a read-ahead batch parks on its fourth chunk with nothing in the read queue
	const auto chunk_bytes = BufferedData::CopyForBuffering(chunk)->GetDataSize();
	REQUIRE_NO_FAIL(con.Query("SET max_streaming_buffer_size='" + to_string(4 * chunk_bytes) + " bytes'"));
	ResultFormatContext format_context {
	    {LogicalType::BIGINT}, {Identifier("i")}, ClientProperties(), ResultOrdering::BATCH_INDEX_ORDERED};
	BatchedBufferedData buffered(*con.context, ResultLifetime::DRAINING, std::move(format_context), nullptr);
	auto signal = make_shared_ptr<InterruptDoneSignalState>();
	weak_ptr<InterruptDoneSignalState> weak_signal(signal);
	InterruptState read_ahead(weak_signal);

	auto chunk_unit = [&chunk]() {
		return make_uniq<ChunkUnit>(BufferedData::CopyForBuffering(chunk));
	};

	idx_t appended = 0;
	while (!buffered.AppendOrBlock(chunk_unit(), 1, read_ahead)) {
		appended++;
		REQUIRE(appended <= 4);
	}
	REQUIRE(appended == 3);
	REQUIRE(buffered.HasBlockedSink());
	REQUIRE(!buffered.HasObservableUnit());
	// Nothing was deposited yet: the cap only ever saw the queued units, the metric also sees the held one
	REQUIRE(buffered.PeakBufferedBytes() == appended * chunk_bytes);
	REQUIRE(buffered.PeakStreamingBytes() == (appended + 1) * chunk_bytes);
	// The park waits on the minimum batch, not on the consumer: reporting otherwise makes a consumer
	// that waits for a task spin until the minimum batch delivers
	REQUIRE(!buffered.WaitsOnConsumer());

	// The minimum batch always gets its reserve, and its unit is what the consumer pops
	InterruptState minimum(weak_signal);
	REQUIRE(!buffered.AppendOrBlock(chunk_unit(), 0, minimum));
	REQUIRE(buffered.HasObservableUnit());
	REQUIRE(buffered.WaitsOnConsumer());
	REQUIRE(buffered.Scan());
	REQUIRE(!buffered.HasObservableUnit());
	REQUIRE(!buffered.WaitsOnConsumer());
}

TEST_CASE("The simple buffer's peak counts the unit a parked producer holds", "[api][stream_buffer]") {
	DuckDB db(nullptr);
	Connection con(db);
	DataChunk chunk;
	chunk.Initialize(Allocator::DefaultAllocator(), {LogicalType::BIGINT});
	auto writer = FlatVector::Writer<int64_t>(chunk.data[0], STANDARD_VECTOR_SIZE);
	for (idx_t i = 0; i < STANDARD_VECTOR_SIZE; i++) {
		writer.WriteValue(0);
	}
	// Two units fill the cap exactly, so the third producer parks holding its unit
	const auto chunk_bytes = BufferedData::CopyForBuffering(chunk)->GetDataSize();
	REQUIRE_NO_FAIL(con.Query("SET max_streaming_buffer_size='" + to_string(2 * chunk_bytes) + " bytes'"));
	ResultFormatContext format_context {
	    {LogicalType::BIGINT}, {Identifier("i")}, ClientProperties(), ResultOrdering::UNORDERED};
	SimpleBufferedData buffered(*con.context, ResultLifetime::DRAINING, std::move(format_context), nullptr);
	auto signal = make_shared_ptr<InterruptDoneSignalState>();
	weak_ptr<InterruptDoneSignalState> weak_signal(signal);
	InterruptState parked(weak_signal);

	idx_t appended = 0;
	while (!buffered.AppendOrBlock(make_uniq<ChunkUnit>(BufferedData::CopyForBuffering(chunk)), parked)) {
		appended++;
		REQUIRE(appended <= 2);
	}
	REQUIRE(appended == 2);
	REQUIRE(buffered.HasBlockedSink());
	// Nothing was deposited yet: the cap only ever saw the queued units, the metric also sees the held one
	REQUIRE(buffered.PeakBufferedBytes() == appended * chunk_bytes);
	REQUIRE(buffered.PeakStreamingBytes() == (appended + 1) * chunk_bytes);
}

TEST_CASE("Poll on a draining stream reports READY exactly when a chunk is poppable", "[api][stream_buffer]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t AS SELECT range i FROM range(2000000)"));
	// A small buffer keeps producers parking for the whole drain
	REQUIRE_NO_FAIL(con.Query("SET max_streaming_buffer_size='64KB'"));

	// The unordered plan uses the simple store, the table scan the batched one
	for (auto query : {"SELECT i FROM range(2000000) t(i)", "SELECT i FROM t"}) {
		auto result = ExecuteStreaming(con, query);
		auto &stream = *result;
		auto &buffered = stream.GetBufferedData();
		Deadline deadline;
		idx_t row_count = 0;
		idx_t unparked_polls = 0;
		while (true) {
			// Nothing but this thread pops, so a chunk observed here is still there for the poll
			const bool observable = buffered.HasObservableUnit();
			const bool engine_waits = buffered.WaitsOnConsumer();
			const auto polled = stream.Poll();
			if (observable) {
				REQUIRE(polled == QueryResultState::READY);
				if (!engine_waits) {
					unparked_polls++;
				}
			}
			if (polled == QueryResultState::READY) {
				REQUIRE(buffered.HasObservableUnit());
			}
			unique_ptr<DataChunk> chunk;
			auto state = stream.TryFetch(chunk);
			if (state == QueryResultState::READY) {
				row_count += chunk->size();
				continue;
			}
			if (IsTerminal(state)) {
				REQUIRE(state == QueryResultState::FINISHED);
				break;
			}
			REQUIRE(!deadline.Passed());
		}
		REQUIRE(row_count == 2000000);
		// The engine reports READY only for a parked producer: the trailing chunks of a finished
		// execution are poppable with no producer left, and only the buffer's poll announces them
		REQUIRE(unparked_polls > 0);
	}
}

TEST_CASE("Submit returns before any chunk is buffered", "[api][stream_buffer]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t AS SELECT range i FROM range(500000)"));
	REQUIRE_NO_FAIL(con.Query("SET max_streaming_buffer_size='100KB'"));

	for (auto query : {"SELECT i FROM range(500000) t(i)", "SELECT i FROM t"}) {
		auto handle = Submit(con, query);
		auto &buffered = handle->GetBufferedData();
		// The first producer parks with its chunk unconsumed until the consumer chooses
		Deadline deadline;
		while (!buffered.WaitsOnConsumer()) {
			REQUIRE(!deadline.Passed());
			std::this_thread::sleep_for(std::chrono::microseconds(100));
		}
		REQUIRE(buffered.Lifetime() == ResultLifetime::UNDECIDED);
		REQUIRE(buffered.PeakBufferedBytes() == 0);
		DrainWatchdog watchdog(con);
		QueryResultStream<> stream(std::move(handle));
		auto chunk = stream.Fetch();
		REQUIRE(chunk);
		REQUIRE(chunk->size() > 0);
		REQUIRE(buffered.Lifetime() == ResultLifetime::DRAINING);
		REQUIRE(buffered.PeakBufferedBytes() > 0);
	}
}

TEST_CASE("Completing a fresh submission stages nothing in the buffer", "[api][stream_buffer]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t AS SELECT range i FROM range(500000)"));
	REQUIRE_NO_FAIL(con.Query("SET max_streaming_buffer_size='100KB'"));

	for (auto query : {"SELECT i FROM range(500000) t(i)", "SELECT i FROM t"}) {
		auto handle = Submit(con, query);
		DrainWatchdog watchdog(con);
		handle->Complete();
		REQUIRE(!handle->HasError());
		REQUIRE(handle->RowCount() == 500000);
		REQUIRE(handle->Collection().GetValue(0, 0).GetValue<int64_t>() == 0);
		REQUIRE(handle->Collection().GetValue(0, 499999).GetValue<int64_t>() == 499999);
		// Producers appended into the collection directly: the streaming buffer never held a byte
		REQUIRE(handle->GetBufferedData().Lifetime() == ResultLifetime::RETAINED);
		REQUIRE(handle->GetBufferedData().PeakBufferedBytes() == 0);
	}
}

TEST_CASE("A zero-row batch-ordered query completes and drains", "[api][stream_buffer]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t AS SELECT range i FROM range(500000)"));

	// No chunk ever reaches the sink, so the query finishes before the consumer decides
	{
		auto handle = Submit(con, "SELECT i FROM t WHERE i < 0");
		REQUIRE_NOTHROW(handle->GetBufferedData().Cast<BatchedBufferedData>());
		handle->Complete();
		REQUIRE(!handle->HasError());
		REQUIRE(handle->RowCount() == 0);
	}
	{
		auto stream = ExecuteStreaming(con, "SELECT i FROM t WHERE i < 0");
		REQUIRE(!stream->Fetch());
		REQUIRE(!stream->HasError());
	}
}

TEST_CASE("Empty partitions under a parallel retained sink", "[api][stream_buffer]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=4"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t AS SELECT range i FROM range(2000000)"));

	// Only the first row group yields rows, so most producers combine without ever having sunk a chunk
	for (auto preserve : {"true", "false"}) {
		REQUIRE_NO_FAIL(con.Query(string("SET preserve_insertion_order=") + preserve));
		auto result = con.Query("SELECT i FROM t WHERE i < 10");
		REQUIRE(!result->HasError());
		REQUIRE(result->RowCount() == 10);

		auto handle = Submit(con, "SELECT i FROM t WHERE i < 10");
		handle->Complete();
		REQUIRE(!handle->HasError());
		REQUIRE(handle->RowCount() == 10);
	}
}

TEST_CASE("An interrupt during a retained completion surfaces the interrupt", "[api][stream_buffer]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto handle = Submit(con, "SELECT i FROM range(100000000000) t(i) WHERE i % 10 = 0");
	std::thread interrupter([&con]() {
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		con.Interrupt();
	});
	handle->Complete();
	interrupter.join();
	REQUIRE(handle->HasError());
	REQUIRE(StringUtil::Contains(handle->GetError(), "INTERRUPT"));
}

#endif
