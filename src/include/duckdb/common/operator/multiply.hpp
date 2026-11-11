//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/operator/multiply.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/type_util.hpp"
#include "duckdb/common/types/cast_helpers.hpp"

namespace duckdb {

struct interval_t;

struct MultiplyOperator {
	template <class TA, class TB, class TR>
	static inline TR Operation(TA left, TB right) {
		return left * right;
	}
};

template <>
float MultiplyOperator::Operation(float left, float right);
template <>
double MultiplyOperator::Operation(double left, double right);
template <>
interval_t MultiplyOperator::Operation(interval_t left, int64_t right);
template <>
interval_t MultiplyOperator::Operation(int64_t left, interval_t right);
template <>
interval_t MultiplyOperator::Operation(interval_t left, double right);
template <>
interval_t MultiplyOperator::Operation(double left, interval_t right);

struct TryMultiplyOperator {
	template <class TA, class TB, class TR>
	static inline bool Operation(TA left, TB right, TR &result) {
		InternalException::Throw("Unimplemented type for TryMultiplyOperator");
	}
};

template <>
bool TryMultiplyOperator::Operation(uint8_t left, uint8_t right, uint8_t &result);
template <>
bool TryMultiplyOperator::Operation(uint16_t left, uint16_t right, uint16_t &result);
template <>
bool TryMultiplyOperator::Operation(uint32_t left, uint32_t right, uint32_t &result);
template <>
bool TryMultiplyOperator::Operation(uint64_t left, uint64_t right, uint64_t &result);

template <>
bool TryMultiplyOperator::Operation(int8_t left, int8_t right, int8_t &result);
template <>
bool TryMultiplyOperator::Operation(int16_t left, int16_t right, int16_t &result);
template <>
bool TryMultiplyOperator::Operation(int32_t left, int32_t right, int32_t &result);
template <>
DUCKDB_API bool TryMultiplyOperator::Operation(int64_t left, int64_t right, int64_t &result);
template <>
DUCKDB_API bool TryMultiplyOperator::Operation(hugeint_t left, hugeint_t right, hugeint_t &result);
template <>
DUCKDB_API bool TryMultiplyOperator::Operation(uhugeint_t left, uhugeint_t right, uhugeint_t &result);

template <>
bool TryMultiplyOperator::Operation(interval_t left, double right, interval_t &result);

struct MultiplyOperatorOverflowCheck {
	template <class TA, class TB, class TR>
	static inline TR Operation(TA left, TB right) {
		TR result;
		if (!TryMultiplyOperator::Operation(left, right, result)) {
			throw OutOfRangeException("Overflow in multiplication of %s (%s * %s)!", TypeIdToString(GetTypeId<TA>()),
			                          NumericHelper::ToString(left), NumericHelper::ToString(right));
		}
		return result;
	}
};

struct TryDecimalMultiply {
	template <class TA, class TB, class TR>
	static inline bool Operation(TA left, TB right, TR &result) {
		InternalException::Throw("Unimplemented type for TryDecimalMultiply");
	}
};

template <>
bool TryDecimalMultiply::Operation(int16_t left, int16_t right, int16_t &result);
template <>
bool TryDecimalMultiply::Operation(int32_t left, int32_t right, int32_t &result);
template <>
bool TryDecimalMultiply::Operation(int64_t left, int64_t right, int64_t &result);
template <>
bool TryDecimalMultiply::Operation(hugeint_t left, hugeint_t right, hugeint_t &result);

struct DecimalMultiplyOverflowCheck {
	template <class TA, class TB, class TR>
	static inline TR Operation(TA left, TB right) {
		TR result;
		if (!TryDecimalMultiply::Operation<TA, TB, TR>(left, right, result)) {
			throw OutOfRangeException("Overflow in multiplication of DECIMAL(18) (%d * %d). You might want to add an "
			                          "explicit cast to a bigger decimal.",
			                          left, right);
		}
		return result;
	}
};

template <>
hugeint_t DecimalMultiplyOverflowCheck::Operation(hugeint_t left, hugeint_t right);

} // namespace duckdb
