#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/common/vector/dictionary_vector.hpp"
#include "duckdb/common/types/bignum.hpp"
#include "duckdb/common/types/bit.hpp"

namespace duckdb {

VectorWriter<string_t>::VectorWriter(Vector &vector, idx_t count, idx_t offset)
    : vector(vector), data(FlatVector::GetDataMutable<string_t>(vector)), validity(FlatVector::ValidityMutable(vector)),
      count(offset + count), current_idx(offset) {
}

void VectorWriter<string_t>::InitializeHeap() {
	heap = StringVector::GetStringHeap(vector);
}

VectorScatterWriter<string_t>::VectorScatterWriter(Vector &vector)
    : vector(vector), data(FlatVector::GetDataMutable<string_t>(vector)),
      validity(FlatVector::ValidityMutable(vector)) {
}

void VectorScatterWriter<string_t>::InitializeHeap() {
	heap = StringVector::GetStringHeap(vector);
}

VectorStringBuffer::VectorStringBuffer() : StandardVectorBuffer(capacity_t(0), sizeof(string_t)) {
	buffer_type = VectorBufferType::STRING_BUFFER;
}

VectorStringBuffer::VectorStringBuffer(Allocator &allocator)
    : StandardVectorBuffer(allocator, capacity_t(0), sizeof(string_t)), heap(AllocateHeap(allocator)) {
	buffer_type = VectorBufferType::STRING_BUFFER;
}

VectorStringBuffer::VectorStringBuffer(Allocator &allocator, capacity_t capacity)
    : StandardVectorBuffer(allocator, capacity, sizeof(string_t)) {
	buffer_type = VectorBufferType::STRING_BUFFER;
}

VectorStringBuffer::VectorStringBuffer(AllocatedData allocation, capacity_t capacity)
    : StandardVectorBuffer(std::move(allocation), capacity, sizeof(string_t)) {
	buffer_type = VectorBufferType::STRING_BUFFER;
}

VectorStringBuffer::VectorStringBuffer(capacity_t capacity) : StandardVectorBuffer(capacity, sizeof(string_t)) {
	buffer_type = VectorBufferType::STRING_BUFFER;
}

VectorStringBuffer::VectorStringBuffer(data_ptr_t data_ptr_p, count_t count)
    : StandardVectorBuffer(data_ptr_p, count, sizeof(string_t)) {
	buffer_type = VectorBufferType::STRING_BUFFER;
}

VectorStringBuffer::VectorStringBuffer(AllocatedData &&data_p, count_t count)
    : StandardVectorBuffer(std::move(data_p), count, sizeof(string_t)), heap(AllocateHeap()) {
	buffer_type = VectorBufferType::STRING_BUFFER;
}

VectorStringBuffer::VectorStringBuffer(AllocatedData &&data_p, count_t count, const VectorStringBuffer &other)
    : StandardVectorBuffer(std::move(data_p), count, sizeof(string_t)) {
	auto auxiliary_data = other.GetAuxiliaryData();
	if (auxiliary_data) {
		AddAuxiliaryData(make_uniq<AuxiliaryDataSetHolder>(std::move(auxiliary_data)));
	}
	buffer_type = VectorBufferType::STRING_BUFFER;
}

StringHeap &VectorStringBuffer::AllocateHeap(Allocator &allocator) {
	auto data = make_uniq<StringHeapHolder>(allocator);
	auto &result = data->heap;
	AddAuxiliaryData(std::move(data));
	return result;
}

StringHeap &VectorStringBuffer::AllocateHeap() {
	return AllocateHeap(Allocator::DefaultAllocator());
}

idx_t StringHeapHolder::GetAllocationSize() const {
	return heap.AllocationSize();
}

void VectorStringBuffer::CopyInternal(const Vector &source, const SelectionVector &source_sel, idx_t source_count,
                                      idx_t source_offset, idx_t target_offset, idx_t copy_count) {
	auto ldata = FlatVector::GetData<string_t>(source);
	auto tdata = reinterpret_cast<string_t *>(data_ptr);
	auto &append_heap = GetHeap();
	for (idx_t i = 0; i < copy_count; i++) {
		auto source_idx = source_sel.get_index(source_offset + i);
		auto target_idx = target_offset + i;
		if (!validity.RowIsValid(target_idx)) {
			continue;
		}
		tdata[target_idx] = append_heap.AddBlob(ldata[source_idx]);
	}
}

void VectorStringBuffer::SetValue(const LogicalType &type, idx_t index, const Value &val) {
	EnsureDataAllocated();
	if (!val.IsNull() && val.type() != type) {
		SetValue(type, index, val.DefaultCastAs(type));
		return;
	}
	validity.Set(index, !val.IsNull());
	if (!val.IsNull()) {
		reinterpret_cast<string_t *>(data_ptr)[index] = GetHeap().AddBlob(StringValue::Get(val));
	}
}

void VectorStringBuffer::VerifyInternal(const LogicalType &type, const SelectionVector &sel, idx_t count) const {
	StandardVectorBuffer::VerifyInternal(type, sel, count);

	D_ASSERT(type.InternalType() == PhysicalType::VARCHAR);
	auto data = reinterpret_cast<const string_t *>(data_ptr);
	for (idx_t i = 0; i < count; i++) {
		auto idx = sel.get_index(i);
		if (!validity.RowIsValid(idx)) {
			// NULL
			continue;
		}
		auto &str = data[idx];
		switch (type.id()) {
		case LogicalTypeId::BIT: {
			auto buf = str.GetData();
			if (idx_t(*buf) >= 8) {
				throw InternalException("Internal bit type inconsistency - padding bits should be < 8 but got %d",
				                        idx_t(*buf));
			}
			Bit::Verify(str);
			break;
		}
		case LogicalTypeId::BIGNUM:
			Bignum::Verify(static_cast<bignum_t>(str));
			break;
		case LogicalTypeId::VARCHAR:
			// verify that the string is correct unicode
			str.ForceVerify();
			break;
		default:
			break;
		}
	}
}

buffer_ptr<VectorBuffer> VectorStringBuffer::SliceInternal(const LogicalType &type, idx_t offset, idx_t end) {
	auto type_size = GetTypeIdSize(type.InternalType());
	auto offset_ptr = data_ptr ? data_ptr + type_size * offset : nullptr;
	auto count = count_t(end - offset);
	auto result = make_buffer<VectorStringBuffer>(offset_ptr, count);
	result->GetValidityMask().Slice(validity, offset, count);
	result->AddAuxiliaryData(make_uniq<VectorBufferHolder>(shared_from_this()));
	return result;
}

buffer_ptr<VectorBuffer> VectorStringBuffer::ConstantSliceInternal(const LogicalType &type, count_t count) {
	// a constant has one physical entry - the logical count is tracked separately from the physical capacity
	auto result = make_buffer<VectorStringBuffer>(data_ptr, count_t(1ULL));
	result->GetValidityMask().Set(0, validity.RowIsValid(0));
	result->SetVectorType(VectorType::CONSTANT_VECTOR);
	result->SetVectorSize(count);
	result->AddAuxiliaryData(make_uniq<VectorBufferHolder>(shared_from_this()));
	return result;
}

buffer_ptr<VectorBuffer> VectorStringBuffer::CreateBuffer(AllocatedData &&new_data, count_t count) const {
	return make_buffer<VectorStringBuffer>(std::move(new_data), count, *this);
}

buffer_ptr<VectorBuffer> VectorStringBuffer::FlattenSliceInternal(const LogicalType &type, const SelectionVector &sel,
                                                                  idx_t count) const {
	auto result = StandardVectorBuffer::FlattenSliceInternal(type, sel, count);
	if (!result) {
		// already flat - bail
		return nullptr;
	}
	// add heap reference from source to result
	if (auxiliary_data) {
		result->AddAuxiliaryData(make_uniq<AuxiliaryDataSetHolder>(auxiliary_data));
	}
	return result;
}

string_t StringVector::AddString(Vector &vector, const char *data, idx_t len) {
	return StringVector::AddString(vector, string_t(data, UnsafeNumericCast<uint32_t>(len)));
}

string_t StringVector::AddStringOrBlob(Vector &vector, const char *data, idx_t len) {
	return StringVector::AddStringOrBlob(vector, string_t(data, UnsafeNumericCast<uint32_t>(len)));
}

string_t StringVector::AddString(Vector &vector, const char *data) {
	return StringVector::AddString(vector, string_t(data, UnsafeNumericCast<uint32_t>(strlen(data))));
}

string_t StringVector::AddString(Vector &vector, const string &data) {
	return StringVector::AddString(vector, string_t(data.c_str(), UnsafeNumericCast<uint32_t>(data.size())));
}

VectorStringBuffer &StringVector::GetStringBuffer(Vector &vector) {
	if (vector.GetType().InternalType() != PhysicalType::VARCHAR) {
		throw InternalException("StringVector::GetStringBuffer - vector is not of internal type VARCHAR but of type %s",
		                        vector.GetType());
	}
	// check if the main buffer is a VectorStringBuffer
	if (!vector.GetBufferRef()) {
		vector.SetBuffer(make_buffer<VectorStringBuffer>(nullptr, count_t(0)));
	}
	if (vector.Buffer().GetBufferType() != VectorBufferType::STRING_BUFFER) {
		throw InternalException(
		    "StringVector::GetStringBuffer called on a vector - but that vector does NOT have a string buffer");
	}
	return vector.BufferMutable().Cast<VectorStringBuffer>();
}

ArenaAllocator &StringVector::GetStringAllocator(Vector &vector) {
	return GetStringBuffer(vector).GetStringAllocator();
}

StringHeap &StringVector::GetStringHeap(Vector &vector) {
	auto &string_buffer = GetStringBuffer(vector);
	return string_buffer.GetHeap();
}

string_t StringVector::AddString(Vector &vector, string_t data) {
	D_ASSERT(vector.GetType().id() == LogicalTypeId::VARCHAR || vector.GetType().id() == LogicalTypeId::BIT);
	auto &string_heap = GetStringHeap(vector);
	return string_heap.AddString(data);
}

string_t StringVector::AddStringOrBlob(Vector &vector, string_t data) {
	D_ASSERT(vector.GetType().InternalType() == PhysicalType::VARCHAR);
	return GetStringHeap(vector).AddBlob(data);
}

string_t StringVector::EmptyString(Vector &vector, idx_t len) {
	D_ASSERT(vector.GetType().InternalType() == PhysicalType::VARCHAR);
	auto &string_heap = GetStringHeap(vector);
	return string_heap.EmptyString(len);
}

void StringVector::AddAuxiliaryData(Vector &vector, unique_ptr<AuxiliaryDataHolder> data) {
	vector.AddAuxiliaryData(std::move(data));
}

void StringVector::AddHandle(Vector &vector, BufferHandle handle) {
	AddAuxiliaryData(vector, make_uniq<PinnedBufferHolder>(std::move(handle)));
}

void StringVector::AddHeapReference(Vector &vector, const Vector &other) {
	vector.AddHeapReference(other);
}

} // namespace duckdb
