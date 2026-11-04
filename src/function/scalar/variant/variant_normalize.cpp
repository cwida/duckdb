#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/common/vector/variant_vector.hpp"
#include "duckdb/function/scalar/regexp.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/execution/expression_executor.hpp"

#include "duckdb/function/cast/variant/to_variant_fwd.hpp"
#include "duckdb/common/types/variant_visitor.hpp"
#include "duckdb/function/variant/variant_normalize.hpp"
#include "duckdb/function/scalar/variant_functions.hpp"

namespace duckdb {

VariantNormalizerState::VariantNormalizerState(idx_t result_row, VariantVectorData &source,
                                               OrderedOwningStringMap<uint32_t> &dictionary,
                                               SelectionVector &keys_selvec)
    : source(source), dictionary(dictionary), keys_selvec(keys_selvec),
      keys_index_validity(source.keys_index_validity) {
	auto keys_list_entry = source.keys_data[result_row];
	auto values_list_entry = source.values_data[result_row];
	auto children_list_entry = source.children_data[result_row];

	keys_offset = keys_list_entry.offset;
	children_offset = children_list_entry.offset;

	blob_data = data_ptr_cast(source.blob_data[result_row].GetDataWriteable());
	type_ids = source.type_ids_data + values_list_entry.offset;
	byte_offsets = source.byte_offset_data + values_list_entry.offset;
	values_indexes = source.values_index_data + children_list_entry.offset;
	keys_indexes = source.keys_index_data + children_list_entry.offset;
}

data_ptr_t VariantNormalizerState::GetDestination() {
	return blob_data + blob_size;
}
uint32_t VariantNormalizerState::GetOrCreateIndex(const string_t &key) {
	auto unsorted_idx = dictionary.size();
	//! This will later be remapped to the sorted idx (see FinalizeVariantKeys in 'to_variant.cpp')
	return dictionary.emplace(std::make_pair(key, unsorted_idx)).first->second;
}

void VariantNormalizer::VisitNull(VariantNormalizerState &state) {
	return;
}
void VariantNormalizer::VisitBoolean(bool val, VariantNormalizerState &state) {
	return;
}

void VariantNormalizer::VisitMetadata(VariantLogicalType type_id, VariantNormalizerState &state) {
	state.type_ids[state.values_size] = static_cast<uint8_t>(type_id);
	state.byte_offsets[state.values_size] = state.blob_size;
	state.values_size++;
}

void VariantNormalizer::VisitFloat(float val, VariantNormalizerState &state) {
	VisitInteger(val, state);
}
void VariantNormalizer::VisitDouble(double val, VariantNormalizerState &state) {
	VisitInteger(val, state);
}
void VariantNormalizer::VisitUUID(hugeint_t val, VariantNormalizerState &state) {
	VisitInteger(val, state);
}
void VariantNormalizer::VisitDate(date_t val, VariantNormalizerState &state) {
	VisitInteger(val, state);
}
void VariantNormalizer::VisitInterval(interval_t val, VariantNormalizerState &state) {
	VisitInteger(val, state);
}
void VariantNormalizer::VisitTime(dtime_t val, VariantNormalizerState &state) {
	VisitInteger(val, state);
}
void VariantNormalizer::VisitTimeNanos(dtime_ns_t val, VariantNormalizerState &state) {
	VisitInteger(val, state);
}
void VariantNormalizer::VisitTimeTZ(dtime_tz_t val, VariantNormalizerState &state) {
	VisitInteger(val, state);
}
void VariantNormalizer::VisitTimestampSec(timestamp_sec_t val, VariantNormalizerState &state) {
	VisitInteger(val, state);
}
void VariantNormalizer::VisitTimestampMs(timestamp_ms_t val, VariantNormalizerState &state) {
	VisitInteger(val, state);
}
void VariantNormalizer::VisitTimestamp(timestamp_t val, VariantNormalizerState &state) {
	VisitInteger(val, state);
}
void VariantNormalizer::VisitTimestampNanos(timestamp_ns_t val, VariantNormalizerState &state) {
	VisitInteger(val, state);
}
void VariantNormalizer::VisitTimestampTZ(timestamp_tz_t val, VariantNormalizerState &state) {
	VisitInteger(val, state);
}
void VariantNormalizer::VisitTimestampTZNanos(timestamp_tz_ns_t val, VariantNormalizerState &state) {
	VisitInteger(val, state);
}

void VariantNormalizer::VisitString(const string_t &str, VariantNormalizerState &state) {
	auto length = str.GetSize();
	state.blob_size += VarintEncode(length, state.GetDestination());
	memcpy(state.GetDestination(), str.GetData(), length);
	state.blob_size += length;
}
void VariantNormalizer::VisitBlob(const string_t &blob, VariantNormalizerState &state) {
	return VisitString(blob, state);
}
void VariantNormalizer::VisitBignum(const string_t &bignum, VariantNormalizerState &state) {
	return VisitString(bignum, state);
}
void VariantNormalizer::VisitGeometry(const string_t &geom, VariantNormalizerState &state) {
	return VisitString(geom, state);
}
void VariantNormalizer::VisitBitstring(const string_t &bits, VariantNormalizerState &state) {
	return VisitString(bits, state);
}

void VariantNormalizer::VisitArray(const UnifiedVariantVectorData &variant, idx_t row,
                                   const VariantNestedData &nested_data, idx_t depth, VariantNormalizerState &state) {
	state.blob_size += VarintEncode(nested_data.child_count, state.GetDestination());
	if (!nested_data.child_count) {
		return;
	}
	idx_t result_children_idx = state.children_size;
	state.blob_size += VarintEncode(result_children_idx, state.GetDestination());
	state.children_size += nested_data.child_count;

	for (idx_t i = 0; i < nested_data.child_count; i++) {
		auto source_children_idx = nested_data.children_idx + i;
		auto values_index = variant.GetValuesIndex(row, source_children_idx);

		//! Set the 'values_index' for the child, and set the 'keys_index' to NULL
		state.values_indexes[result_children_idx] = state.values_size;
		state.keys_index_validity.SetInvalid(state.children_offset + result_children_idx);
		result_children_idx++;

		//! Visit the child value
		VariantVisitor<VariantNormalizer>::Visit(variant, row, values_index, depth + 1, state);
	}
}

void VariantNormalizer::VisitObject(const UnifiedVariantVectorData &variant, idx_t row,
                                    const VariantNestedData &nested_data, idx_t depth, VariantNormalizerState &state) {
	state.blob_size += VarintEncode(nested_data.child_count, state.GetDestination());
	if (!nested_data.child_count) {
		return;
	}
	uint32_t children_idx = state.children_size;
	uint32_t keys_idx = state.keys_size;
	state.blob_size += VarintEncode(children_idx, state.GetDestination());
	state.children_size += nested_data.child_count;
	state.keys_size += nested_data.child_count;

	//! First iterate through all fields to populate the map of key -> field
	map<string, idx_t> sorted_fields;
	for (idx_t i = 0; i < nested_data.child_count; i++) {
		auto keys_index = variant.GetKeysIndex(row, nested_data.children_idx + i);
		auto &key = variant.GetKey(row, keys_index);
		sorted_fields.emplace(key, i);
	}

	//! Then visit the fields in sorted order
	for (auto &entry : sorted_fields) {
		auto source_children_idx = nested_data.children_idx + entry.second;

		//! Add the key of the field to the result
		auto keys_index = variant.GetKeysIndex(row, source_children_idx);
		auto &key = variant.GetKey(row, keys_index);
		auto dict_index = state.GetOrCreateIndex(key);
		state.keys_selvec.set_index(state.keys_offset + keys_idx, dict_index);

		//! Visit the child value
		auto values_index = variant.GetValuesIndex(row, source_children_idx);
		state.values_indexes[children_idx] = state.values_size;
		state.keys_indexes[children_idx] = keys_idx;
		children_idx++;
		keys_idx++;
		VariantVisitor<VariantNormalizer>::Visit(variant, row, values_index, depth + 1, state);
	}
}

void VariantNormalizer::VisitDefault(VariantLogicalType type_id, const_data_ptr_t, VariantNormalizerState &state) {
	throw InternalException("VariantLogicalType(%s) not handled", EnumUtil::ToString(type_id));
}

void VariantNormalizer::Normalize(const Vector &variant_vec, Vector &result) {
	D_ASSERT(variant_vec.GetType() == LogicalType::VARIANT());
	const idx_t count = variant_vec.size();

	//! Set up the access helper for the source VARIANT
	RecursiveUnifiedVectorFormat source_format;
	Vector::RecursiveToUnifiedFormat(variant_vec, source_format);
	UnifiedVariantVectorData variant(source_format);

	//! Take the original sizes of the lists, the result will be similar size, never bigger
	auto original_keys_size = ListVector::GetTotalEntryCount(VariantVector::GetKeys(variant_vec));
	auto original_children_size = ListVector::GetTotalEntryCount(VariantVector::GetChildren(variant_vec));
	auto original_values_size = ListVector::GetTotalEntryCount(VariantVector::GetValues(variant_vec));

	auto &keys = VariantVector::GetKeys(result);
	auto &children = VariantVector::GetChildren(result);
	auto &values = VariantVector::GetValues(result);
	auto &data = VariantVector::GetData(result);

	ListVector::Reserve(keys, original_keys_size);
	ListVector::SetListSize(keys, 0);
	ListVector::Reserve(children, original_children_size);
	ListVector::SetListSize(children, 0);
	ListVector::Reserve(values, original_values_size);
	ListVector::SetListSize(values, 0);

	//! Initialize the dictionary
	auto &keys_entry = ListVector::GetChildMutable(keys);
	OrderedOwningStringMap<uint32_t> dictionary(StringVector::GetStringAllocator(keys_entry));

	VariantVectorData variant_data(result);
	SelectionVector keys_selvec;
	keys_selvec.Initialize(original_keys_size);
	for (idx_t i = 0; i < count; i++) {
		if (!variant.RowIsValid(i)) {
			FlatVector::SetNull(result, i, true);
			continue;
		}
		//! Allocate for the new data, use the same size as source
		auto &blob_data = variant_data.blob_data[i];
		auto original_data = variant.GetData(i);
		blob_data = StringVector::EmptyString(data, original_data.GetSize());

		auto &keys_list_entry = variant_data.keys_data[i];
		keys_list_entry.offset = ListVector::GetListSize(keys);

		auto &children_list_entry = variant_data.children_data[i];
		children_list_entry.offset = ListVector::GetListSize(children);

		auto &values_list_entry = variant_data.values_data[i];
		values_list_entry.offset = ListVector::GetListSize(values);

		//! Visit the source to populate the result
		VariantNormalizerState visitor_state(i, variant_data, dictionary, keys_selvec);
		VariantVisitor<VariantNormalizer>::Visit(variant, i, 0, 0, visitor_state);

		blob_data.SetSizeAndFinalize(visitor_state.blob_size, original_data.GetSize());
		keys_list_entry.length = visitor_state.keys_size;
		children_list_entry.length = visitor_state.children_size;
		values_list_entry.length = visitor_state.values_size;

		ListVector::SetListSize(keys, ListVector::GetListSize(keys) + visitor_state.keys_size);
		ListVector::SetListSize(children, ListVector::GetListSize(children) + visitor_state.children_size);
		ListVector::SetListSize(values, ListVector::GetListSize(values) + visitor_state.values_size);
	}

	VariantUtils::FinalizeVariantKeys(result, dictionary, keys_selvec, ListVector::GetListSize(keys));
	keys_entry.Slice(keys_selvec, ListVector::GetListSize(keys));
}

static void VariantNormalizeFunction(DataChunk &input, ExpressionState &state, Vector &result) {
	D_ASSERT(input.ColumnCount() == 1);
	const auto &variant_vec = input.data[0];
	VariantNormalizer::Normalize(variant_vec, result);
}

static unique_ptr<BaseStatistics> VariantNormalizeStats(ClientContext &context, FunctionStatisticsInput &input) {
	// variant_normalize re-encodes the VARIANT in a canonical binary form, but does not change any values
	return input.child_stats[0].ToUnique();
}

ScalarFunction VariantNormalizeFun::GetFunction() {
	auto variant_type = LogicalType::VARIANT();
	ScalarFunction fun("variant_normalize", {}, variant_type, VariantNormalizeFunction, nullptr, VariantNormalizeStats);
	fun.GetSignature().AddParameter("input_variant", variant_type);
	fun.SetFallible();
	return fun;
}

} // namespace duckdb
