#include "duckdb/function/scalar/generic_functions.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

using namespace std;

namespace duckdb {

struct StatsBindData : public FunctionData {
	StatsBindData(string stats = string()) : stats(move(stats)) {
	}

	string stats;

public:
	unique_ptr<FunctionData> Copy() override {
		return make_unique<StatsBindData>(stats);
	}
};

static void stats_function(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = (BoundFunctionExpression &)state.expr;
	auto &info = (StatsBindData &)*func_expr.bind_info;
	if (info.stats.empty()) {
		info.stats = "No statistics";
	}
	Value v(info.stats);
	result.Reference(v);
}

unique_ptr<FunctionData> stats_bind(ClientContext &context, ScalarFunction &bound_function,
                                               vector<unique_ptr<Expression>> &arguments) {
	return make_unique<StatsBindData>();
}

static unique_ptr<BaseStatistics> stats_propagate_stats(ClientContext &context, BoundFunctionExpression &expr,
                                                              FunctionData *bind_data,
                                                              vector<unique_ptr<BaseStatistics>> &child_stats) {
	if (child_stats[0]) {
		auto &info = (StatsBindData &) *bind_data;
		info.stats = child_stats[0]->ToString();
	}
	return nullptr;
}

void StatsFun::RegisterFunction(BuiltinFunctions &set) {
	set.AddFunction(ScalarFunction("stats", {LogicalType::ANY}, LogicalType::VARCHAR,
	                                     stats_function, true, stats_bind, nullptr, stats_propagate_stats));
}

} // namespace duckdb
