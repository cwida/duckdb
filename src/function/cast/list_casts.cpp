#include "duckdb/function/cast/nested_to_varchar_cast.hpp"
#include "duckdb/common/vector/array_vector.hpp"
#include "duckdb/common/vector/constant_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/function/cast/default_casts.hpp"
#include "duckdb/function/cast/cast_function_set.hpp"
#include "duckdb/function/cast/bound_cast_data.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/function/cast/vector_cast_helpers.hpp"

namespace duckdb {

unique_ptr<BoundCastData> ListBoundCastData::BindListToListCast(BindCastInput &input, const LogicalType &source,
                                                                const LogicalType &target) {
	vector<BoundCastInfo> child_cast_info;
	auto &source_child_type = ListType::GetChildType(source);
	auto &result_child_type = ListType::GetChildType(target);
	auto child_cast = input.GetCastFunction(source_child_type, result_child_type);
	return make_uniq<ListBoundCastData>(std::move(child_cast));
}

static unique_ptr<BoundCastData> BindListToArrayCast(BindCastInput &input, const LogicalType &source,
                                                     const LogicalType &target) {
	vector<BoundCastInfo> child_cast_info;
	auto &source_child_type = ListType::GetChildType(source);
	auto &result_child_type = ArrayType::GetChildType(target);
	auto child_cast = input.GetCastFunction(source_child_type, result_child_type);
	return make_uniq<ListBoundCastData>(std::move(child_cast));
}

unique_ptr<FunctionLocalState> ListBoundCastData::InitListLocalState(CastLocalStateParameters &parameters) {
	auto &cast_data = parameters.cast_data->Cast<ListBoundCastData>();
	if (!cast_data.child_cast_info.HasInitLocalState()) {
		return nullptr;
	}
	CastLocalStateParameters child_parameters(parameters, cast_data.child_cast_info.GetCastData());
	return cast_data.child_cast_info.InitLocalState(child_parameters);
}

bool ListCast::ListToListCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	auto &cast_data = parameters.cast_data->Cast<ListBoundCastData>();

	auto source_data = source.Values<list_entry_t>();
	auto &source_cc = ListVector::GetChildMutable(source);
	auto source_size = ListVector::GetListSize(source);
	CastParameters child_parameters(parameters, cast_data.child_cast_info.GetCastData(), parameters.local_state);

	// the entries can reference an arbitrary subset of the child vector (e.g. after list_slice) - casting the whole
	// child then does needless work and reports errors for elements that are not part of any list
	// a constant vector repeats the same entry for every row, so only the first entry is inspected
	const auto is_constant = source.GetVectorType() == VectorType::CONSTANT_VECTOR;
	const idx_t entry_count = is_constant ? MinValue<idx_t>(count, 1) : count;
	idx_t child_count = 0;
	bool covers_child = true;
	for (idx_t r = 0; r < entry_count; r++) {
		auto source_list = source_data[r];
		if (!source_list.IsValid()) {
			continue;
		}
		auto entry = source_list.GetValue();
		covers_child = covers_child && entry.offset == child_count;
		child_count += entry.length;
	}
	covers_child = covers_child && child_count == source_size;

	if (covers_child) {
		// the lists cover the child vector exactly and in order - cast it as-is and keep the entries
		auto result_data = FlatVector::Writer<list_entry_t>(result, count);
		for (idx_t r = 0; r < count; r++) {
			auto source_list = source_data[r];
			if (!source_list.IsValid()) {
				result_data.WriteNull();
				continue;
			}
			result_data.WriteValue(source_list.GetValue());
		}
		ListVector::Reserve(result, source_size);
		auto &append_vector = ListVector::GetChildMutable(result);
		bool all_succeeded = cast_data.child_cast_info.Cast(source_cc, append_vector, source_size, child_parameters);
		ListVector::SetListSize(result, source_size);
		D_ASSERT(ListVector::GetListSize(result) == source_size);
		return all_succeeded;
	}

	// gather the referenced elements in list order, so only those are cast
	SelectionVector child_sel(child_count);
	idx_t child_offset = 0;
	for (idx_t r = 0; r < entry_count; r++) {
		auto source_list = source_data[r];
		if (!source_list.IsValid()) {
			continue;
		}
		auto entry = source_list.GetValue();
		for (idx_t i = 0; i < entry.length; i++) {
			child_sel.set_index(child_offset + i, entry.offset + i);
		}
		child_offset += entry.length;
	}

	auto result_data = FlatVector::Writer<list_entry_t>(result, count);
	child_offset = 0;
	for (idx_t r = 0; r < count; r++) {
		auto source_list = source_data[r];
		if (!source_list.IsValid()) {
			result_data.WriteNull();
			continue;
		}
		auto entry = source_list.GetValue();
		result_data.WriteValue(list_entry_t(child_offset, entry.length));
		if (!is_constant) {
			child_offset += entry.length;
		}
	}

	Vector source_slice(source_cc, child_sel, child_count);
	ListVector::Reserve(result, child_count);
	auto &append_vector = ListVector::GetChildMutable(result);
	bool all_succeeded = cast_data.child_cast_info.Cast(source_slice, append_vector, child_count, child_parameters);
	ListVector::SetListSize(result, child_count);
	return all_succeeded;
}

static bool ListToVarcharCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	// first cast the child vector to varchar[]
	Vector varchar_list(LogicalType::LIST(LogicalType::VARCHAR), count);
	FlatVector::SetSize(varchar_list, count);
	ListCast::ListToListCast(source, varchar_list, count, parameters);

	// now construct the actual varchar vector
	auto &child_vec = ListVector::GetChild(source);
	auto child_is_nested = child_vec.GetType().IsNested();
	auto string_length_func = child_is_nested ? VectorCastHelpers::CalculateStringLength
	                                          : VectorCastHelpers::CalculateEscapedStringLength<false>;
	auto write_string_func =
	    child_is_nested ? VectorCastHelpers::WriteString : VectorCastHelpers::WriteEscapedString<false>;

	auto values = varchar_list.Values<VectorListType<string_t>>();

	static constexpr idx_t SEP_LENGTH = 2;
	static constexpr idx_t NULL_LENGTH = 4;
	unsafe_unique_array<bool> needs_quotes;
	idx_t needs_quotes_length = DConstants::INVALID_INDEX;

	auto result_data = FlatVector::Writer<string_t>(result, count);
	for (idx_t i = 0; i < count; i++) {
		auto list_entry = values[i];
		if (!list_entry.IsValid()) {
			result_data.WriteNull();
			continue;
		}
		auto list = list_entry.GetValue();
		// figure out how long the result needs to be
		if (!needs_quotes || list.length > needs_quotes_length) {
			needs_quotes = make_unsafe_uniq_array_uninitialized<bool>(list.length);
			needs_quotes_length = list.length;
		}
		idx_t list_length = 2; // "[" and "]"
		idx_t list_idx = 0;
		for (auto child_value : list_entry.GetChildValues()) {
			if (list_idx > 0) {
				list_length += SEP_LENGTH; // ", "
			}
			// string length, or "NULL"
			if (child_value.IsValid()) {
				list_length += string_length_func(child_value.GetValue(), needs_quotes[list_idx]);
			} else {
				list_length += NULL_LENGTH;
			}
			list_idx++;
		}
		auto &result_str = result_data.WriteEmptyString(list_length);
		auto dataptr = result_str.GetDataWriteable();
		idx_t offset = 0;
		dataptr[offset++] = '[';
		list_idx = 0;
		for (auto child_value : list_entry.GetChildValues()) {
			if (list_idx > 0) {
				memcpy(dataptr + offset, ", ", SEP_LENGTH);
				offset += SEP_LENGTH;
			}
			if (child_value.IsValid()) {
				offset += write_string_func(dataptr + offset, child_value.GetValue(), needs_quotes[list_idx]);
			} else {
				memcpy(dataptr + offset, "NULL", NULL_LENGTH);
				offset += NULL_LENGTH;
			}
			list_idx++;
		}
		dataptr[offset] = ']';
		result_str.Finalize();
	}
	return true;
}

static bool ListToArrayCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	auto &cast_data = parameters.cast_data->Cast<ListBoundCastData>();
	auto array_size = ArrayType::GetSize(result.GetType());

	// only handle constant and flat vectors here for now
	if (source.GetVectorType() == VectorType::CONSTANT_VECTOR) {
		result.SetVectorType(source.GetVectorType());
		if (ConstantVector::IsNull(source)) {
			ConstantVector::SetNull(result, count_t(count));
			return true;
		}

		auto ldata = ConstantVector::GetData<list_entry_t>(source)[0];
		if (!ConstantVector::IsNull(source) && ldata.length != array_size) {
			// Cant cast to array, list size mismatch
			auto msg = StringUtil::Format("Cannot cast list with length %llu to array with length %u", ldata.length,
			                              array_size);
			HandleCastError::AssignError(msg, parameters);
			ConstantVector::SetNull(result, count_t(count));
			return false;
		}

		auto &source_cc = ListVector::GetChildMutable(source);
		auto &result_cc = ArrayVector::GetChildMutable(result);

		CastParameters child_parameters(parameters, cast_data.child_cast_info.GetCastData(), parameters.local_state);

		if (ldata.offset == 0) {
			// Fast path: offset is zero, we can just cast `array_size` elements of the child vectors directly
			// Since the list was constant, there can only be one sequence of data in the child vector
			return cast_data.child_cast_info.Cast(source_cc, result_cc, array_size, child_parameters);
		}

		// Else, we need to copy the range we want to cast to a new vector and cast that
		// In theory we could slice the source child to create a dictionary, but we would then have to flatten the
		// result child which is going to allocate a temp vector and perform a copy anyway. Since we just want to copy a
		// single contiguous range with a single offset, this is simpler.

		Vector payload_vector(source_cc.GetType(), array_size);
		VectorOperations::Copy(source_cc, payload_vector, ldata.offset + array_size, ldata.offset, 0);
		return cast_data.child_cast_info.Cast(payload_vector, result_cc, array_size, child_parameters);

	} else {
		source.Flatten();
		result.SetVectorType(VectorType::FLAT_VECTOR);

		auto child_type = ArrayType::GetChildType(result.GetType());
		auto &source_cc = ListVector::GetChildMutable(source);
		auto &result_cc = ArrayVector::GetChildMutable(result);
		auto ldata = FlatVector::GetData<list_entry_t>(source);

		auto child_count = array_size * count;
		SelectionVector child_sel(child_count);

		bool all_ok = true;

		for (idx_t i = 0; i < count; i++) {
			if (FlatVector::IsNull(source, i)) {
				FlatVector::SetNull(result, i, true);
				for (idx_t array_elem = 0; array_elem < array_size; array_elem++) {
					FlatVector::SetNull(result_cc, i * array_size + array_elem, true);
					child_sel.set_index(i * array_size + array_elem, 0);
				}
			} else if (ldata[i].length != array_size) {
				if (all_ok) {
					all_ok = false;
					auto msg = StringUtil::Format("Cannot cast list with length %llu to array with length %u",
					                              ldata[i].length, array_size);
					HandleCastError::AssignError(msg, parameters);
				}
				FlatVector::SetNull(result, i, true);
				for (idx_t array_elem = 0; array_elem < array_size; array_elem++) {
					FlatVector::SetNull(result_cc, i * array_size + array_elem, true);
					child_sel.set_index(i * array_size + array_elem, 0);
				}
			} else {
				for (idx_t array_elem = 0; array_elem < array_size; array_elem++) {
					child_sel.set_index(i * array_size + array_elem, ldata[i].offset + array_elem);
				}
			}
		}

		CastParameters child_parameters(parameters, cast_data.child_cast_info.GetCastData(), parameters.local_state);

		// Fast path: No lists are null
		// We can just cast the child vector directly
		// Note: Its worth doing a CheckAllValid here, the slow path is significantly more expensive
		if (FlatVector::ValidityMutable(result).CheckAllValid(count)) {
			// Cast the entries the lists actually point at: the source child's size is unrelated to
			// array_size * count, so casting the first child_count entries can both overrun the child and
			// touch entries that no list references.
			Vector source_slice(source_cc, child_sel, child_count);
			Vector payload_vector(result_cc.GetType(), child_count);

			bool ok = cast_data.child_cast_info.Cast(source_slice, payload_vector, child_count, child_parameters);
			if (all_ok && !ok) {
				all_ok = false;
				HandleCastError::AssignError(*child_parameters.error_message, parameters);
			}
			// The slice already put the entries in list order, so this is a straight copy
			VectorOperations::Copy(payload_vector, result_cc, child_count, 0, 0);
			return all_ok;
		}

		// Slow path: Some lists are null, so we need to copy the data list by list to the right place
		auto list_data = FlatVector::GetData<list_entry_t>(source);
		DataChunk cast_chunk;
		cast_chunk.Initialize(Allocator::DefaultAllocator(), {source_cc.GetType(), result_cc.GetType()}, array_size);

		for (idx_t i = 0; i < count; i++) {
			if (FlatVector::IsNull(result, i)) {
				// We've already failed to cast this list above (e.g. length mismatch), so theres nothing to do here.
				continue;
			} else {
				auto &list_cast_input = cast_chunk.data[0];
				auto &list_cast_output = cast_chunk.data[1];
				auto list_entry = list_data[i];

				VectorOperations::Copy(source_cc, list_cast_input, list_entry.offset + array_size, list_entry.offset,
				                       0);

				bool ok =
				    cast_data.child_cast_info.Cast(list_cast_input, list_cast_output, array_size, child_parameters);
				if (all_ok && !ok) {
					all_ok = false;
					HandleCastError::AssignError(*child_parameters.error_message, parameters);
				}
				VectorOperations::Copy(list_cast_output, result_cc, array_size, 0, i * array_size);

				// Reset the cast_chunk
				cast_chunk.Reset();
			}
		}

		return all_ok;
	}
}

BoundCastInfo DefaultCasts::ListCastSwitch(BindCastInput &input, const LogicalType &source, const LogicalType &target) {
	switch (target.id()) {
	case LogicalTypeId::LIST:
		return BoundCastInfo(ListCast::ListToListCast, ListBoundCastData::BindListToListCast(input, source, target),
		                     ListBoundCastData::InitListLocalState);
	case LogicalTypeId::VARCHAR: {
		auto cast_data = ListBoundCastData::BindListToListCast(input, source, LogicalType::LIST(LogicalType::VARCHAR));
		NestedToVarcharCast::WrapChildCast(cast_data->Cast<ListBoundCastData>().child_cast_info,
		                                   ListType::GetChildType(source));
		return BoundCastInfo(ListToVarcharCast, std::move(cast_data), ListBoundCastData::InitListLocalState);
	}
	case LogicalTypeId::ARRAY:
		return BoundCastInfo(ListToArrayCast, BindListToArrayCast(input, source, target),
		                     ListBoundCastData::InitListLocalState);
	default:
		return DefaultCasts::TryVectorNullCast;
	}
}

/*

 */

} // namespace duckdb
