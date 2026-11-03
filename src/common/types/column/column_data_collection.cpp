#include "duckdb/common/vector/array_vector.hpp"
#include "duckdb/common/vector/constant_vector.hpp"
#include "duckdb/common/vector/dictionary_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/map_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"

#include "duckdb/common/printer.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/column/column_data_collection_segment.hpp"
#include "duckdb/common/types/value_map.hpp"
#include "duckdb/common/uhugeint.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/storage/buffer_manager.hpp"

namespace duckdb {

struct ColumnDataMetaData;

typedef void (*column_data_copy_function_t)(ColumnDataMetaData &meta_data, const UnifiedVectorFormat &source_data,
                                            Vector &source, idx_t offset, idx_t copy_count);

struct ColumnDataCopyFunction {
	column_data_copy_function_t function;
	vector<ColumnDataCopyFunction> child_functions;
};

struct ColumnDataMetaData {
	ColumnDataMetaData(ColumnDataCopyFunction &copy_function, ColumnDataCollectionSegment &segment,
	                   ColumnDataAppendState &state, ChunkMetaData &chunk_data, VectorDataIndex vector_data_index)
	    : copy_function(copy_function), segment(segment), state(state), chunk_data(chunk_data),
	      vector_data_index(vector_data_index) {
	}
	ColumnDataMetaData(ColumnDataCopyFunction &copy_function, ColumnDataMetaData &parent,
	                   VectorDataIndex vector_data_index)
	    : copy_function(copy_function), segment(parent.segment), state(parent.state), chunk_data(parent.chunk_data),
	      vector_data_index(vector_data_index) {
	}

	ColumnDataCopyFunction &copy_function;
	ColumnDataCollectionSegment &segment;
	ColumnDataAppendState &state;
	ChunkMetaData &chunk_data;
	VectorDataIndex vector_data_index;
	idx_t child_list_size = DConstants::INVALID_INDEX;

	VectorMetaData &GetVectorMetaData() {
		return segment.GetVectorData(vector_data_index);
	}
};

//! Explicitly initialized without types
ColumnDataCollection::ColumnDataCollection(Allocator &allocator_p) {
	types.clear();
	count = 0;
	this->finished_append = false;
	allocator = make_shared_ptr<ColumnDataAllocator>(allocator_p);
}

ColumnDataCollection::ColumnDataCollection(Allocator &allocator_p, vector<LogicalType> types_p) {
	Initialize(std::move(types_p));
	allocator = make_shared_ptr<ColumnDataAllocator>(allocator_p);
}

ColumnDataCollection::ColumnDataCollection(BufferManager &buffer_manager, vector<LogicalType> types_p,
                                           ColumnDataCollectionLifetime lifetime) {
	Initialize(std::move(types_p));
	allocator = make_shared_ptr<ColumnDataAllocator>(buffer_manager, lifetime);
}

ColumnDataCollection::ColumnDataCollection(shared_ptr<ColumnDataAllocator> allocator_p, vector<LogicalType> types_p) {
	Initialize(std::move(types_p));
	this->allocator = std::move(allocator_p);
}

ColumnDataCollection::ColumnDataCollection(ClientContext &context, vector<LogicalType> types_p,
                                           ColumnDataAllocatorType type, ColumnDataCollectionLifetime lifetime)
    : ColumnDataCollection(make_shared_ptr<ColumnDataAllocator>(context, type, lifetime), std::move(types_p)) {
}

ColumnDataCollection::ColumnDataCollection(ColumnDataCollection &other)
    : ColumnDataCollection(other.allocator, other.types) {
	other.finished_append = true;
}

ColumnDataCollection::~ColumnDataCollection() {
}

void ColumnDataCollection::Initialize(vector<LogicalType> types_p) {
	this->types = std::move(types_p);
	this->count = 0;
	this->finished_append = false;
	copy_functions.reserve(types.size());
	for (auto &type : types) {
		copy_functions.push_back(GetCopyFunction(type));
	}
}

void ColumnDataCollection::CreateSegment() {
	segments.emplace_back(make_uniq<ColumnDataCollectionSegment>(allocator, types));
}

Allocator &ColumnDataCollection::GetAllocator() const {
	return allocator->GetAllocator();
}

idx_t ColumnDataCollection::SizeInBytes() const {
	idx_t total_size = 0;
	for (const auto &segment : segments) {
		total_size += segment->SizeInBytes();
	}
	return total_size;
}

idx_t ColumnDataCollection::AllocationSize() const {
	idx_t total_size = 0;
	for (const auto &segment : segments) {
		total_size += segment->AllocationSize();
	}
	return total_size;
}

void ColumnDataCollection::SetPartitionIndex(const idx_t index) {
	D_ASSERT(!partition_index.IsValid());
	D_ASSERT(Count() == 0);
	partition_index = index;
	allocator->SetPartitionIndex(index);
}

//===--------------------------------------------------------------------===//
// ColumnDataRow
//===--------------------------------------------------------------------===//
ColumnDataRow::ColumnDataRow(DataChunk &chunk_p, idx_t row_index, idx_t base_index)
    : chunk(chunk_p), row_index(row_index), base_index(base_index) {
}

Value ColumnDataRow::GetValue(idx_t column_index) const {
	D_ASSERT(column_index < chunk.ColumnCount());
	D_ASSERT(row_index < chunk.size());
	return chunk.data[column_index].GetValue(row_index);
}

idx_t ColumnDataRow::RowIndex() const {
	return base_index + row_index;
}

//===--------------------------------------------------------------------===//
// ColumnDataRowCollection
//===--------------------------------------------------------------------===//
ColumnDataRowCollection::ColumnDataRowCollection(const ColumnDataCollection &collection,
                                                 const ColumnDataScanProperties properties) {
	if (collection.Count() == 0) {
		return;
	}
	// read all the chunks
	ColumnDataScanState temp_scan_state;
	collection.InitializeScan(temp_scan_state, properties);
	while (true) {
		auto chunk = make_uniq<DataChunk>();
		// Use default allocator so the chunk is independently usable even after the DB allocator is destroyed
		if (properties == ColumnDataScanProperties::DISALLOW_ZERO_COPY) {
			collection.InitializeScanChunk(Allocator::DefaultAllocator(), *chunk);
		} else {
			collection.InitializeScanChunk(*chunk);
		}
		if (!collection.Scan(temp_scan_state, *chunk)) {
			break;
		}
		chunks.push_back(std::move(chunk));
	}
	// now create all of the column data rows
	rows.reserve(collection.Count());
	idx_t base_row = 0;
	for (auto &chunk : chunks) {
		for (idx_t row_idx = 0; row_idx < chunk->size(); row_idx++) {
			rows.emplace_back(*chunk, row_idx, base_row);
		}
		base_row += chunk->size();
	}
}

ColumnDataRow &ColumnDataRowCollection::operator[](idx_t i) {
	return rows[i];
}

const ColumnDataRow &ColumnDataRowCollection::operator[](idx_t i) const {
	return rows[i];
}

Value ColumnDataRowCollection::GetValue(idx_t column, idx_t index) const {
	return rows[index].GetValue(column);
}

//===--------------------------------------------------------------------===//
// ColumnDataChunkIterator
//===--------------------------------------------------------------------===//
ColumnDataChunkIterationHelper ColumnDataCollection::Chunks() const {
	vector<column_t> column_ids;
	for (idx_t i = 0; i < ColumnCount(); i++) {
		column_ids.push_back(i);
	}
	return Chunks(column_ids);
}

ColumnDataChunkIterationHelper ColumnDataCollection::Chunks(vector<column_t> column_ids) const {
	return ColumnDataChunkIterationHelper(*this, std::move(column_ids));
}

ColumnDataChunkIterationHelper::ColumnDataChunkIterationHelper(const ColumnDataCollection &collection_p,
                                                               vector<column_t> column_ids_p)
    : collection(collection_p), column_ids(std::move(column_ids_p)) {
}

ColumnDataChunkIterationHelper::ColumnDataChunkIterator::ColumnDataChunkIterator(
    optional_ptr<const ColumnDataCollection> collection_p, vector<column_t> column_ids_p)
    : collection(collection_p), scan_chunk(make_uniq<DataChunk>()), row_index(0) {
	if (!collection) {
		return;
	}
	collection->InitializeScan(scan_state, std::move(column_ids_p));
	collection->InitializeScanChunk(scan_state, *scan_chunk);
	collection->Scan(scan_state, *scan_chunk);
}

ColumnDataChunkIterationHelper::ColumnDataChunkIterator::ColumnDataChunkIterator(
    ColumnDataChunkIterator &&other) noexcept
    : row_index(0) {
	std::swap(collection, other.collection);
	std::swap(scan_state, other.scan_state);
	std::swap(scan_chunk, other.scan_chunk);
	std::swap(row_index, other.row_index);
}

void ColumnDataChunkIterationHelper::ColumnDataChunkIterator::Next() {
	if (!collection) {
		return;
	}
	if (!collection->Scan(scan_state, *scan_chunk)) {
		collection = nullptr;
		row_index = 0;
	} else {
		row_index += scan_chunk->size();
	}
}

ColumnDataChunkIterationHelper::ColumnDataChunkIterator &
ColumnDataChunkIterationHelper::ColumnDataChunkIterator::operator++() {
	Next();
	return *this;
}

bool ColumnDataChunkIterationHelper::ColumnDataChunkIterator::operator!=(const ColumnDataChunkIterator &other) const {
	return collection != other.collection || row_index != other.row_index;
}

DataChunk &ColumnDataChunkIterationHelper::ColumnDataChunkIterator::operator*() const {
	return *scan_chunk;
}

//===--------------------------------------------------------------------===//
// ColumnDataRowIterator
//===--------------------------------------------------------------------===//
ColumnDataRowIterationHelper ColumnDataCollection::Rows() const {
	return ColumnDataRowIterationHelper(*this);
}

ColumnDataRowIterationHelper::ColumnDataRowIterationHelper(const ColumnDataCollection &collection_p)
    : collection(collection_p) {
}

ColumnDataRowIterationHelper::ColumnDataRowIterator::ColumnDataRowIterator(const ColumnDataCollection *collection_p,
                                                                           ColumnDataScanProperties properties)
    : collection(collection_p), scan_chunk(make_shared_ptr<DataChunk>()), current_row(*scan_chunk, 0, 0) {
	if (!collection) {
		return;
	}
	collection->InitializeScan(scan_state, properties);
	collection->InitializeScanChunk(*scan_chunk);
	collection->Scan(scan_state, *scan_chunk);
}

void ColumnDataRowIterationHelper::ColumnDataRowIterator::Next() {
	if (!collection) {
		return;
	}
	current_row.row_index++;
	if (current_row.row_index >= scan_chunk->size()) {
		current_row.base_index += scan_chunk->size();
		current_row.row_index = 0;
		if (!collection->Scan(scan_state, *scan_chunk)) {
			// exhausted collection: move iterator to nop state
			current_row.base_index = 0;
			collection = nullptr;
		}
	}
}

ColumnDataRowIterationHelper::ColumnDataRowIterator ColumnDataRowIterationHelper::begin() { // NOLINT
	return ColumnDataRowIterationHelper::ColumnDataRowIterator(collection.Count() == 0 ? nullptr : &collection);
}
ColumnDataRowIterationHelper::ColumnDataRowIterator ColumnDataRowIterationHelper::end() { // NOLINT
	return ColumnDataRowIterationHelper::ColumnDataRowIterator(nullptr);
}

ColumnDataRowIterationHelper::ColumnDataRowIterator &ColumnDataRowIterationHelper::ColumnDataRowIterator::operator++() {
	Next();
	return *this;
}

bool ColumnDataRowIterationHelper::ColumnDataRowIterator::operator!=(const ColumnDataRowIterator &other) const {
	return collection != other.collection || current_row.row_index != other.current_row.row_index ||
	       current_row.base_index != other.current_row.base_index;
}

const ColumnDataRow &ColumnDataRowIterationHelper::ColumnDataRowIterator::operator*() const {
	return current_row;
}

//===--------------------------------------------------------------------===//
// Append
//===--------------------------------------------------------------------===//
void ColumnDataCollection::InitializeAppend(ColumnDataAppendState &state) {
	D_ASSERT(!finished_append);
	state.current_chunk_state.handles.clear();
	state.vector_data.resize(types.size());
	if (segments.empty()) {
		CreateSegment();
	}
	auto &segment = *segments.back();
	if (segment.chunk_data.empty()) {
		segment.AllocateNewChunk();
	}
	segment.InitializeChunkState(segment.chunk_data.size() - 1, state.current_chunk_state);
}

void ColumnDataCopyValidity(const UnifiedVectorFormat &source_data, validity_t *target, idx_t source_offset,
                            idx_t target_offset, idx_t copy_count) {
	ValidityMask validity(target, STANDARD_VECTOR_SIZE);
	if (target_offset == 0) {
		// first time appending to this vector
		// all data here is still uninitialized
		// initialize the validity mask to set all to valid
		validity.SetAllValid(STANDARD_VECTOR_SIZE);
	}
	// FIXME: we can do something more optimized here using bitshifts & bitwise ors
	if (source_data.validity.CanHaveNull()) {
		for (idx_t i = 0; i < copy_count; i++) {
			auto idx = source_data.sel->get_index(source_offset + i);
			if (!source_data.validity.RowIsValid(idx)) {
				validity.SetInvalid(target_offset + i);
			}
		}
	}
}

template <class T>
struct BaseValueCopy {
	static idx_t TypeSize() {
		return sizeof(T);
	}

	template <class OP>
	static void Assign(ColumnDataMetaData &meta_data, data_ptr_t target, const_data_ptr_t source, idx_t target_idx,
	                   idx_t source_idx) {
		auto result_data = (T *)target;
		auto source_data = (const T *)source;
		result_data[target_idx] = OP::Operation(meta_data, source_data[source_idx]);
	}
};

template <class T>
struct StandardValueCopy : public BaseValueCopy<T> {
	static T Operation(ColumnDataMetaData &, T input) {
		return input;
	}
};

struct StringValueCopy : public BaseValueCopy<string_t> {
	static string_t Operation(ColumnDataMetaData &meta_data, string_t input) {
		return meta_data.segment.heap->AddBlob(input);
	}
};

struct ConstListValueCopy : public BaseValueCopy<list_entry_t> {
	using TYPE = list_entry_t;

	static TYPE Operation(ColumnDataMetaData &meta_data, TYPE input) {
		input.offset = meta_data.child_list_size;
		return input;
	}
};

struct ListValueCopy : public BaseValueCopy<list_entry_t> {
	using TYPE = list_entry_t;

	static TYPE Operation(ColumnDataMetaData &meta_data, TYPE input) {
		input.offset = meta_data.child_list_size;
		meta_data.child_list_size += input.length;
		return input;
	}
};

struct StructValueCopy {
	static idx_t TypeSize() {
		return 0;
	}

	template <class OP>
	static void Assign(ColumnDataMetaData &meta_data, data_ptr_t target, const_data_ptr_t source, idx_t target_idx,
	                   idx_t source_idx) {
	}
};

static void EnsureVectorDataAllocated(ColumnDataMetaData &meta_data, VectorDataIndex index, idx_t type_size) {
	auto &segment = meta_data.segment;
	if (segment.GetVectorData(index).is_allocated) {
		return;
	}
	const auto allocation_size = ColumnDataCollectionSegment::GetDataSize(type_size) + ValidityMask::STANDARD_MASK_SIZE;
	segment.AllocateVectorStorage(index, allocation_size, meta_data.chunk_data, &meta_data.state.current_chunk_state);
}

template <class OP>
static void TemplatedColumnDataCopy(ColumnDataMetaData &meta_data, const UnifiedVectorFormat &source_data,
                                    Vector &source, idx_t offset, idx_t count) {
	auto &segment = meta_data.segment;
	auto &append_state = meta_data.state;

	auto current_index = meta_data.vector_data_index;
	idx_t remaining = count;
	while (remaining > 0) {
		EnsureVectorDataAllocated(meta_data, current_index, OP::TypeSize());
		auto &current_segment = segment.GetVectorData(current_index);
		idx_t append_count = MinValue<idx_t>(STANDARD_VECTOR_SIZE - current_segment.count, remaining);

		auto base_ptr = segment.allocator->GetDataPointer(append_state.current_chunk_state, current_segment.block_id,
		                                                  current_segment.offset);
		auto validity_data = ColumnDataCollectionSegment::GetValidityPointerForWriting(base_ptr, OP::TypeSize());

		ValidityMask result_validity(validity_data, STANDARD_VECTOR_SIZE);
		if (current_segment.count == 0) {
			// first time appending to this vector
			// all data here is still uninitialized
			// initialize the validity mask to set all to valid
			result_validity.SetAllValid(STANDARD_VECTOR_SIZE);
		}
		if (source_data.validity.CannotHaveNull()) {
			// Fast path: all valid
			for (idx_t i = 0; i < append_count; i++) {
				auto source_idx = source_data.sel->get_index(offset + i);
				OP::template Assign<OP>(meta_data, base_ptr, source_data.data, current_segment.count + i, source_idx);
			}
		} else {
			for (idx_t i = 0; i < append_count; i++) {
				auto source_idx = source_data.sel->get_index(offset + i);
				if (source_data.validity.RowIsValid(source_idx)) {
					OP::template Assign<OP>(meta_data, base_ptr, source_data.data, current_segment.count + i,
					                        source_idx);
				} else {
					result_validity.SetInvalid(current_segment.count + i);
				}
			}
		}
		current_segment.count += append_count;
		offset += append_count;
		remaining -= append_count;
		if (remaining > 0) {
			// need to append more, check if we need to allocate a new vector or not
			if (!current_segment.next_data.IsValid()) {
				segment.AllocateVector(source.GetType(), meta_data.chunk_data, append_state, current_index);
			}
			D_ASSERT(segment.GetVectorData(current_index).next_data.IsValid());
			current_index = segment.GetVectorData(current_index).next_data;
		}
	}
}

template <class T>
static void ColumnDataCopy(ColumnDataMetaData &meta_data, const UnifiedVectorFormat &source_data, Vector &source,
                           idx_t offset, idx_t copy_count) {
	TemplatedColumnDataCopy<StandardValueCopy<T>>(meta_data, source_data, source, offset, copy_count);
}

//! Shares the string bytes of constant and small-dictionary appends that did not become a compact entry,
//! e.g. in managed result collections or entries that already hold flat rows
bool ColumnDataCopyCompressedStrings(ColumnDataMetaData &meta_data, const VectorDataIndex &current_index,
                                     VectorDataIndex &child_index, const UnifiedVectorFormat &source_data,
                                     Vector &source, const idx_t &offset, const idx_t &vector_remaining,
                                     idx_t &append_count, idx_t &heap_size, data_ptr_t &base_heap_ptr) {
	// check if we can do the optimization at all
	switch (source.GetVectorType()) {
	case VectorType::CONSTANT_VECTOR: {
		const auto &constant_string = ConstantVector::GetData<string_t>(source)[0];
		if (ConstantVector::IsNull(source) || constant_string.IsInlined()) {
			return false; // regular path is OK
		}
		heap_size = constant_string.GetSize();
		break;
	}
	case VectorType::DICTIONARY_VECTOR: {
		const auto dictionary_size = DictionaryVector::DictionarySize(source);
		if (!dictionary_size.IsValid() || dictionary_size.GetIndex() >= vector_remaining / 2) {
			return false; // not a dictionary from storage or dictionary too large
		}

		const auto &dictionary_vector = DictionaryVector::Child(source);
		const auto dictionary_strings = FlatVector::GetData<string_t>(dictionary_vector);
		const auto &dictionary_validity = FlatVector::Validity(dictionary_vector);

		// Compute total size needed for dictionary strings
		const auto dictionary_size_idx = dictionary_size.GetIndex();
		if (dictionary_validity.CannotHaveNull()) {
			for (idx_t i = 0; i < dictionary_size_idx; i++) {
				const auto &dictionary_string = dictionary_strings[i];
				heap_size += !dictionary_string.IsInlined() * dictionary_string.GetSize();
			}
		} else {
			for (idx_t i = 0; i < dictionary_size_idx; i++) {
				const auto &dictionary_string = dictionary_strings[i];
				const auto add_size = dictionary_validity.RowIsValidUnsafe(i) && !dictionary_string.IsInlined();
				heap_size += add_size * dictionary_string.GetSize();
			}
		}

		if (heap_size == 0) {
			return false; // regular path is OK
		}
		break;
	}
	default:
		return false;
	}
	D_ASSERT(heap_size != 0);

	auto &segment = meta_data.segment;
	auto &append_state = meta_data.state;

	// allocate string heap for the compressed strings
	child_index = segment.AllocateStringHeap(heap_size, meta_data.chunk_data, append_state, child_index);
	if (!meta_data.GetVectorMetaData().child_index.IsValid()) {
		meta_data.GetVectorMetaData().child_index = meta_data.segment.AddChildIndex(child_index);
	}
	auto &child_segment = segment.GetVectorData(child_index);
	base_heap_ptr = segment.allocator->GetDataPointer(append_state.current_chunk_state, child_segment.block_id,
	                                                  child_segment.offset);

	auto &current_segment = segment.GetVectorData(current_index);
	const auto base_ptr = segment.allocator->GetDataPointer(append_state.current_chunk_state, current_segment.block_id,
	                                                        current_segment.offset);

	// initialize validity mask
	auto validity_data = ColumnDataCollectionSegment::GetValidityPointerForWriting(base_ptr, sizeof(string_t));
	ValidityMask target_validity(validity_data, STANDARD_VECTOR_SIZE);
	if (current_segment.count == 0) {
		// first time appending to this vector
		// all data here is still uninitialized
		// initialize the validity mask to set all to valid
		target_validity.SetAllValid(STANDARD_VECTOR_SIZE);
	}

	// now write the compressed data
	const auto target_entries = reinterpret_cast<string_t *>(base_ptr);
	if (source.GetVectorType() == VectorType::CONSTANT_VECTOR) {
		// copy over the constant string
		auto constant_string = ConstantVector::GetData<string_t>(source)[0];
		memcpy(base_heap_ptr, constant_string.GetData(), constant_string.GetSize());
		constant_string.SetPointer(char_ptr_cast(base_heap_ptr));

		// duplicate it
		for (idx_t i = 0; i < vector_remaining; i++) {
			const auto target_idx = current_segment.count + i;
			target_entries[target_idx] = constant_string;
		}
	} else {
		D_ASSERT(source.GetVectorType() == VectorType::DICTIONARY_VECTOR);
		const auto dictionary_size = DictionaryVector::DictionarySize(source);
		const auto &dictionary_vector = DictionaryVector::Child(source);
		const auto dictionary_strings = FlatVector::GetData<string_t>(dictionary_vector);
		const auto &dictionary_validity = FlatVector::Validity(dictionary_vector);

		// Copy over dictionary, computing offsets as we go
		idx_t current_string_offset = 0;
		idx_t string_offsets[STANDARD_VECTOR_SIZE];
		const auto dictionary_size_idx = dictionary_size.GetIndex();
		for (idx_t i = 0; i < dictionary_size_idx; i++) {
			const auto &dictionary_string = dictionary_strings[i];
			if (dictionary_validity.RowIsValid(i) && !dictionary_string.IsInlined()) {
				string_offsets[i] = current_string_offset;
				memcpy(base_heap_ptr + current_string_offset, dictionary_string.GetPointer(),
				       dictionary_string.GetSize());
				current_string_offset += dictionary_string.GetSize();
			}
		}

		// Now copy over the string vector, pointing to the new dictionary
		const auto source_entries = UnifiedVectorFormat::GetData<string_t>(source_data);
		for (idx_t i = 0; i < vector_remaining; i++) {
			const auto source_idx = UnsafeNumericCast<idx_t>((*source_data.sel)[offset + i]);
			const auto target_idx = current_segment.count + i;
			if (!source_data.validity.RowIsValid(source_idx)) {
				target_validity.SetInvalid(target_idx);
				continue;
			}
			const auto &source_entry = source_entries[source_idx];
			auto &target_entry = target_entries[target_idx];
			target_entry = source_entry;
			if (!source_entry.IsInlined()) {
				target_entry.SetPointer(char_ptr_cast(base_heap_ptr + string_offsets[source_idx]));
#ifdef D_ASSERT_IS_ENABLED
				if (source.GetType() == LogicalType::VARCHAR) {
					target_entry.Verify();
				}
#endif
			}
		}
	}

	append_count = vector_remaining;
	return true;
}

struct ColumnDataStringAppendInfo {
	idx_t append_count = 0;
	idx_t heap_size = 0;
	const char *contiguous_source_ptr = nullptr;
	bool source_bytes_are_contiguous = true;
};

static inline void AnalyzeColumnDataStringEntry(ColumnDataStringAppendInfo &result, const string_t &entry,
                                                const idx_t block_size, bool &appending,
                                                bool &has_contiguous_source_ptr, uintptr_t &expected_next_ptr) {
	const auto entry_size = entry.GetSize();
	const idx_t heap_size = entry_size * idx_t(entry_size > string_t::INLINE_LENGTH);
	const auto next_heap_size = result.heap_size + heap_size;
	const bool append_entry = appending & (next_heap_size <= block_size);
	const bool copy_bytes = append_entry & (heap_size != 0);
	const auto source_ptr = entry.GetData();
	const auto source_ptr_value = reinterpret_cast<uintptr_t>(source_ptr);
	const bool source_matches = !has_contiguous_source_ptr | (source_ptr_value == expected_next_ptr);

	result.append_count += idx_t(append_entry);
	result.heap_size += idx_t(append_entry) * heap_size;
	result.source_bytes_are_contiguous &= !copy_bytes | source_matches;
	result.contiguous_source_ptr =
	    (!has_contiguous_source_ptr & copy_bytes) ? source_ptr : result.contiguous_source_ptr;
	expected_next_ptr = copy_bytes ? source_ptr_value + heap_size : expected_next_ptr;
	has_contiguous_source_ptr |= copy_bytes;
	appending &= append_entry;
}

template <bool HAS_SELECTION, bool ALL_VALID>
static ColumnDataStringAppendInfo
AnalyzeColumnDataStringAppendInternal(const UnifiedVectorFormat &source_data, const idx_t offset,
                                      const idx_t vector_remaining, const idx_t block_size) {
	ColumnDataStringAppendInfo result;
	const auto source_entries = UnifiedVectorFormat::GetData<string_t>(source_data);
	bool appending = true;
	bool has_contiguous_source_ptr = false;
	uintptr_t expected_next_ptr = 0;

	for (idx_t i = 0; i < vector_remaining; i++) {
		const auto source_idx = HAS_SELECTION ? source_data.sel->get_index_unsafe(offset + i) : offset + i;
		if constexpr (!ALL_VALID) {
			if (!source_data.validity.RowIsValidUnsafe(source_idx)) {
				result.append_count += static_cast<idx_t>(appending);
				continue;
			}
		}
		AnalyzeColumnDataStringEntry(result, source_entries[source_idx], block_size, appending,
		                             has_contiguous_source_ptr, expected_next_ptr);
	}

	if (vector_remaining != 0 && result.append_count == 0) {
		// The string exceeds Storage::DEFAULT_BLOCK_SIZE, so we allocate one block at a time for long strings.
		auto source_idx = HAS_SELECTION ? source_data.sel->get_index_unsafe(offset) : offset;
		if constexpr (!ALL_VALID) {
			D_ASSERT(source_data.validity.RowIsValidUnsafe(source_idx));
		}
		D_ASSERT(!source_entries[source_idx].IsInlined());
		D_ASSERT(source_entries[source_idx].GetSize() > block_size);
		result.contiguous_source_ptr = source_entries[source_idx].GetData();
		result.heap_size += source_entries[source_idx].GetSize();
		result.append_count++;
	}

	return result;
}

static ColumnDataStringAppendInfo AnalyzeColumnDataStringAppend(const UnifiedVectorFormat &source_data,
                                                                const idx_t offset, const idx_t vector_remaining,
                                                                const idx_t block_size) {
	const auto has_selection = source_data.sel->IsSet();
	const auto all_valid = source_data.validity.CannotHaveNull();
	if (has_selection) {
		return all_valid ? AnalyzeColumnDataStringAppendInternal<true, true>(source_data, offset, vector_remaining,
		                                                                     block_size)
		                 : AnalyzeColumnDataStringAppendInternal<true, false>(source_data, offset, vector_remaining,
		                                                                      block_size);
	}
	return all_valid
	           ? AnalyzeColumnDataStringAppendInternal<false, true>(source_data, offset, vector_remaining, block_size)
	           : AnalyzeColumnDataStringAppendInternal<false, false>(source_data, offset, vector_remaining, block_size);
}

template <>
void ColumnDataCopy<string_t>(ColumnDataMetaData &meta_data, const UnifiedVectorFormat &source_data, Vector &source,
                              idx_t offset, idx_t copy_count) {
	const auto &allocator_type = meta_data.segment.allocator->GetType();
	if (allocator_type == ColumnDataAllocatorType::IN_MEMORY_ALLOCATOR ||
	    allocator_type == ColumnDataAllocatorType::HYBRID) {
		// strings cannot be spilled to disk - use StringHeap
		TemplatedColumnDataCopy<StringValueCopy>(meta_data, source_data, source, offset, copy_count);
		return;
	}
	D_ASSERT(allocator_type == ColumnDataAllocatorType::BUFFER_MANAGER_ALLOCATOR);

	auto &segment = meta_data.segment;
	auto &append_state = meta_data.state;

	VectorDataIndex child_index;
	if (meta_data.GetVectorMetaData().child_index.IsValid()) {
		// find the last child index
		child_index = segment.GetChildIndex(meta_data.GetVectorMetaData().child_index);
		auto next_child_index = segment.GetVectorData(child_index).next_data;
		while (next_child_index.IsValid()) {
			child_index = next_child_index;
			next_child_index = segment.GetVectorData(child_index).next_data;
		}
	}

	auto current_index = meta_data.vector_data_index;
	idx_t remaining = copy_count;
	auto block_size = meta_data.segment.allocator->GetBufferManager().GetBlockSize();
	while (remaining > 0) {
		EnsureVectorDataAllocated(meta_data, current_index, sizeof(string_t));
		// how many values fit in the current string vector
		const auto vector_remaining =
		    MinValue<idx_t>(STANDARD_VECTOR_SIZE - segment.GetVectorData(current_index).count, remaining);

		idx_t append_count = 0;
		idx_t heap_size = 0;
		data_ptr_t base_heap_ptr = nullptr;
		if (!ColumnDataCopyCompressedStrings(meta_data, current_index, child_index, source_data, source, offset,
		                                     vector_remaining, append_count, heap_size, base_heap_ptr)) {
			// 'append_count' is less if we cannot fit that amount of non-inlined strings on one buffer-managed block
			const auto append_info = AnalyzeColumnDataStringAppend(source_data, offset, vector_remaining, block_size);
			append_count = append_info.append_count;
			heap_size = append_info.heap_size;
			const auto source_entries = UnifiedVectorFormat::GetData<string_t>(source_data);

			// allocate string heap for the next 'append_count' strings
			if (heap_size != 0) {
				child_index = segment.AllocateStringHeap(heap_size, meta_data.chunk_data, append_state, child_index);
				if (!meta_data.GetVectorMetaData().child_index.IsValid()) {
					meta_data.GetVectorMetaData().child_index = meta_data.segment.AddChildIndex(child_index);
				}
				const auto &child_segment = segment.GetVectorData(child_index);
				base_heap_ptr = segment.allocator->GetDataPointer(append_state.current_chunk_state,
				                                                  child_segment.block_id, child_segment.offset);
				if (append_info.source_bytes_are_contiguous) {
					memcpy(base_heap_ptr, append_info.contiguous_source_ptr, heap_size);
				}
			}

			// We get a reference to the "current_segment" only after allocating the string heap above,
			// because this can resize the vector holding the segments, moving it somewhere else
			auto &current_segment = segment.GetVectorData(current_index);
			auto base_ptr = segment.allocator->GetDataPointer(append_state.current_chunk_state,
			                                                  current_segment.block_id, current_segment.offset);
			auto validity_data = ColumnDataCollectionSegment::GetValidityPointerForWriting(base_ptr, sizeof(string_t));
			ValidityMask target_validity(validity_data, STANDARD_VECTOR_SIZE);
			if (current_segment.count == 0) {
				// first time appending to this vector
				// all data here is still uninitialized
				// initialize the validity mask to set all to valid
				target_validity.SetAllValid(STANDARD_VECTOR_SIZE);
			}

			auto target_entries = reinterpret_cast<string_t *>(base_ptr);
			idx_t heap_offset = 0;
			for (idx_t i = 0; i < append_count; i++) {
				auto source_idx = source_data.sel->get_index(offset + i);
				auto target_idx = current_segment.count + i;
				if (!source_data.validity.RowIsValid(source_idx)) {
					target_validity.SetInvalid(target_idx);
					continue;
				}
				const auto &source_entry = source_entries[source_idx];
				auto &target_entry = target_entries[target_idx];
				if (source_entry.IsInlined()) {
					target_entry = source_entry;
				} else {
					D_ASSERT(base_heap_ptr != nullptr);
					auto target_ptr = base_heap_ptr + heap_offset;
					if (!append_info.source_bytes_are_contiguous) {
						memcpy(target_ptr, source_entry.GetData(), source_entry.GetSize());
					}
					target_entry = source_entry;
					target_entry.SetPointer(char_ptr_cast(target_ptr));
					heap_offset += source_entry.GetSize();
				}
			}
		}

		auto &current_segment = segment.GetVectorData(current_index);
		if (heap_size != 0) {
#ifdef D_ASSERT_IS_ENABLED
			const auto &child_segment = segment.GetVectorData(child_index);
			D_ASSERT(base_heap_ptr == segment.allocator->GetDataPointer(append_state.current_chunk_state,
			                                                            child_segment.block_id, child_segment.offset));
#endif
			current_segment.swizzle_data.emplace_back(child_index, base_heap_ptr, current_segment.count, append_count);
		}

		current_segment.count += append_count;
		offset += append_count;
		remaining -= append_count;

		if (remaining != 0 && vector_remaining - append_count == 0) {
			// need to append more, check if we need to allocate a new vector or not
			if (!current_segment.next_data.IsValid()) {
				segment.AllocateVector(source.GetType(), meta_data.chunk_data, append_state, current_index);
			}
			D_ASSERT(segment.GetVectorData(current_index).next_data.IsValid());
			current_index = segment.GetVectorData(current_index).next_data;
		}
	}
}

template <>
void ColumnDataCopy<list_entry_t>(ColumnDataMetaData &meta_data, const UnifiedVectorFormat &source_data, Vector &source,
                                  idx_t offset, idx_t copy_count) {
	auto &segment = meta_data.segment;

	auto &child_vector = ListVector::GetChildMutable(source);
	auto &child_type = child_vector.GetType();

	if (!meta_data.GetVectorMetaData().child_index.IsValid()) {
		auto child_index = segment.AllocateVector(child_type, meta_data.chunk_data, meta_data.state);
		meta_data.GetVectorMetaData().child_index = meta_data.segment.AddChildIndex(child_index);
	}

	auto &child_function = meta_data.copy_function.child_functions[0];
	auto child_index = segment.GetChildIndex(meta_data.GetVectorMetaData().child_index);

	// figure out the current list size by traversing the set of child entries
	idx_t current_list_size = 0;
	auto current_child_index = child_index;
	while (current_child_index.IsValid()) {
		auto &child_vdata = segment.GetVectorData(current_child_index);
		current_list_size += child_vdata.count;
		current_child_index = child_vdata.next_data;
	}

	// set the child vector
	UnifiedVectorFormat child_vector_data;
	ColumnDataMetaData child_meta_data(child_function, meta_data, child_index);
	auto info = ListVector::GetConsecutiveChildListInfo(source, offset, copy_count);

	if (info.needs_slicing) {
		SelectionVector sel(info.child_list_info.length);
		ListVector::GetConsecutiveChildSelVector(source, sel, offset, copy_count);

		auto sliced_child_vector = Vector(child_vector, sel, info.child_list_info.length);
		sliced_child_vector.Flatten();
		info.child_list_info.offset = 0;

		sliced_child_vector.ToUnifiedFormat(child_vector_data);
		child_function.function(child_meta_data, child_vector_data, sliced_child_vector, info.child_list_info.offset,
		                        info.child_list_info.length);

	} else {
		child_vector.ToUnifiedFormat(child_vector_data);
		child_function.function(child_meta_data, child_vector_data, child_vector, info.child_list_info.offset,
		                        info.child_list_info.length);
	}

	// now copy the list entries
	meta_data.child_list_size = current_list_size;
	if (info.is_constant) {
		TemplatedColumnDataCopy<ConstListValueCopy>(meta_data, source_data, source, offset, copy_count);
	} else {
		TemplatedColumnDataCopy<ListValueCopy>(meta_data, source_data, source, offset, copy_count);
	}
}

void ColumnDataCopyStruct(ColumnDataMetaData &meta_data, const UnifiedVectorFormat &source_data, Vector &source,
                          idx_t offset, idx_t copy_count) {
	auto &segment = meta_data.segment;

	// copy the NULL values for the main struct vector
	TemplatedColumnDataCopy<StructValueCopy>(meta_data, source_data, source, offset, copy_count);

	auto &child_types = StructType::GetChildTypes(source.GetType());
	// now copy all the child vectors
	D_ASSERT(meta_data.GetVectorMetaData().child_index.IsValid());
	auto &child_vectors = StructVector::GetEntries(source);
	for (idx_t child_idx = 0; child_idx < child_types.size(); child_idx++) {
		auto &child_function = meta_data.copy_function.child_functions[child_idx];
		auto child_index = segment.GetChildIndex(meta_data.GetVectorMetaData().child_index, child_idx);
		ColumnDataMetaData child_meta_data(child_function, meta_data, child_index);

		UnifiedVectorFormat child_data;
		child_vectors[child_idx].ToUnifiedFormat(child_data);

		child_function.function(child_meta_data, child_data, child_vectors[child_idx], offset, copy_count);
	}
}

void ColumnDataCopyArray(ColumnDataMetaData &meta_data, const UnifiedVectorFormat &source_data, Vector &source,
                         idx_t offset, idx_t copy_count) {
	auto &segment = meta_data.segment;

	// copy the NULL values for the main array vector (the same as for a struct vector)
	TemplatedColumnDataCopy<StructValueCopy>(meta_data, source_data, source, offset, copy_count);

	auto &child_vector = ArrayVector::GetChildMutable(source);
	auto &child_type = child_vector.GetType();
	auto array_size = ArrayType::GetSize(source.GetType());

	if (!meta_data.GetVectorMetaData().child_index.IsValid()) {
		auto child_index = segment.AllocateVector(child_type, meta_data.chunk_data, meta_data.state);
		meta_data.GetVectorMetaData().child_index = meta_data.segment.AddChildIndex(child_index);
	}

	auto &child_function = meta_data.copy_function.child_functions[0];
	auto child_index = segment.GetChildIndex(meta_data.GetVectorMetaData().child_index);

	auto current_child_index = child_index;
	while (current_child_index.IsValid()) {
		auto &child_vdata = segment.GetVectorData(current_child_index);
		current_child_index = child_vdata.next_data;
	}

	UnifiedVectorFormat child_vector_data;
	ColumnDataMetaData child_meta_data(child_function, meta_data, child_index);
	child_vector.ToUnifiedFormat(child_vector_data);

	// Broadcast and sync the validity of the array vector to the child vector
	// This requires creating a copy of the validity mask: we cannot modify the input validity
	child_vector_data.validity = ValidityMask(child_vector_data.validity, child_vector_data.validity.Capacity());
	if (source_data.validity.IsMaskSet()) {
		for (idx_t i = 0; i < copy_count; i++) {
			auto source_idx = source_data.sel->get_index(offset + i);
			if (!source_data.validity.RowIsValid(source_idx)) {
				for (idx_t j = 0; j < array_size; j++) {
					child_vector_data.validity.SetInvalid(source_idx * array_size + j);
				}
			}
		}
	}

	auto is_constant = source.GetVectorType() == VectorType::CONSTANT_VECTOR;
	// If the array is constant, we need to copy the child vector n times
	if (is_constant) {
		for (idx_t i = 0; i < copy_count; i++) {
			child_function.function(child_meta_data, child_vector_data, child_vector, 0, array_size);
		}
	} else {
		child_function.function(child_meta_data, child_vector_data, child_vector, offset * array_size,
		                        copy_count * array_size);
	}
}

ColumnDataCopyFunction ColumnDataCollection::GetCopyFunction(const LogicalType &type) {
	ColumnDataCopyFunction result;
	column_data_copy_function_t function;
	switch (type.InternalType()) {
	case PhysicalType::BOOL:
		function = ColumnDataCopy<bool>;
		break;
	case PhysicalType::INT8:
		function = ColumnDataCopy<int8_t>;
		break;
	case PhysicalType::INT16:
		function = ColumnDataCopy<int16_t>;
		break;
	case PhysicalType::INT32:
		function = ColumnDataCopy<int32_t>;
		break;
	case PhysicalType::INT64:
		function = ColumnDataCopy<int64_t>;
		break;
	case PhysicalType::INT128:
		function = ColumnDataCopy<hugeint_t>;
		break;
	case PhysicalType::UINT8:
		function = ColumnDataCopy<uint8_t>;
		break;
	case PhysicalType::UINT16:
		function = ColumnDataCopy<uint16_t>;
		break;
	case PhysicalType::UINT32:
		function = ColumnDataCopy<uint32_t>;
		break;
	case PhysicalType::UINT64:
		function = ColumnDataCopy<uint64_t>;
		break;
	case PhysicalType::UINT128:
		function = ColumnDataCopy<uhugeint_t>;
		break;
	case PhysicalType::FLOAT:
		function = ColumnDataCopy<float>;
		break;
	case PhysicalType::DOUBLE:
		function = ColumnDataCopy<double>;
		break;
	case PhysicalType::INTERVAL:
		function = ColumnDataCopy<interval_t>;
		break;
	case PhysicalType::VARCHAR:
		function = ColumnDataCopy<string_t>;
		break;
	case PhysicalType::STRUCT: {
		function = ColumnDataCopyStruct;
		auto &child_types = StructType::GetChildTypes(type);
		for (auto &kv : child_types) {
			result.child_functions.push_back(GetCopyFunction(kv.second));
		}
		break;
	}
	case PhysicalType::LIST: {
		function = ColumnDataCopy<list_entry_t>;
		auto child_function = GetCopyFunction(ListType::GetChildType(type));
		result.child_functions.push_back(child_function);
		break;
	}
	case PhysicalType::ARRAY: {
		function = ColumnDataCopyArray;
		auto child_function = GetCopyFunction(ArrayType::GetChildType(type));
		result.child_functions.push_back(child_function);
		break;
	}
	default:
		throw InternalException("Unsupported type %s for ColumnDataCollection::GetCopyFunction",
		                        EnumUtil::ToString(type.InternalType()));
	}
	result.function = function;
	return result;
}

//===--------------------------------------------------------------------===//
// Compact representations
//===--------------------------------------------------------------------===//
static bool UsesStringHeap(ColumnDataAllocatorType allocator_type) {
	return allocator_type == ColumnDataAllocatorType::IN_MEMORY_ALLOCATOR ||
	       allocator_type == ColumnDataAllocatorType::HYBRID;
}

static void AppendConstantEntry(ColumnDataMetaData &meta_data, const UnifiedVectorFormat &source_data, Vector &source,
                                idx_t offset, idx_t copy_count) {
	auto &segment = meta_data.segment;
	auto internal_type = source.GetType().InternalType();
	auto source_idx = source_data.sel->get_index(offset);

	ColumnDataConstantValue constant;
	constant.is_valid = source_data.validity.RowIsValid(source_idx);
	if (constant.is_valid) {
		auto type_size = GetTypeIdSize(internal_type);
		D_ASSERT(type_size <= sizeof(constant.data));
		if (internal_type == PhysicalType::VARCHAR) {
			auto value = UnifiedVectorFormat::GetData<string_t>(source_data)[source_idx];
			if (!value.IsInlined()) {
				if (UsesStringHeap(segment.allocator->GetType())) {
					value = segment.heap->AddBlob(value);
				} else {
					// copy the string bytes once into a heap entry
					auto heap_index =
					    segment.AllocateStringHeap(value.GetSize(), meta_data.chunk_data, meta_data.state);
					auto &heap_vdata = segment.GetVectorData(heap_index);
					auto heap_ptr = segment.allocator->GetDataPointer(meta_data.state.current_chunk_state,
					                                                  heap_vdata.block_id, heap_vdata.offset);
					memcpy(heap_ptr, value.GetData(), value.GetSize());
					constant.heap_index = heap_index;
				}
			}
			Store<string_t>(value, constant.data);
		} else {
			memcpy(constant.data, source_data.data + source_idx * type_size, type_size);
		}
	}
	auto &vdata = meta_data.GetVectorMetaData();
	vdata.constant = constant;
	vdata.representation = ColumnDataRepresentation::CONSTANT;
	vdata.count = UnsafeNumericCast<uint16_t>(copy_count);
	// account the metadata bytes that hold the value
	segment.constant_metadata_size += sizeof(ColumnDataConstantValue);
	segment.total_allocated += sizeof(ColumnDataConstantValue);
}

static bool TryExtendConstantEntry(ColumnDataMetaData &meta_data, const UnifiedVectorFormat &source_data,
                                   Vector &source, idx_t offset, idx_t copy_count) {
	if (source.GetVectorType() != VectorType::CONSTANT_VECTOR) {
		return false;
	}
	auto &segment = meta_data.segment;
	auto &vdata = meta_data.GetVectorMetaData();
	D_ASSERT(vdata.count + copy_count <= STANDARD_VECTOR_SIZE);
	auto source_idx = source_data.sel->get_index(offset);
	if (vdata.constant.is_valid != source_data.validity.RowIsValid(source_idx)) {
		return false;
	}
	if (vdata.constant.is_valid) {
		auto internal_type = source.GetType().InternalType();
		if (internal_type == PhysicalType::VARCHAR) {
			auto stored = segment.GetConstantString(meta_data.state.current_chunk_state, vdata);
			auto value = UnifiedVectorFormat::GetData<string_t>(source_data)[source_idx];
			if (stored != value) {
				return false;
			}
		} else {
			auto type_size = GetTypeIdSize(internal_type);
			if (memcmp(vdata.constant.data, source_data.data + source_idx * type_size, type_size) != 0) {
				return false;
			}
		}
	}
	vdata.count += UnsafeNumericCast<uint16_t>(copy_count);
	return true;
}

static void MaterializeConstantEntry(ColumnDataMetaData &meta_data, const LogicalType &type) {
	auto &segment = meta_data.segment;
	auto &state = meta_data.state;
	auto internal_type = type.InternalType();
	auto type_size = GetTypeIdSize(internal_type);
	EnsureVectorDataAllocated(meta_data, meta_data.vector_data_index, type_size);

	auto &vdata = meta_data.GetVectorMetaData();
	auto base_ptr = segment.allocator->GetDataPointer(state.current_chunk_state, vdata.block_id, vdata.offset);
	auto validity_data = ColumnDataCollectionSegment::GetValidityPointerForWriting(base_ptr, type_size);
	ValidityMask validity(validity_data, STANDARD_VECTOR_SIZE);
	validity.SetAllValid(STANDARD_VECTOR_SIZE);

	const idx_t count = vdata.count;
	if (!vdata.constant.is_valid) {
		for (idx_t i = 0; i < count; i++) {
			validity.SetInvalid(i);
		}
	} else if (internal_type == PhysicalType::VARCHAR) {
		auto value = segment.GetConstantString(state.current_chunk_state, vdata);
		auto entries = reinterpret_cast<string_t *>(base_ptr);
		for (idx_t i = 0; i < count; i++) {
			entries[i] = value;
		}
		if (vdata.constant.heap_index.IsValid()) {
			// the entries point into the heap entry: track them so relocated blocks can be repaired
			auto &heap_vdata = segment.GetVectorData(vdata.constant.heap_index);
			auto heap_ptr =
			    segment.allocator->GetDataPointer(state.current_chunk_state, heap_vdata.block_id, heap_vdata.offset);
			vdata.swizzle_data.emplace_back(vdata.constant.heap_index, heap_ptr, 0, UnsafeNumericCast<uint16_t>(count));
		}
	} else {
		for (idx_t i = 0; i < count; i++) {
			memcpy(base_ptr + i * type_size, vdata.constant.data, type_size);
		}
	}
	vdata.representation = ColumnDataRepresentation::FLAT;
	vdata.constant = ColumnDataConstantValue();
	// the constant charge stays counted, like the storage of any superseded entry: batching
	// consumers require that collection sizes never shrink while appending
}

static idx_t DictionaryCodesAllocationSize() {
	return AlignValue(STANDARD_VECTOR_SIZE * sizeof(sel_t));
}

static idx_t GetOrCreateSegmentDictionary(ColumnDataMetaData &meta_data, Vector &dict_child, idx_t dict_size,
                                          const string &dict_id) {
	auto &segment = meta_data.segment;
	auto entry = segment.dictionary_map.find(dict_id);
	if (entry != segment.dictionary_map.end()) {
		auto slot = entry->second;
		auto &dict_data = segment.dictionaries[slot];
		if (dict_data.type != dict_child.GetType() || segment.GetVectorData(dict_data.data_index).count != dict_size) {
			return DConstants::INVALID_INDEX;
		}
		return slot;
	}
	auto &type = dict_child.GetType();
	auto allocator_type = segment.allocator->GetType();
	if (type.InternalType() == PhysicalType::VARCHAR && !UsesStringHeap(allocator_type)) {
		// the dictionary values must stay a single entry with a single string heap
		idx_t heap_size = 0;
		auto strings = FlatVector::GetData<string_t>(dict_child);
		auto &validity = FlatVector::Validity(dict_child);
		for (idx_t i = 0; i < dict_size; i++) {
			if (validity.RowIsValid(i) && !strings[i].IsInlined()) {
				heap_size += strings[i].GetSize();
			}
		}
		if (heap_size > segment.allocator->GetBufferManager().GetBlockSize()) {
			return DConstants::INVALID_INDEX;
		}
	}
	// materialize the dictionary values once for the whole segment
	auto dict_index = segment.AllocateVector(type, meta_data.chunk_data, meta_data.state);
	ColumnDataMetaData dict_meta(meta_data.copy_function, meta_data, dict_index);
	UnifiedVectorFormat dict_format;
	dict_child.ToUnifiedFormat(dict_format);
	meta_data.copy_function.function(dict_meta, dict_format, dict_child, 0, dict_size);

	auto &dict_vdata = segment.GetVectorData(dict_index);
	D_ASSERT(!dict_vdata.next_data.IsValid());
	D_ASSERT(dict_vdata.swizzle_data.size() <= 1);
	SegmentDictionaryData dict_data;
	dict_data.id = dict_id;
	dict_data.type = type;
	dict_data.data_index = dict_index;
	if (allocator_type == ColumnDataAllocatorType::BUFFER_MANAGER_ALLOCATOR ||
	    allocator_type == ColumnDataAllocatorType::HYBRID) {
		dict_data.block_ids.insert(dict_vdata.block_id);
		for (auto &swizzle_segment : dict_vdata.swizzle_data) {
			dict_data.block_ids.insert(segment.GetVectorData(swizzle_segment.child_index).block_id);
		}
	}
	auto slot = segment.dictionaries.size();
	segment.dictionaries.push_back(std::move(dict_data));
	segment.dictionary_map[dict_id] = slot;
	return slot;
}

static void WriteDictionaryCodes(ColumnDataMetaData &meta_data, const UnifiedVectorFormat &source_data, idx_t offset,
                                 idx_t copy_count, idx_t target_offset) {
	auto &segment = meta_data.segment;
	auto &vdata = meta_data.GetVectorMetaData();
	auto base_ptr =
	    segment.allocator->GetDataPointer(meta_data.state.current_chunk_state, vdata.block_id, vdata.offset);
	auto codes = reinterpret_cast<sel_t *>(base_ptr);
	for (idx_t i = 0; i < copy_count; i++) {
		codes[target_offset + i] = UnsafeNumericCast<sel_t>(source_data.sel->get_index(offset + i));
	}
}

static bool TryAppendDictionaryEntry(ColumnDataMetaData &meta_data, const UnifiedVectorFormat &source_data,
                                     Vector &source, idx_t offset, idx_t copy_count) {
	auto internal_type = source.GetType().InternalType();
	if (GetTypeIdSize(internal_type) < sizeof(sel_t)) {
		// the codes would be larger than the values
		return false;
	}
	auto dict_size = DictionaryVector::DictionarySize(source);
	if (!dict_size.IsValid() || dict_size.GetIndex() > STANDARD_VECTOR_SIZE) {
		return false;
	}
	auto &dict_id = DictionaryVector::DictionaryId(source);
	if (dict_id.empty()) {
		// an ad hoc selection, not a re-occurring dictionary
		return false;
	}
	auto &dict_child = DictionaryVector::Child(source);
	if (dict_child.GetVectorType() != VectorType::FLAT_VECTOR) {
		return false;
	}
	auto &segment = meta_data.segment;
	if (segment.dictionary_map.find(dict_id) == segment.dictionary_map.end() && copy_count * 2 < dict_size.GetIndex()) {
		// copying a fresh dictionary only pays off when the append covers enough of it - the factor
		// matches the compressed string fallback gate; reusing an existing dictionary is always free
		return false;
	}
	auto slot = GetOrCreateSegmentDictionary(meta_data, dict_child, dict_size.GetIndex(), dict_id);
	if (slot == DConstants::INVALID_INDEX) {
		return false;
	}
	D_ASSERT(!meta_data.GetVectorMetaData().is_allocated);
	segment.AllocateVectorStorage(meta_data.vector_data_index, DictionaryCodesAllocationSize(), meta_data.chunk_data,
	                              &meta_data.state.current_chunk_state);
	// the chunk must reference and pin the dictionary blocks of earlier chunks
	for (auto &block_id : segment.dictionaries[slot].block_ids) {
		meta_data.chunk_data.block_ids.insert(block_id);
	}
	segment.allocator->InitializeChunkState(meta_data.state.current_chunk_state, meta_data.chunk_data);

	auto &vdata = meta_data.GetVectorMetaData();
	vdata.representation = ColumnDataRepresentation::DICTIONARY;
	vdata.dictionary_slot = slot;
	WriteDictionaryCodes(meta_data, source_data, offset, copy_count, 0);
	meta_data.GetVectorMetaData().count = UnsafeNumericCast<uint16_t>(copy_count);
	return true;
}

static bool TryExtendDictionaryEntry(ColumnDataMetaData &meta_data, const UnifiedVectorFormat &source_data,
                                     Vector &source, idx_t offset, idx_t copy_count) {
	if (source.GetVectorType() != VectorType::DICTIONARY_VECTOR) {
		return false;
	}
	auto &segment = meta_data.segment;
	auto &vdata = meta_data.GetVectorMetaData();
	auto &dict_data = segment.dictionaries[vdata.dictionary_slot];
	auto dict_size = DictionaryVector::DictionarySize(source);
	if (!dict_size.IsValid() || DictionaryVector::DictionaryId(source) != dict_data.id) {
		return false;
	}
	if (segment.GetVectorData(dict_data.data_index).count != dict_size.GetIndex()) {
		return false;
	}
	D_ASSERT(vdata.count + copy_count <= STANDARD_VECTOR_SIZE);
	WriteDictionaryCodes(meta_data, source_data, offset, copy_count, vdata.count);
	vdata.count += UnsafeNumericCast<uint16_t>(copy_count);
	return true;
}

static void MaterializeDictionaryEntry(ColumnDataMetaData &meta_data, const LogicalType &type) {
	auto &segment = meta_data.segment;
	auto &state = meta_data.state;
	auto internal_type = type.InternalType();
	auto type_size = GetTypeIdSize(internal_type);

	// copy out the codes before replacing the entry storage
	auto &vdata_before = meta_data.GetVectorMetaData();
	const idx_t count = vdata_before.count;
	const auto slot = vdata_before.dictionary_slot;
	sel_t codes[STANDARD_VECTOR_SIZE];
	auto codes_ptr =
	    segment.allocator->GetDataPointer(state.current_chunk_state, vdata_before.block_id, vdata_before.offset);
	memcpy(codes, codes_ptr, count * sizeof(sel_t));

	// read the dictionary with current pointers and decode into freshly allocated flat storage
	auto dict = segment.ReadSegmentDictionary(state.current_chunk_state, slot);
	auto dict_values = FlatVector::GetData(dict);
	auto &dict_validity = FlatVector::Validity(dict);
	segment.AllocateVectorStorage(meta_data.vector_data_index,
	                              ColumnDataCollectionSegment::GetDataSize(type_size) +
	                                  ValidityMask::STANDARD_MASK_SIZE,
	                              meta_data.chunk_data, &state.current_chunk_state);
	auto &vdata = meta_data.GetVectorMetaData();
	auto base_ptr = segment.allocator->GetDataPointer(state.current_chunk_state, vdata.block_id, vdata.offset);
	auto validity_data = ColumnDataCollectionSegment::GetValidityPointerForWriting(base_ptr, type_size);
	ValidityMask validity(validity_data, STANDARD_VECTOR_SIZE);
	validity.SetAllValid(STANDARD_VECTOR_SIZE);

	for (idx_t i = 0; i < count; i++) {
		auto code = codes[i];
		if (!dict_validity.RowIsValid(code)) {
			validity.SetInvalid(i);
			continue;
		}
		memcpy(base_ptr + i * type_size, dict_values + code * type_size, type_size);
	}
	if (internal_type == PhysicalType::VARCHAR) {
		auto &dict_vdata = segment.GetVectorData(segment.dictionaries[slot].data_index);
		if (!dict_vdata.swizzle_data.empty()) {
			// the decoded entries point into the dictionary heap entry
			auto &dict_swizzle = dict_vdata.swizzle_data[0];
			auto &heap_vdata = segment.GetVectorData(dict_swizzle.child_index);
			auto heap_ptr =
			    segment.allocator->GetDataPointer(state.current_chunk_state, heap_vdata.block_id, heap_vdata.offset);
			vdata.swizzle_data.emplace_back(dict_swizzle.child_index, heap_ptr, 0, UnsafeNumericCast<uint16_t>(count));
		}
	}
	vdata.representation = ColumnDataRepresentation::FLAT;
	vdata.dictionary_slot = DConstants::INVALID_INDEX;
}

static bool TryAppendCompactVector(ColumnDataMetaData &meta_data, const UnifiedVectorFormat &source_data,
                                   Vector &source, idx_t offset, idx_t copy_count) {
	if (meta_data.segment.allocator->IsManagedResultSet()) {
		// client result sets keep flat data, external consumers may rely on it
		return false;
	}
	auto &vdata = meta_data.GetVectorMetaData();
	switch (vdata.representation) {
	case ColumnDataRepresentation::CONSTANT:
		if (TryExtendConstantEntry(meta_data, source_data, source, offset, copy_count)) {
			return true;
		}
		MaterializeConstantEntry(meta_data, source.GetType());
		return false;
	case ColumnDataRepresentation::DICTIONARY:
		if (TryExtendDictionaryEntry(meta_data, source_data, source, offset, copy_count)) {
			return true;
		}
		MaterializeDictionaryEntry(meta_data, source.GetType());
		return false;
	default:
		break;
	}
	if (vdata.count != 0) {
		return false;
	}
	if (source.GetVectorType() == VectorType::CONSTANT_VECTOR) {
		AppendConstantEntry(meta_data, source_data, source, offset, copy_count);
		return true;
	}
	if (source.GetVectorType() == VectorType::DICTIONARY_VECTOR) {
		return TryAppendDictionaryEntry(meta_data, source_data, source, offset, copy_count);
	}
	return false;
}

static bool IsComplexType(const LogicalType &type) {
	switch (type.InternalType()) {
	case PhysicalType::STRUCT:
	case PhysicalType::LIST:
	case PhysicalType::ARRAY:
		return true;
	default:
		return false;
	};
}

void ColumnDataCollection::Append(ColumnDataAppendState &state, DataChunk &input) {
	D_ASSERT(!finished_append);
	{
		auto input_types = input.GetTypes();
		D_ASSERT(types == input_types);
	}

	auto &segment = *segments.back();
	for (idx_t vector_idx = 0; vector_idx < types.size(); vector_idx++) {
		if (IsComplexType(input.data[vector_idx].GetType())) {
			input.data[vector_idx].Flatten();
		}
		input.data[vector_idx].ToUnifiedFormat(state.vector_data[vector_idx]);
	}

	idx_t remaining = input.size();
	while (remaining > 0) {
		auto &chunk_data = segment.chunk_data.back();
		idx_t append_amount = MinValue<idx_t>(remaining, STANDARD_VECTOR_SIZE - chunk_data.count);
		if (append_amount > 0) {
			idx_t offset = input.size() - remaining;
			for (idx_t vector_idx = 0; vector_idx < types.size(); vector_idx++) {
				ColumnDataMetaData meta_data(copy_functions[vector_idx], segment, state, chunk_data,
				                             chunk_data.vector_data[vector_idx]);
				const auto allocated_before = segment.total_allocated;
				if (!TryAppendCompactVector(meta_data, state.vector_data[vector_idx], input.data[vector_idx], offset,
				                            append_amount)) {
					copy_functions[vector_idx].function(meta_data, state.vector_data[vector_idx],
					                                    input.data[vector_idx], offset, append_amount);
				}
				chunk_data.allocation_sizes[vector_idx] += segment.total_allocated - allocated_before;
			}
			chunk_data.count += append_amount;
		}
		remaining -= append_amount;
		if (remaining > 0) {
			// more to do
			// allocate a new chunk
			segment.AllocateNewChunk();
			segment.InitializeChunkState(segment.chunk_data.size() - 1, state.current_chunk_state);
		}
	}
	segment.count += input.size();
	count += input.size();
}

void ColumnDataCollection::Append(DataChunk &input) {
	ColumnDataAppendState state;
	InitializeAppend(state);
	Append(state, input);
}

void ColumnDataCollection::Append(const ColumnDataCollection &other) {
	ColumnDataAppendState state;
	InitializeAppend(state);
	for (auto &chunk : other.Chunks()) {
		Append(state, chunk);
	}
}

//===--------------------------------------------------------------------===//
// Scan
//===--------------------------------------------------------------------===//
void ColumnDataCollection::InitializeScan(ColumnDataScanState &state, ColumnDataScanProperties properties) const {
	vector<column_t> column_ids;
	column_ids.reserve(types.size());
	for (idx_t i = 0; i < types.size(); i++) {
		column_ids.push_back(i);
	}
	InitializeScan(state, std::move(column_ids), properties);
}

void ColumnDataCollection::InitializeScan(ColumnDataScanState &state, vector<column_t> column_ids,
                                          ColumnDataScanProperties properties) const {
	state.db = allocator->GetDatabase();
	state.chunk_index = 0;
	state.segment_index = 0;
	state.current_row_index = 0;
	state.next_row_index = 0;
	state.current_chunk_state.handles.clear();
	state.properties = properties;
	state.column_ids = std::move(column_ids);
}

void ColumnDataCollection::InitializeScan(ColumnDataParallelScanState &state,
                                          ColumnDataScanProperties properties) const {
	InitializeScan(state.scan_state, properties);
}

void ColumnDataCollection::InitializeScan(ColumnDataParallelScanState &state, vector<column_t> column_ids,
                                          ColumnDataScanProperties properties) const {
	InitializeScan(state.scan_state, std::move(column_ids), properties);
}

bool ColumnDataCollection::Scan(ColumnDataParallelScanState &state, ColumnDataLocalScanState &lstate,
                                DataChunk &result) const {
	D_ASSERT(result.GetTypes() == types);
	result.Reset();

	idx_t chunk_index;
	idx_t segment_index;
	idx_t row_index;
	{
		lock_guard<mutex> l(state.lock);
		if (!NextScanIndex(state.scan_state, chunk_index, segment_index, row_index)) {
			return false;
		}
	}
	ScanAtIndex(state, lstate, result, chunk_index, segment_index, row_index);
	return true;
}

void ColumnDataCollection::InitializeScanChunk(DataChunk &chunk) const {
	InitializeScanChunk(allocator->GetAllocator(), chunk);
}

void ColumnDataCollection::InitializeScanChunk(Allocator &allocator, DataChunk &chunk) const {
	chunk.Initialize(allocator, types);
}

void ColumnDataCollection::InitializeScanChunk(ColumnDataScanState &state, DataChunk &chunk) const {
	vector<LogicalType> chunk_types;
	chunk_types.reserve(state.column_ids.size());
	for (idx_t i = 0; i < state.column_ids.size(); i++) {
		auto column_idx = state.column_ids[i];
		D_ASSERT(column_idx < types.size());
		chunk_types.push_back(types[column_idx]);
	}
	chunk.Initialize(allocator->GetAllocator(), chunk_types);
}

bool ColumnDataCollection::NextScanIndex(ColumnDataScanState &state, idx_t &chunk_index, idx_t &segment_index,
                                         idx_t &row_index) const {
	row_index = state.current_row_index = state.next_row_index;
	// check if we still have collections to scan
	if (state.segment_index >= segments.size()) {
		// no more data left in the scan
		return false;
	}
	// check within the current collection if we still have chunks to scan
	while (state.chunk_index >= segments[state.segment_index]->chunk_data.size()) {
		// exhausted all chunks for this internal data structure: move to the next one
		state.chunk_index = 0;
		state.segment_index++;
		state.current_chunk_state.handles.clear();
		if (state.segment_index >= segments.size()) {
			return false;
		}
	}
	state.next_row_index += segments[state.segment_index]->chunk_data[state.chunk_index].count;
	segment_index = state.segment_index;
	chunk_index = state.chunk_index++;
	return true;
}

bool ColumnDataCollection::PrevScanIndex(ColumnDataScanState &state, idx_t &chunk_index, idx_t &segment_index,
                                         idx_t &row_index) const {
	// check within the current segment if we still have chunks to scan
	// Note that state.chunk_index is 1-indexed, with 0 as undefined.
	while (state.chunk_index <= 1) {
		if (!state.segment_index) {
			return false;
		}

		--state.segment_index;
		state.chunk_index = segments[state.segment_index]->chunk_data.size() + 1;
		state.current_chunk_state.handles.clear();
	}

	--state.chunk_index;
	segment_index = state.segment_index;
	chunk_index = state.chunk_index - 1;
	state.next_row_index = state.current_row_index;
	state.current_row_index -= segments[state.segment_index]->chunk_data[chunk_index].count;
	row_index = state.current_row_index;
	return true;
}

void ColumnDataCollection::ScanAtIndex(ColumnDataParallelScanState &state, ColumnDataLocalScanState &lstate,
                                       DataChunk &result, idx_t chunk_index, idx_t segment_index,
                                       idx_t row_index) const {
	if (segment_index != lstate.current_segment_index) {
		lstate.current_chunk_state.handles.clear();
		lstate.current_segment_index = segment_index;
	}
	auto &segment = *segments[segment_index];
	lstate.current_chunk_state.properties = state.scan_state.properties;
	segment.ReadChunk(chunk_index, lstate.current_chunk_state, result, state.scan_state.column_ids);
	lstate.current_row_index = row_index;
	result.Verify(state.scan_state.db);
}

bool ColumnDataCollection::Scan(ColumnDataScanState &state, DataChunk &result) const {
	for (idx_t i = 0; i < state.column_ids.size(); i++) {
		D_ASSERT(result.GetTypes()[i] == types[state.column_ids[i]]);
	}

	result.Reset();

	idx_t chunk_index;
	idx_t segment_index;
	idx_t row_index;
	if (!NextScanIndex(state, chunk_index, segment_index, row_index)) {
		return false;
	}

	// found a chunk to scan -> scan it
	auto &segment = *segments[segment_index];
	state.current_chunk_state.properties = state.properties;
	segment.ReadChunk(chunk_index, state.current_chunk_state, result, state.column_ids);
	result.Verify(state.db);
	return true;
}

bool ColumnDataCollection::Seek(idx_t seek_idx, ColumnDataScanState &state, DataChunk &result) const {
	//	Idempotency: Don't change anything if the row is already in range
	if (state.current_row_index <= seek_idx && seek_idx < state.next_row_index) {
		return true;
	}

	result.Reset();

	//	Linear scan for now. We could use a current_row_index => chunk map at some point
	//	but most use cases should be pretty local
	idx_t chunk_index;
	idx_t segment_index;
	idx_t row_index;
	while (seek_idx < state.current_row_index) {
		if (!PrevScanIndex(state, chunk_index, segment_index, row_index)) {
			return false;
		}
	}
	while (state.next_row_index <= seek_idx) {
		if (!NextScanIndex(state, chunk_index, segment_index, row_index)) {
			return false;
		}
	}

	// found a chunk to scan -> scan it
	auto &segment = *segments[segment_index];
	state.current_chunk_state.properties = state.properties;
	segment.ReadChunk(chunk_index, state.current_chunk_state, result, state.column_ids);
	result.Verify(state.db);
	return true;
}

void ColumnDataCollection::SeekScan(ColumnDataScanState &state, const ColumnDataChunkPosition &position) const {
	D_ASSERT(position.segment_index < segments.size());
	D_ASSERT(position.chunk_index < segments[position.segment_index]->ChunkCount());
	state.segment_index = position.segment_index;
	state.chunk_index = position.chunk_index;
	state.current_row_index = position.row_offset;
	state.next_row_index = position.row_offset;
	state.current_chunk_state.handles.clear();
}

ColumnDataChunkCursor::ColumnDataChunkCursor(const ColumnDataCollection &collection_p) : collection(collection_p) {
}

bool ColumnDataChunkCursor::Next() {
	auto &segments = collection.GetSegments();
	if (!started) {
		started = true;
	} else if (position.segment_index < segments.size()) {
		position.row_offset += Count();
		position.chunk_index++;
	}
	while (position.segment_index < segments.size() &&
	       position.chunk_index >= segments[position.segment_index]->ChunkCount()) {
		position.segment_index++;
		position.chunk_index = 0;
	}
	return position.segment_index < segments.size();
}

idx_t ColumnDataChunkCursor::Count() const {
	return collection.GetSegments()[position.segment_index]->chunk_data[position.chunk_index].count;
}

idx_t ColumnDataChunkCursor::AllocationSize() const {
	return collection.GetSegments()[position.segment_index]->GetChunkAllocationSize(position.chunk_index);
}

idx_t ColumnDataChunkCursor::AllocationSize(const vector<column_t> &column_ids) const {
	return collection.GetSegments()[position.segment_index]->GetChunkAllocationSize(position.chunk_index, column_ids);
}

ColumnDataRowCollection ColumnDataCollection::GetRows() const {
	return ColumnDataRowCollection(*this);
}

Value ColumnDataCollection::GetValue(idx_t column, idx_t index) const {
	return GetRows().GetValue(column, index);
}

//===--------------------------------------------------------------------===//
// Combine
//===--------------------------------------------------------------------===//
void ColumnDataCollection::Combine(ColumnDataCollection &other) {
	if (other.count == 0) {
		return;
	}
	if (types != other.types) {
		throw InternalException("Attempting to combine ColumnDataCollections with mismatching types");
	}
	this->count += other.count;
	this->segments.reserve(segments.size() + other.segments.size());
	for (auto &other_seg : other.segments) {
		segments.push_back(std::move(other_seg));
	}
	other.Reset();
	Verify();
}

void ColumnDataCollection::Swap(ColumnDataCollection &other) {
	if (types != other.types) {
		throw InternalException("Attempting to swap ColumnDataCollections with mismatching types");
	}
	std::swap(allocator, other.allocator);
	std::swap(types, other.types);
	std::swap(count, other.count);
	std::swap(segments, other.segments);
	std::swap(copy_functions, other.copy_functions);
	std::swap(finished_append, other.finished_append);
	std::swap(partition_index, other.partition_index);
}

//===--------------------------------------------------------------------===//
// Fetch
//===--------------------------------------------------------------------===//
idx_t ColumnDataCollection::ChunkCount() const {
	idx_t chunk_count = 0;
	for (auto &segment : segments) {
		chunk_count += segment->ChunkCount();
	}
	return chunk_count;
}

void ColumnDataCollection::FetchChunk(idx_t chunk_idx, DataChunk &result) const {
	D_ASSERT(result.GetTypes() == types);
	D_ASSERT(chunk_idx < ChunkCount());
	for (auto &segment : segments) {
		if (chunk_idx >= segment->ChunkCount()) {
			chunk_idx -= segment->ChunkCount();
		} else {
			segment->FetchChunk(chunk_idx, result);
			return;
		}
	}
	throw InternalException("Failed to find chunk in ColumnDataCollection");
}

//===--------------------------------------------------------------------===//
// Helpers
//===--------------------------------------------------------------------===//
void ColumnDataCollection::Verify() {
#ifdef DEBUG
	// verify counts
	idx_t total_segment_count = 0;
	for (auto &segment : segments) {
		segment->Verify();
		total_segment_count += segment->count;
	}
	D_ASSERT(total_segment_count == this->count);
#endif
}

// LCOV_EXCL_START
string ColumnDataCollection::ToString() const {
	DataChunk chunk;
	InitializeScanChunk(chunk);

	ColumnDataScanState scan_state;
	InitializeScan(scan_state);

	string result = StringUtil::Format("ColumnDataCollection - [%llu Chunks, %llu Rows]\n", ChunkCount(), Count());
	idx_t chunk_idx = 0;
	idx_t row_count = 0;
	while (Scan(scan_state, chunk)) {
		result +=
		    StringUtil::Format("Chunk %llu - [Rows %llu - %llu]\n", chunk_idx, row_count, row_count + chunk.size()) +
		    chunk.ToString();
		chunk_idx++;
		row_count += chunk.size();
	}

	return result;
}
// LCOV_EXCL_STOP

void ColumnDataCollection::Print() const {
	Printer::Print(ToString());
}

void ColumnDataCollection::Reset() {
	count = 0;
	segments.clear();

	// Refreshes the ColumnDataAllocator to prevent holding on to allocated data unnecessarily
	allocator = make_shared_ptr<ColumnDataAllocator>(*allocator);
}

void ColumnDataCollection::ResetForReuse() {
	count = 0;
	finished_append = false;
	if (segments.empty()) {
		allocator->Reset();
		return;
	}

	// Combined segments can retain an allocator distinct from the collection allocator.
	auto segment_allocator = segments.front()->allocator;
	for (auto &segment : segments) {
		segment->Reset();
	}

	if (segments.size() > 1) {
		segments.resize(1);
	}
	if (segment_allocator.get() != allocator.get()) {
		segment_allocator->ResetPreserveLastBlock();
	}
	allocator->ResetPreserveLastBlock();
}

struct ValueResultEquals {
	bool operator()(const Value &a, const Value &b) const {
		return Value::DefaultValuesAreEqual(a, b);
	}
};

bool ColumnDataCollection::ResultEquals(const ColumnDataCollection &left, const ColumnDataCollection &right,
                                        string &error_message, bool ordered) {
	if (left.ColumnCount() != right.ColumnCount()) {
		error_message = StringUtil::Format("Column count mismatch (%d vs %d)", left.Count(), right.Count());
		return false;
	}
	if (left.Count() != right.Count()) {
		error_message = StringUtil::Format("Row count mismatch (%d vs %d)", left.Count(), right.Count());
		return false;
	}
	auto left_rows = left.GetRows();
	auto right_rows = right.GetRows();
	for (idx_t r = 0; r < left.Count(); r++) {
		for (idx_t c = 0; c < left.ColumnCount(); c++) {
			auto lvalue = left_rows.GetValue(c, r);
			auto rvalue = right_rows.GetValue(c, r);

			if (!Value::DefaultValuesAreEqual(lvalue, rvalue)) {
				error_message =
				    StringUtil::Format("%s <> %s (row: %lld, col: %lld)\n", lvalue.ToString(), rvalue.ToString(), r, c);
				break;
			}
		}
		if (!error_message.empty()) {
			if (ordered) {
				return false;
			} else {
				break;
			}
		}
	}
	if (!error_message.empty()) {
		// do an unordered comparison
		bool found_all = true;
		for (idx_t c = 0; c < left.ColumnCount(); c++) {
			std::unordered_multiset<Value, ValueHashFunction, ValueResultEquals> lvalues;
			for (idx_t r = 0; r < left.Count(); r++) {
				auto lvalue = left_rows.GetValue(c, r);
				lvalues.insert(lvalue);
			}
			for (idx_t r = 0; r < right.Count(); r++) {
				auto rvalue = right_rows.GetValue(c, r);
				auto entry = lvalues.find(rvalue);
				if (entry == lvalues.end()) {
					found_all = false;
					break;
				}
				lvalues.erase(entry);
			}
			if (!found_all) {
				break;
			}
		}
		if (!found_all) {
			return false;
		}
		error_message = string();
	}
	return true;
}

vector<shared_ptr<StringHeap>> ColumnDataCollection::GetHeapReferences() {
	vector<shared_ptr<StringHeap>> result(segments.size(), nullptr);
	for (idx_t segment_idx = 0; segment_idx < segments.size(); segment_idx++) {
		result[segment_idx] = segments[segment_idx]->heap;
	}
	return result;
}

ColumnDataAllocatorType ColumnDataCollection::GetAllocatorType() const {
	return allocator->GetType();
}

BufferManager &ColumnDataCollection::GetBufferManager() const {
	D_ASSERT(allocator->GetType() == ColumnDataAllocatorType::BUFFER_MANAGER_ALLOCATOR);
	return allocator->GetBufferManager();
}

const vector<unique_ptr<ColumnDataCollectionSegment>> &ColumnDataCollection::GetSegments() const {
	return segments;
}

void ColumnDataCollection::Serialize(Serializer &serializer) const {
	vector<vector<Value>> values;
	values.resize(ColumnCount());
	for (auto &chunk : Chunks()) {
		for (idx_t c = 0; c < chunk.ColumnCount(); c++) {
			for (idx_t r = 0; r < chunk.size(); r++) {
				values[c].push_back(chunk.GetValue(c, r));
			}
		}
	}
	serializer.WriteProperty(100, "types", types);
	serializer.WriteProperty(101, "values", values);
}

unique_ptr<ColumnDataCollection> ColumnDataCollection::Deserialize(Deserializer &deserializer) {
	auto types = deserializer.ReadProperty<vector<LogicalType>>(100, "types");
	auto values = deserializer.ReadProperty<vector<vector<Value>>>(101, "values");

	auto collection = make_uniq<ColumnDataCollection>(Allocator::DefaultAllocator(), types);
	if (values.empty()) {
		return collection;
	}
	DataChunk chunk;
	chunk.Initialize(Allocator::DefaultAllocator(), types);

	for (idx_t r = 0; r < values[0].size(); r++) {
		for (idx_t c = 0; c < types.size(); c++) {
			chunk.data[c].Append(values[c][r]);
		}
		// the appends above already grow the child vectors, so chunk.size() reflects the new cardinality
		if (chunk.size() == STANDARD_VECTOR_SIZE) {
			collection->Append(chunk);
			chunk.Reset();
		}
	}
	if (chunk.size() > 0) {
		collection->Append(chunk);
	}
	return collection;
}

} // namespace duckdb
