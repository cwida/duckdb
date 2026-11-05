#include "duckdb/function/cast/nested_to_varchar_cast.hpp"
#include "duckdb/common/vector/array_vector.hpp"
#include "duckdb/common/vector/constant_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/function/cast/cast_function_set.hpp"
#include "duckdb/function/cast/default_casts.hpp"
#include "duckdb/function/cast/bound_cast_data.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/function/cast/vector_cast_helpers.hpp"

namespace duckdb {

unique_ptr<BoundCastData> ArrayBoundCastData::BindArrayToArrayCast(BindCastInput &input, const LogicalType &source,
                                                                   const LogicalType &target) {
	vector<BoundCastInfo> child_cast_info;
	auto &source_child_type = ArrayType::GetChildType(source);
	auto &result_child_type = ArrayType::GetChildType(target);
	auto child_cast = input.GetCastFunction(source_child_type, result_child_type);
	return make_uniq<ArrayBoundCastData>(std::move(child_cast));
}

static unique_ptr<BoundCastData> BindArrayToListCast(BindCastInput &input, const LogicalType &source,
                                                     const LogicalType &target) {
	D_ASSERT(source.id() == LogicalTypeId::ARRAY);
	D_ASSERT(target.id() == LogicalTypeId::LIST);

	vector<BoundCastInfo> child_cast_info;
	auto &source_child_type = ArrayType::GetChildType(source);
	auto &result_child_type = ListType::GetChildType(target);
	auto child_cast = input.GetCastFunction(source_child_type, result_child_type);
	return make_uniq<ArrayBoundCastData>(std::move(child_cast));
}

unique_ptr<FunctionLocalState> ArrayBoundCastData::InitArrayLocalState(CastLocalStateParameters &parameters) {
	auto &cast_data = parameters.cast_data->Cast<ArrayBoundCastData>();
	if (!cast_data.child_cast_info.HasInitLocalState()) {
		return nullptr;
	}
	CastLocalStateParameters child_parameters(parameters, cast_data.child_cast_info.GetCastData());
	return cast_data.child_cast_info.InitLocalState(child_parameters);
}

//------------------------------------------------------------------------------
// ARRAY -> ARRAY
//------------------------------------------------------------------------------
static bool ArrayToArrayCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	auto source_array_size = ArrayType::GetSize(source.GetType());
	auto target_array_size = ArrayType::GetSize(result.GetType());
	if (source_array_size != target_array_size) {
		// Cant cast between arrays of different sizes
		auto msg = StringUtil::Format("Cannot cast array of size %u to array of size %u", source_array_size,
		                              target_array_size);
		HandleCastError::AssignError(msg, parameters);
		// every row fails, so just return null
		ConstantVector::SetNull(result, count_t(count));
		return false;
	}

	auto &cast_data = parameters.cast_data->Cast<ArrayBoundCastData>();
	if (source.GetVectorType() == VectorType::CONSTANT_VECTOR) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);

		if (ConstantVector::IsNull(source)) {
			ConstantVector::SetNull(result, true);
			return true;
		}

		auto &source_cc = ArrayVector::GetChildMutable(source);
		auto &result_cc = ArrayVector::GetChildMutable(result);

		// If the array vector is constant, the child vector must be flat (or constant if array size is 1)
		D_ASSERT(source_cc.GetVectorType() == VectorType::FLAT_VECTOR || source_array_size == 1);

		CastParameters child_parameters(parameters, cast_data.child_cast_info.GetCastData(), parameters.local_state);
		bool all_ok = cast_data.child_cast_info.Cast(source_cc, result_cc, source_array_size, child_parameters);
		FlatVector::SetSize(result, count_t(count));
		return all_ok;
	}
	source.Flatten();
	FlatVector::CopyValidity(result, source, count);
	auto &source_cc = ArrayVector::GetChildMutable(source);
	auto &result_cc = ArrayVector::GetChildMutable(result);

	CastParameters child_parameters(parameters, cast_data.child_cast_info.GetCastData(), parameters.local_state);
	bool all_ok = cast_data.child_cast_info.Cast(source_cc, result_cc, count * source_array_size, child_parameters);
	return all_ok;
}

//------------------------------------------------------------------------------
// ARRAY -> VARCHAR
//------------------------------------------------------------------------------
static bool ArrayToVarcharCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	auto is_constant = source.GetVectorType() == VectorType::CONSTANT_VECTOR;

	auto size = ArrayType::GetSize(source.GetType());
	Vector varchar_list(LogicalType::ARRAY(LogicalType::VARCHAR, size), count);
	FlatVector::SetSize(varchar_list, count);
	ArrayToArrayCast(source, varchar_list, count, parameters);

	varchar_list.Flatten();
	auto &validity = FlatVector::Validity(varchar_list);
	auto &child = ArrayVector::GetChild(varchar_list);
	auto &child_validity = FlatVector::Validity(child);

	auto in_data = FlatVector::GetData<string_t>(child);

	// string elements are quoted and escaped where required, as for lists
	auto child_is_nested = ArrayType::GetChildType(source.GetType()).IsNested();
	auto string_length_func = child_is_nested ? VectorCastHelpers::CalculateStringLength
	                                          : VectorCastHelpers::CalculateEscapedStringLength<false>;
	auto write_string_func =
	    child_is_nested ? VectorCastHelpers::WriteString : VectorCastHelpers::WriteEscapedString<false>;
	auto needs_quotes = make_unsafe_uniq_array_uninitialized<bool>(MaxValue<idx_t>(size, 1));

	static constexpr idx_t SEP_LENGTH = 2;
	static constexpr idx_t NULL_LENGTH = 4;

	auto result_data = FlatVector::Writer<string_t>(result, count);
	for (idx_t i = 0; i < count; i++) {
		if (!validity.RowIsValid(i)) {
			result_data.WriteNull();
			continue;
		}

		// First pass, compute the length
		idx_t array_varchar_length = 2;
		for (idx_t j = 0; j < size; j++) {
			auto elem_idx = (i * size) + j;
			auto elem = in_data[elem_idx];
			if (j > 0) {
				array_varchar_length += SEP_LENGTH;
			}
			array_varchar_length +=
			    child_validity.RowIsValid(elem_idx) ? string_length_func(elem, needs_quotes[j]) : NULL_LENGTH;
		}

		auto &out_str = result_data.WriteEmptyString(array_varchar_length);
		auto dataptr = out_str.GetDataWriteable();
		idx_t offset = 0;
		dataptr[offset++] = '[';

		// Second pass, write the actual data
		for (idx_t j = 0; j < size; j++) {
			auto elem_idx = (i * size) + j;
			auto elem = in_data[elem_idx];
			if (j > 0) {
				memcpy(dataptr + offset, ", ", SEP_LENGTH);
				offset += SEP_LENGTH;
			}
			if (child_validity.RowIsValid(elem_idx)) {
				offset += write_string_func(dataptr + offset, elem, needs_quotes[j]);
			} else {
				memcpy(dataptr + offset, "NULL", NULL_LENGTH);
				offset += NULL_LENGTH;
			}
		}
		dataptr[offset] = ']';
		out_str.Finalize();
	}

	if (is_constant) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}

	return true;
}

//------------------------------------------------------------------------------
// ARRAY -> LIST
//------------------------------------------------------------------------------
static bool ArrayToListCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	auto &cast_data = parameters.cast_data->Cast<ArrayBoundCastData>();

	// FIXME: dont flatten
	source.Flatten();

	auto array_size = ArrayType::GetSize(source.GetType());
	auto child_count = count * array_size;

	ListVector::Reserve(result, child_count);

	auto &source_child = ArrayVector::GetChildMutable(source);
	auto &result_child = ListVector::GetChildMutable(result);

	CastParameters child_parameters(parameters, cast_data.child_cast_info.GetCastData(), parameters.local_state);
	bool all_ok = cast_data.child_cast_info.Cast(source_child, result_child, child_count, child_parameters);
	// set the list size after the child cast, since the cast may have replaced the child buffer
	ListVector::SetListSize(result, child_count);

	auto list_data = FlatVector::Writer<list_entry_t>(result, count);
	for (idx_t i = 0; i < count; i++) {
		if (FlatVector::IsNull(source, i)) {
			list_data.WriteNull();
			continue;
		}

		list_data.WriteValue(list_entry_t(i * array_size, array_size));
	}
	return all_ok;
}

BoundCastInfo DefaultCasts::ArrayCastSwitch(BindCastInput &input, const LogicalType &source,
                                            const LogicalType &target) {
	switch (target.id()) {
	case LogicalTypeId::VARCHAR: {
		auto size = ArrayType::GetSize(source);
		auto cast_data =
		    ArrayBoundCastData::BindArrayToArrayCast(input, source, LogicalType::ARRAY(LogicalType::VARCHAR, size));
		NestedToVarcharCast::WrapChildCast(cast_data->Cast<ArrayBoundCastData>().child_cast_info,
		                                   ArrayType::GetChildType(source));
		return BoundCastInfo(ArrayToVarcharCast, std::move(cast_data), ArrayBoundCastData::InitArrayLocalState);
	}
	case LogicalTypeId::ARRAY:
		return BoundCastInfo(ArrayToArrayCast, ArrayBoundCastData::BindArrayToArrayCast(input, source, target),
		                     ArrayBoundCastData::InitArrayLocalState);
	case LogicalTypeId::LIST:
		return BoundCastInfo(ArrayToListCast, BindArrayToListCast(input, source, target),
		                     ArrayBoundCastData::InitArrayLocalState);
	default:
		return DefaultCasts::TryVectorNullCast;
	};
}

} // namespace duckdb
