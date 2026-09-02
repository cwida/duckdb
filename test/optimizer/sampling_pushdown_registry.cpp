#include "catch.hpp"
#include "duckdb.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/optimizer/sampling_pushdown_registry.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

namespace {

struct SampleHookBindData : public TableFunctionData {
	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<SampleHookBindData>();
	}

	bool Equals(const FunctionData &other) const override {
		return true;
	}
};

struct SampleHookGlobalState : public GlobalTableFunctionState {
	idx_t offset = 0;
};

unique_ptr<FunctionData> SampleHookBind(ClientContext &, TableFunctionBindInput &, vector<LogicalType> &return_types,
                                        vector<Identifier> &names) {
	return_types.emplace_back(LogicalType::INTEGER);
	names.emplace_back("i");
	return make_uniq<SampleHookBindData>();
}

unique_ptr<GlobalTableFunctionState> SampleHookInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<SampleHookGlobalState>();
}

void SampleHookFunction(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<SampleHookGlobalState>();
	if (state.offset >= 100) {
		output.SetChildCardinality(0);
		return;
	}
	auto result_data = FlatVector::GetDataMutable<int32_t>(output.data[0]);
	idx_t count = 0;
	while (state.offset < 100 && count < STANDARD_VECTOR_SIZE) {
		result_data[count++] = NumericCast<int32_t>(state.offset++);
	}
	output.SetChildCardinality(count);
}

void RegisterSampleHookTableFunction(DuckDB &db) {
	TableFunction table_function("sample_hook_scan", {}, SampleHookFunction, SampleHookBind, SampleHookInit);
	table_function.sampling_pushdown = false;

	ExtensionInfo extension_info {};
	ExtensionActiveLoad load_info {*db.instance, extension_info, "test_extension", ""};
	ExtensionLoader loader {load_info};
	loader.RegisterFunction(table_function);
}

string GetSampleHookExplainPlan(Connection &con, const string &query) {
	auto result = con.Query("EXPLAIN " + query);
	REQUIRE_NO_FAIL(*result);

	string explain;
	for (idx_t row = 0; row < result->RowCount(); row++) {
		for (idx_t col = 0; col < result->ColumnCount(); col++) {
			explain += result->Collection().GetValue(col, row).ToString();
			explain += "\n";
		}
	}
	return explain;
}

bool AlwaysSupportsSampling(const FunctionData &, const SampleOptions &) {
	return true;
}

} // namespace

TEST_CASE("Sampling pushdown registry opts a table function into system row sampling", "[optimizer][sampling]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET profiling_renderer_settings = MAP {'operator_casing': 'upper'}"));
	RegisterSampleHookTableFunction(db);

	const string query = "SELECT * FROM sample_hook_scan() USING SAMPLE 10 ROWS (system)";
	auto before = GetSampleHookExplainPlan(con, query);
	REQUIRE(StringUtil::Contains(before, "STREAMING_SAMPLE"));

	SamplingPushdownRegistry::Register(*db.instance, "sample_hook_scan", AlwaysSupportsSampling);

	auto after = GetSampleHookExplainPlan(con, query);
	REQUIRE(!StringUtil::Contains(after, "STREAMING_SAMPLE"));
	REQUIRE(StringUtil::Contains(after, "LIMIT"));
}
