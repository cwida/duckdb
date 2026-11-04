#include "duckdb/execution/operator/join/physical_piecewise_merge_join.hpp"

#include <numeric>

#include "duckdb/common/atomic.hpp"
#include "duckdb/common/row_operations/row_operations.hpp"
#include "duckdb/common/sorting/sort_key.hpp"
#include "duckdb/common/types/row/block_iterator.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/execution/operator/join/outer_join_marker.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parallel/event.hpp"
#include "duckdb/parallel/thread_context.hpp"

namespace duckdb {

PhysicalPiecewiseMergeJoin::PhysicalPiecewiseMergeJoin(PhysicalPlan &physical_plan, LogicalComparisonJoin &op,
                                                       PhysicalOperator &left, PhysicalOperator &right,
                                                       vector<JoinCondition> cond, JoinType join_type,
                                                       idx_t estimated_cardinality,
                                                       unique_ptr<JoinFilterPushdownInfo> pushdown_info_p)
    : PhysicalRangeJoin(physical_plan, op, PhysicalOperatorType::PIECEWISE_MERGE_JOIN, left, right, std::move(cond),
                        join_type, estimated_cardinality, std::move(pushdown_info_p)) {
	for (auto &join_cond : conditions) {
		D_ASSERT(join_cond.GetLHS().GetReturnType() == join_cond.GetRHS().GetReturnType());
		join_key_types.push_back(join_cond.GetLHS().GetReturnType());

		// Convert the conditions to sort orders
		auto left_expr = join_cond.GetLHS().Copy();
		auto right_expr = join_cond.GetRHS().Copy();
		switch (join_cond.GetComparisonType()) {
		case ExpressionType::COMPARE_LESSTHAN:
		case ExpressionType::COMPARE_LESSTHANOREQUALTO:
			lhs_orders.emplace_back(OrderType::ASCENDING, OrderByNullType::NULLS_LAST, std::move(left_expr));
			rhs_orders.emplace_back(OrderType::ASCENDING, OrderByNullType::NULLS_LAST, std::move(right_expr));
			break;
		case ExpressionType::COMPARE_GREATERTHAN:
		case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
			lhs_orders.emplace_back(OrderType::DESCENDING, OrderByNullType::NULLS_LAST, std::move(left_expr));
			rhs_orders.emplace_back(OrderType::DESCENDING, OrderByNullType::NULLS_LAST, std::move(right_expr));
			break;
		case ExpressionType::COMPARE_NOTEQUAL:
		case ExpressionType::COMPARE_DISTINCT_FROM:
			// Allowed in multi-predicate joins, but can't be first/sort.
			D_ASSERT(!lhs_orders.empty());
			lhs_orders.emplace_back(OrderType::INVALID, OrderByNullType::NULLS_LAST, std::move(left_expr));
			rhs_orders.emplace_back(OrderType::INVALID, OrderByNullType::NULLS_LAST, std::move(right_expr));
			break;

		default:
			// COMPARE EQUAL not supported with merge join
			throw NotImplementedException("Unimplemented join type for merge join");
		}
	}
}

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//
class MergeJoinLocalState;

class MergeJoinGlobalState : public GlobalSinkState {
public:
	using GlobalSortedTable = PhysicalRangeJoin::GlobalSortedTable;

public:
	MergeJoinGlobalState(ClientContext &client, const PhysicalPiecewiseMergeJoin &op) : op(op) {
		const auto &rhs_types = op.children[1].get().GetTypes();
		vector<BoundOrderByNode> rhs_order;
		rhs_order.emplace_back(op.rhs_orders[0].Copy());
		table = make_uniq<GlobalSortedTable>(client, rhs_order, rhs_types, op);
		ResetState(client);
	}

	inline idx_t Count() const {
		return table->count;
	}

	void Sink(ExecutionContext &context, DataChunk &input, MergeJoinLocalState &lstate);

	const PhysicalPiecewiseMergeJoin &op;
	//! The sorted table
	unique_ptr<GlobalSortedTable> table;
	//! Should we not bother pushing down filters?
	bool skip_filter_pushdown = false;
	//! The global filter states to push down (if any)
	unique_ptr<JoinFilterGlobalState> global_filter_state;

private:
	void ResetState(ClientContext &client) {
		table->ResetForReuse(client);
		if (op.filter_pushdown) {
			skip_filter_pushdown = op.filter_pushdown->probe_info.empty();
			global_filter_state = op.filter_pushdown->GetGlobalState(client, op);
		} else {
			skip_filter_pushdown = false;
			global_filter_state.reset();
		}
		GlobalSinkState::Reset(client);
	}

public:
	bool SupportsReuse() const override {
		return true;
	}

	void Reset(ClientContext &client) override {
		ResetState(client);
	}
};

class MergeJoinLocalState : public LocalSinkState {
public:
	using GlobalSortedTable = PhysicalRangeJoin::GlobalSortedTable;
	using LocalSortedTable = PhysicalRangeJoin::LocalSortedTable;

	MergeJoinLocalState(ExecutionContext &context, MergeJoinGlobalState &gstate, const idx_t child)
	    : table(context, *gstate.table, child) {
		ResetState(gstate);
	}

	//! The local sort state
	LocalSortedTable table;
	//! Local state for accumulating filter statistics
	unique_ptr<JoinFilterLocalState> local_filter_state;

private:
	void ResetState(MergeJoinGlobalState &gstate) {
		auto &op = gstate.table->op;
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
		auto &gstate = gstate_p.Cast<MergeJoinGlobalState>();
		table.ResetForReuse(context);
		ResetState(gstate);
	}
};

unique_ptr<GlobalSinkState> PhysicalPiecewiseMergeJoin::GetGlobalSinkState(ClientContext &context) const {
	return make_uniq<MergeJoinGlobalState>(context, *this);
}

unique_ptr<LocalSinkState> PhysicalPiecewiseMergeJoin::GetLocalSinkState(ExecutionContext &context) const {
	// We only sink the RHS
	auto &gstate = sink_state->Cast<MergeJoinGlobalState>();
	return make_uniq<MergeJoinLocalState>(context, gstate, 1U);
}

void MergeJoinGlobalState::Sink(ExecutionContext &context, DataChunk &input, MergeJoinLocalState &lstate) {
	// Sink the data into the local sort state
	lstate.table.Sink(context, input);
}

SinkResultType PhysicalPiecewiseMergeJoin::Sink(ExecutionContext &context, DataChunk &chunk,
                                                OperatorSinkInput &input) const {
	auto &gstate = input.global_state.Cast<MergeJoinGlobalState>();
	auto &lstate = input.local_state.Cast<MergeJoinLocalState>();

	gstate.Sink(context, chunk, lstate);

	if (filter_pushdown && !gstate.skip_filter_pushdown) {
		filter_pushdown->Sink(lstate.table.keys, *lstate.local_filter_state);
	}

	return SinkResultType::NEED_MORE_INPUT;
}

SinkCombineResultType PhysicalPiecewiseMergeJoin::Combine(ExecutionContext &context,
                                                          OperatorSinkCombineInput &input) const {
	auto &gstate = input.global_state.Cast<MergeJoinGlobalState>();
	auto &lstate = input.local_state.Cast<MergeJoinLocalState>();
	gstate.table->Combine(context, lstate.table);
	auto &client_profiler = QueryProfiler::Get(context.client);

	context.thread.profiler.Flush(*this);
	client_profiler.Flush(context.thread.profiler);
	if (filter_pushdown && !gstate.skip_filter_pushdown) {
		filter_pushdown->Combine(*gstate.global_filter_state, *lstate.local_filter_state);
	}

	return SinkCombineResultType::FINISHED;
}

//===--------------------------------------------------------------------===//
// Finalize
//===--------------------------------------------------------------------===//
SinkFinalizeType PhysicalPiecewiseMergeJoin::Finalize(Pipeline &pipeline, Event &event, ClientContext &client,
                                                      OperatorSinkFinalizeInput &input) const {
	auto &gstate = input.global_state.Cast<MergeJoinGlobalState>();
	if (filter_pushdown && !gstate.skip_filter_pushdown) {
		(void)filter_pushdown->Finalize(client, *gstate.global_filter_state, *this);
	}

	gstate.table->Finalize(client, input.interrupt_state);

	if (PropagatesBuildSide(join_type)) {
		// for FULL/RIGHT OUTER JOIN, initialize found_match to false for every tuple
		gstate.table->InitializeMatches();
	}

	if (gstate.table->Count() == 0 && EmptyResultIfRHSIsEmpty()) {
		// Empty input!
		gstate.table->MaterializeEmpty(client);
		return SinkFinalizeType::NO_OUTPUT_POSSIBLE;
	}

	// Sort the current input child
	gstate.table->Materialize(pipeline, event);

	return SinkFinalizeType::READY;
}

//===--------------------------------------------------------------------===//
// Operator
//===--------------------------------------------------------------------===//
class PiecewiseMergeJoinState : public CachingOperatorState {
public:
	using LocalSortedTable = PhysicalRangeJoin::LocalSortedTable;
	using GlobalSortedTable = PhysicalRangeJoin::GlobalSortedTable;

	PiecewiseMergeJoinState(ClientContext &client, const PhysicalPiecewiseMergeJoin &op)
	    : client(client), allocator(Allocator::Get(client)), op(op), left_outer(IsLeftOuterJoin(op.join_type)),
	      left_position(0), first_fetch(true), finished(true), right_position(0), right_chunk_index(0),
	      rhs_executor(client), pred_executor(client) {
		left_outer.Initialize(STANDARD_VECTOR_SIZE);
		lhs_payload.Initialize(client, op.children[0].get().GetTypes());

		//	Sort on the first column
		lhs_order.emplace_back(op.lhs_orders[0].Copy());
		lhs_global_table = make_uniq<GlobalSortedTable>(client, lhs_order, op.children[0].get().GetTypes(), op);

		// Set up shared data for multiple predicates
		sel.Initialize(STANDARD_VECTOR_SIZE);
		vector<LogicalType> condition_types;
		for (auto &order : op.rhs_orders) {
			rhs_executor.AddExpression(*order.expression);
			condition_types.push_back(order.expression->GetReturnType());
		}
		rhs_keys.Initialize(client, condition_types);
		rhs_input.Initialize(client, op.children[1].get().GetTypes());

		auto &gsink = op.sink_state->Cast<MergeJoinGlobalState>();
		InitializeRightScanState(gsink);

		if (op.predicate) {
			pred_executor.AddExpression(*op.predicate);
			pred_matches.Initialize();
		}
	}

	ClientContext &client;
	Allocator &allocator;
	const PhysicalPiecewiseMergeJoin &op;

	// Block sorting
	DataChunk lhs_payload;
	OuterJoinMarker left_outer;
	vector<BoundOrderByNode> lhs_order;
	unique_ptr<GlobalSortedTable> lhs_global_table;
	unique_ptr<LocalSortedTable> lhs_local_table;
	SortKeyType sort_key_type;
	TupleDataScanState lhs_scan;

	// Simple scans
	idx_t left_position;

	// Complex scans
	bool first_fetch;
	bool finished;
	unique_ptr<ExternalBlockIteratorState> lhs_iterator;
	unique_ptr<ExternalBlockIteratorState> rhs_iterator;
	idx_t right_position;
	idx_t right_chunk_index;
	idx_t right_base;
	idx_t prev_left_index;
	TupleDataChunkState rhs_chunk_state;
	unique_ptr<SortedRunScanState> rhs_scan_state;

	// Secondary predicate shared data
	SelectionVector sel;
	DataChunk rhs_keys;
	DataChunk rhs_input;
	ExpressionExecutor rhs_executor;

	//! Arbitrary expressions
	ExpressionExecutor pred_executor;
	SelectionVector pred_matches;

public:
	bool SupportsReuse() const override {
		return true;
	}

	void Reset() override {
		ResetCachingState();
		left_outer.Reset();
		lhs_payload.Reset();
		lhs_scan.Reset();
		left_position = 0;
		first_fetch = true;
		finished = true;
		lhs_iterator.reset();
		rhs_iterator.reset();
		right_position = 0;
		right_chunk_index = 0;
		right_base = 0;
		prev_left_index = 0;
		rhs_chunk_state.ResetForScan();
		rhs_keys.Reset();
		rhs_input.Reset();
		lhs_global_table->ResetForReuse(client);
	}

	void InitializeRightScanState(MergeJoinGlobalState &gsink) {
		auto &rhs_table = *gsink.table;
		rhs_iterator = rhs_table.CreateIteratorState();
		rhs_chunk_state.ResetForScan();
		rhs_table.InitializePayloadState(rhs_chunk_state);
		if (!rhs_scan_state) {
			rhs_scan_state = rhs_table.CreateScanState(client);
		}
		//	Since we have now materialized the payload, the keys will not have payloads?
		sort_key_type = rhs_table.GetSortKeyType();
	}

	void ResolveJoinKeys(ExecutionContext &context, DataChunk &input) {
		// sort by join key
		lhs_global_table->ResetForReuse(context.client);
		if (!lhs_local_table) {
			lhs_local_table = make_uniq<LocalSortedTable>(context, *lhs_global_table, 0U);
		} else {
			lhs_local_table->ResetForReuse(context);
		}
		lhs_local_table->Sink(context, input);
		lhs_global_table->Combine(context, *lhs_local_table);

		InterruptState interrupt;
		lhs_global_table->Finalize(context.client, interrupt);
		lhs_global_table->Materialize(context, interrupt);

		// Scan the sorted payload (minus the primary sort column)
		auto &lhs_table = *lhs_global_table;
		auto &lhs_payload_data = *lhs_table.sorted->payload_data;
		lhs_payload_data.InitializeScan(lhs_scan);
		lhs_payload_data.Scan(lhs_scan, lhs_payload);

		// Recompute the sorted keys from the sorted input
		auto &lhs_keys = lhs_local_table->keys;
		lhs_keys.Reset();
		lhs_local_table->executor.Execute(lhs_payload, lhs_keys);

		lhs_iterator = lhs_table.CreateIteratorState();
	}
};

unique_ptr<OperatorState> PhysicalPiecewiseMergeJoin::GetOperatorState(ExecutionContext &context) const {
	return make_uniq<PiecewiseMergeJoinState>(context.client, *this);
}

static inline idx_t SortedChunkNotNull(const idx_t chunk_idx, const idx_t count, const idx_t has_null) {
	const auto chunk_begin = chunk_idx * STANDARD_VECTOR_SIZE;
	const auto chunk_end = MinValue(chunk_begin + STANDARD_VECTOR_SIZE, count);
	const auto not_null = count - has_null;
	return MinValue(chunk_end, MaxValue(chunk_begin, not_null)) - chunk_begin;
}

static bool MergeJoinStrictComparison(ExpressionType comparison) {
	switch (comparison) {
	case ExpressionType::COMPARE_LESSTHAN:
	case ExpressionType::COMPARE_GREATERTHAN:
		return true;
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		return false;
	default:
		throw InternalException("Unimplemented comparison type for merge join!");
	}
}

//	Compare using </<=
template <typename T>
static bool MergeJoinBefore(const T &lhs, const T &rhs, const bool strict) {
	const bool less_than = lhs < rhs;
	if (!less_than && !strict) {
		return !(rhs < lhs);
	}
	return less_than;
}

template <SortKeyType SORT_KEY_TYPE>
static idx_t TemplatedMergeJoinSimpleBlocks(PiecewiseMergeJoinState &lstate, MergeJoinGlobalState &gstate,
                                            bool *found_match, const bool strict) {
	using SORT_KEY = SortKey<SORT_KEY_TYPE>;
	using BLOCK_ITERATOR = block_iterator_t<ExternalBlockIteratorState, SORT_KEY>;

	//	We only need the keys because we are extracting the row numbers
	auto &lhs_table = *lstate.lhs_global_table;
	D_ASSERT(SORT_KEY_TYPE == lhs_table.GetSortKeyType());
	auto &lhs_iterator = *lstate.lhs_iterator;
	const auto lhs_not_null = lhs_table.count - lhs_table.has_null;

	auto &rhs_table = *gstate.table;
	auto &rhs_iterator = *lstate.rhs_iterator;
	const auto rhs_not_null = rhs_table.count - rhs_table.has_null;

	idx_t l_entry_idx = 0;
	BLOCK_ITERATOR lhs_itr(lhs_iterator);
	BLOCK_ITERATOR rhs_itr(rhs_iterator);
	for (idx_t r_idx = 0; r_idx < rhs_not_null; r_idx += STANDARD_VECTOR_SIZE) {
		//	Repin the RHS to release memory
		//	This is safe because we only return the LHS values
		//	Note we only do this for the RHS because the LHS is only one chunk.
		rhs_table.Repin(rhs_iterator);

		// we only care about the BIGGEST value in the RHS
		// because we want to figure out if the LHS values are less than [or equal] to ANY value
		const auto r_entry_idx = MinValue<idx_t>(r_idx + STANDARD_VECTOR_SIZE, rhs_not_null) - 1;

		// now we start from the current lpos value and check if we found a new value that is [<= OR <] the max RHS
		// value
		while (true) {
			//	Note that both subscripts here are table indices, not chunk indices.
			if (MergeJoinBefore(lhs_itr[l_entry_idx], rhs_itr[r_entry_idx], strict)) {
				// found a match for lpos, set it in the found_match vector
				found_match[l_entry_idx] = true;
				l_entry_idx++;
				if (l_entry_idx >= lhs_not_null) {
					// early out: we exhausted the entire LHS and they all match
					return 0;
				}
			} else {
				// we found no match: any subsequent value from the LHS we scan now will be bigger and thus also not
				// match. Move to the next RHS chunk
				break;
			}
		}
	}
	return 0;
}

static idx_t MergeJoinSimpleBlocks(PiecewiseMergeJoinState &lstate, MergeJoinGlobalState &gstate, bool *match,
                                   const ExpressionType comparison) {
	const auto strict = MergeJoinStrictComparison(comparison);

	switch (lstate.sort_key_type) {
#define DUCKDB_SORT_KEY_CASE(SORT_KEY_TYPE)                                                                            \
	case SortKeyType::SORT_KEY_TYPE:                                                                                   \
		return TemplatedMergeJoinSimpleBlocks<SortKeyType::SORT_KEY_TYPE>(lstate, gstate, match, strict);
		DUCKDB_FOR_EACH_SORT_KEY_TYPE(DUCKDB_SORT_KEY_CASE)
#undef DUCKDB_SORT_KEY_CASE
	default:
		throw NotImplementedException("MergeJoinSimpleBlocks for %s", EnumUtil::ToString(lstate.sort_key_type));
	}
}

void PhysicalPiecewiseMergeJoin::ResolveSimpleJoin(ExecutionContext &context, DataChunk &input, DataChunk &chunk,
                                                   OperatorState &state_p) const {
	auto &state = state_p.Cast<PiecewiseMergeJoinState>();
	auto &gstate = sink_state->Cast<MergeJoinGlobalState>();

	state.ResolveJoinKeys(context, input);
	auto &lhs_table = *state.lhs_global_table;
	auto &lhs_keys = state.lhs_local_table->keys;

	// perform the actual join
	bool found_match[STANDARD_VECTOR_SIZE];
	memset(found_match, 0, sizeof(found_match));
	MergeJoinSimpleBlocks(state, gstate, found_match, conditions[0].GetComparisonType());

	// use the sorted payload
	const auto lhs_not_null = lhs_table.count - lhs_table.has_null;
	auto &payload = state.lhs_payload;

	// now construct the result based on the join result
	switch (join_type) {
	case JoinType::MARK: {
		// The only part of the join keys that is actually used is the validity mask.
		// Since the payload is sorted, we can just set the tail end of the validity masks to invalid.
		for (auto &key : lhs_keys.data) {
			key.Flatten();
			auto &mask = FlatVector::ValidityMutable(key);
			if (mask.CannotHaveNull()) {
				continue;
			}
			mask.SetAllValid(lhs_not_null);
			for (idx_t i = lhs_not_null; i < lhs_table.count; ++i) {
				mask.SetInvalid(i);
			}
		}
		// So we make a set of keys that have the validity mask set for the
		PhysicalJoin::ConstructMarkJoinResult(lhs_keys, payload, chunk, found_match, gstate.table->has_null);
		break;
	}
	case JoinType::SEMI:
		PhysicalJoin::ConstructSemiJoinResult(payload, chunk, found_match);
		break;
	case JoinType::ANTI:
		PhysicalJoin::ConstructAntiJoinResult(payload, chunk, found_match);
		break;
	default:
		throw NotImplementedException("Unimplemented join type for merge join");
	}
}

struct ChunkMergeInfo {
	//! The iteration state
	ExternalBlockIteratorState &state;
	//! The block being scanned
	const idx_t block_idx;
	//! The number of not-NULL values in the chunk (they are at the end)
	const idx_t not_null;
	//! The current offset in the chunk
	idx_t &entry_idx;
	//! The left chunk offsets that match
	SelectionVector lhs;
	//! The right table offsets that match
	unsafe_vector<idx_t> rhs;

	ChunkMergeInfo(ExternalBlockIteratorState &state, idx_t block_idx, idx_t &entry_idx, idx_t not_null)
	    : state(state), block_idx(block_idx), not_null(not_null), entry_idx(entry_idx), lhs(STANDARD_VECTOR_SIZE) {
	}

	idx_t GetIndex() const {
		return state.GetIndex(block_idx, entry_idx);
	}
};

template <SortKeyType SORT_KEY_TYPE>
static idx_t TemplatedMergeJoinComplexBlocks(ChunkMergeInfo &l, ChunkMergeInfo &r, const bool strict,
                                             idx_t &prev_left_index) {
	using SORT_KEY = SortKey<SORT_KEY_TYPE>;
	using BLOCK_ITERATOR = block_iterator_t<ExternalBlockIteratorState, SORT_KEY>;

	idx_t result_count = 0;
	r.rhs.resize(0);
	if (r.entry_idx >= r.not_null) {
		return result_count;
	}

	auto *const lhs_sel = l.lhs.data();
	while (true) {
		// Phase 1: bulk-emit known matches (l.entry_idx < prev_left_index already matched current r)
		if (l.entry_idx < prev_left_index) {
			const idx_t batch = MinValue(prev_left_index - l.entry_idx, STANDARD_VECTOR_SIZE - result_count);
			std::iota(lhs_sel + result_count, lhs_sel + result_count + batch, NumericCast<sel_t>(l.entry_idx));
			r.rhs.resize(r.rhs.size() + batch, r.entry_idx);
			result_count += batch;
			l.entry_idx += batch;
			if (result_count == STANDARD_VECTOR_SIZE) {
				break;
			}
		}

		// Phase 2: binary search for the new boundary on the left side
		if (l.entry_idx < l.not_null) {
			const BLOCK_ITERATOR search_begin(l.state, l.entry_idx);
			const BLOCK_ITERATOR search_end(l.state, l.not_null);

			const auto &r_val = r.state.template GetValueAtIndex<SORT_KEY>(r.block_idx, r.entry_idx);
			const auto new_boundary_itr = std::lower_bound(search_begin, search_end, r_val,
			                                               [strict](const SORT_KEY &lhs_val, const SORT_KEY &rhs_val) {
				                                               return MergeJoinBefore(lhs_val, rhs_val, strict);
			                                               });

			const auto new_boundary = l.entry_idx + NumericCast<idx_t>(new_boundary_itr - search_begin);
			if (new_boundary > l.entry_idx) {
				const idx_t batch = MinValue(new_boundary - l.entry_idx, STANDARD_VECTOR_SIZE - result_count);
				std::iota(lhs_sel + result_count, lhs_sel + result_count + batch, sel_t(l.entry_idx));
				r.rhs.resize(r.rhs.size() + batch, r.entry_idx);
				result_count += batch;
				l.entry_idx += batch;
				if (result_count == STANDARD_VECTOR_SIZE) {
					prev_left_index = new_boundary;
					break;
				}
			}
		}

		prev_left_index = l.entry_idx;
		r.entry_idx++;
		if (r.entry_idx >= r.not_null) {
			break;
		}
		l.entry_idx = 0;
	}

	return result_count;
}

static idx_t MergeJoinComplexBlocks(const SortKeyType &sort_key_type, ChunkMergeInfo &l, ChunkMergeInfo &r,
                                    const ExpressionType comparison, idx_t &prev_left_index) {
	const auto strict = MergeJoinStrictComparison(comparison);

	switch (sort_key_type) {
#define DUCKDB_SORT_KEY_CASE(SORT_KEY_TYPE)                                                                            \
	case SortKeyType::SORT_KEY_TYPE:                                                                                   \
		return TemplatedMergeJoinComplexBlocks<SortKeyType::SORT_KEY_TYPE>(l, r, strict, prev_left_index);
		DUCKDB_FOR_EACH_SORT_KEY_TYPE(DUCKDB_SORT_KEY_CASE)
#undef DUCKDB_SORT_KEY_CASE
	default:
		throw NotImplementedException("MergeJoinSimpleBlocks for %s", EnumUtil::ToString(sort_key_type));
	}
}

OperatorResultType PhysicalPiecewiseMergeJoin::ResolveComplexJoin(ExecutionContext &context, DataChunk &input,
                                                                  DataChunk &chunk, OperatorState &state_p) const {
	auto &state = state_p.Cast<PiecewiseMergeJoinState>();
	auto &gstate = sink_state->Cast<MergeJoinGlobalState>();
	const auto left_cols = input.ColumnCount();
	const auto tail_cols = conditions.size() - 1;

	do {
		if (state.first_fetch) {
			state.ResolveJoinKeys(context, input);
			state.InitializeRightScanState(gstate);
			state.lhs_payload.Verify(context.client.db);

			state.right_chunk_index = 0;
			state.right_base = 0;
			state.left_position = 0;
			state.prev_left_index = 0;
			state.right_position = 0;
			state.first_fetch = false;
			state.finished = false;
		}
		if (state.finished) {
			if (state.left_outer.Enabled()) {
				// left join: before we move to the next chunk, see if we need to output any vectors that didn't
				// have a match found
				state.left_outer.ConstructLeftJoinResult(state.lhs_payload, chunk);
				state.left_outer.Reset();
			}
			state.first_fetch = true;
			state.finished = false;
			return OperatorResultType::NEED_MORE_INPUT;
		}

		auto &lhs_table = *state.lhs_global_table;
		const auto lhs_not_null = lhs_table.count - lhs_table.has_null;
		ChunkMergeInfo left_info(*state.lhs_iterator, 0, state.left_position, lhs_not_null);

		auto &rhs_table = *gstate.table;
		auto &rhs_iterator = *state.rhs_iterator;
		const auto rhs_not_null = SortedChunkNotNull(state.right_chunk_index, rhs_table.count, rhs_table.has_null);
		ChunkMergeInfo right_info(rhs_iterator, state.right_chunk_index, state.right_position, rhs_not_null);

		//	Repin so we don't hang on to data after we have scanned it
		//	Note we only do this for the RHS because the LHS is only one chunk.
		rhs_table.Repin(rhs_iterator);

		idx_t result_count = MergeJoinComplexBlocks(state.sort_key_type, left_info, right_info,
		                                            conditions[0].GetComparisonType(), state.prev_left_index);
		if (result_count == 0) {
			// exhausted this chunk on the right side
			// move to the next right chunk
			state.left_position = 0;
			state.right_position = 0;
			state.right_base += STANDARD_VECTOR_SIZE;
			state.right_chunk_index++;
			if (state.right_chunk_index >= rhs_table.BlockCount()) {
				state.finished = true;
			}
		} else {
			// found matches: extract them
			SliceSortedPayload(state.rhs_input, rhs_table, rhs_iterator, state.rhs_chunk_state, right_info.block_idx,
			                   right_info.rhs, *state.rhs_scan_state);

			chunk.Reset();
			for (idx_t col_idx = 0; col_idx < chunk.ColumnCount(); ++col_idx) {
				if (col_idx < left_cols) {
					chunk.data[col_idx].Slice(state.lhs_payload.data[col_idx], left_info.lhs, result_count);
				} else {
					chunk.data[col_idx].Reference(state.rhs_input.data[col_idx - left_cols]);
				}
			}

			auto sel = FlatVector::IncrementalSelectionVector();
			if (tail_cols) {
				// If there are more expressions to compute,
				// split the result chunk into the left and right halves
				// so we can compute the values for comparison.
				state.rhs_executor.SetChunk(state.rhs_input);
				state.rhs_keys.Reset();

				auto tail_count = result_count;
				for (size_t cmp_idx = 1; cmp_idx < conditions.size(); ++cmp_idx) {
					Vector left(Vector::Ref(state.lhs_local_table->keys.data[cmp_idx]));
					left.Slice(left_info.lhs, result_count);

					auto &right = state.rhs_keys.data[cmp_idx];
					state.rhs_executor.ExecuteExpression(cmp_idx, right);

					if (tail_count < result_count) {
						left.Slice(*sel, tail_count);
						right.Slice(*sel, tail_count);
					}
					tail_count = SelectJoinTail(conditions[cmp_idx].GetComparisonType(), left, right, sel, tail_count,
					                            &state.sel);
					sel = &state.sel;
				}

				if (tail_count < result_count) {
					result_count = tail_count;
					if (result_count == 0) {
						// Need to reset here otherwise we may use the non-flat chunk when constructing LEFT/OUTER
						chunk.Reset();
					} else {
						chunk.Slice(*sel, result_count);
					}
				}
			}

			//	Apply any arbitrary predicate
			if (predicate) {
				result_count = state.pred_executor.SelectExpression(chunk, state.pred_matches);
				chunk.Slice(state.pred_matches, result_count);
				for (idx_t i = 0; i < result_count; i++) {
					state.pred_matches.set_index(i, sel->get_index(state.pred_matches.get_index(i)));
				}
				sel = &state.pred_matches;
			}

			// found matches: mark the found matches if required
			if (state.left_outer.Enabled()) {
				for (idx_t i = 0; i < result_count; i++) {
					state.left_outer.SetMatch(left_info.lhs[sel->get_index(i)]);
				}
			}
			if (gstate.table->found_match) {
				//	Absolute position of the block + start position inside that block
				for (idx_t i = 0; i < result_count; i++) {
					gstate.table->found_match[state.right_base + right_info.rhs[sel->get_index(i)]] = true;
				}
			}
			chunk.Verify(context.client.db);
		}
	} while (chunk.size() == 0);
	return OperatorResultType::HAVE_MORE_OUTPUT;
}

OperatorResultType PhysicalPiecewiseMergeJoin::ExecuteInternal(ExecutionContext &context, DataChunk &input,
                                                               DataChunk &chunk, GlobalOperatorState &gstate_p,
                                                               OperatorState &state) const {
	auto &gstate = sink_state->Cast<MergeJoinGlobalState>();

	if (gstate.Count() == 0) {
		// empty RHS
		if (!EmptyResultIfRHSIsEmpty()) {
			ConstructEmptyJoinResult(join_type, gstate.table->has_null, input, chunk);
			return OperatorResultType::NEED_MORE_INPUT;
		} else {
			return OperatorResultType::FINISHED;
		}
	}

	input.Verify(context.client.db);
	switch (join_type) {
	case JoinType::SEMI:
	case JoinType::ANTI:
	case JoinType::MARK:
		// simple joins can have max STANDARD_VECTOR_SIZE matches per chunk
		ResolveSimpleJoin(context, input, chunk, state);
		return OperatorResultType::NEED_MORE_INPUT;
	case JoinType::LEFT:
	case JoinType::INNER:
	case JoinType::RIGHT:
	case JoinType::OUTER:
		return ResolveComplexJoin(context, input, chunk, state);
	default:
		throw NotImplementedException("Unimplemented type for piecewise merge loop join!");
	}
}

//===--------------------------------------------------------------------===//
// Source
//===--------------------------------------------------------------------===//
class PiecewiseJoinGlobalScanState : public GlobalSourceState {
public:
	explicit PiecewiseJoinGlobalScanState(TupleDataCollection &payload) : payload(payload), right_outer_position(0) {
		payload.InitializeScan(parallel_scan);
	}

	idx_t Scan(TupleDataLocalScanState &local_scan, DataChunk &chunk) {
		lock_guard<mutex> guard(lock);
		const auto result = right_outer_position;
		payload.Scan(parallel_scan, local_scan, chunk);
		right_outer_position += chunk.size();
		return result;
	}

	TupleDataCollection &payload;
	atomic<idx_t> rows_scanned {0};

public:
	idx_t MaxThreads() override {
		return payload.ChunkCount();
	}

private:
	mutex lock;
	TupleDataParallelScanState parallel_scan;
	idx_t right_outer_position;
};

class PiecewiseJoinLocalScanState : public LocalSourceState {
public:
	explicit PiecewiseJoinLocalScanState(PiecewiseJoinGlobalScanState &gstate) : rsel(STANDARD_VECTOR_SIZE) {
		gstate.payload.InitializeScan(scanner);
		gstate.payload.InitializeChunk(rhs_chunk);
	}

	TupleDataLocalScanState scanner;
	DataChunk rhs_chunk;
	SelectionVector rsel;
};

unique_ptr<GlobalSourceState> PhysicalPiecewiseMergeJoin::GetGlobalSourceState(ClientContext &context) const {
	auto &gsink = sink_state->Cast<MergeJoinGlobalState>();
	return make_uniq<PiecewiseJoinGlobalScanState>(*gsink.table->sorted->payload_data);
}

ProgressData PhysicalPiecewiseMergeJoin::GetProgress(ClientContext &context, GlobalSourceState &gstate) const {
	auto &state = gstate.Cast<PiecewiseJoinGlobalScanState>();
	const auto total = state.payload.Count();
	if (total == 0) {
		return ProgressData {1.0, 1.0, false};
	}
	return ProgressData {double(state.rows_scanned.load()), double(total), false};
}

unique_ptr<LocalSourceState> PhysicalPiecewiseMergeJoin::GetLocalSourceState(ExecutionContext &context,
                                                                             GlobalSourceState &gstate) const {
	return make_uniq<PiecewiseJoinLocalScanState>(gstate.Cast<PiecewiseJoinGlobalScanState>());
}

SourceResultType PhysicalPiecewiseMergeJoin::GetDataInternal(ExecutionContext &context, DataChunk &result,
                                                             OperatorSourceInput &source) const {
	D_ASSERT(PropagatesBuildSide(join_type));
	// check if we need to scan any unmatched tuples from the RHS for the full/right outer join
	auto &gsink = sink_state->Cast<MergeJoinGlobalState>();
	auto &gsource = source.global_state.Cast<PiecewiseJoinGlobalScanState>();

	// RHS was empty, so nothing to do?
	if (!gsink.table->count) {
		return SourceResultType::FINISHED;
	}

	// if the LHS is exhausted in a FULL/RIGHT OUTER JOIN, we scan the found_match for any chunks we
	// still need to output
	const auto found_match = gsink.table->found_match.get();

	auto &lsource = source.local_state.Cast<PiecewiseJoinLocalScanState>();
	auto &rhs_chunk = lsource.rhs_chunk;
	auto &rsel = lsource.rsel;
	for (;;) {
		// Read the next sorted chunk
		rhs_chunk.Reset();
		const auto rhs_pos = gsource.Scan(lsource.scanner, rhs_chunk);

		const auto count = rhs_chunk.size();
		if (count == 0) {
			return result.size() == 0 ? SourceResultType::FINISHED : SourceResultType::HAVE_MORE_OUTPUT;
		}

		idx_t result_count = 0;
		// figure out which tuples didn't find a match in the RHS
		for (idx_t i = 0; i < count; i++) {
			if (!found_match[rhs_pos + i]) {
				rsel.set_index(result_count++, i);
			}
		}

		if (result_count > 0) {
			// if there were any tuples that didn't find a match, output them
			const idx_t left_column_count = children[0].get().GetTypes().size();
			for (idx_t col_idx = 0; col_idx < left_column_count; ++col_idx) {
				ConstantVector::SetNull(result.data[col_idx], count_t(result_count));
			}
			const idx_t right_column_count = children[1].get().GetTypes().size();
			for (idx_t col_idx = 0; col_idx < right_column_count; ++col_idx) {
				result.data[left_column_count + col_idx].Slice(rhs_chunk.data[col_idx], rsel, result_count);
			}
		}
		gsource.rows_scanned.fetch_add(count, std::memory_order_relaxed);
		if (result_count > 0) {
			break;
		}
	}

	return result.size() == 0 ? SourceResultType::FINISHED : SourceResultType::HAVE_MORE_OUTPUT;
}

} // namespace duckdb
