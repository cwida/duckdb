#include "benchmark_runner.hpp"
#include "compare_result.hpp"
#include "dbgen.hpp"
#include "duckdb_benchmark_macro.hpp"

using namespace duckdb;
using namespace std;

#define SF 0.1

DUCKDB_BENCHMARK(ReadLineitemCSV, "[csv]")
int64_t count = 0;
virtual void Load(DuckDBBenchmarkState *state) {
	// load the data into the tpch schema
	state->conn.Query("CREATE SCHEMA tpch");
	tpch::dbgen(SF, state->db, "tpch");
	// create the CSV file
	auto result = state->conn.Query("COPY tpch.lineitem TO 'lineitem.csv' DELIMITER '|' HEADER");
	assert(result->GetSuccess());
	count = result->collection.chunks[0]->data[0].GetValue(0).GetNumericValue();
	// delete the database
	state->conn.Query("DROP SCHEMA tpch CASCADE");
	// create the empty schema to load into
	tpch::dbgen(0, state->db);
}
virtual string GetQuery() {
	return "COPY lineitem FROM 'lineitem.csv' DELIMITER '|' HEADER";
}
virtual string VerifyResult(DuckDBResult *result) {
	if (!result->GetSuccess()) {
		return result->GetErrorMessage();
	}
	auto expected_count = result->collection.chunks[0]->data[0].GetValue(0).GetNumericValue();
	if (expected_count != count) {
		return StringUtil::Format("Count mismatch, expected %lld elements but got %lld", count, expected_count);
	}
	return string();
}
virtual string BenchmarkInfo() {
	return "Read the lineitem table from SF 0.1 from CSV format";
}
FINISH_BENCHMARK(ReadLineitemCSV)
