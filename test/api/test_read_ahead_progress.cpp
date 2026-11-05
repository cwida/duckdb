#include "catch.hpp"
#include "duckdb/common/multi_file/table_function_multi_file.hpp"
#include "duckdb/parallel/scan_read_ahead.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "test_helpers.hpp"

using namespace duckdb;
using namespace std;

TEST_CASE("Parquet read-ahead supports a schema supplied without binding a file", "[api][parquet]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto path = TestCreatePath("read_ahead_schema.parquet");
	REQUIRE_NO_FAIL(con.Query("COPY (SELECT range AS i FROM range(10000)) TO '" + path +
	                          "' (FORMAT parquet, ROW_GROUP_SIZE 2048)"));

	string query;
	bool supplied_schema = false;
	SECTION("Schema read from the file") {
		query = "SELECT i FROM read_parquet('" + path + "')";
	}
	SECTION("Schema supplied by the caller") {
		supplied_schema = true;
		query = "SELECT i FROM read_parquet('" + path +
		        "', schema=map{'i': {'name': 'i', 'type': 'BIGINT', 'default_value': NULL}})";
	}
	auto plan = con.ExtractPlan(query);
	auto op = plan.get();
	while (op->type != LogicalOperatorType::LOGICAL_GET) {
		REQUIRE(op->children.size() == 1);
		op = op->children[0].get();
	}
	auto &bind_data = op->Cast<LogicalGet>().bind_data->Cast<MultiFileBindData>();
	if (supplied_schema) {
		auto &data = bind_data.bind_data->Cast<TableFunctionMultiFileData>();
		REQUIRE_FALSE(data.options.schema_bind_data);
		REQUIRE(bind_data.union_readers.empty());
	}
	REQUIRE(bind_data.interface->SupportsReadAhead(bind_data));
	auto result = con.Query(query);
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->RowCount() == 10000);
}

TEST_CASE("Read-ahead progress only counts the assignments a thread is decoding", "[api]") {
	auto path = TestCreatePath("read_ahead_progress.db");
	DeleteDatabase(path);
	DuckDB db(path);
	Connection con(db);

	// ten row groups
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE integers AS SELECT range AS i FROM range(1228800)"));
	REQUIRE_NO_FAIL(con.Query("CHECKPOINT"));
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	REQUIRE_NO_FAIL(con.Query("SET async_threads=4"));
	REQUIRE_NO_FAIL(con.Query("SET storage_block_prefetch='debug_force_always'"));
	REQUIRE_NO_FAIL(con.Query("SET read_ahead_depth=4"));
	REQUIRE_NO_FAIL(con.Query("SET enable_progress_bar=true"));
	REQUIRE_NO_FAIL(con.Query("SET enable_progress_bar_print=false"));
	// the default streaming buffer holds the whole table, so a fetch would drain the scan before we look
	REQUIRE_NO_FAIL(con.Query("SET streaming_buffer_size='64KB'"));

	// after one chunk read-ahead has claimed four row groups, only the first of them is being decoded
	auto stream = OpenStream(con, "SELECT i FROM integers");
	REQUIRE_FALSE(stream->HasError());
	auto chunk = stream->Fetch();
	REQUIRE(chunk);
	auto percentage = con.context->GetQueryProgress().GetPercentage();
	REQUIRE(percentage >= 0);
	REQUIRE(percentage < 20);
	stream.reset();

	// with single vector assignments the claimed rows are single vectors as well, buffer only a chunk or two
	REQUIRE_NO_FAIL(con.Query("PRAGMA verify_parallelism"));
	REQUIRE_NO_FAIL(con.Query("SET streaming_buffer_size='16KB'"));
	stream = OpenStream(con, "SELECT i FROM integers");
	REQUIRE_FALSE(stream->HasError());
	chunk = stream->Fetch();
	REQUIRE(chunk);
	percentage = con.context->GetQueryProgress().GetPercentage();
	REQUIRE(percentage > 0);
	REQUIRE(percentage < 5);
	stream.reset();
}

TEST_CASE("Read-ahead settles a file open that never runs", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);

	std::atomic<bool> opened {false};
	std::atomic<bool> settled {false};
	{
		ScanReadAhead read_ahead(*con.context, 1, nullptr);
		read_ahead.PushError(ErrorData("injected read-ahead error"));
		read_ahead.ScheduleFileOpen([&]() { opened = true; }, [&]() { settled = true; });
		// leaving the scope cancels and drains, retiring the open without running it
	}
	REQUIRE(settled);
	REQUIRE(!opened);
}

#ifndef DUCKDB_NO_THREADS
TEST_CASE("Automatic read-ahead grows its backlog within the memory budget", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=2"));
	REQUIRE_NO_FAIL(con.Query("SET async_threads=1"));
	REQUIRE_NO_FAIL(con.Query("SET read_ahead_depth=-1"));
	REQUIRE_NO_FAIL(con.Query("SET memory_limit='1GB'"));

	bool expect_overlap = true;
	SECTION("Two scan threads") {
	}
	SECTION("One scan thread") {
		REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	}
	SECTION("Low memory limits the backlog") {
		REQUIRE_NO_FAIL(con.Query("SET memory_limit='32MB'"));
		expect_overlap = false;
	}

	auto read_ahead = ScanReadAhead::Create(*con.context);
	REQUIRE(read_ahead);
	struct JobState {};
	const idx_t job_count = 32;
	idx_t produced = 0;
	auto produce = [&](duckdb::vector<duckdb::unique_ptr<AsyncTask>> &) -> duckdb::unique_ptr<ScanReadAheadJob> {
		if (produced == job_count) {
			return nullptr;
		}
		auto job = make_uniq<ScanReadAheadJobWrapper<JobState>>();
		job->batch_index = produced++;
		return std::move(job);
	};
	TableFunctionInput input(nullptr, nullptr, nullptr);
	for (idx_t consumed = 0; consumed < job_count; consumed++) {
		duckdb::unique_ptr<ScanReadAheadJob> job;
		REQUIRE(read_ahead->AcquireJob(*con.context, input, produce, job) == ScanReadAheadAcquire::ACQUIRED);
		REQUIRE(job->batch_index == consumed);
		if (consumed == 0) {
			// One producer must fill a bounded backlog before the first job is consumed.
			REQUIRE(produced < job_count);
			if (expect_overlap) {
				REQUIRE(produced > 1);
			} else {
				REQUIRE(produced == 1);
			}
		}
	}
	duckdb::unique_ptr<ScanReadAheadJob> job;
	REQUIRE(read_ahead->AcquireJob(*con.context, input, produce, job) == ScanReadAheadAcquire::EXHAUSTED);
	REQUIRE_FALSE(job);
	REQUIRE(produced == job_count);
}
#endif
