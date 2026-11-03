#pragma once

#include "duckdb/function/cast/variant/to_variant_fwd.hpp"
#include "duckdb/common/types/variant_visitor.hpp"

namespace duckdb {
namespace variant {

namespace {

struct AnalyzeState {
public:
	explicit AnalyzeState(uint32_t &children_offset) : children_offset(children_offset) {
	}

public:
	uint32_t &children_offset;
};

struct WriteState {
public:
	WriteState(uint32_t &keys_offset, uint32_t &children_offset, uint32_t &blob_offset, data_ptr_t blob_data,
	           uint32_t &blob_size)
	    : keys_offset(keys_offset), children_offset(children_offset), blob_offset(blob_offset), blob_data(blob_data),
	      blob_size(blob_size) {
	}

public:
	inline data_ptr_t GetDestination() {
		return blob_data + blob_offset + blob_size;
	}

public:
	uint32_t &keys_offset;
	uint32_t &children_offset;
	uint32_t &blob_offset;
	data_ptr_t blob_data;
	uint32_t &blob_size;
};

struct VariantToVariantSizeAnalyzer {
	using result_type = uint32_t;

	static uint32_t VisitNull(AnalyzeState &state) {
		return 0;
	}
	static uint32_t VisitBoolean(bool, AnalyzeState &state) {
		return 0;
	}

	template <typename T>
	static uint32_t VisitInteger(T, AnalyzeState &state) {
		return sizeof(T);
	}

	static uint32_t VisitFloat(float, AnalyzeState &state) {
		return sizeof(float);
	}
	static uint32_t VisitDouble(double, AnalyzeState &state) {
		return sizeof(double);
	}
	static uint32_t VisitUUID(hugeint_t, AnalyzeState &state) {
		return sizeof(hugeint_t);
	}
	static uint32_t VisitDate(date_t, AnalyzeState &state) {
		return sizeof(int32_t);
	}
	static uint32_t VisitInterval(interval_t, AnalyzeState &state) {
		return sizeof(interval_t);
	}

	static uint32_t VisitTime(dtime_t, AnalyzeState &state) {
		return sizeof(dtime_t);
	}
	static uint32_t VisitTimeNanos(dtime_ns_t, AnalyzeState &state) {
		return sizeof(dtime_ns_t);
	}
	static uint32_t VisitTimeTZ(dtime_tz_t, AnalyzeState &state) {
		return sizeof(dtime_tz_t);
	}
	static uint32_t VisitTimestampSec(timestamp_sec_t, AnalyzeState &state) {
		return sizeof(timestamp_sec_t);
	}
	static uint32_t VisitTimestampMs(timestamp_ms_t, AnalyzeState &state) {
		return sizeof(timestamp_ms_t);
	}
	static uint32_t VisitTimestamp(timestamp_t, AnalyzeState &state) {
		return sizeof(timestamp_t);
	}
	static uint32_t VisitTimestampNanos(timestamp_ns_t, AnalyzeState &state) {
		return sizeof(timestamp_ns_t);
	}
	static uint32_t VisitTimestampTZ(timestamp_tz_t, AnalyzeState &state) {
		return sizeof(timestamp_tz_t);
	}
	static uint32_t VisitTimestampTZNanos(timestamp_tz_ns_t, AnalyzeState &state) {
		return sizeof(timestamp_tz_ns_t);
	}

	static uint32_t VisitString(const string_t &str, AnalyzeState &state) {
		auto length = static_cast<uint32_t>(str.GetSize());
		return GetVarintSize(length) + length;
	}

	static uint32_t VisitBlob(const string_t &blob, AnalyzeState &state) {
		return VisitString(blob, state);
	}
	static uint32_t VisitBignum(const string_t &bignum, AnalyzeState &state) {
		return VisitString(bignum, state);
	}
	static uint32_t VisitGeometry(const string_t &geom, AnalyzeState &state) {
		return VisitString(geom, state);
	}
	static uint32_t VisitBitstring(const string_t &bits, AnalyzeState &state) {
		return VisitString(bits, state);
	}

	template <typename T>
	static uint32_t VisitDecimal(T, uint32_t width, uint32_t scale, AnalyzeState &state) {
		uint32_t size = GetVarintSize(width) + GetVarintSize(scale);
		size += sizeof(T);
		return size;
	}

	static uint32_t VisitArray(const UnifiedVariantVectorData &variant, idx_t row, const VariantNestedData &nested_data,
	                           idx_t depth, AnalyzeState &state) {
		uint32_t size = GetVarintSize(nested_data.child_count);
		if (nested_data.child_count) {
			size += GetVarintSize(nested_data.children_idx + state.children_offset);
		}
		return size;
	}

	static uint32_t VisitObject(const UnifiedVariantVectorData &variant, idx_t row,
	                            const VariantNestedData &nested_data, idx_t depth, AnalyzeState &state) {
		return VisitArray(variant, row, nested_data, depth, state);
	}

	static uint32_t VisitDefault(VariantLogicalType type_id, const_data_ptr_t, AnalyzeState &) {
		throw InternalException("Unrecognized VariantLogicalType: %s", EnumUtil::ToString(type_id));
	}
};

struct VariantToVariantDataWriter {
	using result_type = void;

	static void VisitNull(WriteState &state) {
		return;
	}
	static void VisitBoolean(bool, WriteState &state) {
		return;
	}

	template <typename T>
	static void VisitInteger(T val, WriteState &state) {
		Store<T>(val, state.GetDestination());
		state.blob_size += sizeof(T);
	}
	static void VisitFloat(float val, WriteState &state) {
		VisitInteger(val, state);
	}
	static void VisitDouble(double val, WriteState &state) {
		VisitInteger(val, state);
	}
	static void VisitUUID(hugeint_t val, WriteState &state) {
		VisitInteger(val, state);
	}
	static void VisitDate(date_t val, WriteState &state) {
		VisitInteger(val, state);
	}
	static void VisitInterval(interval_t val, WriteState &state) {
		VisitInteger(val, state);
	}
	static void VisitTime(dtime_t val, WriteState &state) {
		VisitInteger(val, state);
	}
	static void VisitTimeNanos(dtime_ns_t val, WriteState &state) {
		VisitInteger(val, state);
	}
	static void VisitTimeTZ(dtime_tz_t val, WriteState &state) {
		VisitInteger(val, state);
	}
	static void VisitTimestampSec(timestamp_sec_t val, WriteState &state) {
		VisitInteger(val, state);
	}
	static void VisitTimestampMs(timestamp_ms_t val, WriteState &state) {
		VisitInteger(val, state);
	}
	static void VisitTimestamp(timestamp_t val, WriteState &state) {
		VisitInteger(val, state);
	}
	static void VisitTimestampNanos(timestamp_ns_t val, WriteState &state) {
		VisitInteger(val, state);
	}
	static void VisitTimestampTZ(timestamp_tz_t val, WriteState &state) {
		VisitInteger(val, state);
	}
	static void VisitTimestampTZNanos(timestamp_tz_ns_t val, WriteState &state) {
		VisitInteger(val, state);
	}

	static void VisitString(const string_t &str, WriteState &state) {
		auto length = str.GetSize();
		state.blob_size += VarintEncode(length, state.GetDestination());
		memcpy(state.GetDestination(), str.GetData(), length);
		state.blob_size += length;
	}
	static void VisitBlob(const string_t &blob, WriteState &state) {
		return VisitString(blob, state);
	}
	static void VisitBignum(const string_t &bignum, WriteState &state) {
		return VisitString(bignum, state);
	}
	static void VisitGeometry(const string_t &geom, WriteState &state) {
		return VisitString(geom, state);
	}
	static void VisitBitstring(const string_t &bits, WriteState &state) {
		return VisitString(bits, state);
	}

	template <typename T>
	static void VisitDecimal(T val, uint32_t width, uint32_t scale, WriteState &state) {
		state.blob_size += VarintEncode(width, state.GetDestination());
		state.blob_size += VarintEncode(scale, state.GetDestination());
		Store<T>(val, state.GetDestination());
		state.blob_size += sizeof(T);
	}

	static void VisitArray(const UnifiedVariantVectorData &variant, idx_t row, const VariantNestedData &nested_data,
	                       idx_t depth, WriteState &state) {
		state.blob_size += VarintEncode(nested_data.child_count, state.GetDestination());
		if (nested_data.child_count) {
			//! NOTE: The 'child_index' stored in the OBJECT/ARRAY data could require more bits
			//! That's the reason we have to rewrite the data in VARIANT->VARIANT cast
			state.blob_size += VarintEncode(nested_data.children_idx + state.children_offset, state.GetDestination());
		}
	}

	static void VisitObject(const UnifiedVariantVectorData &variant, idx_t row, const VariantNestedData &nested_data,
	                        idx_t depth, WriteState &state) {
		return VisitArray(variant, row, nested_data, depth, state);
	}

	static void VisitDefault(VariantLogicalType type_id, const_data_ptr_t, WriteState &) {
		throw InternalException("Unrecognized VariantLogicalType: %s", EnumUtil::ToString(type_id));
	}
};

} // namespace

template <bool WRITE_DATA, bool IGNORE_NULLS>
bool ConvertVariantToVariant(ToVariantSourceData &source_data, ToVariantGlobalResultData &result_data, idx_t count,
                             optional_ptr<const SelectionVector> selvec,
                             optional_ptr<const SelectionVector> values_index_selvec, const bool is_root) {
	auto keys_offset_data = OffsetData::GetKeys(result_data.offsets);
	auto children_offset_data = OffsetData::GetChildren(result_data.offsets);
	auto values_offset_data = OffsetData::GetValues(result_data.offsets);
	auto blob_offset_data = OffsetData::GetBlob(result_data.offsets);

	RecursiveUnifiedVectorFormat source_format;
	Vector::RecursiveToUnifiedFormat(source_data.vec, source_format);
	UnifiedVariantVectorData source(source_format);

	auto &result = result_data.variant;
	for (idx_t source_index = 0; source_index < count; source_index++) {
		//! Map the loop index through the incoming selection to the actual source row.
		const auto scan_index = source_data.GetMappedIndex(source_index);
		auto result_index = selvec ? selvec->get_index(source_index) : source_index;

		auto &keys_list_entry = result.keys_data[result_index];
		auto &children_list_entry = result.children_data[result_index];
		auto blob_data = data_ptr_cast(result.blob_data[result_index].GetDataWriteable());

		auto &keys_offset = keys_offset_data[result_index];
		auto &children_offset = children_offset_data[result_index];
		auto &blob_offset = blob_offset_data[result_index];

		uint32_t keys_count = 0;
		uint32_t blob_size = 0;
		if (!source.RowIsValid(scan_index)) {
			if (!IGNORE_NULLS) {
				HandleVariantNull<WRITE_DATA>(result_data, result_index, values_offset_data, blob_offset,
				                              values_index_selvec, source_index, is_root);
			}
			continue;
		}
		if (WRITE_DATA && values_index_selvec) {
			auto &values_offset = values_offset_data[result_index];
			//! Write the values_index for the parent of this column
			result.values_index_data[values_index_selvec->get_index(source_index)] = values_offset;
		}

		//! First write all children
		//! NOTE: this has to happen first because we use 'values_offset', which is increased when we write the values
		auto source_children_list_entry = source.GetChildrenListEntry(scan_index);
		for (idx_t source_children_index = 0; source_children_index < source_children_list_entry.length;
		     source_children_index++) {
			//! values_index
			if (WRITE_DATA) {
				auto &values_offset = values_offset_data[result_index];
				auto source_value_index = source.GetValuesIndex(scan_index, source_children_index);
				result.values_index_data[children_list_entry.offset + children_offset + source_children_index] =
				    values_offset + source_value_index;
			}

			//! keys_index
			if (source.KeysIndexIsValid(scan_index, source_children_index)) {
				if (WRITE_DATA) {
					//! Look up the existing key from 'source'
					auto source_key_index = source.GetKeysIndex(scan_index, source_children_index);
					auto &source_key_value = source.GetKey(scan_index, source_key_index);

					//! Now write this key to the dictionary of the result
					auto dict_index = result_data.GetOrCreateIndex(source_key_value);
					result.keys_index_data[children_list_entry.offset + children_offset + source_children_index] =
					    NumericCast<uint32_t>(keys_offset + keys_count);
					result_data.keys_selvec.set_index(keys_list_entry.offset + keys_offset + keys_count, dict_index);
				}
				keys_count++;
			} else {
				if (WRITE_DATA) {
					result.keys_index_validity.SetInvalid(children_list_entry.offset + children_offset +
					                                      source_children_index);
				}
			}
		}

		auto source_values_list_entry = source.GetValuesListEntry(scan_index);

		if (WRITE_DATA) {
			WriteState write_state(keys_offset, children_offset, blob_offset, blob_data, blob_size);
			for (uint32_t source_value_index = 0; source_value_index < source_values_list_entry.length;
			     source_value_index++) {
				auto source_type_id = source.GetTypeId(scan_index, source_value_index);
				WriteVariantMetadata<WRITE_DATA>(result_data, result_index, values_offset_data, blob_offset + blob_size,
				                                 nullptr, 0, source_type_id);

				VariantVisitor<VariantToVariantDataWriter>::Visit(source, scan_index, source_value_index, 0,
				                                                  write_state);
			}
		} else {
			AnalyzeState analyze_state(children_offset);
			for (uint32_t source_value_index = 0; source_value_index < source_values_list_entry.length;
			     source_value_index++) {
				values_offset_data[result_index]++;
				OffsetData::AddBlobOffset(blob_size, VariantVisitor<VariantToVariantSizeAnalyzer>::Visit(
				                                         source, scan_index, source_value_index, 0, analyze_state));
			}
		}

		keys_offset += keys_count;
		children_offset += source_children_list_entry.length;
		OffsetData::AddBlobOffset(blob_offset, blob_size);
	}
	return true;
}

} // namespace variant
} // namespace duckdb
