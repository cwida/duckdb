#include "duckdb/execution/operator/set/physical_trampoline.hpp"

#include "duckdb/common/vector/vector_iterator.hpp"
#include "duckdb/execution/operator/set/physical_recursive_cte_state.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/parallel/meta_pipeline.hpp"
#include "duckdb/parallel/pipeline.hpp"
#include "duckdb/parallel/pipeline_executor.hpp"

namespace duckdb {

PhysicalTrampoline::PhysicalTrampoline(PhysicalPlan &physical_plan, Identifier ctename, TableIndex table_index,
                                       vector<LogicalType> types, idx_t selector_idx, PhysicalOperator &seed,
                                       const vector<reference<PhysicalOperator>> &branches, idx_t estimated_cardinality)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::TRAMPOLINE, std::move(types), estimated_cardinality),
      ctename(std::move(ctename)), table_index(table_index), selector_idx(selector_idx),
      shared_executor_pool(make_shared_ptr<RecursiveExecutorPool>()) {
	children.push_back(seed);
	for (auto &branch : branches) {
		children.push_back(branch.get());
	}
}

PhysicalTrampoline::~PhysicalTrampoline() {
}

//===--------------------------------------------------------------------===//
// Sink state: routed waves, the emit backlog and the wave scheduler
//===--------------------------------------------------------------------===//
struct TrampolineWaveQueue {
	deque<unique_ptr<ColumnDataCollection>> waves;
};

class TrampolineState : public GlobalSinkState {
public:
	TrampolineState(ClientContext &context, const PhysicalTrampoline &op)
	    : op(op), context(context),
	      scheduler(op.shared_executor_pool, Settings::Get<EnableCachingOperatorsSetting>(context)) {
		const auto branch_count = op.BranchCount();
		staged.resize(branch_count + 1);
		for (idx_t branch_idx = 0; branch_idx <= branch_count; branch_idx++) {
			pending.push_back(make_uniq<TrampolineWaveQueue>());
		}
		route_chunk.Initialize(Allocator::Get(context), op.GetTypes());
	}

	const PhysicalTrampoline &op;
	ClientContext &context;
	RecursiveCTESchedulerState scheduler;

	mutex lock;
	//! Rows routed by the currently executing plan; staged[0] holds rows to emit
	vector<unique_ptr<ColumnDataCollection>> staged;
	//! Pending waves per branch; index 0 is unused
	vector<unique_ptr<TrampolineWaveQueue>> pending;
	//! Emitted rows that have not yet been handed to the consumer
	unique_ptr<ColumnDataCollection> emit_backlog;
	//! Emitted rows currently being scanned
	unique_ptr<ColumnDataCollection> emit_table;
	ColumnDataScanState emit_scan;
	bool emit_active = false;
	//! The wave the currently executing branch scans; kept alive during execution
	unique_ptr<ColumnDataCollection> current_input;
	//! Round-robin cursor over the branches
	idx_t next_branch = 1;
	bool initialized = false;

	DataChunk route_chunk;
	vector<SelectionVector> route_selections;
	vector<idx_t> route_counts;
	//! Reusable empty collections, bounded to keep at most a few buffers alive
	static constexpr idx_t RECYCLED_COLLECTIONS = 8;
	vector<unique_ptr<ColumnDataCollection>> collection_pool;

	void Route(DataChunk &chunk);
	void Recycle(unique_ptr<ColumnDataCollection> collection);
	void CloseExecution();
	bool NextWave(idx_t &branch_idx, unique_ptr<ColumnDataCollection> &wave);
	SourceResultType GetData(ExecutionContext &context, DataChunk &chunk);

private:
	template <class T>
	void GatherTargets(Vector &selector, idx_t count);
	ColumnDataCollection &Staged(idx_t target);
};

ColumnDataCollection &TrampolineState::Staged(idx_t target) {
	if (!staged[target]) {
		if (!collection_pool.empty()) {
			staged[target] = std::move(collection_pool.back());
			collection_pool.pop_back();
		} else {
			staged[target] = make_uniq<ColumnDataCollection>(context, op.GetTypes());
		}
	}
	return *staged[target];
}

void TrampolineState::Recycle(unique_ptr<ColumnDataCollection> collection) {
	if (!collection || collection_pool.size() >= RECYCLED_COLLECTIONS) {
		return;
	}
	collection->ResetForReuse();
	collection_pool.push_back(std::move(collection));
}

template <class T>
void TrampolineState::GatherTargets(Vector &selector, idx_t count) {
	const auto branch_count = op.BranchCount();
	auto values = selector.Values<T>();
	D_ASSERT(values.size() == count);
	(void)count;
	for (auto entry : values) {
		if (!entry.IsValid()) {
			throw InvalidInputException("TRAMPOLINE branch selector value may not be NULL "
			                            "(expected 0 to %llu)",
			                            branch_count);
		}
		const auto value = entry.GetValue();
		bool out_of_range = std::is_signed<T>::value && value < 0;
		out_of_range = out_of_range || static_cast<uint64_t>(value) > branch_count;
		if (out_of_range) {
			throw InvalidInputException("Invalid TRAMPOLINE branch selector value %s (expected 0 to %llu)",
			                            std::to_string(value), branch_count);
		}
		const auto target = static_cast<idx_t>(value);
		route_selections[target].set_index(route_counts[target]++, entry.GetIndex());
	}
}

void TrampolineState::Route(DataChunk &chunk) {
	const auto count = chunk.size();
	if (count == 0) {
		return;
	}
	const auto target_count = op.BranchCount() + 1;
	if (route_selections.size() != target_count) {
		route_selections.clear();
		for (idx_t target = 0; target < target_count; target++) {
			route_selections.emplace_back(STANDARD_VECTOR_SIZE);
		}
		route_counts.resize(target_count);
	}
	std::fill(route_counts.begin(), route_counts.end(), 0);

	auto &selector = chunk.data[op.selector_idx];
	switch (selector.GetType().InternalType()) {
	case PhysicalType::INT8:
		GatherTargets<int8_t>(selector, count);
		break;
	case PhysicalType::INT16:
		GatherTargets<int16_t>(selector, count);
		break;
	case PhysicalType::INT32:
		GatherTargets<int32_t>(selector, count);
		break;
	case PhysicalType::INT64:
		GatherTargets<int64_t>(selector, count);
		break;
	case PhysicalType::UINT8:
		GatherTargets<uint8_t>(selector, count);
		break;
	case PhysicalType::UINT16:
		GatherTargets<uint16_t>(selector, count);
		break;
	case PhysicalType::UINT32:
		GatherTargets<uint32_t>(selector, count);
		break;
	case PhysicalType::UINT64:
		GatherTargets<uint64_t>(selector, count);
		break;
	default:
		throw InternalException("Unsupported TRAMPOLINE selector type");
	}

	for (idx_t target = 0; target < target_count; target++) {
		const auto target_rows = route_counts[target];
		if (target_rows == 0) {
			continue;
		}
		if (target_rows == count) {
			Staged(target).Append(chunk);
			return;
		}
		route_chunk.Reset();
		route_chunk.Slice(chunk, route_selections[target], target_rows);
		Staged(target).Append(route_chunk);
	}
}

void TrampolineState::CloseExecution() {
	const auto branch_count = op.BranchCount();
	for (idx_t branch_idx = 1; branch_idx <= branch_count; branch_idx++) {
		auto &branch_staged = staged[branch_idx];
		if (!branch_staged || branch_staged->Count() == 0) {
			branch_staged.reset();
			continue;
		}
		auto &queue = pending[branch_idx]->waves;
		if (op.merge_waves && !queue.empty()) {
			queue.back()->Combine(*branch_staged);
			branch_staged.reset();
		} else {
			queue.push_back(std::move(branch_staged));
		}
	}
	auto &emitted = staged[0];
	if (emitted && emitted->Count() > 0) {
		if (emit_backlog) {
			emit_backlog->Combine(*emitted);
		} else {
			emit_backlog = std::move(emitted);
		}
	}
	emitted.reset();
}

bool TrampolineState::NextWave(idx_t &branch_idx, unique_ptr<ColumnDataCollection> &wave) {
	const auto branch_count = op.BranchCount();
	for (idx_t offset = 0; offset < branch_count; offset++) {
		const auto candidate = ((next_branch - 1 + offset) % branch_count) + 1;
		auto &queue = pending[candidate]->waves;
		if (queue.empty()) {
			continue;
		}
		branch_idx = candidate;
		wave = std::move(queue.front());
		queue.pop_front();
		next_branch = (candidate % branch_count) + 1;
		return true;
	}
	return false;
}

SourceResultType TrampolineState::GetData(ExecutionContext &execution_context, DataChunk &chunk) {
	if (!initialized) {
		// the seed pipeline has finished sinking; its routed rows form the first waves
		CloseExecution();
		initialized = true;
	}
	while (chunk.size() == 0) {
		if (emit_active) {
			emit_table->Scan(emit_scan, chunk);
			if (chunk.size() != 0) {
				break;
			}
			emit_active = false;
			Recycle(std::move(emit_table));
		}
		if (emit_backlog) {
			Recycle(std::move(emit_table));
			emit_table = std::move(emit_backlog);
			emit_table->InitializeScan(emit_scan);
			emit_active = true;
			continue;
		}
		idx_t branch_idx;
		unique_ptr<ColumnDataCollection> wave;
		if (!NextWave(branch_idx, wave)) {
			return SourceResultType::FINISHED;
		}
		context.InterruptCheck();
		current_input = std::move(wave);
		for (auto &scan_ref : op.branch_recursive_scans[branch_idx - 1]) {
			scan_ref.get().collection = *current_input;
		}
		op.ExecuteBranchPipelines(execution_context, branch_idx);
		Recycle(std::move(current_input));
		CloseExecution();
	}
	return SourceResultType::HAVE_MORE_OUTPUT;
}

//===--------------------------------------------------------------------===//
// Sink interface
//===--------------------------------------------------------------------===//
unique_ptr<GlobalSinkState> PhysicalTrampoline::GetGlobalSinkState(ClientContext &context) const {
	return make_uniq<TrampolineState>(context, *this);
}

SinkResultType PhysicalTrampoline::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
	auto &gstate = input.global_state.Cast<TrampolineState>();
	lock_guard<mutex> guard(gstate.lock);
	gstate.Route(chunk);
	return SinkResultType::NEED_MORE_INPUT;
}

//===--------------------------------------------------------------------===//
// Source interface
//===--------------------------------------------------------------------===//
unique_ptr<GlobalSourceState> PhysicalTrampoline::GetGlobalSourceState(ClientContext &context) const {
	return make_uniq<GlobalSourceState>();
}

SourceResultType PhysicalTrampoline::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                     OperatorSourceInput &input) const {
	auto &gstate = sink_state->Cast<TrampolineState>();
	return gstate.GetData(context, chunk);
}

vector<const_reference<PhysicalOperator>> PhysicalTrampoline::GetSources() const {
	return {*this};
}

InsertionOrderPreservingMap<string> PhysicalTrampoline::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	result["CTE Name"] = ctename.GetIdentifierName();
	result["Table Index"] = StringUtil::Format("%llu", table_index.index);
	result["Branches"] = StringUtil::Format("%llu", BranchCount());
	result["Waves"] = merge_waves ? "merged" : "per-batch";
	SetEstimatedCardinality(result, estimated_cardinality);
	return result;
}

//===--------------------------------------------------------------------===//
// Branch execution: single-threaded inline execution with cached executors
//===--------------------------------------------------------------------===//
void PhysicalTrampoline::ExecuteBranchPipelines(ExecutionContext &context, idx_t branch_idx) const {
	D_ASSERT(branch_idx >= 1 && branch_idx <= BranchCount());
	auto &meta_pipeline = *branch_meta_pipelines[branch_idx - 1];
	auto &plan = *branch_schedule_plans[branch_idx - 1];
	auto &executor = meta_pipeline.GetExecutor();
	auto &gstate = sink_state->Cast<TrampolineState>();
	auto &scheduler = gstate.scheduler;

	auto &branch = branch_pipelines[branch_idx - 1];
	if (!Settings::Get<EnableCachingOperatorsSetting>(context.client)) {
		scheduler.ClearExecutors();
	}
	if (!scheduler.HasExecutorEntries()) {
		for (auto &branch_entry : branch_pipelines) {
			for (auto &pipeline : branch_entry.pipelines) {
				scheduler.PrepareExecutorEntry(*pipeline);
			}
		}
	}

	// reset every sink that belongs to the branch plan; the trampoline itself keeps its routed state
	for (auto &sink_reset : branch.sink_resets) {
		sink_reset.first->ResetSinkForReschedule(1, sink_reset.second);
	}

	for (auto &pipeline : plan.initialize_on_schedule_pipelines) {
		pipeline.get().ResetSource(true);
	}

	// operators can be shared between pipelines (e.g. above a UNION), so their global
	// state is reset once up-front; per-stage resets below only touch the source
	for (auto &stage : plan.stages) {
		if (stage.type == PipelineScheduleStageType::EXECUTE) {
			stage.pipeline.get().ResetOperatorsForReschedule();
		}
	}

	scheduler.InitializeInlinePlan(plan);
	for (idx_t ready_idx = 0; ready_idx < scheduler.ReadyStageCount(); ready_idx++) {
		const auto stage_idx = scheduler.ReadyStage(ready_idx);
		auto &stage = plan.stages[stage_idx];
		auto &pipeline = stage.pipeline.get();
		switch (stage.type) {
		case PipelineScheduleStageType::EXECUTE: {
			pipeline.ResetSourceForReschedule();
			scheduler.PrepareExecutors(pipeline, 1);
			auto &executors = scheduler.GetExecutors(pipeline);
			D_ASSERT(!executors.empty());
			executors[0]->PrepareForExecution();
			ExecuteRecursivePipelineInline(*executors[0]);
			break;
		}
		case PipelineScheduleStageType::PREPARE_FINISH:
			pipeline.PrepareFinalize();
			break;
		case PipelineScheduleStageType::FINISH:
			ExecuteRecursivePipelineFinishInline(pipeline, executor);
			break;
		default:
			throw InternalException("Unsupported trampoline schedule stage");
		}
		scheduler.CompleteInlineStage(plan, stage_idx);
	}
	if (scheduler.ReadyStageCount() != plan.stages.size()) {
		throw InternalException("Trampoline branch plan did not schedule every stage");
	}
}

//===--------------------------------------------------------------------===//
// Pipeline construction
//===--------------------------------------------------------------------===//
static void GatherTrampolineCTEScans(const PhysicalOperator &op, vector<const_reference<PhysicalOperator>> &cte_scans) {
	if (op.type == PhysicalOperatorType::DELIM_SCAN || op.type == PhysicalOperatorType::CTE_SCAN) {
		cte_scans.push_back(op);
	}
	for (auto child : op.GetChildren()) {
		GatherTrampolineCTEScans(child.get(), cte_scans);
	}
}

void PhysicalTrampoline::BuildPipelines(Pipeline &current, MetaPipeline &meta_pipeline) {
	op_state.reset();
	sink_state.reset();
	branch_meta_pipelines.clear();
	branch_schedule_plans.clear();
	branch_recursive_scans.clear();
	branch_pipelines.clear();
	{
		D_ASSERT(shared_executor_pool);
		lock_guard<mutex> guard(shared_executor_pool->lock);
		shared_executor_pool->executors.clear();
	}

	auto &state = meta_pipeline.GetState();
	state.SetPipelineSource(current, *this);

	auto &executor = meta_pipeline.GetExecutor();
	executor.AddRecursiveCTE(*this);

	// the seed is a regular child meta pipeline that sinks into the trampoline
	auto &initial_state_pipeline = meta_pipeline.CreateChildMetaPipeline(current, *this);
	initial_state_pipeline.Build(children[0]);

	// every branch is a detached meta pipeline the trampoline re-executes per wave
	const reference_set_t<const MetaPipeline> no_invariant_pipelines;
	for (idx_t branch_idx = 1; branch_idx < children.size(); branch_idx++) {
		auto branch_meta_pipeline = make_shared_ptr<MetaPipeline>(executor, state, this);
		branch_meta_pipeline->SetRecursiveCTE();
		branch_meta_pipeline->Build(children[branch_idx]);

		vector<reference<PhysicalColumnDataScan>> recursive_scans;
		GatherRecursiveScans(children[branch_idx], table_index, recursive_scans);
		branch_recursive_scans.push_back(std::move(recursive_scans));

		vector<shared_ptr<MetaPipeline>> branch_meta_pipelines_flat;
		branch_meta_pipeline->GetMetaPipelines(branch_meta_pipelines_flat, true, false);
		branch_schedule_plans.push_back(
		    BuildRecursivePipelineSchedulePlan(branch_meta_pipelines_flat, no_invariant_pipelines, *this));

		TrampolineBranchPipelines branch_entry;
		reference_map_t<const PhysicalOperator, idx_t> sink_producer_counts;
		reference_set_t<const Pipeline> seen_pipelines;
		for (auto &flat_meta : branch_meta_pipelines_flat) {
			vector<shared_ptr<Pipeline>> flat_pipelines;
			flat_meta->GetPipelines(flat_pipelines, false);
			for (auto &pipeline : flat_pipelines) {
				if (!seen_pipelines.insert(*pipeline).second) {
					continue;
				}
				auto sink = pipeline->GetSink();
				if (sink && sink.get() != this) {
					sink_producer_counts[*sink]++;
				}
				branch_entry.pipelines.push_back(pipeline);
			}
		}
		reference_set_t<const PhysicalOperator> seen_sinks;
		for (auto &pipeline : branch_entry.pipelines) {
			auto sink = pipeline->GetSink();
			if (sink && sink.get() != this && seen_sinks.insert(*sink).second) {
				branch_entry.sink_resets.emplace_back(pipeline, sink_producer_counts[*sink] == 1);
			}
		}
		branch_pipelines.push_back(std::move(branch_entry));
		branch_meta_pipelines.push_back(std::move(branch_meta_pipeline));
	}

	// materialized CTEs scanned inside branches must finish before this pipeline starts
	for (idx_t branch_idx = 1; branch_idx < children.size(); branch_idx++) {
		vector<const_reference<PhysicalOperator>> cte_scans;
		GatherTrampolineCTEScans(children[branch_idx], cte_scans);
		for (auto scan : cte_scans) {
			auto entry = state.cte_dependencies.find(scan);
			if (entry == state.cte_dependencies.end()) {
				continue;
			}
			auto cte_dependency = entry->second.get().shared_from_this();
			current.AddDependency(cte_dependency);
		}
	}
}

} // namespace duckdb
