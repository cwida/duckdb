#include "duckdb/parallel/pipeline.hpp"
#include "duckdb/parallel/pipeline_broadcast_exchange.hpp"

#include "duckdb/common/algorithm.hpp"
#include "duckdb/common/printer.hpp"
#include "duckdb/common/tree_renderer/text_tree_renderer.hpp"
#include "duckdb/execution/executor.hpp"
#include "duckdb/execution/operator/aggregate/physical_ungrouped_aggregate.hpp"
#include "duckdb/execution/operator/helper/physical_result_collector.hpp"
#include "duckdb/execution/operator/scan/physical_table_scan.hpp"
#include "duckdb/execution/operator/set/physical_cte.hpp"
#include "duckdb/execution/operator/set/physical_recursive_cte.hpp"
#include "duckdb/logging/log_type.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parallel/meta_pipeline.hpp"
#include "duckdb/parallel/pipeline_event.hpp"
#include "duckdb/parallel/pipeline_executor.hpp"
#include "duckdb/parallel/pipeline_schedule.hpp"
#include "duckdb/parallel/progress_verifier.hpp"
#include "duckdb/parallel/task_scheduler.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/storage/buffer_manager.hpp"

namespace duckdb {

static shared_ptr<GlobalSourceState> ToSharedSourceState(unique_ptr<GlobalSourceState> state) {
	return shared_ptr<GlobalSourceState>(std::move(state));
}

PipelineTask::PipelineTask(Pipeline &pipeline_p, shared_ptr<Event> event_p)
    : ExecutorTask(pipeline_p.executor, std::move(event_p)), pipeline(pipeline_p) {
	auto sink = pipeline.GetSink();
	if (sink && sink->RequiredPartitionInfo().AnyRequired()) {
		// Account for every task before lazy executor construction can advance the batch minimum.
		reserved_batch_index = pipeline.RegisterNewBatchIndex();
	}
}

TaskExecutionResult PipelineTask::ExecuteTask(TaskExecutionMode mode) {
	if (!pipeline_executor) {
		pipeline_executor = make_uniq<PipelineExecutor>(pipeline.GetClientContext(), pipeline, reserved_batch_index);
	}

	pipeline_executor->SetTaskForInterrupts(shared_from_this());

	if (mode == TaskExecutionMode::PROCESS_PARTIAL) {
		auto res = pipeline_executor->Execute(PARTIAL_CHUNK_COUNT);

		switch (res) {
		case PipelineExecuteResult::NOT_FINISHED:
			return TaskExecutionResult::TASK_NOT_FINISHED;
		case PipelineExecuteResult::INTERRUPTED:
			return TaskExecutionResult::TASK_BLOCKED;
		case PipelineExecuteResult::FINISHED:
			break;
		}
	} else {
		auto res = pipeline_executor->Execute();
		switch (res) {
		case PipelineExecuteResult::NOT_FINISHED:
			throw InternalException("Execute without limit should not return NOT_FINISHED");
		case PipelineExecuteResult::INTERRUPTED:
			return TaskExecutionResult::TASK_BLOCKED;
		case PipelineExecuteResult::FINISHED:
			break;
		}
	}

	// release the local states and their buffer pins before the sink can be finalized
	pipeline_executor.reset();
	event->FinishTask();
	return TaskExecutionResult::TASK_FINISHED;
}

Pipeline::Pipeline(Executor &executor_p)
    : executor(executor_p), ready(false), initialized(false), source(nullptr), sink(nullptr) {
}

ClientContext &Pipeline::GetClientContext() {
	return executor.context;
}

bool Pipeline::GetProgress(ProgressData &progress) {
	PipelineProgress detailed_progress;
	GetDetailedProgress(detailed_progress);
	progress = detailed_progress.pipeline;
	return progress.IsValid();
}

void Pipeline::GetDetailedProgress(PipelineProgress &progress) {
	D_ASSERT(source);
	idx_t source_cardinality = MinValue<idx_t>(source->estimated_cardinality, 1ULL << 48ULL);
	if (source_cardinality < 1) {
		source_cardinality = 1;
	}
	if (!initialized) {
		progress.source.done = 0;
		progress.source.total = double(source_cardinality);
		progress.pipeline = progress.source;
		return;
	}
	auto &client = executor.context;

	auto state = GetSourceState();
	if (state) {
		progress.source = source->GetProgress(client, *state);
	} else {
		progress.source.done = 0;
		progress.source.total = double(source_cardinality);
	}
	progress.pipeline = progress.source;
	progress.pipeline.Normalize(double(source_cardinality));
	if (sink) {
		lock_guard<mutex> guard(sink->lock);
		if (sink->sink_state) {
			progress.pipeline = sink->GetSinkProgress(client, *sink->sink_state, progress.pipeline);
		}
	}
}

void Pipeline::ScheduleSequentialTask(shared_ptr<Event> &event) {
	vector<shared_ptr<Task>> tasks;
	tasks.push_back(make_uniq<PipelineTask>(*this, event));
	event->SetTasks(std::move(tasks));
}

bool Pipeline::TryGetMaxThreads(idx_t &max_threads) {
	// check if the sink, source and all intermediate operators support parallelism
	if (!sink->ParallelSink()) {
		return false;
	}
	if (!source->ParallelSource()) {
		return false;
	}
	auto source_state = GetSourceState();
	if (!source_state) {
		// the source state was never initialized, e.g. a recursive pipeline before its first iteration
		return false;
	}
	max_threads = source_state->MaxThreads();

	for (auto &op_ref : operators) {
		auto &op = op_ref.get();
		if (!op.ParallelOperator()) {
			return false;
		}
		lock_guard<mutex> guard(op.lock);
		if (op.op_state) {
			max_threads = MinValue<idx_t>(max_threads, op.op_state->MaxThreads(max_threads));
		}
	}

	auto &scheduler = TaskScheduler::GetScheduler(executor.context);
	auto active_threads = scheduler.NumberOfThreads();
	if (max_threads > active_threads) {
		max_threads = active_threads;
	}
	if (sink) {
		lock_guard<mutex> guard(sink->lock);
		if (sink->sink_state) {
			max_threads = MinValue<idx_t>(max_threads, sink->sink_state->MaxThreads(max_threads));
		}
	}

	return true;
}

bool Pipeline::ScheduleParallel(shared_ptr<Event> &event) {
	idx_t max_threads;

	if (!TryGetMaxThreads(max_threads)) {
		return false;
	}

	// Handle partition requirements specific to scheduling
	auto partition_info = sink->RequiredPartitionInfo();
	if (partition_info.batch_index) {
		if (!source->SupportsPartitioning(partition_info)) {
			throw InternalException(
			    "Attempting to schedule a pipeline where the sink requires batch index but source does not support it");
		}
	}

	return LaunchScanTasks(event, max_threads);
}

idx_t Pipeline::GetMaxThreads() {
	idx_t max_threads;

	if (!TryGetMaxThreads(max_threads)) {
		return 1; // Fallback for unsupported parallelism
	}

	return max_threads;
}

bool Pipeline::IsOrderDependent() const {
	if (source) {
		auto source_order = source->SourceOrder();
		if (source_order == OrderPreservationType::FIXED_ORDER) {
			return true;
		}
		if (source_order == OrderPreservationType::NO_ORDER) {
			return false;
		}
	}
	for (auto &op_ref : operators) {
		auto &op = op_ref.get();
		if (op.OperatorOrder() == OrderPreservationType::NO_ORDER) {
			return false;
		}
		if (op.OperatorOrder() == OrderPreservationType::FIXED_ORDER) {
			return true;
		}
	}
	if (!Settings::Get<PreserveInsertionOrderSetting>(executor.context)) {
		return false;
	}
	if (sink && sink->SinkOrderDependent()) {
		return true;
	}
	return false;
}

void Pipeline::SetExternalInput(const vector<reference<Pipeline>> &producer_pipelines) {
	D_ASSERT(!producer_pipelines.empty());
	input_mode = PipelineInputMode::EXTERNAL_INPUT;
	external_input_producers.clear();
	external_input_producers.reserve(producer_pipelines.size());
	for (auto &producer : producer_pipelines) {
		external_input_producers.emplace_back(producer.get().shared_from_this());
	}
	annotated_lock_guard<annotated_mutex> guard(external_input_lock);
	external_input_event.reset();
	external_input_event_state = ExternalInputEventState::EXTERNAL_INPUT_UNSET;
}

void Pipeline::ClearExternalInput() {
	D_ASSERT(IsExternalInput());
	input_mode = PipelineInputMode::SCHEDULED_SOURCE;
	external_input_producers.clear();
	annotated_lock_guard<annotated_mutex> guard(external_input_lock);
	external_input_event.reset();
	external_input_event_state = ExternalInputEventState::EXTERNAL_INPUT_UNSET;
}

bool Pipeline::HasExternalInputProducer(const Pipeline &pipeline) const {
	for (auto &producer_ref : external_input_producers) {
		auto producer = producer_ref.lock();
		D_ASSERT(producer);
		if (RefersToSameObject(*producer, pipeline)) {
			return true;
		}
	}
	return false;
}

bool Pipeline::CanUseExternalInput(const OperatorPartitionInfo &source_partition_info) const {
	if (!sink || !sink->ParallelSink() || sink->SinkOrderDependent()) {
		return false;
	}
	if (sink->GetExternalInputSupport() != PipelineExternalInputSupport::SUPPORTED) {
		return false;
	}
	auto required_partition_info = sink->RequiredPartitionInfo();
	if (required_partition_info.RequiresPartitionColumns()) {
		return false;
	}
	if (required_partition_info.RequiresBatchIndex() && base_batch_index != 0) {
		// Later ordered sink pipelines rely on pipeline dependencies to sequence their batch ranges.
		return false;
	}
	if (required_partition_info.RequiresBatchIndex() && !source_partition_info.RequiresBatchIndex()) {
		return false;
	}
	for (auto &op_ref : operators) {
		auto &op = op_ref.get();
		if (op.GetExternalInputSupport() != PipelineExternalInputSupport::SUPPORTED) {
			return false;
		}
		if (!op.ParallelOperator()) {
			return false;
		}
		if (op.OperatorOrder() == OrderPreservationType::FIXED_ORDER) {
			return false;
		}
	}
	return true;
}

PipelineExternalInputCost Pipeline::GetExternalInputCost() const {
	if (sink && sink->GetExternalInputCost() == PipelineExternalInputCost::SERIALIZED_FANOUT) {
		return PipelineExternalInputCost::SERIALIZED_FANOUT;
	}
	for (auto &op_ref : operators) {
		if (op_ref.get().GetExternalInputCost() == PipelineExternalInputCost::SERIALIZED_FANOUT) {
			return PipelineExternalInputCost::SERIALIZED_FANOUT;
		}
	}
	return PipelineExternalInputCost::PIPELINED;
}

bool Pipeline::CanStopSourceEarly() const {
	// Used by CTE fanout selection to keep streaming only when the consumer may finish early.
	if (sink && sink->GetSourceConsumption() == PipelineSourceConsumption::MAY_STOP_EARLY) {
		return true;
	}
	for (auto &op_ref : operators) {
		if (op_ref.get().GetSourceConsumption() == PipelineSourceConsumption::MAY_STOP_EARLY) {
			return true;
		}
	}
	return false;
}

void Pipeline::Schedule(shared_ptr<Event> &event) {
	D_ASSERT(ready);
	D_ASSERT(sink);
	if (IsExternalInput()) {
		ScheduleExternalInputEvent(event);
		return;
	}
	Reset();
	if (!ScheduleParallel(event)) {
		// could not parallelize this pipeline: push a sequential task instead
		ScheduleSequentialTask(event);
	}
}

bool Pipeline::LaunchScanTasks(shared_ptr<Event> &event, idx_t max_threads) {
	// split the scan up into parts and schedule the parts
	if (max_threads <= 1) {
		// too small to parallelize
		return false;
	}

	// launch a task for every thread
	vector<shared_ptr<Task>> tasks;
	for (idx_t i = 0; i < max_threads; i++) {
		tasks.push_back(make_uniq<PipelineTask>(*this, event));
	}
	event->SetTasks(std::move(tasks));
	return true;
}

void Pipeline::ResetSink() {
	if (sink) {
		if (!sink->IsSink()) {
			throw InternalException("Sink of pipeline does not have IsSink set");
		}
		lock_guard<mutex> guard(sink->lock);
		if (!sink->sink_state) {
			sink->sink_state = sink->GetGlobalSinkState(GetClientContext());
		}
	}
}

void Pipeline::ResetSinkForReschedule(idx_t scheduled_workers, bool single_producer) {
	if (!sink) {
		return;
	}
	if (!sink->IsSink()) {
		throw InternalException("Sink of pipeline does not have IsSink set");
	}
	NotifyProgressReset();
	lock_guard<mutex> guard(sink->lock);
	auto &client = GetClientContext();
	auto allow_reuse = Settings::Get<EnableCachingOperatorsSetting>(client);
	if (allow_reuse && sink->sink_state && sink->sink_state->SupportsReuse()) {
		sink->sink_state->reschedule_worker_limit = scheduled_workers;
		sink->sink_state->reschedule_single_producer = single_producer;
		sink->sink_state->Reset(client);
		return;
	}
	sink->sink_state = sink->GetGlobalSinkState(client);
	sink->sink_state->reschedule_worker_limit = scheduled_workers;
	sink->sink_state->reschedule_single_producer = single_producer;
}

void Pipeline::PrepareFinalize() {
	if (sink) {
		if (!sink->IsSink()) {
			throw InternalException("Sink of pipeline does not have IsSink set");
		}
		lock_guard<mutex> guard(sink->lock);
		if (!sink->sink_state) {
			throw InternalException("Sink of pipeline does not have sink state");
		}
		sink->PrepareFinalize(GetClientContext(), *sink->sink_state);
	}
}

void Pipeline::ResetSinkAndOperators() {
	ResetSink();
	InitializeOperatorState();
}

void Pipeline::InitializeOperatorState() {
	for (auto &op_ref : operators) {
		auto &op = op_ref.get();
		lock_guard<mutex> guard(op.lock);
		if (!op.op_state) {
			op.op_state = op.GetGlobalOperatorState(GetClientContext());
		}
	}
}

void Pipeline::Reset() {
	ResetSinkAndOperators();
	ResetSource(false);
	initialized = true;
}

void Pipeline::InvalidateOperatorStateForReschedule(ClientContext &client, PhysicalOperator &op) {
	auto allow_reuse = Settings::Get<EnableCachingOperatorsSetting>(client);
	lock_guard<mutex> guard(op.lock);
	if (allow_reuse && op.op_state && op.ResetGlobalOperatorState(client, *op.op_state)) {
		return;
	}
	op.op_state.reset();
}

void Pipeline::ResetSourceForReschedule() {
	NotifyProgressReset();
	if (source && !source->IsSource()) {
		throw InternalException("Source of pipeline does not have IsSource set");
	}
	auto &client = GetClientContext();
	auto allow_reuse = Settings::Get<EnableCachingOperatorsSetting>(client);
	auto source_state = GetSourceState();
	auto partition_info = sink ? sink->RequiredPartitionInfo() : OperatorPartitionInfo::NoPartitionInfo();
	if (allow_reuse && source_state && source_state->SupportsReuse()) {
		source_state->Reset(client);
	} else {
		SetSourceState(ToSharedSourceState(source->GetGlobalSourceState(client, partition_info)));
	}
	if (partition_info.AnyRequired()) {
		ResetBatchIndexes();
	}
	initialized = true;
}

void Pipeline::NotifyProgressReset() {
	auto progress_verifier = executor.GetProgressVerifier();
	if (progress_verifier) {
		progress_verifier->OnReset(*this);
	}
}

void Pipeline::ResetSource(bool force) {
	if (source && !source->IsSource()) {
		throw InternalException("Source of pipeline does not have IsSource set");
	}
	auto source_state = GetSourceState();
	if (force || !source_state) {
		NotifyProgressReset();
		auto partition_info = sink ? sink->RequiredPartitionInfo() : OperatorPartitionInfo::NoPartitionInfo();
		SetSourceState(ToSharedSourceState(source->GetGlobalSourceState(GetClientContext(), partition_info)));
	}
}

void Pipeline::PrepareExternalInput() {
	if (!IsExternalInput()) {
		throw InternalException("PrepareExternalInput called for a pipeline that is not externally fed");
	}
	if (initialized) {
		return;
	}
	annotated_lock_guard<annotated_mutex> guard(external_input_lock);
	if (initialized) {
		return;
	}
	ResetSinkAndOperators();
	initialized = true;
}

shared_ptr<GlobalSourceState> Pipeline::GetSourceState() {
	annotated_lock_guard<annotated_mutex> guard(source_state_lock);
	return source_state;
}

void Pipeline::SetSourceState(shared_ptr<GlobalSourceState> state) {
	annotated_lock_guard<annotated_mutex> guard(source_state_lock);
	source_state = std::move(state);
}

void Pipeline::FinishSourceAndPreventBlocking(ClientContext &context) {
	if (IsExternalInput() || !source) {
		return;
	}
	auto source_state = GetSourceState();
	if (!source_state) {
		return;
	}
	source->SourceFinished(context, *source_state);
	annotated_lock_guard<annotated_mutex> state_guard(source_state->lock);
	source_state->PreventBlocking();
	source_state->UnblockTasks();
}

void Pipeline::PreventSourceBlocking() {
	auto source_state = GetSourceState();
	if (!source_state) {
		return;
	}
	annotated_lock_guard<annotated_mutex> state_guard(source_state->lock);
	source_state->PreventBlocking();
	source_state->UnblockTasks();
}

void Pipeline::PreventSinkBlocking() {
	auto sink = GetSink();
	if (!sink) {
		return;
	}
	lock_guard<mutex> sink_guard(sink->lock);
	if (!sink->sink_state) {
		return;
	}
	annotated_lock_guard<annotated_mutex> state_guard(sink->sink_state->lock);
	sink->sink_state->PreventBlocking();
	sink->sink_state->UnblockTasks();
}

void Pipeline::PreventBlocking() {
	PreventSourceBlocking();
	PreventSinkBlocking();
}

void Pipeline::SetExternalInputEvent(const shared_ptr<Event> &event) {
	if (!IsExternalInput()) {
		throw InternalException("SetExternalInputEvent called for a pipeline that is not externally fed");
	}
	if (!event) {
		throw InternalException("SetExternalInputEvent called with a null event");
	}
	annotated_lock_guard<annotated_mutex> guard(external_input_lock);
	auto existing_event = external_input_event.lock();
	if (existing_event && existing_event.get() != event.get()) {
		throw InternalException("External input pipeline event was registered more than once");
	}
	external_input_event = event;
	external_input_event_state = ExternalInputEventState::EXTERNAL_INPUT_REGISTERED;
}

void Pipeline::ScheduleExternalInputEvent(shared_ptr<Event> event) {
	shared_ptr<Event> event_to_finish;
	{
		annotated_lock_guard<annotated_mutex> guard(external_input_lock);
		if (!IsExternalInput()) {
			throw InternalException("ScheduleExternalInputEvent called for a pipeline that is not externally fed");
		}
		if (!event) {
			throw InternalException("ScheduleExternalInputEvent called with a null event");
		}
		auto existing_event = external_input_event.lock();
		if (existing_event && existing_event.get() != event.get()) {
			throw InternalException("External input pipeline event was registered more than once");
		}
		external_input_event = event;
		switch (external_input_event_state) {
		case ExternalInputEventState::EXTERNAL_INPUT_COMPLETED_BEFORE_SCHEDULE:
			external_input_event_state = ExternalInputEventState::EXTERNAL_INPUT_COMPLETED;
			event_to_finish = std::move(event);
			break;
		case ExternalInputEventState::EXTERNAL_INPUT_COMPLETED:
			event_to_finish = std::move(event);
			break;
		case ExternalInputEventState::EXTERNAL_INPUT_UNSET:
		case ExternalInputEventState::EXTERNAL_INPUT_REGISTERED:
		case ExternalInputEventState::EXTERNAL_INPUT_EVENT_SCHEDULED:
			external_input_event_state = ExternalInputEventState::EXTERNAL_INPUT_EVENT_SCHEDULED;
			break;
		}
	}
	if (event_to_finish && !event_to_finish->IsFinished()) {
		event_to_finish->Finish();
	}
}

void Pipeline::CompleteExternalInput() {
	shared_ptr<Event> event;
	{
		annotated_lock_guard<annotated_mutex> guard(external_input_lock);
		if (!IsExternalInput()) {
			throw InternalException("CompleteExternalInput called for a pipeline that is not externally fed");
		}
		if (external_input_event_state == ExternalInputEventState::EXTERNAL_INPUT_COMPLETED ||
		    external_input_event_state == ExternalInputEventState::EXTERNAL_INPUT_COMPLETED_BEFORE_SCHEDULE) {
			return;
		}
		event = external_input_event.lock();
		if (!event) {
			throw InternalException("Completing external input pipeline before its event was scheduled");
		}
		if (external_input_event_state == ExternalInputEventState::EXTERNAL_INPUT_REGISTERED) {
			external_input_event_state = ExternalInputEventState::EXTERNAL_INPUT_COMPLETED_BEFORE_SCHEDULE;
			return;
		}
		D_ASSERT(external_input_event_state == ExternalInputEventState::EXTERNAL_INPUT_EVENT_SCHEDULED);
		external_input_event_state = ExternalInputEventState::EXTERNAL_INPUT_COMPLETED;
	}
	if (!event->IsFinished()) {
		event->Finish();
	}
}

void Pipeline::Ready() {
	if (ready) {
		return;
	}
	ready = true;
	std::reverse(operators.begin(), operators.end());
}

void Pipeline::AddDependency(shared_ptr<Pipeline> &pipeline) {
	D_ASSERT(pipeline);
	dependencies.push_back(weak_ptr<Pipeline>(pipeline));
	pipeline->parents.push_back(weak_ptr<Pipeline>(shared_from_this()));
}

void Pipeline::RemoveDependency(const shared_ptr<Pipeline> &pipeline) {
	D_ASSERT(pipeline);
	bool found_dependency = false;
	for (auto entry = dependencies.begin(); entry != dependencies.end(); entry++) {
		auto dependency = entry->lock();
		D_ASSERT(dependency);
		if (RefersToSameObject(*dependency, *pipeline)) {
			dependencies.erase(entry);
			found_dependency = true;
			break;
		}
	}
	if (!found_dependency) {
		throw InternalException("Attempted to remove a missing pipeline dependency");
	}
	for (auto entry = pipeline->parents.begin(); entry != pipeline->parents.end(); entry++) {
		auto parent = entry->lock();
		D_ASSERT(parent);
		if (RefersToSameObject(*parent, *this)) {
			pipeline->parents.erase(entry);
			return;
		}
	}
	throw InternalException("Pipeline dependency had no matching parent");
}

void Pipeline::AddDataflowDependency(shared_ptr<Pipeline> &pipeline) {
	D_ASSERT(pipeline);
	dataflow_dependencies.push_back(weak_ptr<Pipeline>(pipeline));
	pipeline->parents.push_back(weak_ptr<Pipeline>(shared_from_this()));
}

void Pipeline::RemoveDataflowDependency(const shared_ptr<Pipeline> &pipeline) {
	D_ASSERT(pipeline);
	bool found_dependency = false;
	for (auto entry = dataflow_dependencies.begin(); entry != dataflow_dependencies.end(); entry++) {
		auto dependency = entry->lock();
		D_ASSERT(dependency);
		if (RefersToSameObject(*dependency, *pipeline)) {
			dataflow_dependencies.erase(entry);
			found_dependency = true;
			break;
		}
	}
	if (!found_dependency) {
		throw InternalException("Attempted to remove a missing pipeline dataflow dependency");
	}
	for (auto entry = pipeline->parents.begin(); entry != pipeline->parents.end(); entry++) {
		auto parent = entry->lock();
		D_ASSERT(parent);
		if (RefersToSameObject(*parent, *this)) {
			pipeline->parents.erase(entry);
			return;
		}
	}
	throw InternalException("Pipeline dataflow dependency had no matching parent");
}

vector<weak_ptr<Pipeline>> Pipeline::GetDependencies() const {
	return dependencies;
}

PipelineDependency::PipelineDependency(Pipeline &pipeline_p, PipelineDependencyType type_p)
    : pipeline(pipeline_p.shared_from_this()), type(type_p) {
}

void Pipeline::AddIntraDependency(Pipeline &dependency, PipelineDependencyType type) {
	intra_dependencies.emplace_back(dependency, type);
}

bool Pipeline::RemoveOptionalIntraDependency(const Pipeline &dependency) {
	for (auto it = intra_dependencies.begin(); it != intra_dependencies.end(); it++) {
		if (it->type != PipelineDependencyType::OPTIONAL_DEPENDENCY) {
			continue;
		}
		auto dep = it->pipeline.lock();
		if (dep && RefersToSameObject(*dep, dependency)) {
			intra_dependencies.erase(it);
			return true;
		}
	}
	return false;
}

void Pipeline::InheritDependencies(const Pipeline &other) {
	dependencies = other.dependencies;
	intra_dependencies = other.intra_dependencies;
}

vector<shared_ptr<Pipeline>> Pipeline::GetAllDependencies() const {
	vector<shared_ptr<Pipeline>> result;
	for (auto &intra_dependency : intra_dependencies) {
		auto dep = intra_dependency.pipeline.lock();
		if (dep) {
			result.push_back(std::move(dep));
		}
	}
	for (auto &weak_dep : dependencies) {
		auto dep = weak_dep.lock();
		if (dep) {
			result.push_back(std::move(dep));
		}
	}
	return result;
}

string Pipeline::ToString() const {
	TextTreeRenderer renderer;
	return renderer.ToString(*this);
}

void Pipeline::Print() const {
	Printer::Print(ToString());
}

void Pipeline::PrintDependencies() const {
	for (auto &dep : dependencies) {
		shared_ptr<Pipeline>(dep)->Print();
	}
}

vector<reference<PhysicalOperator>> Pipeline::GetOperators() {
	vector<reference<PhysicalOperator>> result;
	D_ASSERT(source);
	result.push_back(*source);
	for (auto &op : operators) {
		result.push_back(op.get());
	}
	if (sink) {
		result.push_back(*sink);
	}
	return result;
}

vector<const_reference<PhysicalOperator>> Pipeline::GetOperators() const {
	vector<const_reference<PhysicalOperator>> result;
	D_ASSERT(source);
	result.push_back(*source);
	for (auto &op : operators) {
		result.push_back(op.get());
	}
	if (sink) {
		result.push_back(*sink);
	}
	return result;
}

const vector<reference<PhysicalOperator>> &Pipeline::GetIntermediateOperators() const {
	return operators;
}

void Pipeline::ClearSource() {
	{
		annotated_lock_guard<annotated_mutex> source_guard(source_state_lock);
		source_state.reset();
	}
	ResetBatchIndexes();
}

void Pipeline::ResetBatchIndexes() {
	lock_guard<mutex> batch_guard(batch_lock);
	batch_indexes.clear();
}

idx_t Pipeline::RegisterNewBatchIndex() {
	lock_guard<mutex> l(batch_lock);
	idx_t minimum = batch_indexes.empty() ? base_batch_index : *batch_indexes.begin();
	batch_indexes.insert(minimum);
	return minimum;
}

idx_t Pipeline::UpdateBatchIndex(idx_t old_index, idx_t new_index) {
	lock_guard<mutex> l(batch_lock);
	if (new_index < *batch_indexes.begin()) {
		throw InternalException("Processing batch index %llu, but previous min batch index was %llu", new_index,
		                        *batch_indexes.begin());
	}
	auto entry = batch_indexes.find(old_index);
	if (entry == batch_indexes.end()) {
		throw InternalException("Batch index %llu was not found in set of active batch indexes", old_index);
	}
	batch_indexes.erase(entry);
	batch_indexes.insert(new_index);
	return *batch_indexes.begin();
}
//===--------------------------------------------------------------------===//
// Pipeline Build State
//===--------------------------------------------------------------------===//
struct PipelineExternalInputCandidate {
	enum class InputMode : uint8_t { MATERIALIZED_COST, EXTERNAL_INPUT, MATERIALIZED_CYCLE };

	PipelineExternalInputCandidate(Pipeline &pipeline_p, PhysicalOperator &materialized_source_p,
	                               PhysicalOperator &external_source_p, shared_ptr<Pipeline> cte_dependency_p,
	                               PhysicalCTE &cte_p, idx_t consumer_idx_p)
	    : pipeline(pipeline_p), materialized_source(materialized_source_p), external_source(external_source_p),
	      cte_dependency(std::move(cte_dependency_p)), cte(cte_p), consumer_idx(consumer_idx_p),
	      has_blocking_dependencies(pipeline_p.GetDependencies().size() > 1),
	      input_cost(pipeline_p.GetExternalInputCost()) {
	}

	reference<Pipeline> pipeline;
	reference<PhysicalOperator> materialized_source;
	reference<PhysicalOperator> external_source;
	shared_ptr<Pipeline> cte_dependency;
	reference<PhysicalCTE> cte;
	idx_t consumer_idx;
	idx_t selection_idx = DConstants::INVALID_INDEX;
	bool has_blocking_dependencies;
	PipelineExternalInputCost input_cost;
	InputMode input_mode = InputMode::MATERIALIZED_COST;

	bool UsesExternalInput() const {
		return input_mode == InputMode::EXTERNAL_INPUT;
	}
};

struct CTEPipelineSelection {
	CTEPipelineSelection(PhysicalCTE &cte_p, Pipeline &pipeline_p, shared_ptr<Pipeline> cte_dependency_p,
	                     bool dependency_added_p)
	    : cte(cte_p), pipeline(pipeline_p), cte_dependency(std::move(cte_dependency_p)),
	      fallback_dependency_added(dependency_added_p), fallback_dependency_active(dependency_added_p) {
	}

	reference<PhysicalCTE> cte;
	reference<Pipeline> pipeline;
	shared_ptr<Pipeline> cte_dependency;
	idx_t candidate_count = 0;
	idx_t serialized_fanout_candidate_count = 0;
	idx_t direct_candidate_count = 0;
	idx_t estimated_materialization_size = 0;
	bool fallback_dependency_added;
	bool fallback_dependency_active;
	bool stream_serialized_candidates = false;
};

struct PipelineBufferedConsumerRecord {
	PipelineBufferedConsumerRecord(Pipeline &pipeline_p, PhysicalOperator &materialized_source_p,
	                               shared_ptr<Pipeline> cte_dependency_p, PhysicalCTE &cte_p, idx_t consumer_idx_p)
	    : pipeline(pipeline_p), materialized_source(materialized_source_p), cte_dependency(std::move(cte_dependency_p)),
	      cte(cte_p), consumer_idx(consumer_idx_p) {
	}

	reference<Pipeline> pipeline;
	reference<PhysicalOperator> materialized_source;
	shared_ptr<Pipeline> cte_dependency;
	reference<PhysicalCTE> cte;
	idx_t consumer_idx;
	bool demoted = false;
};

class PipelineBuildStateData {
public:
	vector<PipelineExternalInputCandidate> external_input_candidates;
	vector<CTEPipelineSelection> cte_selections;
	reference_map_t<PhysicalCTE, idx_t> cte_selection_map;
	vector<PipelineBufferedConsumerRecord> buffered_consumers;
};

struct RemovedOptionalPipelineDependency {
	RemovedOptionalPipelineDependency(Pipeline &pipeline_p, Pipeline &dependency_p)
	    : pipeline(pipeline_p), dependency(dependency_p) {
	}

	reference<Pipeline> pipeline;
	reference<Pipeline> dependency;
};

static bool RemoveOptionalDependencyInCycle(const PipelineSchedule &schedule, const vector<PipelineScheduleEdge> &cycle,
                                            vector<RemovedOptionalPipelineDependency> &removed_dependencies) {
	for (auto &edge : cycle) {
		auto &pipeline_stage = schedule.stages[edge.dependent];
		auto &dependency_stage = schedule.stages[edge.dependency];
		if (pipeline_stage.type != PipelineScheduleStageType::EXECUTE ||
		    dependency_stage.type != PipelineScheduleStageType::EXECUTE) {
			continue;
		}
		if (MetaPipeline::RemoveOptionalDependency(*pipeline_stage.pipeline, *dependency_stage.pipeline)) {
			removed_dependencies.emplace_back(*pipeline_stage.pipeline, *dependency_stage.pipeline);
			return true;
		}
	}
	return false;
}

static void RestoreOptionalDependencies(vector<RemovedOptionalPipelineDependency> &dependencies) {
	for (auto &dependency : dependencies) {
		MetaPipeline::AddOptionalDependency(dependency.pipeline, dependency.dependency);
	}
	dependencies.clear();
}

static idx_t SaturatingAdd(idx_t left, idx_t right) {
	auto maximum = NumericLimits<idx_t>::Maximum();
	return right > maximum - left ? maximum : left + right;
}

static idx_t EstimateMaterializationSize(const PhysicalCTE &cte) {
	D_ASSERT(!cte.children.empty());
	auto &producer = cte.children[0].get();
	idx_t row_width = 0;
	for (auto &type : producer.GetTypes()) {
		row_width = SaturatingAdd(row_width, MaxValue<idx_t>(GetTypeIdSize(type.InternalType()), 1));
	}
	if (row_width == 0 || producer.estimated_cardinality == 0) {
		return 0;
	}
	auto maximum = NumericLimits<idx_t>::Maximum();
	return producer.estimated_cardinality > maximum / row_width ? maximum : producer.estimated_cardinality * row_width;
}

static void SelectExternalInputCandidates(PipelineBuildStateData &data) {
	for (auto &candidate : data.external_input_candidates) {
		auto selection_entry = data.cte_selection_map.find(candidate.cte.get());
		D_ASSERT(selection_entry != data.cte_selection_map.end());
		candidate.selection_idx = selection_entry->second;
		auto &selection = data.cte_selections[candidate.selection_idx];
		selection.candidate_count++;
		if (candidate.input_cost == PipelineExternalInputCost::SERIALIZED_FANOUT) {
			selection.serialized_fanout_candidate_count++;
		}
	}

	vector<idx_t> fanout_selections;
	for (idx_t selection_idx = 0; selection_idx < data.cte_selections.size(); selection_idx++) {
		auto &selection = data.cte_selections[selection_idx];
		auto &cte = selection.cte.get();
		D_ASSERT(cte.exchange);
		auto consumer_summary = cte.exchange->GetConsumerSummary();
		D_ASSERT(consumer_summary.unresolved == selection.candidate_count);
		if (selection.serialized_fanout_candidate_count == 0) {
			continue;
		}
		// An existing materialized consumer already makes the materialization cost unavoidable.
		if (consumer_summary.materialized > 0) {
			continue;
		}
		// A single consumer cannot introduce serialized fanout.
		if (selection.candidate_count == 1 && consumer_summary.ExchangeConsumerCount() == 0) {
			selection.stream_serialized_candidates = true;
			continue;
		}
		selection.estimated_materialization_size = EstimateMaterializationSize(cte);
		fanout_selections.push_back(selection_idx);
	}

	// Direct fanout trades materialization memory for serialized pushes into every consumer. Keep small fanouts
	// materialized, and stream the largest CTEs when materializing all eligible inputs would consume too much memory.
	auto &context = data.external_input_candidates[0].pipeline.get().GetClientContext();
	auto materialization_budget = BufferManager::GetBufferManager(context).GetOperatorMemoryLimit() / 2;
	std::sort(fanout_selections.begin(), fanout_selections.end(), [&](idx_t left, idx_t right) {
		return data.cte_selections[left].estimated_materialization_size <
		       data.cte_selections[right].estimated_materialization_size;
	});
	idx_t materialization_size = 0;
	for (auto selection_idx : fanout_selections) {
		auto &selection = data.cte_selections[selection_idx];
		if (selection.estimated_materialization_size <= materialization_budget - materialization_size) {
			materialization_size += selection.estimated_materialization_size;
		} else {
			selection.stream_serialized_candidates = true;
		}
	}

	for (auto &candidate : data.external_input_candidates) {
		auto &selection = data.cte_selections[candidate.selection_idx];
		if (candidate.input_cost == PipelineExternalInputCost::PIPELINED || selection.stream_serialized_candidates) {
			candidate.input_mode = PipelineExternalInputCandidate::InputMode::EXTERNAL_INPUT;
		}
	}
}

static optional_ptr<PipelineExternalInputCandidate>
FindExternalInputCandidateInCycle(PipelineBuildStateData &data, const vector<PipelineScheduleEdge> &cycle) {
	reference_set_t<const Pipeline> cycle_consumers;
	for (auto &edge : cycle) {
		if (edge.external_input_consumer) {
			cycle_consumers.insert(*edge.external_input_consumer);
		}
	}
	optional_ptr<PipelineExternalInputCandidate> result;
	for (auto &candidate : data.external_input_candidates) {
		if (!candidate.UsesExternalInput()) {
			continue;
		}
		if (cycle_consumers.find(candidate.pipeline.get()) == cycle_consumers.end()) {
			continue;
		}
		if (candidate.has_blocking_dependencies) {
			return candidate;
		}
		result = candidate;
	}
	return result;
}

PipelineBuildState::PipelineBuildState() : data(make_uniq<PipelineBuildStateData>()) {
}

PipelineBuildState::~PipelineBuildState() = default;

void PipelineBuildState::SetPipelineSource(Pipeline &pipeline, PhysicalOperator &op) {
	pipeline.source = &op;
}

void PipelineBuildState::SetPipelineSink(Pipeline &pipeline, optional_ptr<PhysicalOperator> op,
                                         idx_t sink_pipeline_count) {
	pipeline.sink = op;
	// set the base batch index of this pipeline based on how many other pipelines have this node as their sink
	pipeline.base_batch_index = BATCH_INCREMENT * sink_pipeline_count;
}

void PipelineBuildState::AddPipelineOperator(Pipeline &pipeline, PhysicalOperator &op) {
	pipeline.operators.push_back(op);
}

optional_ptr<PhysicalOperator> PipelineBuildState::GetPipelineSource(Pipeline &pipeline) {
	return pipeline.source;
}

optional_ptr<PhysicalOperator> PipelineBuildState::GetPipelineSink(Pipeline &pipeline) {
	return pipeline.sink;
}

void PipelineBuildState::SetPipelineOperators(Pipeline &pipeline, vector<reference<PhysicalOperator>> operators) {
	pipeline.operators = std::move(operators);
}

shared_ptr<Pipeline> PipelineBuildState::CreateChildPipeline(Executor &executor, Pipeline &pipeline,
                                                             PhysicalOperator &op) {
	return executor.CreateChildPipeline(pipeline, op);
}

vector<reference<PhysicalOperator>> PipelineBuildState::GetPipelineOperators(Pipeline &pipeline) {
	return pipeline.operators;
}

void PipelineBuildState::AddExternalInputCandidate(Pipeline &pipeline, PhysicalOperator &materialized_source,
                                                   PhysicalOperator &external_source,
                                                   shared_ptr<Pipeline> cte_dependency, PhysicalCTE &cte,
                                                   idx_t consumer_idx) {
	data->external_input_candidates.emplace_back(pipeline, materialized_source, external_source,
	                                             std::move(cte_dependency), cte, consumer_idx);
}

void PipelineBuildState::AddBufferedConsumer(Pipeline &pipeline, PhysicalOperator &materialized_source,
                                             shared_ptr<Pipeline> cte_dependency, PhysicalCTE &cte,
                                             idx_t consumer_idx) {
	data->buffered_consumers.emplace_back(pipeline, materialized_source, std::move(cte_dependency), cte, consumer_idx);
}

//! Deadlock avoidance for buffered exchange consumers. An exchange producer throttles once the exchange buffer is
//! full. It can only progress when a direct consumer runs it, or when a buffered consumer is draining. A buffered
//! consumer drains when it can start without waiting on the exchange's own progress, and when its sink chain is
//! itself live. This computes a least fixed point of stage and exchange liveness over the scheduled and analysis
//! dependencies and returns a buffered consumer of an exchange that cannot be proven live, if any. Such a consumer
//! is demoted to the materialized working table, which removes the producer's dependence on it.
static optional_ptr<PipelineBufferedConsumerRecord>
FindExchangeStarvedBufferedConsumer(PipelineBuildStateData &data, const PipelineSchedule &schedule) {
	struct ExchangeLiveness {
		vector<reference<PipelineBufferedConsumerRecord>> records;
		bool has_direct = false;
		//! The producers can run and the buffered consumers can drain, so the producers are never parked forever.
		bool live = false;
		//! The exchange can also deliver EOF: live, and every producer's own dataflow inputs can finish. An EOF
		//! wait must only be discharged by this, never by liveness alone, because a live producer that consumes
		//! another exchange completes only once that exchange finishes.
		bool finishes = false;
	};
	reference_map_t<PhysicalCTE, ExchangeLiveness> exchanges;
	for (auto &record : data.buffered_consumers) {
		if (!record.demoted) {
			exchanges[record.cte.get()].records.push_back(record);
		}
	}
	if (exchanges.empty()) {
		return nullptr;
	}
	// An exchange with a direct consumer creates a shared spool at the buffer high watermark instead of
	// throttling, so its producer never waits on the buffered consumers.
	for (auto &entry : exchanges) {
		auto &cte = entry.first.get();
		D_ASSERT(cte.exchange);
		entry.second.has_direct = cte.exchange->GetConsumerSummary().direct > 0;
	}
	for (auto &candidate : data.external_input_candidates) {
		if (!candidate.UsesExternalInput()) {
			continue;
		}
		auto entry = exchanges.find(candidate.cte.get());
		if (entry != exchanges.end()) {
			entry->second.has_direct = true;
		}
	}

	auto stage_count = schedule.stages.size();
	// The exchange a stage's pipeline produces into, if that exchange has active buffered consumers.
	auto producing_exchange = [&](const PipelineScheduleStage &stage) -> optional_ptr<ExchangeLiveness> {
		auto sink = stage.pipeline->GetSink();
		if (!sink || sink->type != PhysicalOperatorType::CTE) {
			return nullptr;
		}
		auto entry = exchanges.find(sink->Cast<PhysicalCTE>());
		if (entry == exchanges.end()) {
			return nullptr;
		}
		return &entry->second;
	};
	// A consumer's execute only completes after its own producer finalized (the mirrored analysis edge), but for
	// liveness that wait resolves whenever the exchange itself is live, so it must not count as a gate. For two
	// ordered consumers of one exchange this exclusion is sound only together with the exchange's shared-spool
	// rule: with more than one active consumer the producer spools at the high watermark instead of throttling
	// (ShouldCreateSharedSpoolLocked), so the pinned later consumer cannot park the producer. Tightening that
	// spool rule would reintroduce an ordered-pair deadlock invisible to this analysis.
	auto is_own_producer_eof = [&](const PipelineScheduleStage &stage, idx_t dependency) {
		if (stage.type != PipelineScheduleStageType::EXECUTE) {
			return false;
		}
		auto &target = schedule.stages[dependency];
		if (target.type != PipelineScheduleStageType::FINISH) {
			return false;
		}
		for (auto &dataflow : stage.pipeline->GetDataflowDependencies()) {
			auto producer = dataflow.lock();
			if (producer && RefersToSameObject(*producer, *target.pipeline)) {
				return true;
			}
		}
		return false;
	};
	// Index the execute stages once: the stage array is scanned repeatedly below.
	reference_map_t<Pipeline, idx_t> execute_stages;
	vector<optional_ptr<ExchangeLiveness>> stage_producing_exchange(stage_count, nullptr);
	vector<pair<reference<ExchangeLiveness>, idx_t>> producer_execute_stages;
	for (idx_t stage_idx = 0; stage_idx < stage_count; stage_idx++) {
		auto &stage = schedule.stages[stage_idx];
		if (stage.type != PipelineScheduleStageType::EXECUTE) {
			continue;
		}
		execute_stages.emplace(*stage.pipeline, stage_idx);
		auto exchange = producing_exchange(stage);
		stage_producing_exchange[stage_idx] = exchange;
		if (exchange) {
			producer_execute_stages.emplace_back(*exchange, stage_idx);
		}
	}
	auto record_execute_stage = [&](const PipelineBufferedConsumerRecord &record) {
		auto entry = execute_stages.find(record.pipeline.get());
		return entry == execute_stages.end() ? optional_idx() : optional_idx(entry->second);
	};

	// The exchange each active dataflow producer pipeline feeds, for resolving EOF waits.
	reference_map_t<Pipeline, reference<ExchangeLiveness>> producer_pipeline_exchange;
	for (auto &record : data.buffered_consumers) {
		if (record.demoted) {
			continue;
		}
		auto entry = exchanges.find(record.cte.get());
		D_ASSERT(entry != exchanges.end());
		producer_pipeline_exchange.emplace(*record.cte_dependency, entry->second);
	}

	vector<bool> stage_live(stage_count, false);
	// An analysis dependency is the mirrored wait on a producer's finalization. It resolves only once the
	// producer's exchange can deliver EOF: exchange liveness alone proves the producer can run, not that it can
	// finish, because the producer may itself wait on another exchange's EOF. The finish stage's own liveness
	// must not be used either, since a dataflow consumer's execute is considered live without its EOF wait.
	auto dependencies_live = [&](const PipelineScheduleStage &stage) {
		for (auto dependency : stage.dependencies) {
			if (!stage_live[dependency]) {
				return false;
			}
		}
		for (auto dependency : stage.analysis_dependencies) {
			if (is_own_producer_eof(stage, dependency)) {
				continue;
			}
			auto target_exchange = producing_exchange(schedule.stages[dependency]);
			if (target_exchange) {
				if (!target_exchange->finishes) {
					return false;
				}
			} else if (!stage_live[dependency]) {
				return false;
			}
		}
		return true;
	};
	// The exchange's producers must be able to run: a live exchange with a producer that is itself gated would
	// wrongly resolve the finalization waits of its consumers. Ordering between the exchange's own producer
	// pipelines (a UNION's later branches wait on the base pipeline) resolves with the exchange itself and must
	// not count as a gate.
	auto producers_startable = [&](const ExchangeLiveness &exchange) {
		for (auto &entry : producer_execute_stages) {
			if (&entry.first.get() != &exchange) {
				continue;
			}
			auto &stage = schedule.stages[entry.second];
			for (auto dependency : stage.dependencies) {
				if (stage_live[dependency] || producing_exchange(schedule.stages[dependency]).get() == &exchange) {
					continue;
				}
				return false;
			}
			for (auto dependency : stage.analysis_dependencies) {
				if (stage_live[dependency] || is_own_producer_eof(stage, dependency)) {
					continue;
				}
				auto target_exchange = producing_exchange(schedule.stages[dependency]);
				if (target_exchange && (target_exchange->live || target_exchange.get() == &exchange)) {
					continue;
				}
				return false;
			}
		}
		return true;
	};

	bool changed = true;
	while (changed) {
		changed = false;
		for (auto &entry : exchanges) {
			auto &exchange = entry.second;
			if (exchange.live) {
				continue;
			}
			if (!producers_startable(exchange)) {
				continue;
			}
			bool all_draining = true;
			if (!exchange.has_direct) {
				// A direct consumer creates the shared spool; otherwise every buffered consumer must be able
				// to start (everything its execute waits on is live) and to drain (its own sink's exchange
				// is live).
				for (auto &record_ref : exchange.records) {
					auto &record = record_ref.get();
					auto execute_stage = record_execute_stage(record);
					if (!execute_stage.IsValid()) {
						continue;
					}
					if (!dependencies_live(schedule.stages[execute_stage.GetIndex()])) {
						all_draining = false;
						break;
					}
					// Only CTE exchange sinks are modeled as drain obstacles. Blocking batch sinks (batch
					// INSERT/COPY) cannot stall a consumer here because PhysicalUnion::BuildPipelines forces
					// ordering edges between sibling pipelines whenever the sink requires a batch index, which
					// makes that gate visible as scheduled dependencies.
					auto consumer_sink_exchange = stage_producing_exchange[execute_stage.GetIndex()];
					if (consumer_sink_exchange && !consumer_sink_exchange->live) {
						all_draining = false;
						break;
					}
				}
			}
			if (all_draining) {
				exchange.live = true;
				changed = true;
			}
		}
		for (auto &entry : exchanges) {
			auto &exchange = entry.second;
			if (!exchange.live || exchange.finishes) {
				continue;
			}
			// EOF requires every producer's own dataflow inputs to finish as well. CTE references are acyclic,
			// so this grounds out at exchanges whose producers read no other exchange.
			bool inputs_finish = true;
			for (auto &producer_entry : producer_execute_stages) {
				if (&producer_entry.first.get() != &exchange) {
					continue;
				}
				for (auto &dataflow : schedule.stages[producer_entry.second].pipeline->GetDataflowDependencies()) {
					auto producer = dataflow.lock();
					D_ASSERT(producer);
					auto upstream = producer_pipeline_exchange.find(*producer);
					if (upstream == producer_pipeline_exchange.end() || !upstream->second.get().finishes) {
						inputs_finish = false;
						break;
					}
				}
				if (!inputs_finish) {
					break;
				}
			}
			if (inputs_finish) {
				exchange.finishes = true;
				changed = true;
			}
		}
		for (idx_t stage_idx = 0; stage_idx < stage_count; stage_idx++) {
			if (stage_live[stage_idx]) {
				continue;
			}
			auto &stage = schedule.stages[stage_idx];
			if (!dependencies_live(stage)) {
				continue;
			}
			auto exchange = stage_producing_exchange[stage_idx];
			if (exchange && !exchange->live) {
				continue;
			}
			stage_live[stage_idx] = true;
			changed = true;
		}
	}

	// Select the victim in registration order: the exchange map is keyed by pointers, and an address-dependent
	// choice would make the demoted side of mutually gated exchanges differ across runs.
	optional_ptr<PipelineBufferedConsumerRecord> fallback;
	for (auto &record : data.buffered_consumers) {
		if (record.demoted) {
			continue;
		}
		auto entry = exchanges.find(record.cte.get());
		if (entry == exchanges.end() || entry->second.live) {
			continue;
		}
		// Prefer a consumer that cannot start, otherwise any consumer of a dead exchange.
		auto execute_stage = record_execute_stage(record);
		if (execute_stage.IsValid() && !dependencies_live(schedule.stages[execute_stage.GetIndex()])) {
			return &record;
		}
		if (!fallback) {
			fallback = &record;
		}
	}
	return fallback;
}

void PipelineBuildState::AddCTEPipelineSelection(PhysicalCTE &cte, Pipeline &pipeline,
                                                 shared_ptr<Pipeline> cte_dependency, bool dependency_added) {
	D_ASSERT(data->cte_selection_map.find(cte) == data->cte_selection_map.end());
	data->cte_selection_map.emplace(cte, data->cte_selections.size());
	data->cte_selections.emplace_back(cte, pipeline, std::move(cte_dependency), dependency_added);
}

void PipelineBuildState::ResolveExternalInputs(const vector<shared_ptr<MetaPipeline>> &meta_pipelines,
                                               bool model_runtime_filter_edges) {
	if (data->external_input_candidates.empty() && data->buffered_consumers.empty()) {
		return;
	}
	if (!data->external_input_candidates.empty()) {
		SelectExternalInputCandidates(*data);
	}
	auto activate_candidate = [&](PipelineExternalInputCandidate &candidate) {
		D_ASSERT(candidate.UsesExternalInput());
		auto &pipeline = candidate.pipeline.get();
		pipeline.RemoveDependency(candidate.cte_dependency);
		SetPipelineSource(pipeline, candidate.external_source.get());
		pipeline.SetExternalInput(candidate.cte.get().GetProducerPipelines());
		data->cte_selections[candidate.selection_idx].direct_candidate_count++;
	};
	auto restore_materialized_candidate = [&](PipelineExternalInputCandidate &candidate) {
		D_ASSERT(candidate.UsesExternalInput());
		auto &pipeline = candidate.pipeline.get();
		pipeline.ClearExternalInput();
		SetPipelineSource(pipeline, candidate.materialized_source.get());
		pipeline.AddDependency(candidate.cte_dependency);
		candidate.input_mode = PipelineExternalInputCandidate::InputMode::MATERIALIZED_CYCLE;

		auto &selection = data->cte_selections[candidate.selection_idx];
		D_ASSERT(selection.direct_candidate_count > 0);
		selection.direct_candidate_count--;
		if (selection.direct_candidate_count == 0 && selection.fallback_dependency_added &&
		    !selection.fallback_dependency_active) {
			selection.pipeline.get().AddDependency(selection.cte_dependency);
			selection.fallback_dependency_active = true;
		}
	};
	for (auto &candidate : data->external_input_candidates) {
		if (candidate.UsesExternalInput()) {
			activate_candidate(candidate);
		}
	}
	for (auto &selection : data->cte_selections) {
		if (selection.direct_candidate_count == 0 || !selection.fallback_dependency_active) {
			continue;
		}
		selection.pipeline.get().RemoveDependency(selection.cte_dependency);
		selection.fallback_dependency_active = false;
	}

	auto demote_buffered_consumer = [&](PipelineBufferedConsumerRecord &record) {
		auto &pipeline = record.pipeline.get();
		pipeline.RemoveDataflowDependency(record.cte_dependency);
		SetPipelineSource(pipeline, record.materialized_source.get());
		pipeline.AddDependency(record.cte_dependency);
		record.cte.get().DemoteBufferedConsumer(record.consumer_idx);
		record.demoted = true;
		DUCKDB_LOG(pipeline.GetClientContext(), PhysicalOperatorLogType, record.cte.get(), "PhysicalCTE",
		           "SelectConsumer",
		           {{"consumer", to_string(record.consumer_idx)}, {"mode", "MATERIALIZED"}, {"reason", "GATED"}});
	};

	vector<RemovedOptionalPipelineDependency> removed_dependencies;
	while (true) {
		auto schedule = BuildPipelineSchedule(meta_pipelines);
		if (model_runtime_filter_edges) {
			// The executor adds runtime-filter ordering edges to the final schedule under the same condition
			// (Executor::ScheduleEventsInternal, initial schedule without recursive CTEs). Cycle detection and
			// the exchange liveness analysis must see those edges: an edge they never saw can gate a buffered
			// exchange consumer invisibly and deadlock the producer at the exchange watermark. The selection is
			// a deterministic function of the schedule graph, so the final iteration models exactly the edges
			// the executor will add. A modeled edge can also close a WITH_ANALYSIS cycle (TryAddDependency only
			// guards against scheduled cycles); such cycles resolve through candidate or starved demotion like
			// any other, or fail loudly below where the pre-fix behavior was a runtime deadlock.
			AddRuntimeFilterScheduleDependencies(*schedule);
		}
		auto cycle = schedule->GetCycle();
		if (cycle.empty()) {
			auto starved = FindExchangeStarvedBufferedConsumer(*data, *schedule);
			if (!starved) {
				break;
			}
			demote_buffered_consumer(*starved);
			RestoreOptionalDependencies(removed_dependencies);
			continue;
		}
		if (RemoveOptionalDependencyInCycle(*schedule, cycle, removed_dependencies)) {
			continue;
		}
		auto candidate = FindExternalInputCandidateInCycle(*data, cycle);
		if (!candidate) {
			throw InternalException("Cyclic dependency in pipeline schedule without an external input candidate");
		}
		restore_materialized_candidate(*candidate);
		RestoreOptionalDependencies(removed_dependencies);
	}

	for (auto &candidate : data->external_input_candidates) {
		auto &pipeline = candidate.pipeline.get();
		auto &cte = candidate.cte.get();
		if (candidate.UsesExternalInput()) {
			cte.RegisterDirectConsumer(pipeline, candidate.consumer_idx);
			DUCKDB_LOG(pipeline.GetClientContext(), PhysicalOperatorLogType, cte, "PhysicalCTE", "SelectConsumer",
			           {{"consumer", to_string(candidate.consumer_idx)}, {"mode", "DIRECT"}});
		} else {
			cte.RegisterMaterializedConsumer(candidate.consumer_idx);
			DUCKDB_LOG(pipeline.GetClientContext(), PhysicalOperatorLogType, cte, "PhysicalCTE", "SelectConsumer",
			           {{"consumer", to_string(candidate.consumer_idx)},
			            {"mode", "MATERIALIZED"},
			            {"reason", candidate.input_mode == PipelineExternalInputCandidate::InputMode::MATERIALIZED_CYCLE
			                           ? "CYCLE"
			                           : "COST"}});
		}
	}

	for (auto &selection : data->cte_selections) {
		selection.cte.get().SetPipelineSelectionResolved();
	}
#ifdef D_ASSERT_IS_ENABLED
	{
		auto final_schedule = BuildPipelineSchedule(meta_pipelines);
		if (model_runtime_filter_edges) {
			AddRuntimeFilterScheduleDependencies(*final_schedule);
		}
		D_ASSERT(!final_schedule->HasCycle());
	}
#endif
}

} // namespace duckdb
