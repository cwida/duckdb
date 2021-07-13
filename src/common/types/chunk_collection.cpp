#include "duckdb/common/types/chunk_collection.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/printer.hpp"
#include "duckdb/common/value_operations/value_operations.hpp"
#include "duckdb/common/operator/comparison_operators.hpp"
#include "duckdb/common/assert.hpp"

#include <algorithm>
#include <cstring>
#include <queue>

namespace duckdb {

void ChunkCollection::Verify() {
#ifdef DEBUG
	for (auto &chunk : chunks) {
		chunk->Verify();
	}
#endif
}

void ChunkCollection::Append(ChunkCollection &other) {
	for (auto &chunk : other.chunks) {
		Append(*chunk);
	}
}

void ChunkCollection::Merge(ChunkCollection &other) {
	if (other.count == 0) {
		return;
	}
	if (count == 0) {
		chunks = move(other.chunks);
		types = move(other.types);
		count = other.count;
		return;
	}
	unique_ptr<DataChunk> old_back;
	if (!chunks.empty() && chunks.back()->size() != STANDARD_VECTOR_SIZE) {
		old_back = move(chunks.back());
		chunks.pop_back();
		count -= old_back->size();
	}
	for (auto &chunk : other.chunks) {
		chunks.push_back(move(chunk));
	}
	count += other.count;
	if (old_back) {
		Append(*old_back);
	}
	Verify();
}

void ChunkCollection::Append(DataChunk &new_chunk) {
	if (new_chunk.size() == 0) {
		return;
	}
	new_chunk.Verify();

	// we have to ensure that every chunk in the ChunkCollection is completely
	// filled, otherwise our O(1) lookup in GetValue and SetValue does not work
	// first fill the latest chunk, if it exists
	count += new_chunk.size();

	idx_t remaining_data = new_chunk.size();
	idx_t offset = 0;
	if (chunks.empty()) {
		// first chunk
		types = new_chunk.GetTypes();
	} else {
		// the types of the new chunk should match the types of the previous one
		D_ASSERT(types.size() == new_chunk.ColumnCount());
		auto new_types = new_chunk.GetTypes();
		for (idx_t i = 0; i < types.size(); i++) {
			if (new_types[i] != types[i]) {
				throw TypeMismatchException(new_types[i], types[i], "Type mismatch when combining rows");
			}
			if (types[i].InternalType() == PhysicalType::LIST) {
				// need to check all the chunks because they can have only-null list entries
				for (auto &chunk : chunks) {
					auto &chunk_vec = chunk->data[i];
					auto &new_vec = new_chunk.data[i];
					auto &chunk_type = chunk_vec.GetType();
					auto &new_type = new_vec.GetType();
					if (chunk_type != new_type) {
						throw TypeMismatchException(chunk_type, new_type, "Type mismatch when combining lists");
					}
				}
			}
			// TODO check structs, too
		}

		// first append data to the current chunk
		DataChunk &last_chunk = *chunks.back();
		idx_t added_data = MinValue<idx_t>(remaining_data, STANDARD_VECTOR_SIZE - last_chunk.size());
		if (added_data > 0) {
			// copy <added_data> elements to the last chunk
			new_chunk.Normalify();
			// have to be careful here: setting the cardinality without calling normalify can cause incorrect partial
			// decompression
			idx_t old_count = new_chunk.size();
			new_chunk.SetCardinality(added_data);

			last_chunk.Append(new_chunk);
			remaining_data -= added_data;
			// reset the chunk to the old data
			new_chunk.SetCardinality(old_count);
			offset = added_data;
		}
	}

	if (remaining_data > 0) {
		// create a new chunk and fill it with the remainder
		auto chunk = make_unique<DataChunk>();
		chunk->Initialize(types);
		new_chunk.Copy(*chunk, offset);
		chunks.push_back(move(chunk));
	}
}

// returns an int similar to a C comparator:
// -1 if left < right
// 0 if left == right
// 1 if left > right

template <class TYPE>
static int8_t TemplatedCompareValue(Vector &left_vec, Vector &right_vec, idx_t left_idx, idx_t right_idx) {
	D_ASSERT(left_vec.GetType() == right_vec.GetType());
	auto left_val = FlatVector::GetData<TYPE>(left_vec)[left_idx];
	auto right_val = FlatVector::GetData<TYPE>(right_vec)[right_idx];
	if (Equals::Operation<TYPE>(left_val, right_val)) {
		return 0;
	}
	if (LessThan::Operation<TYPE>(left_val, right_val)) {
		return -1;
	}
	return 1;
}

// return type here is int32 because strcmp() on some platforms returns rather large values
static int32_t CompareValue(Vector &left_vec, Vector &right_vec, idx_t vector_idx_left, idx_t vector_idx_right,
                            OrderByNullType null_order) {
	auto left_null = FlatVector::IsNull(left_vec, vector_idx_left);
	auto right_null = FlatVector::IsNull(right_vec, vector_idx_right);

	if (left_null && right_null) {
		return 0;
	} else if (right_null) {
		return null_order == OrderByNullType::NULLS_FIRST ? 1 : -1;
	} else if (left_null) {
		return null_order == OrderByNullType::NULLS_FIRST ? -1 : 1;
	}

	switch (left_vec.GetType().InternalType()) {
	case PhysicalType::BOOL:
	case PhysicalType::INT8:
		return TemplatedCompareValue<int8_t>(left_vec, right_vec, vector_idx_left, vector_idx_right);
	case PhysicalType::INT16:
		return TemplatedCompareValue<int16_t>(left_vec, right_vec, vector_idx_left, vector_idx_right);
	case PhysicalType::INT32:
		return TemplatedCompareValue<int32_t>(left_vec, right_vec, vector_idx_left, vector_idx_right);
	case PhysicalType::INT64:
		return TemplatedCompareValue<int64_t>(left_vec, right_vec, vector_idx_left, vector_idx_right);
	case PhysicalType::UINT8:
		return TemplatedCompareValue<uint8_t>(left_vec, right_vec, vector_idx_left, vector_idx_right);
	case PhysicalType::UINT16:
		return TemplatedCompareValue<uint16_t>(left_vec, right_vec, vector_idx_left, vector_idx_right);
	case PhysicalType::UINT32:
		return TemplatedCompareValue<uint32_t>(left_vec, right_vec, vector_idx_left, vector_idx_right);
	case PhysicalType::UINT64:
		return TemplatedCompareValue<uint64_t>(left_vec, right_vec, vector_idx_left, vector_idx_right);
	case PhysicalType::INT128:
		return TemplatedCompareValue<hugeint_t>(left_vec, right_vec, vector_idx_left, vector_idx_right);
	case PhysicalType::FLOAT:
		return TemplatedCompareValue<float>(left_vec, right_vec, vector_idx_left, vector_idx_right);
	case PhysicalType::DOUBLE:
		return TemplatedCompareValue<double>(left_vec, right_vec, vector_idx_left, vector_idx_right);
	case PhysicalType::VARCHAR:
		return TemplatedCompareValue<string_t>(left_vec, right_vec, vector_idx_left, vector_idx_right);
	case PhysicalType::INTERVAL:
		return TemplatedCompareValue<interval_t>(left_vec, right_vec, vector_idx_left, vector_idx_right);
	default:
		throw NotImplementedException("Type for comparison");
	}
}

static int CompareTuple(ChunkCollection *sort_by, vector<OrderType> &desc, vector<OrderByNullType> &null_order,
                        idx_t left, idx_t right) {
	D_ASSERT(sort_by);

	idx_t chunk_idx_left = left / STANDARD_VECTOR_SIZE;
	idx_t chunk_idx_right = right / STANDARD_VECTOR_SIZE;
	idx_t vector_idx_left = left % STANDARD_VECTOR_SIZE;
	idx_t vector_idx_right = right % STANDARD_VECTOR_SIZE;

	auto &left_chunk = sort_by->GetChunk(chunk_idx_left);
	auto &right_chunk = sort_by->GetChunk(chunk_idx_right);

	for (idx_t col_idx = 0; col_idx < desc.size(); col_idx++) {
		auto order_type = desc[col_idx];

		auto &left_vec = left_chunk.data[col_idx];
		auto &right_vec = right_chunk.data[col_idx];

		D_ASSERT(left_vec.GetVectorType() == VectorType::FLAT_VECTOR);
		D_ASSERT(right_vec.GetVectorType() == VectorType::FLAT_VECTOR);
		D_ASSERT(left_vec.GetType() == right_vec.GetType());

		auto comp_res = CompareValue(left_vec, right_vec, vector_idx_left, vector_idx_right, null_order[col_idx]);

		if (comp_res == 0) {
			continue;
		}
		return comp_res < 0 ? (order_type == OrderType::ASCENDING ? -1 : 1)
		                    : (order_type == OrderType::ASCENDING ? 1 : -1);
	}
	return 0;
}

static int64_t QuicksortInitial(ChunkCollection *sort_by, vector<OrderType> &desc, vector<OrderByNullType> &null_order,
                                idx_t *result) {
	// select pivot
	int64_t pivot = 0;
	int64_t low = 0, high = sort_by->Count() - 1;
	// now insert elements
	for (idx_t i = 1; i < sort_by->Count(); i++) {
		if (CompareTuple(sort_by, desc, null_order, i, pivot) <= 0) {
			result[low++] = i;
		} else {
			result[high--] = i;
		}
	}
	D_ASSERT(low == high);
	result[low] = pivot;
	return low;
}

struct QuicksortInfo {
	QuicksortInfo(int64_t left_p, int64_t right_p) : left(left_p), right(right_p) {
	}

	int64_t left;
	int64_t right;
};

struct QuicksortStack {
	std::queue<QuicksortInfo> info_queue;

	QuicksortInfo Pop() {
		auto element = info_queue.front();
		info_queue.pop();
		return element;
	}

	bool IsEmpty() {
		return info_queue.empty();
	}

	void Enqueue(int64_t left, int64_t right) {
		if (left >= right) {
			return;
		}
		info_queue.emplace(left, right);
	}
};

static void QuicksortInPlace(ChunkCollection *sort_by, vector<OrderType> &desc, vector<OrderByNullType> &null_order,
                             idx_t *result, QuicksortInfo info, QuicksortStack &stack) {
	auto left = info.left;
	auto right = info.right;

	D_ASSERT(left < right);

	int64_t middle = left + (right - left) / 2;
	int64_t pivot = result[middle];
	// move the mid point value to the front.
	int64_t i = left + 1;
	int64_t j = right;

	std::swap(result[middle], result[left]);
	bool all_equal = true;
	while (i <= j) {
		if (result) {
			while (i <= j) {
				int cmp = CompareTuple(sort_by, desc, null_order, result[i], pivot);
				if (cmp < 0) {
					all_equal = false;
				} else if (cmp > 0) {
					all_equal = false;
					break;
				}
				i++;
			}
		}

		while (i <= j && CompareTuple(sort_by, desc, null_order, result[j], pivot) > 0) {
			j--;
		}

		if (i < j) {
			std::swap(result[i], result[j]);
		}
	}
	std::swap(result[i - 1], result[left]);
	int64_t part = i - 1;

	if (all_equal) {
		return;
	}

	stack.Enqueue(left, part - 1);
	stack.Enqueue(part + 1, right);
}

void ChunkCollection::Sort(vector<OrderType> &desc, vector<OrderByNullType> &null_order, idx_t result[]) {
	D_ASSERT(result);
	if (count == 0) {
		return;
	}
	// start off with an initial quicksort
	int64_t part = QuicksortInitial(this, desc, null_order, result);

	// now continuously perform
	QuicksortStack stack;
	stack.Enqueue(0, part);
	stack.Enqueue(part + 1, count - 1);
	while (!stack.IsEmpty()) {
		auto element = stack.Pop();
		QuicksortInPlace(this, desc, null_order, result, element, stack);
	}
}

// FIXME make this more efficient by not using the Value API
// just use memcpy in the vectors
// assert that there is no selection list
void ChunkCollection::Reorder(idx_t order_org[]) {
	auto order = unique_ptr<idx_t[]>(new idx_t[count]);
	memcpy(order.get(), order_org, sizeof(idx_t) * count);

	// adapted from https://stackoverflow.com/a/7366196/2652376

	auto val_buf = vector<Value>();
	val_buf.resize(ColumnCount());

	idx_t j, k;
	for (idx_t i = 0; i < count; i++) {
		for (idx_t col_idx = 0; col_idx < ColumnCount(); col_idx++) {
			val_buf[col_idx] = GetValue(col_idx, i);
		}
		j = i;
		while (true) {
			k = order[j];
			order[j] = j;
			if (k == i) {
				break;
			}
			for (idx_t col_idx = 0; col_idx < ColumnCount(); col_idx++) {
				SetValue(col_idx, j, GetValue(col_idx, k));
			}
			j = k;
		}
		for (idx_t col_idx = 0; col_idx < ColumnCount(); col_idx++) {
			SetValue(col_idx, j, val_buf[col_idx]);
		}
	}
}

template <class TYPE>
static void TemplatedSetValues(ChunkCollection *src_coll, Vector &tgt_vec, idx_t order[], idx_t col_idx,
                               idx_t start_offset, idx_t remaining_data) {
	D_ASSERT(src_coll);

	for (idx_t row_idx = 0; row_idx < remaining_data; row_idx++) {
		idx_t chunk_idx_src = order[start_offset + row_idx] / STANDARD_VECTOR_SIZE;
		idx_t vector_idx_src = order[start_offset + row_idx] % STANDARD_VECTOR_SIZE;

		auto &src_chunk = src_coll->GetChunk(chunk_idx_src);
		Vector &src_vec = src_chunk.data[col_idx];
		auto source_data = FlatVector::GetData<TYPE>(src_vec);
		auto target_data = FlatVector::GetData<TYPE>(tgt_vec);

		if (FlatVector::IsNull(src_vec, vector_idx_src)) {
			FlatVector::SetNull(tgt_vec, row_idx, true);
		} else {
			target_data[row_idx] = source_data[vector_idx_src];
		}
	}
}

Value ChunkCollection::GetValue(idx_t column, idx_t index) {
	return chunks[LocateChunk(index)]->GetValue(column, index % STANDARD_VECTOR_SIZE);
}

void ChunkCollection::SetValue(idx_t column, idx_t index, const Value &value) {
	chunks[LocateChunk(index)]->SetValue(column, index % STANDARD_VECTOR_SIZE, value);
}

void ChunkCollection::Print() {
	Printer::Print(ToString());
}

bool ChunkCollection::Equals(ChunkCollection &other) {
	if (count != other.count) {
		return false;
	}
	if (ColumnCount() != other.ColumnCount()) {
		return false;
	}
	if (types != other.types) {
		return false;
	}
	// if count is equal amount of chunks should be equal
	for (idx_t row_idx = 0; row_idx < count; row_idx++) {
		for (idx_t col_idx = 0; col_idx < ColumnCount(); col_idx++) {
			auto lvalue = GetValue(col_idx, row_idx);
			auto rvalue = other.GetValue(col_idx, row_idx);
			if (!Value::ValuesAreEqual(lvalue, rvalue)) {
				return false;
			}
		}
	}
	return true;
}
static void Heapify(ChunkCollection *input, vector<OrderType> &desc, vector<OrderByNullType> &null_order, idx_t *heap,
                    idx_t heap_size, idx_t current_index) {
	if (current_index >= heap_size) {
		return;
	}
	idx_t left_child_index = current_index * 2 + 1;
	idx_t right_child_index = current_index * 2 + 2;
	idx_t swap_index = current_index;

	if (left_child_index < heap_size) {
		swap_index = CompareTuple(input, desc, null_order, heap[swap_index], heap[left_child_index]) <= 0
		                 ? left_child_index
		                 : swap_index;
	}

	if (right_child_index < heap_size) {
		swap_index = CompareTuple(input, desc, null_order, heap[swap_index], heap[right_child_index]) <= 0
		                 ? right_child_index
		                 : swap_index;
	}

	if (swap_index != current_index) {
		std::swap(heap[current_index], heap[swap_index]);
		Heapify(input, desc, null_order, heap, heap_size, swap_index);
	}
}

static void HeapCreate(ChunkCollection *input, vector<OrderType> &desc, vector<OrderByNullType> &null_order,
                       idx_t *heap, idx_t heap_size) {
	for (idx_t i = 0; i < heap_size; i++) {
		heap[i] = i;
	}

	// build heap
	for (int64_t i = heap_size / 2 - 1; i >= 0; i--) {
		Heapify(input, desc, null_order, heap, heap_size, i);
	}

	// Run through all the rows.
	for (idx_t i = heap_size; i < input->Count(); i++) {
		if (CompareTuple(input, desc, null_order, i, heap[0]) <= 0) {
			heap[0] = i;
			Heapify(input, desc, null_order, heap, heap_size, 0);
		}
	}
}

void ChunkCollection::Heap(vector<OrderType> &desc, vector<OrderByNullType> &null_order, idx_t heap[],
                           idx_t heap_size) {
	D_ASSERT(heap);
	if (count == 0) {
		return;
	}

	HeapCreate(this, desc, null_order, heap, heap_size);

	// Heap is ready. Now do a heapsort
	for (int64_t i = heap_size - 1; i >= 0; i--) {
		std::swap(heap[i], heap[0]);
		Heapify(this, desc, null_order, heap, i, 0);
	}
}

idx_t ChunkCollection::MaterializeHeapChunk(DataChunk &target, idx_t order[], idx_t start_offset, idx_t heap_size) {
	idx_t remaining_data = MinValue<idx_t>(STANDARD_VECTOR_SIZE, heap_size - start_offset);
	D_ASSERT(target.GetTypes() == types);

	target.SetCardinality(remaining_data);
	for (idx_t col_idx = 0; col_idx < ColumnCount(); col_idx++) {
		switch (types[col_idx].InternalType()) {
		case PhysicalType::BOOL:
		case PhysicalType::INT8:
			TemplatedSetValues<int8_t>(this, target.data[col_idx], order, col_idx, start_offset, remaining_data);
			break;
		case PhysicalType::INT16:
			TemplatedSetValues<int16_t>(this, target.data[col_idx], order, col_idx, start_offset, remaining_data);
			break;
		case PhysicalType::INT32:
			TemplatedSetValues<int32_t>(this, target.data[col_idx], order, col_idx, start_offset, remaining_data);
			break;
		case PhysicalType::INT64:
			TemplatedSetValues<int64_t>(this, target.data[col_idx], order, col_idx, start_offset, remaining_data);
			break;
		case PhysicalType::INT128:
			TemplatedSetValues<hugeint_t>(this, target.data[col_idx], order, col_idx, start_offset, remaining_data);
			break;
		case PhysicalType::FLOAT:
			TemplatedSetValues<float>(this, target.data[col_idx], order, col_idx, start_offset, remaining_data);
			break;
		case PhysicalType::DOUBLE:
			TemplatedSetValues<double>(this, target.data[col_idx], order, col_idx, start_offset, remaining_data);
			break;
		case PhysicalType::VARCHAR:
			TemplatedSetValues<string_t>(this, target.data[col_idx], order, col_idx, start_offset, remaining_data);
			break;
		// TODO this is ugly and sloooow!
		case PhysicalType::STRUCT:
		case PhysicalType::LIST: {
			for (idx_t row_idx = 0; row_idx < remaining_data; row_idx++) {
				idx_t chunk_idx_src = order[start_offset + row_idx] / STANDARD_VECTOR_SIZE;
				idx_t vector_idx_src = order[start_offset + row_idx] % STANDARD_VECTOR_SIZE;

				auto &src_chunk = chunks[chunk_idx_src];
				Vector &src_vec = src_chunk->data[col_idx];
				auto &tgt_vec = target.data[col_idx];
				if (FlatVector::IsNull(src_vec, vector_idx_src)) {
					FlatVector::SetNull(tgt_vec, row_idx, true);
				} else {
					tgt_vec.SetValue(row_idx, src_vec.GetValue(vector_idx_src));
				}
			}
		} break;

		default:
			throw NotImplementedException("Type is unsupported in MaterializeHeapChunk()");
		}
	}
	target.Verify();
	return start_offset + remaining_data;
}

} // namespace duckdb
