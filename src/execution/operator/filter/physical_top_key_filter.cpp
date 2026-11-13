#include "duckdb/execution/operator/filter/physical_top_key_filter.hpp"

#include "duckdb/common/set.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/create_sort_key.hpp"
#include "duckdb/parallel/thread_context.hpp"
#include "duckdb/planner/filter/table_filter_functions.hpp"
#include "duckdb/storage/buffer_manager.hpp"

namespace duckdb {

PhysicalTopKeyFilter::PhysicalTopKeyFilter(PhysicalPlan &physical_plan, vector<LogicalType> types,
                                           vector<BoundOrderByNode> orders_p, idx_t limit,
                                           shared_ptr<DynamicFilterData> dynamic_filter_p, idx_t estimated_cardinality)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::TOP_KEY_FILTER, std::move(types), estimated_cardinality),
      orders(std::move(orders_p)), limit(limit), dynamic_filter(std::move(dynamic_filter_p)) {
	D_ASSERT(!orders.empty());
}

PhysicalTopKeyFilter::~PhysicalTopKeyFilter() {
}

//===--------------------------------------------------------------------===//
// Global State
//===--------------------------------------------------------------------===//
//! The bound shared by the threads: the smallest of the largest keys of the full local sets, as a sort key
class TopKeyFilterGlobalState : public GlobalOperatorState {
public:
	explicit TopKeyFilterGlobalState(const PhysicalTopKeyFilter &op)
	    : op(op), boundary_vector(op.orders[0].expression->GetReturnType()),
	      boundary_modifiers(op.orders[0].type, op.orders[0].null_order) {
	}

	const PhysicalTopKeyFilter &op;
	mutex lock;
	string boundary_value;
	bool is_set = false;
	Vector boundary_vector;
	OrderModifiers boundary_modifiers;

public:
	bool TryGetBoundaryValue(string &current_boundary) {
		unique_lock<mutex> l(lock, std::try_to_lock);
		if (!l.owns_lock() || boundary_value == current_boundary) {
			return false;
		}
		current_boundary = boundary_value;
		return true;
	}

	void UpdateValue(const string &new_boundary) {
		unique_lock<mutex> l(lock);
		if (is_set && new_boundary >= boundary_value) {
			return;
		}
		boundary_value = new_boundary;
		is_set = true;
		if (!op.dynamic_filter) {
			return;
		}
		// the first key of the bound is the bound of the dynamic filter in the scans
		CreateSortKeyHelpers::DecodeSortKey(string_t(boundary_value), boundary_vector, 0, boundary_modifiers);
		auto new_dynamic_value = boundary_vector.GetValue(0);
		l.unlock();
		if (!new_dynamic_value.IsNull()) {
			op.dynamic_filter->SetValue(std::move(new_dynamic_value));
		}
	}
};

//===--------------------------------------------------------------------===//
// Local State
//===--------------------------------------------------------------------===//
class TopKeyFilterState : public OperatorState {
public:
	//! The number of key hashes that the cache of offered keys remembers
	static constexpr idx_t SEEN_HASH_SLOTS = 4096;
	//! After this many chunks without a change of the local set, a chunk is only offered to it when the number of
	//! idle chunks has doubled. Skipping an offer delays the bound, it never makes the filter drop a row that it has
	//! to keep.
	static constexpr idx_t IDLE_WARMUP_CHUNKS = 16;
	//! A thread offers keys to its set only after it has seen this many rows per key of the limit: filling the set
	//! costs a sort key and an insert per distinct key, which a thread that sees few rows never earns back
	static constexpr idx_t WARMUP_ROWS_PER_KEY = 256;
	//! A thread that still passes more than one in INEFFECTIVE_PASS_FRACTION of this many rows stops filtering: the
	//! limit covers too many of the keys for the filter to pay off
	static constexpr idx_t INEFFECTIVE_CHECK_ROWS = idx_t(100) * STANDARD_VECTOR_SIZE;
	static constexpr idx_t INEFFECTIVE_PASS_FRACTION = 8;

public:
	TopKeyFilterState(ExecutionContext &context, const PhysicalTopKeyFilter &op)
	    : op(op), executor(context.client), seen(SEEN_HASH_SLOTS, 0), hashes(LogicalType::HASH),
	      pass_sel(STANDARD_VECTOR_SIZE), ordered_sel(STANDARD_VECTOR_SIZE), candidate_sel(STANDARD_VECTOR_SIZE),
	      new_sel(STANDARD_VECTOR_SIZE), true_sel(STANDARD_VECTOR_SIZE), false_sel(STANDARD_VECTOR_SIZE),
	      new_remaining_sel(STANDARD_VECTOR_SIZE) {
		vector<LogicalType> key_types;
		for (auto &order : op.orders) {
			key_types.push_back(order.expression->GetReturnType());
			executor.AddExpression(*order.expression);
			modifiers.emplace_back(order.type, order.null_order);
		}
		auto &allocator = BufferManager::GetBufferManager(context.client).GetBufferAllocator();
		key_chunk.Initialize(allocator, key_types);
		boundary_values.Initialize(allocator, key_types);
		local_max_values.Initialize(allocator, key_types);
		compare_chunk.Initialize(allocator, key_types);
		candidates.Initialize(allocator, key_types, vector<bool>(key_types.size(), false));
		sort_key_chunk.Initialize(allocator, {LogicalType::BLOB});
		pass_flags = make_unsafe_uniq_array<bool>(STANDARD_VECTOR_SIZE);
	}

	const PhysicalTopKeyFilter &op;
	ExpressionExecutor executor;
	vector<OrderModifiers> modifiers;
	//! The keys of the current input chunk
	DataChunk key_chunk;
	//! The global bound as this thread last saw it: as a sort key and as constant vectors
	string boundary_val;
	DataChunk boundary_values;
	//! The smallest "limit" distinct sort keys that this thread has seen, and the largest of them as constant vectors
	set<string> heap;
	DataChunk local_max_values;
	//! The hashes of keys that were offered to the set: a hit skips the sort key (a collision only skips an offer)
	vector<hash_t> seen;
	Vector hashes;
	DataChunk candidates;
	DataChunk sort_key_chunk;
	DataChunk compare_chunk;
	//! The number of chunks since the local set last changed
	idx_t idle_chunks = 0;
	//! The number of rows that this thread has seen
	idx_t rows_seen = 0;
	//! The number of rows that this thread has compared with the bound, and how many of them passed
	idx_t checked_rows = 0;
	idx_t passed_rows = 0;
	//! Whether this thread has stopped filtering
	bool pass_through = false;

	SelectionVector pass_sel;
	SelectionVector ordered_sel;
	SelectionVector candidate_sel;
	SelectionVector new_sel;
	SelectionVector true_sel;
	SelectionVector false_sel;
	SelectionVector new_remaining_sel;
	unsafe_unique_array<bool> pass_flags;

public:
	void Finalize(const PhysicalOperator &op, ExecutionContext &context) override {
		context.thread.profiler.Flush(op);
	}

	static void Decode(const string &sort_key, DataChunk &target, const vector<OrderModifiers> &modifiers) {
		target.Reset();
		CreateSortKeyHelpers::DecodeSortKey(string_t(sort_key), target, 0, modifiers);
		for (auto &col : target.data) {
			col.SetVectorType(VectorType::CONSTANT_VECTOR);
		}
	}

	//! Selects the rows whose left value sorts strictly before the right value in the given order
	static idx_t SelectBefore(const BoundOrderByNode &order, Vector &left, Vector &right, SelectionVector &sel,
	                          idx_t count, SelectionVector &true_sel, SelectionVector &false_sel) {
		if (order.null_order == OrderByNullType::NULLS_LAST) {
			if (order.type == OrderType::ASCENDING) {
				return VectorOperations::DistinctLessThan(left, right, &sel, count, &true_sel, &false_sel);
			}
			return VectorOperations::DistinctGreaterThanNullsFirst(left, right, &sel, count, &true_sel, &false_sel);
		}
		if (order.type == OrderType::ASCENDING) {
			return VectorOperations::DistinctLessThanNullsFirst(left, right, &sel, count, &true_sel, &false_sel);
		}
		return VectorOperations::DistinctGreaterThan(left, right, &sel, count, &true_sel, &false_sel);
	}

	//! Selects the rows (of sel, or of the whole chunk) whose keys sort before the bound - or, if inclusive, before or
	//! equal to the bound - in the lexicographic order of the keys. Returns their number; their indexes are written
	//! to result in ascending order if the input selection is ascending.
	idx_t SelectPrefix(DataChunk &bound, bool inclusive, optional_ptr<const SelectionVector> sel, idx_t count,
	                   SelectionVector &result) {
		bound.SetChildCardinality(count);
		idx_t final_count = 0;
		idx_t appended_runs = 0;
		SelectionVector remaining_sel(nullptr);
		if (sel) {
			remaining_sel.Initialize(*sel);
		}
		idx_t remaining_count = count;
		const auto column_count = op.orders.size();
		for (idx_t i = 0; i < column_count && remaining_count > 0; i++) {
			auto &order = op.orders[i];
			if (remaining_sel.data()) {
				compare_chunk.data[i].Slice(key_chunk.data[i], remaining_sel, remaining_count);
			} else {
				compare_chunk.data[i].Reference(key_chunk.data[i]);
			}
			const auto is_last = i + 1 == column_count;
			if (is_last && inclusive) {
				// before or equal to the bound: the rows that the bound does not sort before
				const auto after_count = SelectBefore(order, bound.data[i], compare_chunk.data[i], remaining_sel,
				                                      remaining_count, false_sel, true_sel);
				const auto pass_count = remaining_count - after_count;
				if (pass_count > 0) {
					memcpy(result.data() + final_count, true_sel.data(), pass_count * sizeof(sel_t));
					final_count += pass_count;
					appended_runs++;
				}
				break;
			}
			const auto true_count = SelectBefore(order, compare_chunk.data[i], bound.data[i], remaining_sel,
			                                     remaining_count, true_sel, false_sel);
			if (true_count > 0) {
				memcpy(result.data() + final_count, true_sel.data(), true_count * sizeof(sel_t));
				final_count += true_count;
				appended_runs++;
			}
			const auto false_count = remaining_count - true_count;
			if (false_count == 0 || is_last) {
				break;
			}
			// the rows that are equal on this key continue with the next key
			compare_chunk.data[i].Slice(key_chunk.data[i], false_sel, false_count);
			remaining_count = VectorOperations::NotDistinctFrom(compare_chunk.data[i], bound.data[i], &false_sel,
			                                                    false_count, &new_remaining_sel, nullptr);
			remaining_sel.Initialize(new_remaining_sel);
		}
		if (appended_runs > 1) {
			// restore the order of the rows
			const auto row_count = key_chunk.size();
			memset(pass_flags.get(), 0, row_count * sizeof(bool));
			for (idx_t i = 0; i < final_count; i++) {
				pass_flags[result.get_index(i)] = true;
			}
			idx_t result_idx = 0;
			for (idx_t row_idx = 0; row_idx < row_count; row_idx++) {
				result.set_index(result_idx, row_idx);
				result_idx += pass_flags[row_idx];
			}
			D_ASSERT(result_idx == final_count);
		}
		return final_count;
	}

	//! Offers the rows that passed (sel, or the whole chunk) to the local set and publishes a changed bound
	void MaintainHeap(optional_ptr<const SelectionVector> sel, idx_t count, TopKeyFilterGlobalState &gstate) {
		if (count == 0) {
			return;
		}
		if (idle_chunks >= IDLE_WARMUP_CHUNKS && !IsPowerOfTwo(idle_chunks)) {
			idle_chunks++;
			return;
		}
		// the rows that can change the set: all of them while it is not full, or else those before its largest key
		const auto is_full = heap.size() >= op.limit;
		auto candidate_count = count;
		auto candidates_sel = sel;
		if (is_full) {
			candidate_count = SelectPrefix(local_max_values, false, sel, count, candidate_sel);
			candidates_sel = &candidate_sel;
		}
		if (candidate_count == 0) {
			idle_chunks++;
			return;
		}
		// skip the keys that have been offered before
		key_chunk.Hash(hashes);
		hashes.Flatten();
		auto hash_data = FlatVector::GetData<hash_t>(hashes);
		idx_t new_count = 0;
		for (idx_t i = 0; i < candidate_count; i++) {
			const auto row = candidates_sel ? candidates_sel->get_index(i) : i;
			const auto hash = hash_data[row];
			auto &slot = seen[hash & (SEEN_HASH_SLOTS - 1)];
			if (slot == hash) {
				continue;
			}
			slot = hash;
			new_sel.set_index(new_count++, row);
		}
		if (new_count == 0) {
			idle_chunks++;
			return;
		}
		// create the sort keys of the new rows
		candidates.Reset();
		candidates.Slice(key_chunk, new_sel, new_count);
		sort_key_chunk.Reset();
		CreateSortKeyHelpers::CreateSortKey(candidates, modifiers, sort_key_chunk.data[0]);
		auto sort_keys = FlatVector::GetData<string_t>(sort_key_chunk.data[0]);
		bool changed = false;
		for (idx_t i = 0; i < new_count; i++) {
			auto sort_key = sort_keys[i].GetString();
			if (heap.size() >= op.limit && sort_key >= *heap.rbegin()) {
				continue;
			}
			if (!heap.insert(std::move(sort_key)).second) {
				continue;
			}
			changed = true;
			if (heap.size() > op.limit) {
				heap.erase(std::prev(heap.end()));
			}
		}
		if (!changed) {
			idle_chunks++;
			return;
		}
		idle_chunks = 0;
		if (heap.size() >= op.limit) {
			auto &local_max = *heap.rbegin();
			Decode(local_max, local_max_values, modifiers);
			gstate.UpdateValue(local_max);
		}
	}
};

void PhysicalTopKeyFilter::InitializeOutputChunk(Allocator &allocator, DataChunk &chunk) const {
	// every output column only references or slices the input
	chunk.Initialize(allocator, types, vector<bool>(types.size(), false));
}

unique_ptr<OperatorState> PhysicalTopKeyFilter::GetOperatorState(ExecutionContext &context) const {
	return make_uniq<TopKeyFilterState>(context, *this);
}

unique_ptr<GlobalOperatorState> PhysicalTopKeyFilter::GetGlobalOperatorState(ClientContext &context) const {
	// a new bound for every execution
	if (dynamic_filter) {
		dynamic_filter->Reset();
	}
	return make_uniq<TopKeyFilterGlobalState>(*this);
}

OperatorResultType PhysicalTopKeyFilter::Execute(ExecutionContext &context, DataChunk &input, DataChunk &chunk,
                                                 GlobalOperatorState &gstate_p, OperatorState &state_p) const {
	auto &gstate = gstate_p.Cast<TopKeyFilterGlobalState>();
	auto &state = state_p.Cast<TopKeyFilterState>();
	const auto count = input.size();
	if (count == 0) {
		return OperatorResultType::NEED_MORE_INPUT;
	}
	if (state.pass_through) {
		chunk.Reference(input);
		return OperatorResultType::NEED_MORE_INPUT;
	}
	state.rows_seen += count;
	const auto warming_up = state.rows_seen <= TopKeyFilterState::WARMUP_ROWS_PER_KEY * limit;
	const auto boundary_updated = gstate.TryGetBoundaryValue(state.boundary_val);
	if (warming_up && state.boundary_val.empty()) {
		// no bound to filter with and no key to offer yet: the rows pass as they are
		chunk.Reference(input);
		return OperatorResultType::NEED_MORE_INPUT;
	}
	state.key_chunk.Reset();
	state.executor.Execute(input, state.key_chunk);

	// the rows up to the global bound, as far as this thread knows it
	idx_t pass_count = count;
	if (!state.boundary_val.empty()) {
		if (boundary_updated) {
			TopKeyFilterState::Decode(state.boundary_val, state.boundary_values, state.modifiers);
		}
		pass_count = state.SelectPrefix(state.boundary_values, true, nullptr, count, state.pass_sel);
		if (pass_count == 0) {
			return OperatorResultType::NEED_MORE_INPUT;
		}
		if (!dynamic_filter && !warming_up) {
			// without a dynamic filter in the scans this operator sees every row: if too many of them pass, the
			// filter costs more than the aggregate saves. Passing every row is always correct.
			state.checked_rows += count;
			state.passed_rows += pass_count;
			if (state.checked_rows >= TopKeyFilterState::INEFFECTIVE_CHECK_ROWS) {
				state.pass_through =
				    state.passed_rows > state.checked_rows / TopKeyFilterState::INEFFECTIVE_PASS_FRACTION;
				state.checked_rows = 0;
				state.passed_rows = 0;
			}
		}
	}
	const auto filtered = pass_count < count;
	// offer the rows that pass to the local set. A row after the bound cannot enter it, and a thread that is still
	// warming up only filters with the bound of the other threads.
	if (!warming_up) {
		state.MaintainHeap(filtered ? &state.pass_sel : nullptr, pass_count, gstate);
	}
	if (filtered) {
		chunk.Slice(input, state.pass_sel, pass_count);
	} else {
		chunk.Reference(input);
	}
	return OperatorResultType::NEED_MORE_INPUT;
}

InsertionOrderPreservingMap<string> PhysicalTopKeyFilter::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	string keys;
	for (idx_t i = 0; i < orders.size(); i++) {
		if (i > 0) {
			keys += "\n";
		}
		keys += orders[i].ToString();
	}
	result["Keys"] = keys;
	result["Limit"] = to_string(limit);
	if (dynamic_filter) {
		result["Dynamic Filter"] = "true";
	}
	SetEstimatedCardinality(result, estimated_cardinality);
	return result;
}

} // namespace duckdb
