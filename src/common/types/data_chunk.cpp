#include "duckdb/common/types/data_chunk.hpp"

#include "duckdb/common/array.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/printer.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/types/sel_cache.hpp"
#include "duckdb/common/types/vector_cache.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/enums/debug_verification_mode.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/execution/execution_context.hpp"

#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/common/serializer/binary_deserializer.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/common/enums/debug_verification_mode.hpp"
#include "duckdb/main/config.hpp"

namespace duckdb {

DataChunk::DataChunk() {
}

DataChunk::~DataChunk() {
}

void DataChunk::InitializeEmpty(const vector<LogicalType> &types) {
	D_ASSERT(data.empty());
	for (idx_t i = 0; i < types.size(); i++) {
		data.emplace_back(types[i], nullptr);
	}
}

void DataChunk::Initialize(ClientContext &context, const vector<LogicalType> &types, idx_t capacity_p) {
	Initialize(Allocator::Get(context), types, capacity_p);
}

void DataChunk::Initialize(Allocator &allocator, const vector<LogicalType> &types, idx_t capacity_p) {
	auto initialize = vector<bool>(types.size(), true);
	Initialize(allocator, types, initialize, capacity_p);
}

void DataChunk::Initialize(ClientContext &context, const vector<LogicalType> &types, const vector<bool> &initialize,
                           idx_t capacity_p) {
	Initialize(Allocator::Get(context), types, initialize, capacity_p);
}

void DataChunk::Initialize(Allocator &allocator, const vector<LogicalType> &types, const vector<bool> &initialize,
                           idx_t capacity) {
	D_ASSERT(types.size() == initialize.size());
	D_ASSERT(data.empty());

	for (idx_t i = 0; i < types.size(); i++) {
		// We copy the type here so we don't create another reference to the same LogicalTypeInfo
		// Otherwise, threads will constantly increment/decrement the atomic ref count to the same shared_ptr
		// This is necessary to avoid heavy contention on the atomic on many-core machines
		// Note that for nested types, there will still be contention on the atomic(s) one level down,
		// because this is a shallow copy (only copies LogicalTypeInfo to depth=1)
		auto copied_type = types[i].Copy();
		if (!initialize[i]) {
			data.emplace_back(copied_type, nullptr);
			vector_caches.emplace_back();
			continue;
		}

		VectorCache cache(allocator, copied_type, capacity);
		data.emplace_back(cache);
		vector_caches.push_back(std::move(cache));
	}
}

idx_t DataChunk::DeriveSize() const {
	for (const auto &v : data) {
		if (v.GetBufferRef()) {
			return v.size();
		}
	}
	if (data.empty() || !vector_caches.empty()) {
		// a column-less chunk, or an initialized chunk whose columns are all unset, is empty without an explicit count
		return 0;
	}
	throw InternalException(
	    "DataChunk::size() called but neither count was set, nor any vectors with valid counts were set");
}

idx_t DataChunk::GetCapacity() const {
	for (auto &cache : vector_caches) {
		auto capacity = cache.GetCapacity();
		if (capacity.IsValid()) {
			return capacity.GetIndex();
		}
	}
	// a chunk that was not initialized with its own vectors holds at most a standard vector's worth of rows
	return STANDARD_VECTOR_SIZE;
}

idx_t DataChunk::GetDataSize() const {
	idx_t total_size = 0;
	for (auto &vec : data) {
		total_size += vec.GetDataSize();
	}
	return total_size;
}

idx_t DataChunk::GetAllocationSize() const {
	idx_t total_size = 0;
	for (auto &vec : data) {
		total_size += vec.GetAllocationSize();
	}
	return total_size;
}

void DataChunk::Reset() {
	count = optional_idx();
	if (data.empty() || vector_caches.empty()) {
		count = 0;
		return;
	}
	if (vector_caches.size() != data.size()) {
		throw InternalException("VectorCache and column count mismatch in DataChunk::Reset");
	}
	for (idx_t i = 0; i < ColumnCount(); i++) {
		data[i].ResetFromCache(vector_caches[i]);
	}
}

void DataChunk::Destroy() {
	data.clear();
	vector_caches.clear();
	count = optional_idx();
}

Value DataChunk::GetValue(idx_t col_idx, idx_t index) const {
	D_ASSERT(index < size());
	return data[col_idx].GetValue(index);
}

void DataChunk::SetValue(idx_t col_idx, idx_t index, const Value &val) {
	data[col_idx].SetValue(index, val);
}

bool DataChunk::AllConstant() const {
	for (auto &v : data) {
		if (v.GetVectorType() != VectorType::CONSTANT_VECTOR) {
			return false;
		}
	}
	return true;
}

void DataChunk::CheckCardinality(idx_t count_p) {
	for (auto &v : data) {
		if (v.size() != count_p) {
			throw InternalException("DataChunk::CheckCardinality - vector has size %d but expected size %d", v.size(),
			                        count_p);
		}
	}
	this->count = optional_idx();
}

void DataChunk::SetChildCardinality(idx_t count_p) {
	for (auto &v : data) {
		// null-buffer placeholders (InitializeEmpty) and non-flat/constant vectors cannot be resized
		if (!v.GetBufferRef()) {
			continue;
		}
		auto vtype = v.GetVectorType();
		if (vtype == VectorType::FLAT_VECTOR || vtype == VectorType::CONSTANT_VECTOR) {
			FlatVector::SetSize(v, count_p);
		} else if (v.size() != count_p) {
			throw InternalException("DataChunk::SetChildCardinality - vector has size %d but expected size %d - and "
			                        "cannot change vector size because it is not a flat or constant vector",
			                        v.size(), count_p);
		}
	}
	this->count = count_p;
}

void DataChunk::Reference(DataChunk &chunk) {
	D_ASSERT(chunk.ColumnCount() <= ColumnCount());
	for (idx_t i = 0; i < chunk.ColumnCount(); i++) {
		data[i].Reference(chunk.data[i]);
	}
	count = chunk.count;
}

void DataChunk::Move(DataChunk &chunk) {
	data = std::move(chunk.data);
	vector_caches = std::move(chunk.vector_caches);
	count = chunk.count;
	chunk.Destroy();
}

void DataChunk::Copy(DataChunk &other, idx_t offset) const {
	D_ASSERT(ColumnCount() == other.ColumnCount());
	D_ASSERT(other.size() == 0);

	const idx_t target_count = size() - offset;
	for (idx_t i = 0; i < ColumnCount(); i++) {
		D_ASSERT(other.data[i].GetVectorType() == VectorType::FLAT_VECTOR);
		VectorOperations::Copy(data[i], other.data[i], size(), offset, 0);
		FlatVector::SetSize(other.data[i], target_count);
	}
	other.count = optional_idx();
}

void DataChunk::Copy(DataChunk &other, const SelectionVector &sel, const idx_t source_count, const idx_t offset) const {
	D_ASSERT(ColumnCount() == other.ColumnCount());
	D_ASSERT(other.size() == 0);
	D_ASSERT(source_count <= size());

	const idx_t target_count = source_count - offset;
	for (idx_t i = 0; i < ColumnCount(); i++) {
		D_ASSERT(other.data[i].GetVectorType() == VectorType::FLAT_VECTOR);
		VectorOperations::Copy(data[i], other.data[i], sel, source_count, offset, 0);
		FlatVector::SetSize(other.data[i], target_count);
	}
	other.count = optional_idx();
}

void DataChunk::Split(DataChunk &other, idx_t split_idx) {
	D_ASSERT(other.size() == 0);
	D_ASSERT(other.data.empty());
	D_ASSERT(split_idx < data.size());
	const idx_t num_cols = data.size();
	for (idx_t col_idx = split_idx; col_idx < num_cols; col_idx++) {
		other.data.push_back(std::move(data[col_idx]));
		other.vector_caches.push_back(std::move(vector_caches[col_idx]));
	}
	for (idx_t col_idx = split_idx; col_idx < num_cols; col_idx++) {
		data.pop_back();
		vector_caches.pop_back();
	}
	count = other.count;
}

void DataChunk::Fuse(DataChunk &other) {
	D_ASSERT(other.size() == size());
	const idx_t num_cols = other.data.size();
	for (idx_t col_idx = 0; col_idx < num_cols; ++col_idx) {
		data.emplace_back(std::move(other.data[col_idx]));
		vector_caches.emplace_back(std::move(other.vector_caches[col_idx]));
	}
	count = other.count;
	other.Destroy();
}

void DataChunk::ReferenceColumns(DataChunk &other, const vector<column_t> &column_ids) {
	D_ASSERT(ColumnCount() == column_ids.size());
	Reset();
	for (idx_t col_idx = 0; col_idx < ColumnCount(); col_idx++) {
		const auto &other_col = other.data[column_ids[col_idx]];
		auto &this_col = data[col_idx];
		D_ASSERT(other_col.GetType() == this_col.GetType());
		this_col.Reference(other_col);
	}
	count = other.count;
}

void DataChunk::Append(const DataChunk &other, VectorAppendMode append_mode) {
	Append(other, *FlatVector::IncrementalSelectionVector(), other.size(), append_mode);
}

void DataChunk::Append(const DataChunk &other, const SelectionVector &sel, idx_t sel_count,
                       VectorAppendMode append_mode) {
	if (sel_count == 0) {
		return;
	}
	if (ColumnCount() != other.ColumnCount()) {
		throw InternalException("Column counts of appending chunk doesn't match!");
	}
	idx_t current_size = size();
	for (idx_t i = 0; i < ColumnCount(); i++) {
		// ensure data[i] has the chunk's current size so the append computes new_size = current + append_size
		if (data[i].size() != current_size) {
			throw InternalException("DataChunk::Append size mismatch - child has count %d but chunk has size %d",
			                        data[i].size(), current_size);
		}
		if (sel.IsSet()) {
			data[i].Append(other.data[i], sel, sel_count, append_mode);
		} else {
			data[i].Append(other.data[i], other.size(), append_mode);
		}
	}
	CheckCardinality(current_size + sel_count);
	if (ColumnCount() == 0) {
		// a column-less chunk has no child vectors to carry the cardinality - record it explicitly
		count = current_size + sel_count;
	} else {
		count = optional_idx();
	}
}

void DataChunk::Flatten() {
	for (idx_t i = 0; i < ColumnCount(); i++) {
		data[i].Flatten();
	}
}

vector<LogicalType> DataChunk::GetTypes() const {
	vector<LogicalType> types;
	for (idx_t i = 0; i < ColumnCount(); i++) {
		types.push_back(data[i].GetType());
	}
	return types;
}

string DataChunk::ToString() const {
	string retval = "Chunk - [" + to_string(ColumnCount()) + " Columns]\n";
	for (idx_t i = 0; i < ColumnCount(); i++) {
		retval += "- " + data[i].ToString() + "\n";
	}
	return retval;
}

void DataChunk::Serialize(Serializer &serializer, bool compressed_serialization) const {
	// write the count
	auto row_count = size();
	serializer.WriteProperty<sel_t>(100, "rows", NumericCast<sel_t>(row_count));

	// we should never try to serialize empty data chunks
	auto column_count = ColumnCount();
	D_ASSERT(column_count);

	// write the types
	serializer.WriteList(101, "types", column_count,
	                     [&](Serializer::List &list, idx_t i) { list.WriteElement(data[i].GetType()); });

	// write the data
	serializer.WriteList(102, "columns", column_count, [&](Serializer::List &list, idx_t i) {
		list.WriteObject([&](Serializer &object) {
			// Reference the vector to avoid potentially mutating it during serialization
			Vector serialized_vector(data[i].GetType());
			if (data[i].GetBufferRef()) {
				serialized_vector.Reference(data[i]);
			}
			serialized_vector.Serialize(object, compressed_serialization);
		});
	});
}

void DataChunk::Deserialize(Deserializer &deserializer) {
	// read and set the row count
	auto row_count = deserializer.ReadProperty<sel_t>(100, "rows");

	// read the types
	vector<LogicalType> types;
	deserializer.ReadList(101, "types", [&](Deserializer::List &list, idx_t i) {
		auto type = list.ReadElement<LogicalType>();
		types.push_back(type);
	});

	// initialize the data chunk
	D_ASSERT(!types.empty());
	Initialize(Allocator::DefaultAllocator(), types, MaxValue<idx_t>(row_count, STANDARD_VECTOR_SIZE));

	// read the data
	deserializer.ReadList(102, "columns", [&](Deserializer::List &list, idx_t i) {
		list.ReadObject([&](Deserializer &object) { data[i].Deserialize(object, row_count); });
	});
	SetChildCardinality(row_count);
}

void DataChunk::Slice(const SelectionVector &sel_vector, idx_t count_p) {
	SelCache merge_cache;
	for (idx_t c = 0; c < ColumnCount(); c++) {
		data[c].Slice(sel_vector, count_p, merge_cache);
	}
	count = count_p;
}

void DataChunk::Slice(const DataChunk &other, idx_t offset, idx_t end) {
	if (end < offset) {
		throw InternalException("end is smaller than offset");
	}
	for (idx_t c = 0; c < other.ColumnCount(); c++) {
		data[c].Slice(other.data[c], offset, end);
	}
	count = end - offset;
}

void DataChunk::Slice(const DataChunk &other, const SelectionVector &sel, idx_t count_p, idx_t col_offset) {
	D_ASSERT(other.ColumnCount() <= col_offset + ColumnCount());
	SelCache merge_cache;
	for (idx_t c = 0; c < other.ColumnCount(); c++) {
		if (other.data[c].GetVectorType() == VectorType::DICTIONARY_VECTOR) {
			// already a dictionary! merge the dictionaries
			data[col_offset + c].Reference(other.data[c]);
			data[col_offset + c].Slice(sel, count_p, merge_cache);
		} else {
			data[col_offset + c].Slice(other.data[c], sel, count_p);
		}
	}
	count = count_p;
}

void DataChunk::Slice(idx_t offset, idx_t slice_count) {
	D_ASSERT(offset + slice_count <= size());
	if (offset == 0 && slice_count == size()) {
		return;
	}
	SelectionVector sel(slice_count);
	for (idx_t i = 0; i < slice_count; i++) {
		sel.set_index(i, offset + i);
	}
	Slice(sel, slice_count);
}

unsafe_unique_array<UnifiedVectorFormat> DataChunk::ToUnifiedFormat() {
	auto unified_data = make_unsafe_uniq_array<UnifiedVectorFormat>(ColumnCount());
	for (idx_t col_idx = 0; col_idx < ColumnCount(); col_idx++) {
		data[col_idx].ToUnifiedFormat(unified_data[col_idx]);
	}
	return unified_data;
}

void DataChunk::Hash(Vector &result) {
	D_ASSERT(result.GetType().id() == LogicalType::HASH);
	auto hash_count = size();
	VectorOperations::Hash(data[0], result, hash_count);
	for (idx_t i = 1; i < ColumnCount(); i++) {
		VectorOperations::CombineHash(result, data[i], hash_count);
	}
}

void DataChunk::Hash(vector<idx_t> &column_ids, Vector &result) {
	D_ASSERT(result.GetType().id() == LogicalType::HASH);
	D_ASSERT(!column_ids.empty());
	auto hash_count = size();
	VectorOperations::Hash(data[column_ids[0]], result, hash_count);
	for (idx_t i = 1; i < column_ids.size(); i++) {
		VectorOperations::CombineHash(result, data[column_ids[i]], hash_count);
	}
}

void DataChunk::Verify(ClientContext &context) {
	Verify(DatabaseInstance::GetDatabase(context));
}

void DataChunk::Verify(shared_ptr<DatabaseInstance> &db) {
	if (db) {
		Verify(*db);
	} else {
		Verify();
	}
}

void DataChunk::Verify(optional_ptr<ClientContext> context) {
	if (context) {
		Verify(*context);
	} else {
		Verify();
	}
}

void DataChunk::Verify(DatabaseInstance &database_instance) {
	VerifyInternal(DebugVerificationMode::DEFAULT, database_instance);
}

void DataChunk::Verify() {
	VerifyInternal(DebugVerificationMode::DEFAULT, nullptr);
}

void DataChunk::VerifyInternal(DebugVerificationMode mode, optional_ptr<DatabaseInstance> db) {
	if (mode == DebugVerificationMode::DEFAULT) {
		mode = DBConfigOptions::global_verification_mode;
	}
	if (mode == DebugVerificationMode::NONE) {
		return;
	}
	if (mode == DebugVerificationMode::VERIFY_VECTORS) {
		// verify all vectors have the same size as the chunk
		for (idx_t i = 0; i < ColumnCount(); i++) {
			if (data[i].size() != size()) {
				throw InternalException(
				    "DataChunk::Verify - size mismatch: vector %d (%s) has size %d but chunk has size %d", i,
				    data[i].GetType().ToString(), data[i].size(), size());
			}
		}
		// verify internal consistency of the chunk
		for (idx_t i = 0; i < ColumnCount(); i++) {
			data[i].Verify();
		}
	}
	if (mode == DebugVerificationMode::VERIFY_SERIALIZATION) {
		if (!ColumnCount()) {
			// don't try to round-trip dummy data chunks with no data
			// e.g., these exist in queries like 'SELECT distinct(col0, col1) FROM tbl', where we have groups, but no
			// payload so the payload will be such an empty data chunk
			return;
		}

		// verify that we can round-trip chunk serialization
		Allocator allocator;
		MemoryStream mem_stream(allocator);

		// this is the way we can ensure that the `Serialize` and `Deserialize` methods of a `DataChunk` are consistent
		// with each other within the current and previous versions of the code. This is an internal round-trip sanity
		// check performed in memory. It does not write to a persistent database file. Therefore, when a version is not
		// indicated (latest), it should always use the full set of capabilities currently supported by the engine to
		// ensure that all valid in-memory states can be verified.

		SerializationOptions options;
		options.storage_compatibility = StorageCompatibility::Latest();

		if (db) {
			DBConfig &config = DBConfig::GetConfig(*db);
			if (config.options.storage_compatibility.manually_set) {
				options.storage_compatibility = config.options.storage_compatibility;
			}
		}

		BinarySerializer serializer(mem_stream, options);

		serializer.Begin();
		Serialize(serializer);
		serializer.End();

		mem_stream.Rewind();

		BinaryDeserializer deserializer(mem_stream);
		DataChunk new_chunk;

		deserializer.Begin();
		new_chunk.Deserialize(deserializer);
		deserializer.End();

		if (size() != new_chunk.size()) {
			throw InternalException("Data Chunk Verification: Serialization size mismatch");
		}
	}
}

void DataChunk::Print() const {
	Printer::Print(ToString());
}

} // namespace duckdb
