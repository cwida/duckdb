//===--------------------------------------------------------------------===//
// comparison_operators.cpp
// Description: This file contains the implementation of the comparison
// operations == != >= <= > <
//===--------------------------------------------------------------------===//

#include "duckdb/common/operator/comparison_operators.hpp"

#include "duckdb/common/uhugeint.hpp"
#include "duckdb/common/types/variant.hpp"
#include "duckdb/common/vector/array_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/numeric_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/common/vector/vector_iterator.hpp"
#include "duckdb/common/vector_operations/binary_executor.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"

#include "duckdb/common/likely.hpp"
#include "duckdb/common/smaller_binary.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Fast path: direct BinaryExecutor for primitive types (single pass, no intermediate vector)
//===--------------------------------------------------------------------===//
template <class OP>
static bool TryPrimitiveComparisonExecute(const Vector &left, const Vector &right, Vector &result) {
#if DUCKDB_SMALLER_BINARY(primitive_comparison_execute)
	return false;
#else
	D_ASSERT(left.GetType().InternalType() == right.GetType().InternalType());
	if (left.GetType().id() == LogicalTypeId::NUMERIC) {
		// NUMERIC values compare natively when both sides have a typed representation
		unique_ptr<Vector> left_typed, right_typed;
		if (NumericVector::TryNativePair(left, right, left_typed, right_typed)) {
			return TryPrimitiveComparisonExecute<OP>(*left_typed, *right_typed, result);
		}
	}
	switch (left.GetType().InternalType()) {
	case PhysicalType::BOOL:
	case PhysicalType::INT8:
		BinaryExecutor::Execute<int8_t, int8_t, bool, OP>(left, right, result);
		return true;
	case PhysicalType::INT16:
		BinaryExecutor::Execute<int16_t, int16_t, bool, OP>(left, right, result);
		return true;
	case PhysicalType::INT32:
		BinaryExecutor::Execute<int32_t, int32_t, bool, OP>(left, right, result);
		return true;
	case PhysicalType::INT64:
		BinaryExecutor::Execute<int64_t, int64_t, bool, OP>(left, right, result);
		return true;
	case PhysicalType::UINT8:
		BinaryExecutor::Execute<uint8_t, uint8_t, bool, OP>(left, right, result);
		return true;
	case PhysicalType::UINT16:
		BinaryExecutor::Execute<uint16_t, uint16_t, bool, OP>(left, right, result);
		return true;
	case PhysicalType::UINT32:
		BinaryExecutor::Execute<uint32_t, uint32_t, bool, OP>(left, right, result);
		return true;
	case PhysicalType::UINT64:
		BinaryExecutor::Execute<uint64_t, uint64_t, bool, OP>(left, right, result);
		return true;
	case PhysicalType::INT128:
		BinaryExecutor::Execute<hugeint_t, hugeint_t, bool, OP>(left, right, result);
		return true;
	case PhysicalType::UINT128:
		BinaryExecutor::Execute<uhugeint_t, uhugeint_t, bool, OP>(left, right, result);
		return true;
	case PhysicalType::FLOAT:
		BinaryExecutor::Execute<float, float, bool, OP>(left, right, result);
		return true;
	case PhysicalType::DOUBLE:
		BinaryExecutor::Execute<double, double, bool, OP>(left, right, result);
		return true;
	case PhysicalType::INTERVAL:
		BinaryExecutor::Execute<interval_t, interval_t, bool, OP>(left, right, result);
		return true;
	case PhysicalType::VARCHAR:
		BinaryExecutor::Execute<string_t, string_t, bool, OP>(left, right, result);
		return true;
	default:
		return false;
	}
#endif
}

template <class PREDICATE>
static void ComparatorToBoolean(const Vector &left, const Vector &right, Vector &result, const ExpressionType comp,
                                PREDICATE predicate) {
	D_ASSERT(result.GetType() == LogicalType::BOOLEAN);
	Vector comparator_result(LogicalType::TINYINT);
	VectorOperations::Comparator(left, right, comparator_result, comp);
	const auto count = comparator_result.size();
	auto cmp_data = comparator_result.Values<int8_t>();
	if (left.GetVectorType() == VectorType::CONSTANT_VECTOR && right.GetVectorType() == VectorType::CONSTANT_VECTOR) {
		// the comparison of two constants is a constant
		auto entry = cmp_data[0];
		auto value = entry.IsValid() ? Value::BOOLEAN(predicate(entry.GetValue())) : Value(LogicalType::BOOLEAN);
		result.Reference(value, count_t(count));
		return;
	}
	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto result_data = FlatVector::Writer<bool>(result, count);
	for (idx_t i = 0; i < count; i++) {
		auto entry = cmp_data[i];
		if (!entry.IsValid()) {
			result_data.WriteNull();
		} else {
			result_data.WriteValue(predicate(entry.GetValue()));
		}
	}
}

static idx_t GetComparisonCount(const Vector &left, const Vector &right, const char *fname) {
	const bool left_is_const = left.GetVectorType() == VectorType::CONSTANT_VECTOR;
	const bool right_is_const = right.GetVectorType() == VectorType::CONSTANT_VECTOR;
	if (!left_is_const && !right_is_const && left.size() != right.size()) {
		throw InternalException("Mismatch in input vector sizes for %s - left has %d rows but right has %d", fname,
		                        left.size(), right.size());
	}
	return left_is_const ? right.size() : left.size();
}

void VectorOperations::Equals(const Vector &left, const Vector &right, Vector &result) {
	if (!TryPrimitiveComparisonExecute<duckdb::Equals>(left, right, result)) {
		ComparatorToBoolean(left, right, result, ExpressionType::COMPARE_EQUAL, [](int8_t v) { return v == 0; });
	}
}

void VectorOperations::NotEquals(const Vector &left, const Vector &right, Vector &result) {
	if (!TryPrimitiveComparisonExecute<duckdb::NotEquals>(left, right, result)) {
		ComparatorToBoolean(left, right, result, ExpressionType::COMPARE_NOTEQUAL, [](int8_t v) { return v != 0; });
	}
}

void VectorOperations::GreaterThan(const Vector &left, const Vector &right, Vector &result) {
	if (!TryPrimitiveComparisonExecute<duckdb::GreaterThan>(left, right, result)) {
		ComparatorToBoolean(left, right, result, ExpressionType::COMPARE_GREATERTHAN, [](int8_t v) { return v > 0; });
	}
}

void VectorOperations::GreaterThanEquals(const Vector &left, const Vector &right, Vector &result) {
	if (!TryPrimitiveComparisonExecute<duckdb::GreaterThanEquals>(left, right, result)) {
		ComparatorToBoolean(left, right, result, ExpressionType::COMPARE_GREATERTHANOREQUALTO,
		                    [](int8_t v) { return v >= 0; });
	}
}

void VectorOperations::LessThan(const Vector &left, const Vector &right, Vector &result) {
	// NOLINTNEXTLINE: flip right / left (left < right is equal to right > left)
	if (!TryPrimitiveComparisonExecute<duckdb::GreaterThan>(right, left, result)) {
		ComparatorToBoolean(left, right, result, ExpressionType::COMPARE_LESSTHAN, [](int8_t v) { return v < 0; });
	}
}

void VectorOperations::LessThanEquals(const Vector &left, const Vector &right, Vector &result) {
	// NOLINTNEXTLINE: flip right / left (left <= right is equal to right >= left)
	if (!TryPrimitiveComparisonExecute<duckdb::GreaterThanEquals>(right, left, result)) {
		ComparatorToBoolean(left, right, result, ExpressionType::COMPARE_LESSTHANOREQUALTO,
		                    [](int8_t v) { return v <= 0; });
	}
}

struct StandardComparatorExecute {
	template <class T>
	static inline void Execute(const Vector &left, const Vector &right, int8_t *result_data,
	                           const SelectionVector &lhs_sel, const SelectionVector &rhs_sel, idx_t sel_count,
	                           ValidityMask &result_validity) {
		UnifiedVectorFormat left_format, right_format;
		left.ToUnifiedFormat(left_format);
		right.ToUnifiedFormat(right_format);
		auto ldata = UnifiedVectorFormat::GetData<T>(left_format);
		auto rdata = UnifiedVectorFormat::GetData<T>(right_format);
		for (idx_t i = 0; i < sel_count; i++) {
			auto lidx = left_format.sel->get_index(lhs_sel.get_index(i));
			auto ridx = right_format.sel->get_index(rhs_sel.get_index(i));
			bool left_null = !left_format.validity.RowIsValid(lidx);
			bool right_null = !right_format.validity.RowIsValid(ridx);
			if (left_null || right_null) {
				result_validity.SetInvalid(i);
			} else {
				result_data[i] = duckdb::Comparator::Operation<T>(ldata[lidx], rdata[ridx]);
			}
		}
	}
};

struct DistinctComparatorExecute {
	template <class T>
	static void Execute(const Vector &left, const Vector &right, int8_t *result_data, const SelectionVector &lhs_sel,
	                    const SelectionVector &rhs_sel, idx_t sel_count) {
		UnifiedVectorFormat left_format, right_format;
		left.ToUnifiedFormat(left_format);
		right.ToUnifiedFormat(right_format);
		auto ldata = UnifiedVectorFormat::GetData<T>(left_format);
		auto rdata = UnifiedVectorFormat::GetData<T>(right_format);
		for (idx_t i = 0; i < sel_count; i++) {
			auto lidx = left_format.sel->get_index(lhs_sel.get_index(i));
			auto ridx = right_format.sel->get_index(rhs_sel.get_index(i));
			bool left_null = !left_format.validity.RowIsValid(lidx);
			bool right_null = !right_format.validity.RowIsValid(ridx);
			result_data[i] = duckdb::DistinctComparator::Operation<T>(ldata[lidx], rdata[ridx], left_null, right_null);
		}
	}
};

// forward declaration - nested comparators call DistinctComparator recursively for children
static void DistinctComparatorTypeSwitch(const Vector &left, const Vector &right, int8_t *result_data,
                                         const SelectionVector &lhs_sel, const SelectionVector &rhs_sel,
                                         idx_t sel_count);

static int8_t DistinctNullComparator(bool left_null, bool right_null) {
	if (left_null && right_null) {
		return Comparator::VALUES_ARE_EQUAL;
	}
	// default is NULLS LAST - i.e. NULL is greater than every non-NULL value
	if (left_null) {
		return Comparator::LEFT_IS_GREATER;
	}
	return Comparator::RIGHT_IS_GREATER;
}

static void ComparatorTypeSwitch(const Vector &left, const Vector &right, int8_t *result_data,
                                 const SelectionVector &lhs_sel, const SelectionVector &rhs_sel, idx_t sel_count,
                                 const ExpressionType comp, ValidityMask &validity);

static idx_t NestedValidity(const VectorValidityIterator &left_validity, const VectorValidityIterator &right_validity,
                            int8_t *result_data, const SelectionVector &lhs_sel, const SelectionVector &rhs_sel,
                            idx_t sel_count, SelectionVector &remaining_lhs_sel, SelectionVector &remaining_rhs_sel,
                            SelectionVector &remaining_result_sel, optional_ptr<ValidityMask> result_validity) {
	idx_t remaining_count = 0;

	bool has_nulls = left_validity.CanHaveNull() || right_validity.CanHaveNull();
	if (!has_nulls) {
		remaining_count = sel_count;
		memset(result_data, 0, sel_count * sizeof(int8_t));
		for (idx_t i = 0; i < sel_count; i++) {
			remaining_lhs_sel.set_index(i, lhs_sel.get_index(i));
			remaining_rhs_sel.set_index(i, rhs_sel.get_index(i));
			remaining_result_sel.set_index(i, i);
		}
	} else {
		remaining_count = 0;
		for (idx_t i = 0; i < sel_count; i++) {
			bool left_null = !left_validity.IsValid(lhs_sel.get_index(i));
			bool right_null = !right_validity.IsValid(rhs_sel.get_index(i));
			if (left_null || right_null) {
				if (!result_validity) {
					// DISTINCT
					result_data[i] = DistinctNullComparator(left_null, right_null);
				} else {
					// regular comparison - set NULL if any value is NULL
					result_validity->SetInvalid(i);
				}
			} else {
				result_data[i] = Comparator::VALUES_ARE_EQUAL;
				remaining_lhs_sel.set_index(remaining_count, lhs_sel.get_index(i));
				remaining_rhs_sel.set_index(remaining_count, rhs_sel.get_index(i));
				remaining_result_sel.set_index(remaining_count, i);
				remaining_count++;
			}
		}
	}

	return remaining_count;
}

// PREDICATE(cmp, null) returns true if this row is done.
template <class PREDICATE>
static idx_t NestedScatter(const int8_t *child_result, int8_t *result_data, idx_t remaining_count,
                           SelectionVector &remaining_lhs_sel, SelectionVector &remaining_rhs_sel,
                           SelectionVector &remaining_result_sel, const ValidityMask &child_validity,
                           optional_ptr<ValidityMask> result_validity, PREDICATE predicate) {
	idx_t new_remaining_count = 0;
	for (idx_t i = 0; i < remaining_count; i++) {
		const auto remaining_idx = remaining_result_sel.get_index(i);
		const auto null = result_validity && !child_validity.RowIsValidUnsafe(i);
		if (predicate(child_result[i], null)) {
			if (null) {
				result_validity->SetInvalid(remaining_idx);
			} else {
				result_data[remaining_idx] = child_result[i];
			}
		} else {
			// still equal at this position - need to check the next entry
			remaining_lhs_sel.set_index(new_remaining_count, remaining_lhs_sel.get_index(i));
			remaining_rhs_sel.set_index(new_remaining_count, remaining_rhs_sel.get_index(i));
			remaining_result_sel.set_index(new_remaining_count, remaining_idx);
			new_remaining_count++;
		}
	}

	return new_remaining_count;
}

static void StructComparator(const Vector &left, const Vector &right, int8_t *result_data,
                             const SelectionVector &lhs_sel, const SelectionVector &rhs_sel, idx_t sel_count,
                             const ExpressionType comp, optional_ptr<ValidityMask> result_validity = nullptr) {
	//	STRUCT comparisons use ordering semantics, so the result is never NULL
	D_ASSERT(left.GetType().id() != LogicalTypeId::UNION);
	if (sel_count == 0) {
		return;
	}
	auto &lchildren = StructVector::GetEntries(left);
	auto &rchildren = StructVector::GetEntries(right);
	D_ASSERT(lchildren.size() == rchildren.size());

	// step 1: handle struct-level validity and initialize results
	auto left_validity = left.Validity();
	auto right_validity = right.Validity();

	// remaining tracks which rows still need child comparison
	// along with their corresponding lhs/rhs selection indices
	SelectionVector remaining_lhs_sel(sel_count);
	SelectionVector remaining_rhs_sel(sel_count);
	SelectionVector remaining_result_sel(sel_count);
	idx_t remaining_count = NestedValidity(left_validity, right_validity, result_data, lhs_sel, rhs_sel, sel_count,
	                                       remaining_lhs_sel, remaining_rhs_sel, remaining_result_sel, result_validity);

	// step 2: compare child vectors one by one
	// child results are written densely, then scattered back to the correct output positions
	auto child_result = make_unsafe_uniq_array<int8_t>(remaining_count);
	ValidityMask child_validity(remaining_count);
	for (idx_t child_idx = 0; child_idx < lchildren.size() && remaining_count > 0; child_idx++) {
		switch (comp) {
		case ExpressionType::COMPARE_DISTINCT_FROM:
		case ExpressionType::COMPARE_NOT_DISTINCT_FROM:
		case ExpressionType::COMPARE_LESSTHAN:
		case ExpressionType::COMPARE_GREATERTHAN:
		case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		case ExpressionType::COMPARE_EQUAL:
		case ExpressionType::COMPARE_NOTEQUAL:
			// children always compare with distinct semantics - a NULL child makes the structs distinct
			// rather than the result NULL, matching the Value comparison path. only a NULL struct itself
			// makes the result NULL, which the validity pass above handled
			DistinctComparatorTypeSwitch(lchildren[child_idx], rchildren[child_idx], child_result.get(),
			                             remaining_lhs_sel, remaining_rhs_sel, remaining_count);
			remaining_count = NestedScatter(child_result.get(), result_data, remaining_count, remaining_lhs_sel,
			                                remaining_rhs_sel, remaining_result_sel, child_validity, nullptr,
			                                [](int8_t c, bool n) { return c != Comparator::VALUES_ARE_EQUAL || n; });
			break;
		default:
			throw InternalException("Invalid STRUCT Comparison");
		}
	}
}

static idx_t UnionScatter(const Vector &key, const idx_t child_idx, const int8_t *child_result, int8_t *result_data,
                          const idx_t remaining_count, SelectionVector &remaining_lhs_sel,
                          SelectionVector &remaining_rhs_sel, SelectionVector &remaining_result_sel,
                          const ValidityMask &child_validity, ValidityMask &result_validity) {
	//	For SQL-equality comparisons of UNION types,
	//	we don't know if the NULL means NULL or a different column
	//	So we have to further restrict the comparison by column index
	idx_t new_remaining_count = 0;
	UnifiedVectorFormat key_format;
	key.ToUnifiedFormat(key_format);
	const uint8_t *key_data = UnifiedVectorFormat::GetData<uint8_t>(key_format);
	for (idx_t i = 0; i < remaining_count; i++) {
		const auto remaining_idx = remaining_result_sel.get_index(i);
		const idx_t key_idx = key_format.sel->get_index(remaining_lhs_sel.get_index(i));
		//	Keep if not the current column
		if (key_data[key_idx] != child_idx - 1) {
			remaining_lhs_sel.set_index(new_remaining_count, remaining_lhs_sel.get_index(i));
			remaining_rhs_sel.set_index(new_remaining_count, remaining_rhs_sel.get_index(i));
			remaining_result_sel.set_index(new_remaining_count, remaining_idx);
			new_remaining_count++;
		} else if (!child_validity.RowIsValidUnsafe(i)) {
			result_validity.SetInvalid(remaining_idx);
		} else if (child_result[i] != Comparator::VALUES_ARE_EQUAL) {
			// not equal at this position - we found the final result for this row
			result_data[remaining_idx] = child_result[i];
		} else {
			// still equal at this position - need to check the next entry
			remaining_lhs_sel.set_index(new_remaining_count, remaining_lhs_sel.get_index(i));
			remaining_rhs_sel.set_index(new_remaining_count, remaining_rhs_sel.get_index(i));
			remaining_result_sel.set_index(new_remaining_count, remaining_idx);
			new_remaining_count++;
		}
	}

	return new_remaining_count;
}

static void UnionComparator(const Vector &left, const Vector &right, int8_t *result_data,
                            const SelectionVector &lhs_sel, const SelectionVector &rhs_sel, idx_t sel_count,
                            const ExpressionType comp, optional_ptr<ValidityMask> result_validity = nullptr) {
	//	UNION data columns are independent, so we can just treat them as disjoint single columns
	D_ASSERT(left.GetType().id() == LogicalTypeId::UNION);
	if (sel_count == 0) {
		return;
	}
	auto &lchildren = StructVector::GetEntries(left);
	auto &rchildren = StructVector::GetEntries(right);
	D_ASSERT(lchildren.size() == rchildren.size());

	// step 1: handle struct-level validity and initialize results
	auto left_validity = left.Validity();
	auto right_validity = right.Validity();

	// remaining tracks which rows still need child comparison
	// along with their corresponding lhs/rhs selection indices
	SelectionVector remaining_lhs_sel(sel_count);
	SelectionVector remaining_rhs_sel(sel_count);
	SelectionVector remaining_result_sel(sel_count);
	idx_t remaining_count = NestedValidity(left_validity, right_validity, result_data, lhs_sel, rhs_sel, sel_count,
	                                       remaining_lhs_sel, remaining_rhs_sel, remaining_result_sel, result_validity);

	// step 2: compare child vectors one by one
	// child results are written densely, then scattered back to the correct output positions
	auto child_result = make_unsafe_uniq_array<int8_t>(remaining_count);
	ValidityMask child_validity(remaining_count);
	for (idx_t child_idx = 0; child_idx < lchildren.size() && remaining_count > 0; child_idx++) {
		switch (comp) {
		case ExpressionType::COMPARE_DISTINCT_FROM:
		case ExpressionType::COMPARE_NOT_DISTINCT_FROM:
			D_ASSERT(!result_validity);
			DistinctComparatorTypeSwitch(lchildren[child_idx], rchildren[child_idx], child_result.get(),
			                             remaining_lhs_sel, remaining_rhs_sel, remaining_count);
			break;
		case ExpressionType::COMPARE_EQUAL:
		case ExpressionType::COMPARE_NOTEQUAL:
		case ExpressionType::COMPARE_LESSTHAN:
		case ExpressionType::COMPARE_GREATERTHAN:
		case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
			child_validity.SetAllValid(remaining_count);
			ComparatorTypeSwitch(lchildren[child_idx], rchildren[child_idx], child_result.get(), remaining_lhs_sel,
			                     remaining_rhs_sel, remaining_count, comp, child_validity);
			break;
		default:
			throw InternalException("Invalid UNION Comparison");
		}

		//	Scatter back
		if (child_idx && result_validity) {
			remaining_count = UnionScatter(lchildren[0], child_idx, child_result.get(), result_data, remaining_count,
			                               remaining_lhs_sel, remaining_rhs_sel, remaining_result_sel, child_validity,
			                               *result_validity);
		} else {
			remaining_count = NestedScatter(child_result.get(), result_data, remaining_count, remaining_lhs_sel,
			                                remaining_rhs_sel, remaining_result_sel, child_validity, result_validity,
			                                [](int8_t c, bool n) { return c != Comparator::VALUES_ARE_EQUAL || n; });
		}
	}
}

struct ListEntryAccessor {
	static const Vector &GetChild(const Vector &vector) {
		return ListVector::GetChild(vector);
	}
	static void FlattenChild(const Vector &) {
		// no-op: ToUnifiedFormat handles all vector types without needing to flatten first
	}
	static idx_t GetOffset(UnifiedVectorFormat &format, idx_t sel_idx) {
		auto entries = UnifiedVectorFormat::GetData<list_entry_t>(format);
		auto idx = format.sel->get_index(sel_idx);
		return entries[idx].offset;
	}
	static idx_t GetLength(UnifiedVectorFormat &format, idx_t sel_idx) {
		auto entries = UnifiedVectorFormat::GetData<list_entry_t>(format);
		auto idx = format.sel->get_index(sel_idx);
		return entries[idx].length;
	}
};

struct ArrayEntryAccessor {
	explicit ArrayEntryAccessor(idx_t array_size) : array_size(array_size) {
	}
	static const Vector &GetChild(const Vector &vector) {
		return ArrayVector::GetChild(vector);
	}
	static void FlattenChild(const Vector &) {
		// no-op: ToUnifiedFormat handles all vector types without needing to flatten first
	}
	idx_t GetOffset(UnifiedVectorFormat &format, idx_t sel_idx) {
		return format.sel->get_index(sel_idx) * array_size;
	}
	idx_t GetLength(UnifiedVectorFormat &, idx_t) {
		return array_size;
	}
	idx_t array_size;
};

template <class ACCESSOR>
static idx_t ListOrArrayExhausted(UnifiedVectorFormat &left_format, UnifiedVectorFormat &right_format,
                                  SelectionVector &left_child_sel, SelectionVector &right_child_sel,
                                  const idx_t index_in_list, int8_t *result_data, idx_t remaining_count,
                                  SelectionVector &remaining_lhs_sel, SelectionVector &remaining_rhs_sel,
                                  SelectionVector &remaining_result_sel, ACCESSOR accessor) {
	idx_t active_count = 0;
	for (idx_t i = 0; i < remaining_count; i++) {
		auto left_length = accessor.GetLength(left_format, remaining_lhs_sel.get_index(i));
		auto right_length = accessor.GetLength(right_format, remaining_rhs_sel.get_index(i));
		bool left_exhausted = index_in_list >= left_length;
		bool right_exhausted = index_in_list >= right_length;
		if (left_exhausted || right_exhausted) {
			// either (or both) lists are exhausted at this position
			if (!left_exhausted || !right_exhausted) {
				// one of the lists is shorter than the other - the shorter list is the smallest
				result_data[remaining_result_sel.get_index(i)] =
				    left_exhausted ? Comparator::RIGHT_IS_GREATER : Comparator::LEFT_IS_GREATER;
			}
			// else: same length, all elements matched - result stays 0
		} else {
			auto left_offset = accessor.GetOffset(left_format, remaining_lhs_sel.get_index(i));
			auto right_offset = accessor.GetOffset(right_format, remaining_rhs_sel.get_index(i));
			left_child_sel.set_index(active_count, left_offset + index_in_list);
			right_child_sel.set_index(active_count, right_offset + index_in_list);
			remaining_lhs_sel.set_index(active_count, remaining_lhs_sel.get_index(i));
			remaining_rhs_sel.set_index(active_count, remaining_rhs_sel.get_index(i));
			remaining_result_sel.set_index(active_count, remaining_result_sel.get_index(i));
			active_count++;
		}
	}

	return active_count;
}

template <class ACCESSOR>
static void ListOrArrayComparator(const Vector &left, const Vector &right, int8_t *result_data,
                                  const SelectionVector &lhs_sel, const SelectionVector &rhs_sel, idx_t sel_count,
                                  ACCESSOR accessor, const ExpressionType comp,
                                  optional_ptr<ValidityMask> result_validity = nullptr) {
	if (sel_count == 0) {
		return;
	}
	// FlattenChild is a no-op; ToUnifiedFormat handles all vector types in the recursive comparators
	accessor.FlattenChild(left);
	accessor.FlattenChild(right);

	// step 1: handle top-level validity
	auto left_validity = left.Validity();
	auto right_validity = right.Validity();

	SelectionVector remaining_lhs_sel(sel_count);
	SelectionVector remaining_rhs_sel(sel_count);
	SelectionVector remaining_result_sel(sel_count);
	idx_t remaining_count = NestedValidity(left_validity, right_validity, result_data, lhs_sel, rhs_sel, sel_count,
	                                       remaining_lhs_sel, remaining_rhs_sel, remaining_result_sel, result_validity);

	if (remaining_count == 0) {
		return;
	}

	// step 2: get entries and child vector
	UnifiedVectorFormat left_format, right_format;
	left.ToUnifiedFormat(left_format);
	right.ToUnifiedFormat(right_format);
	auto &left_child = accessor.GetChild(left);
	auto &right_child = accessor.GetChild(right);

	// step 3: iterate position-by-position through child elements of the list / array
	SelectionVector left_child_sel(remaining_count);
	SelectionVector right_child_sel(remaining_count);
	ValidityMask child_validity(remaining_count);
	auto child_result = make_unsafe_uniq_array<int8_t>(remaining_count);

	for (idx_t index_in_list = 0; remaining_count > 0; index_in_list++) {
		// resolve the rows whose list ran out of elements, and compact the selections to the rest
		remaining_count =
		    ListOrArrayExhausted(left_format, right_format, left_child_sel, right_child_sel, index_in_list, result_data,
		                         remaining_count, remaining_lhs_sel, remaining_rhs_sel, remaining_result_sel, accessor);
		if (remaining_count == 0) {
			break;
		}

		// compare child elements at this position
		switch (comp) {
		case ExpressionType::COMPARE_DISTINCT_FROM:
		case ExpressionType::COMPARE_NOT_DISTINCT_FROM:
		case ExpressionType::COMPARE_LESSTHAN:
		case ExpressionType::COMPARE_GREATERTHAN:
		case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		case ExpressionType::COMPARE_EQUAL:
		case ExpressionType::COMPARE_NOTEQUAL:
			// child elements always compare with distinct semantics - a NULL element makes the lists
			// distinct rather than the result NULL, which is what the Value comparison path does too.
			// only a NULL list itself makes the result NULL, which the validity pass above handled
			DistinctComparatorTypeSwitch(left_child, right_child, child_result.get(), left_child_sel, right_child_sel,
			                             remaining_count);
			// partition into resolved vs still-remaining
			remaining_count = NestedScatter(child_result.get(), result_data, remaining_count, remaining_lhs_sel,
			                                remaining_rhs_sel, remaining_result_sel, child_validity, nullptr,
			                                [](int8_t c, bool n) { return c != Comparator::VALUES_ARE_EQUAL || n; });
			break;
		default:
			throw InternalException("Invalid LIST Comparison");
		}
	}
}

static void ListComparator(const Vector &left, const Vector &right, int8_t *result_data, const SelectionVector &lhs_sel,
                           const SelectionVector &rhs_sel, idx_t sel_count, const ExpressionType comp,
                           optional_ptr<ValidityMask> result_validity = nullptr) {
	ListEntryAccessor accessor;
	ListOrArrayComparator(left, right, result_data, lhs_sel, rhs_sel, sel_count, accessor, comp, result_validity);
}

static void ArrayComparator(const Vector &left, const Vector &right, int8_t *result_data,
                            const SelectionVector &lhs_sel, const SelectionVector &rhs_sel, idx_t sel_count,
                            const ExpressionType comp, optional_ptr<ValidityMask> result_validity = nullptr) {
	ArrayEntryAccessor accessor(ArrayType::GetSize(left.GetType()));
	ListOrArrayComparator(left, right, result_data, lhs_sel, rhs_sel, sel_count, accessor, comp, result_validity);
}

static void DistinctComparatorTypeSwitch(const Vector &left, const Vector &right, int8_t *result_data,
                                         const SelectionVector &lhs_sel, const SelectionVector &rhs_sel,
                                         idx_t sel_count) {
	D_ASSERT(left.GetType().InternalType() == right.GetType().InternalType());
	const auto comp = ExpressionType::COMPARE_NOT_DISTINCT_FROM;
	switch (left.GetType().InternalType()) {
	case PhysicalType::BOOL:
	case PhysicalType::INT8:
		DistinctComparatorExecute::Execute<int8_t>(left, right, result_data, lhs_sel, rhs_sel, sel_count);
		break;
	case PhysicalType::INT16:
		DistinctComparatorExecute::Execute<int16_t>(left, right, result_data, lhs_sel, rhs_sel, sel_count);
		break;
	case PhysicalType::INT32:
		DistinctComparatorExecute::Execute<int32_t>(left, right, result_data, lhs_sel, rhs_sel, sel_count);
		break;
	case PhysicalType::INT64:
		DistinctComparatorExecute::Execute<int64_t>(left, right, result_data, lhs_sel, rhs_sel, sel_count);
		break;
	case PhysicalType::UINT8:
		DistinctComparatorExecute::Execute<uint8_t>(left, right, result_data, lhs_sel, rhs_sel, sel_count);
		break;
	case PhysicalType::UINT16:
		DistinctComparatorExecute::Execute<uint16_t>(left, right, result_data, lhs_sel, rhs_sel, sel_count);
		break;
	case PhysicalType::UINT32:
		DistinctComparatorExecute::Execute<uint32_t>(left, right, result_data, lhs_sel, rhs_sel, sel_count);
		break;
	case PhysicalType::UINT64:
		DistinctComparatorExecute::Execute<uint64_t>(left, right, result_data, lhs_sel, rhs_sel, sel_count);
		break;
	case PhysicalType::INT128:
		DistinctComparatorExecute::Execute<hugeint_t>(left, right, result_data, lhs_sel, rhs_sel, sel_count);
		break;
	case PhysicalType::UINT128:
		DistinctComparatorExecute::Execute<uhugeint_t>(left, right, result_data, lhs_sel, rhs_sel, sel_count);
		break;
	case PhysicalType::FLOAT:
		DistinctComparatorExecute::Execute<float>(left, right, result_data, lhs_sel, rhs_sel, sel_count);
		break;
	case PhysicalType::DOUBLE:
		DistinctComparatorExecute::Execute<double>(left, right, result_data, lhs_sel, rhs_sel, sel_count);
		break;
	case PhysicalType::INTERVAL:
		DistinctComparatorExecute::Execute<interval_t>(left, right, result_data, lhs_sel, rhs_sel, sel_count);
		break;
	case PhysicalType::VARCHAR:
		DistinctComparatorExecute::Execute<string_t>(left, right, result_data, lhs_sel, rhs_sel, sel_count);
		break;
	case PhysicalType::STRUCT:
		if (left.GetType().id() == LogicalTypeId::UNION) {
			UnionComparator(left, right, result_data, lhs_sel, rhs_sel, sel_count, comp);
		} else {
			StructComparator(left, right, result_data, lhs_sel, rhs_sel, sel_count, comp);
		}
		break;
	case PhysicalType::LIST:
		ListComparator(left, right, result_data, lhs_sel, rhs_sel, sel_count, comp);
		break;
	case PhysicalType::ARRAY:
		ArrayComparator(left, right, result_data, lhs_sel, rhs_sel, sel_count, comp);
		break;
	default:
		throw InternalException("Invalid type for comparator");
	}
}

static void ComparatorTypeSwitch(const Vector &left, const Vector &right, int8_t *result_data,
                                 const SelectionVector &lhs_sel, const SelectionVector &rhs_sel, idx_t sel_count,
                                 const ExpressionType comp, ValidityMask &validity) {
	D_ASSERT(left.GetType().InternalType() == right.GetType().InternalType());
	switch (left.GetType().InternalType()) {
	case PhysicalType::BOOL:
	case PhysicalType::INT8:
		StandardComparatorExecute::Execute<int8_t>(left, right, result_data, lhs_sel, rhs_sel, sel_count, validity);
		break;
	case PhysicalType::INT16:
		StandardComparatorExecute::Execute<int16_t>(left, right, result_data, lhs_sel, rhs_sel, sel_count, validity);
		break;
	case PhysicalType::INT32:
		StandardComparatorExecute::Execute<int32_t>(left, right, result_data, lhs_sel, rhs_sel, sel_count, validity);
		break;
	case PhysicalType::INT64:
		StandardComparatorExecute::Execute<int64_t>(left, right, result_data, lhs_sel, rhs_sel, sel_count, validity);
		break;
	case PhysicalType::UINT8:
		StandardComparatorExecute::Execute<uint8_t>(left, right, result_data, lhs_sel, rhs_sel, sel_count, validity);
		break;
	case PhysicalType::UINT16:
		StandardComparatorExecute::Execute<uint16_t>(left, right, result_data, lhs_sel, rhs_sel, sel_count, validity);
		break;
	case PhysicalType::UINT32:
		StandardComparatorExecute::Execute<uint32_t>(left, right, result_data, lhs_sel, rhs_sel, sel_count, validity);
		break;
	case PhysicalType::UINT64:
		StandardComparatorExecute::Execute<uint64_t>(left, right, result_data, lhs_sel, rhs_sel, sel_count, validity);
		break;
	case PhysicalType::INT128:
		StandardComparatorExecute::Execute<hugeint_t>(left, right, result_data, lhs_sel, rhs_sel, sel_count, validity);
		break;
	case PhysicalType::UINT128:
		StandardComparatorExecute::Execute<uhugeint_t>(left, right, result_data, lhs_sel, rhs_sel, sel_count, validity);
		break;
	case PhysicalType::FLOAT:
		StandardComparatorExecute::Execute<float>(left, right, result_data, lhs_sel, rhs_sel, sel_count, validity);
		break;
	case PhysicalType::DOUBLE:
		StandardComparatorExecute::Execute<double>(left, right, result_data, lhs_sel, rhs_sel, sel_count, validity);
		break;
	case PhysicalType::INTERVAL:
		StandardComparatorExecute::Execute<interval_t>(left, right, result_data, lhs_sel, rhs_sel, sel_count, validity);
		break;
	case PhysicalType::VARCHAR:
		StandardComparatorExecute::Execute<string_t>(left, right, result_data, lhs_sel, rhs_sel, sel_count, validity);
		break;
	case PhysicalType::STRUCT:
		if (left.GetType().id() == LogicalTypeId::UNION) {
			UnionComparator(left, right, result_data, lhs_sel, rhs_sel, sel_count, comp, validity);
		} else {
			StructComparator(left, right, result_data, lhs_sel, rhs_sel, sel_count, comp, validity);
		}
		break;
	case PhysicalType::LIST:
		ListComparator(left, right, result_data, lhs_sel, rhs_sel, sel_count, comp, validity);
		break;
	case PhysicalType::ARRAY:
		ArrayComparator(left, right, result_data, lhs_sel, rhs_sel, sel_count, comp, validity);
		break;
	default:
		throw InternalException("Invalid type for comparator");
	}
}

template <class T, class OP>
static void ComparatorExecute(const Vector &left, const Vector &right, Vector &result, idx_t count) {
	BinaryExecutor::Execute<T, T, int8_t, OP>(left, right, result, count);
}

template <class OP>
static bool TryPrimitiveComparatorExecute(const Vector &left, const Vector &right, Vector &result, idx_t count) {
#if DUCKDB_SMALLER_BINARY(primitive_comparator_execute)
	return false;
#else
	D_ASSERT(left.GetType().InternalType() == right.GetType().InternalType());
	switch (left.GetType().InternalType()) {
	case PhysicalType::BOOL:
	case PhysicalType::INT8:
		ComparatorExecute<int8_t, OP>(left, right, result, count);
		return true;
	case PhysicalType::INT16:
		ComparatorExecute<int16_t, OP>(left, right, result, count);
		return true;
	case PhysicalType::INT32:
		ComparatorExecute<int32_t, OP>(left, right, result, count);
		return true;
	case PhysicalType::INT64:
		ComparatorExecute<int64_t, OP>(left, right, result, count);
		return true;
	case PhysicalType::UINT8:
		ComparatorExecute<uint8_t, OP>(left, right, result, count);
		return true;
	case PhysicalType::UINT16:
		ComparatorExecute<uint16_t, OP>(left, right, result, count);
		return true;
	case PhysicalType::UINT32:
		ComparatorExecute<uint32_t, OP>(left, right, result, count);
		return true;
	case PhysicalType::UINT64:
		ComparatorExecute<uint64_t, OP>(left, right, result, count);
		return true;
	case PhysicalType::INT128:
		ComparatorExecute<hugeint_t, OP>(left, right, result, count);
		return true;
	case PhysicalType::UINT128:
		ComparatorExecute<uhugeint_t, OP>(left, right, result, count);
		return true;
	case PhysicalType::FLOAT:
		ComparatorExecute<float, OP>(left, right, result, count);
		return true;
	case PhysicalType::DOUBLE:
		ComparatorExecute<double, OP>(left, right, result, count);
		return true;
	case PhysicalType::INTERVAL:
		ComparatorExecute<interval_t, OP>(left, right, result, count);
		return true;
	case PhysicalType::VARCHAR:
		ComparatorExecute<string_t, OP>(left, right, result, count);
		return true;
	default:
		return false;
	}
#endif
}

void VectorOperations::ComparatorFill(const Vector &left, const Vector &right, Vector &result, idx_t count,
                                      const ExpressionType comp) {
	D_ASSERT(result.GetType() == LogicalType::TINYINT);
	if (!TryPrimitiveComparatorExecute<duckdb::Comparator>(left, right, result, count)) {
		result.SetVectorType(VectorType::FLAT_VECTOR);
		auto result_data = FlatVector::GetDataMutable<int8_t>(result);
		auto &sel = *FlatVector::IncrementalSelectionVector();
		auto &validity = FlatVector::ValidityMutable(result);
		ComparatorTypeSwitch(left, right, result_data, sel, sel, count, comp, validity);
	}
	FlatVector::SetSize(result, count);
}

void VectorOperations::Comparator(const Vector &left, const Vector &right, Vector &result, const ExpressionType comp) {
	const auto count = GetComparisonCount(left, right, "Comparator");
	ComparatorFill(left, right, result, count, comp);
}

template <class T, class OP>
static void DistinctExecuteGenericLoop(const T *__restrict ldata, const T *__restrict rdata,
                                       int8_t *__restrict result_data, const SelectionVector *__restrict lsel,
                                       const SelectionVector *__restrict rsel, idx_t count, const ValidityMask &lmask,
                                       const ValidityMask &rmask) {
	for (idx_t i = 0; i < count; i++) {
		auto lindex = lsel->get_index(i);
		auto rindex = rsel->get_index(i);
		result_data[i] = OP::template Operation<T>(ldata[lindex], rdata[rindex], !lmask.RowIsValid(lindex),
		                                           !rmask.RowIsValid(rindex));
	}
}

template <class T, class OP>
static void DistinctExecuteConstant(const Vector &left, const Vector &right, Vector &result) {
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	auto ldata = ConstantVector::GetData<T>(left);
	auto rdata = ConstantVector::GetData<T>(right);
	auto result_data = ConstantVector::GetData<int8_t>(result);
	*result_data =
	    OP::template Operation<T>(*ldata, *rdata, ConstantVector::IsNull(left), ConstantVector::IsNull(right));
}

template <class T, class OP>
static void DistinctExecute(const Vector &left, const Vector &right, Vector &result, idx_t count) {
	if (left.GetVectorType() == VectorType::CONSTANT_VECTOR && right.GetVectorType() == VectorType::CONSTANT_VECTOR) {
		DistinctExecuteConstant<T, OP>(left, right, result);
	} else {
		UnifiedVectorFormat ldata, rdata;
		left.ToUnifiedFormat(ldata);
		right.ToUnifiedFormat(rdata);
		result.SetVectorType(VectorType::FLAT_VECTOR);
		auto result_data = FlatVector::GetDataMutable<int8_t>(result);
		DistinctExecuteGenericLoop<T, OP>(UnifiedVectorFormat::GetData<T>(ldata),
		                                  UnifiedVectorFormat::GetData<T>(rdata), result_data, ldata.sel, rdata.sel,
		                                  count, ldata.validity, rdata.validity);
	}
}

template <class OP>
static bool TryPrimitiveDistinctComparatorExecute(const Vector &left, const Vector &right, Vector &result,
                                                  idx_t count) {
#if DUCKDB_SMALLER_BINARY(primitive_distinct_execute)
	return false;
#else
	D_ASSERT(left.GetType().InternalType() == right.GetType().InternalType());
	switch (left.GetType().InternalType()) {
	case PhysicalType::BOOL:
	case PhysicalType::INT8:
		DistinctExecute<int8_t, OP>(left, right, result, count);
		return true;
	case PhysicalType::INT16:
		DistinctExecute<int16_t, OP>(left, right, result, count);
		return true;
	case PhysicalType::INT32:
		DistinctExecute<int32_t, OP>(left, right, result, count);
		return true;
	case PhysicalType::INT64:
		DistinctExecute<int64_t, OP>(left, right, result, count);
		return true;
	case PhysicalType::UINT8:
		DistinctExecute<uint8_t, OP>(left, right, result, count);
		return true;
	case PhysicalType::UINT16:
		DistinctExecute<uint16_t, OP>(left, right, result, count);
		return true;
	case PhysicalType::UINT32:
		DistinctExecute<uint32_t, OP>(left, right, result, count);
		return true;
	case PhysicalType::UINT64:
		DistinctExecute<uint64_t, OP>(left, right, result, count);
		return true;
	case PhysicalType::INT128:
		DistinctExecute<hugeint_t, OP>(left, right, result, count);
		return true;
	case PhysicalType::UINT128:
		DistinctExecute<uhugeint_t, OP>(left, right, result, count);
		return true;
	case PhysicalType::FLOAT:
		DistinctExecute<float, OP>(left, right, result, count);
		return true;
	case PhysicalType::DOUBLE:
		DistinctExecute<double, OP>(left, right, result, count);
		return true;
	case PhysicalType::INTERVAL:
		DistinctExecute<interval_t, OP>(left, right, result, count);
		return true;
	case PhysicalType::VARCHAR:
		DistinctExecute<string_t, OP>(left, right, result, count);
		return true;
	default:
		return false;
	}
#endif
}

void VectorOperations::DistinctComparatorFill(const Vector &left, const Vector &right, Vector &result, idx_t count) {
	D_ASSERT(result.GetType() == LogicalType::TINYINT);
	if (!TryPrimitiveDistinctComparatorExecute<duckdb::DistinctComparator>(left, right, result, count)) {
		result.SetVectorType(VectorType::FLAT_VECTOR);
		auto result_data = FlatVector::GetDataMutable<int8_t>(result);
		auto &sel = *FlatVector::IncrementalSelectionVector();
		DistinctComparatorTypeSwitch(left, right, result_data, sel, sel, count);
	}
	FlatVector::SetSize(result, count);
}

void VectorOperations::DistinctComparator(const Vector &left, const Vector &right, Vector &result) {
	const auto count = GetComparisonCount(left, right, "DistinctComparator");
	DistinctComparatorFill(left, right, result, count);
}

void VectorOperations::DistinctComparatorNullsFirstFill(const Vector &left, const Vector &right, Vector &result,
                                                        idx_t count) {
	if (TryPrimitiveDistinctComparatorExecute<duckdb::DistinctComparatorNullsFirst>(left, right, result, count)) {
		FlatVector::SetSize(result, count);
		return;
	}
	// run the NULLS LAST comparator, then flip the sign for NULL-involving rows
	// note that even for NULLS FIRST, ONLY the top-level is NULLS FIRST,
	// i.e. within structs we still use NULLS LAST semantics
	DistinctComparatorFill(left, right, result, count);
	result.Flatten();
	auto result_data = FlatVector::GetDataMutable<int8_t>(result);
	auto left_validity = left.Validity();
	auto right_validity = right.Validity();
	if (!left_validity.CanHaveNull() && !right_validity.CanHaveNull()) {
		return;
	}
	for (idx_t i = 0; i < count; i++) {
		bool left_null = !left_validity.IsValid(i);
		bool right_null = !right_validity.IsValid(i);
		if ((left_null || right_null) && !(left_null && right_null)) {
			result_data[i] = UnsafeNumericCast<int8_t>(-result_data[i]);
		}
	}
}

void VectorOperations::DistinctComparatorNullsFirst(const Vector &left, const Vector &right, Vector &result) {
	const auto count = GetComparisonCount(left, right, "DistinctComparatorNullsFirst");
	DistinctComparatorNullsFirstFill(left, right, result, count);
}

} // namespace duckdb
