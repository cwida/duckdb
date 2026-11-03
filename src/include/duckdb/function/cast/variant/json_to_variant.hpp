#pragma once

#include "duckdb/function/cast/variant/to_variant_fwd.hpp"
#include "duckdb/function/scalar/variant_utils.hpp"
#include "duckdb/function/cast/default_casts.hpp"
#include "yyjson.hpp"
#include "yyjson_memory.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/common/serializer/varint.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/string_map_set.hpp"
#include "duckdb/common/types/selection_vector.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/operator/cast_operators.hpp"

namespace duckdb {
namespace variant {

using namespace duckdb_yyjson; // NOLINT

namespace {

static inline string_t GetString(yyjson_val *val) {
	return string_t(unsafe_yyjson_get_str(val), NumericCast<uint32_t>(unsafe_yyjson_get_len(val)));
}

struct JSONObjectEntry {
	yyjson_val *key;
	yyjson_val *value;
};

//! Traversal state of an ARRAY or OBJECT whose children are still being converted
struct JSONConversionFrame {
	//! Whether this frame belongs to an OBJECT (ARRAY otherwise)
	bool is_object = false;
	//! Iteration state of an ARRAY
	yyjson_arr_iter arr_iter {};
	//! The deduplicated entries of an OBJECT
	vector<JSONObjectEntry> entries;
	//! The next OBJECT entry to convert
	idx_t entry_index = 0;
	//! The children index reserved for the container's next child
	idx_t child_index = 0;
	//! The keys index reserved for the OBJECT's next key
	uint32_t key_index = 0;
};

} // namespace

template <bool WRITE_DATA>
static JSONConversionFrame ConvertJSONArray(yyjson_val *arr, ToVariantGlobalResultData &result, idx_t result_index) {
	JSONConversionFrame frame;
	yyjson_arr_iter_init(arr, &frame.arr_iter);

	auto &variant = result.variant;

	auto children_offset_data = OffsetData::GetChildren(result.offsets);
	auto values_offset_data = OffsetData::GetValues(result.offsets);
	auto blob_offset_data = OffsetData::GetBlob(result.offsets);

	WriteVariantMetadata<WRITE_DATA>(result, result_index, values_offset_data, blob_offset_data[result_index], nullptr,
	                                 0, VariantLogicalType::ARRAY);

	auto &children_list_entry = variant.children_data[result_index];
	uint32_t count = NumericCast<uint32_t>(frame.arr_iter.max);
	frame.child_index = children_list_entry.offset + children_offset_data[result_index];
	WriteContainerData<WRITE_DATA>(result.variant, result_index, blob_offset_data[result_index], count,
	                               children_offset_data[result_index]);

	//! Reserve these indices for the array
	children_offset_data[result_index] += count;
	return frame;
}

template <bool WRITE_DATA>
static JSONConversionFrame ConvertJSONObject(yyjson_val *obj, ToVariantGlobalResultData &result, idx_t result_index) {
	JSONConversionFrame frame;
	frame.is_object = true;

	yyjson_obj_iter iter;
	yyjson_obj_iter_init(obj, &iter);

	// Maps object keys to their entry index.
	string_map_t<idx_t> key_to_entry;
	auto &entries = frame.entries;
	key_to_entry.reserve(iter.max);
	entries.reserve(iter.max);

	while (auto key = yyjson_obj_iter_next(&iter)) {
		auto key_string = GetString(key);
		auto inserted_entry = key_to_entry.emplace(key_string, entries.size());
		auto val = yyjson_obj_iter_get_val(key);
		if (inserted_entry.second) {
			entries.push_back({key, val});
		} else {
			entries[inserted_entry.first->second].value = val;
		}
	}

	auto keys_offset_data = OffsetData::GetKeys(result.offsets);
	auto children_offset_data = OffsetData::GetChildren(result.offsets);
	auto values_offset_data = OffsetData::GetValues(result.offsets);
	auto blob_offset_data = OffsetData::GetBlob(result.offsets);

	WriteVariantMetadata<WRITE_DATA>(result, result_index, values_offset_data, blob_offset_data[result_index], nullptr,
	                                 0, VariantLogicalType::OBJECT);

	auto &variant = result.variant;
	auto &children_list_entry = variant.children_data[result_index];
	uint32_t count = NumericCast<uint32_t>(entries.size());
	frame.child_index = children_list_entry.offset + children_offset_data[result_index];
	WriteContainerData<WRITE_DATA>(result.variant, result_index, blob_offset_data[result_index], count,
	                               children_offset_data[result_index]);

	frame.key_index = keys_offset_data[result_index];
	//! Reserve these indices for the object
	children_offset_data[result_index] += count;
	keys_offset_data[result_index] += count;
	return frame;
}

template <bool WRITE_DATA>
static bool ConvertJSONPrimitive(yyjson_val *val, ToVariantGlobalResultData &result, idx_t result_index, bool is_root) {
	auto json_tag = unsafe_yyjson_get_tag(val);

	auto &variant = result.variant;
	auto values_offset_data = OffsetData::GetValues(result.offsets);
	auto blob_offset_data = OffsetData::GetBlob(result.offsets);
	auto blob_data = data_ptr_cast(variant.blob_data[result_index].GetDataWriteable());

	switch (json_tag) {
	case YYJSON_TYPE_STR | YYJSON_SUBTYPE_NOESC:
	case YYJSON_TYPE_STR | YYJSON_SUBTYPE_NONE: {
		WriteVariantMetadata<WRITE_DATA>(result, result_index, values_offset_data, blob_offset_data[result_index],
		                                 nullptr, 0, VariantLogicalType::VARCHAR);
		uint32_t length = NumericCast<uint32_t>(unsafe_yyjson_get_len(val));
		auto length_varint_size = GetVarintSize(length);
		if (WRITE_DATA) {
			auto str = unsafe_yyjson_get_str(val);
			auto str_blob_data = blob_data + blob_offset_data[result_index];
			VarintEncode(length, str_blob_data);
			memcpy(str_blob_data + length_varint_size, const_data_ptr_cast(str), length);
		}
		OffsetData::AddBlobOffset(blob_offset_data[result_index], length_varint_size);
		OffsetData::AddBlobOffset(blob_offset_data[result_index], length);
		break;
	}
	case YYJSON_TYPE_BOOL | YYJSON_SUBTYPE_TRUE: {
		WriteVariantMetadata<WRITE_DATA>(result, result_index, values_offset_data, blob_offset_data[result_index],
		                                 nullptr, 0, VariantLogicalType::BOOL_TRUE);
		break;
	}
	case YYJSON_TYPE_BOOL | YYJSON_SUBTYPE_FALSE: {
		WriteVariantMetadata<WRITE_DATA>(result, result_index, values_offset_data, blob_offset_data[result_index],
		                                 nullptr, 0, VariantLogicalType::BOOL_FALSE);
		break;
	}
	case YYJSON_TYPE_NUM | YYJSON_SUBTYPE_UINT: {
		WriteVariantMetadata<WRITE_DATA>(result, result_index, values_offset_data, blob_offset_data[result_index],
		                                 nullptr, 0, VariantLogicalType::UINT64);
		if (WRITE_DATA) {
			auto value = unsafe_yyjson_get_uint(val);
			memcpy(blob_data + blob_offset_data[result_index], const_data_ptr_cast(&value), sizeof(uint64_t));
		}
		OffsetData::AddBlobOffset(blob_offset_data[result_index], sizeof(uint64_t));
		break;
	}
	case YYJSON_TYPE_NUM | YYJSON_SUBTYPE_SINT: {
		WriteVariantMetadata<WRITE_DATA>(result, result_index, values_offset_data, blob_offset_data[result_index],
		                                 nullptr, 0, VariantLogicalType::INT64);
		if (WRITE_DATA) {
			auto value = unsafe_yyjson_get_sint(val);
			memcpy(blob_data + blob_offset_data[result_index], const_data_ptr_cast(&value), sizeof(int64_t));
		}
		OffsetData::AddBlobOffset(blob_offset_data[result_index], sizeof(int64_t));
		break;
	}
	case YYJSON_TYPE_RAW | YYJSON_SUBTYPE_NONE:
	case YYJSON_TYPE_NUM | YYJSON_SUBTYPE_REAL: {
		WriteVariantMetadata<WRITE_DATA>(result, result_index, values_offset_data, blob_offset_data[result_index],
		                                 nullptr, 0, VariantLogicalType::DOUBLE);
		double value;
		if (WRITE_DATA) {
			if (json_tag == (YYJSON_TYPE_RAW | YYJSON_SUBTYPE_NONE)) {
				auto success = TryCast::Operation<string_t, double>(GetString(val), value, true);
				if (!success) {
					return false;
				}
			} else {
				value = unsafe_yyjson_get_real(val);
			}
			memcpy(blob_data + blob_offset_data[result_index], const_data_ptr_cast(&value), sizeof(double));
		}
		OffsetData::AddBlobOffset(blob_offset_data[result_index], sizeof(double));
		break;
	}
	default:
		throw InternalException("Unknown yyjson tag in ConvertJSON");
	}
	return true;
}

//! Convert a single value, pushing a frame for containers so their children are converted iteratively
template <bool WRITE_DATA, bool IGNORE_NULLS>
static bool ConvertJSONValue(yyjson_val *val, ToVariantGlobalResultData &result, idx_t result_index, bool is_root,
                             vector<JSONConversionFrame> &stack) {
	if (unsafe_yyjson_is_null(val)) {
		if (!IGNORE_NULLS) {
			auto values_offset_data = OffsetData::GetValues(result.offsets);
			auto blob_offset_data = OffsetData::GetBlob(result.offsets);
			HandleVariantNull<WRITE_DATA>(result, result_index, values_offset_data, blob_offset_data[result_index],
			                              nullptr, 0, is_root);
		}
		return true;
	}

	auto json_tag = unsafe_yyjson_get_tag(val);
	if (json_tag == (YYJSON_TYPE_ARR | YYJSON_SUBTYPE_NONE)) {
		stack.push_back(ConvertJSONArray<WRITE_DATA>(val, result, result_index));
		return true;
	}
	if (json_tag == (YYJSON_TYPE_OBJ | YYJSON_SUBTYPE_NONE)) {
		stack.push_back(ConvertJSONObject<WRITE_DATA>(val, result, result_index));
		return true;
	}
	return ConvertJSONPrimitive<WRITE_DATA>(val, result, result_index, is_root);
}

//! Convert a JSON document with an explicit stack so deeply nested input cannot overflow the thread stack
template <bool WRITE_DATA, bool IGNORE_NULLS>
static bool ConvertJSON(yyjson_val *val, ToVariantGlobalResultData &result, idx_t result_index, bool is_root,
                        vector<JSONConversionFrame> &stack) {
	D_ASSERT(stack.empty());
	if (!ConvertJSONValue<WRITE_DATA, IGNORE_NULLS>(val, result, result_index, is_root, stack)) {
		return false;
	}

	auto &variant = result.variant;
	auto values_offset_data = OffsetData::GetValues(result.offsets);
	while (!stack.empty()) {
		auto &frame = stack.back();
		yyjson_val *child;
		if (frame.is_object) {
			if (frame.entry_index == frame.entries.size()) {
				stack.pop_back();
				continue;
			}
			auto &entry = frame.entries[frame.entry_index++];
			if (WRITE_DATA) {
				auto key_string = GetString(entry.key);
				auto keys_index = frame.key_index++;
				auto dictionary_index = result.GetOrCreateIndex(key_string);
				auto &keys_list_entry = variant.keys_data[result_index];

				//! Set the keys_index
				variant.keys_index_data[frame.child_index] = keys_index;
				//! Set the values_index
				variant.values_index_data[frame.child_index++] = values_offset_data[result_index];
				result.keys_selvec.set_index(keys_list_entry.offset + keys_index, dictionary_index);
			}
			child = entry.value;
		} else {
			if (!yyjson_arr_iter_has_next(&frame.arr_iter)) {
				stack.pop_back();
				continue;
			}
			child = yyjson_arr_iter_next(&frame.arr_iter);
			if (WRITE_DATA) {
				//! Set the child index
				variant.keys_index_validity.SetInvalid(frame.child_index);
				variant.values_index_data[frame.child_index++] = values_offset_data[result_index];
			}
		}
		//! Converting the child may push a new frame, invalidating 'frame'
		if (!ConvertJSONValue<WRITE_DATA, IGNORE_NULLS>(child, result, result_index, false, stack)) {
			return false;
		}
	}
	return true;
}

template <bool WRITE_DATA, bool IGNORE_NULLS>
bool ConvertJSONToVariant(ToVariantSourceData &source, ToVariantGlobalResultData &result, idx_t count,
                          optional_ptr<const SelectionVector> selvec,
                          optional_ptr<const SelectionVector> values_index_selvec, const bool is_root) {
	D_ASSERT(source.vec.GetType().IsJSONType());

	auto &source_format = source.source_format;
	auto source_data = source_format.GetData<string_t>(source_format);

	auto values_offset_data = OffsetData::GetValues(result.offsets);
	auto blob_offset_data = OffsetData::GetBlob(result.offsets);

	auto &variant = result.variant;
	JSONAllocator json_allocator(result.allocator);
	vector<JSONConversionFrame> stack;
	for (idx_t i = 0; i < count; i++) {
		auto source_index = source[i];
		auto result_index = selvec ? selvec->get_index(i) : i;

		if (!source_format.validity.RowIsValid(source_index)) {
			if (!IGNORE_NULLS) {
				HandleVariantNull<WRITE_DATA>(result, result_index, values_offset_data, blob_offset_data[result_index],
				                              values_index_selvec, i, is_root);
			}
			continue;
		}

		if (WRITE_DATA && values_index_selvec) {
			//! Write the 'values_index' for the parent of this column
			variant.values_index_data[values_index_selvec->get_index(i)] = values_offset_data[result_index];
		}

		auto &val = source_data[source_index];
		auto doc = yyjson_read_opts(val.GetDataWriteable(), val.GetSize(),
		                            YYJSON_READ_ALLOW_INF_AND_NAN | YYJSON_READ_ALLOW_TRAILING_COMMAS |
		                                YYJSON_READ_BIGNUM_AS_RAW,
		                            json_allocator.GetYYAlc(), nullptr);
		if (!doc) {
			json_allocator.Reset();
			if (!IGNORE_NULLS) {
				HandleVariantNull<WRITE_DATA>(result, result_index, values_offset_data, blob_offset_data[result_index],
				                              values_index_selvec, i, is_root);
			}
			continue;
		}
		auto *root = yyjson_doc_get_root(doc);

		if (!ConvertJSON<WRITE_DATA, IGNORE_NULLS>(root, result, result_index, is_root, stack)) {
			return false;
		}
		json_allocator.Reset();
	}
	return true;
}

} // namespace variant
} // namespace duckdb
