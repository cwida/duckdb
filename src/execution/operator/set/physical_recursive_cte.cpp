#include "duckdb/execution/operator/set/physical_recursive_cte.hpp"

#include "duckdb/execution/operator/set/physical_recursive_cte_keyed_state.hpp"
#include "duckdb/execution/operator/set/physical_recursive_cte_state.hpp"
#include "duckdb/execution/operator/set/physical_recursive_cte_union_state.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/settings.hpp"

#include <cmath>

namespace duckdb {

RecursiveCTEPartialKeySpec::RecursiveCTEPartialKeySpec(vector<idx_t> indices_p, idx_t full_key_count)
    : indices(std::move(indices_p)) {
	if (indices.empty() || indices.size() >= full_key_count || !std::is_sorted(indices.begin(), indices.end()) ||
	    std::adjacent_find(indices.begin(), indices.end()) != indices.end() || indices.back() >= full_key_count) {
		throw InternalException("Invalid USING KEY partial-key index specification");
	}
}

PhysicalRecursiveCTE::PhysicalRecursiveCTE(PhysicalPlan &physical_plan, Identifier ctename, TableIndex table_index,
                                           vector<LogicalType> types, RecursiveCTEMode mode, PhysicalOperator &top,
                                           PhysicalOperator &bottom, idx_t estimated_cardinality)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::RECURSIVE_CTE, std::move(types), estimated_cardinality),
      ctename(std::move(ctename)), table_index(table_index), mode(mode),
      shared_executor_pool(make_shared_ptr<RecursiveExecutorPool>()) {
	children.push_back(top);
	children.push_back(bottom);
}

PhysicalRecursiveCTE::~PhysicalRecursiveCTE() {
}

idx_t PhysicalRecursiveCTE::NextMetricsInvocation() const {
	return metrics_invocations.fetch_add(1) + 1;
}

//===--------------------------------------------------------------------===//
// Sink State
//===--------------------------------------------------------------------===//
RecursiveCTEState::RecursiveCTEState(ClientContext &context, const PhysicalRecursiveCTE &op)
    : op(op), context(context), allow_executor_reuse(Settings::Get<EnableCachingOperatorsSetting>(context)),
      metrics(context, op), scheduler(op.shared_executor_pool, allow_executor_reuse) {
	if (metrics.Enabled()) {
		epoch_metrics = make_uniq<RecursiveCTEEpochMetrics>();
	}
}

RecursiveCTEState::~RecursiveCTEState() {
}

void RecursiveCTEState::LogMetrics(const vector<unique_ptr<RecursiveCTEPartialKeyIndex>> &partial_key_indexes) {
	metrics.Log(partial_key_indexes);
	if (epoch_metrics) {
		metrics.LogEpochSummary(*epoch_metrics);
	}
}

void RecursiveCTEState::RecordSinkMetrics(idx_t wait_ns, idx_t work_ns, idx_t rows) {
	metrics.RecordSink(wait_ns, work_ns, rows);
}

unique_ptr<GlobalSinkState> PhysicalRecursiveCTE::GetGlobalSinkState(ClientContext &context) const {
	if (UsingKey()) {
		return make_uniq<RecursiveCTEKeyedState>(context, *this);
	}
	return make_uniq<RecursiveCTEUnionState>(context, *this);
}

unique_ptr<LocalSinkState> PhysicalRecursiveCTE::GetLocalSinkState(ExecutionContext &context) const {
	return sink_state->Cast<RecursiveCTEState>().GetLocalSinkState(context);
}

SinkResultType PhysicalRecursiveCTE::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
	return input.global_state.Cast<RecursiveCTEState>().Sink(context, chunk, input);
}

void PhysicalRecursiveCTE::PrepareFinalize(ClientContext &context, GlobalSinkState &sink_state) const {
	if (UsingKey()) {
		sink_state.Cast<RecursiveCTEKeyedState>().CommitUsingKeyUpdates();
	}
}

SinkCombineResultType PhysicalRecursiveCTE::Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const {
	return input.global_state.Cast<RecursiveCTEState>().Combine(context, input);
}

//===--------------------------------------------------------------------===//
// Source
//===--------------------------------------------------------------------===//
SourceResultType PhysicalRecursiveCTE::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                       OperatorSourceInput &input) const {
	auto &gstate = sink_state->Cast<RecursiveCTEState>();
	return gstate.GetData(context, chunk, input.global_state.Cast<RecursiveCTEGlobalSourceState>());
}

unique_ptr<GlobalSourceState> PhysicalRecursiveCTE::GetGlobalSourceState(ClientContext &context) const {
	return make_uniq<RecursiveCTEGlobalSourceState>();
}

void RecursiveCTEGlobalSourceState::StartIteration(idx_t row_count) {
	lock_guard<mutex> guard(iteration_lock);
	if (iterations == 0) {
		anchor_rows = row_count;
	}
	iterations++;
	iteration_start_rows = emitted_rows.load(std::memory_order_relaxed);
	previous_iteration_rows = iteration_rows;
	iteration_rows = row_count;
}

void RecursiveCTEGlobalSourceState::StartFinalDrain(idx_t row_count) {
	lock_guard<mutex> guard(iteration_lock);
	draining = true;
	iteration_start_rows = emitted_rows.load(std::memory_order_relaxed);
	iteration_rows = row_count;
}

ProgressData RecursiveCTEGlobalSourceState::GetProgress() {
	if (finished.load(std::memory_order_relaxed)) {
		return ProgressData {1.0, 1.0, false};
	}
	auto emitted = static_cast<double>(emitted_rows.load(std::memory_order_relaxed));
	double fraction;
	{
		lock_guard<mutex> guard(iteration_lock);
		if (draining) {
			// the final state of a USING KEY recursion is emitted in one go - its size is known
			auto total = static_cast<double>(iteration_start_rows + iteration_rows);
			fraction = total == 0 ? 1.0 : emitted / total;
		} else {
			// the total number of rows is unknown - converge towards 100% as rows are emitted, logarithmically in the
			// number of rows relative to the first iteration (50% after one anchor's worth of rows, 67% after 3, 91%
			// after ~1000)
			auto anchor = static_cast<double>(MaxValue<idx_t>(anchor_rows, 1));
			fraction = 1 - 1 / (1 + std::log2(1 + emitted / anchor));
			// if the iterations are shrinking, extrapolate the remaining rows as a geometric series
			if (iteration_rows < previous_iteration_rows) {
				auto current = static_cast<double>(iteration_rows);
				auto ratio = current / static_cast<double>(previous_iteration_rows);
				auto total = static_cast<double>(iteration_start_rows) + current + current * ratio / (1 - ratio);
				if (total > 0) {
					fraction = MaxValue<double>(fraction, emitted / total);
				}
			}
			// we are not done until the recursion finishes
			fraction = MinValue<double>(fraction, 0.999);
		}
	}
	// the estimate can drop when an iteration is larger than expected
	return progress.Update(ProgressData {MinValue<double>(fraction, 1.0), 1.0, false});
}

ProgressData PhysicalRecursiveCTE::GetProgress(ClientContext &context, GlobalSourceState &gstate) const {
	return gstate.Cast<RecursiveCTEGlobalSourceState>().GetProgress();
}

vector<const_reference<PhysicalOperator>> PhysicalRecursiveCTE::GetSources() const {
	return {*this};
}

InsertionOrderPreservingMap<string> PhysicalRecursiveCTE::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	result["CTE Name"] = ctename.GetIdentifierName();
	result["Table Index"] = StringUtil::Format("%llu", table_index.index);
	SetEstimatedCardinality(result, estimated_cardinality);
	return result;
}

} // namespace duckdb
