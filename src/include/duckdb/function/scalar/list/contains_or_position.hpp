#pragma once
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/function/create_sort_key.hpp"
#include "duckdb/common/operator/comparison_operators.hpp"

namespace duckdb {

template <class T, class RETURN_TYPE, bool FIND_NULLS>
idx_t ListSearchSimpleOp(const Vector &input_list, const Vector &list_child, const Vector &target, Vector &result,
                         const idx_t count) {
	// If the return type is not a bool, return the position
	const auto return_pos = std::is_same<RETURN_TYPE, int32_t>::value;

	const auto list_entries = input_list.Values<list_entry_t>();
	const auto child_data = list_child.Values<T>();
	const auto target_data = target.Values<T>();

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto result_data = FlatVector::Writer<RETURN_TYPE>(result, count);

	idx_t total_matches = 0;

	for (idx_t row_idx = 0; row_idx < count; ++row_idx) {
		auto list_entry = list_entries[row_idx];

		// The entire list is NULL, the result is also NULL.
		if (!list_entry.IsValid()) {
			result_data.WriteNull();
			continue;
		}
		auto list = list_entry.GetValue();
		auto target_entry = target_data[row_idx];

		const bool target_valid = target_entry.IsValid();

		// An empty list never contains anything, not even NULL.
		if (list.length == 0 && !return_pos) {
			result_data.WriteValue(RETURN_TYPE(false));
			continue;
		}

		// We are finished, if we are not looking for NULL, and the target is NULL.
		const auto finished = !FIND_NULLS && !target_valid;
		// We did not find the target (finished, or list is empty).
		if (finished || list.length == 0) {
			// Return NULL as the position.
			result_data.WriteNull();
			continue;
		}

		const auto entry_length = list.length;
		const auto entry_offset = list.offset;

		bool found = false;
		RETURN_TYPE found_value {};

		for (auto list_idx = entry_offset; list_idx < entry_length + entry_offset && !found; list_idx++) {
			auto child_entry = child_data[list_idx];
			const bool child_valid = child_entry.IsValid();

			if ((FIND_NULLS && !child_valid && !target_valid) ||
			    (child_valid && target_valid &&
			     Equals::Operation<T>(child_entry.GetValue(), target_entry.GetValue()))) {
				found = true;
				total_matches++;
				if (return_pos) {
					found_value = UnsafeNumericCast<int32_t>(1 + list_idx - entry_offset);
				} else {
					found_value = RETURN_TYPE(true);
				}
			}
		}

		if (!found) {
			if (return_pos) {
				result_data.WriteNull();
			} else {
				result_data.WriteValue(RETURN_TYPE(false));
			}
		} else {
			result_data.WriteValue(found_value);
		}
	}

	return total_matches;
}

template <class RETURN_TYPE, bool FIND_NULLS>
idx_t ListSearchNestedOp(const Vector &list_vec, const Vector &source_vec, const Vector &target_vec, Vector &result_vec,
                         const idx_t target_count) {
	// Set up sort keys for nested types.
	auto source_count = ListVector::GetListSize(list_vec);
	Vector source_sort_key_vec(LogicalType::BLOB, source_count);
	Vector target_sort_key_vec(LogicalType::BLOB, target_count);

	const OrderModifiers order_modifiers(OrderType::ASCENDING, OrderByNullType::NULLS_LAST);
	// Pass explicit counts: source_vec and target_vec may have different sizes than source_count/target_count
	// when called via dictionary expression optimization (TryExecuteDictionaryExpression uses the dictionary
	// size as the chunk count, while the non-key arguments retain their original size).
	CreateSortKeyHelpers::CreateSortKeyWithValidity(source_vec, source_sort_key_vec, order_modifiers, source_count);
	CreateSortKeyHelpers::CreateSortKeyWithValidity(target_vec, target_sort_key_vec, order_modifiers, target_count);

	return ListSearchSimpleOp<string_t, RETURN_TYPE, FIND_NULLS>(list_vec, source_sort_key_vec, target_sort_key_vec,
	                                                             result_vec, target_count);
}

//! "Search" each list in the list vector for the corresponding value in the target vector, returning either
//! true/false or the position of the value in the list. The result vector is populated with the result of the search.
//! usually the "source" vector is the list child vector, but it is passed separately to enable searching nested
//! children, for example when searching the keys of a MAP vectors.
template <class RETURN_TYPE, bool FIND_NULLS = false>
idx_t ListSearchOp(const Vector &list_v, const Vector &source_v, const Vector &target_v, Vector &result_v,
                   idx_t target_count) {
	const auto type = target_v.GetType().InternalType();
	switch (type) {
	case PhysicalType::BOOL:
	case PhysicalType::INT8:
		return ListSearchSimpleOp<int8_t, RETURN_TYPE, FIND_NULLS>(list_v, source_v, target_v, result_v, target_count);
	case PhysicalType::INT16:
		return ListSearchSimpleOp<int16_t, RETURN_TYPE, FIND_NULLS>(list_v, source_v, target_v, result_v, target_count);
	case PhysicalType::INT32:
		return ListSearchSimpleOp<int32_t, RETURN_TYPE, FIND_NULLS>(list_v, source_v, target_v, result_v, target_count);
	case PhysicalType::INT64:
		return ListSearchSimpleOp<int64_t, RETURN_TYPE, FIND_NULLS>(list_v, source_v, target_v, result_v, target_count);
	case PhysicalType::INT128:
		return ListSearchSimpleOp<hugeint_t, RETURN_TYPE, FIND_NULLS>(list_v, source_v, target_v, result_v,
		                                                              target_count);
	case PhysicalType::UINT8:
		return ListSearchSimpleOp<uint8_t, RETURN_TYPE, FIND_NULLS>(list_v, source_v, target_v, result_v, target_count);
	case PhysicalType::UINT16:
		return ListSearchSimpleOp<uint16_t, RETURN_TYPE, FIND_NULLS>(list_v, source_v, target_v, result_v,
		                                                             target_count);
	case PhysicalType::UINT32:
		return ListSearchSimpleOp<uint32_t, RETURN_TYPE, FIND_NULLS>(list_v, source_v, target_v, result_v,
		                                                             target_count);
	case PhysicalType::UINT64:
		return ListSearchSimpleOp<uint64_t, RETURN_TYPE, FIND_NULLS>(list_v, source_v, target_v, result_v,
		                                                             target_count);
	case PhysicalType::UINT128:
		return ListSearchSimpleOp<uhugeint_t, RETURN_TYPE, FIND_NULLS>(list_v, source_v, target_v, result_v,
		                                                               target_count);
	case PhysicalType::FLOAT:
		return ListSearchSimpleOp<float, RETURN_TYPE, FIND_NULLS>(list_v, source_v, target_v, result_v, target_count);
	case PhysicalType::DOUBLE:
		return ListSearchSimpleOp<double, RETURN_TYPE, FIND_NULLS>(list_v, source_v, target_v, result_v, target_count);
	case PhysicalType::VARCHAR:
		return ListSearchSimpleOp<string_t, RETURN_TYPE, FIND_NULLS>(list_v, source_v, target_v, result_v,
		                                                             target_count);
	case PhysicalType::INTERVAL:
		return ListSearchSimpleOp<interval_t, RETURN_TYPE, FIND_NULLS>(list_v, source_v, target_v, result_v,
		                                                               target_count);
	case PhysicalType::STRUCT:
	case PhysicalType::LIST:
	case PhysicalType::ARRAY:
		return ListSearchNestedOp<RETURN_TYPE, FIND_NULLS>(list_v, source_v, target_v, result_v, target_count);
	default:
		throw NotImplementedException("This function has not been implemented for logical type %s",
		                              TypeIdToString(type));
	}
}

} // namespace duckdb
