#include "catch.hpp"
#include "duckdb.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/multi_file/table_function_multi_file.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "test_helpers.hpp"

#include <fstream>

using namespace duckdb;

namespace {

//! Counts the executions of the wrapped reader - the rows it returns are constant, so re-execution is only
//! observable through this counter
atomic<idx_t> wrapped_reader_executions {0};

//! Counts the executions of the counting reader - the rows it returns encode this counter, so a wrongly
//! retained or shared scan changes query results
atomic<idx_t> counting_reader_executions {0};

struct WrappedReaderGlobalState : public GlobalTableFunctionState {
	idx_t emitted_rows = 0;
	//! The first row value of this execution - the counting reader derives it from its execution counter
	idx_t base = 0;
};

unique_ptr<FunctionData> WrappedReaderBind(ClientContext &, TableFunctionBindInput &, vector<LogicalType> &return_types,
                                           vector<Identifier> &names) {
	return_types.emplace_back(LogicalType::INTEGER);
	names.emplace_back("k");
	return make_uniq<TableFunctionData>();
}

unique_ptr<GlobalTableFunctionState> WrappedReaderInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<WrappedReaderGlobalState>();
}

void WrappedReaderFunction(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	// emit chunk-wise: small STANDARD_VECTOR_SIZE builds cannot hold all rows in one chunk
	constexpr idx_t TOTAL_ROWS = 3;
	auto &state = data.global_state->Cast<WrappedReaderGlobalState>();
	if (state.emitted_rows >= TOTAL_ROWS) {
		output.SetChildCardinality(0);
		return;
	}
	if (state.emitted_rows == 0) {
		++wrapped_reader_executions;
	}
	const idx_t count = MinValue<idx_t>(TOTAL_ROWS - state.emitted_rows, STANDARD_VECTOR_SIZE);
	auto result_data = FlatVector::GetDataMutable<int32_t>(output.data[0]);
	for (idx_t i = 0; i < count; i++) {
		result_data[i] = NumericCast<int32_t>(state.emitted_rows + i + 1);
	}
	output.SetChildCardinality(count);
	state.emitted_rows += count;
}

void CountingReaderFunction(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	// emit chunk-wise: small STANDARD_VECTOR_SIZE builds cannot hold all rows in one chunk
	constexpr idx_t TOTAL_ROWS = 3;
	auto &state = data.global_state->Cast<WrappedReaderGlobalState>();
	if (state.emitted_rows >= TOTAL_ROWS) {
		output.SetChildCardinality(0);
		return;
	}
	if (state.emitted_rows == 0) {
		// execution e returns the rows (e - 1) * TOTAL_ROWS + 1 .. e * TOTAL_ROWS
		state.base = (++counting_reader_executions - 1) * TOTAL_ROWS;
	}
	const idx_t count = MinValue<idx_t>(TOTAL_ROWS - state.emitted_rows, STANDARD_VECTOR_SIZE);
	auto result_data = FlatVector::GetDataMutable<int32_t>(output.data[0]);
	for (idx_t i = 0; i < count; i++) {
		result_data[i] = NumericCast<int32_t>(state.base + state.emitted_rows + i + 1);
	}
	output.SetChildCardinality(count);
	state.emitted_rows += count;
}

void RegisterWrappedReader(DuckDB &db, const string &name, table_function_t function, bool declare_volatile) {
	TableFunction single_file(Identifier(name + "_single_file"),
	                          FunctionSignature().AddPositionalOnly("file", LogicalType::ANY), function,
	                          WrappedReaderBind, WrappedReaderInit);
	if (declare_volatile) {
		single_file.stability = FunctionStability::VOLATILE;
	}
	TableFunctionMultiFileSettings settings;
	auto wrapped =
	    TableFunctionMultiFileWrapper::CreateFunction(std::move(single_file), Identifier(name), std::move(settings));

	ExtensionInfo extension_info {};
	ExtensionActiveLoad load_info {*db.instance, extension_info, "test_extension", ""};
	ExtensionLoader loader {load_info};
	loader.RegisterFunction(std::move(wrapped));
}

string CreateReaderInputFile(const string &name) {
	// the multi-file wrapper globs the file argument, so the file has to exist - its content is never read
	auto path = TestCreatePath(name);
	std::ofstream input_file(path);
	input_file << "unused" << std::endl;
	input_file.close();
	return path;
}

} // namespace

// SQL cannot register a multi-file-wrapped reader whose wrapped function carries a controlled stability
// declaration and an observable execution counter. Keep the wrapper admission contract here, with the builtin
// reader coverage in SQL: the wrapper carries the wrapped reader's declared stability through, so an undeclared
// reader falls under the stable-within-query default while an explicitly volatile reader stays excluded.
TEST_CASE("Multi-file wrapping carries the wrapped reader's declared stability", "[optimizer][common_subplan]") {
	DuckDB db(nullptr);
	Connection con(db);
	RegisterWrappedReader(db, "wrapped_reader_unknown", WrappedReaderFunction, false);
	RegisterWrappedReader(db, "wrapped_reader_volatile", WrappedReaderFunction, true);

	auto path = CreateReaderInputFile("multi_file_repeatability_input");

	// pin the reader to the retained build side, like the repeatability logging fixture does
	REQUIRE_NO_FAIL(con.Query("SET enable_caching_operators=true"));
	REQUIRE_NO_FAIL(con.Query("SET disabled_optimizers='join_order,build_side_probe_side'"));

	auto run_query = [&](const string &name) {
		const string query = "WITH RECURSIVE r(i) AS ("
		                     "    VALUES (0)"
		                     "    UNION ALL"
		                     "    SELECT r.i + 1 FROM r"
		                     "    JOIN (SELECT k FROM " +
		                     name + "('" + path +
		                     "')) build ON build.k = r.i + 1"
		                     "    WHERE r.i < 3"
		                     ") SELECT count(*) FROM r";
		wrapped_reader_executions = 0;
		auto result = con.Query(query);
		REQUIRE_NO_FAIL(*result);
		REQUIRE(CHECK_COLUMN(result, 0, {4}));
	};

	// contract decision from the PR review: an undeclared table function is stable within the query, so the
	// wrapped reader's hash build is retained across the iterations and the reader executes once
	run_query("wrapped_reader_unknown");
	REQUIRE(wrapped_reader_executions.load() == 1);

	// an explicitly volatile reader must be re-executed every iteration
	run_query("wrapped_reader_volatile");
	REQUIRE(wrapped_reader_executions.load() >= 2);
}

// SQL cannot register a reader whose returned rows encode its execution count. Unlike the counter test above,
// which pins the no-observable-side-effects contract, this reader makes a retained or shared scan visible
// directly in the query result - the shape of the original wrong-results report.
TEST_CASE("Multi-file wrapping keeps execution-count-valued reader results correct", "[optimizer][common_subplan]") {
	DuckDB db(nullptr);
	Connection con(db);
	RegisterWrappedReader(db, "counting_reader_unknown", CountingReaderFunction, false);
	RegisterWrappedReader(db, "counting_reader_volatile", CountingReaderFunction, true);

	auto path = CreateReaderInputFile("multi_file_repeatability_counting_input");

	// pin the reader to the retained build side, like the repeatability logging fixture does
	REQUIRE_NO_FAIL(con.Query("SET disabled_optimizers='join_order,build_side_probe_side'"));

	// execution 1 returns the rows 1..3, so the recursion only continues past i = 1 when the first execution
	// is retained - re-execution returns 4..6 next and stops the recursion
	auto run_query = [&](const string &name, bool caching, const string &expected) {
		const string query = "WITH RECURSIVE r(i) AS ("
		                     "    VALUES (0)"
		                     "    UNION ALL"
		                     "    SELECT r.i + 1 FROM r"
		                     "    JOIN (SELECT k FROM " +
		                     name + "('" + path +
		                     "')) build ON build.k = r.i + 1"
		                     "    WHERE r.i < 5"
		                     ") SELECT list(i ORDER BY i)::VARCHAR FROM r";
		REQUIRE_NO_FAIL(con.Query("SET enable_caching_operators=" + string(caching ? "true" : "false")));
		counting_reader_executions = 0;
		auto result = con.Query(query);
		REQUIRE_NO_FAIL(*result);
		REQUIRE(CHECK_COLUMN(result, 0, {expected}));
	};

	// an explicitly volatile reader is re-executed every iteration, with and without caching operators
	run_query("counting_reader_volatile", false, "[0, 1]");
	run_query("counting_reader_volatile", true, "[0, 1]");

	// contract decision from the PR review: an undeclared reader is stable within the query, so its build is
	// retained and the recursion keeps joining against the rows of the first execution. This reader violates
	// the default contract on purpose - a reader with execution-count-valued rows must declare VOLATILE
	run_query("counting_reader_unknown", false, "[0, 1]");
	run_query("counting_reader_unknown", true, "[0, 1, 2, 3]");
}
