//===----------------------------------------------------------------------===//
//
// benchmark/include/interpreted_benchmark.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "benchmark.hpp"
#include "duckdb/main/query_result.hpp"

#include <unordered_map>
#include <unordered_set>
#include "duckdb/common/insertion_order_preserving_map.hpp"

namespace duckdb {
struct BenchmarkFileReader;
struct InterpretedBenchmarkState;

//! How the benchmark consumes the result of its run query
enum class BenchmarkResultMode : uint8_t { RETAINED, STREAMING, ARROW, ARROW_STREAM };

const string DEFAULT_DB_PATH = "duckdb_benchmark_db.db";

struct BenchmarkQuery {
public:
	BenchmarkQuery() {
	}

public:
	string query;
	idx_t column_count = 0;
	vector<vector<string>> expected_result;
};

//! Interpreted benchmarks read the benchmark from a file
class InterpretedBenchmark : public Benchmark {
public:
	explicit InterpretedBenchmark(string full_path);

	void LoadBenchmark();
	//! Initialize the benchmark state
	duckdb::unique_ptr<BenchmarkState> Initialize(BenchmarkConfiguration &config) override;
	//! Assert correct/expected state of the db, before Run
	void Assert(BenchmarkState *state) override;
	//! Run the benchmark
	void Run(BenchmarkState *state) override;
	//! Cleanup the benchmark, called after each Run
	void Cleanup(BenchmarkState *state) override;
	//! Verify that the output of the benchmark was correct
	string Verify(BenchmarkState *state) override;

	string GetQuery() override;
	//! Interrupt the benchmark because of a timeout
	void Interrupt(BenchmarkState *state) override;
	//! Returns information about the benchmark
	string BenchmarkInfo() override;

	string GetLogOutput(BenchmarkState *state) override;

	string DisplayName() override;
	string Group() override;
	string Subgroup() override;

	string GetDatabasePath();

	bool InMemory() {
		return in_memory;
	}

	bool RequireReinit() override {
		return require_reinit;
	}

private:
	string VerifyInternal(BenchmarkState *state_p, const BenchmarkQuery &query, QueryResult &result);

	BenchmarkQuery ReadQueryFromFile(BenchmarkFileReader &reader, string file);
	BenchmarkQuery ReadQueryFromReader(BenchmarkFileReader &reader, const string &sql, const string &header);

	unique_ptr<QueryResult> RunLoadQuery(InterpretedBenchmarkState &state, const string &load_query);

	void ProcessFile(const string &path);
	void AddExtension(const string &extension, bool load_only);
	void LoadExtensions(InterpretedBenchmarkState &state, bool is_load_set);

private:
	bool is_loaded = false;
	std::unordered_map<string, string> replacement_mapping;
	unordered_set<string> handled_arguments;

	std::unordered_map<string, string> queries;
	string run_query;

	string benchmark_path;
	string cache_db = "";
	string cache_file = "";
	// check the existence of a cached db, but do not connect
	// can be used to test accessing data from a different db in a non-persistent connection
	bool cache_no_connect = false;
	InsertionOrderPreservingMap<idx_t> extensions_map;
	InsertionOrderPreservingMap<idx_t> load_extensions_map;

	//! Queries used to assert a given state of the data
	vector<BenchmarkQuery> assert_queries;
	vector<BenchmarkQuery> result_queries;
	//! How many times to retry the load, if any
	idx_t retry_load = 0;

	string display_name;
	string display_group;
	string subgroup;

	bool in_memory = true;
	string storage_version;
	BenchmarkResultMode result_mode = BenchmarkResultMode::RETAINED;
	//! Discard fetched chunks instead of materializing them into the benchmark result
	bool discard_stream_result = false;
	idx_t arrow_batch_size = STANDARD_VECTOR_SIZE;
	bool require_reinit = false;
};

} // namespace duckdb
