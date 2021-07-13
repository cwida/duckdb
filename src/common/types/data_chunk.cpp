#include "duckdb/common/types/data_chunk.hpp"

#include "duckdb/common/array.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/printer.hpp"
#include "duckdb/common/serializer.hpp"
#include "duckdb/common/types/null_value.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/types/sel_cache.hpp"
#include "duckdb/common/arrow.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/to_string.hpp"
#include "duckdb/common/types/vector_cache.hpp"
#include "duckdb/common/types/interval.hpp"

namespace duckdb {

DataChunk::DataChunk() : count(0) {
}

DataChunk::~DataChunk() {
}

void DataChunk::InitializeEmpty(const vector<LogicalType> &types) {
	D_ASSERT(data.empty());   // can only be initialized once
	D_ASSERT(!types.empty()); // empty chunk not allowed
	for (idx_t i = 0; i < types.size(); i++) {
		data.emplace_back(Vector(types[i], nullptr));
	}
}

void DataChunk::Initialize(const vector<LogicalType> &types) {
	D_ASSERT(data.empty());   // can only be initialized once
	D_ASSERT(!types.empty()); // empty chunk not allowed
	for (idx_t i = 0; i < types.size(); i++) {
		VectorCache cache(types[i]);
		data.emplace_back(cache);
		vector_caches.push_back(move(cache));
	}
}

void DataChunk::Reset() {
	if (data.empty()) {
		return;
	}
	if (vector_caches.size() != data.size()) {
		throw InternalException("VectorCache and column count mismatch in DataChunk::Reset");
	}
	for (idx_t i = 0; i < ColumnCount(); i++) {
		data[i].ResetFromCache(vector_caches[i]);
	}
	SetCardinality(0);
}

void DataChunk::Destroy() {
	data.clear();
	vector_caches.clear();
	SetCardinality(0);
}

Value DataChunk::GetValue(idx_t col_idx, idx_t index) const {
	D_ASSERT(index < size());
	return data[col_idx].GetValue(index);
}

void DataChunk::SetValue(idx_t col_idx, idx_t index, const Value &val) {
	data[col_idx].SetValue(index, val);
}

void DataChunk::Reference(DataChunk &chunk) {
	D_ASSERT(chunk.ColumnCount() <= ColumnCount());
	SetCardinality(chunk);
	for (idx_t i = 0; i < chunk.ColumnCount(); i++) {
		data[i].Reference(chunk.data[i]);
	}
}

void DataChunk::Move(DataChunk &chunk) {
	SetCardinality(chunk);
	data = move(chunk.data);
	vector_caches = move(chunk.vector_caches);

	chunk.Destroy();
}

void DataChunk::Copy(DataChunk &other, idx_t offset) const {
	D_ASSERT(ColumnCount() == other.ColumnCount());
	D_ASSERT(other.size() == 0);

	for (idx_t i = 0; i < ColumnCount(); i++) {
		D_ASSERT(other.data[i].GetVectorType() == VectorType::FLAT_VECTOR);
		VectorOperations::Copy(data[i], other.data[i], size(), offset, 0);
	}
	other.SetCardinality(size() - offset);
}

void DataChunk::Copy(DataChunk &other, const SelectionVector &sel, const idx_t source_count, const idx_t offset) const {
	D_ASSERT(ColumnCount() == other.ColumnCount());
	D_ASSERT(other.size() == 0);
	D_ASSERT((offset + source_count) <= size());

	for (idx_t i = 0; i < ColumnCount(); i++) {
		D_ASSERT(other.data[i].GetVectorType() == VectorType::FLAT_VECTOR);
		VectorOperations::Copy(data[i], other.data[i], sel, source_count, offset, 0);
	}
	other.SetCardinality(source_count - offset);
}

void DataChunk::Append(const DataChunk &other) {
	if (other.size() == 0) {
		return;
	}
	if (ColumnCount() != other.ColumnCount()) {
		throw InternalException("Column counts of appending chunk doesn't match!");
	}
	for (idx_t i = 0; i < ColumnCount(); i++) {
		D_ASSERT(data[i].GetVectorType() == VectorType::FLAT_VECTOR);
		VectorOperations::Copy(other.data[i], data[i], other.size(), 0, size());
	}
	SetCardinality(size() + other.size());
}

void DataChunk::Normalify() {
	for (idx_t i = 0; i < ColumnCount(); i++) {
		data[i].Normalify(size());
	}
}

vector<LogicalType> DataChunk::GetTypes() {
	vector<LogicalType> types;
	for (idx_t i = 0; i < ColumnCount(); i++) {
		types.push_back(data[i].GetType());
	}
	return types;
}

string DataChunk::ToString() const {
	string retval = "Chunk - [" + to_string(ColumnCount()) + " Columns]\n";
	for (idx_t i = 0; i < ColumnCount(); i++) {
		retval += "- " + data[i].ToString(size()) + "\n";
	}
	return retval;
}

void DataChunk::Serialize(Serializer &serializer) {
	// write the count
	serializer.Write<sel_t>(size());
	serializer.Write<idx_t>(ColumnCount());
	for (idx_t col_idx = 0; col_idx < ColumnCount(); col_idx++) {
		// write the types
		data[col_idx].GetType().Serialize(serializer);
	}
	// write the data
	for (idx_t col_idx = 0; col_idx < ColumnCount(); col_idx++) {
		data[col_idx].Serialize(size(), serializer);
	}
}

void DataChunk::Deserialize(Deserializer &source) {
	auto rows = source.Read<sel_t>();
	idx_t column_count = source.Read<idx_t>();

	vector<LogicalType> types;
	for (idx_t i = 0; i < column_count; i++) {
		types.push_back(LogicalType::Deserialize(source));
	}
	Initialize(types);
	// now load the column data
	SetCardinality(rows);
	for (idx_t i = 0; i < column_count; i++) {
		data[i].Deserialize(rows, source);
	}
	Verify();
}

void DataChunk::Slice(const SelectionVector &sel_vector, idx_t count_p) {
	this->count = count_p;
	SelCache merge_cache;
	for (idx_t c = 0; c < ColumnCount(); c++) {
		data[c].Slice(sel_vector, count_p, merge_cache);
	}
}

void DataChunk::Slice(DataChunk &other, const SelectionVector &sel, idx_t count_p, idx_t col_offset) {
	D_ASSERT(other.ColumnCount() <= col_offset + ColumnCount());
	this->count = count_p;
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
}

unique_ptr<VectorData[]> DataChunk::Orrify() {
	auto orrified_data = unique_ptr<VectorData[]>(new VectorData[ColumnCount()]);
	for (idx_t col_idx = 0; col_idx < ColumnCount(); col_idx++) {
		data[col_idx].Orrify(size(), orrified_data[col_idx]);
	}
	return orrified_data;
}

void DataChunk::Hash(Vector &result) {
	D_ASSERT(result.GetType().id() == LogicalTypeId::HASH);
	VectorOperations::Hash(data[0], result, size());
	for (idx_t i = 1; i < ColumnCount(); i++) {
		VectorOperations::CombineHash(result, data[i], size());
	}
}

void DataChunk::Verify() {
#ifdef DEBUG
	D_ASSERT(size() <= STANDARD_VECTOR_SIZE);
	// verify that all vectors in this chunk have the chunk selection vector
	for (idx_t i = 0; i < ColumnCount(); i++) {
		data[i].Verify(size());
	}
#endif
}

void DataChunk::Print() {
	Printer::Print(ToString());
}

struct DuckDBArrowArrayChildHolder {
	ArrowArray array;
	//! need max three pointers for strings
	duckdb::array<const void *, 3> buffers = {{nullptr, nullptr, nullptr}};
	unique_ptr<Vector> vector;
	unique_ptr<data_t[]> offsets;
	unique_ptr<data_t[]> data;
	//! Children of nested structures
	::duckdb::vector<DuckDBArrowArrayChildHolder> children;
	::duckdb::vector<ArrowArray *> children_ptrs;
};

struct DuckDBArrowArrayHolder {
	vector<DuckDBArrowArrayChildHolder> children = {};
	vector<ArrowArray *> children_ptrs = {};
	array<const void *, 1> buffers = {{nullptr}};
};

static void ReleaseDuckDBArrowArray(ArrowArray *array) {
	if (!array || !array->release) {
		return;
	}
	array->release = nullptr;
	auto holder = static_cast<DuckDBArrowArrayHolder *>(array->private_data);
	delete holder;
}

void InitializeChild(DuckDBArrowArrayChildHolder &child_holder, idx_t size) {
	auto &child = child_holder.array;
	child.private_data = nullptr;
	child.release = ReleaseDuckDBArrowArray;
	child.n_children = 0;
	child.null_count = 0;
	child.offset = 0;
	child.dictionary = nullptr;
	child.buffers = child_holder.buffers.data();

	child.length = size;
}

void SetChildValidityMask(Vector &vector, ArrowArray &child) {
	auto &mask = FlatVector::Validity(vector);
	if (!mask.AllValid()) {
		//! any bits are set: might have nulls
		child.null_count = -1;
	} else {
		//! no bits are set; we know there are no nulls
		child.null_count = 0;
	}
	child.buffers[0] = (void *)mask.GetData();
}

void SetArrowChild(DuckDBArrowArrayChildHolder &child_holder, const LogicalType &type, Vector &data, idx_t size);

void SetList(DuckDBArrowArrayChildHolder &child_holder, const LogicalType &type, Vector &data, idx_t size) {
	auto &child = child_holder.array;
	child_holder.vector = make_unique<Vector>(data);

	//! Lists have two buffers
	child.n_buffers = 2;
	//! Second Buffer is the list offsets
	child_holder.offsets = unique_ptr<data_t[]>(new data_t[sizeof(uint32_t) * (size + 1)]);
	child.buffers[1] = child_holder.offsets.get();
	auto offset_ptr = (uint32_t *)child.buffers[1];
	auto list_data = FlatVector::GetData<list_entry_t>(data);
	auto list_mask = FlatVector::Validity(data);
	idx_t offset = 0;
	offset_ptr[0] = 0;
	for (idx_t i = 0; i < size; i++) {
		auto &le = list_data[i];

		if (list_mask.RowIsValid(i)) {
			offset += le.length;
		}

		offset_ptr[i + 1] = offset;
	}
	auto list_size = ListVector::GetListSize(data);
	child_holder.children.resize(1);
	InitializeChild(child_holder.children[0], list_size);
	child.n_children = 1;
	child_holder.children_ptrs.push_back(&child_holder.children[0].array);
	child.children = &child_holder.children_ptrs[0];
	auto &child_vector = ListVector::GetEntry(data);
	auto &child_type = ListType::GetChildType(type);
	SetArrowChild(child_holder.children[0], child_type, child_vector, list_size);
	SetChildValidityMask(child_vector, child_holder.children[0].array);
}

void SetStruct(DuckDBArrowArrayChildHolder &child_holder, const LogicalType &type, Vector &data, idx_t size) {
	auto &child = child_holder.array;
	child_holder.vector = make_unique<Vector>(data);

	//! Structs only have validity buffers
	child.n_buffers = 1;
	auto &children = StructVector::GetEntries(*child_holder.vector);
	child.n_children = children.size();
	child_holder.children.resize(child.n_children);
	for (auto &struct_child : child_holder.children) {
		InitializeChild(struct_child, size);
		child_holder.children_ptrs.push_back(&struct_child.array);
	}
	child.children = &child_holder.children_ptrs[0];
	for (idx_t child_idx = 0; child_idx < child_holder.children.size(); child_idx++) {
		SetArrowChild(child_holder.children[child_idx], StructType::GetChildType(type, child_idx), *children[child_idx],
		              size);
		SetChildValidityMask(*children[child_idx], child_holder.children[child_idx].array);
	}
}

void SetStructMap(DuckDBArrowArrayChildHolder &child_holder, const LogicalType &type, Vector &data, idx_t size) {
	auto &child = child_holder.array;
	child_holder.vector = make_unique<Vector>(data);

	//! Structs only have validity buffers
	child.n_buffers = 1;
	auto &children = StructVector::GetEntries(*child_holder.vector);
	child.n_children = children.size();
	child_holder.children.resize(child.n_children);
	auto list_size = ListVector::GetListSize(*children[0]);
	child.length = list_size;
	for (auto &struct_child : child_holder.children) {
		InitializeChild(struct_child, list_size);
		child_holder.children_ptrs.push_back(&struct_child.array);
	}
	child.children = &child_holder.children_ptrs[0];
	auto &child_types = StructType::GetChildTypes(type);
	for (idx_t child_idx = 0; child_idx < child_holder.children.size(); child_idx++) {
		auto &list_vector_child = ListVector::GetEntry(*children[child_idx]);
		if (child_idx == 0) {
			VectorData list_data;
			children[child_idx]->Orrify(size, list_data);
			auto list_child_validity = FlatVector::Validity(list_vector_child);
			if (!list_child_validity.AllValid()) {
				//! Get the offsets to check from the selection vector
				auto list_offsets = FlatVector::GetData<list_entry_t>(*children[child_idx]);
				for (idx_t list_idx = 0; list_idx < size; list_idx++) {
					auto offset = list_offsets[list_data.sel->get_index(list_idx)];
					if (!list_child_validity.CheckAllValid(offset.length + offset.offset, offset.offset)) {
						throw std::runtime_error("Arrow doesnt accept NULL keys on Maps");
					}
				}
			}
		} else {
			SetChildValidityMask(list_vector_child, child_holder.children[child_idx].array);
		}
		SetArrowChild(child_holder.children[child_idx], ListType::GetChildType(child_types[child_idx].second),
		              list_vector_child, list_size);
	}
}

void SetArrowChild(DuckDBArrowArrayChildHolder &child_holder, const LogicalType &type, Vector &data, idx_t size) {
	auto &child = child_holder.array;
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN: {
		//! Gotta bitpack these booleans
		child_holder.vector = make_unique<Vector>(data);
		child.n_buffers = 2;
		idx_t num_bytes = (size + 8 - 1) / 8;
		child_holder.data = unique_ptr<data_t[]>(new data_t[sizeof(uint8_t) * num_bytes]);
		child.buffers[1] = child_holder.data.get();
		auto source_ptr = FlatVector::GetData<uint8_t>(*child_holder.vector);
		auto target_ptr = (uint8_t *)child.buffers[1];
		idx_t target_pos = 0;
		idx_t cur_bit = 0;
		for (idx_t row_idx = 0; row_idx < size; row_idx++) {
			if (cur_bit == 8) {
				target_pos++;
				cur_bit = 0;
			}
			if (source_ptr[row_idx] == 0) {
				//! We set the bit to 0
				target_ptr[target_pos] &= ~(1 << cur_bit);
			} else {
				//! We set the bit to 1
				target_ptr[target_pos] |= 1 << cur_bit;
			}
			cur_bit++;
		}
		break;
	}
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::DATE:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIME:
		child_holder.vector = make_unique<Vector>(data);
		child.n_buffers = 2;
		child.buffers[1] = (void *)FlatVector::GetData(*child_holder.vector);
		break;
	case LogicalTypeId::SQLNULL:
		child.n_buffers = 1;
		break;
	case LogicalTypeId::DECIMAL: {
		child.n_buffers = 2;
		child_holder.vector = make_unique<Vector>(data);

		//! We have to convert to INT128
		switch (type.InternalType()) {

		case PhysicalType::INT16: {
			child_holder.data = unique_ptr<data_t[]>(new data_t[sizeof(hugeint_t) * (size)]);
			child.buffers[1] = child_holder.data.get();
			auto source_ptr = FlatVector::GetData<int16_t>(*child_holder.vector);
			auto target_ptr = (hugeint_t *)child.buffers[1];
			for (idx_t row_idx = 0; row_idx < size; row_idx++) {
				target_ptr[row_idx] = source_ptr[row_idx];
			}
			break;
		}
		case PhysicalType::INT32: {
			child_holder.data = unique_ptr<data_t[]>(new data_t[sizeof(hugeint_t) * (size)]);
			child.buffers[1] = child_holder.data.get();
			auto source_ptr = FlatVector::GetData<int32_t>(*child_holder.vector);
			auto target_ptr = (hugeint_t *)child.buffers[1];
			for (idx_t row_idx = 0; row_idx < size; row_idx++) {
				target_ptr[row_idx] = source_ptr[row_idx];
			}
			break;
		}
		case PhysicalType::INT64: {
			child_holder.data = unique_ptr<data_t[]>(new data_t[sizeof(hugeint_t) * (size)]);
			child.buffers[1] = child_holder.data.get();
			auto source_ptr = FlatVector::GetData<int64_t>(*child_holder.vector);
			auto target_ptr = (hugeint_t *)child.buffers[1];
			for (idx_t row_idx = 0; row_idx < size; row_idx++) {
				target_ptr[row_idx] = source_ptr[row_idx];
			}
			break;
		}
		case PhysicalType::INT128: {
			child.buffers[1] = (void *)FlatVector::GetData(*child_holder.vector);
			break;
		}
		default:
			throw std::runtime_error("Unsupported physical type for Decimal" + TypeIdToString(type.InternalType()));
		}
		break;
	}
	case LogicalTypeId::BLOB:
	case LogicalTypeId::VARCHAR: {
		child_holder.vector = make_unique<Vector>(data);
		child.n_buffers = 3;
		child_holder.offsets = unique_ptr<data_t[]>(new data_t[sizeof(uint32_t) * (size + 1)]);
		child.buffers[1] = child_holder.offsets.get();
		D_ASSERT(child.buffers[1]);
		//! step 1: figure out total string length:
		idx_t total_string_length = 0;
		auto string_t_ptr = FlatVector::GetData<string_t>(*child_holder.vector);
		auto &mask = FlatVector::Validity(*child_holder.vector);
		for (idx_t row_idx = 0; row_idx < size; row_idx++) {
			if (!mask.RowIsValid(row_idx)) {
				continue;
			}
			total_string_length += string_t_ptr[row_idx].GetSize();
		}
		//! step 2: allocate this much
		child_holder.data = unique_ptr<data_t[]>(new data_t[total_string_length]);
		child.buffers[2] = child_holder.data.get();
		D_ASSERT(child.buffers[2]);
		//! step 3: assign buffers
		idx_t current_heap_offset = 0;
		auto target_ptr = (uint32_t *)child.buffers[1];

		for (idx_t row_idx = 0; row_idx < size; row_idx++) {
			target_ptr[row_idx] = current_heap_offset;
			if (!mask.RowIsValid(row_idx)) {
				continue;
			}
			auto &str = string_t_ptr[row_idx];
			memcpy((void *)((uint8_t *)child.buffers[2] + current_heap_offset), str.GetDataUnsafe(), str.GetSize());
			current_heap_offset += str.GetSize();
		}
		target_ptr[size] = current_heap_offset; //! need to terminate last string!
		break;
	}
	case LogicalTypeId::LIST: {
		SetList(child_holder, type, data, size);
		break;
	}
	case LogicalTypeId::STRUCT: {
		SetStruct(child_holder, type, data, size);
		break;
	}
	case LogicalTypeId::MAP: {
		child_holder.vector = make_unique<Vector>(data);

		auto &map_mask = FlatVector::Validity(*child_holder.vector);
		child.n_buffers = 2;
		//! Maps have one child
		child.n_children = 1;
		child_holder.children.resize(1);
		InitializeChild(child_holder.children[0], size);
		child_holder.children_ptrs.push_back(&child_holder.children[0].array);
		//! Second Buffer is the offsets
		child_holder.offsets = unique_ptr<data_t[]>(new data_t[sizeof(uint32_t) * (size + 1)]);
		child.buffers[1] = child_holder.offsets.get();
		auto &struct_children = StructVector::GetEntries(data);
		auto offset_ptr = (uint32_t *)child.buffers[1];
		auto list_data = FlatVector::GetData<list_entry_t>(*struct_children[0]);
		idx_t offset = 0;
		offset_ptr[0] = 0;
		for (idx_t i = 0; i < size; i++) {
			auto &le = list_data[i];
			if (map_mask.RowIsValid(i)) {
				offset += le.length;
			}
			offset_ptr[i + 1] = offset;
		}
		child.children = &child_holder.children_ptrs[0];
		//! We need to set up a struct
		auto struct_type = LogicalType::STRUCT(StructType::GetChildTypes(type));

		SetStructMap(child_holder.children[0], struct_type, *child_holder.vector, size);
		break;
	}
	case LogicalTypeId::INTERVAL: {
		//! convert interval from month/days/ucs to milliseconds
		child_holder.vector = make_unique<Vector>(data);
		child.n_buffers = 2;
		child_holder.data = unique_ptr<data_t[]>(new data_t[sizeof(int64_t) * (size)]);
		child.buffers[1] = child_holder.data.get();
		auto source_ptr = FlatVector::GetData<interval_t>(*child_holder.vector);
		auto target_ptr = (int64_t *)child.buffers[1];
		for (idx_t row_idx = 0; row_idx < size; row_idx++) {
			target_ptr[row_idx] = Interval::GetMilli(source_ptr[row_idx]);
		}
		break;
	}
	default:
		throw std::runtime_error("Unsupported type " + type.ToString());
	}
} // namespace duckdb
void DataChunk::ToArrowArray(ArrowArray *out_array) {
	Normalify();
	D_ASSERT(out_array);

	// Allocate as unique_ptr first to cleanup properly on error
	auto root_holder = make_unique<DuckDBArrowArrayHolder>();

	// Allocate the children
	root_holder->children.resize(ColumnCount());
	root_holder->children_ptrs.resize(ColumnCount(), nullptr);
	for (size_t i = 0; i < ColumnCount(); ++i) {
		root_holder->children_ptrs[i] = &root_holder->children[i].array;
	}
	out_array->children = root_holder->children_ptrs.data();
	out_array->n_children = ColumnCount();

	// Configure root array
	out_array->length = size();
	out_array->n_children = ColumnCount();
	out_array->n_buffers = 1;
	out_array->buffers = root_holder->buffers.data(); // there is no actual buffer there since we don't have NULLs
	out_array->offset = 0;
	out_array->null_count = 0; // needs to be 0
	out_array->dictionary = nullptr;

	//! Configure child arrays
	for (idx_t col_idx = 0; col_idx < ColumnCount(); col_idx++) {
		auto &child_holder = root_holder->children[col_idx];
		InitializeChild(child_holder, size());
		auto &vector = child_holder.vector;
		auto &child = child_holder.array;

		//! We could, in theory, output other types of vectors here, currently only FLAT Vectors
		SetArrowChild(child_holder, GetTypes()[col_idx], data[col_idx], size());
		SetChildValidityMask(*vector, child);
		out_array->children[col_idx] = &child;
	}

	// Release ownership to caller
	out_array->private_data = root_holder.release();
	out_array->release = ReleaseDuckDBArrowArray;
}

} // namespace duckdb
