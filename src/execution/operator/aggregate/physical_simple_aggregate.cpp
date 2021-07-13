#include "duckdb/execution/operator/aggregate/physical_simple_aggregate.hpp"
#include "duckdb/parallel/thread_context.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/catalog/catalog_entry/aggregate_function_catalog_entry.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {

PhysicalSimpleAggregate::PhysicalSimpleAggregate(vector<LogicalType> types, vector<unique_ptr<Expression>> expressions,
                                                 bool all_combinable, idx_t estimated_cardinality)
    : PhysicalSink(PhysicalOperatorType::SIMPLE_AGGREGATE, move(types), estimated_cardinality),
      aggregates(move(expressions)), all_combinable(all_combinable) {
}

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//

struct AggregateState {
	explicit AggregateState(vector<unique_ptr<Expression>> &aggregate_expressions) {
		for (auto &aggregate : aggregate_expressions) {
			D_ASSERT(aggregate->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE);
			auto &aggr = (BoundAggregateExpression &)*aggregate;
			auto state = unique_ptr<data_t[]>(new data_t[aggr.function.state_size()]);
			aggr.function.initialize(state.get());
			aggregates.push_back(move(state));
			destructors.push_back(aggr.function.destructor);
		}
	}
	~AggregateState() {
		D_ASSERT(destructors.size() == aggregates.size());
		for (idx_t i = 0; i < destructors.size(); i++) {
			if (!destructors[i]) {
				continue;
			}
			Vector state_vector(Value::POINTER((uintptr_t)aggregates[i].get()));
			state_vector.SetVectorType(VectorType::FLAT_VECTOR);

			destructors[i](state_vector, 1);
		}
	}

	void Move(AggregateState &other) {
		other.aggregates = move(aggregates);
		other.destructors = move(destructors);
	}

	//! The aggregate values
	vector<unique_ptr<data_t[]>> aggregates;
	// The destructors
	vector<aggregate_destructor_t> destructors;
};

class SimpleAggregateGlobalState : public GlobalOperatorState {
public:
	explicit SimpleAggregateGlobalState(vector<unique_ptr<Expression>> &aggregates) : state(aggregates) {
	}

	//! The lock for updating the global aggregate state
	mutex lock;
	//! The global aggregate state
	AggregateState state;
};

class SimpleAggregateLocalState : public LocalSinkState {
public:
	explicit SimpleAggregateLocalState(vector<unique_ptr<Expression>> &aggregates) : state(aggregates) {
		vector<LogicalType> payload_types;
		for (auto &aggregate : aggregates) {
			D_ASSERT(aggregate->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE);
			auto &aggr = (BoundAggregateExpression &)*aggregate;
			// initialize the payload chunk
			if (!aggr.children.empty()) {
				for (auto &child : aggr.children) {
					payload_types.push_back(child->return_type);
					child_executor.AddExpression(*child);
				}
			}
		}
		if (!payload_types.empty()) { // for select count(*) from t; there is no payload at all
			payload_chunk.Initialize(payload_types);
		}
	}
	void Reset() {
		payload_chunk.Reset();
	}

	//! The local aggregate state
	AggregateState state;
	//! The executor
	ExpressionExecutor child_executor;
	//! The payload chunk
	DataChunk payload_chunk;
};

unique_ptr<GlobalOperatorState> PhysicalSimpleAggregate::GetGlobalState(ClientContext &context) {
	return make_unique<SimpleAggregateGlobalState>(aggregates);
}

unique_ptr<LocalSinkState> PhysicalSimpleAggregate::GetLocalSinkState(ExecutionContext &context) {
	return make_unique<SimpleAggregateLocalState>(aggregates);
}

void PhysicalSimpleAggregate::Sink(ExecutionContext &context, GlobalOperatorState &state, LocalSinkState &lstate,
                                   DataChunk &input) const {
	auto &sink = (SimpleAggregateLocalState &)lstate;
	// perform the aggregation inside the local state
	idx_t payload_idx = 0, payload_expr_idx = 0;
	sink.Reset();

	DataChunk &payload_chunk = sink.payload_chunk;
	sink.child_executor.SetChunk(input);
	payload_chunk.SetCardinality(input);
	for (idx_t aggr_idx = 0; aggr_idx < aggregates.size(); aggr_idx++) {
		DataChunk filtered_input;
		auto &aggregate = (BoundAggregateExpression &)*aggregates[aggr_idx];
		idx_t payload_cnt = 0;
		// resolve the filter (if any)
		if (aggregate.filter) {
			ExpressionExecutor filter_execution(aggregate.filter.get());
			SelectionVector true_sel(STANDARD_VECTOR_SIZE);
			auto count = filter_execution.SelectExpression(input, true_sel);
			auto input_types = input.GetTypes();
			filtered_input.Initialize(input_types);
			filtered_input.Slice(input, true_sel, count);
			sink.child_executor.SetChunk(filtered_input);
			payload_chunk.SetCardinality(count);
		}
		// resolve the child expressions of the aggregate (if any)
		if (!aggregate.children.empty()) {
			for (idx_t i = 0; i < aggregate.children.size(); ++i) {
				sink.child_executor.ExecuteExpression(payload_expr_idx, payload_chunk.data[payload_idx + payload_cnt]);
				payload_expr_idx++;
				payload_cnt++;
			}
		}

		aggregate.function.simple_update(payload_cnt == 0 ? nullptr : &payload_chunk.data[payload_idx],
		                                 aggregate.bind_info.get(), payload_cnt, sink.state.aggregates[aggr_idx].get(),
		                                 payload_chunk.size());
		payload_idx += payload_cnt;
	}
}

//===--------------------------------------------------------------------===//
// Finalize
//===--------------------------------------------------------------------===//
void PhysicalSimpleAggregate::Combine(ExecutionContext &context, GlobalOperatorState &state, LocalSinkState &lstate) {
	auto &gstate = (SimpleAggregateGlobalState &)state;
	auto &source = (SimpleAggregateLocalState &)lstate;

	// finalize: combine the local state into the global state
	if (all_combinable) {
		// all aggregates are combinable: we might be doing a parallel aggregate
		// use the combine method to combine the partial aggregates
		lock_guard<mutex> glock(gstate.lock);
		for (idx_t aggr_idx = 0; aggr_idx < aggregates.size(); aggr_idx++) {
			auto &aggregate = (BoundAggregateExpression &)*aggregates[aggr_idx];
			Vector source_state(Value::POINTER((uintptr_t)source.state.aggregates[aggr_idx].get()));
			Vector dest_state(Value::POINTER((uintptr_t)gstate.state.aggregates[aggr_idx].get()));

			aggregate.function.combine(source_state, dest_state, 1);
		}
	} else {
		// complex aggregates: this is necessarily a non-parallel aggregate
		// simply move over the source state into the global state
		source.state.Move(gstate.state);
	}

	context.thread.profiler.Flush(this, &source.child_executor, "child_executor", 0);
	context.client.profiler->Flush(context.thread.profiler);
}

//===--------------------------------------------------------------------===//
// GetChunkInternal
//===--------------------------------------------------------------------===//
void PhysicalSimpleAggregate::GetChunkInternal(ExecutionContext &context, DataChunk &chunk,
                                               PhysicalOperatorState *state) const {
	auto &gstate = (SimpleAggregateGlobalState &)*sink_state;
	// initialize the result chunk with the aggregate values
	chunk.SetCardinality(1);
	for (idx_t aggr_idx = 0; aggr_idx < aggregates.size(); aggr_idx++) {
		auto &aggregate = (BoundAggregateExpression &)*aggregates[aggr_idx];

		Vector state_vector(Value::POINTER((uintptr_t)gstate.state.aggregates[aggr_idx].get()));
		aggregate.function.finalize(state_vector, aggregate.bind_info.get(), chunk.data[aggr_idx], 1);
	}
	state->finished = true;
}

string PhysicalSimpleAggregate::ParamsToString() const {
	string result;
	for (idx_t i = 0; i < aggregates.size(); i++) {
		auto &aggregate = (BoundAggregateExpression &)*aggregates[i];
		if (i > 0) {
			result += "\n";
		}
		result += aggregates[i]->GetName();
		if (aggregate.filter) {
			result += " Filter: " + aggregate.filter->GetName();
		}
	}
	return result;
}
void PhysicalSimpleAggregate::FinalizeOperatorState(PhysicalOperatorState &state, ExecutionContext &context) {
}

} // namespace duckdb
