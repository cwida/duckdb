#include "duckdb/execution/operator/set/physical_trampoline.hpp"

#include "duckdb/common/vector/vector_iterator.hpp"
#include "duckdb/execution/operator/set/recursive_cte_scheduling.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/parallel/meta_pipeline.hpp"
#include "duckdb/parallel/pipeline.hpp"
#include "duckdb/parallel/pipeline_executor.hpp"
#include "duckdb/parallel/task_scheduler.hpp"
#include "duckdb/parallel/event.hpp"
#include "duckdb/execution/aggregate_hashtable.hpp"
#include "duckdb/execution/operator/join/physical_delim_join.hpp"
#include "duckdb/execution/operator/aggregate/physical_hash_aggregate.hpp"

#include <cmath>

namespace duckdb {

// Waves are often small; aim for about half a vector of input per worker like the recursive runtime
static constexpr const idx_t TRAMPOLINE_ROWS_PER_THREAD = STANDARD_VECTOR_SIZE / 2;
// Pure frontier work (no independent source tasks) must amortize the event-graph setup of a
// parallel wave; below this input size the inline path wins for cheap branch plans
static constexpr const idx_t TRAMPOLINE_PARALLEL_MIN_ROWS = 16 * STANDARD_VECTOR_SIZE;

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

//! Thread-local routing state: rows are partitioned by selector into private
//! collections without locking; Combine folds them into the shared staging.
class TrampolineLocalSinkState : public LocalSinkState {
public:
	TrampolineLocalSinkState(ClientContext &context, const PhysicalTrampoline &op) : op(op) {
		const auto target_count = op.BranchCount() + 1;
		staged.resize(target_count);
		appends.resize(target_count);
		for (idx_t target = 0; target < target_count; target++) {
			selections.emplace_back(STANDARD_VECTOR_SIZE);
		}
		counts.resize(target_count);
		route_chunk.Initialize(Allocator::Get(context), op.GetTypes());
		tee_chunk.Initialize(Allocator::Get(context), op.GetTypes());
	}

	const PhysicalTrampoline &op;
	//! Routed rows per target; staged[0] holds rows to emit
	vector<unique_ptr<ColumnDataCollection>> staged;
	//! Emptied collections handed back by Combine for reuse
	vector<unique_ptr<ColumnDataCollection>> local_pool;
	vector<ColumnDataAppendState> appends;
	vector<SelectionVector> selections;
	vector<idx_t> counts;
	DataChunk route_chunk;
	DataChunk tee_chunk;

	void Route(ClientContext &context, DataChunk &chunk);
	void AppendTee(ClientContext &context, DataChunk &routed);

	bool SupportsReuse() const override {
		return true;
	}
	void Reset(ExecutionContext &context, GlobalSinkState &gstate) override {
		// combined collections were handed off; anything left is stale
		for (auto &collection : staged) {
			collection.reset();
		}
	}

private:
	template <class T>
	void GatherTargets(Vector &selector, idx_t count);
	ColumnDataCollection &Staged(ClientContext &context, idx_t target);
};

class TrampolineGlobalSourceState : public GlobalSourceState {
public:
	//! Progress estimation: rows handed to the consumer, the seed size, and completion
	atomic<idx_t> emitted_rows {0};
	atomic<idx_t> seed_rows {0};
	atomic<bool> finished {false};
	MonotonicProgress progress;

	ProgressData GetProgress() {
		if (finished.load(std::memory_order_relaxed)) {
			return ProgressData {1.0, 1.0, false};
		}
		// like the recursive CTE, the number of waves is unknown up front: converge towards
		// 100% logarithmically in the rows emitted relative to the seed
		const auto emitted = static_cast<double>(emitted_rows.load(std::memory_order_relaxed));
		const auto anchor = static_cast<double>(MaxValue<idx_t>(seed_rows.load(std::memory_order_relaxed), 1));
		auto fraction = 1 - 1 / (1 + std::log2(1 + emitted / anchor));
		fraction = MinValue<double>(fraction, 0.999);
		return progress.Update(ProgressData {fraction, 1.0, false});
	}
};

class TrampolineState : public GlobalSinkState {
public:
	TrampolineState(ClientContext &context, const PhysicalTrampoline &op)
	    : op(op), context(context), metrics(context, op, op.metrics_invocations.fetch_add(1) + 1),
	      scheduler(op.shared_executor_pool, Settings::Get<EnableCachingOperatorsSetting>(context)) {
		if (metrics.Enabled()) {
			epoch_metrics = make_uniq<RecursiveCTEEpochMetrics>();
		}
		const auto branch_count = op.BranchCount();
		staged.resize(branch_count + 1);
		invariant_materialized.resize(branch_count, false);
		for (idx_t branch_idx = 0; branch_idx <= branch_count; branch_idx++) {
			pending.push_back(make_uniq<TrampolineWaveQueue>());
		}
		if (op.RequiresRounds()) {
			for (idx_t branch_idx = 0; branch_idx <= branch_count; branch_idx++) {
				next_pending.push_back(make_uniq<TrampolineWaveQueue>());
			}
		}
		seen.resize(branch_count + 1);
	}

	const PhysicalTrampoline &op;
	ClientContext &context;
	RecursiveCTEMetrics metrics;
	unique_ptr<RecursiveCTEEpochMetrics> epoch_metrics;
	RecursiveCTESchedulerState scheduler;

	~TrampolineState() override {
		metrics.Log();
		if (epoch_metrics) {
			metrics.LogEpochSummary(*epoch_metrics);
		}
	}

	mutex lock;
	//! Rows routed by the currently executing plan; staged[0] holds rows to emit
	vector<unique_ptr<ColumnDataCollection>> staged;
	//! Pending waves per branch; index 0 is unused
	vector<unique_ptr<TrampolineWaveQueue>> pending;
	//! Waves produced during the current round, forming the next round (rounds only)
	vector<unique_ptr<TrampolineWaveQueue>> next_pending;
	//! Frozen frontier snapshot of the current round: the bag union of its input waves
	unique_ptr<ColumnDataCollection> frontier_current;
	//! Per-destination admission tables over every row ever admitted (in-memory)
	vector<unique_ptr<GroupedAggregateHashTable>> seen;
	Vector admission_addresses = Vector(LogicalType::POINTER);
	SelectionVector admission_sel = SelectionVector(STANDARD_VECTOR_SIZE);
	DataChunk admission_chunk;
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
	//! Per branch, whether its invariant builds are materialized and retained
	vector<bool> invariant_materialized;

	//! Reusable empty collections, bounded to keep at most a few buffers alive
	static constexpr idx_t RECYCLED_COLLECTIONS = 8;
	vector<unique_ptr<ColumnDataCollection>> collection_pool;

	void CombineLocal(TrampolineLocalSinkState &local_state);
	void Recycle(unique_ptr<ColumnDataCollection> collection);
	void CloseExecution();
	bool NextWave(idx_t &branch_idx, unique_ptr<ColumnDataCollection> &wave);
	bool StartRound();
	unique_ptr<ColumnDataCollection> FilterSeenRows(idx_t target, ColumnDataCollection &rows);
	SourceResultType GetData(ExecutionContext &context, DataChunk &chunk, TrampolineGlobalSourceState &source_state);
};

void TrampolineState::CombineLocal(TrampolineLocalSinkState &local_state) {
	vector<unique_ptr<ColumnDataCollection>> drained;
	{
		lock_guard<mutex> guard(lock);
		for (idx_t target = 0; target < local_state.staged.size(); target++) {
			auto &local_staged = local_state.staged[target];
			if (!local_staged) {
				continue;
			}
			if (local_staged->Count() == 0) {
				drained.push_back(std::move(local_staged));
			} else if (staged[target]) {
				staged[target]->Combine(*local_staged);
				drained.push_back(std::move(local_staged));
			} else {
				staged[target] = std::move(local_staged);
				if (!collection_pool.empty()) {
					local_state.local_pool.push_back(std::move(collection_pool.back()));
					collection_pool.pop_back();
				}
			}
			local_staged.reset();
		}
	}
	// resetting buffers can do real work; keep it outside the shared lock
	for (auto &collection : drained) {
		collection->ResetForReuse();
		local_state.local_pool.push_back(std::move(collection));
	}
}

void TrampolineState::Recycle(unique_ptr<ColumnDataCollection> collection) {
	if (!collection || collection_pool.size() >= RECYCLED_COLLECTIONS) {
		return;
	}
	collection->ResetForReuse();
	collection_pool.push_back(std::move(collection));
}

ColumnDataCollection &TrampolineLocalSinkState::Staged(ClientContext &context, idx_t target) {
	if (!staged[target]) {
		if (!local_pool.empty()) {
			staged[target] = std::move(local_pool.back());
			local_pool.pop_back();
		} else {
			staged[target] = make_uniq<ColumnDataCollection>(context, op.GetTypes());
		}
		staged[target]->InitializeAppend(appends[target]);
	}
	return *staged[target];
}

template <class T>
void TrampolineLocalSinkState::GatherTargets(Vector &selector, idx_t count) {
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
		selections[target].set_index(counts[target]++, entry.GetIndex());
	}
}

void TrampolineLocalSinkState::Route(ClientContext &context, DataChunk &chunk) {
	const auto count = chunk.size();
	if (count == 0) {
		return;
	}
	const auto target_count = op.BranchCount() + 1;
	std::fill(counts.begin(), counts.end(), 0);

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
		const auto target_rows = counts[target];
		if (target_rows == 0) {
			continue;
		}
		if (target_rows == count) {
			Staged(context, target).Append(appends[target], chunk);
			if (target != 0 && op.branch_options[target - 1].tee) {
				AppendTee(context, chunk);
			}
			return;
		}
		route_chunk.Reset();
		route_chunk.Slice(chunk, selections[target], target_rows);
		Staged(context, target).Append(appends[target], route_chunk);
		if (target != 0 && op.branch_options[target - 1].tee) {
			AppendTee(context, route_chunk);
		}
	}
}

void TrampolineLocalSinkState::AppendTee(ClientContext &context, DataChunk &routed) {
	// the emitted copy carries selector 0; reference the routed payload without
	// touching the continuation's selector vector
	tee_chunk.Reset();
	tee_chunk.Reference(routed);
	tee_chunk.data[op.selector_idx].Reference(Value::Numeric(tee_chunk.data[op.selector_idx].GetType(), 0),
	                                          count_t(routed.size()));
	tee_chunk.SetCardinality(routed.size());
	Staged(context, 0).Append(appends[0], tee_chunk);
}

void TrampolineState::CloseExecution() {
	const auto branch_count = op.BranchCount();
	for (idx_t branch_idx = 1; branch_idx <= branch_count; branch_idx++) {
		auto &branch_staged = staged[branch_idx];
		if (!branch_staged || branch_staged->Count() == 0) {
			branch_staged.reset();
			continue;
		}
		auto &queue = op.RequiresRounds() ? next_pending[branch_idx]->waves : pending[branch_idx]->waves;
		if (op.branch_wave_merge[branch_idx - 1] && !queue.empty()) {
			queue.back()->Combine(*branch_staged);
			branch_staged.reset();
		} else {
			queue.push_back(std::move(branch_staged));
		}
	}
	auto &emitted = staged[0];
	if (emitted && emitted->Count() > 0 && op.emit_distinct) {
		auto filtered = FilterSeenRows(0, *emitted);
		Recycle(std::move(emitted));
		if (filtered->Count() > 0) {
			emitted = std::move(filtered);
		}
	}
	if (emitted && emitted->Count() > 0) {
		// GetData drains the backlog before it invokes another branch, so a completed
		// invocation's emissions are handed over directly; the invocation's producers
		// were already combined in CombineLocal
		D_ASSERT(!emit_backlog);
		emit_backlog = std::move(emitted);
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

//! Admits the rows of a target not yet in its admission table, in wave order
unique_ptr<ColumnDataCollection> TrampolineState::FilterSeenRows(idx_t target, ColumnDataCollection &rows) {
	if (!seen[target]) {
		seen[target] = make_uniq<GroupedAggregateHashTable>(context, BufferAllocator::Get(context), op.types);
	}
	auto admitted = make_uniq<ColumnDataCollection>(context, op.types);
	ColumnDataAppendState append_state;
	admitted->InitializeAppend(append_state);
	if (admission_chunk.data.empty()) {
		rows.InitializeScanChunk(admission_chunk);
	}
	const auto collect_metrics = metrics.Enabled() && epoch_metrics;
	ColumnDataScanState scan;
	rows.InitializeScan(scan);
	while (rows.Scan(scan, admission_chunk)) {
		const auto candidate_count = admission_chunk.size();
		const auto admit_start =
		    collect_metrics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
		const auto new_count = seen[target]->FindOrCreateGroups(admission_chunk, admission_addresses, admission_sel);
		if (collect_metrics) {
			const auto admit_end = std::chrono::steady_clock::now();
			metrics.RecordHashRows(candidate_count);
			epoch_metrics->RecordDistinctGrouping(
			    candidate_count, new_count,
			    NumericCast<idx_t>(
			        std::chrono::duration_cast<std::chrono::nanoseconds>(admit_end - admit_start).count()));
		}
		if (new_count == 0) {
			continue;
		}
		admission_chunk.Slice(admission_sel, new_count);
		admitted->Append(append_state, admission_chunk);
	}
	return admitted;
}

//! Seals the next generation of waves into the current one and freezes the round's
//! frontier snapshot: the bag union of every input wave, built before any of them runs.
//! DISTINCT destinations are arbitrated first: the surviving rows form one wave.
bool TrampolineState::StartRound() {
	const auto branch_count = op.BranchCount();
	for (idx_t branch_idx = 1; branch_idx <= branch_count; branch_idx++) {
		const auto &options = op.branch_options[branch_idx - 1];
		if (!options.Coalescing()) {
			continue;
		}
		auto &waves = next_pending[branch_idx]->waves;
		if (waves.empty()) {
			continue;
		}
		auto candidates = std::move(waves.front());
		waves.pop_front();
		while (!waves.empty()) {
			candidates->Combine(*waves.front());
			waves.pop_front();
		}
		if (!options.Distinct()) {
			// MERGE: the round's routed rows form one bag wave, no admission
			waves.push_back(std::move(candidates));
			continue;
		}
		auto admitted = FilterSeenRows(branch_idx, *candidates);
		Recycle(std::move(candidates));
		if (admitted->Count() > 0) {
			waves.push_back(std::move(admitted));
		}
	}
	bool any_waves = false;
	for (idx_t branch_idx = 1; branch_idx <= branch_count; branch_idx++) {
		any_waves = any_waves || !next_pending[branch_idx]->waves.empty();
	}
	if (!any_waves) {
		return false;
	}
	if (!op.frontier_scans_present) {
		for (idx_t branch_idx = 1; branch_idx <= branch_count; branch_idx++) {
			std::swap(pending[branch_idx], next_pending[branch_idx]);
		}
		return true;
	}
	frontier_current = make_uniq<ColumnDataCollection>(context, op.types);
	ColumnDataAppendState frontier_append;
	frontier_current->InitializeAppend(frontier_append);
	for (idx_t branch_idx = 1; branch_idx <= branch_count; branch_idx++) {
		for (auto &wave : next_pending[branch_idx]->waves) {
			for (auto &chunk : wave->Chunks()) {
				frontier_current->Append(frontier_append, chunk);
			}
		}
		std::swap(pending[branch_idx], next_pending[branch_idx]);
	}
	return true;
}

SourceResultType TrampolineState::GetData(ExecutionContext &execution_context, DataChunk &chunk,
                                          TrampolineGlobalSourceState &source_state) {
	if (!initialized) {
		// the seed pipeline has finished sinking; its routed rows form the first waves
		CloseExecution();
		idx_t seed_rows = emit_backlog ? emit_backlog->Count() : 0;
		for (idx_t candidate = 1; candidate <= op.BranchCount(); candidate++) {
			for (auto &wave : pending[candidate]->waves) {
				seed_rows += wave->Count();
			}
		}
		if (op.RequiresRounds()) {
			StartRound();
			seed_rows = emit_backlog ? emit_backlog->Count() : 0;
			for (idx_t candidate = 1; candidate <= op.BranchCount(); candidate++) {
				for (auto &wave : pending[candidate]->waves) {
					seed_rows += wave->Count();
				}
			}
		}
		source_state.seed_rows.store(seed_rows, std::memory_order_relaxed);
		initialized = true;
	}
	while (chunk.size() == 0) {
		if (emit_active) {
			emit_table->Scan(emit_scan, chunk);
			if (chunk.size() != 0) {
				source_state.emitted_rows.fetch_add(chunk.size(), std::memory_order_relaxed);
				break;
			}
			emit_active = false;
			Recycle(std::move(emit_table));
		}
		if (emit_backlog && emit_backlog->Count() > 0) {
			// pending output stays consumable without completing further branch work: a
			// nonterminating next invocation must not block an already emitted prefix, so
			// batching happens only within one invocation's emissions
			Recycle(std::move(emit_table));
			emit_table = std::move(emit_backlog);
			emit_table->InitializeScan(emit_scan);
			emit_active = true;
			continue;
		}
		idx_t branch_idx;
		unique_ptr<ColumnDataCollection> wave;
		if (!NextWave(branch_idx, wave)) {
			if (op.RequiresRounds() && StartRound()) {
				continue;
			}
			source_state.finished.store(true, std::memory_order_relaxed);
			return SourceResultType::FINISHED;
		}
		context.InterruptCheck();
		current_input = std::move(wave);
		for (auto &scan_ref : op.branch_recursive_scans[branch_idx - 1]) {
			scan_ref.get().collection = *current_input;
		}
		if (op.frontier_scans_present) {
			for (auto &scan_ref : op.branch_frontier_scans[branch_idx - 1]) {
				scan_ref.get().collection = *frontier_current;
			}
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

unique_ptr<LocalSinkState> PhysicalTrampoline::GetLocalSinkState(ExecutionContext &context) const {
	return make_uniq<TrampolineLocalSinkState>(context.client, *this);
}

SinkResultType PhysicalTrampoline::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
	auto &lstate = input.local_state.Cast<TrampolineLocalSinkState>();
	lstate.Route(context.client, chunk);
	return SinkResultType::NEED_MORE_INPUT;
}

SinkCombineResultType PhysicalTrampoline::Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const {
	auto &gstate = input.global_state.Cast<TrampolineState>();
	auto &lstate = input.local_state.Cast<TrampolineLocalSinkState>();
	gstate.CombineLocal(lstate);
	return SinkCombineResultType::FINISHED;
}

//===--------------------------------------------------------------------===//
// Source interface
//===--------------------------------------------------------------------===//
unique_ptr<GlobalSourceState> PhysicalTrampoline::GetGlobalSourceState(ClientContext &context) const {
	return make_uniq<TrampolineGlobalSourceState>();
}

SourceResultType PhysicalTrampoline::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                     OperatorSourceInput &input) const {
	auto &gstate = sink_state->Cast<TrampolineState>();
	return gstate.GetData(context, chunk, input.global_state.Cast<TrampolineGlobalSourceState>());
}

vector<const_reference<PhysicalOperator>> PhysicalTrampoline::GetSources() const {
	return {*this};
}

ProgressData PhysicalTrampoline::GetProgress(ClientContext &context, GlobalSourceState &gstate_p) const {
	return gstate_p.Cast<TrampolineGlobalSourceState>().GetProgress();
}

InsertionOrderPreservingMap<string> PhysicalTrampoline::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	result["CTE Name"] = ctename.GetIdentifierName();
	result["Table Index"] = StringUtil::Format("%llu", table_index.index);
	result["Branches"] = StringUtil::Format("%llu", BranchCount());
	idx_t merged_branches = 0;
	for (auto merge_branch : branch_wave_merge) {
		merged_branches += merge_branch ? 1 : 0;
	}
	if (merged_branches == BranchCount()) {
		result["Waves"] = "merged";
	} else if (merged_branches == 0) {
		result["Waves"] = "per-batch";
	} else {
		result["Waves"] = "mixed";
		string merged_list;
		for (idx_t branch_idx = 0; branch_idx < branch_wave_merge.size(); branch_idx++) {
			if (branch_wave_merge[branch_idx]) {
				merged_list += (merged_list.empty() ? "" : ",") + to_string(branch_idx + 1);
			}
		}
		result["Merged Branches"] = merged_list;
	}
	string tee_list;
	string distinct_list;
	string merge_list;
	for (idx_t branch_idx = 0; branch_idx < branch_options.size(); branch_idx++) {
		auto &options = branch_options[branch_idx];
		if (options.tee) {
			tee_list += (tee_list.empty() ? "" : ",") + to_string(branch_idx + 1);
		}
		if (options.Distinct()) {
			distinct_list += (distinct_list.empty() ? "" : ",") + to_string(branch_idx + 1);
		}
		if (options.Merge()) {
			merge_list += (merge_list.empty() ? "" : ",") + to_string(branch_idx + 1);
		}
	}
	if (!tee_list.empty()) {
		result["Tee Branches"] = tee_list;
	}
	if (!distinct_list.empty()) {
		result["Distinct Branches"] = distinct_list;
	}
	if (!merge_list.empty()) {
		result["Merge Branches"] = merge_list;
	}
	if (emit_distinct) {
		result["Emit"] = "distinct";
	}
	SetEstimatedCardinality(result, estimated_cardinality);
	return result;
}

//===--------------------------------------------------------------------===//
// Branch execution: single-threaded inline execution with cached executors
//===--------------------------------------------------------------------===//
void PhysicalTrampoline::ExecuteBranchPipelines(ExecutionContext &context, idx_t branch_idx) const {
	D_ASSERT(branch_idx >= 1 && branch_idx <= BranchCount());
	auto &meta_pipeline = *branch_meta_pipelines[branch_idx - 1];
	auto &executor = meta_pipeline.GetExecutor();
	auto &gstate = sink_state->Cast<TrampolineState>();
	auto &scheduler = gstate.scheduler;

	auto &branch = branch_pipelines[branch_idx - 1];
	const auto allow_reuse = Settings::Get<EnableCachingOperatorsSetting>(context.client);
	if (!allow_reuse) {
		scheduler.ClearExecutors();
	}
	if (!scheduler.HasExecutorEntries()) {
		for (auto &branch_entry : branch_pipelines) {
			for (auto &pipeline : branch_entry.pipelines) {
				scheduler.PrepareExecutorEntry(*pipeline);
			}
		}
	}

	// once the invariant builds of this branch are materialized, later waves skip them
	const auto can_cache_invariant = allow_reuse && !branch.invariant_meta_pipelines.empty();
	const bool materialized = gstate.invariant_materialized[branch_idx - 1];
	const auto use_active_plan = can_cache_invariant && materialized && branch.active_plan;
	auto &plan = use_active_plan ? *branch.active_plan : *branch.plan;

	// one wave is executed by as many workers as its rows and chunks support; branches with
	// independent source work (table scans) parallelize that work regardless of the wave size,
	// while pure frontier work must be large enough to amortize the parallel event setup
	const auto configured_threads = NumericCast<idx_t>(TaskScheduler::GetScheduler(context.client).NumberOfThreads());
	idx_t frontier_workers = 1;
	idx_t input_rows = 0;
	if (gstate.current_input) {
		const auto scan_count = MaxValue<idx_t>(branch_recursive_scans[branch_idx - 1].size(), 1);
		input_rows = gstate.current_input->Count() * scan_count;
		auto chunk_units = gstate.current_input->ChunkCount() * scan_count;
		// a frontier-reading branch scans the whole round snapshot regardless of its own wave
		// size, so that work participates in worker sizing as well
		const auto frontier_scan_count = branch_frontier_scans[branch_idx - 1].size();
		if (frontier_scan_count > 0 && gstate.frontier_current) {
			input_rows += gstate.frontier_current->Count() * frontier_scan_count;
			chunk_units += gstate.frontier_current->ChunkCount() * frontier_scan_count;
		}
		const auto row_limit = (input_rows + TRAMPOLINE_ROWS_PER_THREAD - 1) / TRAMPOLINE_ROWS_PER_THREAD;
		const auto work_units = MaxValue<idx_t>(chunk_units, 1);
		frontier_workers = MaxValue<idx_t>(MinValue(MinValue(row_limit, work_units), configured_threads), 1);
	}
	// Independent sources parallelize on their own work estimate: a branch body scanning a large
	// base table (or draining a large window/range-join materialization) keeps full source
	// parallelism regardless of its wave size, but a small independent source must not drag a
	// cheap wave through the parallel event machinery, so it only joins a wave that is already
	// worth parallelizing.
	idx_t source_task_workers = frontier_workers;
	bool parallel_source_pays = false;
	if ((plan.has_source_tasks || plan.has_parallel_source) &&
	    plan.max_independent_source_rows >= TRAMPOLINE_PARALLEL_MIN_ROWS) {
		const auto source_row_limit =
		    (plan.max_independent_source_rows + TRAMPOLINE_ROWS_PER_THREAD - 1) / TRAMPOLINE_ROWS_PER_THREAD;
		source_task_workers = MaxValue<idx_t>(MinValue(source_row_limit, configured_threads), 1);
		parallel_source_pays = source_task_workers > 1;
	}
	const auto go_parallel =
	    parallel_source_pays || (frontier_workers > 1 && input_rows >= TRAMPOLINE_PARALLEL_MIN_ROWS);
	const auto workers = go_parallel ? MaxValue(frontier_workers, source_task_workers) : idx_t(1);

	// reset every sink that belongs to the executed plan; the trampoline keeps its routed
	// state, and materialized invariant builds keep theirs
	auto &sink_resets = use_active_plan ? branch.active_sink_resets : branch.sink_resets;
	for (auto &sink_reset : sink_resets) {
		const auto worker_bound = sink_reset.invariant_build ? configured_threads : workers;
		sink_reset.pipeline->ResetSinkForReschedule(worker_bound, sink_reset.single_producer);
	}
	// preserve-build flags live in the (re)created sink states, so set them after the resets
	if (!branch.invariant_meta_pipelines.empty()) {
		ConfigureInvariantRecursiveBuildReuse(branch.invariant_meta_pipelines, can_cache_invariant);
	}

	// Invalidate global operator state from the coordinating thread while nothing executes:
	// reusable state is reset in place, everything else is destroyed. Both schedules recreate
	// missing state after a stage's dependencies finished, because state construction may
	// consume dependency-produced sink state (shared lifecycle contract).
	for (auto &op_ref : plan.invalidate_operators) {
		Pipeline::InvalidateOperatorStateForReschedule(context.client, op_ref.get());
	}

	const auto collect_metrics = gstate.metrics.Enabled();
	if (collect_metrics && materialized && can_cache_invariant) {
		RecordInvariantRetentionReuse(gstate.metrics, branch.invariant_meta_pipelines);
	}
	const auto epoch_task_start = collect_metrics ? gstate.metrics.TaskCount() : 0;
	const auto epoch_start =
	    collect_metrics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
	RecursiveSchedulingContext scheduling_context {executor, scheduler, collect_metrics ? &gstate.metrics : nullptr,
	                                               collect_metrics ? gstate.epoch_metrics.get() : nullptr, go_parallel};
	if (go_parallel) {
		vector<shared_ptr<Event>> events;
		ScheduleRecursivePlan(plan, scheduling_context, events,
		                      RecursiveCTEParallelism(frontier_workers, source_task_workers));
		WaitForRecursiveEvents(executor, events);
	} else {
		// single-threaded wave: run the shared inline plan executor, which creates missing
		// operator state per ready stage and skips the finish events of flagged trivial stages
		ExecuteRecursiveInlinePlan(scheduling_context, plan);
	}
	if (can_cache_invariant && !materialized &&
	    InvariantRecursiveBuildsRemainReusable(branch.invariant_meta_pipelines)) {
		if (collect_metrics) {
			RecordInvariantRetentionTransition(gstate.metrics, branch.invariant_meta_pipelines);
		}
		gstate.invariant_materialized[branch_idx - 1] = true;
	}
	if (collect_metrics && gstate.current_input) {
		const auto epoch_end = std::chrono::steady_clock::now();
		const auto elapsed_us =
		    NumericCast<idx_t>(std::chrono::duration_cast<std::chrono::microseconds>(epoch_end - epoch_start).count());
		const auto &wave = *gstate.current_input;
		gstate.metrics.RecordEpoch(workers, elapsed_us, wave.Count(), wave.ChunkCount(), input_rows);
		gstate.epoch_metrics->Record(wave.Count(), workers, gstate.metrics.TaskCount() - epoch_task_start, elapsed_us,
		                             wave.SizeInBytes(), wave.AllocationSize());
	}
}

//===--------------------------------------------------------------------===//
// Pipeline construction
//===--------------------------------------------------------------------===//
void PhysicalTrampoline::BuildPipelines(Pipeline &current, MetaPipeline &meta_pipeline) {
	op_state.reset();
	sink_state.reset();
	branch_meta_pipelines.clear();
	branch_recursive_scans.clear();
	branch_frontier_scans.clear();
	frontier_scans_present = false;
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
	for (idx_t branch_idx = 1; branch_idx < children.size(); branch_idx++) {
		auto branch_meta_pipeline = make_shared_ptr<MetaPipeline>(executor, state, this);
		branch_meta_pipeline->SetRecursiveCTE();
		branch_meta_pipeline->Build(children[branch_idx]);

		vector<reference<PhysicalColumnDataScan>> recursive_scans;
		GatherRecursiveScans(children[branch_idx], table_index, recursive_scans);
		branch_recursive_scans.push_back(std::move(recursive_scans));

		vector<reference<PhysicalColumnDataScan>> frontier_scans;
		GatherFrontierScans(children[branch_idx], table_index, frontier_scans);
		frontier_scans_present = frontier_scans_present || !frontier_scans.empty();
		branch_frontier_scans.push_back(std::move(frontier_scans));

		vector<const_reference<PhysicalOperator>> cte_scan_ops;
		GatherColumnDataScans(children[branch_idx], cte_scan_ops);
		bool has_delim_scan = false;
		for (auto scan_op : cte_scan_ops) {
			has_delim_scan = has_delim_scan || scan_op.get().type == PhysicalOperatorType::DELIM_SCAN;
		}

		vector<shared_ptr<MetaPipeline>> branch_meta_pipelines_flat;
		branch_meta_pipeline->GetMetaPipelines(branch_meta_pipelines_flat, true, false);

		TrampolineBranchPipelines branch_entry;
		if (!has_delim_scan) {
			branch_entry.invariant_meta_pipelines =
			    FindInvariantRecursiveMetaPipelines(branch_meta_pipelines_flat, table_index, non_repeatable_operators);
		}
		branch_entry.plan = BuildRecursivePipelineSchedulePlan(branch_meta_pipelines_flat,
		                                                       branch_entry.invariant_meta_pipelines, *this);
		vector<shared_ptr<MetaPipeline>> active_meta_pipelines_flat;
		if (!branch_entry.invariant_meta_pipelines.empty()) {
			for (auto &flat_meta : branch_meta_pipelines_flat) {
				if (branch_entry.invariant_meta_pipelines.find(*flat_meta) ==
				    branch_entry.invariant_meta_pipelines.end()) {
					active_meta_pipelines_flat.push_back(flat_meta);
				}
			}
			branch_entry.active_plan = BuildRecursivePipelineSchedulePlan(active_meta_pipelines_flat,
			                                                              branch_entry.invariant_meta_pipelines, *this);
		}

		auto collect = [&](const vector<shared_ptr<MetaPipeline>> &metas, vector<TrampolineSinkReset> &sink_resets,
		                   vector<shared_ptr<Pipeline>> *all_pipelines) {
			reference_map_t<const PhysicalOperator, idx_t> sink_producer_counts;
			reference_set_t<const Pipeline> seen_pipelines;
			reference_set_t<const Pipeline> invariant_pipelines;
			vector<shared_ptr<Pipeline>> collected;
			for (auto &flat_meta : metas) {
				const auto is_invariant = branch_entry.invariant_meta_pipelines.find(*flat_meta) !=
				                          branch_entry.invariant_meta_pipelines.end();
				vector<shared_ptr<Pipeline>> flat_pipelines;
				flat_meta->GetPipelines(flat_pipelines, false);
				for (auto &pipeline : flat_pipelines) {
					if (is_invariant) {
						invariant_pipelines.insert(*pipeline);
					}
					if (!seen_pipelines.insert(*pipeline).second) {
						continue;
					}
					auto sink = pipeline->GetSink();
					if (sink && sink.get() != this) {
						sink_producer_counts[*sink]++;
					}
					collected.push_back(pipeline);
				}
			}
			reference_set_t<const PhysicalOperator> seen_sinks;
			for (auto &pipeline : collected) {
				auto sink = pipeline->GetSink();
				if (sink && sink.get() != this && seen_sinks.insert(*sink).second) {
					sink_resets.push_back({pipeline, sink_producer_counts[*sink] == 1,
					                       invariant_pipelines.find(*pipeline) != invariant_pipelines.end()});
				}
			}
			if (all_pipelines) {
				*all_pipelines = std::move(collected);
			}
		};
		collect(branch_meta_pipelines_flat, branch_entry.sink_resets, &branch_entry.pipelines);
		if (branch_entry.active_plan) {
			collect(active_meta_pipelines_flat, branch_entry.active_sink_resets, nullptr);
		}

		auto flag_trivial = [&](RecursiveCTEPipelineSchedulePlan &schedule) {
			for (auto &stage : schedule.stages) {
				if (stage.type != PipelineScheduleStageType::FINISH) {
					continue;
				}
				auto &pipeline = stage.pipeline.get();
				if (pipeline.GetSink().get() != this) {
					continue;
				}
				bool requires_finalize = false;
				for (auto &op_ref : pipeline.GetIntermediateOperators()) {
					requires_finalize = requires_finalize || op_ref.get().RequiresOperatorFinalize();
				}
				stage.trivial_finish = !requires_finalize;
			}
		};
		flag_trivial(*branch_entry.plan);
		if (branch_entry.active_plan) {
			flag_trivial(*branch_entry.active_plan);
		}

		branch_pipelines.push_back(std::move(branch_entry));
		branch_meta_pipelines.push_back(std::move(branch_meta_pipeline));
	}

	// materialized CTEs scanned inside branches must finish before this pipeline starts
	for (idx_t branch_idx = 1; branch_idx < children.size(); branch_idx++) {
		vector<const_reference<PhysicalOperator>> cte_scans;
		GatherColumnDataScans(children[branch_idx], cte_scans);
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
