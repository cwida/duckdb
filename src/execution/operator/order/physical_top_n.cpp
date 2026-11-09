#include "duckdb/execution/operator/order/physical_top_n.hpp"
#include "duckdb/common/atomic.hpp"

#include "duckdb/common/assert.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/arena_containers/arena_vector.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/create_sort_key.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/planner/filter/table_filter_function_helpers.hpp"

namespace duckdb {

PhysicalTopN::PhysicalTopN(PhysicalPlan &physical_plan, vector<LogicalType> types, vector<BoundOrderByNode> orders,
                           idx_t limit, idx_t offset, shared_ptr<DynamicFilterData> dynamic_filter_p,
                           idx_t estimated_cardinality)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::TOP_N, std::move(types), estimated_cardinality),
      orders(std::move(orders)), limit(limit), offset(offset), dynamic_filter(std::move(dynamic_filter_p)) {
}

PhysicalTopN::~PhysicalTopN() {
}

//===--------------------------------------------------------------------===//
// Heaps
//===--------------------------------------------------------------------===//
class TopNHeap;

struct TopNEntry {
	string_t sort_key;
	idx_t index;

	bool operator<(const TopNEntry &other) const {
		return sort_key < other.sort_key;
	}
};

struct TopNScanState {
	TopNScanState() : pos(0), sel(STANDARD_VECTOR_SIZE) {
	}

	idx_t pos;
	vector<sel_t> scan_order;
	SelectionVector sel;
};

struct TopNBoundaryValue {
	explicit TopNBoundaryValue(const PhysicalTopN &op)
	    : op(op), boundary_vector(op.orders[0].expression->GetReturnType()),
	      boundary_modifiers(op.orders[0].type, op.orders[0].null_order) {
	}

	const PhysicalTopN &op;
	mutex lock;
	string boundary_value;
	bool is_set = false;
	Vector boundary_vector;
	OrderModifiers boundary_modifiers;

	bool TryGetBoundaryValue(string &current_boundary) {
		unique_lock<mutex> l(lock, std::try_to_lock);
		if (!l.owns_lock() || boundary_value == current_boundary) {
			return false;
		}
		current_boundary = boundary_value;
		return true;
	}

	void UpdateValue(string_t boundary_val) {
		unique_lock<mutex> l(lock);
		if (!is_set || boundary_val < string_t(boundary_value)) {
			boundary_value = boundary_val.GetString();
			is_set = true;
			if (op.dynamic_filter) {
				CreateSortKeyHelpers::DecodeSortKey(boundary_val, boundary_vector, 0, boundary_modifiers);
				auto new_dynamic_value = boundary_vector.GetValue(0);
				l.unlock();
				op.dynamic_filter->SetValue(std::move(new_dynamic_value));
			}
		}
	}
};

class TopNHeap {
public:
	TopNHeap(ClientContext &context, const vector<LogicalType> &payload_types, const vector<BoundOrderByNode> &orders,
	         idx_t limit, idx_t offset);
	TopNHeap(ExecutionContext &context, const vector<LogicalType> &payload_types,
	         const vector<BoundOrderByNode> &orders, idx_t limit, idx_t offset);
	TopNHeap(ClientContext &context, Allocator &allocator, const vector<LogicalType> &payload_types,
	         const vector<BoundOrderByNode> &orders, idx_t limit, idx_t offset);

	Allocator &allocator;
	BufferManager &buffer_manager;
	ArenaAllocator arena_allocator;
	unsafe_arena_vector<TopNEntry> heap;
	const vector<LogicalType> &payload_types;
	const vector<BoundOrderByNode> &orders;
	vector<OrderModifiers> modifiers;
	idx_t limit;
	idx_t offset;
	idx_t heap_size;
	ExpressionExecutor executor;
	DataChunk sort_chunk;
	DataChunk heap_data;
	DataChunk payload_chunk;
	DataChunk sort_keys;
	StringHeap sort_key_heap;

	SelectionVector matching_sel;

	DataChunk compare_chunk;
	//! Cached global boundary value as a set of constant vectors
	DataChunk boundary_values;
	//! Cached global boundary value in sort-key format
	string boundary_val;
	SelectionVector final_sel;
	SelectionVector true_sel;
	SelectionVector false_sel;
	SelectionVector new_remaining_sel;

public:
	void Sink(DataChunk &input, optional_ptr<TopNBoundaryValue> boundary_value = nullptr);
	void Combine(TopNHeap &other);
	void Reduce();
	void Finalize();

	void InitializeScan(TopNScanState &state, bool exclude_offset);
	void Scan(TopNScanState &state, DataChunk &chunk, idx_t &pos);

	bool CheckBoundaryValues(DataChunk &sort_chunk, DataChunk &payload, TopNBoundaryValue &boundary_val);
	void AddSmallHeap(DataChunk &input, const Vector &sort_keys_vec);
	void AddLargeHeap(DataChunk &input, const Vector &sort_keys_vec);

public:
	idx_t ReduceThreshold() const {
		return MaxValue<idx_t>(STANDARD_VECTOR_SIZE * 5ULL, 2ULL * heap_size);
	}

	idx_t InitialHeapAllocSize() const {
		return MinValue<idx_t>(STANDARD_VECTOR_SIZE * 100ULL, ReduceThreshold()) + STANDARD_VECTOR_SIZE;
	}

private:
	inline bool EntryShouldBeAdded(const string_t &sort_key) {
		if (heap_size == 0) {
			// the heap has no capacity (LIMIT 0) - no entry can ever be added
			return false;
		}
		if (heap.size() < heap_size) {
			// heap is not full yet - the entry can be added
			return true;
		}
		if (sort_key < heap.front().sort_key) {
			// sort key is smaller than current max value
			return true;
		}
		// heap is full and there is no room for the entry
		return false;
	}

	inline void AddEntryToHeap(const TopNEntry &entry) {
		D_ASSERT(heap_size > 0);
		if (heap.size() >= heap_size) {
			D_ASSERT(!heap.empty());
			std::pop_heap(heap.begin(), heap.end());
			heap.pop_back();
		}
		heap.push_back(entry);
		std::push_heap(heap.begin(), heap.end());
	}
};

//===--------------------------------------------------------------------===//
// TopNHeap
//===--------------------------------------------------------------------===//
TopNHeap::TopNHeap(ClientContext &context, Allocator &allocator, const vector<LogicalType> &payload_types_p,
                   const vector<BoundOrderByNode> &orders_p, idx_t limit, idx_t offset)
    : allocator(allocator), buffer_manager(BufferManager::GetBufferManager(context)), arena_allocator(allocator),
      heap(arena_allocator), payload_types(payload_types_p), orders(orders_p), limit(limit), offset(offset),
      heap_size(limit + offset), executor(context), sort_key_heap(allocator), matching_sel(STANDARD_VECTOR_SIZE),
      final_sel(STANDARD_VECTOR_SIZE), true_sel(STANDARD_VECTOR_SIZE), false_sel(STANDARD_VECTOR_SIZE),
      new_remaining_sel(STANDARD_VECTOR_SIZE) {
	// initialize the executor and the sort_chunk
	vector<LogicalType> sort_types;
	for (auto &order : orders) {
		auto &expr = order.expression;
		sort_types.push_back(expr->GetReturnType());
		executor.AddExpression(*expr);
		modifiers.emplace_back(order.type, order.null_order);
	}
	heap.reserve(InitialHeapAllocSize());
	vector<LogicalType> sort_keys_type {LogicalType::BLOB};
	sort_keys.Initialize(allocator, sort_keys_type);
	heap_data.Initialize(allocator, payload_types, InitialHeapAllocSize());
	payload_chunk.Initialize(allocator, payload_types);
	sort_chunk.Initialize(allocator, sort_types);
	compare_chunk.Initialize(allocator, sort_types);
	boundary_values.Initialize(allocator, sort_types);
}

TopNHeap::TopNHeap(ClientContext &context, const vector<LogicalType> &payload_types,
                   const vector<BoundOrderByNode> &orders, idx_t limit, idx_t offset)
    : TopNHeap(context, BufferAllocator::Get(context), payload_types, orders, limit, offset) {
}

TopNHeap::TopNHeap(ExecutionContext &context, const vector<LogicalType> &payload_types,
                   const vector<BoundOrderByNode> &orders, idx_t limit, idx_t offset)
    : TopNHeap(context.client, BufferAllocator::Get(context.client), payload_types, orders, limit, offset) {
}

void TopNHeap::AddSmallHeap(DataChunk &input, const Vector &sort_keys_vec) {
	// insert the sort keys into the priority queue
	constexpr idx_t BASE_INDEX = NumericLimits<uint32_t>::Maximum();

	bool any_added = false;
	auto sort_key_values = FlatVector::GetData<string_t>(sort_keys_vec);
	for (idx_t r = 0; r < input.size(); r++) {
		auto &sort_key = sort_key_values[r];
		if (!EntryShouldBeAdded(sort_key)) {
			continue;
		}
		// replace the previous top entry with the new entry
		TopNEntry entry;
		entry.sort_key = sort_key;
		entry.index = BASE_INDEX + r;
		AddEntryToHeap(entry);
		any_added = true;
	}
	if (!any_added) {
		// early-out: no matches
		return;
	}

	// for all matching entries we need to copy over the corresponding payload values
	idx_t match_count = 0;
	for (auto &entry : heap) {
		if (entry.index < BASE_INDEX) {
			continue;
		}
		// this entry was added in this chunk
		// copy over the string to the string heap
		entry.sort_key = sort_key_heap.AddBlob(entry.sort_key);
		// to finalize the addition of this entry we need to move over the payload data
		matching_sel.set_index(match_count, entry.index - BASE_INDEX);
		entry.index = heap_data.size() + match_count;
		match_count++;
	}

	// copy over the input rows to the payload chunk
	heap_data.Append(input, matching_sel, match_count, VectorAppendMode::ALLOW_RESIZE);
}

void TopNHeap::AddLargeHeap(DataChunk &input, const Vector &sort_keys_vec) {
	auto sort_key_values = FlatVector::GetData<string_t>(sort_keys_vec);
	idx_t base_index = heap_data.size();
	idx_t match_count = 0;
	for (idx_t r = 0; r < input.size(); r++) {
		auto &sort_key = sort_key_values[r];
		if (!EntryShouldBeAdded(sort_key)) {
			continue;
		}
		// replace the previous top entry with the new entry
		TopNEntry entry;
		entry.sort_key = sort_key_heap.AddBlob(sort_key);
		entry.index = base_index + match_count;
		AddEntryToHeap(entry);
		matching_sel.set_index(match_count++, r);
	}
	if (match_count == 0) {
		// early-out: no matches
		return;
	}

	// copy over the input rows to the payload chunk
	heap_data.Append(input, matching_sel, match_count, VectorAppendMode::ALLOW_RESIZE);
}

bool TopNHeap::CheckBoundaryValues(DataChunk &sort_chunk, DataChunk &payload, TopNBoundaryValue &global_boundary) {
	// An older local boundary remains valid if refreshing it would block.
	const auto boundary_updated = global_boundary.TryGetBoundaryValue(boundary_val);
	if (boundary_val.empty()) {
		// no boundary value (yet) - don't do anything
		return true;
	}
	if (boundary_updated) {
		// new boundary value - decode
		boundary_values.Reset();
		CreateSortKeyHelpers::DecodeSortKey(string_t(boundary_val), boundary_values, 0, modifiers);
		for (auto &col : boundary_values.data) {
			col.SetVectorType(VectorType::CONSTANT_VECTOR);
		}
	}
	boundary_values.SetChildCardinality(sort_chunk.size());

	// we have boundary values
	// from these boundary values, determine which values we should insert (if any)
	idx_t final_count = 0;

	SelectionVector remaining_sel(nullptr);
	idx_t remaining_count = sort_chunk.size();
	for (idx_t i = 0; i < orders.size() && remaining_count > 0; i++) {
		if (remaining_sel.data()) {
			compare_chunk.data[i].Slice(sort_chunk.data[i], remaining_sel, remaining_count);
		} else {
			compare_chunk.data[i].Reference(sort_chunk.data[i]);
		}
		bool is_last = i + 1 == orders.size();
		idx_t true_count;
		if (orders[i].null_order == OrderByNullType::NULLS_LAST) {
			if (orders[i].type == OrderType::ASCENDING) {
				true_count = VectorOperations::DistinctLessThan(compare_chunk.data[i], boundary_values.data[i],
				                                                &remaining_sel, remaining_count, &true_sel, &false_sel);
			} else {
				true_count = VectorOperations::DistinctGreaterThanNullsFirst(compare_chunk.data[i],
				                                                             boundary_values.data[i], &remaining_sel,
				                                                             remaining_count, &true_sel, &false_sel);
			}
		} else {
			D_ASSERT(orders[i].null_order == OrderByNullType::NULLS_FIRST);
			if (orders[i].type == OrderType::ASCENDING) {
				true_count = VectorOperations::DistinctLessThanNullsFirst(compare_chunk.data[i],
				                                                          boundary_values.data[i], &remaining_sel,
				                                                          remaining_count, &true_sel, &false_sel);
			} else {
				true_count =
				    VectorOperations::DistinctGreaterThan(compare_chunk.data[i], boundary_values.data[i],
				                                          &remaining_sel, remaining_count, &true_sel, &false_sel);
			}
		}

		if (true_count > 0) {
			memcpy(final_sel.data() + final_count, true_sel.data(), true_count * sizeof(sel_t));
			final_count += true_count;
		}
		idx_t false_count = remaining_count - true_count;
		if (!is_last && false_count > 0) {
			// check what we should continue to check
			compare_chunk.data[i].Slice(sort_chunk.data[i], false_sel, false_count);
			remaining_count = VectorOperations::NotDistinctFrom(compare_chunk.data[i], boundary_values.data[i],
			                                                    &false_sel, false_count, &new_remaining_sel, nullptr);
			remaining_sel.Initialize(new_remaining_sel);
		} else {
			break;
		}
	}
	if (final_count == 0) {
		return false;
	}
	if (final_count < sort_chunk.size()) {
		sort_chunk.Slice(final_sel, final_count);
		payload.Slice(final_sel, final_count);
	}
	return true;
}

void TopNHeap::Sink(DataChunk &input, optional_ptr<TopNBoundaryValue> global_boundary) {
	static constexpr idx_t SMALL_HEAP_THRESHOLD = 100;

	if (heap_size == 0) {
		// LIMIT 0 without OFFSET - the heap can never hold any entry, so there is nothing to do
		return;
	}

	// compute the ordering values for the new chunk
	sort_chunk.Reset();
	executor.Execute(input, sort_chunk);

	if (global_boundary) {
		// if we have a global boundary value check which rows pass before doing anything
		if (!CheckBoundaryValues(sort_chunk, input, *global_boundary)) {
			// nothing in this chunk can be in the final result
			return;
		}
	}

	// construct the sort key from the sort chunk
	sort_keys.Reset();
	auto &sort_keys_vec = sort_keys.data[0];
	CreateSortKeyHelpers::CreateSortKey(sort_chunk, modifiers, sort_keys_vec);

	if (heap_size <= SMALL_HEAP_THRESHOLD) {
		AddSmallHeap(input, sort_keys_vec);
	} else {
		AddLargeHeap(input, sort_keys_vec);
	}

	// if we modified the heap we might be able to update the global boundary
	// note that the global boundary only applies to FULL heaps
	if (!heap.empty() && heap.size() >= heap_size && global_boundary) {
		global_boundary->UpdateValue(heap.front().sort_key);
	}
}

void TopNHeap::Combine(TopNHeap &other) {
	// "other" is sorted at this point
	idx_t match_count = 0;
	// merge the heap of other into this
	for (idx_t i = 0; i < other.heap.size(); i++) {
		// heap is full - check the latest entry
		auto &other_entry = other.heap[i];
		auto &sort_key = other_entry.sort_key;
		if (!EntryShouldBeAdded(sort_key)) {
			break; // We can break out of the loop here since we sorted the heap
		}
		// add this entry
		TopNEntry new_entry;
		new_entry.sort_key = sort_key_heap.AddBlob(sort_key);
		new_entry.index = heap_data.size() + match_count;
		AddEntryToHeap(new_entry);

		matching_sel.set_index(match_count++, other_entry.index);
		if (match_count >= STANDARD_VECTOR_SIZE) {
			// flush
			heap_data.Append(other.heap_data, matching_sel, match_count, VectorAppendMode::ALLOW_RESIZE);
			match_count = 0;
		}
	}
	if (match_count > 0) {
		// flush
		heap_data.Append(other.heap_data, matching_sel, match_count, VectorAppendMode::ALLOW_RESIZE);
		match_count = 0;
	}
	Reduce();
}

void TopNHeap::Finalize() {
	std::sort(heap.begin(), heap.end());
}

void TopNHeap::Reduce() {
	if (heap_data.size() < ReduceThreshold()) {
		// only reduce when we pass the reduce threshold
		return;
	}
	// we have too many values in the heap - reduce them
	StringHeap new_sort_heap(allocator);
	DataChunk new_heap_data;
	new_heap_data.Initialize(allocator, payload_types, heap.size());

	SelectionVector new_payload_sel(heap.size());
	for (idx_t i = 0; i < heap.size(); i++) {
		auto &entry = heap[i];
		// move the sort key to the new sort heap
		entry.sort_key = new_sort_heap.AddBlob(entry.sort_key);
		// move this heap entry to position X in the payload chunk
		new_payload_sel.set_index(i, entry.index);
		entry.index = i;
	}

	// copy over the data from the current payload chunk to the new payload chunk
	new_heap_data.Slice(heap_data, new_payload_sel, heap.size());
	new_heap_data.Flatten();

	sort_key_heap.Destroy();
	sort_key_heap.Move(new_sort_heap);
	heap_data.Reference(new_heap_data);
}

void TopNHeap::InitializeScan(TopNScanState &state, bool exclude_offset) {
	auto heap_copy = heap;
	state.scan_order.resize(heap_copy.size());

	// we sorted the heap in Finalize(), so we can sequentially here
	// we do this because sorting the heap is much more efficient than popping one by one
	for (idx_t i = 0; i < heap_copy.size(); i++) {
		state.scan_order[i] = UnsafeNumericCast<sel_t>(heap_copy[i].index);
	}

	state.pos = exclude_offset ? offset : 0;
}

void TopNHeap::Scan(TopNScanState &state, DataChunk &chunk, idx_t &pos) {
	if (pos >= state.scan_order.size()) {
		return;
	}
	SelectionVector sel(state.scan_order.data() + pos, state.scan_order.size() - pos);
	idx_t count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, state.scan_order.size() - pos);
	pos += STANDARD_VECTOR_SIZE;

	chunk.Reset();
	chunk.Slice(heap_data, sel, count);
}

class TopNGlobalSinkState : public GlobalSinkState {
public:
	TopNGlobalSinkState(ClientContext &context, const PhysicalTopN &op)
	    : heap(context, op.types, op.orders, op.limit, op.offset), boundary_value(op) {
	}

	mutex lock;
	TopNHeap heap;
	TopNBoundaryValue boundary_value;
};

class TopNLocalSinkState : public LocalSinkState {
public:
	TopNLocalSinkState(ExecutionContext &context, const vector<LogicalType> &payload_types,
	                   const vector<BoundOrderByNode> &orders, idx_t limit, idx_t offset)
	    : heap(context, payload_types, orders, limit, offset) {
	}

	TopNHeap heap;
};

unique_ptr<LocalSinkState> PhysicalTopN::GetLocalSinkState(ExecutionContext &context) const {
	return make_uniq<TopNLocalSinkState>(context, types, orders, limit, offset);
}

unique_ptr<GlobalSinkState> PhysicalTopN::GetGlobalSinkState(ClientContext &context) const {
	if (dynamic_filter) {
		dynamic_filter->Reset();
	}
	return make_uniq<TopNGlobalSinkState>(context, *this);
}

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//
SinkResultType PhysicalTopN::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
	// append to the local sink state
	auto &gstate = input.global_state.Cast<TopNGlobalSinkState>();
	auto &sink = input.local_state.Cast<TopNLocalSinkState>();
	sink.heap.Sink(chunk, &gstate.boundary_value);
	sink.heap.Reduce();
	return SinkResultType::NEED_MORE_INPUT;
}

//===--------------------------------------------------------------------===//
// Combine
//===--------------------------------------------------------------------===//
SinkCombineResultType PhysicalTopN::Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const {
	auto &gstate = input.global_state.Cast<TopNGlobalSinkState>();
	auto &lstate = input.local_state.Cast<TopNLocalSinkState>();

	// scan the local top N and append it to the global heap
	lstate.heap.Finalize();
	lock_guard<mutex> guard(gstate.lock);
	gstate.heap.Combine(lstate.heap);

	return SinkCombineResultType::FINISHED;
}

//===--------------------------------------------------------------------===//
// Finalize
//===--------------------------------------------------------------------===//
SinkFinalizeType PhysicalTopN::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                                        OperatorSinkFinalizeInput &input) const {
	auto &gstate = input.global_state.Cast<TopNGlobalSinkState>();
	// global finalize: compute the final top N
	gstate.heap.Finalize();
	return SinkFinalizeType::READY;
}

//===--------------------------------------------------------------------===//
// Source
//===--------------------------------------------------------------------===//
class TopNLocalSourceState : public LocalSourceState {
public:
	idx_t pos = DConstants::INVALID_INDEX;
	idx_t end = DConstants::INVALID_INDEX;
	idx_t batch_index = DConstants::INVALID_INDEX;
};

class TopNGlobalSourceState : public GlobalSourceState {
public:
	explicit TopNGlobalSourceState(TopNGlobalSinkState &sink_p) : sink(sink_p), batch_index(0) {
		sink.heap.InitializeScan(state, true);
		total_rows = state.scan_order.size() - MinValue<idx_t>(sink.heap.offset, state.scan_order.size());
	}

	idx_t MaxThreads() override {
		return MaxValue<idx_t>(sink.heap.heap.size() / TUPLES_PER_BATCH, 1);
	}

public:
	static constexpr idx_t CHUNKS_PER_BATCH = 60;
	static constexpr idx_t TUPLES_PER_BATCH = CHUNKS_PER_BATCH * STANDARD_VECTOR_SIZE;

	TopNGlobalSinkState &sink;
	TopNScanState state;
	idx_t batch_index;
	idx_t total_rows;
	atomic<idx_t> rows_scanned {0};
};

unique_ptr<GlobalSourceState> PhysicalTopN::GetGlobalSourceState(ClientContext &context) const {
	return make_uniq<TopNGlobalSourceState>(this->sink_state->Cast<TopNGlobalSinkState>());
}

ProgressData PhysicalTopN::GetProgress(ClientContext &context, GlobalSourceState &gstate) const {
	auto &state = gstate.Cast<TopNGlobalSourceState>();
	if (state.total_rows == 0) {
		return ProgressData {1.0, 1.0, false};
	}
	return ProgressData {double(state.rows_scanned.load()), double(state.total_rows), false};
}

unique_ptr<LocalSourceState> PhysicalTopN::GetLocalSourceState(ExecutionContext &context,
                                                               GlobalSourceState &gstate) const {
	return make_uniq<TopNLocalSourceState>();
}

SourceResultType PhysicalTopN::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                               OperatorSourceInput &input) const {
	if (limit == 0) {
		return SourceResultType::FINISHED;
	}
	auto &sink = sink_state->Cast<TopNGlobalSinkState>();
	auto &gstate = input.global_state.Cast<TopNGlobalSourceState>();
	auto &lstate = input.local_state.Cast<TopNLocalSourceState>();

	if (lstate.pos == lstate.end) {
		// Obtain new scan indices from the global state
		annotated_lock_guard<annotated_mutex> guard(gstate.lock);
		lstate.pos = gstate.state.pos;
		gstate.state.pos += TopNGlobalSourceState::TUPLES_PER_BATCH;
		lstate.end = gstate.state.pos;
		lstate.batch_index = gstate.batch_index++;
	}

	sink.heap.Scan(gstate.state, chunk, lstate.pos);
	gstate.rows_scanned.fetch_add(chunk.size(), std::memory_order_relaxed);

	return chunk.size() == 0 ? SourceResultType::FINISHED : SourceResultType::HAVE_MORE_OUTPUT;
}

OperatorPartitionData PhysicalTopN::GetPartitionData(ExecutionContext &context, DataChunk &chunk,
                                                     GlobalSourceState &gstate_p, LocalSourceState &lstate_p,
                                                     const OperatorPartitionInfo &partition_info) const {
	if (partition_info.RequiresPartitionColumns()) {
		throw InternalException("PhysicalOrder::GetPartitionData: partition columns not supported");
	}
	auto &lstate = lstate_p.Cast<TopNLocalSourceState>();
	return OperatorPartitionData(lstate.batch_index);
}

InsertionOrderPreservingMap<string> PhysicalTopN::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	result["Top"] = to_string(limit);
	if (offset > 0) {
		result["Offset"] = to_string(offset);
	}

	string orders_info;
	for (idx_t i = 0; i < orders.size(); i++) {
		if (i > 0) {
			orders_info += "\n";
		}
		orders_info += orders[i].expression->ToString() + " ";
		orders_info += orders[i].type == OrderType::DESCENDING ? "DESC" : "ASC";
	}
	result["Order By"] = orders_info;
	SetEstimatedCardinality(result, estimated_cardinality);
	return result;
}

} // namespace duckdb
