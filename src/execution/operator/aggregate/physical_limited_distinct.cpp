#include "duckdb/execution/operator/aggregate/physical_limited_distinct.hpp"

#include "duckdb/execution/aggregate_hashtable.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/execution/operator/aggregate/aggregate_object.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"

namespace duckdb {

static vector<LogicalType> GetPayloadTypes(const vector<unique_ptr<Expression>> &aggregates) {
	vector<LogicalType> payload_types;
	for (auto &aggr_expr : aggregates) {
		auto &aggr = aggr_expr->Cast<BoundAggregateExpression>();
		for (auto &child : aggr.GetChildren()) {
			payload_types.push_back(child->GetReturnType());
		}
	}
	return payload_types;
}

static vector<AggregateObject> GetAggregateObjects(const vector<unique_ptr<Expression>> &aggregates) {
	vector<BoundAggregateExpression *> aggregate_ptrs;
	for (auto &aggr : aggregates) {
		aggregate_ptrs.push_back(&aggr->Cast<BoundAggregateExpression>());
	}
	return AggregateObject::CreateAggregateObjects(aggregate_ptrs);
}

PhysicalLimitedDistinct::PhysicalLimitedDistinct(PhysicalPlan &physical_plan, vector<LogicalType> types,
                                                 vector<unique_ptr<Expression>> groups_p,
                                                 vector<unique_ptr<Expression>> aggregates_p, idx_t limit_p,
                                                 idx_t estimated_cardinality)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::LIMITED_DISTINCT, std::move(types), estimated_cardinality),
      groups(std::move(groups_p)), aggregates(std::move(aggregates_p)), limit(limit_p) {
	for (auto &group : groups) {
		group_types.push_back(group->GetReturnType());
	}
}

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//
struct LimitedDistinctGlobalSinkState : public GlobalSinkState {
	LimitedDistinctGlobalSinkState(const PhysicalLimitedDistinct &op, ClientContext &context) : limit_reached(false) {
		auto aggregate_objects = GetAggregateObjects(op.aggregates);
		auto payload_types = GetPayloadTypes(op.aggregates);
		ht = make_uniq<GroupedAggregateHashTable>(context, BufferAllocator::Get(context), op.group_types, payload_types,
		                                          aggregate_objects);
	}

	unique_ptr<GroupedAggregateHashTable> ht;
	atomic<bool> limit_reached;
	mutex combine_lock;
};

struct LimitedDistinctLocalSinkState : public LocalSinkState {
	LimitedDistinctLocalSinkState(const PhysicalLimitedDistinct &op, ExecutionContext &context)
	    : group_executor(context.client, op.groups), payload_executor(context.client) {
		auto &allocator = BufferAllocator::Get(context.client);

		auto aggregate_objects = GetAggregateObjects(op.aggregates);
		auto payload_types = GetPayloadTypes(op.aggregates);

		ht = make_uniq<GroupedAggregateHashTable>(context.client, allocator, op.group_types, payload_types,
		                                          aggregate_objects);

		group_chunk.Initialize(allocator, op.group_types);

		// Build payload executor for aggregate children
		for (auto &aggr_expr : op.aggregates) {
			auto &aggr = aggr_expr->Cast<BoundAggregateExpression>();
			for (auto &child : aggr.GetChildren()) {
				payload_executor.AddExpression(*child);
			}
		}

		if (!payload_types.empty()) {
			payload_chunk.Initialize(allocator, payload_types);
		}
	}

	unique_ptr<GroupedAggregateHashTable> ht;
	ExpressionExecutor group_executor;
	ExpressionExecutor payload_executor;
	DataChunk group_chunk;
	DataChunk payload_chunk;
};

unique_ptr<GlobalSinkState> PhysicalLimitedDistinct::GetGlobalSinkState(ClientContext &context) const {
	return make_uniq<LimitedDistinctGlobalSinkState>(*this, context);
}

unique_ptr<LocalSinkState> PhysicalLimitedDistinct::GetLocalSinkState(ExecutionContext &context) const {
	return make_uniq<LimitedDistinctLocalSinkState>(*this, context);
}

SinkResultType PhysicalLimitedDistinct::Sink(ExecutionContext &context, DataChunk &chunk,
                                             OperatorSinkInput &input) const {
	auto &gstate = input.global_state.Cast<LimitedDistinctGlobalSinkState>();
	auto &lstate = input.local_state.Cast<LimitedDistinctLocalSinkState>();

	if (limit == 0) {
		return SinkResultType::FINISHED;
	}

	if (gstate.limit_reached) {
		return SinkResultType::FINISHED;
	}

	// Execute group expressions
	lstate.group_chunk.Reset();
	lstate.group_executor.Execute(chunk, lstate.group_chunk);

	// Execute payload expressions (aggregate children)
	if (lstate.payload_chunk.ColumnCount() > 0) {
		lstate.payload_chunk.Reset();
		lstate.payload_executor.Execute(chunk, lstate.payload_chunk);
		unsafe_vector<idx_t> filter;
		lstate.ht->AddChunk(lstate.group_chunk, lstate.payload_chunk, filter);
	} else {
		// No aggregates - just track groups
		DataChunk empty_payload;
		unsafe_vector<idx_t> filter;
		lstate.ht->AddChunk(lstate.group_chunk, empty_payload, filter);
	}

	// Check if we have enough groups for early termination
	if (limit > 0 && lstate.ht->Count() >= limit) {
		gstate.limit_reached = true;
		return SinkResultType::FINISHED;
	}

	return SinkResultType::NEED_MORE_INPUT;
}

SinkCombineResultType PhysicalLimitedDistinct::Combine(ExecutionContext &context,
                                                       OperatorSinkCombineInput &input) const {
	auto &gstate = input.global_state.Cast<LimitedDistinctGlobalSinkState>();
	auto &lstate = input.local_state.Cast<LimitedDistinctLocalSinkState>();

	if (lstate.ht->Count() > 0) {
		lock_guard<mutex> guard(gstate.combine_lock);
		gstate.ht->Combine(*lstate.ht);
	}
	return SinkCombineResultType::FINISHED;
}

SinkFinalizeType PhysicalLimitedDistinct::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                                                   OperatorSinkFinalizeInput &input) const {
	return SinkFinalizeType::READY;
}

//===--------------------------------------------------------------------===//
// Source
//===--------------------------------------------------------------------===//
struct LimitedDistinctGlobalSourceState : public GlobalSourceState {
	LimitedDistinctGlobalSourceState(const PhysicalLimitedDistinct &op, ClientContext &context)
	    : initialized(false), total_rows(op.sink_state->Cast<LimitedDistinctGlobalSinkState>().ht->Count()),
	      rows_scanned(0) {
		group_chunk.Initialize(Allocator::Get(context), op.group_types);

		vector<LogicalType> payload_types;
		for (auto &aggr_expr : op.aggregates) {
			auto &aggr = aggr_expr->Cast<BoundAggregateExpression>();
			payload_types.push_back(aggr.GetReturnType());
		}
		if (!payload_types.empty()) {
			payload_chunk.Initialize(Allocator::Get(context), payload_types);
		}
	}

	AggregateHTScanState scan_state;
	bool initialized;
	const idx_t total_rows;
	atomic<idx_t> rows_scanned;
	DataChunk group_chunk;
	DataChunk payload_chunk;
};

unique_ptr<GlobalSourceState> PhysicalLimitedDistinct::GetGlobalSourceState(ClientContext &context) const {
	return make_uniq<LimitedDistinctGlobalSourceState>(*this, context);
}

ProgressData PhysicalLimitedDistinct::GetProgress(ClientContext &context, GlobalSourceState &gstate) const {
	auto &state = gstate.Cast<LimitedDistinctGlobalSourceState>();
	ProgressData progress;
	progress.total = double(MaxValue<idx_t>(state.total_rows, 1));
	progress.done = state.total_rows == 0 ? 1.0 : double(state.rows_scanned.load(std::memory_order_relaxed));
	return progress;
}

void PhysicalLimitedDistinct::SourceFinished(ClientContext &context, GlobalSourceState &gstate) const {
	auto &state = gstate.Cast<LimitedDistinctGlobalSourceState>();
	state.rows_scanned.store(state.total_rows, std::memory_order_relaxed);
}

unique_ptr<LocalSourceState> PhysicalLimitedDistinct::GetLocalSourceState(ExecutionContext &context,
                                                                          GlobalSourceState &gstate) const {
	return make_uniq<LocalSourceState>();
}

SourceResultType PhysicalLimitedDistinct::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                          OperatorSourceInput &input) const {
	auto &gstate_sink = sink_state->Cast<LimitedDistinctGlobalSinkState>();
	auto &gstate_source = input.global_state.Cast<LimitedDistinctGlobalSourceState>();

	if (!gstate_source.initialized) {
		gstate_sink.ht->InitializeScan(gstate_source.scan_state);
		gstate_source.initialized = true;
	}

	while (true) {
		gstate_source.group_chunk.Reset();
		gstate_source.payload_chunk.Reset();

		if (!gstate_sink.ht->Scan(gstate_source.scan_state, gstate_source.group_chunk, gstate_source.payload_chunk)) {
			return SourceResultType::FINISHED;
		}
		// Aggregate hash table scan returns "has more" when advancing between partitions, even if the current
		// output chunk is empty. Keep scanning until we either see rows or exhaust the partitions.
		if (gstate_source.group_chunk.size() > 0) {
			break;
		}
	}

	// Copy results into output chunk (groups + aggregate results)
	idx_t col = 0;
	for (idx_t i = 0; i < gstate_source.group_chunk.ColumnCount(); i++) {
		chunk.data[col].Reference(gstate_source.group_chunk.data[i]);
		col++;
	}
	for (idx_t i = 0; i < gstate_source.payload_chunk.ColumnCount(); i++) {
		chunk.data[col].Reference(gstate_source.payload_chunk.data[i]);
		col++;
	}

	gstate_source.rows_scanned.fetch_add(chunk.size(), std::memory_order_relaxed);
	return chunk.size() == 0 ? SourceResultType::FINISHED : SourceResultType::HAVE_MORE_OUTPUT;
}

InsertionOrderPreservingMap<string> PhysicalLimitedDistinct::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	string groups_str;
	for (idx_t i = 0; i < groups.size(); i++) {
		if (i > 0) {
			groups_str += "\n";
		}
		groups_str += groups[i]->GetName();
	}
	result["Groups"] = groups_str;
	result["Limit"] = to_string(limit);
	return result;
}

} // namespace duckdb
