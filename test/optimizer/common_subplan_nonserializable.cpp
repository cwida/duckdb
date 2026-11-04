#include "catch.hpp"
#include "duckdb.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

namespace {

struct NonSerializableGlobalState : public GlobalTableFunctionState {
	bool emitted = false;
};

struct NonSerializableBindData : public TableFunctionData {
	unique_ptr<FunctionData> Copy() const override {
		throw InternalException("Copy not supported for NonSerializableBindData");
	}

	bool Equals(const FunctionData &other) const override {
		return false;
	}

	bool SupportStatementCache() const override {
		return false;
	}
};

unique_ptr<FunctionData> NonSerializableBind(ClientContext &, TableFunctionBindInput &,
                                             vector<LogicalType> &return_types, vector<Identifier> &names) {
	return_types.emplace_back(LogicalType::INTEGER);
	names.emplace_back("i");
	return make_uniq<NonSerializableBindData>();
}

unique_ptr<GlobalTableFunctionState> NonSerializableInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<NonSerializableGlobalState>();
}

void NonSerializableFunction(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<NonSerializableGlobalState>();
	if (state.emitted) {
		output.SetChildCardinality(0);
		return;
	}
	auto result_data = FlatVector::GetDataMutable<int32_t>(output.data[0]);
	result_data[0] = 42;
	output.SetChildCardinality(1);
	state.emitted = true;
}

void RegisterNonSerializableTableFunction(DuckDB &db) {
	const TableFunction table_function("nonserial_tf", {}, NonSerializableFunction, NonSerializableBind,
	                                   NonSerializableInit);

	ExtensionInfo extension_info {};
	ExtensionActiveLoad load_info {*db.instance, extension_info, "test_extension", ""};
	ExtensionLoader loader {load_info};
	loader.RegisterFunction(table_function);
}

string GetExplainPlan(Connection &con, const string &query) {
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

} // namespace

TEST_CASE("Common subplan skips table functions without serialization callbacks", "[optimizer][common_subplan]") {
	DuckDB db(nullptr);
	Connection con(db);
	RegisterNonSerializableTableFunction(db);

	const string query = "SELECT lhs.c, rhs.c "
	                     "FROM (SELECT count(*) AS c FROM nonserial_tf()) lhs "
	                     "CROSS JOIN (SELECT count(*) AS c FROM nonserial_tf()) rhs";

	auto result = con.Query(query);
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->RowCount() == 1);
	REQUIRE(result->Collection().GetValue(0, 0).GetValue<int64_t>() == 1);
	REQUIRE(result->Collection().GetValue(1, 0).GetValue<int64_t>() == 1);

	REQUIRE_NO_FAIL(con.Query("PRAGMA explain_output='optimized_only'"));
	const auto explain = GetExplainPlan(con, query);
	REQUIRE(!StringUtil::Contains(explain, "__common_subplan_"));
}
