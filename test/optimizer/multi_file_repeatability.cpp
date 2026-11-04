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

struct WrappedReaderGlobalState : public GlobalTableFunctionState {
	bool emitted = false;
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
	auto &state = data.global_state->Cast<WrappedReaderGlobalState>();
	if (state.emitted) {
		output.SetChildCardinality(0);
		return;
	}
	++wrapped_reader_executions;
	auto result_data = FlatVector::GetDataMutable<int32_t>(output.data[0]);
	for (int32_t i = 0; i < 3; i++) {
		result_data[i] = i + 1;
	}
	output.SetChildCardinality(3);
	state.emitted = true;
}

void RegisterWrappedReader(DuckDB &db, const string &name, bool with_false_callback) {
	TableFunction single_file(Identifier(name + "_single_file"),
	                          FunctionSignature().AddPositionalOnly("file", LogicalType::ANY), WrappedReaderFunction,
	                          WrappedReaderBind, WrappedReaderInit);
	if (with_false_callback) {
		single_file.is_repeatable = [](optional_ptr<const FunctionData>) {
			return false;
		};
	}
	TableFunctionMultiFileSettings settings;
	auto wrapped =
	    TableFunctionMultiFileWrapper::CreateFunction(std::move(single_file), Identifier(name), std::move(settings));

	ExtensionInfo extension_info {};
	ExtensionActiveLoad load_info {*db.instance, extension_info, "test_extension", ""};
	ExtensionLoader loader {load_info};
	loader.RegisterFunction(std::move(wrapped));
}

} // namespace

// SQL cannot register a multi-file-wrapped reader whose wrapped function carries a controlled repeatability
// callback and an observable execution counter. Keep the wrapper admission contract here, with the builtin
// reader coverage in SQL: the wrapper must not declare a wrapped reader repeatable on its behalf, neither when
// the reader leaves the callback unset nor when its own callback explicitly returns false.
TEST_CASE("Multi-file wrapping does not make a stateful reader repeatable", "[optimizer][common_subplan]") {
	DuckDB db(nullptr);
	Connection con(db);
	RegisterWrappedReader(db, "wrapped_reader_unknown", false);
	RegisterWrappedReader(db, "wrapped_reader_false", true);

	// the multi-file wrapper globs the file argument, so the file has to exist - its content is never read
	auto path = TestCreatePath("multi_file_repeatability_input");
	std::ofstream input_file(path);
	input_file << "unused" << std::endl;
	input_file.close();

	// pin the reader to the retained build side, like the repeatability logging fixture does
	REQUIRE_NO_FAIL(con.Query("SET enable_caching_operators=true"));
	REQUIRE_NO_FAIL(con.Query("SET disabled_optimizers='join_order,build_side_probe_side'"));

	for (const auto &name : {"wrapped_reader_unknown", "wrapped_reader_false"}) {
		const string query = "WITH RECURSIVE r(i) AS ("
		                     "    VALUES (0)"
		                     "    UNION ALL"
		                     "    SELECT r.i + 1 FROM r"
		                     "    JOIN (SELECT k FROM " +
		                     string(name) + "('" + path +
		                     "')) build ON build.k = r.i + 1"
		                     "    WHERE r.i < 3"
		                     ") SELECT count(*) FROM r";
		wrapped_reader_executions = 0;
		auto result = con.Query(query);
		REQUIRE_NO_FAIL(*result);
		REQUIRE(CHECK_COLUMN(result, 0, {4}));
		// a reader the wrapper declared repeatable would be retained across the iterations and execute once -
		// without an affirmative callback of its own it must be re-executed every iteration
		REQUIRE(wrapped_reader_executions.load() >= 2);
	}
}
