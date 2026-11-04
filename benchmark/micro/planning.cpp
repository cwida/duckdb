#include "benchmark_runner.hpp"
#include "duckdb_benchmark.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

enum class WideGroupingMode : uint8_t { CONTROL, GENERATED, MIXED };

class WideGroupingBenchmark : public DuckDBBenchmark {
public:
	WideGroupingBenchmark(idx_t column_count, WideGroupingMode mode, const string &name)
	    : DuckDBBenchmark(true, "Planning.WideGrouping." + to_string(column_count) + "." + name, "[planning]"),
	      column_count(column_count), mode(mode) {
	}

	void Load(DuckDBBenchmarkState *state) override {
		vector<string> definitions;
		vector<string> expressions;
		vector<string> groups;
		for (idx_t i = 0; i < column_count; i++) {
			auto column = "c" + to_string(i);
			definitions.push_back(column + " INTEGER");
			expressions.push_back("r." + column + " + 1");
			groups.push_back("r." + column);
		}
		definitions.push_back("g AS (c0 + 1)");
		if (mode != WideGroupingMode::CONTROL) {
			expressions.push_back("r.g");
		}
		if (mode == WideGroupingMode::MIXED) {
			groups.push_back("r.g");
		}
		expressions.push_back("count(*)");
		query = "EXPLAIN SELECT " + StringUtil::Join(expressions, ", ") +
		        " FROM (VALUES (1)) l(i) LEFT JOIN r ON l.i = r.c0 GROUP BY " + StringUtil::Join(groups, ", ");
		auto result = state->conn.Query("SET threads=1; CREATE TABLE r(" + StringUtil::Join(definitions, ", ") + ")");
		if (result->HasError()) {
			throw InvalidInputException(result->GetError());
		}
	}

	string GetQuery() override {
		return query;
	}

	string VerifyResult(QueryResult *result) override {
		return result->HasError() ? result->GetError() : string();
	}

	string BenchmarkInfo() override {
		return "Plan an empty outer join with " + to_string(column_count) +
		       " ordinary grouping keys; table creation is outside the timed EXPLAIN";
	}

private:
	idx_t column_count;
	WideGroupingMode mode;
	string query;
};

static WideGroupingBenchmark wide_grouping_500_control(500, WideGroupingMode::CONTROL, "Control");
static WideGroupingBenchmark wide_grouping_500_generated(500, WideGroupingMode::GENERATED, "Generated");
static WideGroupingBenchmark wide_grouping_500_mixed(500, WideGroupingMode::MIXED, "Mixed");
static WideGroupingBenchmark wide_grouping_1000_control(1000, WideGroupingMode::CONTROL, "Control");
static WideGroupingBenchmark wide_grouping_1000_generated(1000, WideGroupingMode::GENERATED, "Generated");
static WideGroupingBenchmark wide_grouping_1000_mixed(1000, WideGroupingMode::MIXED, "Mixed");
static WideGroupingBenchmark wide_grouping_2000_control(2000, WideGroupingMode::CONTROL, "Control");
static WideGroupingBenchmark wide_grouping_2000_generated(2000, WideGroupingMode::GENERATED, "Generated");
static WideGroupingBenchmark wide_grouping_2000_mixed(2000, WideGroupingMode::MIXED, "Mixed");

} // namespace duckdb
