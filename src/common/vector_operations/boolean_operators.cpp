//===--------------------------------------------------------------------===//
// boolean_operators.cpp
// Description: This file contains the implementation of the boolean
// operations AND OR !
//===--------------------------------------------------------------------===//

#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"

namespace duckdb {

namespace {
//===--------------------------------------------------------------------===//
// AND/OR
//===--------------------------------------------------------------------===//
template <class OP>
void FlatBooleanNullmask(const Vector &left, const Vector &right, Vector &result, idx_t count) {
	D_ASSERT(&left != &result && &right != &result);
	// we use uint8 to avoid load of gunk bools
	auto left_data = FlatVector::GetData<uint8_t>(left);
	auto right_data = FlatVector::GetData<uint8_t>(right);
	auto &left_validity = FlatVector::Validity(left);
	auto &right_validity = FlatVector::Validity(right);

	result.SetVectorType(VectorType::FLAT_VECTOR);
	FlatVector::SetSize(result, count);
	auto result_data = FlatVector::GetDataMutable<bool>(result);
	auto &result_validity = FlatVector::ValidityMutable(result);
	result_validity.Reset(count);

	if (left_validity.CannotHaveNull() && right_validity.CannotHaveNull()) {
		for (idx_t i = 0; i < count; i++) {
			result_data[i] = OP::SimpleOperation(left_data[i] > 0, right_data[i] > 0);
		}
		return;
	}
	// process 64 rows at a time, computing the result validity for the whole entry at once
	const auto entry_count = ValidityMask::EntryCount(count);
	for (idx_t entry_idx = 0; entry_idx < entry_count; entry_idx++) {
		const auto base_idx = entry_idx * ValidityMask::BITS_PER_VALUE;
		const auto entry_size = MinValue<idx_t>(ValidityMask::BITS_PER_VALUE, count - base_idx);
		validity_t left_bits = 0;
		validity_t right_bits = 0;
		for (idx_t i = 0; i < entry_size; i++) {
			const bool left_value = left_data[base_idx + i] > 0;
			const bool right_value = right_data[base_idx + i] > 0;
			result_data[base_idx + i] = OP::SimpleOperation(left_value, right_value);
			left_bits |= validity_t(left_value) << i;
			right_bits |= validity_t(right_value) << i;
		}
		auto valid_bits = OP::ResultValidity(left_bits, right_bits, left_validity.GetValidityEntry(entry_idx),
		                                     right_validity.GetValidityEntry(entry_idx));
		// rows past the end of the vector are marked valid
		valid_bits |= ~ValidityMask::EntryWithValidBits(entry_size);
		if (!ValidityMask::AllValid(valid_bits)) {
			result_validity.EnsureWritable();
			result_validity.GetData()[entry_idx] = valid_bits;
		}
	}
}

template <class OP>
void TemplatedBooleanNullmask(const Vector &left, const Vector &right, Vector &result) {
	const auto count = (left.GetVectorType() == VectorType::CONSTANT_VECTOR) ? right.size() : left.size();
	D_ASSERT(left.GetType().id() == LogicalTypeId::BOOLEAN && right.GetType().id() == LogicalTypeId::BOOLEAN &&
	         result.GetType().id() == LogicalTypeId::BOOLEAN);

	if (left.GetVectorType() == VectorType::CONSTANT_VECTOR && right.GetVectorType() == VectorType::CONSTANT_VECTOR) {
		// operation on two constants, result is constant vector
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
		FlatVector::SetSize(result, count);
		auto ldata = ConstantVector::GetData<uint8_t>(left);
		auto rdata = ConstantVector::GetData<uint8_t>(right);
		auto result_data = ConstantVector::GetData<bool>(result);

		bool is_null = OP::Operation(*ldata > 0, *rdata > 0, ConstantVector::IsNull(left),
		                             ConstantVector::IsNull(right), *result_data);
		if (is_null) {
			ConstantVector::SetNull(result, count_t(count));
		}
		return;
	}
	if (left.GetVectorType() == VectorType::FLAT_VECTOR && right.GetVectorType() == VectorType::FLAT_VECTOR) {
		FlatBooleanNullmask<OP>(left, right, result, count);
		return;
	}
	// perform generic loop
	// we use uint8 to avoid load of gunk bools
	auto left_data = left.Values<uint8_t>();
	auto right_data = right.Values<uint8_t>();

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto result_data = FlatVector::Writer<bool>(result, count);
	if (left_data.CanHaveNull() || right_data.CanHaveNull()) {
		for (idx_t i = 0; i < count; i++) {
			auto left_entry = left_data[i];
			auto right_entry = right_data[i];
			bool result_value = false;
			bool is_null = OP::Operation(left_entry.GetValueUnsafe() > 0, right_entry.GetValueUnsafe() > 0,
			                             !left_entry.IsValid(), !right_entry.IsValid(), result_value);
			if (is_null) {
				result_data.WriteNull(result_value);
			} else {
				result_data.WriteValue(result_value);
			}
		}
	} else {
		for (idx_t i = 0; i < count; i++) {
			result_data.WriteValue(OP::SimpleOperation(left_data.GetValueUnsafe(i), right_data.GetValueUnsafe(i)));
		}
	}
}

/*
SQL AND Rules:

TRUE  AND TRUE   = TRUE
TRUE  AND FALSE  = FALSE
TRUE  AND NULL   = NULL
FALSE AND TRUE   = FALSE
FALSE AND FALSE  = FALSE
FALSE AND NULL   = FALSE
NULL  AND TRUE   = NULL
NULL  AND FALSE  = FALSE
NULL  AND NULL   = NULL

Basically:
- Only true if both are true
- False if either is false (regardless of NULLs)
- NULL otherwise
*/
struct TernaryAnd {
	static bool SimpleOperation(bool left, bool right) {
		return left && right;
	}
	//! The result is valid if both sides are valid, or if either side is a valid FALSE
	static validity_t ResultValidity(validity_t left, validity_t right, validity_t left_valid, validity_t right_valid) {
		return (left_valid & right_valid) | (left_valid & ~left) | (right_valid & ~right);
	}
	static bool Operation(bool left, bool right, bool left_null, bool right_null, bool &result) {
		if (left_null && right_null) {
			// both NULL:
			// result is NULL
			return true;
		} else if (left_null) {
			// left is NULL:
			// result is FALSE if right is false
			// result is NULL if right is true
			result = right;
			return right;
		} else if (right_null) {
			// right is NULL:
			// result is FALSE if left is false
			// result is NULL if left is true
			result = left;
			return left;
		} else {
			// no NULL: perform the AND
			result = left && right;
			return false;
		}
	}
};

/*
SQL OR Rules:

OR
TRUE  OR TRUE  = TRUE
TRUE  OR FALSE = TRUE
TRUE  OR NULL  = TRUE
FALSE OR TRUE  = TRUE
FALSE OR FALSE = FALSE
FALSE OR NULL  = NULL
NULL  OR TRUE  = TRUE
NULL  OR FALSE = NULL
NULL  OR NULL  = NULL

Basically:
- Only false if both are false
- True if either is true (regardless of NULLs)
- NULL otherwise
*/

struct TernaryOr {
	static bool SimpleOperation(bool left, bool right) {
		return left || right;
	}
	//! The result is valid if both sides are valid, or if either side is a valid TRUE
	static validity_t ResultValidity(validity_t left, validity_t right, validity_t left_valid, validity_t right_valid) {
		return (left_valid & right_valid) | (left_valid & left) | (right_valid & right);
	}
	static bool Operation(bool left, bool right, bool left_null, bool right_null, bool &result) {
		if (left_null && right_null) {
			// both NULL:
			// result is NULL
			return true;
		} else if (left_null) {
			// left is NULL:
			// result is TRUE if right is true
			// result is NULL if right is false
			result = right;
			return !right;
		} else if (right_null) {
			// right is NULL:
			// result is TRUE if left is true
			// result is NULL if left is false
			result = left;
			return !left;
		} else {
			// no NULL: perform the OR
			result = left || right;
			return false;
		}
	}
};

struct NotOperator {
	template <class TA, class TR>
	static inline TR Operation(TA left) {
		return !left;
	}
};

} // namespace

void VectorOperations::And(const Vector &left, const Vector &right, Vector &result) {
	const bool left_is_const = left.GetVectorType() == VectorType::CONSTANT_VECTOR;
	const bool right_is_const = right.GetVectorType() == VectorType::CONSTANT_VECTOR;
	if (!left_is_const && !right_is_const && left.size() != right.size()) {
		throw InternalException("Mismatch in input vector sizes for And - left has %d rows but right has %d",
		                        left.size(), right.size());
	}
	TemplatedBooleanNullmask<TernaryAnd>(left, right, result);
}

void VectorOperations::Or(const Vector &left, const Vector &right, Vector &result) {
	const bool left_is_const = left.GetVectorType() == VectorType::CONSTANT_VECTOR;
	const bool right_is_const = right.GetVectorType() == VectorType::CONSTANT_VECTOR;
	if (!left_is_const && !right_is_const && left.size() != right.size()) {
		throw InternalException("Mismatch in input vector sizes for Or - left has %d rows but right has %d",
		                        left.size(), right.size());
	}
	TemplatedBooleanNullmask<TernaryOr>(left, right, result);
}

void VectorOperations::Not(const Vector &input, Vector &result) {
	D_ASSERT(input.GetType() == LogicalType::BOOLEAN && result.GetType() == LogicalType::BOOLEAN);
	UnaryExecutor::Execute<bool, bool, NotOperator>(input, result);
}

} // namespace duckdb
