#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/execution/operator/helper/physical_result_sink.hpp"
#include "duckdb/execution/operator/scan/physical_dummy_scan.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/main/buffered_data/batched_buffered_data.hpp"
#include "duckdb/main/buffered_data/simple_buffered_data.hpp"
#include "duckdb/main/prepared_statement_data.hpp"
#include "duckdb/main/result_format.hpp"

using namespace duckdb;

TEST_CASE("Test results API", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);

	// result equality
	auto result = con.Query("SELECT 42");
	auto result2 = con.Query("SELECT 42");
	REQUIRE(result->Equals(*result2));

	// result inequality
	result = con.Query("SELECT 42");
	result2 = con.Query("SELECT 43");
	REQUIRE(!result->Equals(*result2));

	// submitted query to string
	auto submitted = con.Submit("SELECT 42");
	auto str = submitted->ToString();
	REQUIRE(!str.empty());

	// materialized query to string
	result = con.Query("SELECT 42");
	str = result->ToString();
	REQUIRE(!str.empty());

	// error to string
	result = con.Query("SELEC 42");
	str = result->ToString();
	REQUIRE(!str.empty());
}

TEST_CASE("Test iterating over results", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);

	REQUIRE_NO_FAIL(con.Query("CREATE TABLE data(i INTEGER, j VARCHAR)"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO data VALUES (1, 'hello'), (2, 'test')"));

	duckdb::vector<int> i_values = {1, 2};
	duckdb::vector<string> j_values = {"hello", "test"};
	idx_t row_count = 0;
	auto result = con.Query("SELECT * FROM data;");
	for (auto &row : *result) {
		REQUIRE(row.GetValue<int>(0) == i_values[row_count]);
		REQUIRE(row.GetValue<string>(1) == j_values[row_count]);
		row_count++;
	}
	REQUIRE(row_count == 2);
}

TEST_CASE("Test different result types", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);

	REQUIRE_NO_FAIL(
	    con.Query("CREATE TABLE data(i INTEGER, j VARCHAR, k DECIMAL(38,1), l DECIMAL(18,3), m HUGEINT, n DOUBLE)"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO data VALUES (23, '17.1', 94289, 9842, 4982412, 17.3)"));

	idx_t row_count = 0;
	auto result = con.Query("SELECT * FROM data;");
	for (auto &row : *result) {
		REQUIRE(row.GetValue<int>(0) == 23);
		REQUIRE(row.GetValue<int64_t>(0) == 23);
		REQUIRE(row.GetValue<double>(0) == 23);
		REQUIRE(row.GetValue<string>(0) == "23");

		REQUIRE(row.GetValue<int>(1) == 17);
		REQUIRE(row.GetValue<int64_t>(1) == 17);
		REQUIRE(row.GetValue<double>(1) == 17.1);
		REQUIRE(row.GetValue<string>(1) == "17.1");

		REQUIRE(row.GetValue<int>(2) == 94289);
		REQUIRE(row.GetValue<int64_t>(2) == 94289);
		REQUIRE(row.GetValue<double>(2) == 94289);

		REQUIRE(row.GetValue<int>(3) == 9842);
		REQUIRE(row.GetValue<int64_t>(3) == 9842);
		REQUIRE(row.GetValue<double>(3) == 9842);

		REQUIRE(row.GetValue<int>(4) == 4982412);
		REQUIRE(row.GetValue<int64_t>(4) == 4982412);
		REQUIRE(row.GetValue<double>(4) == 4982412);
		REQUIRE(row.GetValue<string>(4) == "4982412");

		REQUIRE(row.GetValue<int>(5) == 17);
		REQUIRE(row.GetValue<int64_t>(5) == 17);
		REQUIRE(row.GetValue<double>(5) == 17.3);

		row_count++;
	}
	REQUIRE(row_count == 1);
}

TEST_CASE("Test dates/times/timestamps", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);

	REQUIRE_NO_FAIL(con.Query("CREATE TABLE data(i DATE, j TIME, k TIMESTAMP)"));
	REQUIRE_NO_FAIL(
	    con.Query("INSERT INTO data VALUES (DATE '1992-01-01', TIME '13:00:17', TIMESTAMP '1993-01-01 14:00:17')"));

	idx_t row_count = 0;
	auto result = con.Query("SELECT * FROM data;");
	for (auto &row : *result) {
		int32_t year, month, day;
		int32_t hour, minute, second, millisecond;

		auto date = row.GetValue<date_t>(0);
		auto time = row.GetValue<dtime_t>(1);
		auto timestamp = row.GetValue<timestamp_t>(2);
		Date::Convert(date, year, month, day);
		REQUIRE(year == 1992);
		REQUIRE(month == 1);
		REQUIRE(day == 1);

		Time::Convert(time, hour, minute, second, millisecond);
		REQUIRE(hour == 13);
		REQUIRE(minute == 0);
		REQUIRE(second == 17);
		REQUIRE(millisecond == 0);

		Timestamp::Convert(timestamp, date, time);
		Date::Convert(date, year, month, day);
		Time::Convert(time, hour, minute, second, millisecond);

		REQUIRE(year == 1993);
		REQUIRE(month == 1);
		REQUIRE(day == 1);
		REQUIRE(hour == 14);
		REQUIRE(minute == 0);
		REQUIRE(second == 17);
		REQUIRE(millisecond == 0);

		row_count++;
	}
	REQUIRE(row_count == 1);
}

TEST_CASE("Error in streaming result after initial query", "[api][.]") {
	DuckDB db(nullptr);
	Connection con(db);

	// create a big table with strings that are numbers
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE strings(v VARCHAR)"));
	for (size_t i = 0; i < STANDARD_VECTOR_SIZE * 2 - 1; i++) {
		REQUIRE_NO_FAIL(con.Query("INSERT INTO strings VALUES ('" + to_string(i) + "')"));
	}
	// now insert one non-numeric value
	REQUIRE_NO_FAIL(con.Query("INSERT INTO strings VALUES ('hello')"));

	// now create a streaming result: the bad row sits in a later chunk, so the error surfaces on the drain
	auto stream = OpenStream(con, "SELECT CAST(v AS INTEGER) FROM strings");
	while (stream->Fetch()) {
	}
	REQUIRE(stream->HasError());
}

TEST_CASE("A retained result sink rejects results after the connection closes", "[api]") {
	DuckDB db(nullptr);

	auto test_collector = [&](ResultOrdering ordering) {
		auto connection = make_uniq<Connection>(db);
		weak_ptr<ClientContext> weak_context = connection->context;

		PreparedStatementData data(StatementType::SELECT_STATEMENT);
		data.names.emplace_back("value");
		data.types.emplace_back(LogicalType::INTEGER);
		data.physical_plan = make_uniq<PhysicalPlan>(Allocator::Get(*connection->context));
		auto &root = data.physical_plan->Make<PhysicalDummyScan>(data.types, 0);
		data.physical_plan->SetRoot(root);

		// The sink carries no lifetime of its own; the buffer built below is what fixes it to RETAINED
		auto sink = make_uniq<PhysicalResultSink>(*data.physical_plan, data, ordering);
		ResultFormatContext format_context {data.types, data.names, connection->context->GetClientProperties(),
		                                    ordering};
		shared_ptr<BufferedData> buffer;
		if (ordering == ResultOrdering::BATCH_INDEX_ORDERED) {
			buffer = make_shared_ptr<BatchedBufferedData>(*connection->context, ResultLifetime::RETAINED,
			                                              std::move(format_context), nullptr);
		} else {
			buffer = make_shared_ptr<SimpleBufferedData>(*connection->context, ResultLifetime::RETAINED,
			                                             std::move(format_context), nullptr);
		}
		sink->SetResultBuffer(buffer);
		unique_ptr<PhysicalResultCollector> collector = std::move(sink);
		auto sink_state = collector->GetGlobalSinkState(*connection->context);

		connection.reset();
		REQUIRE(weak_context.expired());
		REQUIRE_THROWS_AS(collector->GetResult(*sink_state), ConnectionException);
	};

	SECTION("source ordered") {
		test_collector(ResultOrdering::SOURCE_ORDERED);
	}
	SECTION("batch index ordered") {
		test_collector(ResultOrdering::BATCH_INDEX_ORDERED);
	}
}

TEST_CASE("Test UUID", "[api][uuid]") {
	DuckDB db(nullptr);
	Connection con(db);

	REQUIRE_NO_FAIL(con.Query("CREATE TABLE uuids (u uuid)"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO uuids VALUES ('A0EEBC99-9C0B-4EF8-BB6D-6BB9BD380A11'), "
	                          "('a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11');"));
	REQUIRE_FAIL(con.Query("INSERT INTO uuids VALUES ('');"));
	REQUIRE_FAIL(con.Query("INSERT INTO uuids VALUES ('a0eebc99');"));
	REQUIRE_FAIL(con.Query("INSERT INTO uuids VALUES ('a0eebc99-9c0b-4ef8-bb6d-6bb9bd380z11');"));

	idx_t row_count = 0;
	auto result = con.Query("SELECT * FROM uuids");
	for (auto &row : *result) {
		auto uuid = row.GetValue<string>(0);
		REQUIRE(uuid == "a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11");
		row_count++;
	}
	REQUIRE(row_count == 2);
}

TEST_CASE("Test ARRAY_AGG with ORDER BY", "[api][array_agg]") {
	DuckDB db(nullptr);
	Connection con(db);

	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t2 (a INT, b INT, c INT)"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO t2 VALUES (1,1,1), (1,2,2), (2,1,3), (2,2,4)"));

	auto result = con.Query("select a, array_agg(c ORDER BY b) from t2 GROUP BY a");
	REQUIRE(!result->HasError());
	REQUIRE(result->ColumnName(1) == "array_agg(c ORDER BY b)");
}

TEST_CASE("Issue #9417", "[api][.]") {
	DBConfig config;
	config.SetOptionByName("allow_unsigned_extensions", true);

	DuckDB db(TestCreatePath("issue_replication.db"), &config);
	Connection con(db);
	auto result = con.Query("with max_period as ("
	                        "            select max(reporting_date) as max_record\n"
	                        "            from \"data/parquet-testing/issue9417.parquet\"\n"
	                        "        )\n"
	                        "        select\n"
	                        "            *\n"
	                        "        from \"data/parquet-testing/issue9417.parquet\" e\n"
	                        "            inner join max_period\n"
	                        "            on e.reporting_date = max_period.max_record\n"
	                        "         where e.record_date between '2012-01-31' and '2023-06-30'");
	idx_t count = 0;
	while (true) {
		auto chunk = result->Fetch();
		if (chunk) {
			REQUIRE(count + chunk->size() <= 46);
			count += chunk->size();
		} else {
			break;
		}
	}

	REQUIRE(count == 46);
}
