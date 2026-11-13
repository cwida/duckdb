#include "duckdb/execution/operator/join/physical_nested_loop_join.hpp"
#include "duckdb/parallel/thread_context.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/execution/nested_loop_join.hpp"
#include "duckdb/execution/mark_join_row_comparison.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/execution/operator/join/outer_join_marker.hpp"
#include "duckdb/common/types/vector_cache.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/constant_vector.hpp"

namespace duckdb {

PhysicalNestedLoopJoin::PhysicalNestedLoopJoin(PhysicalPlan &physical_plan, LogicalComparisonJoin &op,
                                               PhysicalOperator &left, PhysicalOperator &right,
                                               vector<JoinCondition> conds, JoinType join_type,
                                               idx_t estimated_cardinality,
                                               unique_ptr<JoinFilterPushdownInfo> pushdown_info_p)
    : PhysicalComparisonJoin(physical_plan, op, PhysicalOperatorType::NESTED_LOOP_JOIN, std::move(conds), join_type,
                             estimated_cardinality) {
	filter_pushdown = std::move(pushdown_info_p);
	D_ASSERT(join_type != JoinType::MARK || (conditions.size() == 1 && !predicate));
	track_unknown =
	    join_type == JoinType::MARK && (conditions[0].GetLHS().GetReturnType().id() == LogicalTypeId::TUPLE ||
	                                    conditions[0].GetComparisonType() == ExpressionType::COMPARE_DISTINCT_FROM);
	children.push_back(left);
	children.push_back(right);
	if (join_type == JoinType::MARK) {
		mark_projection_map = FillProjectionMap(left, op.left_projection_map);
	}
}

PhysicalNestedLoopJoin::PhysicalNestedLoopJoin(PhysicalPlan &physical_plan, LogicalComparisonJoin &op,
                                               PhysicalOperator &left, PhysicalOperator &right,
                                               vector<JoinCondition> cond, JoinType join_type,
                                               idx_t estimated_cardinality)
    : PhysicalNestedLoopJoin(physical_plan, op, left, right, std::move(cond), join_type, estimated_cardinality,
                             nullptr) {
}

bool PhysicalJoin::HasNullValues(DataChunk &chunk) {
	for (idx_t col_idx = 0; col_idx < chunk.ColumnCount(); col_idx++) {
		auto entries = chunk.data[col_idx].Validity();
		if (!entries.CanHaveNull()) {
			continue;
		}
		for (idx_t i = 0; i < chunk.size(); i++) {
			if (!entries.IsValid(i)) {
				return true;
			}
		}
	}
	return false;
}

template <bool MATCH>
static void ConstructSemiOrAntiJoinResult(DataChunk &left, DataChunk &result, bool found_match[]) {
	D_ASSERT(left.ColumnCount() == result.ColumnCount());
	// create the selection vector from the matches that were found
	SelectionVector sel(STANDARD_VECTOR_SIZE);
	optional<array_ptr<sel_t>> true_sel, false_sel;
	if (MATCH) {
		true_sel = array_ptr<sel_t>(sel.data(), sel.Capacity());
	} else {
		false_sel = array_ptr<sel_t>(sel.data(), sel.Capacity());
	}
	auto true_count = SelectionVector::SelectFromBoolean(found_match, nullptr, left.size(), true_sel, false_sel);
	auto result_count = MATCH ? true_count : left.size() - true_count;
	// construct the final result
	if (result_count > 0) {
		// we only return the columns on the left side
		// project them using the result selection vector
		// reference the columns of the left side from the result
		result.Slice(left, sel, result_count);
	}
}

void PhysicalJoin::ConstructSemiJoinResult(DataChunk &left, DataChunk &result, bool found_match[]) {
	ConstructSemiOrAntiJoinResult<true>(left, result, found_match);
}

void PhysicalJoin::ConstructAntiJoinResult(DataChunk &left, DataChunk &result, bool found_match[]) {
	ConstructSemiOrAntiJoinResult<false>(left, result, found_match);
}

void PhysicalJoin::ConstructMarkJoinResult(DataChunk &join_keys, DataChunk &left, DataChunk &result, bool found_match[],
                                           bool has_null, optional_ptr<const bool> found_unknown) {
	// for the initial set of columns we just reference the left side
	result.SetChildCardinality(left.size());
	for (idx_t i = 0; i < left.ColumnCount(); i++) {
		result.data[i].Reference(left.data[i]);
	}
	auto &mark_vector = result.data.back();
	mark_vector.SetVectorType(VectorType::FLAT_VECTOR);
	// first we set the NULL values from the join keys
	// if there is any NULL in the keys, the result is NULL
	auto bool_result = FlatVector::GetDataMutable<bool>(mark_vector);
	auto &mask = FlatVector::ValidityMutable(mark_vector);
	mask.SetAllValid(left.size());
	for (idx_t col_idx = 0; !found_unknown && col_idx < join_keys.ColumnCount(); col_idx++) {
		auto entries = join_keys.data[col_idx].Validity();
		if (!entries.CanHaveNull()) {
			continue;
		}
		for (idx_t i = 0; i < join_keys.size(); i++) {
			mask.Set(i, entries.IsValid(i));
		}
	}
	// now set the remaining entries to either true or false based on whether a match was found
	if (found_match) {
		for (idx_t i = 0; i < left.size(); i++) {
			bool_result[i] = found_match[i];
		}
	} else {
		memset(bool_result, 0, sizeof(bool) * left.size());
	}
	// if the right side contains NULL values, the result of any FALSE becomes NULL
	if (found_unknown) {
		for (idx_t i = 0; i < left.size(); i++) {
			if (!bool_result[i] && found_unknown.get()[i]) {
				mask.SetInvalid(i);
			}
		}
	} else if (has_null) {
		for (idx_t i = 0; i < left.size(); i++) {
			if (!bool_result[i]) {
				mask.SetInvalid(i);
			}
		}
	}
	FlatVector::SetSize(mark_vector, result.size());
}

bool PhysicalNestedLoopJoin::IsSupported(const vector<JoinCondition> &conditions, JoinType join_type) {
	if (join_type == JoinType::MARK) {
		return true;
	}
	idx_t comparison_count = 0;
	for (auto &cond : conditions) {
		if (!cond.IsComparison()) {
			continue;
		}
		if (cond.GetLHS().GetReturnType().InternalType() == PhysicalType::STRUCT ||
		    cond.GetLHS().GetReturnType().InternalType() == PhysicalType::LIST ||
		    cond.GetLHS().GetReturnType().InternalType() == PhysicalType::ARRAY) {
			return false;
		}
		comparison_count++;
	}
	if (comparison_count == 0) {
		// the other conditions are only evaluated on the pairs that match a comparison
		return false;
	}

	// To avoid situations like https://github.com/duckdb/duckdb/issues/10046
	// If there is an equality in the conditions, a hash join is planned
	// with one condition, we can use mark join logic, otherwise we should use physical blockwise nl join
	if (join_type == JoinType::SEMI || join_type == JoinType::ANTI) {
		return comparison_count == 1;
	}
	return true;
}

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//
class NestedLoopJoinGlobalState : public GlobalSinkState {
public:
	explicit NestedLoopJoinGlobalState(ClientContext &context, const PhysicalNestedLoopJoin &op)
	    : op(op), right_payload_data(context, op.children[1].get().GetTypes()),
	      right_condition_data(context, op.GetJoinTypes()), has_null(false),
	      right_outer(PropagatesBuildSide(op.join_type)) {
		ResetState(context);
	}

	const PhysicalNestedLoopJoin &op;
	mutex nj_lock;
	//! Materialized data of the RHS
	ColumnDataCollection right_payload_data;
	//! Materialized join condition of the RHS
	ColumnDataCollection right_condition_data;
	//! Whether or not the RHS of the nested loop join has NULL values
	atomic<bool> has_null;
	//! A bool indicating for each tuple in the RHS if they found a match (only used in FULL OUTER JOIN)
	OuterJoinMarker right_outer;
	//! Should we not bother pushing down filters?
	bool skip_filter_pushdown = false;
	//! The global filter states to push down (if any)
	unique_ptr<JoinFilterGlobalState> global_filter_state;

private:
	void ResetState(ClientContext &context) {
		right_payload_data.Reset();
		right_condition_data.Reset();
		has_null = false;
		right_outer.Reset();
		if (op.filter_pushdown) {
			skip_filter_pushdown = op.filter_pushdown->probe_info.empty();
			global_filter_state = op.filter_pushdown->GetGlobalState(context, op);
		} else {
			skip_filter_pushdown = false;
			global_filter_state.reset();
		}
		GlobalSinkState::Reset(context);
	}

public:
	bool SupportsReuse() const override {
		return true;
	}

	void Reset(ClientContext &context) override {
		ResetState(context);
	}
};

class NestedLoopJoinLocalState : public LocalSinkState {
public:
	explicit NestedLoopJoinLocalState(ExecutionContext &context, const PhysicalNestedLoopJoin &op,
	                                  NestedLoopJoinGlobalState &gstate)
	    : op(op), rhs_executor(context.client) {
		vector<LogicalType> condition_types;
		for (auto &cond : op.conditions) {
			rhs_executor.AddExpression(cond.GetRHS());
			condition_types.push_back(cond.GetRHS().GetReturnType());
		}
		right_condition.Initialize(Allocator::Get(context.client), condition_types);
		ResetState(gstate);
	}

	const PhysicalNestedLoopJoin &op;
	//! The chunk holding the right condition
	DataChunk right_condition;
	//! The executor of the RHS condition
	ExpressionExecutor rhs_executor;
	//! Local state for accumulating filter statistics
	unique_ptr<JoinFilterLocalState> local_filter_state;

private:
	void ResetState(NestedLoopJoinGlobalState &gstate) {
		right_condition.Reset();
		if (op.filter_pushdown) {
			local_filter_state = op.filter_pushdown->GetLocalState(*gstate.global_filter_state);
		} else {
			local_filter_state.reset();
		}
	}

public:
	bool SupportsReuse() const override {
		return true;
	}

	void Reset(ExecutionContext &context, GlobalSinkState &gstate_p) override {
		auto &gstate = gstate_p.Cast<NestedLoopJoinGlobalState>();
		ResetState(gstate);
	}
};

vector<LogicalType> PhysicalNestedLoopJoin::GetJoinTypes() const {
	vector<LogicalType> result;
	for (auto &cond : conditions) {
		result.push_back(cond.GetRHS().GetReturnType());
	}
	return result;
}

SinkResultType PhysicalNestedLoopJoin::Sink(ExecutionContext &context, DataChunk &chunk,
                                            OperatorSinkInput &input) const {
	auto &gstate = input.global_state.Cast<NestedLoopJoinGlobalState>();
	auto &lstate = input.local_state.Cast<NestedLoopJoinLocalState>();

	// resolve the join expression of the right side
	lstate.right_condition.Reset();
	lstate.rhs_executor.Execute(chunk, lstate.right_condition);

	if (filter_pushdown && !gstate.skip_filter_pushdown) {
		filter_pushdown->Sink(lstate.right_condition, *lstate.local_filter_state);
	}

	// if we have not seen any NULL values yet, and we are performing a MARK join, check if there are NULL values in
	// this chunk
	if (join_type == JoinType::MARK && !gstate.has_null) {
		if (HasNullValues(lstate.right_condition)) {
			gstate.has_null = true;
		}
	}

	// append the payload data and the conditions
	lock_guard<mutex> nj_guard(gstate.nj_lock);
	gstate.right_payload_data.Append(chunk);
	gstate.right_condition_data.Append(lstate.right_condition);
	return SinkResultType::NEED_MORE_INPUT;
}

SinkCombineResultType PhysicalNestedLoopJoin::Combine(ExecutionContext &context,
                                                      OperatorSinkCombineInput &input) const {
	auto &gstate = input.global_state.Cast<NestedLoopJoinGlobalState>();
	auto &lstate = input.local_state.Cast<NestedLoopJoinLocalState>();

	auto &client_profiler = QueryProfiler::Get(context.client);
	context.thread.profiler.Flush(*this);
	client_profiler.Flush(context.thread.profiler);
	if (filter_pushdown && !gstate.skip_filter_pushdown) {
		filter_pushdown->Combine(*gstate.global_filter_state, *lstate.local_filter_state);
	}
	return SinkCombineResultType::FINISHED;
}

SinkFinalizeType PhysicalNestedLoopJoin::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                                                  OperatorSinkFinalizeInput &input) const {
	auto &gsink = input.global_state.Cast<NestedLoopJoinGlobalState>();
	if (filter_pushdown && !gsink.skip_filter_pushdown) {
		(void)filter_pushdown->Finalize(context, *gsink.global_filter_state, *this);
	}

	gsink.right_outer.Initialize(gsink.right_payload_data.Count());
	if (gsink.right_payload_data.Count() == 0 && EmptyResultIfRHSIsEmpty()) {
		return SinkFinalizeType::NO_OUTPUT_POSSIBLE;
	}
	return SinkFinalizeType::READY;
}

unique_ptr<GlobalSinkState> PhysicalNestedLoopJoin::GetGlobalSinkState(ClientContext &context) const {
	return make_uniq<NestedLoopJoinGlobalState>(context, *this);
}

unique_ptr<LocalSinkState> PhysicalNestedLoopJoin::GetLocalSinkState(ExecutionContext &context) const {
	auto &gstate = sink_state->Cast<NestedLoopJoinGlobalState>();
	return make_uniq<NestedLoopJoinLocalState>(context, *this, gstate);
}

//===--------------------------------------------------------------------===//
// Operator
//===--------------------------------------------------------------------===//
class PhysicalNestedLoopJoinState : public CachingOperatorState {
public:
	PhysicalNestedLoopJoinState(ClientContext &context, const PhysicalNestedLoopJoin &op,
	                            const vector<JoinCondition> &conditions)
	    : lhs_executor(context), left_outer(IsLeftOuterJoin(op.join_type)), pred_executor(context) {
		vector<LogicalType> condition_types;
		for (auto &cond : conditions) {
			lhs_executor.AddExpression(cond.GetLHS());
			condition_types.push_back(cond.GetLHS().GetReturnType());
		}
		auto &allocator = Allocator::Get(context);
		left_condition.Initialize(allocator, condition_types);
		right_condition.Initialize(allocator, condition_types);
		right_payload.Initialize(allocator, op.children[1].get().GetTypes());
		left_outer.Initialize(STANDARD_VECTOR_SIZE);
		if (op.join_type == JoinType::MARK) {
			auto payload_types = op.GetTypes();
			payload_types.pop_back();
			mark_payload.InitializeEmpty(payload_types);
		}

		if (op.predicate) {
			pred_executor.AddExpression(*op.predicate);
			pred_matches.Initialize();
			auto predicate_types = op.children[0].get().GetTypes();
			for (auto &type : op.children[1].get().GetTypes()) {
				predicate_types.push_back(type);
			}
			pred_input.Initialize(allocator, predicate_types);
		}
		ResetState();
	}

	bool fetch_next_left;
	bool fetch_next_right;
	DataChunk left_condition;
	//! The executor of the LHS condition
	ExpressionExecutor lhs_executor;

	ColumnDataScanState condition_scan_state;
	ColumnDataScanState payload_scan_state;
	DataChunk right_condition;
	DataChunk right_payload;

	idx_t left_tuple;
	idx_t right_tuple;

	OuterJoinMarker left_outer;

	//! Predicate
	ExpressionExecutor pred_executor;
	SelectionVector pred_matches;
	DataChunk pred_input;
	DataChunk mark_payload;

private:
	void ResetState() {
		ResetCachingState();
		fetch_next_left = true;
		fetch_next_right = false;
		left_condition.Reset();
		condition_scan_state = ColumnDataScanState();
		payload_scan_state = ColumnDataScanState();
		right_condition.Reset();
		right_payload.Reset();
		pred_input.Reset();
		mark_payload.Reset();
		left_tuple = 0;
		right_tuple = 0;
		left_outer.Reset();
	}

public:
	void Finalize(const PhysicalOperator &op, ExecutionContext &context) override {
		context.thread.profiler.Flush(op);
	}

	bool SupportsReuse() const override {
		return true;
	}

	void Reset() override {
		ResetState();
	}
};

unique_ptr<OperatorState> PhysicalNestedLoopJoin::GetOperatorState(ExecutionContext &context) const {
	return make_uniq<PhysicalNestedLoopJoinState>(context.client, *this, conditions);
}

OperatorResultType PhysicalNestedLoopJoin::ExecuteInternal(ExecutionContext &context, DataChunk &input,
                                                           DataChunk &chunk, GlobalOperatorState &gstate_p,
                                                           OperatorState &state_p) const {
	auto &gstate = sink_state->Cast<NestedLoopJoinGlobalState>();
	auto &state = state_p.Cast<PhysicalNestedLoopJoinState>();
	if (join_type == JoinType::MARK) {
		state.mark_payload.ReferenceColumns(input, mark_projection_map);
	}

	if (gstate.right_payload_data.Count() == 0) {
		// empty RHS
		if (!EmptyResultIfRHSIsEmpty()) {
			auto &payload = join_type == JoinType::MARK ? state.mark_payload : input;
			ConstructEmptyJoinResult(join_type, gstate.has_null, payload, chunk);
			return OperatorResultType::NEED_MORE_INPUT;
		} else {
			return OperatorResultType::FINISHED;
		}
	}

	switch (join_type) {
	case JoinType::SEMI:
	case JoinType::ANTI:
	case JoinType::MARK:
		// simple joins can have max STANDARD_VECTOR_SIZE matches per chunk
		ResolveSimpleJoin(context, input, chunk, state_p);
		return OperatorResultType::NEED_MORE_INPUT;
	case JoinType::LEFT:
	case JoinType::INNER:
	case JoinType::OUTER:
	case JoinType::RIGHT:
		return ResolveComplexJoin(context, input, chunk, state_p);
	default:
		throw NotImplementedException("Unimplemented type " + JoinTypeToString(join_type) + " for nested loop join!");
	}
}

static void ResolveSimpleJoinPredicate(const vector<JoinCondition> &conditions, DataChunk &input,
                                       PhysicalNestedLoopJoinState &state, NestedLoopJoinGlobalState &gstate,
                                       bool found_match[]) {
	D_ASSERT(conditions.size() == 1);
	gstate.right_condition_data.InitializeScan(state.condition_scan_state);
	gstate.right_payload_data.InitializeScan(state.payload_scan_state);
	Vector comparison(LogicalType::BOOLEAN);
	Vector left_reference(state.left_condition.data[0].GetType());
	VectorCache predicate_cache(state.pred_executor.GetAllocator(), LogicalType::BOOLEAN);
	Vector predicate_result(predicate_cache);
	while (gstate.right_condition_data.Scan(state.condition_scan_state, state.right_condition)) {
		if (!gstate.right_payload_data.Scan(state.payload_scan_state, state.right_payload) ||
		    state.right_condition.size() != state.right_payload.size()) {
			throw InternalException("Nested loop join: payload and conditions are unaligned!?");
		}
		for (idx_t left_row = 0; left_row < input.size(); left_row++) {
			if (found_match[left_row]) {
				continue;
			}
			ConstantVector::Reference(left_reference, count_t(state.right_condition.size()),
			                          state.left_condition.data[0], left_row, state.left_condition.size());
			MarkJoinRowComparison::Compare(left_reference, state.right_condition.data[0],
			                               conditions[0].GetComparisonType(), comparison);
			auto comparisons = comparison.Values<bool>();
			idx_t candidate_count = 0;
			for (idx_t right_row = 0; right_row < state.right_condition.size(); right_row++) {
				auto entry = comparisons[right_row];
				if (entry.IsValid() && entry.GetValue()) {
					state.pred_matches.set_index(candidate_count++, right_row);
				}
			}
			if (candidate_count == 0) {
				continue;
			}
			state.pred_input.Reset();
			state.pred_input.SetChildCardinality(candidate_count);
			state.pred_input.Slice(state.right_payload, state.pred_matches, candidate_count, input.ColumnCount());
			for (idx_t column_idx = 0; column_idx < input.ColumnCount(); column_idx++) {
				ConstantVector::Reference(state.pred_input.data[column_idx], count_t(candidate_count),
				                          input.data[column_idx], left_row, input.size());
			}
			predicate_result.ResetFromCache(predicate_cache);
			state.pred_executor.ExecuteExpression(state.pred_input, predicate_result);
			auto predicates = predicate_result.Values<bool>();
			for (idx_t candidate = 0; candidate < candidate_count; candidate++) {
				auto predicate = predicates[candidate];
				if (predicate.IsValid() && predicate.GetValue()) {
					found_match[left_row] = true;
					break;
				}
			}
		}
	}
}

void PhysicalNestedLoopJoin::ResolveSimpleJoin(ExecutionContext &context, DataChunk &input, DataChunk &chunk,
                                               OperatorState &state_p) const {
	auto &state = state_p.Cast<PhysicalNestedLoopJoinState>();
	auto &gstate = sink_state->Cast<NestedLoopJoinGlobalState>();

	// resolve the left join condition for the current chunk
	state.left_condition.Reset();
	state.lhs_executor.Execute(input, state.left_condition);

	bool found_match[STANDARD_VECTOR_SIZE] = {false};
	bool found_unknown[STANDARD_VECTOR_SIZE] = {false};

	if (predicate) {
		D_ASSERT(join_type == JoinType::SEMI || join_type == JoinType::ANTI);
		ResolveSimpleJoinPredicate(conditions, input, state, gstate, found_match);
	} else {
		NestedLoopJoinMark::Perform(state.left_condition, gstate.right_condition_data, found_match, conditions,
		                            track_unknown ? optional_ptr<bool>(found_unknown) : nullptr);
	}
	switch (join_type) {
	case JoinType::MARK:
		// now construct the mark join result from the found matches
		PhysicalJoin::ConstructMarkJoinResult(state.left_condition, state.mark_payload, chunk, found_match,
		                                      gstate.has_null,
		                                      track_unknown ? optional_ptr<const bool>(found_unknown) : nullptr);
		break;
	case JoinType::SEMI:
		// construct the semi join result from the found matches
		PhysicalJoin::ConstructSemiJoinResult(input, chunk, found_match);
		break;
	case JoinType::ANTI:
		// construct the anti join result from the found matches
		PhysicalJoin::ConstructAntiJoinResult(input, chunk, found_match);
		break;
	default:
		throw NotImplementedException("Unimplemented type for simple nested loop join!");
	}
}

OperatorResultType PhysicalNestedLoopJoin::ResolveComplexJoin(ExecutionContext &context, DataChunk &input,
                                                              DataChunk &chunk, OperatorState &state_p) const {
	auto &state = state_p.Cast<PhysicalNestedLoopJoinState>();
	auto &gstate = sink_state->Cast<NestedLoopJoinGlobalState>();

	idx_t match_count;
	do {
		if (state.fetch_next_right) {
			// we exhausted the chunk on the right: move to the next chunk on the right
			state.left_tuple = 0;
			state.right_tuple = 0;
			state.fetch_next_right = false;
			// check if we exhausted all chunks on the RHS
			if (gstate.right_condition_data.Scan(state.condition_scan_state, state.right_condition)) {
				if (!gstate.right_payload_data.Scan(state.payload_scan_state, state.right_payload)) {
					throw InternalException("Nested loop join: payload and conditions are unaligned!?");
				}
				if (state.right_condition.size() != state.right_payload.size()) {
					throw InternalException("Nested loop join: payload and conditions are unaligned!?");
				}
			} else {
				// we exhausted all chunks on the right: move to the next chunk on the left
				state.fetch_next_left = true;
				if (state.left_outer.Enabled()) {
					// left join: before we move to the next chunk, see if we need to output any vectors that didn't
					// have a match found
					state.left_outer.ConstructLeftJoinResult(input, chunk);
					state.left_outer.Reset();
				}
				return OperatorResultType::NEED_MORE_INPUT;
			}
		}
		if (state.fetch_next_left) {
			// resolve the left join condition for the current chunk
			state.left_condition.Reset();
			state.lhs_executor.Execute(input, state.left_condition);

			state.left_tuple = 0;
			state.right_tuple = 0;
			gstate.right_condition_data.InitializeScan(state.condition_scan_state);
			gstate.right_condition_data.Scan(state.condition_scan_state, state.right_condition);

			gstate.right_payload_data.InitializeScan(state.payload_scan_state);
			gstate.right_payload_data.Scan(state.payload_scan_state, state.right_payload);
			state.fetch_next_left = false;
		}
		// now we have a left and a right chunk that we can join together
		// note that we only get here in the case of a LEFT, INNER or FULL join
		auto &left_chunk = input;
		auto &right_condition = state.right_condition;
		auto &right_payload = state.right_payload;

		// sanity check
		left_chunk.Verify(context.client.db);
		right_condition.Verify(context.client.db);
		right_payload.Verify(context.client.db);

		// now perform the join
		SelectionVector lvector(STANDARD_VECTOR_SIZE), rvector(STANDARD_VECTOR_SIZE);
		match_count = NestedLoopJoinInner::Perform(state.left_tuple, state.right_tuple, state.left_condition,
		                                           right_condition, lvector, rvector, conditions);
		// we have finished resolving the join conditions
		if (match_count > 0) {
			// we have matching tuples!
			// construct the result
			chunk.Slice(input, lvector, match_count);
			chunk.Slice(right_payload, rvector, match_count, input.ColumnCount());

			//	If we have a predicate, apply it to the result
			if (predicate) {
				auto &sel = state.pred_matches;
				match_count = state.pred_executor.SelectExpression(chunk, sel);
				if (match_count == 0) {
					// predicate filtered everything out - reset the chunk to keep vector sizes consistent
					chunk.Reset();
				} else {
					chunk.Slice(sel, match_count);
					lvector.SliceInPlace(sel, match_count);
					rvector.SliceInPlace(sel, match_count);
				}
			}

			state.left_outer.SetMatches(lvector, match_count);
			gstate.right_outer.SetMatches(rvector, match_count, state.condition_scan_state.current_row_index);
		}

		// check if we exhausted the RHS, if we did we need to move to the next right chunk in the next iteration
		if (state.right_tuple >= right_condition.size()) {
			state.fetch_next_right = true;
		}
	} while (match_count == 0);
	return OperatorResultType::HAVE_MORE_OUTPUT;
}

//===--------------------------------------------------------------------===//
// Source
//===--------------------------------------------------------------------===//
class NestedLoopJoinGlobalScanState : public GlobalSourceState {
public:
	explicit NestedLoopJoinGlobalScanState(const PhysicalNestedLoopJoin &op, ClientContext &context) : op(op) {
		ResetState(context);
	}

	const PhysicalNestedLoopJoin &op;
	OuterJoinGlobalScanState scan_state;

private:
	void ResetState(ClientContext &context) {
		auto &sink = op.sink_state->Cast<NestedLoopJoinGlobalState>();
		sink.right_outer.InitializeScan(sink.right_payload_data, scan_state);
		GlobalSourceState::Reset(context);
	}

public:
	idx_t MaxThreads() override {
		auto &sink = op.sink_state->Cast<NestedLoopJoinGlobalState>();
		return sink.right_outer.MaxThreads();
	}

	bool SupportsReuse() const override {
		return true;
	}

	void Reset(ClientContext &context) override {
		ResetState(context);
	}
};

class NestedLoopJoinLocalScanState : public LocalSourceState {
public:
	explicit NestedLoopJoinLocalScanState(const PhysicalNestedLoopJoin &op, NestedLoopJoinGlobalScanState &gstate) {
		auto &sink = op.sink_state->Cast<NestedLoopJoinGlobalState>();
		sink.right_outer.InitializeScan(gstate.scan_state, scan_state);
	}

	OuterJoinLocalScanState scan_state;

public:
	bool SupportsReuse() const override {
		return true;
	}

	void Reset(ExecutionContext &, GlobalSourceState &) override {
		scan_state.Reset();
	}
};

ProgressData PhysicalNestedLoopJoin::GetProgress(ClientContext &context, GlobalSourceState &gstate) const {
	return gstate.Cast<NestedLoopJoinGlobalScanState>().scan_state.GetProgress();
}

unique_ptr<GlobalSourceState> PhysicalNestedLoopJoin::GetGlobalSourceState(ClientContext &context) const {
	return make_uniq<NestedLoopJoinGlobalScanState>(*this, context);
}

unique_ptr<LocalSourceState> PhysicalNestedLoopJoin::GetLocalSourceState(ExecutionContext &context,
                                                                         GlobalSourceState &gstate) const {
	return make_uniq<NestedLoopJoinLocalScanState>(*this, gstate.Cast<NestedLoopJoinGlobalScanState>());
}

SourceResultType PhysicalNestedLoopJoin::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                         OperatorSourceInput &input) const {
	D_ASSERT(PropagatesBuildSide(join_type));
	// check if we need to scan any unmatched tuples from the RHS for the full/right outer join
	auto &sink = sink_state->Cast<NestedLoopJoinGlobalState>();
	auto &gstate = input.global_state.Cast<NestedLoopJoinGlobalScanState>();
	auto &lstate = input.local_state.Cast<NestedLoopJoinLocalScanState>();

	// if the LHS is exhausted in a FULL/RIGHT OUTER JOIN, we scan chunks we still need to output
	sink.right_outer.Scan(gstate.scan_state, lstate.scan_state, chunk);

	return chunk.size() == 0 ? SourceResultType::FINISHED : SourceResultType::HAVE_MORE_OUTPUT;
}

} // namespace duckdb
