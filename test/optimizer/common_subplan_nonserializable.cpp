#include "catch.hpp"
#include "duckdb.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

namespace {

struct NonSerializableGlobalState : public GlobalTableFunctionState {
	bool emitted = false;
	int32_t value = 42;
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
	result_data[0] = state.value;
	output.SetChildCardinality(1);
	state.emitted = true;
}

void RegisterNonSerializableTableFunction(DuckDB &db) {
	// stable under the default declaration - sharing is blocked by the missing serialization callbacks alone
	TableFunction table_function("nonserial_tf", {}, NonSerializableFunction, NonSerializableBind, NonSerializableInit);

	ExtensionInfo extension_info {};
	ExtensionActiveLoad load_info {*db.instance, extension_info, "test_extension", ""};
	ExtensionLoader loader {load_info};
	loader.RegisterFunction(table_function);
}

atomic<int32_t> non_repeatable_scan_count {0};

unique_ptr<GlobalTableFunctionState> NonRepeatableInit(ClientContext &, TableFunctionInitInput &) {
	auto result = make_uniq<NonSerializableGlobalState>();
	result->value = ++non_repeatable_scan_count;
	return std::move(result);
}

unique_ptr<FunctionData> RepeatabilityBind(ClientContext &, TableFunctionBindInput &, vector<LogicalType> &return_types,
                                           vector<Identifier> &names) {
	return_types.emplace_back(LogicalType::INTEGER);
	names.emplace_back("i");
	// Rebinding these parameterized calls is supported, independently of whether their scans are repeatable.
	return make_uniq<TableFunctionData>();
}

void RegisterRepeatabilityTableFunctions(DuckDB &db) {
	ExtensionInfo extension_info {};
	ExtensionActiveLoad load_info {*db.instance, extension_info, "test_extension", ""};
	ExtensionLoader loader {load_info};
	TableFunction unknown("repeatability_unknown", {LogicalType::INTEGER}, NonSerializableFunction, RepeatabilityBind,
	                      NonRepeatableInit);
	loader.RegisterFunction(unknown);
	TableFunction non_repeatable("repeatability_false", {LogicalType::INTEGER}, NonSerializableFunction,
	                             RepeatabilityBind, NonRepeatableInit);
	non_repeatable.stability = FunctionStability::VOLATILE;
	loader.RegisterFunction(non_repeatable);
	TableFunction repeatable("repeatability_true", {LogicalType::INTEGER}, NonSerializableFunction, RepeatabilityBind,
	                         NonSerializableInit);
	repeatable.stability = FunctionStability::CONSISTENT_WITHIN_QUERY;
	loader.RegisterFunction(repeatable);
}

atomic<int32_t> zero_argument_bind_count {0};

struct ZeroArgumentBindData : public TableFunctionData {
	// Statement caching stays supported: rebinding this call is fine, but each bind captures its own row.
	int32_t bound_value = 0;
};

unique_ptr<FunctionData> ZeroArgumentBind(ClientContext &, TableFunctionBindInput &, vector<LogicalType> &return_types,
                                          vector<Identifier> &names) {
	return_types.emplace_back(LogicalType::INTEGER);
	names.emplace_back("i");
	auto result = make_uniq<ZeroArgumentBindData>();
	result->bound_value = ++zero_argument_bind_count;
	return std::move(result);
}

unique_ptr<GlobalTableFunctionState> ZeroArgumentInit(ClientContext &, TableFunctionInitInput &input) {
	auto result = make_uniq<NonSerializableGlobalState>();
	result->value = input.bind_data->Cast<ZeroArgumentBindData>().bound_value;
	return std::move(result);
}

void RegisterZeroArgumentTableFunction(DuckDB &db) {
	ExtensionInfo extension_info {};
	ExtensionActiveLoad load_info {*db.instance, extension_info, "test_extension", ""};
	ExtensionLoader loader {load_info};
	// stable under the default declaration - sharing is blocked by the zero-positional-argument identity guard
	TableFunction zero_argument("repeatability_zero_arg", {}, NonSerializableFunction, ZeroArgumentBind,
	                            ZeroArgumentInit);
	loader.RegisterFunction(zero_argument);
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

// SQL cannot register the stability declaration or make an opaque scan return a different value
// deterministically on each initialization. Keep that API contract here, with builtin scan coverage in SQL.
TEST_CASE("Common subplans follow the declared table function stability", "[optimizer][common_subplan]") {
	DuckDB db(nullptr);
	Connection con(db);
	RegisterRepeatabilityTableFunctions(db);
	REQUIRE_NO_FAIL(con.Query("SET explain_output='optimized_only'"));

	// an explicitly volatile scan is never shared
	const string volatile_query = "SELECT count(*) FROM (SELECT i FROM repeatability_false(7) "
	                              "INTERSECT ALL SELECT i FROM repeatability_false(7))";
	non_repeatable_scan_count = 0;
	auto result = con.Query(volatile_query);
	REQUIRE_NO_FAIL(*result);
	REQUIRE(CHECK_COLUMN(result, 0, {0}));
	REQUIRE(non_repeatable_scan_count.load() == 2);
	REQUIRE(!StringUtil::Contains(GetExplainPlan(con, volatile_query), "__common_subplan_"));

	// contract decision from the PR review: an undeclared scan is stable within the query and shares like an
	// explicitly declared one. This scan violates the default contract on purpose - a scan whose rows change
	// per initialization must declare VOLATILE
	const string unknown_query = "SELECT count(*) FROM (SELECT i FROM repeatability_unknown(7) "
	                             "INTERSECT ALL SELECT i FROM repeatability_unknown(7))";
	non_repeatable_scan_count = 0;
	result = con.Query(unknown_query);
	REQUIRE_NO_FAIL(*result);
	REQUIRE(CHECK_COLUMN(result, 0, {1}));
	REQUIRE(non_repeatable_scan_count.load() == 1);
	REQUIRE(StringUtil::Contains(GetExplainPlan(con, unknown_query), "__common_subplan_"));

	// Positional parameters identify this stable scan without serializing its bind data.
	const string repeatable_query = "SELECT count(*) FROM (SELECT i FROM repeatability_true(7) "
	                                "INTERSECT ALL SELECT i FROM repeatability_true(7))";
	result = con.Query(repeatable_query);
	REQUIRE_NO_FAIL(*result);
	REQUIRE(CHECK_COLUMN(result, 0, {1}));
	REQUIRE(StringUtil::Contains(GetExplainPlan(con, repeatable_query), "__common_subplan_"));
}

// SQL cannot register a statement-cache-supporting, default-stable scan whose separate binds capture
// different fixed rows. Keep the zero-positional-argument identity guard contract here.
TEST_CASE("Common subplans require positional parameters to identify repeatable scans", "[optimizer][common_subplan]") {
	DuckDB db(nullptr);
	Connection con(db);
	RegisterZeroArgumentTableFunction(db);
	REQUIRE_NO_FAIL(con.Query("SET explain_output='optimized_only'"));

	// Without serialization callbacks or positional parameters, equal serialized bytes cannot prove that
	// two binds captured the same rows, so the identity guard must keep sharing disabled.
	const string query = "SELECT count(*) FROM (SELECT i FROM repeatability_zero_arg() "
	                     "INTERSECT ALL SELECT i FROM repeatability_zero_arg())";
	zero_argument_bind_count = 0;
	auto result = con.Query(query);
	REQUIRE_NO_FAIL(*result);
	REQUIRE(CHECK_COLUMN(result, 0, {0}));
	REQUIRE(zero_argument_bind_count.load() >= 2);
	REQUIRE(!StringUtil::Contains(GetExplainPlan(con, query), "__common_subplan_"));
}
