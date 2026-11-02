#include "benchmark_runner.hpp"
#include "duckdb_benchmark_macro.hpp"
#include "duckdb/main/appender.hpp"

#include <random>

using namespace duckdb;

#define GROUP_ROW_COUNT 1000000
#define GROUP_COUNT     5

DUCKDB_BENCHMARK(BulkUpdate, "[bulkupdate]")
int64_t sum = 0;
int64_t count = 0;
void Load(DuckDBBenchmarkState *state) override {
	state->conn.Query("CREATE TABLE integers(i INTEGER);");
	Appender appender(state->conn, "integers");
	// insert the elements into the database
	for (size_t i = 0; i < GROUP_ROW_COUNT; i++) {
		appender.BeginRow();
		appender.Append<int32_t>(i % GROUP_COUNT);
		appender.EndRow();

		sum += i % GROUP_COUNT;
		count++;
	}
}

void RunBenchmark(DuckDBBenchmarkState *state) override {
	state->conn.Query("BEGIN TRANSACTION");
	state->conn.Query("UPDATE integers SET i = i + 1");
	state->result = state->conn.Query("SELECT SUM(i) FROM integers");
	state->conn.Query("ROLLBACK");
}

string VerifyResult(QueryResult *result) override {
	Value val = result->Collection().GetValue(0, 0);
	if (val != Value::BIGINT(sum + count)) {
		return string("Value " + val.ToString() + " does not match expected value " + std::to_string(sum + count));
	}
	return string();
}
string BenchmarkInfo() override {
	return "Run a bulk update followed by an aggregate";
}
FINISH_BENCHMARK(BulkUpdate)

DUCKDB_BENCHMARK(BulkDelete, "[bulkupdate]")
int64_t sum = 0;
int64_t count = 0;
void Load(DuckDBBenchmarkState *state) override {
	state->conn.Query("CREATE TABLE integers(i INTEGER);");
	Appender appender(state->conn, "integers");
	// insert the elements into the database
	for (size_t i = 0; i < GROUP_ROW_COUNT; i++) {
		appender.BeginRow();
		appender.Append<int32_t>(i % GROUP_COUNT);
		appender.EndRow();

		sum += i % GROUP_COUNT;
		if ((i % GROUP_COUNT) == 1) {
			count++;
		}
	}
}

void RunBenchmark(DuckDBBenchmarkState *state) override {
	state->conn.Query("BEGIN TRANSACTION");
	state->conn.Query("DELETE FROM integers WHERE i=1");
	state->result = state->conn.Query("SELECT SUM(i) FROM integers");
	state->conn.Query("ROLLBACK");
}

string VerifyResult(QueryResult *result) override {
	Value val = result->Collection().GetValue(0, 0);
	if (val != Value::BIGINT(sum - count)) {
		return string("Value " + val.ToString() + " does not match expected value " + std::to_string(sum - count));
	}
	return string();
}
string BenchmarkInfo() override {
	return "Run a bulk delete followed by an aggregate";
}
FINISH_BENCHMARK(BulkDelete)
