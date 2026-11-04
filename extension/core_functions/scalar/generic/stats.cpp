#include "core_functions/scalar/generic_functions.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

namespace duckdb {

namespace {
struct StatsBindData : public FunctionData {
	explicit StatsBindData(Value stats_p = Value(LogicalType::VARIANT())) : stats(std::move(stats_p)) {
	}

	Value stats;

public:
	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<StatsBindData>(stats);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<StatsBindData>();
		return Value::NotDistinctFrom(stats, other.stats);
	}
};

void StatsFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &info = func_expr.BindInfo()->Cast<StatsBindData>();
	result.Reference(info.stats, count_t(args.size()));
}

unique_ptr<FunctionData> StatsBind(BindScalarFunctionInput &input) {
	return make_uniq<StatsBindData>();
}

unique_ptr<BaseStatistics> StatsPropagateStats(ClientContext &context, FunctionStatisticsInput &input) {
	auto &child_stats = input.child_stats;
	auto &bind_data = input.bind_data;
	auto &info = bind_data->Cast<StatsBindData>();
	info.stats = child_stats[0].ToStruct().CastAs(context, LogicalType::VARIANT());
	return nullptr;
}

} // namespace

ScalarFunction StatsFun::GetFunction() {
	ScalarFunction stats({}, LogicalType::VARIANT(), StatsFunction, StatsBind, StatsPropagateStats);
	stats.GetSignature().AddParameter("expression", LogicalType::ANY);
	stats.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	stats.SetStability(FunctionStability::VOLATILE);
	return stats;
}

} // namespace duckdb
