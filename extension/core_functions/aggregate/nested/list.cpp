#include "duckdb/function/aggregate/list_aggregate.hpp"
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

} // namespace

AggregateFunction ListFun::GetFunction() {
	auto func = AggregateFunction(
	    {}, LogicalType::LIST(LogicalType::TEMPLATE("T")), AggregateFunction::StateSize<ListAggState>,
	    AggregateFunction::StateInitialize<ListAggState, ListFunction>, ListUpdateFunction<>,
	    ListCombineFunction<ListFunction>, ListFinalize, ListClusterUpdate<>, nullptr, nullptr, nullptr);
	func.GetSignature().AddParameter("arg", LogicalType::TEMPLATE("T"));
	AggregateFunction::WireStructStateType<ListAggState>(func);

	return func;
}

} // namespace duckdb
