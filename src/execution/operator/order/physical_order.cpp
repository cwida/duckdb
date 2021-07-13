#include "duckdb/execution/operator/order/physical_order.hpp"

#include "duckdb/common/row_operations/row_operations.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/row_data_collection.hpp"
#include "duckdb/common/types/row_layout.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parallel/task_context.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/storage/statistics/numeric_statistics.hpp"
#include "duckdb/storage/statistics/string_statistics.hpp"

#include <numeric>

namespace duckdb {

using ValidityBytes = RowLayout::ValidityBytes;

PhysicalOrder::PhysicalOrder(vector<LogicalType> types, vector<BoundOrderByNode> orders, idx_t estimated_cardinality)
    : PhysicalSink(PhysicalOperatorType::ORDER_BY, move(types), estimated_cardinality), orders(move(orders)) {
}

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//
static idx_t GetSortingColSize(const LogicalType &type) {
	auto physical_type = type.InternalType();
	if (TypeIsConstantSize(physical_type)) {
		return GetTypeIdSize(physical_type);
	} else {
		switch (physical_type) {
		case PhysicalType::VARCHAR:
			// TODO: make use of statistics
			return string_t::INLINE_LENGTH;
		case PhysicalType::LIST:
			// Lists get another byte to denote the empty list
			return 2 + GetSortingColSize(ListType::GetChildType(type));
		case PhysicalType::MAP:
		case PhysicalType::STRUCT:
			return 1 + GetSortingColSize(StructType::GetChildType(type, 0));
		default:
			throw NotImplementedException("Unable to order column with type %s", type.ToString());
		}
	}
}

struct SortingState {
	explicit SortingState(const vector<BoundOrderByNode> &orders)
	    : column_count(orders.size()), all_constant(true), comparison_size(0), entry_size(0) {
		vector<LogicalType> blob_layout_types;
		for (idx_t i = 0; i < orders.size(); i++) {
			const auto &order = orders[i];

			order_types.push_back(order.type);
			order_by_null_types.push_back(order.null_order);
			auto &expr = *order.expression;
			logical_types.push_back(expr.return_type);

			auto physical_type = expr.return_type.InternalType();
			all_constant = all_constant && TypeIsConstantSize(physical_type);
			constant_size.push_back(TypeIsConstantSize(physical_type));
			column_sizes.push_back(0);
			auto &col_size = column_sizes.back();

			stats.push_back(expr.stats ? expr.stats.get() : nullptr);
			has_null.push_back(true); // TODO: make use of statistics

			col_size += has_null.back() ? 1 : 0;
			if (TypeIsConstantSize(physical_type)) {
				col_size += GetTypeIdSize(physical_type);
			} else {
				col_size += GetSortingColSize(expr.return_type);
				sorting_to_blob_col[i] = blob_layout_types.size();
				blob_layout_types.push_back(expr.return_type);
			}
			comparison_size += col_size;
		}
		entry_size = comparison_size + sizeof(idx_t);
		blob_layout.Initialize(blob_layout_types, false);
	}

	idx_t column_count;
	vector<OrderType> order_types;
	vector<OrderByNullType> order_by_null_types;
	vector<LogicalType> logical_types;

	bool all_constant;
	vector<bool> constant_size;
	vector<idx_t> column_sizes;
	vector<BaseStatistics *> stats;
	vector<bool> has_null;

	idx_t comparison_size;
	idx_t entry_size;

	RowLayout blob_layout;
	unordered_map<idx_t, idx_t> sorting_to_blob_col;
};

class OrderGlobalState : public GlobalOperatorState {
public:
	OrderGlobalState(SortingState sorting_state, RowLayout payload_layout)
	    : sorting_state(move(sorting_state)), payload_layout(move(payload_layout)), total_count(0),
	      sorting_heap_capacity(Storage::BLOCK_SIZE), payload_heap_capacity(Storage::BLOCK_SIZE), external(false) {
		auto thinnest_row = MinValue(sorting_state.entry_size, payload_layout.GetRowWidth());
		if (!sorting_state.all_constant) {
			thinnest_row = MinValue(thinnest_row, sorting_state.blob_layout.GetRowWidth());
		}
		block_capacity = (Storage::BLOCK_SIZE + thinnest_row - 1) / thinnest_row;
	}

	~OrderGlobalState() override;

	//! The lock for updating the order global state
	std::mutex lock;
	//! Constants concerning sorting and payload data
	const SortingState sorting_state;
	const RowLayout payload_layout;

	//! Sorted data
	vector<unique_ptr<SortedBlock>> sorted_blocks;
	vector<vector<unique_ptr<SortedBlock>>> sorted_blocks_temp;
	unique_ptr<SortedBlock> odd_one_out = nullptr;
	//! Pinned heap data (if sorting in memory)
	vector<RowDataBlock> heap_blocks;
	vector<unique_ptr<BufferHandle>> pinned_blocks;

	//! Total count - get set in PhysicalOrder::Finalize
	idx_t total_count;
	//! Capacity (number of rows) used to initialize blocks
	idx_t block_capacity;
	//! Capacity (number of bytes) used to initialize blocks
	idx_t sorting_heap_capacity;
	idx_t payload_heap_capacity;

	//! Whether we are doing an external sort
	bool external;
	//! Progress in merge path stage
	idx_t pair_idx;
	idx_t l_start;
	idx_t r_start;
};

class OrderLocalState : public LocalSinkState {
public:
	OrderLocalState() : initialized(false) {
	}

	//! Whether this local state has been initialized
	bool initialized;
	//! Local copy of the sorting expression executor
	ExpressionExecutor executor;
	//! Holds a vector of incoming sorting columns
	DataChunk sort;

	//! Initialize the local state using the global state
	void Initialize(ClientContext &context, OrderGlobalState &gstate) {
		auto &buffer_manager = BufferManager::GetBufferManager(context);
		auto &sorting_state = gstate.sorting_state;
		auto &payload_layout = gstate.payload_layout;
		// Radix sorting data
		idx_t vectors_per_block =
		    (Storage::BLOCK_SIZE / sorting_state.entry_size + STANDARD_VECTOR_SIZE) / STANDARD_VECTOR_SIZE;
		radix_sorting_data = make_unique<RowDataCollection>(buffer_manager, vectors_per_block * STANDARD_VECTOR_SIZE,
		                                                    sorting_state.entry_size);
		// Blob sorting data
		if (!sorting_state.all_constant) {
			auto blob_row_width = sorting_state.blob_layout.GetRowWidth();
			vectors_per_block = (Storage::BLOCK_SIZE / blob_row_width + STANDARD_VECTOR_SIZE) / STANDARD_VECTOR_SIZE;
			blob_sorting_data = make_unique<RowDataCollection>(buffer_manager, vectors_per_block * STANDARD_VECTOR_SIZE,
			                                                   blob_row_width);
			blob_sorting_heap = make_unique<RowDataCollection>(buffer_manager, (idx_t)Storage::BLOCK_SIZE, 1, true);
		}
		// Payload data
		auto payload_row_width = payload_layout.GetRowWidth();
		vectors_per_block = (Storage::BLOCK_SIZE / payload_row_width + STANDARD_VECTOR_SIZE) / STANDARD_VECTOR_SIZE;
		payload_data =
		    make_unique<RowDataCollection>(buffer_manager, vectors_per_block * STANDARD_VECTOR_SIZE, payload_row_width);
		payload_heap = make_unique<RowDataCollection>(buffer_manager, (idx_t)Storage::BLOCK_SIZE, 1, true);
		// Init done
		initialized = true;
	}

	//! Whether the localstate has collected enough data to perform an external sort
	bool Full(ClientContext &context, const SortingState &sorting_state, const RowLayout &payload_layout) {
		// Compute the size of the collected data (in bytes)
		idx_t size_in_bytes = radix_sorting_data->count * sorting_state.entry_size;
		if (!sorting_state.all_constant) {
			size_in_bytes += blob_sorting_data->count * sorting_state.blob_layout.GetRowWidth();
			for (auto &block : blob_sorting_heap->blocks) {
				size_in_bytes += block.byte_offset;
			}
		}
		size_in_bytes += payload_data->count * payload_layout.GetRowWidth();
		if (!payload_layout.AllConstant()) {
			for (auto &block : payload_data->blocks) {
				size_in_bytes += block.byte_offset;
			}
		}
		// Get the max memory and number of threads
		auto &buffer_manager = BufferManager::GetBufferManager(context);
		auto &task_scheduler = TaskScheduler::GetScheduler(context);
		idx_t max_memory = buffer_manager.GetMaxMemory();
		idx_t num_threads = task_scheduler.NumberOfThreads();
		// Memory usage per thread should scale with max mem / num threads
		// We take 15% of the max memory, to be VERY conservative
		return size_in_bytes > (0.15 * max_memory / num_threads);
	}

	//! Radix/memcmp sortable data
	unique_ptr<RowDataCollection> radix_sorting_data;
	//! Variable sized sorting data and accompanying heap
	unique_ptr<RowDataCollection> blob_sorting_data;
	unique_ptr<RowDataCollection> blob_sorting_heap;
	//! Payload data and accompanying heap
	unique_ptr<RowDataCollection> payload_data;
	unique_ptr<RowDataCollection> payload_heap;
	//! Sorted data
	vector<unique_ptr<SortedBlock>> sorted_blocks;
	//! Constant buffers allocated for vector serialization
	const SelectionVector &sel_ptr = FlatVector::INCREMENTAL_SELECTION_VECTOR;
	Vector addresses = Vector(LogicalType::POINTER);
	idx_t entry_sizes[STANDARD_VECTOR_SIZE];
};

unique_ptr<GlobalOperatorState> PhysicalOrder::GetGlobalState(ClientContext &context) {
	RowLayout payload_layout;
	payload_layout.Initialize(types, false);
	auto state = make_unique<OrderGlobalState>(SortingState(orders), payload_layout);
	state->external = context.force_external;
	return move(state);
}

unique_ptr<LocalSinkState> PhysicalOrder::GetLocalSinkState(ExecutionContext &context) {
	auto result = make_unique<OrderLocalState>();
	vector<LogicalType> types;
	for (auto &order : orders) {
		types.push_back(order.expression->return_type);
		result->executor.AddExpression(*order.expression);
	}
	result->sort.Initialize(types);
	return move(result);
}

void PhysicalOrder::Sink(ExecutionContext &context, GlobalOperatorState &gstate_p, LocalSinkState &lstate_p,
                         DataChunk &input) const {
	auto &gstate = (OrderGlobalState &)gstate_p;
	auto &lstate = (OrderLocalState &)lstate_p;
	const auto &sorting_state = gstate.sorting_state;
	const auto &payload_layout = gstate.payload_layout;

	if (!lstate.initialized) {
		lstate.Initialize(context.client, gstate);
	}

	// Obtain sorting columns
	auto &sort = lstate.sort;
	lstate.executor.Execute(input, sort);

	// Build and serialize sorting data to radix sortable rows
	auto data_pointers = FlatVector::GetData<data_ptr_t>(lstate.addresses);
	lstate.radix_sorting_data->Build(sort.size(), data_pointers, nullptr);
	for (idx_t sort_col = 0; sort_col < sort.ColumnCount(); sort_col++) {
		bool has_null = sorting_state.has_null[sort_col];
		bool nulls_first = sorting_state.order_by_null_types[sort_col] == OrderByNullType::NULLS_FIRST;
		bool desc = sorting_state.order_types[sort_col] == OrderType::DESCENDING;
		// TODO: use actual string statistics
		lstate.radix_sorting_data->SerializeVectorSortable(
		    sort.data[sort_col], sort.size(), lstate.sel_ptr, sort.size(), data_pointers, desc, has_null, nulls_first,
		    string_t::INLINE_LENGTH, sorting_state.column_sizes[sort_col]);
	}

	// Also fully serialize blob sorting columns (to be able to break ties
	if (!sorting_state.all_constant) {
		DataChunk blob_chunk;
		blob_chunk.SetCardinality(sort.size());
		for (idx_t sort_col = 0; sort_col < sort.ColumnCount(); sort_col++) {
			if (!TypeIsConstantSize(sort.data[sort_col].GetType().InternalType())) {
				blob_chunk.data.emplace_back(sort.data[sort_col]);
			}
		}
		lstate.blob_sorting_data->Build(blob_chunk.size(), data_pointers, nullptr);
		auto blob_data = blob_chunk.Orrify();
		RowOperations::Scatter(blob_chunk, blob_data.get(), sorting_state.blob_layout, lstate.addresses,
		                       *lstate.blob_sorting_heap, lstate.sel_ptr, blob_chunk.size());
	}

	// Finally, serialize payload data
	lstate.payload_data->Build(input.size(), data_pointers, nullptr);
	auto input_data = input.Orrify();
	RowOperations::Scatter(input, input_data.get(), payload_layout, lstate.addresses, *lstate.payload_heap,
	                       lstate.sel_ptr, input.size());

	// When sorting data reaches a certain size, we sort it
	if (lstate.Full(context.client, sorting_state, payload_layout)) {
		SortLocalState(context.client, lstate, gstate);
	}
}

void PhysicalOrder::Combine(ExecutionContext &context, GlobalOperatorState &gstate_p, LocalSinkState &lstate_p) {
	auto &gstate = (OrderGlobalState &)gstate_p;
	auto &lstate = (OrderLocalState &)lstate_p;
	if (!lstate.radix_sorting_data) {
		return;
	}

	SortLocalState(context.client, lstate, gstate);
	lock_guard<mutex> append_lock(gstate.lock);
	for (auto &cb : lstate.sorted_blocks) {
		gstate.sorted_blocks.push_back(move(cb));
	}
}

//! Object that holds sorted rows, and an accompanying heap if there are blobs
struct SortedData {
public:
	SortedData(const RowLayout &layout, BufferManager &buffer_manager, OrderGlobalState &state)
	    : layout(layout), block_idx(0), entry_idx(0), buffer_manager(buffer_manager), state(state) {
	}
	//! Number of rows that this object holds
	idx_t Count() {
		idx_t count = std::accumulate(data_blocks.begin(), data_blocks.end(), (idx_t)0,
		                              [](idx_t a, const RowDataBlock &b) { return a + b.count; });
		if (!layout.AllConstant() && state.external) {
			D_ASSERT(count == std::accumulate(heap_blocks.begin(), heap_blocks.end(), (idx_t)0,
			                                  [](idx_t a, const RowDataBlock &b) { return a + b.count; }));
		}
		return count;
	}
	//! Pin the current block such that it can be read
	void Pin() {
		PinData();
		if (!layout.AllConstant() && state.external) {
			PinHeap();
		}
	}
	//! Pointer to the row that is currently being read from
	inline data_ptr_t DataPtr() const {
		D_ASSERT(data_blocks[block_idx].block->Readers() != 0 &&
		         data_handle->handle->BlockId() == data_blocks[block_idx].block->BlockId());
		return data_ptr + entry_idx * layout.GetRowWidth();
	}
	//! Pointer to the heap row that corresponds to the current row
	inline data_ptr_t HeapPtr() const {
		D_ASSERT(!layout.AllConstant() && state.external);
		D_ASSERT(heap_blocks[block_idx].block->Readers() != 0 &&
		         heap_handle->handle->BlockId() == heap_blocks[block_idx].block->BlockId());
		return heap_ptr + Load<idx_t>(DataPtr() + layout.GetHeapPointerOffset());
	}
	//! Advance one row
	inline void Advance(const bool &adv) {
		entry_idx += adv;
		if (entry_idx == data_blocks[block_idx].count) {
			block_idx++;
			entry_idx = 0;
			if (block_idx < data_blocks.size()) {
				Pin();
			} else {
				UnpinAndReset(block_idx, entry_idx);
				return;
			}
		}
	}
	//! Initialize new block to write to
	void CreateBlock() {
		data_blocks.emplace_back(buffer_manager, state.block_capacity, layout.GetRowWidth());
		if (!layout.AllConstant() && state.external) {
			heap_blocks.emplace_back(buffer_manager, heap_capacity, 1);
			D_ASSERT(data_blocks.size() == heap_blocks.size());
		}
	}
	//! Unpin blocks and reset read indices to the given indices
	void UnpinAndReset(idx_t block_idx_to, idx_t entry_idx_to) {
		data_handle = nullptr;
		heap_handle = nullptr;
		data_ptr = nullptr;
		heap_ptr = nullptr;
		block_idx = block_idx_to;
		entry_idx = entry_idx_to;
	}
	//! Create a slice that holds the rows between the start and end indices
	unique_ptr<SortedData> CreateSlice(idx_t start_block_index, idx_t start_entry_index, idx_t end_block_index,
	                                   idx_t end_entry_index) {
		// Add the corresponding blocks to the result
		auto result = make_unique<SortedData>(layout, buffer_manager, state);
		for (idx_t i = start_block_index; i <= end_block_index; i++) {
			result->data_blocks.push_back(data_blocks[i]);
			if (!layout.AllConstant() && state.external) {
				result->heap_blocks.push_back(heap_blocks[i]);
			}
		}
		// Use start and end entry indices to set the boundaries
		result->entry_idx = start_entry_index;
		D_ASSERT(end_entry_index <= result->data_blocks.back().count);
		result->data_blocks.back().count = end_entry_index;
		if (!layout.AllConstant() && state.external) {
			result->heap_blocks.back().count = end_entry_index;
		}
		return result;
	}

	void Unswizzle() {
		if (layout.AllConstant()) {
			return;
		}
		for (idx_t i = 0; i < data_blocks.size(); i++) {
			auto &data_block = data_blocks[i];
			auto &heap_block = heap_blocks[i];
			auto data_handle_p = buffer_manager.Pin(data_block.block);
			auto heap_handle_p = buffer_manager.Pin(heap_block.block);
			RowOperations::UnswizzleHeapPointer(layout, data_handle_p->Ptr(), heap_handle_p->Ptr(), data_block.count);
			RowOperations::UnswizzleColumns(layout, data_handle_p->Ptr(), data_block.count);
			state.heap_blocks.push_back(move(heap_block));
			state.pinned_blocks.push_back(move(heap_handle_p));
		}
		heap_blocks.clear();
	}

	//! Layout of this data
	const RowLayout layout;
	//! Data and heap blocks
	vector<RowDataBlock> data_blocks;
	vector<RowDataBlock> heap_blocks;
	//! Capacity (in bytes) of the heap blocks
	idx_t heap_capacity;
	//! Read indices
	idx_t block_idx;
	idx_t entry_idx;

private:
	//! Pin fixed-size row data
	void PinData() {
		D_ASSERT(block_idx < data_blocks.size());
		data_handle = buffer_manager.Pin(data_blocks[block_idx].block);
		data_ptr = data_handle->Ptr();
	}
	//! Pin the accompanying heap data (if any)
	void PinHeap() {
		D_ASSERT(!layout.AllConstant() && state.external);
		heap_handle = buffer_manager.Pin(heap_blocks[block_idx].block);
		heap_ptr = heap_handle->Ptr();
	}

	//! The buffer manager
	BufferManager &buffer_manager;
	//! The global state
	OrderGlobalState &state;
	//! Buffer handles to the data being currently read
	unique_ptr<BufferHandle> data_handle;
	unique_ptr<BufferHandle> heap_handle;
	//! Pointers into the buffers being currently read
	data_ptr_t data_ptr;
	data_ptr_t heap_ptr;
};

//! Block that holds sorted rows: radix, blob and payload data
struct SortedBlock {
public:
	SortedBlock(BufferManager &buffer_manager, OrderGlobalState &state)
	    : block_idx(0), entry_idx(0), buffer_manager(buffer_manager), state(state), sorting_state(state.sorting_state),
	      payload_layout(state.payload_layout) {
		blob_sorting_data = make_unique<SortedData>(sorting_state.blob_layout, buffer_manager, state);
		payload_data = make_unique<SortedData>(payload_layout, buffer_manager, state);
	}
	//! Number of rows that this object holds
	idx_t Count() {
		idx_t count = std::accumulate(radix_sorting_data.begin(), radix_sorting_data.end(), 0,
		                              [](idx_t a, const RowDataBlock &b) { return a + b.count; });
		if (!sorting_state.all_constant) {
			D_ASSERT(count == blob_sorting_data->Count());
		}
		D_ASSERT(count == payload_data->Count());
		return count;
	}
	//! The remaining number of rows to be read from this object
	idx_t Remaining() {
		idx_t remaining = 0;
		if (block_idx < radix_sorting_data.size()) {
			remaining += radix_sorting_data[block_idx].count - entry_idx;
			for (idx_t i = block_idx + 1; i < radix_sorting_data.size(); i++) {
				remaining += radix_sorting_data[i].count;
			}
		}
		return remaining;
	}
	//! Initialize this block to write data to
	void InitializeWrite() {
		CreateBlock();
		if (!sorting_state.all_constant) {
			blob_sorting_data->heap_capacity = state.sorting_heap_capacity;
			blob_sorting_data->CreateBlock();
		}
		payload_data->heap_capacity = state.payload_heap_capacity;
		payload_data->CreateBlock();
	}
	//! Init new block to write to
	void CreateBlock() {
		radix_sorting_data.emplace_back(buffer_manager, state.block_capacity, sorting_state.entry_size);
	}
	//! Cleanup sorting data
	void UnregisterSortingBlocks() {
		for (auto &block : radix_sorting_data) {
			buffer_manager.UnregisterBlock(block.block->BlockId(), true);
		}
		if (!sorting_state.all_constant) {
			for (auto &block : blob_sorting_data->data_blocks) {
				buffer_manager.UnregisterBlock(block.block->BlockId(), true);
			}
			if (state.external) {
				for (auto &block : blob_sorting_data->heap_blocks) {
					buffer_manager.UnregisterBlock(block.block->BlockId(), true);
				}
			}
		}
	}
	//! Cleanup payload data
	void UnregisterPayloadBlocks() {
		for (auto &block : payload_data->data_blocks) {
			buffer_manager.UnregisterBlock(block.block->BlockId(), true);
		}
		if (state.external) {
			if (!payload_data->layout.AllConstant()) {
				for (auto &block : payload_data->heap_blocks) {
					buffer_manager.UnregisterBlock(block.block->BlockId(), true);
				}
			}
		}
	}
	//! Fill this sorted block by appending the blocks held by a vector of sorted blocks
	void AppendSortedBlocks(vector<unique_ptr<SortedBlock>> &sorted_blocks) {
		D_ASSERT(Count() == 0);
		for (auto &sb : sorted_blocks) {
			for (auto &radix_block : sb->radix_sorting_data) {
				radix_sorting_data.push_back(move(radix_block));
			}
			if (!sorting_state.all_constant) {
				for (auto &blob_block : sb->blob_sorting_data->data_blocks) {
					blob_sorting_data->data_blocks.push_back(move(blob_block));
				}
				for (auto &heap_block : sb->blob_sorting_data->heap_blocks) {
					blob_sorting_data->heap_blocks.push_back(move(heap_block));
				}
			}
			for (auto &payload_data_block : sb->payload_data->data_blocks) {
				payload_data->data_blocks.push_back(move(payload_data_block));
			}
			if (!payload_data->layout.AllConstant()) {
				for (auto &payload_heap_block : sb->payload_data->heap_blocks) {
					payload_data->heap_blocks.push_back(move(payload_heap_block));
				}
			}
		}
	}
	//! Locate the block and entry index of a row in this block,
	//! given an index between 0 and the total number of rows in this block
	void GlobalToLocalIndex(const idx_t &global_idx, idx_t &local_block_index, idx_t &local_entry_index) {
		if (global_idx == Count()) {
			local_block_index = radix_sorting_data.size() - 1;
			local_entry_index = radix_sorting_data.back().count;
			return;
		}
		D_ASSERT(global_idx < Count());
		local_entry_index = global_idx;
		for (local_block_index = 0; local_block_index < radix_sorting_data.size(); local_block_index++) {
			const idx_t &block_count = radix_sorting_data[local_block_index].count;
			if (local_entry_index >= block_count) {
				local_entry_index -= block_count;
			} else {
				break;
			}
		}
		D_ASSERT(local_entry_index < radix_sorting_data[local_block_index].count);
	}
	//! Create a slice that holds the rows between the start and end indices
	unique_ptr<SortedBlock> CreateSlice(const idx_t start, const idx_t end) {
		// Identify blocks/entry indices of this slice
		idx_t start_block_index;
		idx_t start_entry_index;
		GlobalToLocalIndex(start, start_block_index, start_entry_index);
		idx_t end_block_index;
		idx_t end_entry_index;
		GlobalToLocalIndex(end, end_block_index, end_entry_index);
		// Add the corresponding blocks to the result
		auto result = make_unique<SortedBlock>(buffer_manager, state);
		for (idx_t i = start_block_index; i <= end_block_index; i++) {
			result->radix_sorting_data.push_back(radix_sorting_data[i]);
		}
		// Use start and end entry indices to set the boundaries
		result->entry_idx = start_entry_index;
		D_ASSERT(end_entry_index <= result->radix_sorting_data.back().count);
		result->radix_sorting_data.back().count = end_entry_index;
		// Same for the var size sorting data
		if (!sorting_state.all_constant) {
			result->blob_sorting_data =
			    blob_sorting_data->CreateSlice(start_block_index, start_entry_index, end_block_index, end_entry_index);
		}
		// And the payload data
		result->payload_data =
		    payload_data->CreateSlice(start_block_index, start_entry_index, end_block_index, end_entry_index);
		D_ASSERT(result->Remaining() == end - start);
		return result;
	}

	idx_t HeapSize() const {
		idx_t result = 0;
		if (!sorting_state.all_constant) {
			for (auto &block : blob_sorting_data->heap_blocks) {
				result += block.capacity;
			}
		}
		if (!payload_layout.AllConstant()) {
			for (auto &block : payload_data->heap_blocks) {
				result += block.capacity;
			}
		}
		return result;
	}

public:
	//! Radix/memcmp sortable data
	vector<RowDataBlock> radix_sorting_data;
	idx_t block_idx;
	idx_t entry_idx;
	//! Variable sized sorting data
	unique_ptr<SortedData> blob_sorting_data;
	//! Payload data
	unique_ptr<SortedData> payload_data;

private:
	//! Buffer manager, and sorting state constants
	BufferManager &buffer_manager;
	OrderGlobalState &state;
	const SortingState &sorting_state;
	const RowLayout &payload_layout;

	//! Handle and ptr for sorting_blocks
	unique_ptr<BufferHandle> sorting_handle;
};

OrderGlobalState::~OrderGlobalState() {
	std::lock_guard<mutex> glock(lock);
	for (auto &sb : sorted_blocks) {
		sb->UnregisterPayloadBlocks();
	}
	sorted_blocks.clear();
}

//! Concatenates the blocks in a RowDataCollection into a single block
static RowDataBlock ConcatenateBlocks(BufferManager &buffer_manager, RowDataCollection &row_data) {
	// Create block with the correct capacity
	const idx_t &entry_size = row_data.entry_size;
	idx_t capacity = MaxValue(Storage::BLOCK_SIZE / entry_size + 1, row_data.count);
	RowDataBlock new_block(buffer_manager, capacity, entry_size);
	new_block.count = row_data.count;
	auto new_block_handle = buffer_manager.Pin(new_block.block);
	data_ptr_t new_block_ptr = new_block_handle->Ptr();
	// Copy the data of the blocks into a single block
	for (auto &block : row_data.blocks) {
		auto block_handle = buffer_manager.Pin(block.block);
		memcpy(new_block_ptr, block_handle->Ptr(), block.count * entry_size);
		new_block_ptr += block.count * entry_size;
		buffer_manager.UnregisterBlock(block.block->BlockId(), true);
	}
	row_data.blocks.clear();
	row_data.count = 0;
	return new_block;
}

//! Whether a tie between two blobs can be broken
static inline bool TieIsBreakable(const idx_t &col_idx, const data_ptr_t row_ptr, const RowLayout &row_layout) {
	// Check if the blob is NULL
	ValidityBytes row_mask(row_ptr);
	idx_t entry_idx;
	idx_t idx_in_entry;
	ValidityBytes::GetEntryIndex(col_idx, entry_idx, idx_in_entry);
	if (!row_mask.RowIsValid(row_mask.GetValidityEntry(entry_idx), idx_in_entry)) {
		// Can't break a NULL tie
		return false;
	}
	if (row_layout.GetTypes()[col_idx].InternalType() == PhysicalType::VARCHAR) {
		const auto &tie_col_offset = row_layout.GetOffsets()[col_idx];
		string_t tie_string = Load<string_t>(row_ptr + tie_col_offset);
		if (tie_string.GetSize() < string_t::INLINE_LENGTH) {
			// No need to break the tie - we already compared the full string
			return false;
		}
	}
	return true;
}

template <class T>
static inline int TemplatedCompareVal(const data_ptr_t &left_ptr, const data_ptr_t &right_ptr) {
	const auto left_val = Load<T>(left_ptr);
	const auto right_val = Load<T>(right_ptr);
	if (Equals::Operation<T>(left_val, right_val)) {
		return 0;
	} else if (LessThan::Operation<T>(left_val, right_val)) {
		return -1;
	} else {
		return 1;
	}
}

template <class T>
static inline int TemplatedCompareAndAdvance(data_ptr_t &left_ptr, data_ptr_t &right_ptr) {
	auto result = TemplatedCompareVal<T>(left_ptr, right_ptr);
	left_ptr += sizeof(T);
	right_ptr += sizeof(T);
	return result;
}

static inline int CompareStringAndAdvance(data_ptr_t &left_ptr, data_ptr_t &right_ptr) {
	// Construct the string_t
	uint32_t left_string_size = Load<uint32_t>(left_ptr);
	uint32_t right_string_size = Load<uint32_t>(right_ptr);
	left_ptr += sizeof(uint32_t);
	right_ptr += sizeof(uint32_t);
	string_t left_val((const char *)left_ptr, left_string_size);
	string_t right_val((const char *)right_ptr, left_string_size);
	left_ptr += left_string_size;
	right_ptr += right_string_size;
	// Compare
	return TemplatedCompareVal<string_t>((data_ptr_t)&left_val, (data_ptr_t)&right_val);
}

template <class T>
static inline int TemplatedCompareListLoop(data_ptr_t &left_ptr, data_ptr_t &right_ptr,
                                           const ValidityBytes &left_validity, const ValidityBytes &right_validity,
                                           const idx_t &count) {
	int comp_res = 0;
	bool left_valid;
	bool right_valid;
	idx_t entry_idx;
	idx_t idx_in_entry;
	for (idx_t i = 0; i < count; i++) {
		ValidityBytes::GetEntryIndex(i, entry_idx, idx_in_entry);
		left_valid = left_validity.RowIsValid(left_validity.GetValidityEntry(entry_idx), idx_in_entry);
		right_valid = right_validity.RowIsValid(right_validity.GetValidityEntry(entry_idx), idx_in_entry);
		comp_res = TemplatedCompareAndAdvance<T>(left_ptr, right_ptr);
		if (!left_valid && !right_valid) {
			comp_res = 0;
		} else if (!left_valid) {
			comp_res = 1;
		} else if (!right_valid) {
			comp_res = -1;
		}
		if (comp_res != 0) {
			break;
		}
	}
	return comp_res;
}

//! Compares two struct values at the given pointers (recursive)
static inline int CompareStructAndAdvance(data_ptr_t &left_ptr, data_ptr_t &right_ptr,
                                          const child_list_t<LogicalType> &types) {
	idx_t count = types.size();
	// Load validity masks
	ValidityBytes left_validity(left_ptr);
	ValidityBytes right_validity(right_ptr);
	left_ptr += (count + 7) / 8;
	right_ptr += (count + 7) / 8;
	// Initialize variables
	bool left_valid;
	bool right_valid;
	idx_t entry_idx;
	idx_t idx_in_entry;
	// Compare
	int comp_res = 0;
	for (idx_t i = 0; i < count; i++) {
		ValidityBytes::GetEntryIndex(i, entry_idx, idx_in_entry);
		left_valid = left_validity.RowIsValid(left_validity.GetValidityEntry(entry_idx), idx_in_entry);
		right_valid = right_validity.RowIsValid(right_validity.GetValidityEntry(entry_idx), idx_in_entry);
		auto &type = types[i].second;
		if ((left_valid && right_valid) || TypeIsConstantSize(type.InternalType())) {
			comp_res = PhysicalOrder::CompareValAndAdvance(left_ptr, right_ptr, types[i].second);
		}
		if (!left_valid && !right_valid) {
			comp_res = 0;
		} else if (!left_valid) {
			comp_res = 1;
		} else if (!right_valid) {
			comp_res = -1;
		}
		if (comp_res != 0) {
			break;
		}
	}
	return comp_res;
}

//! Compare two list values at the pointers (can be recursive if nested type)
static inline int CompareListAndAdvance(data_ptr_t &left_ptr, data_ptr_t &right_ptr, const LogicalType &type) {
	// Load list lengths
	auto left_len = Load<idx_t>(left_ptr);
	auto right_len = Load<idx_t>(right_ptr);
	left_ptr += sizeof(idx_t);
	right_ptr += sizeof(idx_t);
	// Load list validity masks
	ValidityBytes left_validity(left_ptr);
	ValidityBytes right_validity(right_ptr);
	left_ptr += (left_len + 7) / 8;
	right_ptr += (right_len + 7) / 8;
	// Compare
	int comp_res = 0;
	idx_t count = MinValue(left_len, right_len);
	if (TypeIsConstantSize(type.InternalType())) {
		// Templated code for fixed-size types
		switch (type.InternalType()) {
		case PhysicalType::BOOL:
		case PhysicalType::INT8:
			comp_res = TemplatedCompareListLoop<int8_t>(left_ptr, right_ptr, left_validity, right_validity, count);
			break;
		case PhysicalType::INT16:
			comp_res = TemplatedCompareListLoop<int16_t>(left_ptr, right_ptr, left_validity, right_validity, count);
			break;
		case PhysicalType::INT32:
			comp_res = TemplatedCompareListLoop<int32_t>(left_ptr, right_ptr, left_validity, right_validity, count);
			break;
		case PhysicalType::INT64:
			comp_res = TemplatedCompareListLoop<int64_t>(left_ptr, right_ptr, left_validity, right_validity, count);
			break;
		case PhysicalType::UINT8:
			comp_res = TemplatedCompareListLoop<uint8_t>(left_ptr, right_ptr, left_validity, right_validity, count);
			break;
		case PhysicalType::UINT16:
			comp_res = TemplatedCompareListLoop<uint16_t>(left_ptr, right_ptr, left_validity, right_validity, count);
			break;
		case PhysicalType::UINT32:
			comp_res = TemplatedCompareListLoop<uint32_t>(left_ptr, right_ptr, left_validity, right_validity, count);
			break;
		case PhysicalType::UINT64:
			comp_res = TemplatedCompareListLoop<uint64_t>(left_ptr, right_ptr, left_validity, right_validity, count);
			break;
		case PhysicalType::INT128:
			comp_res = TemplatedCompareListLoop<hugeint_t>(left_ptr, right_ptr, left_validity, right_validity, count);
			break;
		case PhysicalType::FLOAT:
			comp_res = TemplatedCompareListLoop<float>(left_ptr, right_ptr, left_validity, right_validity, count);
			break;
		case PhysicalType::DOUBLE:
			comp_res = TemplatedCompareListLoop<double>(left_ptr, right_ptr, left_validity, right_validity, count);
			break;
		case PhysicalType::INTERVAL:
			comp_res = TemplatedCompareListLoop<interval_t>(left_ptr, right_ptr, left_validity, right_validity, count);
			break;
		default:
			throw NotImplementedException("CompareListAndAdvance for fixed-size type %s", type.ToString());
		}
	} else {
		// Variable-sized list entries
		bool left_valid;
		bool right_valid;
		idx_t entry_idx;
		idx_t idx_in_entry;
		// Size (in bytes) of all variable-sizes entries is stored before the entries begin,
		// to make deserialization easier. We need to skip over them
		left_ptr += left_len * sizeof(idx_t);
		right_ptr += right_len * sizeof(idx_t);
		for (idx_t i = 0; i < count; i++) {
			ValidityBytes::GetEntryIndex(i, entry_idx, idx_in_entry);
			left_valid = left_validity.RowIsValid(left_validity.GetValidityEntry(entry_idx), idx_in_entry);
			right_valid = right_validity.RowIsValid(right_validity.GetValidityEntry(entry_idx), idx_in_entry);
			if (left_valid && right_valid) {
				switch (type.InternalType()) {
				case PhysicalType::LIST:
					comp_res = CompareListAndAdvance(left_ptr, right_ptr, ListType::GetChildType(type));
					break;
				case PhysicalType::VARCHAR:
					comp_res = CompareStringAndAdvance(left_ptr, right_ptr);
					break;
				case PhysicalType::STRUCT:
					comp_res = CompareStructAndAdvance(left_ptr, right_ptr, StructType::GetChildTypes(type));
					break;
				default:
					throw NotImplementedException("CompareListAndAdvance for variable-size type %s", type.ToString());
				}
			} else if (!left_valid && !right_valid) {
				comp_res = 0;
			} else if (left_valid) {
				comp_res = -1;
			} else {
				comp_res = 1;
			}
			if (comp_res != 0) {
				break;
			}
		}
	}
	// All values that we looped over were equal
	if (comp_res == 0 && left_len != right_len) {
		// Smaller lists first
		if (left_len < right_len) {
			comp_res = -1;
		} else {
			comp_res = 1;
		}
	}
	return comp_res;
}

int PhysicalOrder::CompareValAndAdvance(data_ptr_t &l_ptr, data_ptr_t &r_ptr, const LogicalType &type) {
	switch (type.InternalType()) {
	case PhysicalType::BOOL:
	case PhysicalType::INT8:
		return TemplatedCompareAndAdvance<int8_t>(l_ptr, r_ptr);
	case PhysicalType::INT16:
		return TemplatedCompareAndAdvance<int16_t>(l_ptr, r_ptr);
	case PhysicalType::INT32:
		return TemplatedCompareAndAdvance<int32_t>(l_ptr, r_ptr);
	case PhysicalType::INT64:
		return TemplatedCompareAndAdvance<int64_t>(l_ptr, r_ptr);
	case PhysicalType::UINT8:
		return TemplatedCompareAndAdvance<uint8_t>(l_ptr, r_ptr);
	case PhysicalType::UINT16:
		return TemplatedCompareAndAdvance<uint16_t>(l_ptr, r_ptr);
	case PhysicalType::UINT32:
		return TemplatedCompareAndAdvance<uint32_t>(l_ptr, r_ptr);
	case PhysicalType::UINT64:
		return TemplatedCompareAndAdvance<uint64_t>(l_ptr, r_ptr);
	case PhysicalType::INT128:
		return TemplatedCompareAndAdvance<hugeint_t>(l_ptr, r_ptr);
	case PhysicalType::FLOAT:
		return TemplatedCompareAndAdvance<float>(l_ptr, r_ptr);
	case PhysicalType::DOUBLE:
		return TemplatedCompareAndAdvance<double>(l_ptr, r_ptr);
	case PhysicalType::INTERVAL:
		return TemplatedCompareAndAdvance<interval_t>(l_ptr, r_ptr);
	case PhysicalType::VARCHAR:
		return CompareStringAndAdvance(l_ptr, r_ptr);
	case PhysicalType::LIST:
		return CompareListAndAdvance(l_ptr, r_ptr, ListType::GetChildType(type));
	case PhysicalType::STRUCT:
		return CompareStructAndAdvance(l_ptr, r_ptr, StructType::GetChildTypes(type));
	default:
		throw NotImplementedException("Unimplemented CompareValAndAdvance for type %s", type.ToString());
	}
}

//! Compare two blob values
static inline int CompareVal(const data_ptr_t l_ptr, const data_ptr_t r_ptr, const LogicalType &type) {
	switch (type.InternalType()) {
	case PhysicalType::VARCHAR:
		return TemplatedCompareVal<string_t>(l_ptr, r_ptr);
	case PhysicalType::LIST:
	case PhysicalType::STRUCT: {
		auto l_nested_ptr = Load<data_ptr_t>(l_ptr);
		auto r_nested_ptr = Load<data_ptr_t>(r_ptr);
		return PhysicalOrder::CompareValAndAdvance(l_nested_ptr, r_nested_ptr, type);
	}
	default:
		throw NotImplementedException("Unimplemented CompareVal for type %s", type.ToString());
	}
}

//! Unwizzles an offset into a pointer
static inline void UnswizzleSingleValue(data_ptr_t data_ptr, const data_ptr_t &heap_ptr, const LogicalType &type) {
	if (type.InternalType() == PhysicalType::VARCHAR) {
		data_ptr += sizeof(uint32_t) + string_t::PREFIX_LENGTH;
	}
	Store<data_ptr_t>(heap_ptr + Load<idx_t>(data_ptr), data_ptr);
}

//! Swizzles a pointer into an offset
static inline void SwizzleSingleValue(data_ptr_t data_ptr, const data_ptr_t &heap_ptr, const LogicalType &type) {
	if (type.InternalType() == PhysicalType::VARCHAR) {
		data_ptr += sizeof(uint32_t) + string_t::PREFIX_LENGTH;
	}
	Store<idx_t>(Load<data_ptr_t>(data_ptr) - heap_ptr, data_ptr);
}

//! Compares two blob values that were initially tied by their prefix
static inline int BreakBlobTie(const idx_t &tie_col, const SortedData &left, const SortedData &right,
                               const SortingState &sorting_state, const bool &external) {
	const idx_t &col_idx = sorting_state.sorting_to_blob_col.at(tie_col);
	data_ptr_t l_data_ptr = left.DataPtr();
	data_ptr_t r_data_ptr = right.DataPtr();
	if (!TieIsBreakable(col_idx, l_data_ptr, sorting_state.blob_layout)) {
		// Quick check to see if ties can be broken
		return 0;
	}
	// Align the pointers
	const auto &tie_col_offset = sorting_state.blob_layout.GetOffsets()[col_idx];
	l_data_ptr += tie_col_offset;
	r_data_ptr += tie_col_offset;
	// Do the comparison
	const int order = sorting_state.order_types[tie_col] == OrderType::DESCENDING ? -1 : 1;
	const auto &type = left.layout.GetTypes()[col_idx];
	int result;
	if (external) {
		// Store heap pointers
		data_ptr_t l_heap_ptr = left.HeapPtr();
		data_ptr_t r_heap_ptr = right.HeapPtr();
		// Unswizzle offset to pointer
		UnswizzleSingleValue(l_data_ptr, l_heap_ptr, type);
		UnswizzleSingleValue(r_data_ptr, r_heap_ptr, type);
		// Compare
		result = CompareVal(l_data_ptr, r_data_ptr, type);
		// Swizzle the pointers back to offsets
		SwizzleSingleValue(l_data_ptr, l_heap_ptr, type);
		SwizzleSingleValue(r_data_ptr, r_heap_ptr, type);
	} else {
		result = CompareVal(l_data_ptr, r_data_ptr, type);
	}
	return order * result;
}

//! Calls std::sort on strings that are tied by their prefix after the radix sort
static void SortTiedBlobs(BufferManager &buffer_manager, const data_ptr_t dataptr, const idx_t &start, const idx_t &end,
                          const idx_t &tie_col, bool *ties, const data_ptr_t blob_ptr,
                          const SortingState &sorting_state) {
	const auto row_width = sorting_state.blob_layout.GetRowWidth();
	const idx_t &col_idx = sorting_state.sorting_to_blob_col.at(tie_col);
	// Locate the first blob row in question
	data_ptr_t row_ptr = dataptr + start * sorting_state.entry_size;
	data_ptr_t blob_row_ptr = blob_ptr + Load<idx_t>(row_ptr + sorting_state.comparison_size) * row_width;
	if (!TieIsBreakable(col_idx, blob_row_ptr, sorting_state.blob_layout)) {
		// Quick check to see if ties can be broken
		return;
	}
	// Fill pointer array for sorting
	auto ptr_block = buffer_manager.Allocate(MaxValue((end - start) * sizeof(data_ptr_t), (idx_t)Storage::BLOCK_SIZE));
	auto entry_ptrs = (data_ptr_t *)ptr_block->Ptr();
	for (idx_t i = start; i < end; i++) {
		entry_ptrs[i - start] = row_ptr;
		row_ptr += sorting_state.entry_size;
	}
	// Slow pointer-based sorting
	const int order = sorting_state.order_types[tie_col] == OrderType::DESCENDING ? -1 : 1;
	const auto &tie_col_offset = sorting_state.blob_layout.GetOffsets()[col_idx];
	auto logical_type = sorting_state.blob_layout.GetTypes()[col_idx];
	std::sort(entry_ptrs, entry_ptrs + end - start,
	          [&blob_ptr, &order, &sorting_state, &tie_col_offset, &row_width, &logical_type](const data_ptr_t l,
	                                                                                          const data_ptr_t r) {
		          idx_t left_idx = Load<idx_t>(l + sorting_state.comparison_size);
		          idx_t right_idx = Load<idx_t>(r + sorting_state.comparison_size);
		          data_ptr_t left_ptr = blob_ptr + left_idx * row_width + tie_col_offset;
		          data_ptr_t right_ptr = blob_ptr + right_idx * row_width + tie_col_offset;
		          return order * CompareVal(left_ptr, right_ptr, logical_type) < 0;
	          });
	// Re-order
	auto temp_block =
	    buffer_manager.Allocate(MaxValue((end - start) * sorting_state.entry_size, (idx_t)Storage::BLOCK_SIZE));
	data_ptr_t temp_ptr = temp_block->Ptr();
	for (idx_t i = 0; i < end - start; i++) {
		memcpy(temp_ptr, entry_ptrs[i], sorting_state.entry_size);
		temp_ptr += sorting_state.entry_size;
	}
	memcpy(dataptr + start * sorting_state.entry_size, temp_block->Ptr(), (end - start) * sorting_state.entry_size);
	// Determine if there are still ties (if this is not the last column)
	if (tie_col < sorting_state.column_count - 1) {
		data_ptr_t idx_ptr = dataptr + start * sorting_state.entry_size + sorting_state.comparison_size;
		// Load current entry
		data_ptr_t current_ptr = blob_ptr + Load<idx_t>(idx_ptr) * row_width + tie_col_offset;
		for (idx_t i = 0; i < end - start - 1; i++) {
			// Load next entry and compare
			idx_ptr += sorting_state.entry_size;
			data_ptr_t next_ptr = blob_ptr + Load<idx_t>(idx_ptr) * row_width + tie_col_offset;
			ties[start + i] = CompareVal(current_ptr, next_ptr, logical_type) == 0;
			current_ptr = next_ptr;
		}
	}
}

//! Identifies sequences of rows that are tied by the prefix of a blob column, and sorts them
static void SortTiedBlobs(BufferManager &buffer_manager, SortedBlock &sb, bool *ties, data_ptr_t dataptr,
                          const idx_t &count, const idx_t &tie_col, const SortingState &sorting_state) {
	D_ASSERT(!ties[count - 1]);
	auto &blob_block = sb.blob_sorting_data->data_blocks.back();
	auto blob_handle = buffer_manager.Pin(blob_block.block);
	const data_ptr_t blob_ptr = blob_handle->Ptr();

	for (idx_t i = 0; i < count; i++) {
		if (!ties[i]) {
			continue;
		}
		idx_t j;
		for (j = i; j < count; j++) {
			if (!ties[j]) {
				break;
			}
		}
		SortTiedBlobs(buffer_manager, dataptr, i, j + 1, tie_col, ties, blob_ptr, sorting_state);
		i = j;
	}
}

//! Returns whether there are any 'true' values in the ties[] array
static bool AnyTies(bool ties[], const idx_t &count) {
	D_ASSERT(!ties[count - 1]);
	bool any_ties = false;
	for (idx_t i = 0; i < count - 1; i++) {
		any_ties = any_ties || ties[i];
	}
	return any_ties;
}

//! Compares subsequent rows to check for ties
static void ComputeTies(data_ptr_t dataptr, const idx_t &count, const idx_t &col_offset, const idx_t &tie_size,
                        bool ties[], const SortingState &sorting_state) {
	D_ASSERT(!ties[count - 1]);
	D_ASSERT(col_offset + tie_size <= sorting_state.comparison_size);
	// Align dataptr
	dataptr += col_offset;
	for (idx_t i = 0; i < count - 1; i++) {
		ties[i] = ties[i] && memcmp(dataptr, dataptr + sorting_state.entry_size, tie_size) == 0;
		dataptr += sorting_state.entry_size;
	}
	ties[count - 1] = false;
}

//! Textbook LSD radix sort
static void RadixSort(BufferManager &buffer_manager, data_ptr_t dataptr, const idx_t &count, const idx_t &col_offset,
                      const idx_t &sorting_size, const SortingState &sorting_state) {
	auto temp_block = buffer_manager.Allocate(MaxValue(count * sorting_state.entry_size, (idx_t)Storage::BLOCK_SIZE));
	data_ptr_t temp = temp_block->Ptr();
	bool swap = false;

	idx_t counts[256];
	uint8_t byte;
	for (idx_t offset = col_offset + sorting_size - 1; offset + 1 > col_offset; offset--) {
		// Init counts to 0
		memset(counts, 0, sizeof(counts));
		// Collect counts
		for (idx_t i = 0; i < count; i++) {
			byte = *(dataptr + i * sorting_state.entry_size + offset);
			counts[byte]++;
		}
		// Compute offsets from counts
		for (idx_t val = 1; val < 256; val++) {
			counts[val] = counts[val] + counts[val - 1];
		}
		// Re-order the data in temporary array
		for (idx_t i = count; i > 0; i--) {
			byte = *(dataptr + (i - 1) * sorting_state.entry_size + offset);
			memcpy(temp + (counts[byte] - 1) * sorting_state.entry_size, dataptr + (i - 1) * sorting_state.entry_size,
			       sorting_state.entry_size);
			counts[byte]--;
		}
		std::swap(dataptr, temp);
		swap = !swap;
	}
	// Move data back to original buffer (if it was swapped)
	if (swap) {
		memcpy(temp, dataptr, count * sorting_state.entry_size);
	}
}

//! Identifies sequences of rows that are tied, and calls radix sort on these
static void SubSortTiedTuples(BufferManager &buffer_manager, const data_ptr_t dataptr, const idx_t &count,
                              const idx_t &col_offset, const idx_t &sorting_size, bool ties[],
                              const SortingState &sorting_state) {
	D_ASSERT(!ties[count - 1]);
	for (idx_t i = 0; i < count; i++) {
		if (!ties[i]) {
			continue;
		}
		idx_t j;
		for (j = i + 1; j < count; j++) {
			if (!ties[j]) {
				break;
			}
		}
		RadixSort(buffer_manager, dataptr + i * sorting_state.entry_size, j - i + 1, col_offset, sorting_size,
		          sorting_state);
		i = j;
	}
}

//! Sorts the data in a SortedBlock
static void SortInMemory(BufferManager &buffer_manager, SortedBlock &sb, const SortingState &sorting_state) {
	auto &block = sb.radix_sorting_data.back();
	const auto &count = block.count;
	auto handle = buffer_manager.Pin(block.block);
	const auto dataptr = handle->Ptr();
	// Assign an index to each row
	data_ptr_t idx_dataptr = dataptr + sorting_state.comparison_size;
	for (idx_t i = 0; i < count; i++) {
		Store<idx_t>(i, idx_dataptr);
		idx_dataptr += sorting_state.entry_size;
	}
	// Radix sort and break ties until no more ties, or until all columns are sorted
	idx_t sorting_size = 0;
	idx_t col_offset = 0;
	unique_ptr<BufferHandle> ties_handle;
	bool *ties = nullptr;
	for (idx_t i = 0; i < sorting_state.column_count; i++) {
		sorting_size += sorting_state.column_sizes[i];
		if (sorting_state.constant_size[i] && i < sorting_state.column_count - 1 && sorting_size < 32) {
			// Add columns to the sorting size until we reach a variable size column, or the last column
			continue;
		}

		if (!ties) {
			// This is the first sort
			RadixSort(buffer_manager, dataptr, count, col_offset, sorting_size, sorting_state);
			ties_handle = buffer_manager.Allocate(MaxValue(count, (idx_t)Storage::BLOCK_SIZE));
			ties = (bool *)ties_handle->Ptr();
			std::fill_n(ties, count - 1, true);
			ties[count - 1] = false;
		} else {
			// For subsequent sorts, we only have to subsort the tied tuples
			SubSortTiedTuples(buffer_manager, dataptr, count, col_offset, sorting_size, ties, sorting_state);
		}

		if (sorting_state.constant_size[i] && i == sorting_state.column_count - 1) {
			// All columns are sorted, no ties to break because last column is constant size
			break;
		}

		ComputeTies(dataptr, count, col_offset, sorting_size, ties, sorting_state);
		if (!AnyTies(ties, count)) {
			// No ties, stop sorting
			break;
		}

		if (!sorting_state.constant_size[i]) {
			SortTiedBlobs(buffer_manager, sb, ties, dataptr, count, i, sorting_state);
			if (!AnyTies(ties, count)) {
				// No more ties after tie-breaking, stop
				break;
			}
		}

		col_offset += sorting_size;
		sorting_size = 0;
	}
}

//! Reorders SortedData according to the sorted key columns
static void ReOrder(BufferManager &buffer_manager, SortedData &sd, data_ptr_t sorting_ptr, RowDataCollection &heap,
                    OrderGlobalState &gstate) {
	auto &unordered_data_block = sd.data_blocks.back();
	const idx_t &count = unordered_data_block.count;
	auto unordered_data_handle = buffer_manager.Pin(unordered_data_block.block);
	const data_ptr_t unordered_data_ptr = unordered_data_handle->Ptr();
	// Create new block that will hold re-ordered row data
	RowDataBlock ordered_data_block(buffer_manager, unordered_data_block.capacity, unordered_data_block.entry_size);
	ordered_data_block.count = count;
	auto ordered_data_handle = buffer_manager.Pin(ordered_data_block.block);
	data_ptr_t ordered_data_ptr = ordered_data_handle->Ptr();
	// Re-order fixed-size row layout
	const idx_t row_width = sd.layout.GetRowWidth();
	const idx_t sorting_entry_size = gstate.sorting_state.entry_size;
	for (idx_t i = 0; i < count; i++) {
		idx_t index = Load<idx_t>(sorting_ptr);
		memcpy(ordered_data_ptr, unordered_data_ptr + index * row_width, row_width);
		ordered_data_ptr += row_width;
		sorting_ptr += sorting_entry_size;
	}
	// Replace the unordered data block with the re-ordered data block
	buffer_manager.UnregisterBlock(unordered_data_block.block->BlockId(), true);
	sd.data_blocks.clear();
	sd.data_blocks.push_back(move(ordered_data_block));
	// Deal with the heap (if necessary)
	if (!sd.layout.AllConstant()) {
		// Swizzle the column pointers to offsets
		RowOperations::SwizzleColumns(sd.layout, ordered_data_handle->Ptr(), count);
		// Create a single heap block to store the ordered heap
		idx_t total_byte_offset = std::accumulate(heap.blocks.begin(), heap.blocks.end(), 0,
		                                          [](idx_t a, const RowDataBlock &b) { return a + b.byte_offset; });
		idx_t heap_block_size = MaxValue(total_byte_offset, (idx_t)Storage::BLOCK_SIZE);
		RowDataBlock ordered_heap_block(buffer_manager, heap_block_size, 1);
		ordered_heap_block.count = count;
		ordered_heap_block.byte_offset = total_byte_offset;
		auto ordered_heap_handle = buffer_manager.Pin(ordered_heap_block.block);
		data_ptr_t ordered_heap_ptr = ordered_heap_handle->Ptr();
		// Fill the heap in order
		ordered_data_ptr = ordered_data_handle->Ptr();
		const idx_t heap_pointer_offset = sd.layout.GetHeapPointerOffset();
		for (idx_t i = 0; i < count; i++) {
			auto heap_row_ptr = Load<data_ptr_t>(ordered_data_ptr + heap_pointer_offset);
			auto heap_row_size = Load<idx_t>(heap_row_ptr);
			memcpy(ordered_heap_ptr, heap_row_ptr, heap_row_size);
			ordered_heap_ptr += heap_row_size;
			ordered_data_ptr += row_width;
		}
		// Swizzle the base pointer to the offset of each row in the heap
		RowOperations::SwizzleHeapPointer(sd.layout, ordered_data_handle->Ptr(), ordered_heap_handle->Ptr(), count);
		// Move the re-ordered heap to the SortedData, and clear the local heap
		sd.heap_blocks.push_back(move(ordered_heap_block));
		for (auto &block : heap.blocks) {
			buffer_manager.UnregisterBlock(block.block->BlockId(), true);
		}
	}
	// Reset the localstate heap
	heap.pinned_blocks.clear();
	heap.blocks.clear();
	heap.count = 0;
}

//! Use the ordered sorting data to re-order the rest of the data
static void ReOrder(ClientContext &context, SortedBlock &sb, OrderLocalState &lstate, OrderGlobalState &gstate) {
	auto &buffer_manager = BufferManager::GetBufferManager(context);
	auto sorting_handle = buffer_manager.Pin(sb.radix_sorting_data.back().block);
	const data_ptr_t sorting_ptr = sorting_handle->Ptr() + gstate.sorting_state.comparison_size;
	// Re-order variable size sorting columns
	if (!gstate.sorting_state.all_constant) {
		ReOrder(buffer_manager, *sb.blob_sorting_data, sorting_ptr, *lstate.blob_sorting_heap, gstate);
	}
	// And the payload
	ReOrder(buffer_manager, *sb.payload_data, sorting_ptr, *lstate.payload_heap, gstate);
}

//! Appends and sorts the data accumulated in a local sink state
void PhysicalOrder::SortLocalState(ClientContext &context, OrderLocalState &lstate, OrderGlobalState &gstate) const {
	D_ASSERT(lstate.radix_sorting_data->count == lstate.payload_data->count);
	if (lstate.radix_sorting_data->count == 0) {
		return;
	}
	auto &buffer_manager = BufferManager::GetBufferManager(context);
	const auto &sorting_state = gstate.sorting_state;
	// Move all data to a single SortedBlock
	auto sb = make_unique<SortedBlock>(buffer_manager, gstate);
	// Fixed-size sorting data
	auto sorting_block = ConcatenateBlocks(buffer_manager, *lstate.radix_sorting_data);
	sb->radix_sorting_data.push_back(move(sorting_block));
	// Variable-size sorting data
	if (!sorting_state.blob_layout.AllConstant()) {
		auto &blob_data = *lstate.blob_sorting_data;
		auto new_block = ConcatenateBlocks(buffer_manager, blob_data);
		sb->blob_sorting_data->data_blocks.push_back(move(new_block));
	}
	// Payload data
	auto payload_block = ConcatenateBlocks(buffer_manager, *lstate.payload_data);
	sb->payload_data->data_blocks.push_back(move(payload_block));
	// Now perform the actual sort
	SortInMemory(buffer_manager, *sb, sorting_state);
	// Re-order before the merge sort
	ReOrder(context, *sb, lstate, gstate);
	// Add the sorted block to the local state
	lstate.sorted_blocks.push_back(move(sb));
}

//! Compares the tuples that a being read from in the 'left' and 'right blocks during merge sort
//! (only in case we cannot simply 'memcmp' - if there are blob columns)
inline int CompareTuple(const SortedBlock &left, const SortedBlock &right, const data_ptr_t &l_ptr,
                        const data_ptr_t &r_ptr, const SortingState &sorting_state, const bool &external_sort) {
	// Compare the sorting columns one by one
	int comp_res = 0;
	data_ptr_t l_ptr_offset = l_ptr;
	data_ptr_t r_ptr_offset = r_ptr;
	for (idx_t col_idx = 0; col_idx < sorting_state.column_count; col_idx++) {
		comp_res = memcmp(l_ptr_offset, r_ptr_offset, sorting_state.column_sizes[col_idx]);
		if (comp_res == 0 && !sorting_state.constant_size[col_idx]) {
			comp_res =
			    BreakBlobTie(col_idx, *left.blob_sorting_data, *right.blob_sorting_data, sorting_state, external_sort);
		}
		if (comp_res != 0) {
			break;
		}
		l_ptr_offset += sorting_state.column_sizes[col_idx];
		r_ptr_offset += sorting_state.column_sizes[col_idx];
	}
	return comp_res;
}

class PhysicalOrderMergeTask : public Task {
public:
	PhysicalOrderMergeTask(Pipeline &parent_p, ClientContext &context_p, OrderGlobalState &state_p)
	    : parent(parent_p), context(context_p), buffer_manager(BufferManager::GetBufferManager(context_p)),
	      state(state_p), sorting_state(state_p.sorting_state) {
	}

	void Execute() override {
		ComputeWork();
		auto &left = *left_block;
		auto &right = *right_block;
		D_ASSERT(left.radix_sorting_data.size() == left.payload_data->data_blocks.size());
		D_ASSERT(right.radix_sorting_data.size() == right.payload_data->data_blocks.size());
		if (!state.payload_layout.AllConstant() && state.external) {
			D_ASSERT(left.payload_data->data_blocks.size() == left.payload_data->heap_blocks.size());
			D_ASSERT(right.payload_data->data_blocks.size() == right.payload_data->heap_blocks.size());
		}
		if (!sorting_state.all_constant) {
			D_ASSERT(left.radix_sorting_data.size() == left.blob_sorting_data->data_blocks.size());
			D_ASSERT(right.radix_sorting_data.size() == right.blob_sorting_data->data_blocks.size());
			if (state.external) {
				D_ASSERT(left.blob_sorting_data->data_blocks.size() == left.blob_sorting_data->heap_blocks.size());
				D_ASSERT(right.blob_sorting_data->data_blocks.size() == right.blob_sorting_data->heap_blocks.size());
			}
		}
		// Set up the write block
		result->InitializeWrite();
		// Initialize arrays to store merge data
		bool left_smaller[STANDARD_VECTOR_SIZE];
		idx_t next_entry_sizes[STANDARD_VECTOR_SIZE];
		// Merge loop
		auto l_count = left.Remaining();
		auto r_count = right.Remaining();
		while (true) {
			auto l_remaining = left.Remaining();
			auto r_remaining = right.Remaining();
			if (l_remaining + r_remaining == 0) {
				// Done
				break;
			}
			const idx_t next = MinValue(l_remaining + r_remaining, (idx_t)STANDARD_VECTOR_SIZE);
			if (l_remaining != 0 && r_remaining != 0) {
				// Compute the merge (not needed if one side is exhausted)
				ComputeMerge(next, left_smaller);
			}
			// Actually merge the data (radix, blob, and payload)
			Merge(next, left_smaller);
			if (!sorting_state.all_constant) {
				Merge(*result->blob_sorting_data, *left.blob_sorting_data, *right.blob_sorting_data, next, left_smaller,
				      next_entry_sizes);
				D_ASSERT(left.block_idx == left.blob_sorting_data->block_idx &&
				         left.entry_idx == left.blob_sorting_data->entry_idx);
				D_ASSERT(right.block_idx == right.blob_sorting_data->block_idx &&
				         right.entry_idx == right.blob_sorting_data->entry_idx);
				D_ASSERT(result->radix_sorting_data.size() == result->blob_sorting_data->data_blocks.size());
			}
			Merge(*result->payload_data, *left.payload_data, *right.payload_data, next, left_smaller, next_entry_sizes);
			D_ASSERT(left.block_idx == left.payload_data->block_idx && left.entry_idx == left.payload_data->entry_idx);
			D_ASSERT(right.block_idx == right.payload_data->block_idx &&
			         right.entry_idx == right.payload_data->entry_idx);
			D_ASSERT(result->radix_sorting_data.size() == result->payload_data->data_blocks.size());
		}
		D_ASSERT(result->Count() == l_count + r_count);

		lock_guard<mutex> glock(state.lock);
		parent.finished_tasks++;
		if (parent.finished_tasks == parent.total_tasks) {
			// Unregister processed data
			for (auto &sb : state.sorted_blocks) {
				sb->UnregisterSortingBlocks();
				sb->UnregisterPayloadBlocks();
			}
			state.sorted_blocks.clear();
			if (state.odd_one_out) {
				state.sorted_blocks.push_back(move(state.odd_one_out));
				state.odd_one_out = nullptr;
			}
			for (auto &sorted_block_vector : state.sorted_blocks_temp) {
				state.sorted_blocks.push_back(make_unique<SortedBlock>(buffer_manager, state));
				state.sorted_blocks.back()->AppendSortedBlocks(sorted_block_vector);
			}
			state.sorted_blocks_temp.clear();
			PhysicalOrder::ScheduleMergeTasks(parent, context, state);
		}
	}

	//! Sets the left and right block that this task will merge
	void ComputeWork() {
		// Acquire global lock to compute next intersection
		lock_guard<mutex> glock(state.lock);
		// Create result block
		state.sorted_blocks_temp[state.pair_idx].push_back(make_unique<SortedBlock>(buffer_manager, state));
		result = state.sorted_blocks_temp[state.pair_idx].back().get();
		// Determine which blocks must be merged
		auto &left = *state.sorted_blocks[state.pair_idx * 2];
		auto &right = *state.sorted_blocks[state.pair_idx * 2 + 1];
		const idx_t l_count = left.Count();
		const idx_t r_count = right.Count();
		// Compute the work that this thread must do using Merge Path
		idx_t l_end;
		idx_t r_end;
		if (state.l_start + state.r_start + state.block_capacity < l_count + r_count) {
			const idx_t intersection = state.l_start + state.r_start + state.block_capacity;
			ComputeIntersection(left, right, intersection, l_end, r_end);
			D_ASSERT(l_end <= l_count);
			D_ASSERT(r_end <= r_count);
			D_ASSERT(intersection == l_end + r_end);
			// Unpin after finding the intersection
			if (!sorting_state.blob_layout.AllConstant()) {
				left.blob_sorting_data->UnpinAndReset(0, 0);
				right.blob_sorting_data->UnpinAndReset(0, 0);
			}
		} else {
			l_end = l_count;
			r_end = r_count;
		}
		// Create slices of the data that this thread must merge
		left_block = left.CreateSlice(state.l_start, l_end);
		right_block = right.CreateSlice(state.r_start, r_end);
		// Update global state
		state.l_start = l_end;
		state.r_start = r_end;
		if (state.l_start == l_count && state.r_start == r_count) {
			state.pair_idx++;
			state.l_start = 0;
			state.r_start = 0;
		}
	}

	//! Compare values within SortedBlocks using a global index
	int CompareUsingGlobalIndex(SortedBlock &l, SortedBlock &r, const idx_t l_idx, const idx_t r_idx) {
		D_ASSERT(l_idx < l.Count());
		D_ASSERT(r_idx < r.Count());

		idx_t l_block_idx;
		idx_t l_entry_idx;
		l.GlobalToLocalIndex(l_idx, l_block_idx, l_entry_idx);

		idx_t r_block_idx;
		idx_t r_entry_idx;
		r.GlobalToLocalIndex(r_idx, r_block_idx, r_entry_idx);

		auto l_block_handle = buffer_manager.Pin(l.radix_sorting_data[l_block_idx].block);
		auto r_block_handle = buffer_manager.Pin(r.radix_sorting_data[r_block_idx].block);
		data_ptr_t l_ptr = l_block_handle->Ptr() + l_entry_idx * sorting_state.entry_size;
		data_ptr_t r_ptr = r_block_handle->Ptr() + r_entry_idx * sorting_state.entry_size;

		int comp_res;
		if (sorting_state.all_constant) {
			comp_res = memcmp(l_ptr, r_ptr, sorting_state.comparison_size);
		} else {
			l.blob_sorting_data->block_idx = l_block_idx;
			l.blob_sorting_data->entry_idx = l_entry_idx;
			l.blob_sorting_data->Pin();
			r.blob_sorting_data->block_idx = r_block_idx;
			r.blob_sorting_data->entry_idx = r_entry_idx;
			r.blob_sorting_data->Pin();
			comp_res = CompareTuple(l, r, l_ptr, r_ptr, sorting_state, state.external);
		}
		return comp_res;
	}

	//! Merge path
	void ComputeIntersection(SortedBlock &l, SortedBlock &r, const idx_t sum, idx_t &l_idx, idx_t &r_idx) {
		const idx_t l_count = l.Count();
		const idx_t r_count = r.Count();
		// Cover some edge cases
		if (sum >= l_count + r_count) {
			l_idx = l_count;
			r_idx = r_count;
			return;
		} else if (sum == 0) {
			l_idx = 0;
			r_idx = 0;
			return;
		} else if (l_count == 0) {
			l_idx = 0;
			r_idx = sum;
			return;
		} else if (r_count == 0) {
			r_idx = 0;
			l_idx = sum;
			return;
		}
		// Determine offsets for the binary search
		const idx_t l_offset = MinValue(l_count, sum);
		const idx_t r_offset = sum > l_count ? sum - l_count : 0;
		D_ASSERT(l_offset + r_offset == sum);
		const idx_t search_space =
		    sum > MaxValue(l_count, r_count) ? l_count + r_count - sum : MinValue(sum, MinValue(l_count, r_count));
		// Double binary search
		idx_t left = 0;
		idx_t right = search_space - 1;
		idx_t middle;
		int comp_res;
		while (left <= right) {
			middle = (left + right) / 2;
			l_idx = l_offset - middle;
			r_idx = r_offset + middle;
			if (l_idx == l_count || r_idx == 0) {
				comp_res = CompareUsingGlobalIndex(l, r, l_idx - 1, r_idx);
				if (comp_res > 0) {
					l_idx--;
					r_idx++;
				} else {
					return;
				}
				if (l_idx == 0 || r_idx == r_count) {
					return;
				} else {
					break;
				}
			}
			comp_res = CompareUsingGlobalIndex(l, r, l_idx, r_idx);
			if (comp_res > 0) {
				left = middle + 1;
			} else {
				right = middle - 1;
			}
		}
		// Shift by one (if needed)
		if (l_idx == 0) {
			comp_res = CompareUsingGlobalIndex(l, r, l_idx, r_idx);
			if (comp_res > 0) {
				l_idx--;
				r_idx++;
			}
			return;
		}
		int l_r_min1 = CompareUsingGlobalIndex(l, r, l_idx, r_idx - 1);
		int l_min1_r = CompareUsingGlobalIndex(l, r, l_idx - 1, r_idx);
		if (l_r_min1 > 0 && l_min1_r < 0) {
			return;
		} else if (l_r_min1 > 0) {
			l_idx--;
			r_idx++;
		} else if (l_min1_r < 0) {
			l_idx++;
			r_idx--;
		}
	}

	//! Computes how the next 'count' tuples should be merged by setting the 'left_smaller' array
	void ComputeMerge(const idx_t &count, bool *left_smaller) {
		auto &left = *left_block;
		auto &right = *right_block;
		// Store indices to restore after computing the merge
		idx_t l_block_idx = left.block_idx;
		idx_t r_block_idx = right.block_idx;
		idx_t l_entry_idx = left.entry_idx;
		idx_t r_entry_idx = right.entry_idx;
		// Data handles and pointers for both sides
		unique_ptr<BufferHandle> l_radix_handle;
		unique_ptr<BufferHandle> r_radix_handle;
		data_ptr_t l_radix_ptr;
		data_ptr_t r_radix_ptr;
		// Compute the merge of the next 'count' tuples
		idx_t compared = 0;
		while (compared < count) {
			// Move to the next block (if needed)
			if (l_block_idx < left.radix_sorting_data.size() &&
			    l_entry_idx == left.radix_sorting_data[l_block_idx].count) {
				l_block_idx++;
				l_entry_idx = 0;
				if (!sorting_state.all_constant) {
					left.blob_sorting_data->block_idx = l_block_idx;
					left.blob_sorting_data->entry_idx = l_entry_idx;
				}
			}
			if (r_block_idx < right.radix_sorting_data.size() &&
			    r_entry_idx == right.radix_sorting_data[r_block_idx].count) {
				r_block_idx++;
				r_entry_idx = 0;
				if (!sorting_state.all_constant) {
					right.blob_sorting_data->block_idx = r_block_idx;
					right.blob_sorting_data->entry_idx = r_entry_idx;
				}
			}
			const bool l_done = l_block_idx == left.radix_sorting_data.size();
			const bool r_done = r_block_idx == right.radix_sorting_data.size();
			if (l_done || r_done) {
				// One of the sides is exhausted, no need to compare
				break;
			}
			// Pin the radix sorting data
			if (!l_done) {
				l_radix_handle = buffer_manager.Pin(left.radix_sorting_data[l_block_idx].block);
				l_radix_ptr = l_radix_handle->Ptr() + l_entry_idx * sorting_state.entry_size;
			}
			if (!r_done) {
				r_radix_handle = buffer_manager.Pin(right.radix_sorting_data[r_block_idx].block);
				r_radix_ptr = r_radix_handle->Ptr() + r_entry_idx * sorting_state.entry_size;
			}
			const idx_t &l_count = !l_done ? left.radix_sorting_data[l_block_idx].count : 0;
			const idx_t &r_count = !r_done ? right.radix_sorting_data[r_block_idx].count : 0;
			// Compute the merge
			if (sorting_state.all_constant) {
				// All sorting columns are constant size
				for (; compared < count && l_entry_idx < l_count && r_entry_idx < r_count; compared++) {
					left_smaller[compared] = memcmp(l_radix_ptr, r_radix_ptr, sorting_state.comparison_size) < 0;
					const bool &l_smaller = left_smaller[compared];
					const bool r_smaller = !l_smaller;
					// Use comparison bool (0 or 1) to increment entries and pointers
					l_entry_idx += l_smaller;
					r_entry_idx += r_smaller;
					l_radix_ptr += l_smaller * sorting_state.entry_size;
					r_radix_ptr += r_smaller * sorting_state.entry_size;
				}
			} else {
				// Pin the blob data
				if (!l_done) {
					left.blob_sorting_data->Pin();
				}
				if (!r_done) {
					right.blob_sorting_data->Pin();
				}
				// Merge with variable size sorting columns
				for (; compared < count && l_entry_idx < l_count && r_entry_idx < r_count; compared++) {
					D_ASSERT(l_block_idx == left.blob_sorting_data->block_idx &&
					         l_entry_idx == left.blob_sorting_data->entry_idx);
					D_ASSERT(r_block_idx == right.blob_sorting_data->block_idx &&
					         r_entry_idx == right.blob_sorting_data->entry_idx);
					left_smaller[compared] =
					    CompareTuple(left, right, l_radix_ptr, r_radix_ptr, sorting_state, state.external) < 0;
					const bool &l_smaller = left_smaller[compared];
					const bool r_smaller = !l_smaller;
					// Use comparison bool (0 or 1) to increment entries and pointers
					l_entry_idx += l_smaller;
					r_entry_idx += r_smaller;
					l_radix_ptr += l_smaller * sorting_state.entry_size;
					r_radix_ptr += r_smaller * sorting_state.entry_size;
					left.blob_sorting_data->Advance(l_smaller);
					right.blob_sorting_data->Advance(r_smaller);
				}
			}
		}
		// Reset block indices before the actual merge
		if (!sorting_state.all_constant) {
			left.blob_sorting_data->UnpinAndReset(left.block_idx, left.entry_idx);
			right.blob_sorting_data->UnpinAndReset(right.block_idx, right.entry_idx);
		}
	}

	//! Merges the radix sorting blocks according to the 'left_smaller' array
	void Merge(const idx_t &count, const bool left_smaller[]) {
		auto &left = *left_block;
		auto &right = *right_block;
		RowDataBlock *l_block;
		RowDataBlock *r_block;

		unique_ptr<BufferHandle> l_block_handle;
		unique_ptr<BufferHandle> r_block_handle;
		data_ptr_t l_ptr;
		data_ptr_t r_ptr;

		RowDataBlock *result_block = &result->radix_sorting_data.back();
		auto result_handle = buffer_manager.Pin(result_block->block);
		data_ptr_t result_ptr = result_handle->Ptr() + result_block->count * sorting_state.entry_size;

		idx_t copied = 0;
		while (copied < count) {
			// Move to the next block (if needed)
			if (left.block_idx < left.radix_sorting_data.size() &&
			    left.entry_idx == left.radix_sorting_data[left.block_idx].count) {
				left.block_idx++;
				left.entry_idx = 0;
			}
			if (right.block_idx < right.radix_sorting_data.size() &&
			    right.entry_idx == right.radix_sorting_data[right.block_idx].count) {
				right.block_idx++;
				right.entry_idx = 0;
			}
			const bool l_done = left.block_idx == left.radix_sorting_data.size();
			const bool r_done = right.block_idx == right.radix_sorting_data.size();
			// Pin the radix sortable blocks
			if (!l_done) {
				l_block = &left.radix_sorting_data[left.block_idx];
				l_block_handle = buffer_manager.Pin(l_block->block);
				l_ptr = l_block_handle->Ptr() + left.entry_idx * sorting_state.entry_size;
			}
			if (!r_done) {
				r_block = &right.radix_sorting_data[right.block_idx];
				r_block_handle = buffer_manager.Pin(r_block->block);
				r_ptr = r_block_handle->Ptr() + right.entry_idx * sorting_state.entry_size;
			}
			const idx_t &l_count = !l_done ? l_block->count : 0;
			const idx_t &r_count = !r_done ? r_block->count : 0;
			// Create new result block (if needed)
			if (result_block->count == result_block->capacity) {
				result->CreateBlock();
				result_block = &result->radix_sorting_data.back();
				result_handle = buffer_manager.Pin(result_block->block);
				result_ptr = result_handle->Ptr();
			}
			// Copy using computed merge
			if (!l_done && !r_done) {
				// Both sides have data - merge
				MergeRows(l_ptr, left.entry_idx, l_count, r_ptr, right.entry_idx, r_count, result_block, result_ptr,
				          sorting_state.entry_size, left_smaller, copied, count);
			} else if (r_done) {
				// Right side is exhausted
				FlushRows(l_ptr, left.entry_idx, l_count, result_block, result_ptr, sorting_state.entry_size, copied,
				          count);
			} else {
				// Left side is exhausted
				FlushRows(r_ptr, right.entry_idx, r_count, result_block, result_ptr, sorting_state.entry_size, copied,
				          count);
			}
		}
	}

	//! Merges SortedData according to the 'left_smaller' array
	void Merge(SortedData &result_data, SortedData &l_data, SortedData &r_data, const idx_t &count,
	           const bool left_smaller[], idx_t next_entry_sizes[]) {
		const auto &layout = result_data.layout;
		const idx_t row_width = layout.GetRowWidth();
		const idx_t heap_pointer_offset = layout.GetHeapPointerOffset();

		// Left and right row data to merge
		unique_ptr<BufferHandle> l_data_block_handle;
		unique_ptr<BufferHandle> r_data_block_handle;
		data_ptr_t l_ptr;
		data_ptr_t r_ptr;
		// Accompanying left and right heap data (if needed)
		unique_ptr<BufferHandle> l_heap_handle;
		unique_ptr<BufferHandle> r_heap_handle;
		data_ptr_t l_heap_ptr;
		data_ptr_t r_heap_ptr;

		// Result rows to write to
		RowDataBlock *result_data_block = &result_data.data_blocks.back();
		auto result_data_handle = buffer_manager.Pin(result_data_block->block);
		data_ptr_t result_data_ptr = result_data_handle->Ptr() + result_data_block->count * row_width;
		// Result heap to write to (if needed)
		RowDataBlock *result_heap_block;
		unique_ptr<BufferHandle> result_heap_handle;
		data_ptr_t result_heap_ptr;
		if (!layout.AllConstant() && state.external) {
			result_heap_block = &result_data.heap_blocks.back();
			result_heap_handle = buffer_manager.Pin(result_heap_block->block);
			result_heap_ptr = result_heap_handle->Ptr() + result_heap_block->byte_offset;
		}

		idx_t copied = 0;
		while (copied < count) {
			// Move to new data blocks (if needed)
			if (l_data.block_idx < l_data.data_blocks.size() &&
			    l_data.entry_idx == l_data.data_blocks[l_data.block_idx].count) {
				l_data.block_idx++;
				l_data.entry_idx = 0;
			}
			if (r_data.block_idx < r_data.data_blocks.size() &&
			    r_data.entry_idx == r_data.data_blocks[r_data.block_idx].count) {
				r_data.block_idx++;
				r_data.entry_idx = 0;
			}
			const bool l_done = l_data.block_idx == l_data.data_blocks.size();
			const bool r_done = r_data.block_idx == r_data.data_blocks.size();
			// Pin the row data blocks
			if (!l_done) {
				l_data_block_handle = buffer_manager.Pin(l_data.data_blocks[l_data.block_idx].block);
				l_ptr = l_data_block_handle->Ptr() + l_data.entry_idx * row_width;
			}
			if (!r_done) {
				r_data_block_handle = buffer_manager.Pin(r_data.data_blocks[r_data.block_idx].block);
				r_ptr = r_data_block_handle->Ptr() + r_data.entry_idx * row_width;
			}
			const idx_t &l_count = !l_done ? l_data.data_blocks[l_data.block_idx].count : 0;
			const idx_t &r_count = !r_done ? r_data.data_blocks[r_data.block_idx].count : 0;
			// Create new result data block (if needed)
			if (result_data_block->count == result_data_block->capacity) {
				// Shrink down the last heap block to fit the data
				if (!layout.AllConstant() && state.external &&
				    result_heap_block->byte_offset < result_heap_block->capacity &&
				    result_heap_block->byte_offset >= Storage::BLOCK_SIZE) {
					buffer_manager.ReAllocate(result_heap_block->block, result_heap_block->byte_offset);
					result_heap_block->capacity = result_heap_block->byte_offset;
				}
				result_data.CreateBlock();
				result_data_block = &result_data.data_blocks.back();
				result_data_handle = buffer_manager.Pin(result_data_block->block);
				result_data_ptr = result_data_handle->Ptr();
				if (!layout.AllConstant() && state.external) {
					result_heap_block = &result_data.heap_blocks.back();
					result_heap_handle = buffer_manager.Pin(result_heap_block->block);
					result_heap_ptr = result_heap_handle->Ptr();
				}
			}
			// Perform the merge
			if (layout.AllConstant() || !state.external) {
				// If all constant size, or if we are doing an in-memory sort, we do not need to touch the heap
				if (!l_done && !r_done) {
					// Both sides have data - merge
					MergeRows(l_ptr, l_data.entry_idx, l_count, r_ptr, r_data.entry_idx, r_count, result_data_block,
					          result_data_ptr, row_width, left_smaller, copied, count);
				} else if (r_done) {
					// Right side is exhausted
					FlushRows(l_ptr, l_data.entry_idx, l_count, result_data_block, result_data_ptr, row_width, copied,
					          count);
				} else {
					// Left side is exhausted
					FlushRows(r_ptr, r_data.entry_idx, r_count, result_data_block, result_data_ptr, row_width, copied,
					          count);
				}
			} else {
				// External sorting with variable size data. Pin the heap blocks too
				if (!l_done) {
					l_heap_handle = buffer_manager.Pin(l_data.heap_blocks[l_data.block_idx].block);
					l_heap_ptr = l_heap_handle->Ptr() + Load<idx_t>(l_ptr + heap_pointer_offset);
					D_ASSERT(l_heap_ptr - l_heap_handle->Ptr() >= 0);
					D_ASSERT((idx_t)(l_heap_ptr - l_heap_handle->Ptr()) <
					         l_data.heap_blocks[l_data.block_idx].byte_offset);
				}
				if (!r_done) {
					r_heap_handle = buffer_manager.Pin(r_data.heap_blocks[r_data.block_idx].block);
					r_heap_ptr = r_heap_handle->Ptr() + Load<idx_t>(r_ptr + heap_pointer_offset);
					D_ASSERT(r_heap_ptr - r_heap_handle->Ptr() >= 0);
					D_ASSERT((idx_t)(r_heap_ptr - r_heap_handle->Ptr()) <
					         r_data.heap_blocks[r_data.block_idx].byte_offset);
				}
				// Both the row and heap data need to be dealt with
				if (!l_done && !r_done) {
					// Both sides have data - merge
					idx_t l_idx_copy = l_data.entry_idx;
					idx_t r_idx_copy = r_data.entry_idx;
					data_ptr_t result_data_ptr_copy = result_data_ptr;
					idx_t copied_copy = copied;
					// Merge row data
					MergeRows(l_ptr, l_idx_copy, l_count, r_ptr, r_idx_copy, r_count, result_data_block,
					          result_data_ptr_copy, row_width, left_smaller, copied_copy, count);
					const idx_t merged = copied_copy - copied;
					// Compute the entry sizes and number of heap bytes that will be copied
					idx_t copy_bytes = 0;
					data_ptr_t l_heap_ptr_copy = l_heap_ptr;
					data_ptr_t r_heap_ptr_copy = r_heap_ptr;
					for (idx_t i = 0; i < merged; i++) {
						// Store base heap offset in the row data
						Store<idx_t>(result_heap_block->byte_offset + copy_bytes,
						             result_data_ptr + heap_pointer_offset);
						result_data_ptr += row_width;
						// Compute entry size and add to total
						const bool &l_smaller = left_smaller[copied + i];
						const bool r_smaller = !l_smaller;
						auto &entry_size = next_entry_sizes[copied + i];
						entry_size =
						    l_smaller * Load<idx_t>(l_heap_ptr_copy) + r_smaller * Load<idx_t>(r_heap_ptr_copy);
						D_ASSERT(entry_size >= sizeof(idx_t));
						D_ASSERT(l_heap_ptr_copy - l_heap_handle->Ptr() + l_smaller * entry_size <=
						         l_data.heap_blocks[l_data.block_idx].byte_offset);
						D_ASSERT(r_heap_ptr_copy - r_heap_handle->Ptr() + r_smaller * entry_size <=
						         r_data.heap_blocks[r_data.block_idx].byte_offset);
						l_heap_ptr_copy += l_smaller * entry_size;
						r_heap_ptr_copy += r_smaller * entry_size;
						copy_bytes += entry_size;
					}
					// Reallocate result heap block size (if needed)
					if (result_heap_block->byte_offset + copy_bytes > result_heap_block->capacity) {
						idx_t new_capacity = result_heap_block->byte_offset + copy_bytes;
						buffer_manager.ReAllocate(result_heap_block->block, new_capacity);
						result_heap_block->capacity = new_capacity;
						result_heap_ptr = result_heap_handle->Ptr() + result_heap_block->byte_offset;
					}
					D_ASSERT(result_heap_block->byte_offset + copy_bytes <= result_heap_block->capacity);
					// Now copy the heap data
					for (idx_t i = 0; i < merged; i++) {
						const bool &l_smaller = left_smaller[copied + i];
						const bool r_smaller = !l_smaller;
						const auto &entry_size = next_entry_sizes[copied + i];
						memcpy(result_heap_ptr, l_heap_ptr, l_smaller * entry_size);
						memcpy(result_heap_ptr, r_heap_ptr, r_smaller * entry_size);
						D_ASSERT(Load<idx_t>(result_heap_ptr) == entry_size);
						result_heap_ptr += entry_size;
						l_heap_ptr += l_smaller * entry_size;
						r_heap_ptr += r_smaller * entry_size;
						l_data.entry_idx += l_smaller;
						r_data.entry_idx += r_smaller;
					}
					// Update result indices and pointers
					result_heap_block->count += merged;
					result_heap_block->byte_offset += copy_bytes;
					copied += merged;
				} else if (r_done) {
					// Right side is exhausted - flush left
					FlushBlobs(buffer_manager, layout, l_count, l_ptr, l_data.entry_idx, l_heap_ptr, result_data_block,
					           result_data_ptr, result_heap_block, *result_heap_handle, result_heap_ptr, copied, count);
				} else {
					// Left side is exhausted - flush right
					FlushBlobs(buffer_manager, layout, r_count, r_ptr, r_data.entry_idx, r_heap_ptr, result_data_block,
					           result_data_ptr, result_heap_block, *result_heap_handle, result_heap_ptr, copied, count);
				}
				D_ASSERT(result_data_block->count == result_heap_block->count);
			}
		}
	}

	//! Merges constant size rows
	static void MergeRows(data_ptr_t &l_ptr, idx_t &l_entry_idx, const idx_t &l_count, data_ptr_t &r_ptr,
	                      idx_t &r_entry_idx, const idx_t &r_count, RowDataBlock *target_block, data_ptr_t &target_ptr,
	                      const idx_t &entry_size, const bool *left_smaller, idx_t &copied, const idx_t &count) {
		const idx_t next = MinValue(count - copied, target_block->capacity - target_block->count);
		idx_t i;
		for (i = 0; i < next && l_entry_idx < l_count && r_entry_idx < r_count; i++) {
			const bool &l_smaller = left_smaller[copied + i];
			const bool r_smaller = !l_smaller;
			// Use comparison bool (0 or 1) to copy an entry from either side
			memcpy(target_ptr, l_ptr, l_smaller * entry_size);
			memcpy(target_ptr, r_ptr, r_smaller * entry_size);
			target_ptr += entry_size;
			// Use the comparison bool to increment entries and pointers
			l_entry_idx += l_smaller;
			r_entry_idx += r_smaller;
			l_ptr += l_smaller * entry_size;
			r_ptr += r_smaller * entry_size;
		}
		// Update counts
		target_block->count += i;
		copied += i;
	}

	//! Flushes constant size rows
	static void FlushRows(data_ptr_t &source_ptr, idx_t &source_entry_idx, const idx_t &source_count,
	                      RowDataBlock *target_block, data_ptr_t &target_ptr, const idx_t &entry_size, idx_t &copied,
	                      const idx_t &count) {
		// Compute how many entries we can fit
		idx_t next = MinValue(count - copied, target_block->capacity - target_block->count);
		next = MinValue(next, source_count - source_entry_idx);
		// Copy them all in a single memcpy
		const idx_t copy_bytes = next * entry_size;
		memcpy(target_ptr, source_ptr, copy_bytes);
		target_ptr += copy_bytes;
		source_ptr += copy_bytes;
		// Update counts
		source_entry_idx += next;
		target_block->count += next;
		copied += next;
	}

	//! Flushes blob rows and accompanying heap
	static void FlushBlobs(BufferManager &buffer_manager, const RowLayout &layout, const idx_t &source_count,
	                       data_ptr_t &source_data_ptr, idx_t &source_entry_idx, data_ptr_t &source_heap_ptr,
	                       RowDataBlock *target_data_block, data_ptr_t &target_data_ptr,
	                       RowDataBlock *target_heap_block, BufferHandle &target_heap_handle,
	                       data_ptr_t &target_heap_ptr, idx_t &copied, const idx_t &count) {
		const idx_t row_width = layout.GetRowWidth();
		const idx_t heap_pointer_offset = layout.GetHeapPointerOffset();
		idx_t source_entry_idx_copy = source_entry_idx;
		data_ptr_t target_data_ptr_copy = target_data_ptr;
		idx_t copied_copy = copied;
		// Flush row data
		FlushRows(source_data_ptr, source_entry_idx_copy, source_count, target_data_block, target_data_ptr_copy,
		          row_width, copied_copy, count);
		const idx_t flushed = copied_copy - copied;
		// Compute the entry sizes and number of heap bytes that will be copied
		idx_t copy_bytes = 0;
		data_ptr_t source_heap_ptr_copy = source_heap_ptr;
		for (idx_t i = 0; i < flushed; i++) {
			// Store base heap offset in the row data
			Store<idx_t>(target_heap_block->byte_offset + copy_bytes, target_data_ptr + heap_pointer_offset);
			target_data_ptr += row_width;
			// Compute entry size and add to total
			auto entry_size = Load<idx_t>(source_heap_ptr_copy);
			D_ASSERT(entry_size >= sizeof(idx_t));
			source_heap_ptr_copy += entry_size;
			copy_bytes += entry_size;
		}
		// Reallocate result heap block size (if needed)
		if (target_heap_block->byte_offset + copy_bytes > target_heap_block->capacity) {
			idx_t new_capacity = target_heap_block->byte_offset + copy_bytes;
			buffer_manager.ReAllocate(target_heap_block->block, new_capacity);
			target_heap_block->capacity = new_capacity;
			target_heap_ptr = target_heap_handle.Ptr() + target_heap_block->byte_offset;
		}
		D_ASSERT(target_heap_block->byte_offset + copy_bytes <= target_heap_block->capacity);
		// Copy the heap data in one go
		memcpy(target_heap_ptr, source_heap_ptr, copy_bytes);
		target_heap_ptr += copy_bytes;
		source_heap_ptr += copy_bytes;
		source_entry_idx += flushed;
		copied += flushed;
		// Update result indices and pointers
		target_heap_block->count += flushed;
		target_heap_block->byte_offset += copy_bytes;
		D_ASSERT(target_heap_block->byte_offset <= target_heap_block->capacity);
	}

private:
	Pipeline &parent;
	ClientContext &context;
	BufferManager &buffer_manager;
	OrderGlobalState &state;
	const SortingState &sorting_state;

	unique_ptr<SortedBlock> left_block;
	unique_ptr<SortedBlock> right_block;
	SortedBlock *result;
};

bool PhysicalOrder::Finalize(Pipeline &pipeline, ClientContext &context, unique_ptr<GlobalOperatorState> state_p) {
	this->sink_state = move(state_p);
	auto &state = (OrderGlobalState &)*this->sink_state;
	if (state.sorted_blocks.empty()) {
		return true;
	}
	// Set total count
	for (auto &sb : state.sorted_blocks) {
		state.total_count += sb->radix_sorting_data.back().count;
	}
	// Determine if we need to use do an external sort
	idx_t total_heap_size =
	    std::accumulate(state.sorted_blocks.begin(), state.sorted_blocks.end(), (idx_t)0,
	                    [](idx_t a, const unique_ptr<SortedBlock> &b) { return a + b->HeapSize(); });
	if (total_heap_size > 0.25 * BufferManager::GetBufferManager(context).GetMaxMemory()) {
		state.external = true;
	}
	// Use the data that we have to determine which block size to use during the merge
	const auto &sorting_state = state.sorting_state;
	for (auto &sb : state.sorted_blocks) {
		auto &block = sb->radix_sorting_data.back();
		state.block_capacity = MaxValue(state.block_capacity, block.capacity);
	}
	// Sorting heap data
	if (!sorting_state.all_constant && state.external) {
		for (auto &sb : state.sorted_blocks) {
			auto &heap_block = sb->blob_sorting_data->heap_blocks.back();
			state.sorting_heap_capacity = MaxValue(state.sorting_heap_capacity, heap_block.capacity);
		}
	}
	// Payload heap data
	const auto &payload_layout = state.payload_layout;
	if (!payload_layout.AllConstant() && state.external) {
		for (auto &sb : state.sorted_blocks) {
			auto &heap_block = sb->payload_data->heap_blocks.back();
			state.payload_heap_capacity = MaxValue(state.sorting_heap_capacity, heap_block.capacity);
		}
	}
	// Unswizzle and pin heap blocks if we can fit everything in memory
	if (!state.external) {
		for (auto &sb : state.sorted_blocks) {
			sb->blob_sorting_data->Unswizzle();
			sb->payload_data->Unswizzle();
		}
	}
	// Start the merge or finish if a merge is not necessary
	if (state.sorted_blocks.size() > 1) {
		// More than one block - merge
		PhysicalOrder::ScheduleMergeTasks(pipeline, context, state);
		return false;
	} else {
		// Clean up sorting data - payload is sorted
		for (auto &sb : state.sorted_blocks) {
			sb->UnregisterSortingBlocks();
		}
		return true;
	}
}

void PhysicalOrder::ScheduleMergeTasks(Pipeline &pipeline, ClientContext &context, OrderGlobalState &state) {
	D_ASSERT(state.sorted_blocks_temp.empty());
	if (state.sorted_blocks.size() == 1) {
		for (auto &sb : state.sorted_blocks) {
			sb->UnregisterSortingBlocks();
		}
		pipeline.Finish();
		return;
	}
	// Uneven amount of blocks - keep one on the side
	auto num_blocks = state.sorted_blocks.size();
	if (num_blocks % 2 == 1) {
		state.odd_one_out = move(state.sorted_blocks.back());
		state.sorted_blocks.pop_back();
		num_blocks--;
	}
	// Init merge path path indices
	state.pair_idx = 0;
	state.l_start = 0;
	state.r_start = 0;
	// Compute how many tasks there will be
	idx_t num_tasks = 0;
	const idx_t tuples_per_block = state.block_capacity;
	for (idx_t block_idx = 0; block_idx < num_blocks; block_idx += 2) {
		auto &left = *state.sorted_blocks[block_idx];
		auto &right = *state.sorted_blocks[block_idx + 1];
		const idx_t count = left.Count() + right.Count();
		num_tasks += (count + tuples_per_block - 1) / tuples_per_block;
		// Allocate room for merge results
		state.sorted_blocks_temp.emplace_back();
	}
	// Schedule the tasks
	pipeline.total_tasks += num_tasks;
	for (idx_t tnum = 0; tnum < num_tasks; tnum++) {
		auto new_task = make_unique<PhysicalOrderMergeTask>(pipeline, context, state);
		TaskScheduler::GetScheduler(context).ScheduleTask(pipeline.token, move(new_task));
	}
}

//===--------------------------------------------------------------------===//
// GetChunkInternal
//===--------------------------------------------------------------------===//
class PhysicalOrderOperatorState : public PhysicalOperatorState {
public:
	PhysicalOrderOperatorState(PhysicalOperator &op, PhysicalOperator *child)
	    : PhysicalOperatorState(op, child), initialized(false), global_entry_idx(0), block_idx(0), entry_idx(0) {
	}
	bool initialized;

	SortedData *payload_data;
	Vector addresses = Vector(LogicalType::POINTER);

	idx_t global_entry_idx;
	idx_t block_idx;
	idx_t entry_idx;
};

unique_ptr<PhysicalOperatorState> PhysicalOrder::GetOperatorState() {
	return make_unique<PhysicalOrderOperatorState>(*this, children[0].get());
}

//! Scans the payload of the final SortedBlock result
static void Scan(ClientContext &context, DataChunk &chunk, PhysicalOrderOperatorState &state, OrderGlobalState &gstate,
                 const idx_t &scan_count) {
	auto &buffer_manager = BufferManager::GetBufferManager(context);
	auto &payload_data = *state.payload_data;
	const auto &layout = gstate.payload_layout;
	const idx_t &row_width = layout.GetRowWidth();
	vector<unique_ptr<BufferHandle>> handles;
	// Set up a batch of pointers to scan data from
	idx_t count = 0;
	auto data_pointers = FlatVector::GetData<data_ptr_t>(state.addresses);
	while (count < scan_count) {
		auto &data_block = payload_data.data_blocks[state.block_idx];
		idx_t next = MinValue(data_block.count - state.entry_idx, scan_count - count);
		auto data_handle = buffer_manager.Pin(data_block.block);
		const data_ptr_t payload_dataptr = data_handle->Ptr() + state.entry_idx * row_width;
		handles.push_back(move(data_handle));
		// Set up the next pointers
		data_ptr_t row_ptr = payload_dataptr;
		for (idx_t i = 0; i < next; i++) {
			data_pointers[count + i] = row_ptr;
			row_ptr += row_width;
		}
		// Unswizzle the offsets back to pointers (if needed)
		if (!layout.AllConstant() && gstate.external) {
			auto heap_handle = buffer_manager.Pin(payload_data.heap_blocks[state.block_idx].block);
			RowOperations::UnswizzleHeapPointer(layout, payload_dataptr, heap_handle->Ptr(), next);
			RowOperations::UnswizzleColumns(layout, payload_dataptr, next);
			handles.push_back(move(heap_handle));
		}
		// Update state indices
		state.entry_idx += next;
		if (state.entry_idx == data_block.count) {
			state.block_idx++;
			state.entry_idx = 0;
		}
		count += next;
	}
	D_ASSERT(count == scan_count);
	state.global_entry_idx += scan_count;
	// Deserialize the payload data
	for (idx_t col_idx = 0; col_idx < layout.ColumnCount(); col_idx++) {
		const auto col_offset = layout.GetOffsets()[col_idx];
		RowOperations::Gather(state.addresses, FlatVector::INCREMENTAL_SELECTION_VECTOR, chunk.data[col_idx],
		                      FlatVector::INCREMENTAL_SELECTION_VECTOR, scan_count, col_offset, col_idx);
	}
}

void PhysicalOrder::GetChunkInternal(ExecutionContext &context, DataChunk &chunk,
                                     PhysicalOperatorState *state_p) const {
	auto &state = *reinterpret_cast<PhysicalOrderOperatorState *>(state_p);
	auto &gstate = (OrderGlobalState &)*this->sink_state;

	if (gstate.sorted_blocks.empty()) {
		return;
	}

	if (!state.initialized) {
		D_ASSERT(gstate.sorted_blocks.back()->Count() == gstate.total_count);
		state.payload_data = gstate.sorted_blocks.back()->payload_data.get();
		state.initialized = true;
	}

	auto next = MinValue((idx_t)STANDARD_VECTOR_SIZE, gstate.total_count - state.global_entry_idx);
	Scan(context.client, chunk, state, gstate, next);
	chunk.SetCardinality(next);
	chunk.Verify();
}

string PhysicalOrder::ParamsToString() const {
	string result;
	for (idx_t i = 0; i < orders.size(); i++) {
		if (i > 0) {
			result += "\n";
		}
		result += orders[i].expression->ToString() + " ";
		result += orders[i].type == OrderType::DESCENDING ? "DESC" : "ASC";
	}
	return result;
}

} // namespace duckdb
