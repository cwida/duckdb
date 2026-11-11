#include "duckdb/common/vector/array_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/map_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/common/operator/cast_operators.hpp"

#include "duckdb/common/exception/conversion_exception.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/operator/multiply.hpp"
#include "duckdb/common/types/arrow_aux_data.hpp"
#include "duckdb/common/types/arrow_string_view_type.hpp"
#include "duckdb/function/table/arrow.hpp"

#include "duckdb/common/bswap.hpp"

namespace duckdb {

#if STANDARD_VECTOR_SIZE > 64
static void ShiftRight(unsigned char *ar, int size, int shift) {
	int carry = 0;
	while (shift--) {
		for (int i = size - 1; i >= 0; --i) {
			int next = (ar[i] & 1) ? 0x80 : 0;
			ar[i] = UnsafeNumericCast<unsigned char>(carry | (ar[i] >> 1));
			carry = next;
		}
	}
}
#endif

idx_t GetEffectiveOffset(const ArrowArray &array, int64_t parent_offset, idx_t chunk_offset,
                         int64_t nested_offset = -1) {
	if (nested_offset != -1) {
		// The parent of this array is a list
		// We just ignore the parent offset, it's already applied to the list
		return UnsafeNumericCast<idx_t>(array.offset + nested_offset);
	}
	// Parent offset is set in the case of a struct, it applies to all child arrays
	// 'chunk_offset' is how much of the chunk we've already scanned, in case the chunk size exceeds
	// STANDARD_VECTOR_SIZE
	return UnsafeNumericCast<idx_t>(array.offset + parent_offset) + chunk_offset;
}

//! The children of a struct or a sparse union share its slots, so below a list they start at its offset too
static int64_t GetChildNestedOffset(const ArrowArray &array, int64_t nested_offset) {
	return nested_offset == -1 ? -1 : nested_offset + array.offset;
}

template <class T>
T *ArrowBufferData(ArrowArray &array, idx_t buffer_idx) {
	return (T *)array.buffers[buffer_idx]; // NOLINT
}

static bool IsAligned(const_data_ptr_t ptr, idx_t alignment) {
	return reinterpret_cast<uintptr_t>(ptr) % alignment == 0;
}

//! Arrow buffers are not required to be aligned to their value type, and reading an unaligned buffer through a
//! typed pointer is undefined behavior. This reads the values in place when they are aligned, and from an aligned
//! copy of the [offset, offset + count) range when they are not.
template <class T>
class ArrowAlignedBufferData {
public:
	ArrowAlignedBufferData(const ArrowArray &array, idx_t buffer_idx, idx_t offset, idx_t count) {
		auto source = static_cast<const_data_ptr_t>(array.buffers[buffer_idx]) + offset * sizeof(T);
		if (IsAligned(source, alignof(T))) {
			data = reinterpret_cast<const T *>(source);
			return;
		}
		copy = make_unsafe_uniq_array_uninitialized<T>(count);
		memcpy(static_cast<void *>(copy.get()), source, count * sizeof(T));
		data = copy.get();
	}

	const T &operator[](idx_t idx) const {
		return data[idx];
	}
	const T *get() const {
		return data;
	}

private:
	const T *data;
	unsafe_unique_array<T> copy;
};

//! Points the vector to the values when they are aligned to their type, and copies them into it when they are not
static void SetVectorData(Vector &vector, data_ptr_t data, idx_t size) {
	auto type_size = GetTypeIdSize(vector.GetType().InternalType());
	if (IsAligned(data, MinValue<idx_t>(type_size, sizeof(uint64_t)))) {
		FlatVector::SetData(vector, data, count_t(size));
		return;
	}
	memcpy(FlatVector::GetDataMutable(vector), data, type_size * size);
	FlatVector::SetSize(vector, size);
}

static void GetValidityMask(ValidityMask &mask, ArrowArray &array, idx_t chunk_offset, idx_t size,
                            int64_t parent_offset, int64_t nested_offset = -1, bool add_null = false) {
	// In certain we don't need to or cannot copy arrow's validity mask to duckdb.
	//
	// The conditions where we do want to copy arrow's mask to duckdb are:
	// 1. nulls exist
	// 2. n_buffers > 0, meaning the array's arrow type is not `null`
	// 3. the validity buffer (the first buffer) is not a nullptr
	if (array.null_count != 0 && array.n_buffers > 0 && array.buffers[0]) {
		auto bit_offset = GetEffectiveOffset(array, parent_offset, chunk_offset, nested_offset);
		mask.EnsureWritable();
#if STANDARD_VECTOR_SIZE > 64 && !DUCKDB_IS_BIG_ENDIAN
		auto n_bitmask_bytes = (size + 8 - 1) / 8;
		if (bit_offset % 8 == 0) {
			//! just memcpy nullmask
			memcpy((void *)mask.GetData(), ArrowBufferData<uint8_t>(array, 0) + bit_offset / 8, n_bitmask_bytes);
		} else {
			//! need to re-align nullmask - only the bytes that hold the bits of the rows are guaranteed to exist
			vector<uint8_t> temp_nullmask(n_bitmask_bytes + 1, 0);
			auto n_source_bytes = (bit_offset % 8 + size + 8 - 1) / 8;
			memcpy(temp_nullmask.data(), ArrowBufferData<uint8_t>(array, 0) + bit_offset / 8, n_source_bytes);
			ShiftRight(temp_nullmask.data(), NumericCast<int>(n_bitmask_bytes + 1),
			           NumericCast<int>(bit_offset % 8ull)); //! why this has to be a right shift is a mystery to me
			memcpy((void *)mask.GetData(), data_ptr_cast(temp_nullmask.data()), n_bitmask_bytes);
		}
#else
		auto byte_offset = bit_offset / 8;
		auto source_data = ArrowBufferData<uint8_t>(array, 0);
		bit_offset %= 8;
		for (idx_t i = 0; i < size; i++) {
			mask.Set(i, source_data[byte_offset] & (1 << bit_offset));
			bit_offset++;
			if (bit_offset == 8) {
				bit_offset = 0;
				byte_offset++;
			}
		}
#endif
	}
	if (add_null) {
		//! We are setting a validity mask of the data part of dictionary vector
		//! For some reason, Nulls are allowed to be indexes, hence we need to set the last element here to be null
		//! We might have to resize the mask
		mask.Resize(size + 1);
		mask.SetInvalid(size);
	}
}

void ArrowToDuckDBConversion::SetValidityMask(Vector &vector, ArrowArray &array, idx_t chunk_offset, idx_t size,
                                              int64_t parent_offset, int64_t nested_offset, bool add_null) {
	auto &mask = FlatVector::ValidityMutable(vector);
	GetValidityMask(mask, array, chunk_offset, size, parent_offset, nested_offset, add_null);
}

namespace {

struct ArrowListOffsetData {
	idx_t list_size = 0;
	idx_t start_offset = 0;
};

} // namespace

template <class BUFFER_TYPE>
static ArrowListOffsetData ConvertArrowListOffsetsTemplated(Vector &vector, ArrowArray &array, idx_t size,
                                                            idx_t effective_offset) {
	ArrowListOffsetData result;
	auto &start_offset = result.start_offset;
	auto &list_size = result.list_size;

	if (size == 0) {
		start_offset = 0;
		list_size = 0;
		return result;
	}

	idx_t cur_offset = 0;
	ArrowAlignedBufferData<BUFFER_TYPE> offsets(array, 1, effective_offset, size + 1);
	start_offset = offsets[0];
	auto list_data = FlatVector::Writer<list_entry_t>(vector, size);
	for (idx_t i = 0; i < size; i++) {
		list_entry_t le;
		le.offset = cur_offset;
		le.length = offsets[i + 1] - offsets[i];
		cur_offset += le.length;
		list_data.WriteValue(le);
	}
	list_size = offsets[size];
	list_size -= start_offset;
	return result;
}

template <class BUFFER_TYPE>
static ArrowListOffsetData ConvertArrowListViewOffsetsTemplated(Vector &vector, ArrowArray &array, idx_t size,
                                                                idx_t effective_offset) {
	ArrowListOffsetData result;
	auto &start_offset = result.start_offset;
	auto &list_size = result.list_size;

	list_size = 0;
	ArrowAlignedBufferData<BUFFER_TYPE> offsets(array, 1, effective_offset, size);
	ArrowAlignedBufferData<BUFFER_TYPE> sizes(array, 2, effective_offset, size);

	// In ListArrays the offsets have to be sequential
	// ListViewArrays do not have this same constraint
	// for that reason we need to keep track of the lowest offset, so we can skip all the data that comes before it
	// when we scan the child data

	bool has_non_empty_entry = false;
	BUFFER_TYPE lowest_offset = 0;
	BUFFER_TYPE highest_offset = 0;
	auto list_data = FlatVector::GetDataMutable<list_entry_t>(vector);
	for (idx_t i = 0; i < size; i++) {
		auto &le = list_data[i];
		le.offset = offsets[i];
		le.length = sizes[i];
		if (le.length != 0) {
			auto end_offset = offsets[i] + sizes[i];
			if (!has_non_empty_entry) {
				lowest_offset = offsets[i];
				highest_offset = end_offset;
				has_non_empty_entry = true;
			} else {
				lowest_offset = MinValue(lowest_offset, offsets[i]);
				highest_offset = MaxValue(highest_offset, end_offset);
			}
		}
	}
	start_offset = has_non_empty_entry ? lowest_offset : 0;
	list_size = has_non_empty_entry ? highest_offset - lowest_offset : 0;
	for (idx_t i = 0; i < size; i++) {
		auto &le = list_data[i];
		le.offset = le.length == 0 ? 0 : le.offset - start_offset;
	}
	return result;
}

static ArrowListOffsetData ConvertArrowListOffsets(Vector &vector, ArrowArray &array, idx_t size,
                                                   const ArrowType &arrow_type, idx_t effective_offset) {
	auto &list_info = arrow_type.GetTypeInfo<ArrowListInfo>();
	auto size_type = list_info.GetSizeType();
	if (list_info.IsView()) {
		if (size_type == ArrowVariableSizeType::NORMAL) {
			return ConvertArrowListViewOffsetsTemplated<uint32_t>(vector, array, size, effective_offset);
		} else {
			D_ASSERT(size_type == ArrowVariableSizeType::SUPER_SIZE);
			return ConvertArrowListViewOffsetsTemplated<uint64_t>(vector, array, size, effective_offset);
		}
	} else {
		if (size_type == ArrowVariableSizeType::NORMAL) {
			return ConvertArrowListOffsetsTemplated<uint32_t>(vector, array, size, effective_offset);
		} else {
			D_ASSERT(size_type == ArrowVariableSizeType::SUPER_SIZE);
			return ConvertArrowListOffsetsTemplated<uint64_t>(vector, array, size, effective_offset);
		}
	}
}

static void ArrowToDuckDBList(Vector &vector, ArrowArray &array, idx_t chunk_offset, ArrowArrayScanState &array_state,
                              idx_t size, const ArrowType &arrow_type, int64_t nested_offset,
                              const ValidityMask *parent_mask, int64_t parent_offset) {
	auto &list_info = arrow_type.GetTypeInfo<ArrowListInfo>();
	ArrowToDuckDBConversion::SetValidityMask(vector, array, chunk_offset, size, parent_offset, nested_offset);

	auto effective_offset = GetEffectiveOffset(array, parent_offset, chunk_offset, nested_offset);
	auto list_data = ConvertArrowListOffsets(vector, array, size, arrow_type, effective_offset);
	auto &start_offset = list_data.start_offset;
	auto &list_size = list_data.list_size;

	ListVector::Reserve(vector, list_size);
	ListVector::SetListSize(vector, list_size);
	auto &child_vector = ListVector::GetChildMutable(vector);
	ArrowToDuckDBConversion::SetValidityMask(child_vector, *array.children[0], chunk_offset, list_size, array.offset,
	                                         NumericCast<int64_t>(start_offset));
	auto &list_mask = FlatVector::ValidityMutable(vector);
	if (parent_mask) {
		//! Since this List is owned by a struct we must guarantee their validity map matches on Null
		if (parent_mask->CanHaveNull()) {
			for (idx_t i = 0; i < size; i++) {
				if (!parent_mask->RowIsValid(i)) {
					list_mask.SetInvalid(i);
				}
			}
		}
	}
	auto &child_state = array_state.GetChild(0);
	auto &child_array = *array.children[0];
	auto &child_type = list_info.GetChild();

	auto array_physical_type = child_type.GetPhysicalType();
	switch (array_physical_type) {
	case ArrowArrayPhysicalType::DICTIONARY_ENCODED:
		// TODO: add support for offsets
		ArrowToDuckDBConversion::ColumnArrowToDuckDBDictionary(child_vector, child_array, chunk_offset, child_state,
		                                                       list_size, child_type,
		                                                       NumericCast<int64_t>(start_offset));
		break;
	case ArrowArrayPhysicalType::RUN_END_ENCODED:
		ArrowToDuckDBConversion::ColumnArrowToDuckDBRunEndEncoded(child_vector, child_array, chunk_offset, child_state,
		                                                          list_size, child_type,
		                                                          NumericCast<int64_t>(start_offset));
		break;
	case ArrowArrayPhysicalType::DEFAULT:
		ArrowToDuckDBConversion::ColumnArrowToDuckDB(child_vector, child_array, chunk_offset, child_state, list_size,
		                                             child_type, NumericCast<int64_t>(start_offset));
		break;
	default:
		throw NotImplementedException("ArrowArrayPhysicalType not recognized");
	}
}

static void ArrowToDuckDBArray(Vector &vector, ArrowArray &array, idx_t chunk_offset, ArrowArrayScanState &array_state,
                               idx_t size, const ArrowType &arrow_type, int64_t nested_offset,
                               const ValidityMask *parent_mask, int64_t parent_offset) {
	auto &array_info = arrow_type.GetTypeInfo<ArrowArrayInfo>();
	auto array_size = array_info.FixedSize();
	auto child_count = array_size * size;
	auto child_offset = GetEffectiveOffset(array, parent_offset, chunk_offset, nested_offset) * array_size;

	ArrowToDuckDBConversion::SetValidityMask(vector, array, chunk_offset, size, parent_offset, nested_offset);

	auto &child_vector = ArrayVector::GetChildMutable(vector);
	ArrowToDuckDBConversion::SetValidityMask(child_vector, *array.children[0], chunk_offset, child_count, array.offset,
	                                         NumericCast<int64_t>(child_offset));

	auto &array_mask = FlatVector::ValidityMutable(vector);
	if (parent_mask) {
		//! Since this List is owned by a struct we must guarantee their validity map matches on Null
		if (parent_mask->CanHaveNull()) {
			for (idx_t i = 0; i < size; i++) {
				if (!parent_mask->RowIsValid(i)) {
					array_mask.SetInvalid(i);
				}
			}
		}
	}

	// Broadcast the validity mask to the child vector
	auto broadcast_validity = [&]() {
		if (!array_mask.CanHaveNull()) {
			return;
		}
		auto &child_validity_mask = FlatVector::ValidityMutable(child_vector);
		for (idx_t i = 0; i < size; i++) {
			if (!array_mask.RowIsValid(i)) {
				for (idx_t j = 0; j < array_size; j++) {
					child_validity_mask.SetInvalid(i * array_size + j);
				}
			}
		}
	};
	broadcast_validity();

	auto &child_state = array_state.GetChild(0);
	auto &child_array = *array.children[0];
	auto &child_type = array_info.GetChild();
	bool run_end_encoded = child_type.GetPhysicalType() == ArrowArrayPhysicalType::RUN_END_ENCODED;
	if (child_count == 0 && child_offset == 0 && !run_end_encoded) {
		D_ASSERT(!child_array.dictionary);
		ArrowToDuckDBConversion::ColumnArrowToDuckDB(child_vector, child_array, chunk_offset, child_state, child_count,
		                                             child_type, -1);
	} else {
		if (child_array.dictionary) {
			ArrowToDuckDBConversion::ColumnArrowToDuckDBDictionary(child_vector, child_array, chunk_offset, child_state,
			                                                       child_count, child_type,
			                                                       NumericCast<int64_t>(child_offset));
		} else if (run_end_encoded) {
			ArrowToDuckDBConversion::ColumnArrowToDuckDBRunEndEncoded(child_vector, child_array, chunk_offset,
			                                                          child_state, child_count, child_type,
			                                                          NumericCast<int64_t>(child_offset));
			// decoding sets the validity of every child row again
			broadcast_validity();
		} else {
			// a nested list or array sets its validity again, so it takes the broadcast NULLs as its parent mask
			ValidityMask child_parent_mask;
			if (array_mask.CanHaveNull()) {
				child_parent_mask.Copy(FlatVector::Validity(child_vector), child_count);
			}
			ArrowToDuckDBConversion::ColumnArrowToDuckDB(child_vector, child_array, chunk_offset, child_state,
			                                             child_count, child_type, NumericCast<int64_t>(child_offset),
			                                             array_mask.CanHaveNull() ? &child_parent_mask : nullptr);
		}
	}
}

static void ArrowToDuckDBMapVerify(const Vector &vector, idx_t count) {
	auto valid_check = MapVector::CheckMapValidity(vector, count);
	switch (valid_check) {
	case MapInvalidReason::VALID:
		break;
	case MapInvalidReason::DUPLICATE_KEY: {
		throw InvalidInputException("Arrow map contains duplicate key, which isn't supported by DuckDB map type");
	}
	case MapInvalidReason::NULL_KEY: {
		throw InvalidInputException("Arrow map contains NULL as map key, which isn't supported by DuckDB map type");
	}
	default: {
		throw InternalException("MapInvalidReason not implemented");
	}
	}
}

template <class T>
static void SetVectorString(Vector &vector, idx_t size, char *cdata, const T *offsets) {
	auto strings = FlatVector::GetDataMutable<string_t>(vector);
	for (idx_t row_idx = 0; row_idx < size; row_idx++) {
		if (FlatVector::IsNull(vector, row_idx)) {
			continue;
		}
		if (offsets[row_idx + 1] < offsets[row_idx]) {
			throw ConversionException("Arrow string offsets must be non-decreasing");
		}
		auto cptr = cdata + offsets[row_idx];
		auto str_len = offsets[row_idx + 1] - offsets[row_idx];
		if (str_len > NumericLimits<uint32_t>::Maximum()) { // LCOV_EXCL_START
			throw ConversionException("DuckDB does not support Strings over 4GB");
		} // LCOV_EXCL_STOP
		strings[row_idx] = string_t(cptr, UnsafeNumericCast<uint32_t>(str_len));
	}
}

static void SetVectorStringView(Vector &vector, idx_t size, ArrowArray &array, idx_t current_pos) {
	auto strings = FlatVector::Writer<string_t>(vector, size);
	ArrowAlignedBufferData<arrow_string_view_t> arrow_string(array, 1, current_pos, size);

	for (idx_t row_idx = 0; row_idx < size; row_idx++) {
		if (FlatVector::IsNull(vector, row_idx)) {
			strings.WriteNull();
			continue;
		}
		auto length = UnsafeNumericCast<uint32_t>(arrow_string[row_idx].Length());
		if (arrow_string[row_idx].IsInline()) {
			//	This string is inlined
			//  | Bytes 0-3  | Bytes 4-15                            |
			//  |------------|---------------------------------------|
			//  | length     | data (padded with 0)                  |
			strings.WriteValue(string_t(arrow_string[row_idx].GetInlineData(), length));
		} else {
			//  This string is not inlined, we have to check a different buffer and offsets
			//  | Bytes 0-3  | Bytes 4-7  | Bytes 8-11 | Bytes 12-15 |
			//  |------------|------------|------------|-------------|
			//  | length     | prefix     | buf. index | offset      |
			auto buffer_index = UnsafeNumericCast<uint32_t>(arrow_string[row_idx].GetBufferIndex());
			int32_t offset = arrow_string[row_idx].GetOffset();
			D_ASSERT(array.n_buffers > 2 + buffer_index);
			auto c_data = ArrowBufferData<char>(array, 2 + buffer_index);
			strings.WriteStringRef(string_t(&c_data[offset], length));
		}
	}
}

static void DirectConversion(Vector &vector, ArrowArray &array, idx_t chunk_offset, int64_t nested_offset,
                             uint64_t parent_offset, idx_t size) {
	auto internal_type = GetTypeIdSize(vector.GetType().InternalType());
	auto data_ptr =
	    ArrowBufferData<data_t>(array, 1) +
	    internal_type * GetEffectiveOffset(array, NumericCast<int64_t>(parent_offset), chunk_offset, nested_offset);
	SetVectorData(vector, data_ptr, size);
}

template <class T>
static void TimeConversion(Vector &vector, ArrowArray &array, idx_t chunk_offset, int64_t nested_offset,
                           int64_t parent_offset, idx_t size, int64_t conversion) {
	auto tgt_writer = FlatVector::Writer<dtime_t>(vector, size);
	auto &validity_mask = FlatVector::ValidityMutable(vector);
	ArrowAlignedBufferData<T> src_ptr(array, 1, GetEffectiveOffset(array, parent_offset, chunk_offset, nested_offset),
	                                  size);
	if (validity_mask.CannotHaveNull()) {
		for (idx_t row = 0; row < size; row++) {
			int64_t result;
			if (!TryMultiplyOperator::Operation(static_cast<int64_t>(src_ptr[row]), conversion, result)) {
				throw ConversionException("Could not convert Time to Microsecond");
			}
			tgt_writer.WriteValue(dtime_t(result));
		}
	} else {
		for (idx_t row = 0; row < size; row++) {
			if (!validity_mask.RowIsValid(row)) {
				tgt_writer.WriteNull();
				continue;
			}
			int64_t result;
			if (!TryMultiplyOperator::Operation(static_cast<int64_t>(src_ptr[row]), conversion, result)) {
				throw ConversionException("Could not convert Time to Microsecond");
			}
			tgt_writer.WriteValue(dtime_t(result));
		}
	}
}

template <class T>
static void TimeNSConversion(Vector &vector, ArrowArray &array, idx_t chunk_offset, int64_t nested_offset,
                             int64_t parent_offset, idx_t size, int64_t conversion) {
	auto tgt_ptr = FlatVector::GetDataMutable<dtime_ns_t>(vector);
	auto &validity_mask = FlatVector::ValidityMutable(vector);
	ArrowAlignedBufferData<T> src_ptr(array, 1, GetEffectiveOffset(array, parent_offset, chunk_offset, nested_offset),
	                                  size);
	if (validity_mask.CannotHaveNull()) {
		for (idx_t row = 0; row < size; row++) {
			// dtime_ns_t.micros actually holds nanos (!)
			if (!TryMultiplyOperator::Operation(static_cast<int64_t>(src_ptr[row]), conversion, tgt_ptr[row].value)) {
				throw ConversionException("Could not convert TimeNS to Nanoseconds");
			}
		}
	} else {
		for (idx_t row = 0; row < size; row++) {
			if (!validity_mask.RowIsValid(row)) {
				continue;
			}
			// dtime_ns_t.micros actually holds nanos (!)
			if (!TryMultiplyOperator::Operation(static_cast<int64_t>(src_ptr[row]), conversion, tgt_ptr[row].value)) {
				throw ConversionException("Could not convert TimeNS to Nanoseconds");
			}
		}
	}
}

static void UUIDConversion(Vector &vector, const ArrowArray &array, idx_t chunk_offset, int64_t nested_offset,
                           int64_t parent_offset, idx_t size) {
	auto tgt_ptr = FlatVector::GetDataMutable<hugeint_t>(vector);
	auto &validity_mask = FlatVector::ValidityMutable(vector);
	ArrowAlignedBufferData<hugeint_t> src_ptr(
	    array, 1, GetEffectiveOffset(array, parent_offset, chunk_offset, nested_offset), size);
	if (validity_mask.CannotHaveNull()) {
		for (idx_t row = 0; row < size; row++) {
			tgt_ptr[row].lower = static_cast<uint64_t>(BSwapIfLE(src_ptr[row].upper));
			// flip Upper MSD
			tgt_ptr[row].upper = static_cast<int64_t>(static_cast<uint64_t>(BSwapIfLE(src_ptr[row].lower)) ^
			                                          (static_cast<uint64_t>(1) << 63));
		}
	} else {
		for (idx_t row = 0; row < size; row++) {
			if (!validity_mask.RowIsValid(row)) {
				continue;
			}
			tgt_ptr[row].lower = static_cast<uint64_t>(BSwapIfLE(src_ptr[row].upper));
			// flip Upper MSD
			tgt_ptr[row].upper = static_cast<int64_t>(static_cast<uint64_t>(BSwapIfLE(src_ptr[row].lower)) ^
			                                          (static_cast<uint64_t>(1) << 63));
		}
	}
}

static void TimestampTZConversion(Vector &vector, ArrowArray &array, idx_t chunk_offset, int64_t nested_offset,
                                  int64_t parent_offset, idx_t size, int64_t conversion) {
	auto tgt_ptr = FlatVector::GetDataMutable<timestamp_t>(vector);
	auto &validity_mask = FlatVector::ValidityMutable(vector);
	ArrowAlignedBufferData<int64_t> src_ptr(
	    array, 1, GetEffectiveOffset(array, parent_offset, chunk_offset, nested_offset), size);
	if (validity_mask.CannotHaveNull()) {
		for (idx_t row = 0; row < size; row++) {
			if (!TryMultiplyOperator::Operation(src_ptr[row], conversion, tgt_ptr[row].value)) {
				throw ConversionException("Could not convert TimestampTZ to Microsecond");
			}
		}
	} else {
		for (idx_t row = 0; row < size; row++) {
			if (!validity_mask.RowIsValid(row)) {
				continue;
			}
			if (!TryMultiplyOperator::Operation(src_ptr[row], conversion, tgt_ptr[row].value)) {
				throw ConversionException("Could not convert TimestampTZ to Microsecond");
			}
		}
	}
}

static void IntervalConversionUs(Vector &vector, ArrowArray &array, idx_t chunk_offset, int64_t nested_offset,
                                 int64_t parent_offset, idx_t size, int64_t conversion) {
	auto tgt_ptr = FlatVector::GetDataMutable<interval_t>(vector);
	ArrowAlignedBufferData<int64_t> src_ptr(
	    array, 1, GetEffectiveOffset(array, parent_offset, chunk_offset, nested_offset), size);
	for (idx_t row = 0; row < size; row++) {
		tgt_ptr[row].days = 0;
		tgt_ptr[row].months = 0;
		if (!TryMultiplyOperator::Operation(src_ptr[row], conversion, tgt_ptr[row].micros)) {
			throw ConversionException("Could not convert Interval to Microsecond");
		}
	}
}

static void IntervalConversionMonths(Vector &vector, ArrowArray &array, idx_t chunk_offset, int64_t nested_offset,
                                     int64_t parent_offset, idx_t size) {
	auto tgt_ptr = FlatVector::GetDataMutable<interval_t>(vector);
	ArrowAlignedBufferData<int32_t> src_ptr(
	    array, 1, GetEffectiveOffset(array, parent_offset, chunk_offset, nested_offset), size);
	for (idx_t row = 0; row < size; row++) {
		tgt_ptr[row].days = 0;
		tgt_ptr[row].micros = 0;
		tgt_ptr[row].months = src_ptr[row];
	}
}

static void IntervalConversionMonthDayNanos(Vector &vector, ArrowArray &array, idx_t chunk_offset,
                                            int64_t nested_offset, int64_t parent_offset, idx_t size) {
	auto tgt_ptr = FlatVector::GetDataMutable<interval_t>(vector);
	ArrowAlignedBufferData<ArrowInterval> src_ptr(
	    array, 1, GetEffectiveOffset(array, parent_offset, chunk_offset, nested_offset), size);
	for (idx_t row = 0; row < size; row++) {
		tgt_ptr[row].days = src_ptr[row].days;
		tgt_ptr[row].micros = src_ptr[row].nanoseconds / Interval::NANOS_PER_MICRO;
		tgt_ptr[row].months = src_ptr[row].months;
	}
}

// Find the index of the first run-end that is strictly greater than the offset.
// count is returned if no such run-end is found.
template <class RUN_END_TYPE>
static idx_t FindRunIndex(const VectorIterator<RUN_END_TYPE> &run_ends, idx_t count, idx_t offset) {
	// Binary-search within the [0, count) range. For example:
	// [0, 0, 0, 1, 1, 2] encoded as
	// run_ends: [3, 5, 6]:
	// 0, 1, 2 -> 0
	//    3, 4 -> 1
	//       5 -> 2
	// 6, 7 .. -> 3 (3 == count [not found])
	idx_t begin = 0;
	idx_t end = count;
	while (begin < end) {
		idx_t middle = (begin + end) / 2;
		// begin < end implies middle < end
		if (offset >= static_cast<idx_t>(run_ends[middle].GetValue())) {
			// keep searching in [middle + 1, end)
			begin = middle + 1;
		} else {
			// offset < run_ends[middle], so keep searching in [begin, middle)
			end = middle;
		}
	}
	return begin;
}

template <class RUN_END_TYPE, class VALUE_TYPE>
static void FlattenRunEnds(Vector &result, ArrowRunEndEncodingState &run_end_encoding, idx_t compressed_size,
                           idx_t scan_offset, idx_t count) {
	auto &runs = *run_end_encoding.run_ends;
	auto &values = *run_end_encoding.values;

	auto run_ends_data = runs.Values<RUN_END_TYPE>();
	auto values_data = values.Values<VALUE_TYPE>();
	auto result_data = FlatVector::ScatterWriter<VALUE_TYPE>(result);
	auto &validity = FlatVector::ValidityMutable(result);

	// According to the arrow spec, the 'run_ends' array is always valid
	// so we will assume this is true and not check the validity map

	// Now construct the result vector from the run_ends and the values

	auto run = FindRunIndex(run_ends_data, compressed_size, scan_offset);
	idx_t logical_index = scan_offset;
	idx_t index = 0;
	if (!values_data.CanHaveNull()) {
		// None of the compressed values are NULL
		for (; run < compressed_size; ++run) {
			auto run_end_entry = run_ends_data[run];
			auto value_entry = values_data[run];
			auto &value = value_entry.GetValue();
			auto run_end = static_cast<idx_t>(run_end_entry.GetValueUnsafe());

			D_ASSERT(run_end > (logical_index + index));
			auto to_scan = run_end - (logical_index + index);
			// Cap the amount to scan so we don't go over size
			to_scan = MinValue<idx_t>(to_scan, (count - index));

			for (idx_t i = 0; i < to_scan; i++) {
				result_data[index + i] = value;
			}
			index += to_scan;
			if (index >= count) {
				if (logical_index + index >= run_end) {
					// The last run was completed, forward the run index
					++run;
				}
				break;
			}
		}
	} else {
		for (; run < compressed_size; ++run) {
			auto run_end_entry = run_ends_data[run];
			auto value_entry = values_data[run];
			auto run_end = static_cast<idx_t>(run_end_entry.GetValueUnsafe());

			D_ASSERT(run_end > (logical_index + index));
			auto to_scan = run_end - (logical_index + index);
			// Cap the amount to scan so we don't go over size
			to_scan = MinValue<idx_t>(to_scan, (count - index));

			if (value_entry.IsValid()) {
				auto &value = value_entry.GetValue();
				for (idx_t i = 0; i < to_scan; i++) {
					result_data[index + i] = value;
					validity.SetValid(index + i);
				}
			} else {
				for (idx_t i = 0; i < to_scan; i++) {
					validity.SetInvalid(index + i);
				}
			}
			index += to_scan;
			if (index >= count) {
				if (logical_index + index >= run_end) {
					// The last run was completed, forward the run index
					++run;
				}
				break;
			}
		}
	}
}

template <class RUN_END_TYPE>
static void FlattenRunEndsSwitch(Vector &result, ArrowRunEndEncodingState &run_end_encoding, idx_t compressed_size,
                                 idx_t scan_offset, idx_t size) {
	auto &values = *run_end_encoding.values;
	auto physical_type = values.GetType().InternalType();

	switch (physical_type) {
	case PhysicalType::INT8:
		FlattenRunEnds<RUN_END_TYPE, int8_t>(result, run_end_encoding, compressed_size, scan_offset, size);
		break;
	case PhysicalType::INT16:
		FlattenRunEnds<RUN_END_TYPE, int16_t>(result, run_end_encoding, compressed_size, scan_offset, size);
		break;
	case PhysicalType::INT32:
		FlattenRunEnds<RUN_END_TYPE, int32_t>(result, run_end_encoding, compressed_size, scan_offset, size);
		break;
	case PhysicalType::INT64:
		FlattenRunEnds<RUN_END_TYPE, int64_t>(result, run_end_encoding, compressed_size, scan_offset, size);
		break;
	case PhysicalType::INT128:
		FlattenRunEnds<RUN_END_TYPE, hugeint_t>(result, run_end_encoding, compressed_size, scan_offset, size);
		break;
	case PhysicalType::UINT8:
		FlattenRunEnds<RUN_END_TYPE, uint8_t>(result, run_end_encoding, compressed_size, scan_offset, size);
		break;
	case PhysicalType::UINT16:
		FlattenRunEnds<RUN_END_TYPE, uint16_t>(result, run_end_encoding, compressed_size, scan_offset, size);
		break;
	case PhysicalType::UINT32:
		FlattenRunEnds<RUN_END_TYPE, uint32_t>(result, run_end_encoding, compressed_size, scan_offset, size);
		break;
	case PhysicalType::UINT64:
		FlattenRunEnds<RUN_END_TYPE, uint64_t>(result, run_end_encoding, compressed_size, scan_offset, size);
		break;
	case PhysicalType::BOOL:
		FlattenRunEnds<RUN_END_TYPE, bool>(result, run_end_encoding, compressed_size, scan_offset, size);
		break;
	case PhysicalType::FLOAT:
		FlattenRunEnds<RUN_END_TYPE, float>(result, run_end_encoding, compressed_size, scan_offset, size);
		break;
	case PhysicalType::DOUBLE:
		FlattenRunEnds<RUN_END_TYPE, double>(result, run_end_encoding, compressed_size, scan_offset, size);
		break;
	case PhysicalType::INTERVAL:
		FlattenRunEnds<RUN_END_TYPE, interval_t>(result, run_end_encoding, compressed_size, scan_offset, size);
		break;
	case PhysicalType::VARCHAR: {
		// Share the string heap, we don't need to allocate new strings, we just reference the existing ones
		StringVector::AddHeapReference(result, values);
		FlattenRunEnds<RUN_END_TYPE, string_t>(result, run_end_encoding, compressed_size, scan_offset, size);
		break;
	}
	default:
		throw NotImplementedException("RunEndEncoded value type '%s' not supported yet", TypeIdToString(physical_type));
	}
}

void ArrowToDuckDBConversion::ColumnArrowToDuckDBRunEndEncoded(Vector &vector, const ArrowArray &array,
                                                               idx_t chunk_offset, ArrowArrayScanState &array_state,
                                                               idx_t size, const ArrowType &arrow_type,
                                                               int64_t nested_offset, ValidityMask *parent_mask,
                                                               uint64_t parent_offset) {
	// Scan the 'run_ends' array
	D_ASSERT(array.n_children == 2);
	auto &run_ends_array = *array.children[0];
	auto &values_array = *array.children[1];

	auto &struct_info = arrow_type.GetTypeInfo<ArrowStructInfo>();
	auto &run_ends_type = struct_info.GetChild(0);
	auto &values_type = struct_info.GetChild(1);
	D_ASSERT(vector.GetType() == values_type.GetDuckType());

	if (vector.GetBufferRef()) {
		vector.BufferMutable().AddAuxiliaryData(make_uniq<ArrowAuxiliaryData>(array_state.owned_data));
	}

	D_ASSERT(run_ends_array.length == values_array.length);
	auto compressed_size = NumericCast<idx_t>(run_ends_array.length);
	// Create a vector for the run ends and the values
	auto &run_end_encoding = array_state.RunEndEncoding();
	if (!run_end_encoding.run_ends) {
		// The run ends and values have not been scanned yet for this array
		D_ASSERT(!run_end_encoding.values);
		run_end_encoding.run_ends = make_uniq<Vector>(run_ends_type.GetDuckType(), compressed_size);
		run_end_encoding.values = make_uniq<Vector>(values_type.GetDuckType(), compressed_size);

		// the run ends and values are scanned in full - the offsets of the scan apply to the logical (decoded) rows
		ArrowToDuckDBConversion::ColumnArrowToDuckDB(*run_end_encoding.run_ends, run_ends_array, 0, array_state,
		                                             compressed_size, run_ends_type);
		auto &values = *run_end_encoding.values;
		ArrowToDuckDBConversion::SetValidityMask(values, values_array, 0, compressed_size, 0, -1);
		ArrowToDuckDBConversion::ColumnArrowToDuckDB(values, values_array, 0, array_state, compressed_size,
		                                             values_type);
	}

	idx_t scan_offset = GetEffectiveOffset(array, NumericCast<int64_t>(parent_offset), chunk_offset, nested_offset);
	auto physical_type = run_ends_type.GetDuckType().InternalType();
	switch (physical_type) {
	case PhysicalType::INT16:
		FlattenRunEndsSwitch<int16_t>(vector, run_end_encoding, compressed_size, scan_offset, size);
		break;
	case PhysicalType::INT32:
		FlattenRunEndsSwitch<int32_t>(vector, run_end_encoding, compressed_size, scan_offset, size);
		break;
	case PhysicalType::INT64:
		FlattenRunEndsSwitch<int64_t>(vector, run_end_encoding, compressed_size, scan_offset, size);
		break;
	default:
		throw NotImplementedException("Type '%s' not implemented for RunEndEncoding", TypeIdToString(physical_type));
	}
}
template <class SRC>
static void ConvertDecimal(Vector &vector, ArrowArray &array, idx_t size, idx_t offset) {
	if (sizeof(SRC) == GetTypeIdSize(vector.GetType().InternalType())) {
		auto data = ArrowBufferData<data_t>(array, 1) + offset * sizeof(SRC);
		SetVectorData(vector, data, size);
		return;
	}

	ArrowAlignedBufferData<SRC> src_ptr(array, 1, offset, size);
	auto &val_mask = FlatVector::Validity(vector);
	switch (vector.GetType().InternalType()) {
	case PhysicalType::INT16: {
		auto tgt_ptr = FlatVector::GetDataMutable<int16_t>(vector);
		for (idx_t row = 0; row < size; row++) {
			if (val_mask.RowIsValid(row)) {
				auto result = TryCast::Operation(src_ptr[row], tgt_ptr[row]);
				D_ASSERT(result);
				(void)result;
			}
		}
		break;
	}
	case PhysicalType::INT32: {
		auto tgt_ptr = FlatVector::GetDataMutable<int32_t>(vector);
		for (idx_t row = 0; row < size; row++) {
			if (val_mask.RowIsValid(row)) {
				auto result = TryCast::Operation(src_ptr[row], tgt_ptr[row]);
				D_ASSERT(result);
				(void)result;
			}
		}
		break;
	}
	case PhysicalType::INT64: {
		auto tgt_ptr = FlatVector::GetDataMutable<int64_t>(vector);
		for (idx_t row = 0; row < size; row++) {
			if (val_mask.RowIsValid(row)) {
				auto result = TryCast::Operation(src_ptr[row], tgt_ptr[row]);
				D_ASSERT(result);
				(void)result;
			}
		}
		break;
	}
	case PhysicalType::INT128: {
		auto tgt_ptr = FlatVector::GetDataMutable<hugeint_t>(vector);
		for (idx_t row = 0; row < size; row++) {
			if (val_mask.RowIsValid(row)) {
				auto result = TryCast::Operation(src_ptr[row], tgt_ptr[row]);
				D_ASSERT(result);
				(void)result;
			}
		}
		break;
	}
	default:
		throw NotImplementedException("Unsupported physical type for Decimal: %s",
		                              TypeIdToString(vector.GetType().InternalType()));
	}
}

void ArrowToDuckDBConversion::ColumnArrowToDuckDB(Vector &vector, ArrowArray &array, idx_t chunk_offset,
                                                  ArrowArrayScanState &array_state, idx_t size,
                                                  const ArrowType &arrow_type, int64_t nested_offset,
                                                  ValidityMask *parent_mask, uint64_t parent_offset,
                                                  bool ignore_extensions) {
	D_ASSERT(!array.dictionary);
	if (!ignore_extensions && arrow_type.HasExtension()) {
		if (arrow_type.extension_data->arrow_to_duckdb) {
			// Convert the storage and then call the cast function
			// `size` is the child's own count, which for a nested child is not bounded by
			// STANDARD_VECTOR_SIZE -- size the storage vector for it (as ArrowAppendData::AppendChild does).
			Vector input_data(arrow_type.extension_data->GetInternalType(),
			                  MaxValue<idx_t>(size, STANDARD_VECTOR_SIZE));
			// Every caller has already established this column's top-level validity on `vector` — transfer
			// it onto the storage vector, since the cast function is responsible for carrying it through to
			// its result (a conversion that rebuilds the result does not preserve the pre-set validity, and
			// a spec-clean writer need not mirror a parent NULL in the storage children's own validity).
			FlatVector::CopyValidity(input_data, vector, size);
			ColumnArrowToDuckDB(input_data, array, chunk_offset, array_state, size, arrow_type, nested_offset,
			                    parent_mask, parent_offset, /*ignore_extensions*/ true);
			arrow_type.extension_data->arrow_to_duckdb(array_state.context, input_data, vector, size);
			return;
		}
	}

	switch (vector.GetType().id()) {
	case LogicalTypeId::SQLNULL:
		ConstantVector::SetNull(vector, count_t(size));
		break;
	case LogicalTypeId::BOOLEAN: {
		//! Arrow bit-packs boolean values
		//! Lets first figure out where we are in the source array
		auto effective_offset =
		    GetEffectiveOffset(array, NumericCast<int64_t>(parent_offset), chunk_offset, nested_offset);
		auto src_ptr = ArrowBufferData<uint8_t>(array, 1) + effective_offset / 8;
		auto tgt_ptr = (uint8_t *)FlatVector::GetDataMutable(vector);
		int src_pos = 0;
		idx_t cur_bit = effective_offset % 8;
		for (idx_t row = 0; row < size; row++) {
			if ((src_ptr[src_pos] & (1 << cur_bit)) == 0) {
				tgt_ptr[row] = 0;
			} else {
				tgt_ptr[row] = 1;
			}
			cur_bit++;
			if (cur_bit == 8) {
				src_pos++;
				cur_bit = 0;
			}
		}
		break;
	}
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UHUGEINT:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIME_TZ: {
		DirectConversion(vector, array, chunk_offset, nested_offset, parent_offset, size);
		break;
	}
	case LogicalTypeId::UUID:
		UUIDConversion(vector, array, chunk_offset, nested_offset, NumericCast<int64_t>(parent_offset), size);
		break;
	case LogicalTypeId::BLOB:
	case LogicalTypeId::BIT:
	case LogicalTypeId::BIGNUM:
	case LogicalTypeId::VARCHAR: {
		auto &string_info = arrow_type.GetTypeInfo<ArrowStringInfo>();
		auto size_type = string_info.GetSizeType();
		switch (size_type) {
		case ArrowVariableSizeType::SUPER_SIZE: {
			auto cdata = ArrowBufferData<char>(array, 2);
			ArrowAlignedBufferData<uint64_t> offsets(
			    array, 1, GetEffectiveOffset(array, NumericCast<int64_t>(parent_offset), chunk_offset, nested_offset),
			    size + 1);
			SetVectorString(vector, size, cdata, offsets.get());
			break;
		}
		case ArrowVariableSizeType::NORMAL: {
			auto cdata = ArrowBufferData<char>(array, 2);
			ArrowAlignedBufferData<uint32_t> offsets(
			    array, 1, GetEffectiveOffset(array, NumericCast<int64_t>(parent_offset), chunk_offset, nested_offset),
			    size + 1);
			SetVectorString(vector, size, cdata, offsets.get());
			break;
		}
		case ArrowVariableSizeType::VIEW: {
			SetVectorStringView(
			    vector, size, array,
			    GetEffectiveOffset(array, NumericCast<int64_t>(parent_offset), chunk_offset, nested_offset));
			break;
		}
		case ArrowVariableSizeType::FIXED_SIZE: {
			SetValidityMask(vector, array, chunk_offset, size, NumericCast<int64_t>(parent_offset), nested_offset);
			auto fixed_size = string_info.FixedSize();
			// Have to check validity mask before setting this up
			idx_t offset = GetEffectiveOffset(array, NumericCast<int64_t>(parent_offset), chunk_offset, nested_offset) *
			               fixed_size;
			auto cdata = ArrowBufferData<char>(array, 1);
			auto blob_len = fixed_size;
			auto result = FlatVector::ScatterWriter<string_t>(vector);
			for (idx_t row_idx = 0; row_idx < size; row_idx++) {
				if (FlatVector::IsNull(vector, row_idx)) {
					offset += blob_len;
					continue;
				}
				auto bptr = cdata + offset;
				result[row_idx] = string_t(bptr, UnsafeNumericCast<uint32_t>(blob_len));
				offset += blob_len;
			}
		}
		}
		break;
	}
	case LogicalTypeId::DATE: {
		auto &datetime_info = arrow_type.GetTypeInfo<ArrowDateTimeInfo>();
		auto precision = datetime_info.GetDateTimeType();
		switch (precision) {
		case ArrowDateTimeType::DAYS: {
			DirectConversion(vector, array, chunk_offset, nested_offset, parent_offset, size);
			break;
		}
		case ArrowDateTimeType::MILLISECONDS: {
			//! convert date from nanoseconds to days
			ArrowAlignedBufferData<uint64_t> src_ptr(
			    array, 1, GetEffectiveOffset(array, NumericCast<int64_t>(parent_offset), chunk_offset, nested_offset),
			    size);
			auto tgt_ptr = FlatVector::GetDataMutable<date_t>(vector);
			for (idx_t row = 0; row < size; row++) {
				tgt_ptr[row] = date_t(UnsafeNumericCast<int32_t>(static_cast<int64_t>(src_ptr[row]) /
				                                                 static_cast<int64_t>(1000 * 60 * 60 * 24)));
			}
			break;
		}
		default:
			throw NotImplementedException("Unsupported precision for Date Type ");
		}
		break;
	}
	case LogicalTypeId::TIME: {
		auto &datetime_info = arrow_type.GetTypeInfo<ArrowDateTimeInfo>();
		auto precision = datetime_info.GetDateTimeType();
		switch (precision) {
		case ArrowDateTimeType::SECONDS: {
			TimeConversion<int32_t>(vector, array, chunk_offset, nested_offset, NumericCast<int64_t>(parent_offset),
			                        size, 1000000);
			break;
		}
		case ArrowDateTimeType::MILLISECONDS: {
			TimeConversion<int32_t>(vector, array, chunk_offset, nested_offset, NumericCast<int64_t>(parent_offset),
			                        size, 1000);
			break;
		}
		case ArrowDateTimeType::MICROSECONDS: {
			TimeConversion<int64_t>(vector, array, chunk_offset, nested_offset, NumericCast<int64_t>(parent_offset),
			                        size, 1);
			break;
		}
		case ArrowDateTimeType::NANOSECONDS: {
			auto tgt_ptr = FlatVector::GetDataMutable<dtime_t>(vector);
			ArrowAlignedBufferData<int64_t> src_ptr(
			    array, 1, GetEffectiveOffset(array, NumericCast<int64_t>(parent_offset), chunk_offset, nested_offset),
			    size);
			for (idx_t row = 0; row < size; row++) {
				tgt_ptr[row].value = src_ptr[row] / 1000;
			}
			break;
		}
		default:
			throw NotImplementedException("Unsupported precision for Time Type ");
		}
		break;
	}
	case LogicalTypeId::TIME_NS: {
		auto &datetime_info = arrow_type.GetTypeInfo<ArrowDateTimeInfo>();
		auto precision = datetime_info.GetDateTimeType();
		switch (precision) {
		case ArrowDateTimeType::SECONDS: {
			TimeNSConversion<int32_t>(vector, array, chunk_offset, nested_offset, NumericCast<int64_t>(parent_offset),
			                          size, 1000000000);
			break;
		}
		case ArrowDateTimeType::MILLISECONDS: {
			TimeNSConversion<int32_t>(vector, array, chunk_offset, nested_offset, NumericCast<int64_t>(parent_offset),
			                          size, 1000000);
			break;
		}
		case ArrowDateTimeType::MICROSECONDS: {
			TimeNSConversion<int64_t>(vector, array, chunk_offset, nested_offset, NumericCast<int64_t>(parent_offset),
			                          size, 1000);
			break;
		}
		case ArrowDateTimeType::NANOSECONDS: {
			TimeNSConversion<int64_t>(vector, array, chunk_offset, nested_offset, NumericCast<int64_t>(parent_offset),
			                          size, 1);
			break;
		}
		default:
			throw NotImplementedException("Unsupported precision for TimeNS Type ");
		}
		break;
	}
	case LogicalTypeId::TIMESTAMP_TZ: {
		auto &datetime_info = arrow_type.GetTypeInfo<ArrowDateTimeInfo>();
		auto precision = datetime_info.GetDateTimeType();
		switch (precision) {
		case ArrowDateTimeType::SECONDS: {
			TimestampTZConversion(vector, array, chunk_offset, nested_offset, NumericCast<int64_t>(parent_offset), size,
			                      Interval::MICROS_PER_SEC);
			break;
		}
		case ArrowDateTimeType::MILLISECONDS: {
			TimestampTZConversion(vector, array, chunk_offset, nested_offset, NumericCast<int64_t>(parent_offset), size,
			                      Interval::MICROS_PER_MSEC);
			break;
		}
		case ArrowDateTimeType::MICROSECONDS: {
			DirectConversion(vector, array, chunk_offset, nested_offset, parent_offset, size);
			break;
		}
		case ArrowDateTimeType::NANOSECONDS: {
			auto tgt_ptr = FlatVector::GetDataMutable<timestamp_t>(vector);
			ArrowAlignedBufferData<int64_t> src_ptr(
			    array, 1, GetEffectiveOffset(array, NumericCast<int64_t>(parent_offset), chunk_offset, nested_offset),
			    size);
			for (idx_t row = 0; row < size; row++) {
				tgt_ptr[row].value = src_ptr[row] / 1000;
			}
			break;
		}
		default:
			throw NotImplementedException("Unsupported precision for TimestampTZ(us) Type ");
		}
		break;
	}
	case LogicalTypeId::TIMESTAMP_TZ_NS: {
		auto &datetime_info = arrow_type.GetTypeInfo<ArrowDateTimeInfo>();
		auto precision = datetime_info.GetDateTimeType();
		switch (precision) {
		case ArrowDateTimeType::SECONDS:
			TimestampTZConversion(vector, array, chunk_offset, nested_offset, NumericCast<int64_t>(parent_offset), size,
			                      Interval::NANOS_PER_SEC);
			break;
		case ArrowDateTimeType::MILLISECONDS:
			TimestampTZConversion(vector, array, chunk_offset, nested_offset, NumericCast<int64_t>(parent_offset), size,
			                      Interval::NANOS_PER_MSEC);
			break;
		case ArrowDateTimeType::MICROSECONDS:
			TimestampTZConversion(vector, array, chunk_offset, nested_offset, NumericCast<int64_t>(parent_offset), size,
			                      Interval::NANOS_PER_MICRO);
			break;
		case ArrowDateTimeType::NANOSECONDS:
			DirectConversion(vector, array, chunk_offset, nested_offset, parent_offset, size);
			break;
		default:
			throw NotImplementedException("Unsupported precision for TimestampTZ(ns) Type ");
		}
		break;
	}
	case LogicalTypeId::INTERVAL: {
		auto &datetime_info = arrow_type.GetTypeInfo<ArrowDateTimeInfo>();
		auto precision = datetime_info.GetDateTimeType();
		switch (precision) {
		case ArrowDateTimeType::SECONDS: {
			IntervalConversionUs(vector, array, chunk_offset, nested_offset, NumericCast<int64_t>(parent_offset), size,
			                     1000000);
			break;
		}
		case ArrowDateTimeType::DAYS:
		case ArrowDateTimeType::MILLISECONDS: {
			IntervalConversionUs(vector, array, chunk_offset, nested_offset, NumericCast<int64_t>(parent_offset), size,
			                     1000);
			break;
		}
		case ArrowDateTimeType::MICROSECONDS: {
			IntervalConversionUs(vector, array, chunk_offset, nested_offset, NumericCast<int64_t>(parent_offset), size,
			                     1);
			break;
		}
		case ArrowDateTimeType::NANOSECONDS: {
			auto tgt_ptr = FlatVector::GetDataMutable<interval_t>(vector);
			ArrowAlignedBufferData<int64_t> src_ptr(
			    array, 1, GetEffectiveOffset(array, NumericCast<int64_t>(parent_offset), chunk_offset, nested_offset),
			    size);
			for (idx_t row = 0; row < size; row++) {
				tgt_ptr[row].micros = src_ptr[row] / 1000;
				tgt_ptr[row].days = 0;
				tgt_ptr[row].months = 0;
			}
			break;
		}
		case ArrowDateTimeType::MONTHS: {
			IntervalConversionMonths(vector, array, chunk_offset, nested_offset, NumericCast<int64_t>(parent_offset),
			                         size);
			break;
		}
		case ArrowDateTimeType::MONTH_DAY_NANO: {
			IntervalConversionMonthDayNanos(vector, array, chunk_offset, nested_offset,
			                                NumericCast<int64_t>(parent_offset), size);
			break;
		}
		default:
			throw NotImplementedException("Unsupported precision for Interval/Duration Type ");
		}
		break;
	}
	case LogicalTypeId::DECIMAL: {
		auto &datetime_info = arrow_type.GetTypeInfo<ArrowDecimalInfo>();
		auto offset = GetEffectiveOffset(array, NumericCast<int64_t>(parent_offset), chunk_offset, nested_offset);
		switch (datetime_info.GetBitWidth()) {
		case DecimalBitWidth::DECIMAL_32:
			ConvertDecimal<int32_t>(vector, array, size, offset);
			break;
		case DecimalBitWidth::DECIMAL_64:
			ConvertDecimal<int64_t>(vector, array, size, offset);
			break;
		case DecimalBitWidth::DECIMAL_128:
			ConvertDecimal<hugeint_t>(vector, array, size, offset);
			break;
		default:
			throw NotImplementedException("Unsupported precision for Arrow Decimal Type.");
		}
		break;
	}
	case LogicalTypeId::LIST: {
		ArrowToDuckDBList(vector, array, chunk_offset, array_state, size, arrow_type, nested_offset, parent_mask,
		                  NumericCast<int64_t>(parent_offset));
		break;
	}
	case LogicalTypeId::ARRAY: {
		ArrowToDuckDBArray(vector, array, chunk_offset, array_state, size, arrow_type, nested_offset, parent_mask,
		                   NumericCast<int64_t>(parent_offset));
		break;
	}
	case LogicalTypeId::MAP: {
		ArrowToDuckDBList(vector, array, chunk_offset, array_state, size, arrow_type, nested_offset, parent_mask,
		                  NumericCast<int64_t>(parent_offset));
		ArrowToDuckDBMapVerify(vector, size);
		break;
	}
	case LogicalTypeId::STRUCT:
	case LogicalTypeId::TUPLE: {
		//! Fill the children
		auto &struct_info = arrow_type.GetTypeInfo<ArrowStructInfo>();
		auto &child_entries = StructVector::GetEntries(vector);
		auto &struct_validity_mask = FlatVector::ValidityMutable(vector);
		const auto child_offset = parent_offset + NumericCast<uint64_t>(array.offset);
		const auto child_nested_offset = GetChildNestedOffset(array, nested_offset);
		for (idx_t child_idx = 0; child_idx < NumericCast<idx_t>(array.n_children); child_idx++) {
			auto &child_entry = child_entries[child_idx];
			auto &child_array = *array.children[child_idx];
			auto &child_type = struct_info.GetChild(child_idx);
			auto &child_state = array_state.GetChild(child_idx);

			ArrowToDuckDBConversion::SetValidityMask(child_entry, child_array, chunk_offset, size,
			                                         NumericCast<int64_t>(child_offset), child_nested_offset);
			if (struct_validity_mask.CanHaveNull()) {
				auto &child_validity_mark = FlatVector::ValidityMutable(child_entry);
				for (idx_t i = 0; i < size; i++) {
					if (!struct_validity_mask.RowIsValid(i)) {
						child_validity_mark.SetInvalid(i);
					}
				}
			}

			auto array_physical_type = child_type.GetPhysicalType();
			switch (array_physical_type) {
			case ArrowArrayPhysicalType::DICTIONARY_ENCODED:
				ArrowToDuckDBConversion::ColumnArrowToDuckDBDictionary(
				    child_entry, child_array, chunk_offset, child_state, size, child_type, child_nested_offset,
				    &struct_validity_mask, child_offset);
				break;
			case ArrowArrayPhysicalType::RUN_END_ENCODED:
				ColumnArrowToDuckDBRunEndEncoded(child_entry, child_array, chunk_offset, child_state, size, child_type,
				                                 child_nested_offset, &struct_validity_mask, child_offset);
				break;
			case ArrowArrayPhysicalType::DEFAULT:
				ColumnArrowToDuckDB(child_entry, child_array, chunk_offset, child_state, size, child_type,
				                    child_nested_offset, &struct_validity_mask, child_offset, false);
				break;
			default:
				throw NotImplementedException("ArrowArrayPhysicalType not recognized");
			}
		}
		break;
	}
	case LogicalTypeId::UNION: {
		idx_t type_ids_buffer_idx = array.n_buffers == 1 ? 0 : 1;
		auto effective_offset =
		    GetEffectiveOffset(array, NumericCast<int64_t>(parent_offset), chunk_offset, nested_offset);
		auto type_ids = ArrowBufferData<int8_t>(array, type_ids_buffer_idx) + effective_offset;
		D_ASSERT(type_ids);
		auto members = UnionType::CopyMemberTypes(vector.GetType());

		auto &validity_mask = FlatVector::ValidityMutable(vector);
		auto &union_info = arrow_type.GetTypeInfo<ArrowStructInfo>();
		const auto member_offset = parent_offset + NumericCast<uint64_t>(array.offset);
		const auto member_nested_offset = GetChildNestedOffset(array, nested_offset);
		duckdb::vector<Vector> children;
		for (idx_t child_idx = 0; child_idx < NumericCast<idx_t>(array.n_children); child_idx++) {
			Vector child(members[child_idx].second, size);
			auto &child_array = *array.children[child_idx];
			auto &child_state = array_state.GetChild(child_idx);
			auto &child_type = union_info.GetChild(child_idx);

			// the members of a sparse union have the same length as the union - they are scanned at the same offset
			ArrowToDuckDBConversion::SetValidityMask(child, child_array, chunk_offset, size,
			                                         NumericCast<int64_t>(member_offset), member_nested_offset);
			auto array_physical_type = child_type.GetPhysicalType();

			switch (array_physical_type) {
			case ArrowArrayPhysicalType::DICTIONARY_ENCODED:
				ArrowToDuckDBConversion::ColumnArrowToDuckDBDictionary(child, child_array, chunk_offset, child_state,
				                                                       size, child_type, member_nested_offset,
				                                                       &validity_mask, member_offset);
				break;
			case ArrowArrayPhysicalType::RUN_END_ENCODED:
				ArrowToDuckDBConversion::ColumnArrowToDuckDBRunEndEncoded(child, child_array, chunk_offset, child_state,
				                                                          size, child_type, member_nested_offset,
				                                                          &validity_mask, member_offset);
				break;
			case ArrowArrayPhysicalType::DEFAULT:
				ArrowToDuckDBConversion::ColumnArrowToDuckDB(child, child_array, chunk_offset, child_state, size,
				                                             child_type, member_nested_offset, &validity_mask,
				                                             member_offset);
				break;
			default:
				throw NotImplementedException("ArrowArrayPhysicalType not recognized");
			}

			children.push_back(std::move(child));
		}

		for (idx_t row_idx = 0; row_idx < size; row_idx++) {
			auto tag = NumericCast<uint8_t>(type_ids[row_idx]);

			auto out_of_range = tag >= array.n_children;
			if (out_of_range) {
				throw InvalidInputException("Arrow union tag out of range: %d", tag);
			}

			const Value &value = children[tag].GetValue(row_idx);
			vector.SetValue(row_idx, value.IsNull() ? Value() : Value::UNION(members, tag, value));
		}

		break;
	}
	default:
		throw NotImplementedException("Unsupported type for arrow conversion: %s", vector.GetType().ToString());
	}
	if (vector.GetBufferRef()) {
		vector.BufferMutable().AddAuxiliaryData(make_uniq<ArrowAuxiliaryData>(array_state.owned_data));
	}
}

template <class T>
static void SetSelectionVectorLoop(SelectionVector &sel, data_ptr_t indices_p, idx_t size) {
	auto indices = reinterpret_cast<T *>(indices_p);
	for (idx_t row = 0; row < size; row++) {
		sel.set_index(row, UnsafeNumericCast<idx_t>(indices[row]));
	}
}

template <class T>
static void SetSelectionVectorLoopWithChecks(SelectionVector &sel, data_ptr_t indices_p, idx_t size) {
	auto indices = reinterpret_cast<T *>(indices_p);
	for (idx_t row = 0; row < size; row++) {
		if (indices[row] > NumericLimits<uint32_t>::Maximum()) {
			throw ConversionException("DuckDB only supports indices that fit on an uint32");
		}
		sel.set_index(row, NumericCast<idx_t>(indices[row]));
	}
}

template <class T>
static void SetMaskedSelectionVectorLoop(SelectionVector &sel, data_ptr_t indices_p, idx_t size, ValidityMask &mask,
                                         idx_t last_element_pos) {
	auto indices = reinterpret_cast<T *>(indices_p);
	for (idx_t row = 0; row < size; row++) {
		if (mask.RowIsValid(row)) {
			sel.set_index(row, UnsafeNumericCast<idx_t>(indices[row]));
		} else {
			//! Need to point out to last element
			sel.set_index(row, last_element_pos);
		}
	}
}

static void SetSelectionVector(SelectionVector &sel, data_ptr_t indices_p, const LogicalType &logical_type, idx_t size,
                               ValidityMask *mask = nullptr, idx_t last_element_pos = 0) {
	sel.Initialize(size);

	if (mask) {
		switch (logical_type.id()) {
		case LogicalTypeId::UTINYINT:
			SetMaskedSelectionVectorLoop<uint8_t>(sel, indices_p, size, *mask, last_element_pos);
			break;
		case LogicalTypeId::TINYINT:
			SetMaskedSelectionVectorLoop<int8_t>(sel, indices_p, size, *mask, last_element_pos);
			break;
		case LogicalTypeId::USMALLINT:
			SetMaskedSelectionVectorLoop<uint16_t>(sel, indices_p, size, *mask, last_element_pos);
			break;
		case LogicalTypeId::SMALLINT:
			SetMaskedSelectionVectorLoop<int16_t>(sel, indices_p, size, *mask, last_element_pos);
			break;
		case LogicalTypeId::UINTEGER:
			if (last_element_pos > NumericLimits<uint32_t>::Maximum()) {
				//! Its guaranteed that our indices will point to the last element, so just throw an error
				throw ConversionException("DuckDB only supports indices that fit on an uint32");
			}
			SetMaskedSelectionVectorLoop<uint32_t>(sel, indices_p, size, *mask, last_element_pos);
			break;
		case LogicalTypeId::INTEGER:
			SetMaskedSelectionVectorLoop<int32_t>(sel, indices_p, size, *mask, last_element_pos);
			break;
		case LogicalTypeId::UBIGINT:
			if (last_element_pos > NumericLimits<uint32_t>::Maximum()) {
				//! Its guaranteed that our indices will point to the last element, so just throw an error
				throw ConversionException("DuckDB only supports indices that fit on an uint32");
			}
			SetMaskedSelectionVectorLoop<uint64_t>(sel, indices_p, size, *mask, last_element_pos);
			break;
		case LogicalTypeId::BIGINT:
			if (last_element_pos > NumericLimits<uint32_t>::Maximum()) {
				//! Its guaranteed that our indices will point to the last element, so just throw an error
				throw ConversionException("DuckDB only supports indices that fit on an uint32");
			}
			SetMaskedSelectionVectorLoop<int64_t>(sel, indices_p, size, *mask, last_element_pos);
			break;

		default:
			throw NotImplementedException("(Arrow) Unsupported type for selection vectors %s", logical_type.ToString());
		}

	} else {
		switch (logical_type.id()) {
		case LogicalTypeId::UTINYINT:
			SetSelectionVectorLoop<uint8_t>(sel, indices_p, size);
			break;
		case LogicalTypeId::TINYINT:
			SetSelectionVectorLoop<int8_t>(sel, indices_p, size);
			break;
		case LogicalTypeId::USMALLINT:
			SetSelectionVectorLoop<uint16_t>(sel, indices_p, size);
			break;
		case LogicalTypeId::SMALLINT:
			SetSelectionVectorLoop<int16_t>(sel, indices_p, size);
			break;
		case LogicalTypeId::UINTEGER:
			SetSelectionVectorLoop<uint32_t>(sel, indices_p, size);
			break;
		case LogicalTypeId::INTEGER:
			SetSelectionVectorLoop<int32_t>(sel, indices_p, size);
			break;
		case LogicalTypeId::UBIGINT:
			if (last_element_pos > NumericLimits<uint32_t>::Maximum()) {
				//! We need to check if our indexes fit in a uint32_t
				SetSelectionVectorLoopWithChecks<uint64_t>(sel, indices_p, size);
			} else {
				SetSelectionVectorLoop<uint64_t>(sel, indices_p, size);
			}
			break;
		case LogicalTypeId::BIGINT:
			if (last_element_pos > NumericLimits<uint32_t>::Maximum()) {
				//! We need to check if our indexes fit in a uint32_t
				SetSelectionVectorLoopWithChecks<int64_t>(sel, indices_p, size);
			} else {
				SetSelectionVectorLoop<int64_t>(sel, indices_p, size);
			}
			break;
		default:
			throw ConversionException("(Arrow) Unsupported type for selection vectors %s", logical_type.ToString());
		}
	}
}

static bool CanContainNull(const ArrowArray &array, const ValidityMask *parent_mask) {
	// a null count of -1 means that the number of nulls is unknown
	if (array.null_count != 0) {
		return true;
	}
	if (!parent_mask) {
		return false;
	}
	return parent_mask->CanHaveNull();
}

// Arrow's null type converts to a constant vector, and FlatVector::SetNull needs flat vectors along its recursion
// into STRUCT and ARRAY children, which never reaches LIST contents.
static void FlattenForSetNull(Vector &vector) {
	if (vector.GetVectorType() != VectorType::FLAT_VECTOR) {
		vector.Flatten();
		return;
	}
	switch (vector.GetType().InternalType()) {
	case PhysicalType::STRUCT:
		for (auto &entry : StructVector::GetEntries(vector)) {
			FlattenForSetNull(entry);
		}
		break;
	case PhysicalType::ARRAY:
		FlattenForSetNull(ArrayVector::GetChildMutable(vector));
		break;
	default:
		break;
	}
}

void ArrowToDuckDBConversion::ColumnArrowToDuckDBDictionary(Vector &vector, ArrowArray &array, idx_t chunk_offset,
                                                            ArrowArrayScanState &array_state, idx_t size,
                                                            const ArrowType &arrow_type, int64_t nested_offset,
                                                            const ValidityMask *parent_mask, uint64_t parent_offset) {
	if (vector.GetBufferRef()) {
		vector.BufferMutable().AddAuxiliaryData(make_uniq<ArrowAuxiliaryData>(array_state.owned_data));
	}
	D_ASSERT(arrow_type.HasDictionary());
	const bool has_nulls = CanContainNull(array, parent_mask);
	if (array_state.CacheOutdated(array.dictionary)) {
		//! We need to set the dictionary data for this column
		//! Allocate one extra entry beyond dictionary length for the NULL sentinel.
		//! SetMaskedSelectionVectorLoop points NULL indices at last_element_pos (= dictionary->length),
		//! so the buffer must be large enough and that entry must be marked invalid.
		auto dict_length = NumericCast<idx_t>(array.dictionary->length);
		auto base_vector = make_uniq<Vector>(vector.GetType(), dict_length + 1);
		ArrowToDuckDBConversion::SetValidityMask(*base_vector, *array.dictionary, 0, dict_length, 0, 0, has_nulls);
		auto &dictionary_type = arrow_type.GetDictionary();
		auto arrow_physical_type = dictionary_type.GetPhysicalType();
		;
		switch (arrow_physical_type) {
		case ArrowArrayPhysicalType::DICTIONARY_ENCODED:
			ColumnArrowToDuckDBDictionary(*base_vector, *array.dictionary, 0, array_state,
			                              NumericCast<idx_t>(array.dictionary->length), dictionary_type);
			break;
		case ArrowArrayPhysicalType::RUN_END_ENCODED:
			ColumnArrowToDuckDBRunEndEncoded(*base_vector, *array.dictionary, 0, array_state,
			                                 NumericCast<idx_t>(array.dictionary->length), dictionary_type);
			break;
		case ArrowArrayPhysicalType::DEFAULT:
			ColumnArrowToDuckDB(*base_vector, *array.dictionary, 0, array_state,
			                    NumericCast<idx_t>(array.dictionary->length), dictionary_type);
			break;
		default:
			throw NotImplementedException("ArrowArrayPhysicalType not recognized");
		};
		// the dictionary buffer holds dict_length entries plus one trailing NULL sentinel slot.
		// the inner ColumnArrowToDuckDB call may have replaced the buffer via FlatVector::SetData
		// (e.g. DirectConversion's zero-copy path), shrinking capacity from dict_length+1 to
		// dict_length. Re-extend before sizing so the sentinel slot stays in bounds.
		base_vector->Reserve(dict_length + 1);
		FlatVector::SetSize(*base_vector, count_t(dict_length + 1));
		// nested children of the sentinel slot hold uninitialized data unless they are marked NULL as well
		FlattenForSetNull(*base_vector);
		FlatVector::SetNull(*base_vector, dict_length, true);
		array_state.AddDictionary(std::move(base_vector), array.dictionary);
	}
	auto offset_type = arrow_type.GetDuckType();
	//! Get Pointer to Indices of Dictionary
	auto index_size = GetTypeIdSize(offset_type.InternalType());
	auto indices =
	    ArrowBufferData<data_t>(array, 1) +
	    index_size * GetEffectiveOffset(array, NumericCast<int64_t>(parent_offset), chunk_offset, nested_offset);
	unsafe_unique_array<uint64_t> aligned_indices;
	if (!IsAligned(indices, index_size)) {
		aligned_indices = make_unsafe_uniq_array_uninitialized<uint64_t>((index_size * size + 7) / 8);
		memcpy(aligned_indices.get(), indices, index_size * size);
		indices = reinterpret_cast<data_ptr_t>(aligned_indices.get());
	}

	SelectionVector sel;
	if (has_nulls) {
		// size may exceed STANDARD_VECTOR_SIZE, so the scratch mask must be sized for it.
		ValidityMask indices_validity(size);
		// validity must be read from the same effective offset as the indices
		GetValidityMask(indices_validity, array, chunk_offset, size, NumericCast<int64_t>(parent_offset),
		                nested_offset);
		if (parent_mask && parent_mask->CanHaveNull()) {
			auto &struct_validity_mask = *parent_mask;
			for (idx_t i = 0; i < size; i++) {
				if (!struct_validity_mask.RowIsValid(i)) {
					indices_validity.SetInvalid(i);
				}
			}
		}
		SetSelectionVector(sel, indices, offset_type, size, &indices_validity,
		                   NumericCast<idx_t>(array.dictionary->length));
	} else {
		SetSelectionVector(sel, indices, offset_type, size);
	}
	vector.Slice(array_state.GetDictionary(), sel, size);
	vector.Verify();
}

void ArrowTableFunction::ArrowToDuckDB(ArrowScanLocalState &scan_state, const arrow_column_map_t &arrow_convert_data,
                                       DataChunk &output, bool arrow_scan_is_projected, idx_t rowid_column_index) {
	for (idx_t idx = 0; idx < output.ColumnCount(); idx++) {
		auto col_idx = scan_state.column_ids.empty() ? idx : scan_state.column_ids[idx];

		// If projection was not pushed down into the arrow scanner, but projection pushdown is enabled on the
		// table function, we need to use original column ids here.
		auto arrow_array_idx = arrow_scan_is_projected ? idx : col_idx;

		if (rowid_column_index != COLUMN_IDENTIFIER_ROW_ID) {
			if (col_idx == COLUMN_IDENTIFIER_ROW_ID) {
				arrow_array_idx = rowid_column_index;
			} else if (col_idx >= rowid_column_index) {
				// Since the rowid column is skipped when the table is bound (its not a named column),
				// we need to shift references forward in the Arrow array by one to match the alignment
				// that DuckDB believes the Arrow array is in.
				col_idx += 1;
				arrow_array_idx += 1;
			}
		} else {
			// If there isn't any defined row_id_index, and we're asked for it, skip the column.
			// This is the incumbent behavior.
			if (col_idx == COLUMN_IDENTIFIER_ROW_ID) {
				continue;
			}
		}

		auto &parent_array = scan_state.chunk->arrow_array;
		auto &array = *scan_state.chunk->arrow_array.children[arrow_array_idx];
		if (!array.release) {
			throw InvalidInputException("arrow_scan: released array passed");
		}
		if (array.length != scan_state.chunk->arrow_array.length) {
			throw InvalidInputException("arrow_scan: array length mismatch");
		}

		D_ASSERT(arrow_convert_data.find(col_idx) != arrow_convert_data.end());
		auto &arrow_type = *arrow_convert_data.at(col_idx);
		auto &array_state = scan_state.GetState(col_idx);

		// Make sure this Vector keeps the Arrow chunk alive in case we can zero-copy the data
		if (!array_state.owned_data) {
			array_state.owned_data = scan_state.chunk;
		}
		auto array_physical_type = arrow_type.GetPhysicalType();

		switch (array_physical_type) {
		case ArrowArrayPhysicalType::DICTIONARY_ENCODED:
			ArrowToDuckDBConversion::ColumnArrowToDuckDBDictionary(output.data[idx], array, scan_state.chunk_offset,
			                                                       array_state, output.size(), arrow_type);
			break;
		case ArrowArrayPhysicalType::RUN_END_ENCODED:
			ArrowToDuckDBConversion::ColumnArrowToDuckDBRunEndEncoded(output.data[idx], array, scan_state.chunk_offset,
			                                                          array_state, output.size(), arrow_type);
			break;
		case ArrowArrayPhysicalType::DEFAULT:
			ArrowToDuckDBConversion::SetValidityMask(output.data[idx], array, scan_state.chunk_offset, output.size(),
			                                         parent_array.offset, -1);
			ArrowToDuckDBConversion::ColumnArrowToDuckDB(output.data[idx], array, scan_state.chunk_offset, array_state,
			                                             output.size(), arrow_type);
			break;
		default:
			throw NotImplementedException("ArrowArrayPhysicalType not recognized");
		}
		FlatVector::SetSize(output.data[idx], count_t(output.size()));
	}
}

} // namespace duckdb
