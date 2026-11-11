#include "duckdb/execution/operator/persistent/physical_merge_into.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/parallel/pipeline_broadcast_exchange.hpp"
#include "duckdb/execution/operator/projection/physical_projection.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/execution/row_id_deduplicator.hpp"
#include "duckdb/parallel/meta_pipeline.hpp"
#include "duckdb/parallel/pipeline.hpp"
#include "duckdb/parallel/task_scheduler.hpp"
#include "duckdb/parser/statement/merge_into_statement.hpp"
#include "duckdb/parser/query_node/merge_query_node.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/common/types/vector.hpp"

namespace duckdb {

static string MergeActionName(MergeActionType action_type) {
	switch (action_type) {
	case MergeActionType::MERGE_UPDATE:
		return "UPDATE";
	case MergeActionType::MERGE_INSERT:
		return "INSERT";
	case MergeActionType::MERGE_DELETE:
		return "DELETE";
	case MergeActionType::MERGE_DO_NOTHING:
		return "DO NOTHING";
	case MergeActionType::MERGE_ERROR:
		return "ERROR";
	default:
		throw InternalException("Unsupported merge action");
	}
}

//===--------------------------------------------------------------------===//
// Merge Action Source
//===--------------------------------------------------------------------===//
PhysicalMergeActionSource::PhysicalMergeActionSource(PhysicalPlan &physical_plan, vector<LogicalType> types,
                                                     idx_t estimated_cardinality, MergeActionCondition condition,
                                                     MergeActionType action_type, bool parallel)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::MERGE_ACTION_SOURCE, std::move(types),
                       estimated_cardinality),
      condition(condition), action_type(action_type), parallel(parallel) {
}

PhysicalMergeActionSource::~PhysicalMergeActionSource() {
}

class MergeActionGlobalSourceState : public GlobalSourceState {
public:
	MergeActionGlobalSourceState(ClientContext &context, const PhysicalMergeActionSource &op)
	    : exchange(op.exchange), consumer_idx(op.consumer_idx) {
		if (!exchange) {
			throw InternalException("MERGE INTO action source has no exchange - pipelines have not been built");
		}
		max_threads = op.parallel ? exchange->MaxThreads() : 1;
	}

	~MergeActionGlobalSourceState() override {
		Unregister();
	}

	idx_t MaxThreads() override {
		return max_threads;
	}

	//! Signal that the pipeline of this action no longer reads from the exchange - the merge into must not block on
	//! it, and any rows that are pushed afterwards are discarded
	void Unregister() {
		if (unregistered.exchange(true)) {
			return;
		}
		exchange->UnregisterConsumer(consumer_idx);
	}

	shared_ptr<PipelineBroadcastExchange> exchange;
	idx_t consumer_idx;
	idx_t max_threads;
	atomic<bool> unregistered {false};
	//! The exchange total grows while the producer outruns its estimate - keeps the scan progress monotonic
	MonotonicProgress scan_progress;
};

class MergeActionLocalSourceState : public LocalSourceState {
public:
	explicit MergeActionLocalSourceState(shared_ptr<PipelineBroadcastExchangeScanState> scan_state_p)
	    : scan_state(std::move(scan_state_p)) {
	}

	shared_ptr<PipelineBroadcastExchangeScanState> scan_state;
	optional_idx exchange_batch_index;
};

unique_ptr<GlobalSourceState> PhysicalMergeActionSource::GetGlobalSourceState(ClientContext &context) const {
	return make_uniq<MergeActionGlobalSourceState>(context, *this);
}

unique_ptr<LocalSourceState> PhysicalMergeActionSource::GetLocalSourceState(ExecutionContext &context,
                                                                            GlobalSourceState &gstate) const {
	return make_uniq<MergeActionLocalSourceState>(exchange->GetScanState());
}

SourceResultType PhysicalMergeActionSource::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                            OperatorSourceInput &input) const {
	auto &gstate = input.global_state.Cast<MergeActionGlobalSourceState>();
	auto &lstate = input.local_state.Cast<MergeActionLocalSourceState>();
	return gstate.exchange->Scan(gstate.consumer_idx, chunk, *lstate.scan_state, lstate.exchange_batch_index,
	                             input.batch_index_state, input.interrupt_state);
}

ProgressData PhysicalMergeActionSource::GetProgress(ClientContext &context, GlobalSourceState &gstate_p) const {
	auto &gstate = gstate_p.Cast<MergeActionGlobalSourceState>();
	// we do not know how many rows the merge into will push into this action - the exchange reports the rows we have
	// consumed out of the rows that have been pushed so far
	return gstate.scan_progress.Update(gstate.exchange->ScanProgress(gstate.consumer_idx, estimated_cardinality));
}

void PhysicalMergeActionSource::SourceFinished(ClientContext &context, GlobalSourceState &gstate_p) const {
	gstate_p.Cast<MergeActionGlobalSourceState>().Unregister();
}

InsertionOrderPreservingMap<string> PhysicalMergeActionSource::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	result["Action"] = MergeQueryNode::ActionConditionToString(condition) + " THEN " + MergeActionName(action_type);
	SetEstimatedCardinality(result, estimated_cardinality);
	return result;
}

//===--------------------------------------------------------------------===//
// Merge Into
//===--------------------------------------------------------------------===//
PhysicalMergeInto::PhysicalMergeInto(PhysicalPlan &physical_plan, vector<LogicalType> types, PhysicalOperator &child,
                                     map<MergeActionCondition, vector<unique_ptr<MergeIntoOperator>>> actions_p,
                                     idx_t row_id_index, optional_idx source_marker, bool parallel_p,
                                     bool return_chunk_p, bool serialize_actions_p)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::MERGE_INTO, std::move(types), 1),
      row_id_index(row_id_index), source_marker(source_marker), parallel(parallel_p), return_chunk(return_chunk_p),
      serialize_actions(serialize_actions_p) {
	children.push_back(child);

	map<MergeActionCondition, MergeActionRange> ranges;
	for (auto &entry : actions_p) {
		MergeActionRange range;
		range.condition = entry.first;
		range.start = actions.size();
		for (auto &action : entry.second) {
			PlanAction(physical_plan, entry.first, *action);
			actions.push_back(std::move(action));
		}
		range.end = actions.size();
		ranges.emplace(entry.first, range);
	}
	match_actions = {MergeActionCondition::WHEN_MATCHED, MergeActionCondition::WHEN_NOT_MATCHED_BY_TARGET,
	                 MergeActionCondition::WHEN_NOT_MATCHED_BY_SOURCE};
	for (idx_t i = 0; i < match_actions.size(); i++) {
		auto entry = ranges.find(match_actions[i]);
		MergeActionRange range;
		if (entry != ranges.end()) {
			range = entry->second;
		}
		range.condition = match_actions[i];
		action_ranges.push_back(range);
	}
}

void PhysicalMergeInto::PlanAction(PhysicalPlan &physical_plan, MergeActionCondition condition,
                                   MergeIntoOperator &action) {
	if (!action.op) {
		// no operator to push data into (DO NOTHING/ERROR) - handled by the merge into directly
		return;
	}
	action_pipeline_count++;

	auto &input = children[0].get();
	if (!action.source) {
		// the catalog planned the operators of this action without a merge action source - insert one
		// FIXME: remove this once all catalogs plan their actions through PlanMergeActionSource
		auto &source = physical_plan.Make<PhysicalMergeActionSource>(input.types, input.estimated_cardinality,
		                                                             condition, action.action_type, parallel);
		action.source = source.Cast<PhysicalMergeActionSource>();
		action.source->merge_input = input;

		reference<PhysicalOperator> action_input = source;
		if (!action.expressions.empty()) {
			// the action has expressions (e.g. the values of an INSERT) - execute them in a projection
			vector<LogicalType> projection_types;
			for (auto &expr : action.expressions) {
				projection_types.push_back(expr->GetReturnType());
			}
			auto &projection = physical_plan.Make<PhysicalProjection>(
			    std::move(projection_types), std::move(action.expressions), input.estimated_cardinality);
			projection.children.push_back(action_input);
			action.expressions.clear();
			action_input = projection;
		}
		if (action.op->children.empty()) {
			action.op->children.push_back(action_input);
		} else {
			// catalogs can plan their action operators with the merge input as their child - the data of an action
			// is pushed in by the merge into, so the merge action source takes its place
			action.op->children[0] = action_input;
		}
	}
	action.source->parallel = parallel;

	if (!return_chunk) {
		children.push_back(*action.op);
		return;
	}
	// for RETURNING the action operator emits the modified rows - project them together with the merge action
	vector<unique_ptr<Expression>> select_list;
	for (idx_t c = 0; c + 1 < types.size(); c++) {
		select_list.push_back(make_uniq<BoundReferenceExpression>(types[c], c));
	}
	select_list.push_back(make_uniq<BoundConstantExpression>(Value(MergeActionName(action.action_type))));
	auto &returning_projection =
	    physical_plan.Make<PhysicalProjection>(types, std::move(select_list), input.estimated_cardinality);
	returning_projection.children.push_back(*action.op);
	action.returning_projection = returning_projection;
	children.push_back(returning_projection);
}

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//
struct MergeSinkState {
	MergeSinkState() : selected_sel(STANDARD_VECTOR_SIZE), remaining_sel(STANDARD_VECTOR_SIZE) {
	}

	bool computed_matches = false;
	bool match_initialized = false;
	idx_t match_idx = 0;
	idx_t index_in_match = 0;
	SelectionVector current_sel;
	SelectionVector selected_sel;
	SelectionVector remaining_sel;
	idx_t current_count;
	unique_ptr<DataChunk> sliced_chunk;
	optional_ptr<DataChunk> input_chunk;
};

//! Per-action state - the expression executors of the action
struct MergeActionLocalState {
	unique_ptr<ExpressionExecutor> condition_executor;
	unique_ptr<ExpressionExecutor> expression_executor;
	unique_ptr<DataChunk> expression_chunk;
};

struct MatchResult {
	MatchResult(ClientContext &context, const vector<LogicalType> &types) : sel(STANDARD_VECTOR_SIZE), count(0) {
		chunk = make_uniq<DataChunk>();
		chunk->Initialize(context, types);
	}

	SelectionVector sel;
	idx_t count;
	unique_ptr<DataChunk> chunk;
};

class MergeIntoLocalState : public LocalSinkState {
public:
	MergeIntoLocalState(ExecutionContext &context, const PhysicalMergeInto &op) {
		for (auto &action : op.actions) {
			MergeActionLocalState state;
			if (action->condition) {
				state.condition_executor = make_uniq<ExpressionExecutor>(context.client, *action->condition);
			}
			if (!action->expressions.empty()) {
				state.expression_executor = make_uniq<ExpressionExecutor>(context.client, action->expressions);
				vector<LogicalType> expression_types;
				for (auto &expr : action->expressions) {
					expression_types.push_back(expr->GetReturnType());
				}
				state.expression_chunk = make_uniq<DataChunk>();
				state.expression_chunk->Initialize(context.client, expression_types);
			}
			states.push_back(std::move(state));
			// the state that this thread pushes the rows of the action into its exchange with
			auto &source = action->source;
			exchange_states.push_back(
			    source && source->exchange
			        ? source->exchange->GetLocalState(context.client, context.pipeline->GetBaseBatchIndex())
			        : nullptr);
		}
		for (idx_t i = 0; i < 3; i++) {
			match_results.emplace_back(context.client, op.children[0].get().types);
		}
	}

	MergeSinkState sink_state;
	vector<MatchResult> match_results;
	vector<MergeActionLocalState> states;
	//! Per-action exchange state - NULL for actions that have no operator to push data into
	vector<unique_ptr<PipelineBroadcastExchangeLocalState>> exchange_states;
	idx_t merged_count = 0;

public:
	//! Compute the input chunk of the given action - evaluating the action condition and any expressions
	optional_ptr<DataChunk> ComputeActionInput(ClientContext &context, MergeIntoOperator &action, DataChunk &chunk,
	                                           MergeActionLocalState &action_state) {
		auto &current_count = sink_state.current_count;
		auto &current_sel = sink_state.current_sel;
		auto &sliced_chunk = sink_state.sliced_chunk;
		auto &selected_sel = sink_state.selected_sel;
		auto &remaining_sel = sink_state.remaining_sel;
		if (current_count == 0) {
			return nullptr;
		}
		if (!sliced_chunk) {
			sliced_chunk = make_uniq<DataChunk>();
			sliced_chunk->Initialize(context, chunk.GetTypes());
		} else {
			sliced_chunk->Reset();
		}
		optional_ptr<DataChunk> result;
		if (action.condition) {
			// if we have a condition we need to evaluate it
			auto &executor = *action_state.condition_executor;
			idx_t match_count =
			    executor.SelectExpression(chunk, selected_sel, remaining_sel, current_sel, current_count);
			if (match_count == 0) {
				// no matches - move to next action
				return nullptr;
			}
			// slice the chunk for this action with the matching sel
			sliced_chunk->Slice(chunk, selected_sel, match_count);
			result = sliced_chunk;

			// for the next chunk - update the matches
			current_count = current_count - match_count;
			current_sel.Initialize(remaining_sel);
		} else if (current_count != chunk.size()) {
			// if we have previously processed rows - remove them
			sliced_chunk->Slice(chunk, current_sel, current_count);
			result = sliced_chunk;
		} else {
			result = chunk;
		}
		// if we have any expressions - execute them to generate the new input chunk
		if (action_state.expression_executor) {
			auto &expression_chunk = action_state.expression_chunk;
			expression_chunk->Reset();
			action_state.expression_executor->Execute(*result, *expression_chunk);
			result = expression_chunk.get();
		}
		if (action.op) {
			merged_count += result->size();
		}
		return result;
	}
};

class MergeIntoGlobalState : public GlobalSinkState {
public:
	MergeIntoGlobalState(ClientContext &context, const PhysicalMergeInto &op)
	    : op(op), matched_rows(context, GetRowIdTypes(op)) {
		merged_count = 0;
		for (auto &action : op.actions) {
			shared_ptr<PipelineBroadcastExchange> exchange;
			if (action->source) {
				exchange = action->source->exchange;
				if (!exchange) {
					throw InternalException("MERGE INTO action has no exchange - pipelines have not been built");
				}
			}
			exchanges.push_back(std::move(exchange));
		}
	}

	~MergeIntoGlobalState() override {
		// never leave a consumer waiting for data that is no longer coming
		for (auto &exchange : exchanges) {
			if (exchange) {
				exchange->Cancel();
			}
		}
	}

	static vector<LogicalType> GetRowIdTypes(const PhysicalMergeInto &op) {
		auto &input_types = op.children[0].get().types;
		if (op.row_id_index >= input_types.size()) {
			throw InternalException("MERGE row ID index is out of range");
		}
		auto row_id_offset = NumericCast<vector<LogicalType>::difference_type>(op.row_id_index);
		return vector<LogicalType>(input_types.begin() + row_id_offset, input_types.end());
	}

	const PhysicalMergeInto &op;
	//! The exchange of every action - NULL for actions that have no operator to push data into
	vector<shared_ptr<PipelineBroadcastExchange>> exchanges;
	atomic<idx_t> merged_count;
	//! Target row-ids already matched by a WHEN MATCHED modifying action, to detect a second action on a row.
	mutex match_lock;
	RowIdDeduplicator matched_rows;

	//! Record the matched target rows; throw if any row was already matched (cardinality violation). A distinct
	//! count below the input count means a row ID repeated.
	void CheckMatchedRows(DataChunk &matched, idx_t row_id_index) {
		lock_guard<mutex> glock(match_lock);
		auto distinct = matched_rows.Register(matched, row_id_index);
		if (distinct != matched.size()) {
			throw InvalidInputException(
			    "MERGE INTO command cannot affect the same target row more than once. A target row matched more "
			    "than one source row; ensure the source rows are deduplicated or the ON condition is unique.");
		}
	}

	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, MergeIntoLocalState &local_state,
	                    OperatorSinkInput &input, MergeActionRange range, idx_t &index_in_match) {
		auto &input_chunk = local_state.sink_state.input_chunk;
		for (; range.start + index_in_match < range.end; index_in_match++) {
			idx_t i = range.start + index_in_match;
			auto &action = op.actions[i];
			auto &action_state = local_state.states[i];
			if (!input_chunk) {
				// first time processing this action - compute the input chunk
				input_chunk = local_state.ComputeActionInput(context.client, *action, chunk, action_state);
				if (!input_chunk) {
					// no data for this action - move to next action
					continue;
				}
				// A WHEN MATCHED update/delete must not affect the same target row twice (cardinality violation).
				// Checked here, on the freshly condition-selected rows (which still carry the row-id column), so
				// rows filtered out by the action condition are not counted - matching PostgreSQL. Runs once per
				// input chunk: on a BLOCKED resume input_chunk is still set and we skip re-checking.
				if (range.condition == MergeActionCondition::WHEN_MATCHED &&
				    (action->action_type == MergeActionType::MERGE_UPDATE ||
				     action->action_type == MergeActionType::MERGE_DELETE)) {
					CheckMatchedRows(*input_chunk, op.row_id_index);
				}
			}
			// process the action
			if (!action->op) {
				if (action->action_type == MergeActionType::MERGE_ERROR) {
					// abort - generate an error message
					string merge_condition;
					merge_condition += MergeQueryNode::ActionConditionToString(range.condition);
					if (action->condition) {
						merge_condition += " AND " + action->condition->ToString();
					}
					if (action_state.expression_executor) {
						// if there are any user-provided error messages: add the first error message encountered
						merge_condition += ": " + input_chunk->data[0].GetValue(0).ToString();
					}
					throw ConstraintException("Merge error condition %s", merge_condition);
				}
				D_ASSERT(action->action_type == MergeActionType::MERGE_DO_NOTHING);
				input_chunk = nullptr;
				continue;
			}
			// push the data into the exchange of this action - the pipeline of the action consumes it from there
			auto &exchange_state = *local_state.exchange_states[i];
			auto result =
			    exchanges[i]->Push(*input_chunk, exchange_state, local_state.partition_info, input.interrupt_state);
			if (result == SinkResultType::BLOCKED) {
				return SinkResultType::BLOCKED;
			}
			// move to next action
			input_chunk = nullptr;
		}
		return SinkResultType::NEED_MORE_INPUT;
	}
};

unique_ptr<GlobalSinkState> PhysicalMergeInto::GetGlobalSinkState(ClientContext &context) const {
	return make_uniq<MergeIntoGlobalState>(context, *this);
}

unique_ptr<LocalSinkState> PhysicalMergeInto::GetLocalSinkState(ExecutionContext &context) const {
	return make_uniq<MergeIntoLocalState>(context, *this);
}

idx_t PhysicalMergeInto::GetIndex(MergeActionCondition condition) const {
	for (idx_t i = 0; i < match_actions.size(); ++i) {
		if (match_actions[i] == condition) {
			return i;
		}
	}
	throw InternalException("Unsupported match action condition");
}

void PhysicalMergeInto::ComputeMatches(MergeIntoLocalState &local_state, DataChunk &chunk) const {
	auto &match_results = local_state.match_results;

	// for each row, figure out if we have generated a match or not
	auto &matched = match_results[0];
	auto &not_matched = match_results[1];
	auto &not_matched_by_source = match_results[2];

	matched.count = 0;
	not_matched.count = 0;
	not_matched_by_source.count = 0;

	// The first row-ID component is also the target-presence marker and must be non-NULL for target rows.
	auto row_id_validity = chunk.data[row_id_index].Validity();
	if (source_marker.IsValid()) {
		// source marker - check both row id and source marker
		auto source_marker_validity = chunk.data[source_marker.GetIndex()].Validity();
		for (idx_t i = 0; i < chunk.size(); i++) {
			if (!source_marker_validity.IsValid(i)) {
				// source marker is NULL - no source match
				not_matched_by_source.sel.set_index(not_matched_by_source.count++, i);
			} else if (!row_id_validity.IsValid(i)) {
				// target marker is NULL - no target match
				not_matched.sel.set_index(not_matched.count++, i);
			} else {
				// match
				matched.sel.set_index(matched.count++, i);
			}
		}
	} else {
		// no source marker - only check row-ids
		for (idx_t i = 0; i < chunk.size(); i++) {
			if (row_id_validity.IsValid(i)) {
				// match
				matched.sel.set_index(matched.count++, i);
			} else {
				// no match
				not_matched.sel.set_index(not_matched.count++, i);
			}
		}
	}

	// reset and slice chunks
	for (auto &match : match_results) {
		if (match.count == 0) {
			continue;
		}
		match.chunk->Reset();
		match.chunk->Slice(chunk, match.sel, match.count);
	}
}

SinkResultType PhysicalMergeInto::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
	auto &global_state = input.global_state.Cast<MergeIntoGlobalState>();
	auto &local_state = input.local_state.Cast<MergeIntoLocalState>();

	auto &match_results = local_state.match_results;
	auto &computed_matches = local_state.sink_state.computed_matches;
	auto &match_idx = local_state.sink_state.match_idx;
	auto &index_in_match = local_state.sink_state.index_in_match;
	auto &match_initialized = local_state.sink_state.match_initialized;
	auto &current_sel = local_state.sink_state.current_sel;
	auto &current_count = local_state.sink_state.current_count;
	if (!computed_matches) {
		// we haven't figured out which rows have which types of matches - compute them
		ComputeMatches(local_state, chunk);

		// set up the state so we can prepare sinking into the relevant operators
		computed_matches = true;
		match_idx = 0;
		index_in_match = 0;
		match_initialized = false;
	}
	// now slice and call sink for each of the match conditions
	for (; match_idx < 3; match_idx++) {
		auto &match_result = match_results[match_idx];
		if (match_result.count == 0) {
			// no matches for this action
			continue;
		}
		if (!match_initialized) {
			current_sel = SelectionVector();
			current_count = match_result.count;
			match_initialized = true;
		}
		auto match_range_index = GetIndex(match_actions[match_idx]);
		auto result = global_state.Sink(context, *match_result.chunk, local_state, input,
		                                action_ranges[match_range_index], index_in_match);
		if (result == SinkResultType::BLOCKED) {
			return SinkResultType::BLOCKED;
		}
		// move to next match action
		index_in_match = 0;
		match_initialized = false;
	}
	// finished - prepare for next match
	computed_matches = false;
	return SinkResultType::NEED_MORE_INPUT;
}

SinkCombineResultType PhysicalMergeInto::Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const {
	auto &global_state = input.global_state.Cast<MergeIntoGlobalState>();
	auto &local_state = input.local_state.Cast<MergeIntoLocalState>();

	global_state.merged_count += local_state.merged_count;
	local_state.merged_count = 0;
	return SinkCombineResultType::FINISHED;
}

SinkFinalizeType PhysicalMergeInto::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                                             OperatorSinkFinalizeInput &input) const {
	auto &global_state = input.global_state.Cast<MergeIntoGlobalState>();
	// all data has been pushed into the exchanges of the actions - their pipelines can finish up
	for (auto &exchange : global_state.exchanges) {
		if (exchange) {
			exchange->Finish();
		}
	}
	return SinkFinalizeType::READY;
}

//===--------------------------------------------------------------------===//
// Source
//===--------------------------------------------------------------------===//
SourceResultType PhysicalMergeInto::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                    OperatorSourceInput &input) const {
	if (return_chunk) {
		// the merge into is only a source if there are no actions that emit any data
		return SourceResultType::FINISHED;
	}
	auto &g = sink_state->Cast<MergeIntoGlobalState>();
	chunk.data[0].Append(Value::BIGINT(NumericCast<int64_t>(g.merged_count.load())));
	return SourceResultType::FINISHED;
}

//===--------------------------------------------------------------------===//
// Pipeline Construction
//===--------------------------------------------------------------------===//
void PhysicalMergeInto::BuildPipelines(Pipeline &current, MetaPipeline &meta_pipeline) {
	op_state.reset();
	sink_state.reset();

	auto &state = meta_pipeline.GetState();
	auto &context = current.GetClientContext();

	// set up the exchanges that connect the merge into to the pipelines of the actions - the rows of an action are
	// routed to its pipeline by pushing them into the exchange of that action, which its source scans
	// the merge into blocks if the pipeline of an action cannot keep up. if the actions must run one after the other
	// only the first action can stream: the rows of the other actions are buffered (spilling to disk if required) so
	// that the merge into can always finish pushing them
	bool first_action = true;
	for (auto &action : actions) {
		if (!action->source) {
			continue;
		}
		auto buffer_mode = serialize_actions && !first_action ? PipelineBroadcastExchangeBufferMode::BUFFER_ALL
		                                                      : PipelineBroadcastExchangeBufferMode::THROTTLE_PRODUCER;
		auto exchange = make_shared_ptr<PipelineBroadcastExchange>(
		    context, action->source->types, PipelineBroadcastExchangeCompletionMode::RUN_TO_COMPLETION,
		    OrderPreservationType::NO_ORDER, /*use_batch_index=*/false, buffer_mode);
		// every action has an exchange of its own - the source of the action is its only consumer
		action->source->consumer_idx = exchange->RegisterConsumer();
		exchange->SelectBufferedConsumer(action->source->consumer_idx, PipelineBroadcastExchangeScanMode::CHUNK);
		exchange->SetLogOperator(*action->source);
		action->source->exchange = std::move(exchange);
		first_action = false;
	}

	// build the pipelines that push data into the merge into
	auto &child_meta_pipeline = meta_pipeline.CreateChildMetaPipeline(current, *this);
	child_meta_pipeline.Build(children[0].get());

	// with RETURNING every action emits its own set of rows - create a pipeline per action to emit them
	vector<reference<Pipeline>> result_pipelines;
	if (IsSource()) {
		state.SetPipelineSource(current, *this);
	} else {
		result_pipelines.push_back(current);
		for (idx_t i = 1; i < action_pipeline_count; i++) {
			// the actions emit their rows in order - the union pipelines run after the current pipeline
			auto &union_pipeline = meta_pipeline.CreateUnionPipeline(current, true);
			meta_pipeline.AssignNextBatchIndex(union_pipeline);
			result_pipelines.push_back(union_pipeline);
		}
	}

	// build the pipelines of the actions - these consume the data that the merge into pushes into their queue, and
	// run concurrently with the pipelines that feed the merge into
	idx_t action_idx = 0;
	optional_ptr<shared_ptr<Pipeline>> previous_action_pipeline;
	for (auto &action : actions) {
		if (!action->op) {
			continue;
		}
		action->op->op_state.reset();
		action->op->sink_state.reset();
		auto &action_meta_pipeline = meta_pipeline.CreateChildMetaPipeline(current, *action->op);
		action_meta_pipeline.Build(action->op->children[0].get());
		auto &action_pipeline = action_meta_pipeline.GetBasePipeline();
		if (serialize_actions && previous_action_pipeline) {
			// the operators of the actions cannot append to the table concurrently - chain the pipelines
			action_pipeline->AddDependency(*previous_action_pipeline);
		}
		previous_action_pipeline = action_pipeline;

		if (!result_pipelines.empty()) {
			// emit the RETURNING data of this action - the action operator is the source of the result pipeline
			auto &result_pipeline = result_pipelines[action_idx].get();
			state.AddPipelineOperator(result_pipeline, *action->returning_projection);
			state.SetPipelineSource(result_pipeline, *action->op);
			if (action_idx > 0) {
				// only the current pipeline gets this dependency through CreateChildMetaPipeline
				result_pipeline.AddDependency(action_pipeline);
			}
		}
		action_idx++;
	}
}

vector<const_reference<PhysicalOperator>> PhysicalMergeInto::GetSources() const {
	if (IsSource()) {
		return {*this};
	}
	vector<const_reference<PhysicalOperator>> result;
	for (auto &action : actions) {
		if (!action->op) {
			continue;
		}
		result.push_back(*action->op);
	}
	return result;
}

} // namespace duckdb
