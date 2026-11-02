#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/aggregate_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/parsed_data/create_aggregate_function_info.hpp"
#include "duckdb/planner/expression.hpp"

using namespace duckdb;

// A custom aggregate whose per-group initial value comes from bind data. offset_sum(v, k) returns
// k + sum(v) for each group. The offset k is a constant argument, folded at bind time and stored in
// bind data. State init reads it back to seed every group's state, and state size dereferences the bind
// data. This exercises that the state size and state init callbacks receive the bind data the bind
// callback produced. A call site that threaded the wrong or no bind data would seed a wrong offset (a
// wrong result) or hit the null check in size (a query error), so the wiring is checked, not assumed.

namespace {

struct OffsetSumBindData : public FunctionData {
	explicit OffsetSumBindData(int64_t offset_p) : offset(offset_p) {
	}

	int64_t offset;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<OffsetSumBindData>(offset);
	}
	bool Equals(const FunctionData &other_p) const override {
		return offset == other_p.Cast<OffsetSumBindData>().offset;
	}
};

struct OffsetSumState {
	int64_t sum;
	int64_t offset_seen;
};

unique_ptr<FunctionData> OffsetSumBind(BindAggregateFunctionInput &input) {
	auto &arguments = input.GetArguments();
	D_ASSERT(arguments.size() == 2);
	if (!arguments[1]->IsFoldable()) {
		throw BinderException("offset_sum: the offset argument must be constant");
	}
	Value offset_val = ExpressionExecutor::EvaluateScalar(input.GetClientContext(), *arguments[1]);
	return make_uniq<OffsetSumBindData>(offset_val.GetValue<int64_t>());
}

// Reads the bind data: a call site that threads no bind data trips this in every build.
idx_t OffsetSumSize(AggregateStateInput &input) {
	if (!input.bind_data) {
		throw InternalException("offset_sum: state size callback received no bind data");
	}
	return sizeof(OffsetSumState);
}

// Batched init: seeds each state's offset from the bind data.
void OffsetSumInit(AggregateStateInput &input, data_ptr_t *states, idx_t count) {
	auto &bind = input.bind_data->Cast<OffsetSumBindData>();
	for (idx_t i = 0; i < count; i++) {
		auto &state = *reinterpret_cast<OffsetSumState *>(states[i]);
		state.sum = 0;
		state.offset_seen = bind.offset;
	}
}

void OffsetSumUpdate(Vector inputs[], AggregateInputData &, idx_t input_count, Vector &state_vector, idx_t count) {
	D_ASSERT(input_count == 2);
	UnifiedVectorFormat sdata;
	state_vector.ToUnifiedFormat(sdata);
	auto states = UnifiedVectorFormat::GetData<OffsetSumState *>(sdata);

	UnifiedVectorFormat vdata;
	inputs[0].ToUnifiedFormat(vdata);
	auto vals = UnifiedVectorFormat::GetData<int64_t>(vdata);
	for (idx_t i = 0; i < count; i++) {
		auto vidx = vdata.sel->get_index(i);
		if (!vdata.validity.RowIsValid(vidx)) {
			continue;
		}
		auto &state = *states[sdata.sel->get_index(i)];
		state.sum += vals[vidx];
	}
}

void OffsetSumCombine(Vector &source, Vector &target, AggregateInputData &, idx_t count) {
	UnifiedVectorFormat sdata;
	source.ToUnifiedFormat(sdata);
	auto sources = UnifiedVectorFormat::GetData<OffsetSumState *>(sdata);
	auto targets = FlatVector::GetDataMutable<OffsetSumState *>(target);
	for (idx_t i = 0; i < count; i++) {
		auto &s = *sources[sdata.sel->get_index(i)];
		auto &t = *targets[i];
		// offset_seen is identical across partials (same bind data); only the running sum merges
		t.sum += s.sum;
	}
}

void OffsetSumFinalize(Vector &state_vector, AggregateFinalizeInputData &, Vector &result, idx_t count, idx_t offset) {
	UnifiedVectorFormat sdata;
	state_vector.ToUnifiedFormat(sdata);
	auto states = UnifiedVectorFormat::GetData<OffsetSumState *>(sdata);
	auto res = FlatVector::GetDataMutable<int64_t>(result);
	for (idx_t i = 0; i < count; i++) {
		auto &state = *states[sdata.sel->get_index(i)];
		res[offset + i] = state.sum + state.offset_seen;
	}
}

void RegisterOffsetSum(Connection &con) {
	AggregateFunction fn("offset_sum", {LogicalType::BIGINT, LogicalType::BIGINT}, LogicalType::BIGINT, OffsetSumSize,
	                     OffsetSumInit, OffsetSumUpdate, OffsetSumCombine, OffsetSumFinalize,
	                     FunctionNullHandling::DEFAULT_NULL_HANDLING, nullptr, OffsetSumBind);
	CreateAggregateFunctionInfo info(fn);
	con.context->RunFunctionInTransaction(
	    [&]() { Catalog::GetSystemCatalog(*con.context).CreateFunction(*con.context, info); });
}

void RegisterOffsetSumWithoutCombine(Connection &con) {
	AggregateFunction fn("offset_sum_no_combine", {LogicalType::BIGINT, LogicalType::BIGINT}, LogicalType::BIGINT,
	                     OffsetSumSize, OffsetSumInit, OffsetSumUpdate, nullptr, OffsetSumFinalize,
	                     FunctionNullHandling::DEFAULT_NULL_HANDLING, nullptr, OffsetSumBind);
	fn.SetOrderDependent(AggregateOrderDependent::NOT_ORDER_DEPENDENT);
	CreateAggregateFunctionInfo info(fn);
	con.context->RunFunctionInTransaction(
	    [&]() { Catalog::GetSystemCatalog(*con.context).CreateFunction(*con.context, info); });
}

} // namespace

TEST_CASE("Aggregate state size and init callbacks receive bind data", "[api][aggregate_function]") {
	DuckDB db(nullptr);
	Connection con(db);
	RegisterOffsetSum(con);

	// Ungrouped aggregation: the single state is seeded from the bound offset via init.
	{
		auto result = con.Query("SELECT offset_sum(v, 100) FROM (VALUES (1),(2),(3)) t(v)");
		REQUIRE_NO_FAIL(*result);
		REQUIRE(result->Collection().GetValue(0, 0) == Value::BIGINT(106));
	}

	// Grouped aggregation: every group's state must be seeded with the same bound offset.
	{
		auto result = con.Query("SELECT k, offset_sum(v, 10) FROM (VALUES ('a', 1), ('a', 2), ('b', 5)) t(k, v) "
		                        "GROUP BY k ORDER BY k");
		REQUIRE_NO_FAIL(*result);
		REQUIRE(result->Collection().GetValue(1, 0) == Value::BIGINT(13)); // a: 10 + (1 + 2)
		REQUIRE(result->Collection().GetValue(1, 1) == Value::BIGINT(15)); // b: 10 + 5
	}

	// Windowed aggregation exercises the window state size and init call sites.
	{
		auto result = con.Query("SELECT offset_sum(v, 1000) OVER () FROM (VALUES (1),(2),(3)) t(v)");
		REQUIRE_NO_FAIL(*result);
		REQUIRE(result->Collection().GetValue(0, 0) == Value::BIGINT(1006));
	}

	// A different offset must produce a different result: proves init reads the value, not a constant.
	{
		auto result = con.Query("SELECT offset_sum(v, 0) FROM (VALUES (1),(2),(3)) t(v)");
		REQUIRE_NO_FAIL(*result);
		REQUIRE(result->Collection().GetValue(0, 0) == Value::BIGINT(6));
	}

	// NULL values are skipped, but the offset still seeds the state.
	{
		auto result = con.Query("SELECT offset_sum(v, 7) FROM (VALUES (1), (NULL), (2)) t(v)");
		REQUIRE_NO_FAIL(*result);
		REQUIRE(result->Collection().GetValue(0, 0) == Value::BIGINT(10)); // 7 + (1 + 2)
	}
}

TEST_CASE("USING KEY UNION falls back for an aggregate without combine", "[api][aggregate_function][recursive_cte]") {
	DuckDB db(nullptr);
	Connection con(db);
	RegisterOffsetSumWithoutCombine(con);

	auto result = con.Query("WITH RECURSIVE t(k, v) USING KEY (k, offset_sum_no_combine(v, 0) AS v) AS ("
	                        "VALUES (1::BIGINT, 1::BIGINT) UNION SELECT k, v + 1 FROM t WHERE v < 4) TABLE t");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->RowCount() == 1);
	REQUIRE(result->Collection().GetValue(0, 0) == Value::BIGINT(1));
	REQUIRE(result->Collection().GetValue(1, 0) == Value::BIGINT(7));
}
