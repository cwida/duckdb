#include "duckdb/execution/operator/join/physical_right_delim_join.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"

#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/execution/operator/aggregate/physical_hash_aggregate.hpp"
#include "duckdb/execution/operator/join/physical_join.hpp"
#include "duckdb/execution/operator/scan/physical_dummy_scan.hpp"
#include "duckdb/parallel/meta_pipeline.hpp"
#include "duckdb/parallel/pipeline.hpp"
#include "duckdb/parallel/thread_context.hpp"

namespace duckdb {

static void RightDelimResetSinkState(const PhysicalOperator &op, ClientContext &context,
                                     unique_ptr<GlobalSinkState> &state) {
	if (state && state->SupportsReuse()) {
		state->Reset(context);
		return;
	}
	state = op.GetGlobalSinkState(context);
}

static void RightDelimResetLocalSinkState(const PhysicalOperator &op, ExecutionContext &context,
                                          GlobalSinkState &gstate, unique_ptr<LocalSinkState> &state) {
	if (state && state->SupportsReuse()) {
		state->Reset(context, gstate);
		return;
	}
	state = op.GetLocalSinkState(context);
}

PhysicalRightDelimJoin::PhysicalRightDelimJoin(PhysicalPlan &physical_plan, PhysicalPlanGenerator &planner,
                                               vector<LogicalType> types, PhysicalOperator &original_join,
                                               PhysicalOperator &distinct,
                                               const vector<const_reference<PhysicalOperator>> &delim_scans,
                                               idx_t estimated_cardinality, optional_idx delim_idx)
    : PhysicalDelimJoin(physical_plan, PhysicalOperatorType::RIGHT_DELIM_JOIN, std::move(types), original_join,
                        distinct, delim_scans, estimated_cardinality, delim_idx) {
	D_ASSERT(join.children.size() == 2);
	// now for the original join
	// we take its right child, this is the side that we will duplicate eliminate
	children.push_back(join.children[1]);

	// we replace it with a PhysicalDummyScan, which contains no data, just the types, it won't be scanned anyway
	join.children[1] = planner.Make<PhysicalDummyScan>(children[0].get().GetTypes(), estimated_cardinality);
}

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//
class RightDelimJoinGlobalState : public GlobalSinkState {
public:
	explicit RightDelimJoinGlobalState(const PhysicalRightDelimJoin &op) : op(op) {
	}

	const PhysicalRightDelimJoin &op;

	bool SupportsReuse() const override {
		return true;
	}

	void Reset(ClientContext &context) override {
		// forward the worker bound so the internal distinct aggregate sizes its restart from it
		if (op.join.sink_state) {
			op.join.sink_state->reschedule_worker_limit = reschedule_worker_limit;
			op.join.sink_state->reschedule_single_producer = reschedule_single_producer;
		}
		if (op.distinct.sink_state) {
			op.distinct.sink_state->reschedule_worker_limit = reschedule_worker_limit;
			op.distinct.sink_state->reschedule_single_producer = reschedule_single_producer;
		}
		RightDelimResetSinkState(op.join, context, op.join.sink_state);
		RightDelimResetSinkState(op.distinct, context, op.distinct.sink_state);
		if (op.delim_scans.size() > 1) {
			PhysicalHashAggregate::SetMultiScan(*op.distinct.sink_state);
		}
		GlobalSinkState::Reset(context);
	}

	void AdjustRescheduledWorkers(ClientContext &context, idx_t scheduled_workers) override {
		if (op.distinct.sink_state) {
			op.distinct.sink_state->AdjustRescheduledWorkers(context, scheduled_workers);
		}
	}
};

class RightDelimJoinLocalState : public LocalSinkState {
public:
	explicit RightDelimJoinLocalState(const PhysicalRightDelimJoin &op) : op(op) {
	}

	const PhysicalRightDelimJoin &op;
	unique_ptr<LocalSinkState> join_state;
	unique_ptr<LocalSinkState> distinct_state;

	bool SupportsReuse() const override {
		return true;
	}

	void Reset(ExecutionContext &context, GlobalSinkState &gstate) override {
		RightDelimResetLocalSinkState(op.join, context, *op.join.sink_state, join_state);
		RightDelimResetLocalSinkState(op.distinct, context, *op.distinct.sink_state, distinct_state);
	}
};

unique_ptr<GlobalSinkState> PhysicalRightDelimJoin::GetGlobalSinkState(ClientContext &context) const {
	auto state = make_uniq<RightDelimJoinGlobalState>(*this);
	join.sink_state = join.GetGlobalSinkState(context);
	distinct.sink_state = distinct.GetGlobalSinkState(context);
	if (delim_scans.size() > 1) {
		PhysicalHashAggregate::SetMultiScan(*distinct.sink_state);
	}
	return std::move(state);
}

unique_ptr<LocalSinkState> PhysicalRightDelimJoin::GetLocalSinkState(ExecutionContext &context) const {
	auto state = make_uniq<RightDelimJoinLocalState>(*this);
	state->join_state = join.GetLocalSinkState(context);
	state->distinct_state = distinct.GetLocalSinkState(context);
	return std::move(state);
}

SinkResultType PhysicalRightDelimJoin::Sink(ExecutionContext &context, DataChunk &chunk,
                                            OperatorSinkInput &input) const {
	auto &lstate = input.local_state.Cast<RightDelimJoinLocalState>();

	OperatorSinkInput join_sink_input {*join.sink_state, *lstate.join_state, input.interrupt_state};
	join.Sink(context, chunk, join_sink_input);

	OperatorSinkInput distinct_sink_input {*distinct.sink_state, *lstate.distinct_state, input.interrupt_state};
	distinct.Sink(context, chunk, distinct_sink_input);

	return SinkResultType::NEED_MORE_INPUT;
}

SinkCombineResultType PhysicalRightDelimJoin::Combine(ExecutionContext &context,
                                                      OperatorSinkCombineInput &input) const {
	auto &lstate = input.local_state.Cast<RightDelimJoinLocalState>();

	OperatorSinkCombineInput join_combine_input {*join.sink_state, *lstate.join_state, input.interrupt_state};
	join.Combine(context, join_combine_input);

	OperatorSinkCombineInput distinct_combine_input {*distinct.sink_state, *lstate.distinct_state,
	                                                 input.interrupt_state};
	distinct.Combine(context, distinct_combine_input);

	return SinkCombineResultType::FINISHED;
}

void PhysicalRightDelimJoin::PrepareFinalize(ClientContext &context, GlobalSinkState &sink_state) const {
	join.PrepareFinalize(context, *join.sink_state);
	distinct.PrepareFinalize(context, *distinct.sink_state);
}

SinkFinalizeType PhysicalRightDelimJoin::Finalize(Pipeline &pipeline, Event &event, ClientContext &client,
                                                  OperatorSinkFinalizeInput &input) const {
	OperatorSinkFinalizeInput join_finalize_input {*join.sink_state, input.interrupt_state};
	join.Finalize(pipeline, event, client, join_finalize_input);

	OperatorSinkFinalizeInput distinct_finalize_input {*distinct.sink_state, input.interrupt_state};
	distinct.Finalize(pipeline, event, client, distinct_finalize_input);

	return SinkFinalizeType::READY;
}

//===--------------------------------------------------------------------===//
// Pipeline Construction
//===--------------------------------------------------------------------===//
void PhysicalRightDelimJoin::BuildPipelines(Pipeline &current, MetaPipeline &meta_pipeline) {
	op_state.reset();
	sink_state.reset();

	auto &child_meta_pipeline = meta_pipeline.CreateChildMetaPipeline(current, *this);
	child_meta_pipeline.Build(children[0]);

	D_ASSERT(type == PhysicalOperatorType::RIGHT_DELIM_JOIN);
	// recurse into the actual join
	// any pipelines in there depend on the main pipeline
	// any scan of the duplicate eliminated data on the LHS depends on this pipeline
	// we add an entry to the mapping of (PhysicalOperator*) -> (Pipeline*)
	auto &state = meta_pipeline.GetState();
	for (auto &delim_scan : delim_scans) {
		state.delim_join_dependencies.insert(
		    make_pair(delim_scan, reference<Pipeline>(*child_meta_pipeline.GetBasePipeline())));
	}

	// Build join pipelines without building the RHS (already built in the Sink of this op)
	PhysicalJoin::BuildJoinPipelines(current, meta_pipeline, join, false);
}

} // namespace duckdb
