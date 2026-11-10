#include "duckdb/function/cast/nested_to_varchar_cast.hpp"
#include "duckdb/common/vector/constant_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/map_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/function/cast/default_casts.hpp"
#include "duckdb/function/cast/cast_function_set.hpp"
#include "duckdb/function/cast/bound_cast_data.hpp"
#include "duckdb/function/cast/vector_cast_helpers.hpp"

namespace duckdb {

unique_ptr<BoundCastData> MapBoundCastData::BindMapToMapCast(BindCastInput &input, const LogicalType &source,
                                                             const LogicalType &target) {
	vector<BoundCastInfo> child_cast_info;
	auto source_key = MapType::KeyType(source);
	auto target_key = MapType::KeyType(target);
	auto source_val = MapType::ValueType(source);
	auto target_val = MapType::ValueType(target);
	auto key_cast = input.GetCastFunction(source_key, target_key);
	auto value_cast = input.GetCastFunction(source_val, target_val);
	return make_uniq<MapBoundCastData>(std::move(key_cast), std::move(value_cast));
}

static bool MapToVarcharCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	auto constant = source.GetVectorType() == VectorType::CONSTANT_VECTOR;
	auto varchar_type = LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR);
	Vector varchar_map(varchar_type, count);
	FlatVector::SetSize(varchar_map, count);

	// since map's physical type is a list, the ListCast can be utilized
	ListCast::ListToListCast(source, varchar_map, count, parameters);

	varchar_map.Flatten();
	auto &validity = FlatVector::ValidityMutable(varchar_map);
	auto &key_str = MapVector::GetKeys(varchar_map);
	auto &val_str = MapVector::GetValues(varchar_map);

	key_str.Flatten();
	val_str.Flatten();

	auto list_data = FlatVector::GetData<list_entry_t>(varchar_map);
	auto key_data = FlatVector::GetData<string_t>(key_str);
	auto val_data = FlatVector::GetData<string_t>(val_str);
	auto &key_validity = FlatVector::Validity(key_str);
	auto &val_validity = FlatVector::Validity(val_str);
	auto &struct_validity = FlatVector::Validity(ListVector::GetChild(varchar_map));

	//! {key=value[, ]}
	static constexpr const idx_t SEP_LENGTH = 2;
	static constexpr const idx_t KEY_VALUE_SEP_LENGTH = 1;
	static constexpr const idx_t NULL_LENGTH = 4;
	static constexpr const idx_t INVALID_LENGTH = 7;

	auto &key_vec = MapVector::GetKeys(source);
	auto &value_vec = MapVector::GetValues(source);

	auto key_is_nested = key_vec.GetType().IsNested();
	auto value_is_nested = value_vec.GetType().IsNested();

	auto key_strlen_func = key_is_nested ? VectorCastHelpers::CalculateStringLength
	                                     : VectorCastHelpers::CalculateEscapedStringLength<false>;
	auto key_write_func = key_is_nested ? VectorCastHelpers::WriteString : VectorCastHelpers::WriteEscapedString<false>;

	auto value_strlen_func = value_is_nested ? VectorCastHelpers::CalculateStringLength
	                                         : VectorCastHelpers::CalculateEscapedStringLength<false>;
	auto value_write_func =
	    value_is_nested ? VectorCastHelpers::WriteString : VectorCastHelpers::WriteEscapedString<false>;

	unsafe_unique_array<bool> key_needs_quotes;
	unsafe_unique_array<bool> value_needs_quotes;
	idx_t needs_quotes_length = DConstants::INVALID_INDEX;
	auto result_data = FlatVector::Writer<string_t>(result, count);
	for (idx_t i = 0; i < count; i++) {
		if (!validity.RowIsValid(i)) {
			result_data.WriteNull();
			continue;
		}

		idx_t string_length = 2; // {}
		auto list = list_data[i];

		if (!key_needs_quotes || list.length > needs_quotes_length) {
			key_needs_quotes = make_unsafe_uniq_array_uninitialized<bool>(list.length);
			value_needs_quotes = make_unsafe_uniq_array_uninitialized<bool>(list.length);
			needs_quotes_length = list.length;
		}
		for (idx_t list_idx = 0; list_idx < list.length; list_idx++) {
			if (list_idx > 0) {
				string_length += SEP_LENGTH;
			}

			auto idx = list.offset + list_idx;

			if (!struct_validity.RowIsValid(idx)) {
				string_length += NULL_LENGTH;
				continue;
			}
			if (!key_validity.RowIsValid(idx)) {
				// throw InternalException("Error in map: key validity invalid?!");
				string_length += INVALID_LENGTH;
				continue;
			}
			string_length += key_strlen_func(key_data[idx], key_needs_quotes[list_idx]);
			string_length += KEY_VALUE_SEP_LENGTH;
			if (val_validity.RowIsValid(idx)) {
				string_length += value_strlen_func(val_data[idx], value_needs_quotes[list_idx]);
			} else {
				string_length += NULL_LENGTH;
			}
		}
		auto &result_str = result_data.WriteEmptyString(string_length);
		auto dataptr = result_str.GetDataWriteable();
		idx_t offset = 0;

		dataptr[offset++] = '{';
		for (idx_t list_idx = 0; list_idx < list.length; list_idx++) {
			if (list_idx > 0) {
				memcpy(dataptr + offset, ", ", SEP_LENGTH);
				offset += SEP_LENGTH;
			}

			auto idx = list.offset + list_idx;
			if (!struct_validity.RowIsValid(idx)) {
				memcpy(dataptr + offset, "NULL", NULL_LENGTH);
				offset += NULL_LENGTH;
				continue;
			}
			if (!key_validity.RowIsValid(idx)) {
				// throw InternalException("Error in map: key validity invalid?!");
				memcpy(dataptr + offset, "invalid", INVALID_LENGTH);
				offset += INVALID_LENGTH;
				continue;
			}
			offset += key_write_func(dataptr + offset, key_data[idx], key_needs_quotes[list_idx]);
			dataptr[offset++] = '=';
			if (val_validity.RowIsValid(idx)) {
				offset += value_write_func(dataptr + offset, val_data[idx], value_needs_quotes[list_idx]);
			} else {
				memcpy(dataptr + offset, "NULL", NULL_LENGTH);
				offset += NULL_LENGTH;
			}
		}
		dataptr[offset++] = '}';
		result_str.Finalize();
	}

	if (constant) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
	return true;
}

static bool MapToMapCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	bool all_valid = ListCast::ListToListCast(source, result, count, parameters);

	// CAST: throw exception if error occurred.
	if (!parameters.error_message) {
		if (!all_valid) {
			HandleCastError::AssignError("Failed to cast map entries", parameters);
			// throws exceptions
		}
		MapVector::MapConversionVerify(result, count);
		return true;
	}

	// TRY_CAST: error message, so we need to nullify the invalid map entries.
	SelectionVector row_sel(1);
	// Return true if the map is invalid and we need to nullify the entry.
	auto nullify_invalid_map = [&](MapInvalidReason reason) {
		if (reason == MapInvalidReason::VALID) {
			return false;
		}
		auto error_message =
		    reason == MapInvalidReason::DUPLICATE_KEY ? "Map keys must be unique." : "Map keys can not be NULL.";
		D_ASSERT(parameters.error_message);
		HandleCastError::AssignError(error_message, parameters);
		all_valid = false;
		return true;
	};

	// Special handle constant vector, which only needs to handle one map entry.
	if (result.GetVectorType() == VectorType::CONSTANT_VECTOR) {
		if (nullify_invalid_map(MapVector::CheckMapValidity(result, count))) {
			ConstantVector::SetNull(result, count_t(count));
		}
		return all_valid;
	}

	auto &result_validity = FlatVector::ValidityMutable(result);
	for (idx_t idx = 0; idx < count; ++idx) {
		if (!result_validity.RowIsValid(idx)) {
			continue;
		}
		row_sel.set_index(0, idx);
		if (nullify_invalid_map(MapVector::CheckMapValidity(result, /*count=*/1, row_sel))) {
			result_validity.SetInvalid(idx);
		}
	}
	return all_valid;
}

BoundCastInfo DefaultCasts::MapCastSwitch(BindCastInput &input, const LogicalType &source, const LogicalType &target) {
	switch (target.id()) {
	case LogicalTypeId::MAP:
		return BoundCastInfo(MapToMapCast, ListBoundCastData::BindListToListCast(input, source, target),
		                     ListBoundCastData::InitListLocalState);
	case LogicalTypeId::VARCHAR: {
		auto varchar_type = LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR);
		auto cast_data = ListBoundCastData::BindListToListCast(input, source, varchar_type);
		// the entries are cast as STRUCT(key, value)
		auto entry_cast_data = cast_data->Cast<ListBoundCastData>().child_cast_info.GetCastData();
		if (entry_cast_data) {
			auto &struct_cast_data = entry_cast_data->Cast<StructBoundCastData>();
			D_ASSERT(struct_cast_data.child_cast_info.size() == 2);
			auto &entry_children = StructType::GetChildTypes(ListType::GetChildType(source));
			for (idx_t i = 0; i < struct_cast_data.child_cast_info.size(); i++) {
				NestedToVarcharCast::WrapChildCast(struct_cast_data.child_cast_info[i],
				                                   entry_children[struct_cast_data.source_indexes[i]].second);
			}
		}
		return BoundCastInfo(MapToVarcharCast, std::move(cast_data), ListBoundCastData::InitListLocalState);
	}
	default:
		return TryVectorNullCast;
	}
}

} // namespace duckdb
