#include "duckdb/function/cast/nested_to_varchar_cast.hpp"
#include "duckdb/common/vector/constant_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/map_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/function/cast/default_casts.hpp"
#include "duckdb/function/cast/bound_cast_data.hpp"
#include "duckdb/function/cast/vector_cast_helpers.hpp"

namespace duckdb {

StructCastFieldMapping StructBoundCastData::GetFieldMapping(const LogicalType &source, const LogicalType &target) {
	StructCastFieldMapping result;
	auto &source_children = StructType::GetChildTypes(source);
	auto &target_children = StructType::GetChildTypes(target);

	auto target_is_unnamed = target_children.empty() || StructType::IsUnnamed(target);
	auto source_is_unnamed = source_children.empty() || StructType::IsUnnamed(source);

	auto is_unnamed = target_is_unnamed || source_is_unnamed;
	if (is_unnamed) {
		// Unnamed structs cast positionally
		if (source_children.size() != target_children.size()) {
			result.kind = StructCastFieldMapping::MatchKind::SIZE_MISMATCH;
			return result;
		}
		for (idx_t i = 0; i < source_children.size(); i++) {
			result.source_indexes.push_back(i);
			result.target_indexes.push_back(i);
		}
		return result;
	}

	identifier_map_t<idx_t> target_children_map;
	for (idx_t i = 0; i < target_children.size(); i++) {
		auto &name = target_children[i].first;
		if (target_children_map.find(name) != target_children_map.end()) {
			result.kind = StructCastFieldMapping::MatchKind::DUPLICATE_TARGET_NAME;
			result.duplicate_name = name;
			return result;
		}
		target_children_map[name] = i;
	}

	vector<bool> target_matched(target_children.size(), false);
	for (idx_t i = 0; i < source_children.size(); i++) {
		auto target_child = target_children_map.find(source_children[i].first);
		if (target_child == target_children_map.end() || target_matched[target_child->second]) {
			// Skip source children without a target, and duplicate source names beyond the first match
			continue;
		}
		target_matched[target_child->second] = true;
		result.source_indexes.push_back(i);
		result.target_indexes.push_back(target_child->second);
	}

	if (result.source_indexes.empty()) {
		result.kind = StructCastFieldMapping::MatchKind::NO_MATCHING_FIELD;
		return result;
	}

	// The remaining target children have no match in the source struct.
	// Thus, they become NULL.
	for (idx_t i = 0; i < target_children.size(); i++) {
		if (!target_matched[i]) {
			result.target_null_indexes.push_back(i);
		}
	}
	return result;
}

unique_ptr<BoundCastData> StructBoundCastData::BindStructToStructCast(BindCastInput &input, const LogicalType &source,
                                                                      const LogicalType &target) {
	auto mapping = GetFieldMapping(source, target);
	switch (mapping.kind) {
	case StructCastFieldMapping::MatchKind::SIZE_MISMATCH:
		throw TypeMismatchException(input.query_location, source, target, "Cannot cast STRUCTs of different size");
	case StructCastFieldMapping::MatchKind::DUPLICATE_TARGET_NAME:
		throw NotImplementedException("Error while casting - duplicate name %s in struct", mapping.duplicate_name);
	case StructCastFieldMapping::MatchKind::NO_MATCHING_FIELD:
		throw BinderException("STRUCT to STRUCT cast must have at least one matching member, inputs are (%s) and (%s)",
		                      source.ToString(), target.ToString());
	default:
		break;
	}

	auto &source_children = StructType::GetChildTypes(source);
	auto &target_children = StructType::GetChildTypes(target);
	vector<BoundCastInfo> child_cast_info;
	for (idx_t i = 0; i < mapping.source_indexes.size(); i++) {
		auto &source_child_type = source_children[mapping.source_indexes[i]].second;
		auto &target_child_type = target_children[mapping.target_indexes[i]].second;
		child_cast_info.push_back(input.GetCastFunction(source_child_type, target_child_type));
	}

	return make_uniq<StructBoundCastData>(std::move(child_cast_info), target, std::move(mapping.source_indexes),
	                                      std::move(mapping.target_indexes), std::move(mapping.target_null_indexes));
}

unique_ptr<FunctionLocalState> StructBoundCastData::InitStructCastLocalState(CastLocalStateParameters &parameters) {
	auto &cast_data = parameters.cast_data->Cast<StructBoundCastData>();
	auto result = make_uniq<StructCastLocalState>();

	for (auto &entry : cast_data.child_cast_info) {
		unique_ptr<FunctionLocalState> child_state;
		if (entry.HasInitLocalState()) {
			CastLocalStateParameters child_params(parameters, entry.GetCastData());
			child_state = entry.InitLocalState(child_params);
		}
		result->local_states.push_back(std::move(child_state));
	}
	return std::move(result);
}

bool StructBoundCastData::StructToStructCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	auto &cast_data = parameters.cast_data->Cast<StructBoundCastData>();
	auto &l_state = parameters.local_state->Cast<StructCastLocalState>();

	auto &source_vectors = StructVector::GetEntries(source);
	auto &target_children = StructVector::GetEntries(result);

	bool all_converted = true;
	for (idx_t i = 0; i < cast_data.source_indexes.size(); i++) {
		auto source_idx = cast_data.source_indexes[i];
		auto target_idx = cast_data.target_indexes[i];

		auto &source_vector = source_vectors[source_idx];
		auto &target_vector = target_children[target_idx];

		auto &child_cast_info = cast_data.child_cast_info[i];
		CastParameters child_parameters(parameters, child_cast_info.GetCastData(), l_state.local_states[i]);
		auto success = child_cast_info.Cast(source_vector, target_vector, count, child_parameters);
		if (!success) {
			all_converted = false;
		}
	}

	if (!cast_data.target_null_indexes.empty()) {
		for (idx_t i = 0; i < cast_data.target_null_indexes.size(); i++) {
			auto target_idx = cast_data.target_null_indexes[i];
			auto &target_vector = target_children[target_idx];

			ConstantVector::SetNull(target_vector, count_t(count));
		}
	}
	FlatVector::CopyValidity(result, source, count);
	FlatVector::SetSize(result, count);
	result.Verify();
	return all_converted;
}

static bool StructToVarcharCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	// renders a named STRUCT as "{'name': value, ...}" - unnamed structs (TUPLEs) are handled by TupleToVarcharCast
	// first cast all child elements to varchar
	auto &cast_data = parameters.cast_data->Cast<StructBoundCastData>();
	Vector varchar_struct(cast_data.target, count);
	FlatVector::SetSize(varchar_struct, count);
	StructBoundCastData::StructToStructCast(source, varchar_struct, count, parameters);
	auto &base_children = StructVector::GetEntries(source);

	// now construct the actual varchar vector
	auto &child_types = StructType::GetChildTypes(source.GetType());
	auto &children = StructVector::GetEntries(varchar_struct);
	auto source_validity = varchar_struct.Validity();
	vector<VectorIterator<string_t>> child_iterators;
	for (auto &child : children) {
		child_iterators.emplace_back(child.Values<string_t>());
	}
	static constexpr idx_t SEP_LENGTH = 2;
	static constexpr idx_t NAME_SEP_LENGTH = 2;
	static constexpr idx_t NULL_LENGTH = 4;
	auto key_needs_quotes = make_unsafe_uniq_array_uninitialized<bool>(children.size());
	auto value_needs_quotes = make_unsafe_uniq_array_uninitialized<bool>(children.size());

	auto result_data = FlatVector::Writer<string_t>(result, count);
	for (idx_t r = 0; r < count; r++) {
		if (!source_validity.IsValid(r)) {
			result_data.WriteNull();
			continue;
		}

		//! Calculate the total length of the row
		idx_t string_length = 2; // {}
		for (idx_t c = 0; c < children.size(); c++) {
			if (c > 0) {
				string_length += SEP_LENGTH;
			}
			auto add_escapes = !base_children[c].GetType().IsNested();
			auto string_length_func = add_escapes ? VectorCastHelpers::CalculateEscapedStringLength<false>
			                                      : VectorCastHelpers::CalculateStringLength;
			auto &child_data = child_iterators[c];
			auto &name = child_types[c].first;
			string_length +=
			    VectorCastHelpers::CalculateEscapedStringLength<true>(name.GetIdentifierName(), key_needs_quotes[c]);
			string_length += NAME_SEP_LENGTH; // ": "
			auto child_entry = child_data[r];
			if (child_entry.IsValid()) {
				//! Skip the `\`, not a special character outside quotes
				string_length += string_length_func(child_entry.GetValue(), value_needs_quotes[c]);
			} else {
				string_length += NULL_LENGTH;
			}
		}

		auto &result_str = result_data.WriteEmptyString(string_length);
		auto dataptr = result_str.GetDataWriteable();

		//! Serialize the struct to the string
		idx_t offset = 0;
		dataptr[offset++] = '{';
		for (idx_t c = 0; c < children.size(); c++) {
			if (c > 0) {
				memcpy(dataptr + offset, ", ", SEP_LENGTH);
				offset += SEP_LENGTH;
			}
			auto add_escapes = !base_children[c].GetType().IsNested();
			auto write_string_func =
			    add_escapes ? VectorCastHelpers::WriteEscapedString<false> : VectorCastHelpers::WriteString;

			auto &name = child_types[c].first;
			// "{<name>: <value>}"
			offset += VectorCastHelpers::WriteEscapedString<true>(dataptr + offset, name.GetIdentifierName(),
			                                                      key_needs_quotes[c]);
			dataptr[offset++] = ':';
			dataptr[offset++] = ' ';
			// value
			auto &child_data = child_iterators[c];
			auto child_entry = child_data[r];
			if (child_entry.IsValid()) {
				//! Skip the `\`, not a special character outside quotes
				offset += write_string_func(dataptr + offset, child_entry.GetValue(), value_needs_quotes[c]);
			} else {
				memcpy(dataptr + offset, "NULL", NULL_LENGTH);
				offset += NULL_LENGTH;
			}
		}
		dataptr[offset++] = '}';
		result_str.Finalize();
	}
	return true;
}

unique_ptr<BoundCastData> StructToMapBoundCastData::BindStructToMapCast(BindCastInput &input, const LogicalType &source,
                                                                        const LogicalType &target) {
	if (StructType::IsUnnamed(source)) {
		throw TypeMismatchException(input.query_location, source, target, "Cannot cast unnamed STRUCTs to MAP");
	}
	// Get a cast function for the keys (struct has varchar keys, map has single-type keys)
	auto key_cast = input.GetCastFunction(LogicalType::VARCHAR, MapType::KeyType(target));
	// Get cast function for the values (struct may have different value types per key, map has single-type values)
	vector<BoundCastInfo> value_casts;
	auto target_value_type = MapType::ValueType(target);
	for (idx_t i = 0; i < StructType::GetChildCount(source); i++) {
		auto value_cast = input.GetCastFunction(StructType::GetChildType(source, i), target_value_type);
		value_casts.push_back(std::move(value_cast));
	}
	return make_uniq<StructToMapBoundCastData>(std::move(key_cast), std::move(value_casts));
}

unique_ptr<FunctionLocalState>
StructToMapBoundCastData::InitStructToMapCastLocalState(CastLocalStateParameters &parameters) {
	auto &cast_data = parameters.cast_data->Cast<StructToMapBoundCastData>();
	auto result = make_uniq<StructToMapCastLocalState>();

	if (cast_data.key_cast.HasInitLocalState()) {
		CastLocalStateParameters child_params(parameters, cast_data.key_cast.GetCastData());
		result->key_state = cast_data.key_cast.InitLocalState(child_params);
	}

	for (auto &entry : cast_data.value_casts) {
		unique_ptr<FunctionLocalState> child_state;
		if (entry.HasInitLocalState()) {
			CastLocalStateParameters child_params(parameters, entry.GetCastData());
			child_state = entry.InitLocalState(child_params);
		}
		result->value_states.push_back(std::move(child_state));
	}
	return std::move(result);
}

static bool StructToMapCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	auto &cast_data = parameters.cast_data->Cast<StructToMapBoundCastData>();
	auto &local_state = parameters.local_state->Cast<StructToMapCastLocalState>();

	// Grab all struct columns
	auto &source_children = StructVector::GetEntries(source);
	idx_t field_count = source_children.size();
	idx_t total_count = count * field_count;

	// The STRUCT field names become the MAP keys, so the key cast is independent of the row data.
	// When every row is NULL the result is an all-NULL MAP: skip the key cast entirely so that the
	// field-name -> key-type conversion is not forced (and cannot throw) for an all-NULL input.
	auto validity_entries = source.Validity();
	bool has_valid_row = false;
	for (idx_t i = 0; i < count; i++) {
		if (validity_entries.IsValid(i)) {
			has_valid_row = true;
			break;
		}
	}
	if (!has_valid_row) {
		auto null_list_data = FlatVector::Writer<list_entry_t>(result, count);
		for (idx_t i = 0; i < count; i++) {
			null_list_data.WriteNull();
		}
		ListVector::SetListSize(result, 0);
		return true;
	}

	// Allocate result
	ListVector::Reserve(result, total_count);

	// Create key vector with VARCHAR keys
	Vector varchar_keys(LogicalType::VARCHAR, field_count);
	auto &field_types = StructType::GetChildTypes(source.GetType());
	auto key_data = FlatVector::Writer<string_t>(varchar_keys, field_count);
	for (idx_t field_idx = 0; field_idx < field_count; field_idx++) {
		auto &field_name = field_types[field_idx].first;
		key_data.WriteValue(field_name.GetIdentifierName());
	}

	// Cast keys to result type
	auto &map_keys = MapVector::GetKeys(result);
	CastParameters key_parameters(parameters, cast_data.key_cast.GetCastData(), local_state.key_state);
	auto keys_converted = cast_data.key_cast.Cast(varchar_keys, map_keys, field_count, key_parameters);

	// Slice the map keys to create the dictionary
	SelectionVector slice_sel(total_count);
	for (idx_t r = 0; r < count; r++) {
		for (idx_t f = 0; f < field_count; f++) {
			slice_sel.set_index(r * field_count + f, f);
		}
	}
	map_keys.Slice(slice_sel, total_count);

	// Fill the values vector
	bool values_converted = true;
	auto &map_values = MapVector::GetValues(result);
	for (idx_t field_idx = 0; field_idx < field_count; field_idx++) {
		auto &source_field = source_children[field_idx];
		Vector temp_converted(MapType::ValueType(result.GetType()), count);
		CastParameters child_params(parameters, cast_data.value_casts[field_idx].GetCastData(),
		                            local_state.value_states[field_idx]);
		auto success = cast_data.value_casts[field_idx].Cast(source_field, temp_converted, count, child_params);
		if (!success) {
			values_converted = false;
		}

		// Interleave results
		for (idx_t row_idx = 0; row_idx < count; row_idx++) {
			auto target_idx = row_idx * field_count + field_idx;
			// Copy also does a validity check
			VectorOperations::Copy(temp_converted, map_values, row_idx + 1, row_idx, target_idx);
		}
	}

	// Check for nulls in the source rows, and set the list data
	auto list_data = FlatVector::Writer<list_entry_t>(result, count);
	for (idx_t i = 0; i < count; i++) {
		if (!validity_entries.IsValid(i)) { // is row null?
			list_data.WriteNull();
			continue;
		}
		list_data.WriteValue(list_entry_t(i * field_count, field_count));
	}
	// Set the size
	ListVector::SetListSize(result, total_count);

	return keys_converted && values_converted;
}

BoundCastInfo DefaultCasts::StructCastSwitch(BindCastInput &input, const LogicalType &source,
                                             const LogicalType &target) {
	switch (target.id()) {
	case LogicalTypeId::STRUCT:
	case LogicalTypeId::TUPLE:
		return BoundCastInfo(StructBoundCastData::StructToStructCast,
		                     StructBoundCastData::BindStructToStructCast(input, source, target),
		                     StructBoundCastData::InitStructCastLocalState);
	case LogicalTypeId::VARCHAR: {
		// bind a cast in which we convert all child entries to VARCHAR entries
		auto &struct_children = StructType::GetChildTypes(source);
		child_list_t<LogicalType> varchar_children;
		for (auto &child_entry : struct_children) {
			varchar_children.push_back(make_pair(child_entry.first, LogicalType::VARCHAR));
		}
		auto varchar_type = LogicalType::STRUCT(varchar_children);
		auto cast_data = StructBoundCastData::BindStructToStructCast(input, source, varchar_type);
		auto &struct_cast_data = cast_data->Cast<StructBoundCastData>();
		for (idx_t i = 0; i < struct_cast_data.child_cast_info.size(); i++) {
			NestedToVarcharCast::WrapChildCast(struct_cast_data.child_cast_info[i],
			                                   struct_children[struct_cast_data.source_indexes[i]].second);
		}
		return BoundCastInfo(StructToVarcharCast, std::move(cast_data), StructBoundCastData::InitStructCastLocalState);
	}
	case LogicalTypeId::MAP: {
		return BoundCastInfo(StructToMapCast, StructToMapBoundCastData::BindStructToMapCast(input, source, target),
		                     StructToMapBoundCastData::InitStructToMapCastLocalState);
	}
	default:
		return TryVectorNullCast;
	}
}

} // namespace duckdb
