#include "duckdb/function/aggregate/list_aggregate.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/storage/statistics/list_stats.hpp"
#include "duckdb/storage/statistics/node_statistics.hpp"
#include "core_functions/aggregate/nested_functions.hpp"

namespace duckdb {

namespace {

void ListFinalize(Vector &states_vector, AggregateFinalizeInputData &aggr_input_data, Vector &result, idx_t count,
                  idx_t offset) {
	auto states = states_vector.Values<ListAggState *>();

	D_ASSERT(result.GetType().id() == LogicalTypeId::LIST);

	ListSegmentFunctions functions;
	GetSegmentDataFunctions(functions, ListType::GetChildType(result.GetType()));

	vector<LinkedList> linked_lists;
	linked_lists.reserve(count);
	for (idx_t i = 0; i < count; i++) {
		linked_lists.push_back(states[i].GetValue()->linked_list);
	}
	functions.BuildLists(linked_lists, result, offset);
}

unique_ptr<BaseStatistics> ListStatsFunction(ClientContext &context, BoundAggregateExpression &expr,
                                             AggregateStatisticsInput &input) {
	if (expr.StateExportMode() == AggregateStateExportMode::STATE_EXPORT) {
		return nullptr;
	}
	auto stats = ListStats::CreateUnknown(expr.GetReturnType());
	// the elements are exactly the input values. The callback cannot see grouping-set
	// replication, so additive child statistics (e.g. string byte totals) are reset.
	if (!input.child_stats.empty() && input.child_stats[0].GetType() == ListType::GetChildType(expr.GetReturnType())) {
		auto child_stats = input.child_stats[0].ToUnique();
		child_stats->ResetAdditiveStatistics();
		ListStats::SetChildStats(stats, std::move(child_stats));
	}
	// a list aggregate folds every input row into one list element, and an empty group yields NULL
	auto length_data = ListStats::GetLengthData(stats);
	if (input.node_stats && input.node_stats->has_max_cardinality && input.node_stats->max_cardinality >= 1) {
		length_data.has_min_list_length = true;
		length_data.min_list_length = 1;
		length_data.has_max_list_length = true;
		length_data.max_list_length = input.node_stats->max_cardinality;
	}
	if (input.node_stats && input.node_stats->has_estimated_cardinality) {
		length_data.has_total_list_length = true;
		length_data.total_list_length = input.node_stats->estimated_cardinality;
	}
	ListStats::SetLengthData(stats, length_data);
	return stats.ToUnique();
}

} // namespace

AggregateFunction ListFun::GetFunction() {
	auto func = AggregateFunction(
	    {}, LogicalType::LIST(LogicalType::TEMPLATE("T")), AggregateFunction::StateSize<ListAggState>,
	    AggregateFunction::StateInitialize<ListAggState, ListFunction>, ListUpdateFunction<>,
	    ListCombineFunction<ListFunction>, ListFinalize, ListClusterUpdate<>, nullptr, nullptr, nullptr);
	func.GetSignature().AddParameter("arg", LogicalType::TEMPLATE("T"));
	AggregateFunction::WireStructStateType<ListAggState>(func);
	func.SetStatisticsCallback(ListStatsFunction);

	return func;
}

} // namespace duckdb
