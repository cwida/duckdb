#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/function/cast/default_casts.hpp"
#include "duckdb/common/hugeint.hpp"
#include "duckdb/common/operator/string_cast.hpp"
#include "duckdb/common/operator/numeric_cast.hpp"
#include "duckdb/common/operator/decimal_cast_operators.hpp"
#include "duckdb/common/operator/multiply.hpp"
#include "duckdb/common/operator/add.hpp"
#include "duckdb/common/operator/subtract.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/blob.hpp"
#include "duckdb/common/types/cast_helpers.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/types/geometry.hpp"
#include "duckdb/common/types.hpp"
#include "fast_float/fast_float.h"
#include "duckdb/common/types/bit.hpp"
#include "duckdb/common/operator/integer_cast_operator.hpp"
#include "duckdb/common/operator/double_cast_operator.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/common/serializer/binary_deserializer.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"

#include <cctype>
#include <cmath>
#include <cstdlib>

namespace duckdb {

void TryCast::ThrowUnimplemented(PhysicalType source, PhysicalType target, optional_ptr<CastParameters> parameters) {
	throw UnimplementedErrorMessage(source, target, parameters);
}

ConversionException TryCast::UnimplementedErrorMessage(PhysicalType source, PhysicalType target,
                                                       optional_ptr<CastParameters> parameters) {
	QueryLocation query_location;
	if (parameters) {
		query_location = parameters->query_location;
		if (parameters->cast_source && parameters->cast_target) {
			auto &source_expr = *parameters->cast_source;
			auto &target_expr = *parameters->cast_target;
			return ConversionException(
			    query_location, UnimplementedCastMessage(source_expr.GetReturnType(), target_expr.GetReturnType()));
		}
	}
	return ConversionException(query_location, "Unimplemented type for cast (%s -> %s)", source, target);
}

string TryCast::UnimplementedCastMessage(const LogicalType &source, const LogicalType &target) {
	return StringUtil::Format("Unimplemented type for cast (%s -> %s)", source, target);
}

//===--------------------------------------------------------------------===//
// Cast bool -> Numeric
//===--------------------------------------------------------------------===//
template <>
bool TryCast::Operation(bool input, bool &result, bool strict) {
	return NumericTryCast::Operation<bool, bool>(input, result, strict);
}

template <>
bool TryCast::Operation(bool input, int8_t &result, bool strict) {
	return NumericTryCast::Operation<bool, int8_t>(input, result, strict);
}

template <>
bool TryCast::Operation(bool input, int16_t &result, bool strict) {
	return NumericTryCast::Operation<bool, int16_t>(input, result, strict);
}

template <>
bool TryCast::Operation(bool input, int32_t &result, bool strict) {
	return NumericTryCast::Operation<bool, int32_t>(input, result, strict);
}

template <>
bool TryCast::Operation(bool input, int64_t &result, bool strict) {
	return NumericTryCast::Operation<bool, int64_t>(input, result, strict);
}

template <>
bool TryCast::Operation(bool input, hugeint_t &result, bool strict) {
	return NumericTryCast::Operation<bool, hugeint_t>(input, result, strict);
}

template <>
bool TryCast::Operation(bool input, uhugeint_t &result, bool strict) {
	return NumericTryCast::Operation<bool, uhugeint_t>(input, result, strict);
}

template <>
bool TryCast::Operation(bool input, uint8_t &result, bool strict) {
	return NumericTryCast::Operation<bool, uint8_t>(input, result, strict);
}

template <>
bool TryCast::Operation(bool input, uint16_t &result, bool strict) {
	return NumericTryCast::Operation<bool, uint16_t>(input, result, strict);
}

template <>
bool TryCast::Operation(bool input, uint32_t &result, bool strict) {
	return NumericTryCast::Operation<bool, uint32_t>(input, result, strict);
}

template <>
bool TryCast::Operation(bool input, uint64_t &result, bool strict) {
	return NumericTryCast::Operation<bool, uint64_t>(input, result, strict);
}

template <>
bool TryCast::Operation(bool input, float &result, bool strict) {
	return NumericTryCast::Operation<bool, float>(input, result, strict);
}

template <>
bool TryCast::Operation(bool input, double &result, bool strict) {
	return NumericTryCast::Operation<bool, double>(input, result, strict);
}

//===--------------------------------------------------------------------===//
// Cast int8_t -> Numeric
//===--------------------------------------------------------------------===//
template <>
bool TryCast::Operation(int8_t input, bool &result, bool strict) {
	return NumericTryCast::Operation<int8_t, bool>(input, result, strict);
}

template <>
bool TryCast::Operation(int8_t input, int8_t &result, bool strict) {
	return NumericTryCast::Operation<int8_t, int8_t>(input, result, strict);
}

template <>
bool TryCast::Operation(int8_t input, int16_t &result, bool strict) {
	return NumericTryCast::Operation<int8_t, int16_t>(input, result, strict);
}

template <>
bool TryCast::Operation(int8_t input, int32_t &result, bool strict) {
	return NumericTryCast::Operation<int8_t, int32_t>(input, result, strict);
}

template <>
bool TryCast::Operation(int8_t input, int64_t &result, bool strict) {
	return NumericTryCast::Operation<int8_t, int64_t>(input, result, strict);
}

template <>
bool TryCast::Operation(int8_t input, hugeint_t &result, bool strict) {
	return NumericTryCast::Operation<int8_t, hugeint_t>(input, result, strict);
}

template <>
bool TryCast::Operation(int8_t input, uhugeint_t &result, bool strict) {
	return NumericTryCast::Operation<int8_t, uhugeint_t>(input, result, strict);
}

template <>
bool TryCast::Operation(int8_t input, uint8_t &result, bool strict) {
	return NumericTryCast::Operation<int8_t, uint8_t>(input, result, strict);
}

template <>
bool TryCast::Operation(int8_t input, uint16_t &result, bool strict) {
	return NumericTryCast::Operation<int8_t, uint16_t>(input, result, strict);
}

template <>
bool TryCast::Operation(int8_t input, uint32_t &result, bool strict) {
	return NumericTryCast::Operation<int8_t, uint32_t>(input, result, strict);
}

template <>
bool TryCast::Operation(int8_t input, uint64_t &result, bool strict) {
	return NumericTryCast::Operation<int8_t, uint64_t>(input, result, strict);
}

template <>
bool TryCast::Operation(int8_t input, float &result, bool strict) {
	return NumericTryCast::Operation<int8_t, float>(input, result, strict);
}

template <>
bool TryCast::Operation(int8_t input, double &result, bool strict) {
	return NumericTryCast::Operation<int8_t, double>(input, result, strict);
}

//===--------------------------------------------------------------------===//
// Cast int16_t -> Numeric
//===--------------------------------------------------------------------===//
template <>
bool TryCast::Operation(int16_t input, bool &result, bool strict) {
	return NumericTryCast::Operation<int16_t, bool>(input, result, strict);
}

template <>
bool TryCast::Operation(int16_t input, int8_t &result, bool strict) {
	return NumericTryCast::Operation<int16_t, int8_t>(input, result, strict);
}

template <>
bool TryCast::Operation(int16_t input, int16_t &result, bool strict) {
	return NumericTryCast::Operation<int16_t, int16_t>(input, result, strict);
}

template <>
bool TryCast::Operation(int16_t input, int32_t &result, bool strict) {
	return NumericTryCast::Operation<int16_t, int32_t>(input, result, strict);
}

template <>
bool TryCast::Operation(int16_t input, int64_t &result, bool strict) {
	return NumericTryCast::Operation<int16_t, int64_t>(input, result, strict);
}

template <>
bool TryCast::Operation(int16_t input, hugeint_t &result, bool strict) {
	return NumericTryCast::Operation<int16_t, hugeint_t>(input, result, strict);
}

template <>
bool TryCast::Operation(int16_t input, uhugeint_t &result, bool strict) {
	return NumericTryCast::Operation<int16_t, uhugeint_t>(input, result, strict);
}

template <>
bool TryCast::Operation(int16_t input, uint8_t &result, bool strict) {
	return NumericTryCast::Operation<int16_t, uint8_t>(input, result, strict);
}

template <>
bool TryCast::Operation(int16_t input, uint16_t &result, bool strict) {
	return NumericTryCast::Operation<int16_t, uint16_t>(input, result, strict);
}

template <>
bool TryCast::Operation(int16_t input, uint32_t &result, bool strict) {
	return NumericTryCast::Operation<int16_t, uint32_t>(input, result, strict);
}

template <>
bool TryCast::Operation(int16_t input, uint64_t &result, bool strict) {
	return NumericTryCast::Operation<int16_t, uint64_t>(input, result, strict);
}

template <>
bool TryCast::Operation(int16_t input, float &result, bool strict) {
	return NumericTryCast::Operation<int16_t, float>(input, result, strict);
}

template <>
bool TryCast::Operation(int16_t input, double &result, bool strict) {
	return NumericTryCast::Operation<int16_t, double>(input, result, strict);
}

//===--------------------------------------------------------------------===//
// Cast int32_t -> Numeric
//===--------------------------------------------------------------------===//
template <>
bool TryCast::Operation(int32_t input, bool &result, bool strict) {
	return NumericTryCast::Operation<int32_t, bool>(input, result, strict);
}

template <>
bool TryCast::Operation(int32_t input, int8_t &result, bool strict) {
	return NumericTryCast::Operation<int32_t, int8_t>(input, result, strict);
}

template <>
bool TryCast::Operation(int32_t input, int16_t &result, bool strict) {
	return NumericTryCast::Operation<int32_t, int16_t>(input, result, strict);
}

template <>
bool TryCast::Operation(int32_t input, int32_t &result, bool strict) {
	return NumericTryCast::Operation<int32_t, int32_t>(input, result, strict);
}

template <>
bool TryCast::Operation(int32_t input, int64_t &result, bool strict) {
	return NumericTryCast::Operation<int32_t, int64_t>(input, result, strict);
}

template <>
bool TryCast::Operation(int32_t input, hugeint_t &result, bool strict) {
	return NumericTryCast::Operation<int32_t, hugeint_t>(input, result, strict);
}

template <>
bool TryCast::Operation(int32_t input, uhugeint_t &result, bool strict) {
	return NumericTryCast::Operation<int32_t, uhugeint_t>(input, result, strict);
}

template <>
bool TryCast::Operation(int32_t input, uint8_t &result, bool strict) {
	return NumericTryCast::Operation<int32_t, uint8_t>(input, result, strict);
}

template <>
bool TryCast::Operation(int32_t input, uint16_t &result, bool strict) {
	return NumericTryCast::Operation<int32_t, uint16_t>(input, result, strict);
}

template <>
bool TryCast::Operation(int32_t input, uint32_t &result, bool strict) {
	return NumericTryCast::Operation<int32_t, uint32_t>(input, result, strict);
}

template <>
bool TryCast::Operation(int32_t input, uint64_t &result, bool strict) {
	return NumericTryCast::Operation<int32_t, uint64_t>(input, result, strict);
}

template <>
bool TryCast::Operation(int32_t input, float &result, bool strict) {
	return NumericTryCast::Operation<int32_t, float>(input, result, strict);
}

template <>
bool TryCast::Operation(int32_t input, double &result, bool strict) {
	return NumericTryCast::Operation<int32_t, double>(input, result, strict);
}

//===--------------------------------------------------------------------===//
// Cast int64_t -> Numeric
//===--------------------------------------------------------------------===//
template <>
bool TryCast::Operation(int64_t input, bool &result, bool strict) {
	return NumericTryCast::Operation<int64_t, bool>(input, result, strict);
}

template <>
bool TryCast::Operation(int64_t input, int8_t &result, bool strict) {
	return NumericTryCast::Operation<int64_t, int8_t>(input, result, strict);
}

template <>
bool TryCast::Operation(int64_t input, int16_t &result, bool strict) {
	return NumericTryCast::Operation<int64_t, int16_t>(input, result, strict);
}

template <>
bool TryCast::Operation(int64_t input, int32_t &result, bool strict) {
	return NumericTryCast::Operation<int64_t, int32_t>(input, result, strict);
}

template <>
bool TryCast::Operation(int64_t input, int64_t &result, bool strict) {
	return NumericTryCast::Operation<int64_t, int64_t>(input, result, strict);
}

template <>
bool TryCast::Operation(int64_t input, hugeint_t &result, bool strict) {
	return NumericTryCast::Operation<int64_t, hugeint_t>(input, result, strict);
}

template <>
bool TryCast::Operation(int64_t input, uhugeint_t &result, bool strict) {
	return NumericTryCast::Operation<int64_t, uhugeint_t>(input, result, strict);
}

template <>
bool TryCast::Operation(int64_t input, uint8_t &result, bool strict) {
	return NumericTryCast::Operation<int64_t, uint8_t>(input, result, strict);
}

template <>
bool TryCast::Operation(int64_t input, uint16_t &result, bool strict) {
	return NumericTryCast::Operation<int64_t, uint16_t>(input, result, strict);
}

template <>
bool TryCast::Operation(int64_t input, uint32_t &result, bool strict) {
	return NumericTryCast::Operation<int64_t, uint32_t>(input, result, strict);
}

template <>
bool TryCast::Operation(int64_t input, uint64_t &result, bool strict) {
	return NumericTryCast::Operation<int64_t, uint64_t>(input, result, strict);
}

template <>
bool TryCast::Operation(int64_t input, float &result, bool strict) {
	return NumericTryCast::Operation<int64_t, float>(input, result, strict);
}

template <>
bool TryCast::Operation(int64_t input, double &result, bool strict) {
	return NumericTryCast::Operation<int64_t, double>(input, result, strict);
}

//===--------------------------------------------------------------------===//
// Cast hugeint_t -> Numeric
//===--------------------------------------------------------------------===//
template <>
bool TryCast::Operation(hugeint_t input, bool &result, bool strict) {
	return NumericTryCast::Operation<hugeint_t, bool>(input, result, strict);
}

template <>
bool TryCast::Operation(hugeint_t input, int8_t &result, bool strict) {
	return NumericTryCast::Operation<hugeint_t, int8_t>(input, result, strict);
}

template <>
bool TryCast::Operation(hugeint_t input, int16_t &result, bool strict) {
	return NumericTryCast::Operation<hugeint_t, int16_t>(input, result, strict);
}

template <>
bool TryCast::Operation(hugeint_t input, int32_t &result, bool strict) {
	return NumericTryCast::Operation<hugeint_t, int32_t>(input, result, strict);
}

template <>
bool TryCast::Operation(hugeint_t input, int64_t &result, bool strict) {
	return NumericTryCast::Operation<hugeint_t, int64_t>(input, result, strict);
}

template <>
bool TryCast::Operation(hugeint_t input, hugeint_t &result, bool strict) {
	return NumericTryCast::Operation<hugeint_t, hugeint_t>(input, result, strict);
}

template <>
bool TryCast::Operation(hugeint_t input, uhugeint_t &result, bool strict) {
	return NumericTryCast::Operation<hugeint_t, uhugeint_t>(input, result, strict);
}

template <>
bool TryCast::Operation(hugeint_t input, uint8_t &result, bool strict) {
	return NumericTryCast::Operation<hugeint_t, uint8_t>(input, result, strict);
}

template <>
bool TryCast::Operation(hugeint_t input, uint16_t &result, bool strict) {
	return NumericTryCast::Operation<hugeint_t, uint16_t>(input, result, strict);
}

template <>
bool TryCast::Operation(hugeint_t input, uint32_t &result, bool strict) {
	return NumericTryCast::Operation<hugeint_t, uint32_t>(input, result, strict);
}

template <>
bool TryCast::Operation(hugeint_t input, uint64_t &result, bool strict) {
	return NumericTryCast::Operation<hugeint_t, uint64_t>(input, result, strict);
}

template <>
bool TryCast::Operation(hugeint_t input, float &result, bool strict) {
	return NumericTryCast::Operation<hugeint_t, float>(input, result, strict);
}

template <>
bool TryCast::Operation(hugeint_t input, double &result, bool strict) {
	return NumericTryCast::Operation<hugeint_t, double>(input, result, strict);
}

//===--------------------------------------------------------------------===//
// Cast uhugeint_t -> Numeric
//===--------------------------------------------------------------------===//
template <>
bool TryCast::Operation(uhugeint_t input, bool &result, bool strict) {
	return NumericTryCast::Operation<uhugeint_t, bool>(input, result, strict);
}

template <>
bool TryCast::Operation(uhugeint_t input, int8_t &result, bool strict) {
	return NumericTryCast::Operation<uhugeint_t, int8_t>(input, result, strict);
}

template <>
bool TryCast::Operation(uhugeint_t input, int16_t &result, bool strict) {
	return NumericTryCast::Operation<uhugeint_t, int16_t>(input, result, strict);
}

template <>
bool TryCast::Operation(uhugeint_t input, int32_t &result, bool strict) {
	return NumericTryCast::Operation<uhugeint_t, int32_t>(input, result, strict);
}

template <>
bool TryCast::Operation(uhugeint_t input, int64_t &result, bool strict) {
	return NumericTryCast::Operation<uhugeint_t, int64_t>(input, result, strict);
}

template <>
bool TryCast::Operation(uhugeint_t input, uhugeint_t &result, bool strict) {
	return NumericTryCast::Operation<uhugeint_t, uhugeint_t>(input, result, strict);
}

template <>
bool TryCast::Operation(uhugeint_t input, hugeint_t &result, bool strict) {
	return NumericTryCast::Operation<uhugeint_t, hugeint_t>(input, result, strict);
}

template <>
bool TryCast::Operation(uhugeint_t input, uint8_t &result, bool strict) {
	return NumericTryCast::Operation<uhugeint_t, uint8_t>(input, result, strict);
}

template <>
bool TryCast::Operation(uhugeint_t input, uint16_t &result, bool strict) {
	return NumericTryCast::Operation<uhugeint_t, uint16_t>(input, result, strict);
}

template <>
bool TryCast::Operation(uhugeint_t input, uint32_t &result, bool strict) {
	return NumericTryCast::Operation<uhugeint_t, uint32_t>(input, result, strict);
}

template <>
bool TryCast::Operation(uhugeint_t input, uint64_t &result, bool strict) {
	return NumericTryCast::Operation<uhugeint_t, uint64_t>(input, result, strict);
}

template <>
bool TryCast::Operation(uhugeint_t input, float &result, bool strict) {
	return NumericTryCast::Operation<uhugeint_t, float>(input, result, strict);
}

template <>
bool TryCast::Operation(uhugeint_t input, double &result, bool strict) {
	return NumericTryCast::Operation<uhugeint_t, double>(input, result, strict);
}

//===--------------------------------------------------------------------===//
// Cast uint8_t -> Numeric
//===--------------------------------------------------------------------===//
template <>
bool TryCast::Operation(uint8_t input, bool &result, bool strict) {
	return NumericTryCast::Operation<uint8_t, bool>(input, result, strict);
}

template <>
bool TryCast::Operation(uint8_t input, int8_t &result, bool strict) {
	return NumericTryCast::Operation<uint8_t, int8_t>(input, result, strict);
}

template <>
bool TryCast::Operation(uint8_t input, int16_t &result, bool strict) {
	return NumericTryCast::Operation<uint8_t, int16_t>(input, result, strict);
}

template <>
bool TryCast::Operation(uint8_t input, int32_t &result, bool strict) {
	return NumericTryCast::Operation<uint8_t, int32_t>(input, result, strict);
}

template <>
bool TryCast::Operation(uint8_t input, int64_t &result, bool strict) {
	return NumericTryCast::Operation<uint8_t, int64_t>(input, result, strict);
}

template <>
bool TryCast::Operation(uint8_t input, hugeint_t &result, bool strict) {
	return NumericTryCast::Operation<uint8_t, hugeint_t>(input, result, strict);
}

template <>
bool TryCast::Operation(uint8_t input, uhugeint_t &result, bool strict) {
	return NumericTryCast::Operation<uint8_t, uhugeint_t>(input, result, strict);
}

template <>
bool TryCast::Operation(uint8_t input, uint8_t &result, bool strict) {
	return NumericTryCast::Operation<uint8_t, uint8_t>(input, result, strict);
}

template <>
bool TryCast::Operation(uint8_t input, uint16_t &result, bool strict) {
	return NumericTryCast::Operation<uint8_t, uint16_t>(input, result, strict);
}

template <>
bool TryCast::Operation(uint8_t input, uint32_t &result, bool strict) {
	return NumericTryCast::Operation<uint8_t, uint32_t>(input, result, strict);
}

template <>
bool TryCast::Operation(uint8_t input, uint64_t &result, bool strict) {
	return NumericTryCast::Operation<uint8_t, uint64_t>(input, result, strict);
}

template <>
bool TryCast::Operation(uint8_t input, float &result, bool strict) {
	return NumericTryCast::Operation<uint8_t, float>(input, result, strict);
}

template <>
bool TryCast::Operation(uint8_t input, double &result, bool strict) {
	return NumericTryCast::Operation<uint8_t, double>(input, result, strict);
}

//===--------------------------------------------------------------------===//
// Cast uint16_t -> Numeric
//===--------------------------------------------------------------------===//
template <>
bool TryCast::Operation(uint16_t input, bool &result, bool strict) {
	return NumericTryCast::Operation<uint16_t, bool>(input, result, strict);
}

template <>
bool TryCast::Operation(uint16_t input, int8_t &result, bool strict) {
	return NumericTryCast::Operation<uint16_t, int8_t>(input, result, strict);
}

template <>
bool TryCast::Operation(uint16_t input, int16_t &result, bool strict) {
	return NumericTryCast::Operation<uint16_t, int16_t>(input, result, strict);
}

template <>
bool TryCast::Operation(uint16_t input, int32_t &result, bool strict) {
	return NumericTryCast::Operation<uint16_t, int32_t>(input, result, strict);
}

template <>
bool TryCast::Operation(uint16_t input, int64_t &result, bool strict) {
	return NumericTryCast::Operation<uint16_t, int64_t>(input, result, strict);
}

template <>
bool TryCast::Operation(uint16_t input, hugeint_t &result, bool strict) {
	return NumericTryCast::Operation<uint16_t, hugeint_t>(input, result, strict);
}

template <>
bool TryCast::Operation(uint16_t input, uhugeint_t &result, bool strict) {
	return NumericTryCast::Operation<uint16_t, uhugeint_t>(input, result, strict);
}

template <>
bool TryCast::Operation(uint16_t input, uint8_t &result, bool strict) {
	return NumericTryCast::Operation<uint16_t, uint8_t>(input, result, strict);
}

template <>
bool TryCast::Operation(uint16_t input, uint16_t &result, bool strict) {
	return NumericTryCast::Operation<uint16_t, uint16_t>(input, result, strict);
}

template <>
bool TryCast::Operation(uint16_t input, uint32_t &result, bool strict) {
	return NumericTryCast::Operation<uint16_t, uint32_t>(input, result, strict);
}

template <>
bool TryCast::Operation(uint16_t input, uint64_t &result, bool strict) {
	return NumericTryCast::Operation<uint16_t, uint64_t>(input, result, strict);
}

template <>
bool TryCast::Operation(uint16_t input, float &result, bool strict) {
	return NumericTryCast::Operation<uint16_t, float>(input, result, strict);
}

template <>
bool TryCast::Operation(uint16_t input, double &result, bool strict) {
	return NumericTryCast::Operation<uint16_t, double>(input, result, strict);
}

//===--------------------------------------------------------------------===//
// Cast uint32_t -> Numeric
//===--------------------------------------------------------------------===//
template <>
bool TryCast::Operation(uint32_t input, bool &result, bool strict) {
	return NumericTryCast::Operation<uint32_t, bool>(input, result, strict);
}

template <>
bool TryCast::Operation(uint32_t input, int8_t &result, bool strict) {
	return NumericTryCast::Operation<uint32_t, int8_t>(input, result, strict);
}

template <>
bool TryCast::Operation(uint32_t input, int16_t &result, bool strict) {
	return NumericTryCast::Operation<uint32_t, int16_t>(input, result, strict);
}

template <>
bool TryCast::Operation(uint32_t input, int32_t &result, bool strict) {
	return NumericTryCast::Operation<uint32_t, int32_t>(input, result, strict);
}

template <>
bool TryCast::Operation(uint32_t input, int64_t &result, bool strict) {
	return NumericTryCast::Operation<uint32_t, int64_t>(input, result, strict);
}

template <>
bool TryCast::Operation(uint32_t input, hugeint_t &result, bool strict) {
	return NumericTryCast::Operation<uint32_t, hugeint_t>(input, result, strict);
}

template <>
bool TryCast::Operation(uint32_t input, uhugeint_t &result, bool strict) {
	return NumericTryCast::Operation<uint32_t, uhugeint_t>(input, result, strict);
}

template <>
bool TryCast::Operation(uint32_t input, uint8_t &result, bool strict) {
	return NumericTryCast::Operation<uint32_t, uint8_t>(input, result, strict);
}

template <>
bool TryCast::Operation(uint32_t input, uint16_t &result, bool strict) {
	return NumericTryCast::Operation<uint32_t, uint16_t>(input, result, strict);
}

template <>
bool TryCast::Operation(uint32_t input, uint32_t &result, bool strict) {
	return NumericTryCast::Operation<uint32_t, uint32_t>(input, result, strict);
}

template <>
bool TryCast::Operation(uint32_t input, uint64_t &result, bool strict) {
	return NumericTryCast::Operation<uint32_t, uint64_t>(input, result, strict);
}

template <>
bool TryCast::Operation(uint32_t input, float &result, bool strict) {
	return NumericTryCast::Operation<uint32_t, float>(input, result, strict);
}

template <>
bool TryCast::Operation(uint32_t input, double &result, bool strict) {
	return NumericTryCast::Operation<uint32_t, double>(input, result, strict);
}

//===--------------------------------------------------------------------===//
// Cast uint64_t -> Numeric
//===--------------------------------------------------------------------===//
template <>
bool TryCast::Operation(uint64_t input, bool &result, bool strict) {
	return NumericTryCast::Operation<uint64_t, bool>(input, result, strict);
}

template <>
bool TryCast::Operation(uint64_t input, int8_t &result, bool strict) {
	return NumericTryCast::Operation<uint64_t, int8_t>(input, result, strict);
}

template <>
bool TryCast::Operation(uint64_t input, int16_t &result, bool strict) {
	return NumericTryCast::Operation<uint64_t, int16_t>(input, result, strict);
}

template <>
bool TryCast::Operation(uint64_t input, int32_t &result, bool strict) {
	return NumericTryCast::Operation<uint64_t, int32_t>(input, result, strict);
}

template <>
bool TryCast::Operation(uint64_t input, int64_t &result, bool strict) {
	return NumericTryCast::Operation<uint64_t, int64_t>(input, result, strict);
}

template <>
bool TryCast::Operation(uint64_t input, hugeint_t &result, bool strict) {
	return NumericTryCast::Operation<uint64_t, hugeint_t>(input, result, strict);
}

template <>
bool TryCast::Operation(uint64_t input, uhugeint_t &result, bool strict) {
	return NumericTryCast::Operation<uint64_t, uhugeint_t>(input, result, strict);
}

template <>
bool TryCast::Operation(uint64_t input, uint8_t &result, bool strict) {
	return NumericTryCast::Operation<uint64_t, uint8_t>(input, result, strict);
}

template <>
bool TryCast::Operation(uint64_t input, uint16_t &result, bool strict) {
	return NumericTryCast::Operation<uint64_t, uint16_t>(input, result, strict);
}

template <>
bool TryCast::Operation(uint64_t input, uint32_t &result, bool strict) {
	return NumericTryCast::Operation<uint64_t, uint32_t>(input, result, strict);
}

template <>
bool TryCast::Operation(uint64_t input, uint64_t &result, bool strict) {
	return NumericTryCast::Operation<uint64_t, uint64_t>(input, result, strict);
}

template <>
bool TryCast::Operation(uint64_t input, float &result, bool strict) {
	return NumericTryCast::Operation<uint64_t, float>(input, result, strict);
}

template <>
bool TryCast::Operation(uint64_t input, double &result, bool strict) {
	return NumericTryCast::Operation<uint64_t, double>(input, result, strict);
}

//===--------------------------------------------------------------------===//
// Cast float -> Numeric
//===--------------------------------------------------------------------===//
template <>
bool TryCast::Operation(float input, bool &result, bool strict) {
	return NumericTryCast::Operation<float, bool>(input, result, strict);
}

template <>
bool TryCast::Operation(float input, int8_t &result, bool strict) {
	return NumericTryCast::Operation<float, int8_t>(input, result, strict);
}

template <>
bool TryCast::Operation(float input, int16_t &result, bool strict) {
	return NumericTryCast::Operation<float, int16_t>(input, result, strict);
}

template <>
bool TryCast::Operation(float input, int32_t &result, bool strict) {
	return NumericTryCast::Operation<float, int32_t>(input, result, strict);
}

template <>
bool TryCast::Operation(float input, int64_t &result, bool strict) {
	return NumericTryCast::Operation<float, int64_t>(input, result, strict);
}

template <>
bool TryCast::Operation(float input, hugeint_t &result, bool strict) {
	return NumericTryCast::Operation<float, hugeint_t>(input, result, strict);
}

template <>
bool TryCast::Operation(float input, uhugeint_t &result, bool strict) {
	return NumericTryCast::Operation<float, uhugeint_t>(input, result, strict);
}

template <>
bool TryCast::Operation(float input, uint8_t &result, bool strict) {
	return NumericTryCast::Operation<float, uint8_t>(input, result, strict);
}

template <>
bool TryCast::Operation(float input, uint16_t &result, bool strict) {
	return NumericTryCast::Operation<float, uint16_t>(input, result, strict);
}

template <>
bool TryCast::Operation(float input, uint32_t &result, bool strict) {
	return NumericTryCast::Operation<float, uint32_t>(input, result, strict);
}

template <>
bool TryCast::Operation(float input, uint64_t &result, bool strict) {
	return NumericTryCast::Operation<float, uint64_t>(input, result, strict);
}

template <>
bool TryCast::Operation(float input, float &result, bool strict) {
	return NumericTryCast::Operation<float, float>(input, result, strict);
}

template <>
bool TryCast::Operation(float input, double &result, bool strict) {
	return NumericTryCast::Operation<float, double>(input, result, strict);
}

//===--------------------------------------------------------------------===//
// Cast double -> Numeric
//===--------------------------------------------------------------------===//
template <>
bool TryCast::Operation(double input, bool &result, bool strict) {
	return NumericTryCast::Operation<double, bool>(input, result, strict);
}

template <>
bool TryCast::Operation(double input, int8_t &result, bool strict) {
	return NumericTryCast::Operation<double, int8_t>(input, result, strict);
}

template <>
bool TryCast::Operation(double input, int16_t &result, bool strict) {
	return NumericTryCast::Operation<double, int16_t>(input, result, strict);
}

template <>
bool TryCast::Operation(double input, int32_t &result, bool strict) {
	return NumericTryCast::Operation<double, int32_t>(input, result, strict);
}

template <>
bool TryCast::Operation(double input, int64_t &result, bool strict) {
	return NumericTryCast::Operation<double, int64_t>(input, result, strict);
}

template <>
bool TryCast::Operation(double input, hugeint_t &result, bool strict) {
	return NumericTryCast::Operation<double, hugeint_t>(input, result, strict);
}

template <>
bool TryCast::Operation(double input, uhugeint_t &result, bool strict) {
	return NumericTryCast::Operation<double, uhugeint_t>(input, result, strict);
}

template <>
bool TryCast::Operation(double input, uint8_t &result, bool strict) {
	return NumericTryCast::Operation<double, uint8_t>(input, result, strict);
}

template <>
bool TryCast::Operation(double input, uint16_t &result, bool strict) {
	return NumericTryCast::Operation<double, uint16_t>(input, result, strict);
}

template <>
bool TryCast::Operation(double input, uint32_t &result, bool strict) {
	return NumericTryCast::Operation<double, uint32_t>(input, result, strict);
}

template <>
bool TryCast::Operation(double input, uint64_t &result, bool strict) {
	return NumericTryCast::Operation<double, uint64_t>(input, result, strict);
}

template <>
bool TryCast::Operation(double input, float &result, bool strict) {
	return NumericTryCast::Operation<double, float>(input, result, strict);
}

template <>
bool TryCast::Operation(double input, double &result, bool strict) {
	return NumericTryCast::Operation<double, double>(input, result, strict);
}

//===--------------------------------------------------------------------===//
// Cast String -> Numeric
//===--------------------------------------------------------------------===//

template <>
bool TryCast::Operation(string_t input, bool &result, bool strict) {
	auto input_data = reinterpret_cast<const char *>(input.GetData());
	auto input_size = input.GetSize();
	return TryCastStringBool(input_data, input_size, result, strict);
}
template <>
bool TryCast::Operation(string_t input, int8_t &result, bool strict) {
	return TrySimpleIntegerCast<int8_t>(input.GetData(), input.GetSize(), result, strict);
}
template <>
bool TryCast::Operation(string_t input, int16_t &result, bool strict) {
	return TrySimpleIntegerCast<int16_t>(input.GetData(), input.GetSize(), result, strict);
}
template <>
bool TryCast::Operation(string_t input, int32_t &result, bool strict) {
	return TrySimpleIntegerCast<int32_t>(input.GetData(), input.GetSize(), result, strict);
}
template <>
bool TryCast::Operation(string_t input, int64_t &result, bool strict) {
	return TrySimpleIntegerCast<int64_t>(input.GetData(), input.GetSize(), result, strict);
}

template <>
bool TryCast::Operation(string_t input, uint8_t &result, bool strict) {
	return TrySimpleIntegerCast<uint8_t, false>(input.GetData(), input.GetSize(), result, strict);
}
template <>
bool TryCast::Operation(string_t input, uint16_t &result, bool strict) {
	return TrySimpleIntegerCast<uint16_t, false>(input.GetData(), input.GetSize(), result, strict);
}
template <>
bool TryCast::Operation(string_t input, uint32_t &result, bool strict) {
	return TrySimpleIntegerCast<uint32_t, false>(input.GetData(), input.GetSize(), result, strict);
}
template <>
bool TryCast::Operation(string_t input, uint64_t &result, bool strict) {
	return TrySimpleIntegerCast<uint64_t, false>(input.GetData(), input.GetSize(), result, strict);
}

template <>
bool TryCast::Operation(string_t input, float &result, bool strict) {
	return TryDoubleCast<float>(input.GetData(), input.GetSize(), result, strict);
}

template <>
bool TryCast::Operation(string_t input, double &result, bool strict) {
	return TryDoubleCast<double>(input.GetData(), input.GetSize(), result, strict);
}

template <>
bool TryCastErrorMessageCommaSeparated::Operation(string_t input, float &result, CastParameters &parameters) {
	if (!TryDoubleCast<float>(input.GetData(), input.GetSize(), result, parameters.strict, ',')) {
		HandleCastError::AssignError(StringUtil::Format("Could not cast string to float: \"%s\"", input.GetString()),
		                             parameters);
		return false;
	}
	return true;
}

template <>
bool TryCastErrorMessageCommaSeparated::Operation(string_t input, double &result, CastParameters &parameters) {
	if (!TryDoubleCast<double>(input.GetData(), input.GetSize(), result, parameters.strict, ',')) {
		HandleCastError::AssignError(StringUtil::Format("Could not cast string to double: \"%s\"", input.GetString()),
		                             parameters);
		return false;
	}
	return true;
}

//===--------------------------------------------------------------------===//
// Cast From Date
//===--------------------------------------------------------------------===//
template <>
bool TryCast::Operation(date_t input, date_t &result, bool strict) {
	result = input;
	return true;
}

template <>
bool TryCast::Operation(date_t input, timestamp_t &result, bool strict) {
	if (input == date_t::infinity()) {
		result = timestamp_t::infinity();
		return true;
	} else if (input == date_t::ninfinity()) {
		result = timestamp_t::ninfinity();
		return true;
	}
	return Timestamp::TryFromDatetime(input, Time::FromTime(0, 0, 0), result);
}

//===--------------------------------------------------------------------===//
// Cast From Time
//===--------------------------------------------------------------------===//
template <typename SRC, typename DST>
static bool TryCastTimeBase(const SRC &input, DST &result, bool strict = true) {
	if (SRC::PRECISION == DST::PRECISION || !input.IsFinite()) {
		result.value = input.value;
		return true;
	} else if (SRC::PRECISION < DST::PRECISION) {
		const int64_t scaling = DST::PRECISION / SRC::PRECISION;
		return TryMultiplyOperator::Operation(input.value, scaling, result.value);
	} else {
		//	Round away from the epoch.
		//	Scale first so we don't overflow.
		const int64_t power_of_ten = SRC::PRECISION / DST::PRECISION;
		const int64_t scaling = power_of_ten / 2;
		result.value = input.value / scaling; // NOLINT: power_of_ten >= 10 so scaling >= 5
		if (result.value < 0) {
			--result.value;
		} else {
			++result.value;
		}
		result.value /= 2;
		return true;
	}
}

template <typename SRC, typename DST>
static DST CastTimeBase(const SRC &input) {
	DST result;
	if (!TryCastTimeBase(input, result)) {
		throw ConversionException("Could not convert time to higher precision.");
	}
	return result;
}

template <>
bool TryCast::Operation(dtime_t input, dtime_t &result, bool strict) {
	return TryCastTimeBase<dtime_t, dtime_t>(input, result, strict);
}

template <>
bool TryCast::Operation(dtime_ns_t input, dtime_ns_t &result, bool strict) {
	return TryCastTimeBase<dtime_ns_t, dtime_ns_t>(input, result, strict);
}

template <>
bool TryCast::Operation(dtime_ns_t input, dtime_t &result, bool strict) {
	return TryCastTimeBase<dtime_ns_t, dtime_t>(input, result, strict);
}

template <>
bool TryCast::Operation(dtime_t input, dtime_ns_t &result, bool strict) {
	return TryCastTimeBase<dtime_t, dtime_ns_t>(input, result, strict);
}

template <>
bool TryCast::Operation(dtime_t input, dtime_tz_t &result, bool strict) {
	result = dtime_tz_t(input, 0);
	return true;
}

//===--------------------------------------------------------------------===//
// Cast From Time With Time Zone (Offset)
//===--------------------------------------------------------------------===//
template <>
bool TryCast::Operation(dtime_tz_t input, dtime_tz_t &result, bool strict) {
	result = input;
	return true;
}

template <>
bool TryCast::Operation(dtime_tz_t input, dtime_t &result, bool strict) {
	result = input.time();
	return true;
}

//===--------------------------------------------------------------------===//
// Cast From Timestamps
//===--------------------------------------------------------------------===//
template <typename SRC, typename DST>
static bool TryCastTimestampBase(const SRC &input, DST &result, bool strict = true) {
	if (SRC::PRECISION == DST::PRECISION || !input.IsFinite()) {
		result.value = input.value;
		return true;
	} else if (SRC::PRECISION < DST::PRECISION) {
		const int64_t scaling = DST::PRECISION / SRC::PRECISION;
		return TryMultiplyOperator::Operation(input.value, scaling, result.value);
	} else {
		//	Round away from the epoch.
		//	Scale first so we don't overflow.
		const int64_t power_of_ten = SRC::PRECISION / DST::PRECISION;
		const int64_t scaling = power_of_ten / 2;
		result.value = input.value / scaling; // NOLINT: power_of_ten >= 10 so scaling >= 5
		if (result.value < 0) {
			--result.value;
		} else {
			++result.value;
		}
		result.value /= 2;
		// rounding can go past the largest (or smallest) timestamp - the result must still be a valid timestamp
		if (DST::PRECISION < Interval::MICROS_PER_SEC) {
			int64_t micros;
			if (!TryMultiplyOperator::Operation(result.value, Interval::MICROS_PER_SEC / DST::PRECISION, micros) ||
			    !timestamp_t(micros).IsFinite()) {
				return false;
			}
		}
		return true;
	}
}

template <typename SRC, typename DST>
static DST CastTimestampOperation(const SRC &input, const string &error_message) {
	DST result;
	if (!TryCast::Operation(input, result)) {
		throw ConversionException(error_message);
	}
	return result;
}

template <typename SRC, typename DST>
static DST CastTimestampPrecisionOperation(const SRC &input) {
	return CastTimestampOperation<SRC, DST>(input, "Could not convert Timestamp to higher precision.");
}

template <typename SRC, typename DST>
static DST CastTimestampTargetOperation(const SRC &input) {
	return CastTimestampOperation<SRC, DST>(
	    input, StringUtil::Format("Could not convert Timestamp to %s.", TypeIdToString(GetTypeId<DST>())));
}

template <typename SRC>
static dtime_t CastTimestampTimeOperation(const SRC &input) {
	return Timestamp::GetTime(CastTimestampPrecisionOperation<SRC, timestamp_t>(input));
}

static dtime_ns_t CastTimestampNsTimeNsOperation(timestamp_ns_t input) {
	dtime_ns_t result;
	if (TryCast::Operation(input, result)) {
		return result;
	}
	return Timestamp::GetTimeNs(input);
}

template <>
bool TryCast::Operation(timestamp_t input, date_t &result, bool strict) {
	result = Timestamp::GetDate(input);
	return true;
}

template <>
bool TryCast::Operation(timestamp_t input, dtime_t &result, bool strict) {
	if (!input.IsFinite()) {
		return false;
	}
	result = Timestamp::GetTime(input);
	return true;
}

template <>
bool TryCast::Operation(timestamp_t input, dtime_tz_t &result, bool strict) {
	if (!input.IsFinite()) {
		return false;
	}
	result = dtime_tz_t(Timestamp::GetTime(input), 0);
	return true;
}

template <>
bool TryCast::Operation(timestamp_t input, timestamp_t &result, bool strict) {
	return TryCastTimestampBase<timestamp_t, timestamp_t>(input, result, strict);
}

template <>
bool TryCast::Operation(timestamp_sec_t input, date_t &result, bool strict) {
	timestamp_t us;
	if (!TryCast::Operation<timestamp_sec_t, timestamp_t>(input, us, strict)) {
		return false;
	}
	result = Timestamp::GetDate(us);
	return true;
}

template <>
bool TryCast::Operation(timestamp_sec_t input, dtime_t &result, bool strict) {
	if (!input.IsFinite()) {
		return false;
	}
	timestamp_t us;
	if (!TryCast::Operation<timestamp_sec_t, timestamp_t>(input, us, strict)) {
		return false;
	}
	result = Timestamp::GetTime(us);
	return true;
}

template <>
bool TryCast::Operation(timestamp_sec_t input, timestamp_sec_t &result, bool strict) {
	return TryCastTimestampBase<timestamp_sec_t, timestamp_sec_t>(input, result, strict);
}

template <>
bool TryCast::Operation(timestamp_t input, timestamp_sec_t &result, bool strict) {
	return TryCastTimestampBase<timestamp_t, timestamp_sec_t>(input, result, strict);
}

template <>
bool TryCast::Operation(timestamp_ms_t input, date_t &result, bool strict) {
	timestamp_t us;
	if (!TryCast::Operation<timestamp_ms_t, timestamp_t>(input, us, strict)) {
		return false;
	}
	result = Timestamp::GetDate(us);
	return true;
}

template <>
bool TryCast::Operation(timestamp_ms_t input, dtime_t &result, bool strict) {
	if (!input.IsFinite()) {
		return false;
	}
	timestamp_t us;
	if (!TryCast::Operation<timestamp_ms_t, timestamp_t>(input, us, strict)) {
		return false;
	}
	result = Timestamp::GetTime(us);
	return true;
}

template <>
bool TryCast::Operation(timestamp_ms_t input, timestamp_ms_t &result, bool strict) {
	return TryCastTimestampBase<timestamp_ms_t, timestamp_ms_t>(input, result, strict);
}

template <>
bool TryCast::Operation(timestamp_t input, timestamp_ms_t &result, bool strict) {
	return TryCastTimestampBase<timestamp_t, timestamp_ms_t>(input, result, strict);
}

template <>
bool TryCast::Operation(timestamp_ns_t input, date_t &result, bool strict) {
	timestamp_t us;
	if (!TryCast::Operation<timestamp_ns_t, timestamp_t>(input, us, strict)) {
		return false;
	}
	result = Timestamp::GetDate(us);
	return true;
}

template <>
bool TryCast::Operation(timestamp_ns_t input, dtime_t &result, bool strict) {
	if (!input.IsFinite()) {
		return false;
	}
	timestamp_t us;
	if (!TryCast::Operation<timestamp_ns_t, timestamp_t>(input, us, strict)) {
		return false;
	}
	result = Timestamp::GetTime(us);
	return true;
}

template <>
bool TryCast::Operation(timestamp_ns_t input, dtime_ns_t &result, bool strict) {
	if (!input.IsFinite()) {
		return false;
	}
	result = Timestamp::GetTimeNs(input);
	return true;
}

template <>
bool TryCast::Operation(timestamp_ns_t input, timestamp_ns_t &result, bool strict) {
	return TryCastTimestampBase<timestamp_ns_t, timestamp_ns_t>(input, result, strict);
}

template <>
bool TryCast::Operation(timestamp_t input, timestamp_ns_t &result, bool strict) {
	return TryCastTimestampBase<timestamp_t, timestamp_ns_t>(input, result, strict);
}

template <>
bool TryCast::Operation(timestamp_tz_t input, timestamp_tz_t &result, bool strict) {
	return TryCastTimestampBase<timestamp_tz_t, timestamp_tz_t>(input, result, strict);
}

template <>
bool TryCast::Operation(timestamp_tz_t input, dtime_tz_t &result, bool strict) {
	if (!input.IsFinite()) {
		return false;
	}
	result = dtime_tz_t(Timestamp::GetTime(timestamp_t(input)), 0);
	return true;
}

template <>
bool TryCast::Operation(timestamp_tz_t input, timestamp_t &result, bool strict) {
	return TryCastTimestampBase<timestamp_tz_t, timestamp_t>(input, result, strict);
}

template <>
bool TryCast::Operation(timestamp_tz_t input, timestamp_sec_t &result, bool strict) {
	return TryCastTimestampBase<timestamp_tz_t, timestamp_sec_t>(input, result, strict);
}

template <>
bool TryCast::Operation(timestamp_tz_t input, timestamp_ms_t &result, bool strict) {
	return TryCastTimestampBase<timestamp_tz_t, timestamp_ms_t>(input, result, strict);
}

template <>
bool TryCast::Operation(timestamp_tz_t input, timestamp_ns_t &result, bool strict) {
	return TryCastTimestampBase<timestamp_tz_t, timestamp_ns_t>(input, result, strict);
}

template <>
bool TryCast::Operation(timestamp_tz_ns_t input, timestamp_tz_ns_t &result, bool strict) {
	return TryCastTimestampBase<timestamp_tz_ns_t, timestamp_tz_ns_t>(input, result, strict);
}

template <>
bool TryCast::Operation(timestamp_tz_ns_t input, dtime_tz_t &result, bool strict) {
	if (!input.IsFinite()) {
		return false;
	}
	dtime_t time;
	if (!TryCast::Operation(timestamp_ns_t(input), time, strict)) {
		return false;
	}
	result = dtime_tz_t(time, 0);
	return true;
}

template <>
bool TryCast::Operation(timestamp_tz_ns_t input, timestamp_t &result, bool strict) {
	return TryCastTimestampBase<timestamp_tz_ns_t, timestamp_t>(input, result, strict);
}

template <>
bool TryCast::Operation(timestamp_ns_t input, timestamp_tz_ns_t &result, bool strict) {
	return TryCastTimestampBase<timestamp_ns_t, timestamp_tz_ns_t>(input, result, strict);
}

template <>
bool TryCast::Operation(timestamp_tz_ns_t input, timestamp_ns_t &result, bool strict) {
	return TryCastTimestampBase<timestamp_tz_ns_t, timestamp_ns_t>(input, result, strict);
}

template <>
bool TryCast::Operation(timestamp_tz_ns_t input, timestamp_ms_t &result, bool strict) {
	return TryCastTimestampBase<timestamp_tz_ns_t, timestamp_ms_t>(input, result, strict);
}

template <>
bool TryCast::Operation(timestamp_tz_ns_t input, timestamp_sec_t &result, bool strict) {
	return TryCastTimestampBase<timestamp_tz_ns_t, timestamp_sec_t>(input, result, strict);
}

template <>
bool TryCast::Operation(timestamp_t input, timestamp_tz_t &result, bool strict) {
	return TryCastTimestampBase<timestamp_t, timestamp_tz_t>(input, result, strict);
}

//===--------------------------------------------------------------------===//
// Cast from Interval
//===--------------------------------------------------------------------===//
template <>
bool TryCast::Operation(interval_t input, interval_t &result, bool strict) {
	result = input;
	return true;
}

//===--------------------------------------------------------------------===//
// Non-Standard Timestamps
//===--------------------------------------------------------------------===//
template <>
duckdb::string_t CastFromTimestampNS::Operation(duckdb::timestamp_ns_t input, StringHeap &heap) {
	return StringCast::Operation<timestamp_ns_t>(input, heap);
}
template <>
duckdb::string_t CastFromTimestampMS::Operation(duckdb::timestamp_ms_t input, StringHeap &heap) {
	return StringCast::Operation<timestamp_t>(Cast::Operation<timestamp_ms_t, timestamp_t>(input), heap);
}
template <>
duckdb::string_t CastFromTimestampSec::Operation(duckdb::timestamp_sec_t input, StringHeap &heap) {
	return StringCast::Operation<timestamp_t>(Cast::Operation<timestamp_sec_t, timestamp_t>(input), heap);
}

template <>
timestamp_ms_t Cast::Operation(timestamp_t input) {
	return CastTimestampPrecisionOperation<timestamp_t, timestamp_ms_t>(input);
}

template <>
timestamp_ns_t Cast::Operation(timestamp_t input) {
	return CastTimestampPrecisionOperation<timestamp_t, timestamp_ns_t>(input);
}

template <>
timestamp_sec_t Cast::Operation(timestamp_t input) {
	return CastTimestampPrecisionOperation<timestamp_t, timestamp_sec_t>(input);
}

template <>
dtime_t Cast::Operation(timestamp_t input) {
	return Timestamp::GetTime(input);
}

template <>
dtime_tz_t Cast::Operation(timestamp_t input) {
	return dtime_tz_t(Timestamp::GetTime(input), 0);
}

template <>
dtime_tz_t Cast::Operation(timestamp_tz_t input) {
	return dtime_tz_t(Timestamp::GetTime(timestamp_t(input)), 0);
}

template <>
dtime_tz_t Cast::Operation(timestamp_tz_ns_t input) {
	return dtime_tz_t(CastTimestampTimeOperation(timestamp_ns_t(input)), 0);
}

template <>
timestamp_t Cast::Operation(timestamp_ms_t input) {
	return CastTimestampPrecisionOperation<timestamp_ms_t, timestamp_t>(input);
}

template <>
date_t Cast::Operation(timestamp_ms_t input) {
	return CastTimestampTargetOperation<timestamp_ms_t, date_t>(input);
}

template <>
dtime_t Cast::Operation(timestamp_ms_t input) {
	return CastTimestampTimeOperation(input);
}

template <>
timestamp_ns_t Cast::Operation(timestamp_ms_t input) {
	return CastTimestampPrecisionOperation<timestamp_ms_t, timestamp_ns_t>(input);
}

template <>
bool TryCast::Operation(timestamp_ms_t input, timestamp_sec_t &result, bool strict) {
	return TryCastTimestampBase<timestamp_ms_t, timestamp_sec_t>(input, result, strict);
}

template <>
bool TryCast::Operation(timestamp_ms_t input, timestamp_ns_t &result, bool strict) {
	return TryCastTimestampBase<timestamp_ms_t, timestamp_ns_t>(input, result, strict);
}

template <>
bool TryCast::Operation(timestamp_ns_t input, timestamp_ms_t &result, bool strict) {
	return TryCastTimestampBase<timestamp_ns_t, timestamp_ms_t>(input, result, strict);
}

template <>
bool TryCast::Operation(timestamp_ns_t input, timestamp_t &result, bool strict) {
	return TryCastTimestampBase<timestamp_ns_t, timestamp_t>(input, result, strict);
}

template <>
bool TryCast::Operation(timestamp_ms_t input, timestamp_t &result, bool strict) {
	return TryCastTimestampBase<timestamp_ms_t, timestamp_t>(input, result, strict);
}

template <>
bool TryCast::Operation(timestamp_sec_t input, timestamp_t &result, bool strict) {
	return TryCastTimestampBase<timestamp_sec_t, timestamp_t>(input, result, strict);
}

template <>
bool TryCast::Operation(timestamp_sec_t input, timestamp_ms_t &result, bool strict) {
	return TryCastTimestampBase<timestamp_sec_t, timestamp_ms_t>(input, result, strict);
}

template <>
bool TryCast::Operation(timestamp_sec_t input, timestamp_ns_t &result, bool strict) {
	return TryCastTimestampBase<timestamp_sec_t, timestamp_ns_t>(input, result, strict);
}

template <>
timestamp_t Cast::Operation(timestamp_ns_t input) {
	return CastTimestampPrecisionOperation<timestamp_ns_t, timestamp_t>(input);
}

template <>
timestamp_t Cast::Operation(timestamp_sec_t input) {
	return CastTimestampPrecisionOperation<timestamp_sec_t, timestamp_t>(input);
}

template <>
date_t Cast::Operation(timestamp_ns_t input) {
	return CastTimestampTargetOperation<timestamp_ns_t, date_t>(input);
}

template <>
dtime_t Cast::Operation(timestamp_ns_t input) {
	return CastTimestampTimeOperation(input);
}

template <>
dtime_ns_t Cast::Operation(timestamp_ns_t input) {
	return CastTimestampNsTimeNsOperation(input);
}

template <>
timestamp_ms_t Cast::Operation(timestamp_sec_t input) {
	return CastTimestampPrecisionOperation<timestamp_sec_t, timestamp_ms_t>(input);
}

template <>
timestamp_ns_t Cast::Operation(timestamp_sec_t input) {
	return CastTimestampPrecisionOperation<timestamp_sec_t, timestamp_ns_t>(input);
}

template <>
date_t Cast::Operation(timestamp_sec_t input) {
	return CastTimestampTargetOperation<timestamp_sec_t, date_t>(input);
}

template <>
dtime_t Cast::Operation(timestamp_sec_t input) {
	return CastTimestampTimeOperation(input);
}

//===--------------------------------------------------------------------===//
// Cast To Timestamp
//===--------------------------------------------------------------------===//
template <>
bool TryCast::Operation(string_t input, timestamp_ms_t &result, bool strict) {
	timestamp_t us;
	if (!TryCast::Operation<string_t, timestamp_t>(input, us, strict)) {
		return false;
	}
	return TryCastTimestampBase<timestamp_t, timestamp_ms_t>(us, result, strict);
}

template <>
bool TryCast::Operation(string_t input, timestamp_sec_t &result, bool strict) {
	timestamp_t us;
	if (!TryCast::Operation<string_t, timestamp_t>(input, us, strict)) {
		return false;
	}
	return TryCastTimestampBase<timestamp_t, timestamp_sec_t>(us, result, strict);
}

template <>
bool TryCast::Operation(date_t input, timestamp_ns_t &result, bool strict) {
	timestamp_t us;
	if (!TryCast::Operation<date_t, timestamp_t>(input, us, strict)) {
		return false;
	}
	return TryCastTimestampBase<timestamp_t, timestamp_ns_t>(us, result, strict);
}

template <>
bool TryCast::Operation(date_t input, timestamp_ms_t &result, bool strict) {
	timestamp_t us;
	if (!TryCast::Operation<date_t, timestamp_t>(input, us, strict)) {
		return false;
	}
	return TryCastTimestampBase<timestamp_t, timestamp_ms_t>(us, result, strict);
}

template <>
bool TryCast::Operation(date_t input, timestamp_sec_t &result, bool strict) {
	timestamp_t us;
	if (!TryCast::Operation<date_t, timestamp_t>(input, us, strict)) {
		return false;
	}
	return TryCastTimestampBase<timestamp_t, timestamp_sec_t>(us, result, strict);
}

//===--------------------------------------------------------------------===//
// Cast From Blob
//===--------------------------------------------------------------------===//
template <>
string_t CastFromBlob::Operation(string_t input, StringHeap &heap) {
	idx_t result_size = Blob::GetStringSize(input);

	string_t result = heap.EmptyString(result_size);
	Blob::ToString(input, result.GetDataWriteable());
	result.Finalize();

	return result;
}

template <>
bool TryCastBlobToBit::Operation(string_t input, string_t &result, Vector &result_vector, CastParameters &parameters) {
	if (input.GetSize() == 0) {
		HandleCastError::AssignError("Cannot cast empty BLOB to BIT", parameters);
		return false;
	}
	result = StringVector::EmptyString(result_vector, input.GetSize() + 1);
	Bit::BlobToBit(input, result);
	result.Finalize();
	return true;
}

//===--------------------------------------------------------------------===//
// Cast From Bit
//===--------------------------------------------------------------------===//
template <>
string_t CastFromBitToString::Operation(string_t input, StringHeap &heap) {
	idx_t result_size = Bit::BitLength(input);
	string_t result = heap.EmptyString(result_size);
	Bit::ToString(input, result.GetDataWriteable());
	result.Finalize();

	return result;
}

//===--------------------------------------------------------------------===//
// Cast From Pointer
//===--------------------------------------------------------------------===//
template <>
string_t CastFromPointer::Operation(uintptr_t input, StringHeap &heap) {
	std::string s = duckdb_fmt::format("0x{:x}", input);
	return heap.AddString(s);
}

//===--------------------------------------------------------------------===//
// Cast To Pointer
//===--------------------------------------------------------------------===//
template <>
uintptr_t CastToPointer::Operation(string_t input) {
	auto data = input.GetData();
	auto size = input.GetSize();
	if (size < 3 || data[0] != '0' || (data[1] != 'x' && data[1] != 'X')) {
		throw ConversionException("Could not convert string '%s' to a pointer", input.GetString());
	}
	uint64_t address = 0;
	for (idx_t i = 2; i < size; i++) {
		if (!StringUtil::CharacterIsHex(data[i]) || address > (NumericLimits<uint64_t>::Maximum() >> 4)) {
			throw ConversionException("Could not convert string '%s' to a pointer", input.GetString());
		}
		address = (address << 4) | StringUtil::GetHexValue(data[i]);
	}
	return NumericCast<uintptr_t>(address);
}

//===--------------------------------------------------------------------===//
// Cast From Pointer
//===--------------------------------------------------------------------===//
template <>
string_t CastFromType::Operation(string_t input, StringHeap &heap) {
	MemoryStream stream(data_ptr_cast(input.GetDataWriteable()), input.GetSize());
	BinaryDeserializer deserializer(stream);
	try {
		auto type = LogicalType::Deserialize(deserializer);
		return heap.AddString(type.ToString());
	} catch (std::exception &ex) {
		// TODO: Format better error here?
		return heap.AddString(ex.what());
	}
}

//===--------------------------------------------------------------------===//
// Cast To Blob
//===--------------------------------------------------------------------===//
template <>
bool TryCastToBlob::Operation(string_t input, string_t &result, Vector &result_vector, CastParameters &parameters) {
	idx_t result_size;
	if (!Blob::TryGetBlobSize(input, result_size, parameters)) {
		return false;
	}

	result = StringVector::EmptyString(result_vector, result_size);
	Blob::ToBlob(input, data_ptr_cast(result.GetDataWriteable()));
	result.Finalize();
	return true;
}

//===--------------------------------------------------------------------===//
// Cast To Bit
//===--------------------------------------------------------------------===//
template <>
bool TryCastToBit::Operation(string_t input, string_t &result, Vector &result_vector, CastParameters &parameters) {
	idx_t result_size;
	if (!Bit::TryGetBitStringSize(input, result_size, parameters.error_message)) {
		return false;
	}

	result = StringVector::EmptyString(result_vector, result_size);
	if (!Bit::ToBit(input, result, parameters.error_message)) {
		return false;
	}
	result.Finalize();
	return true;
}

template <>
bool CastFromBitToNumeric::Operation(string_t input, bool &result, CastParameters &parameters) {
	D_ASSERT(input.GetSize() > 1);

	uint8_t value;
	bool success = CastFromBitToNumeric::Operation(input, value, parameters);
	result = (value > 0);
	return (success);
}

template <>
bool CastFromBitToNumeric::Operation(string_t input, hugeint_t &result, CastParameters &parameters) {
	D_ASSERT(input.GetSize() > 1);

	if (input.GetSize() - 1 > sizeof(hugeint_t)) {
		HandleCastError::AssignError("Bitstring doesn't fit inside of " + TypeIdToString(GetTypeId<hugeint_t>()),
		                             parameters);
		return false;
	}
	Bit::BitToNumeric(input, result);
	return (true);
}

template <>
bool CastFromBitToNumeric::Operation(string_t input, uhugeint_t &result, CastParameters &parameters) {
	D_ASSERT(input.GetSize() > 1);

	if (input.GetSize() - 1 > sizeof(uhugeint_t)) {
		HandleCastError::AssignError("Bitstring doesn't fit inside of " + TypeIdToString(GetTypeId<uhugeint_t>()),
		                             parameters);
		return false;
	}
	Bit::BitToNumeric(input, result);
	return (true);
}

//===--------------------------------------------------------------------===//
// Cast From UUID
//===--------------------------------------------------------------------===//
template <>
string_t CastFromUUID::Operation(hugeint_t input, StringHeap &heap) {
	string_t result = heap.EmptyString(36);
	UUID::ToString(input, result.GetDataWriteable());
	result.Finalize();
	return result;
}

//===--------------------------------------------------------------------===//
// Cast From UUID To Blob
//===--------------------------------------------------------------------===//
template <>
string_t CastFromUUIDToBlob::Operation(hugeint_t input, StringHeap &heap) {
	// UUID is 16 bytes (128 bits)
	string_t result = heap.EmptyString(16);
	auto data = result.GetDataWriteable();

	// Use the utility function from BaseUUID
	BaseUUID::ToBlob(input, data_ptr_cast(data));

	result.Finalize();
	return result;
}

//===--------------------------------------------------------------------===//
// Cast To UUID
//===--------------------------------------------------------------------===//
template <>
bool TryCastToUUID::Operation(string_t input, hugeint_t &result, Vector &result_vector, CastParameters &parameters) {
	return UUID::FromString(input, result, parameters.strict);
}

//===--------------------------------------------------------------------===//
// Cast Blob To UUID
//===--------------------------------------------------------------------===//
template <>
bool TryCastBlobToUUID::Operation(string_t input, hugeint_t &result, Vector &result_vector,
                                  CastParameters &parameters) {
	// BLOB must be exactly 16 bytes for UUID
	if (input.GetSize() != 16) {
		HandleCastError::AssignError("BLOB must be exactly 16 bytes to convert to UUID", parameters);
		return false;
	}

	auto data = const_data_ptr_cast(input.GetData());

	// Use the utility function from BaseUUID
	result = BaseUUID::FromBlob(data);

	return true;
}

template <>
bool TryCastBlobToUUID::Operation(string_t input, hugeint_t &result, bool strict) {
	// BLOB must be exactly 16 bytes for UUID
	if (input.GetSize() != 16) {
		return false;
	}

	auto data = const_data_ptr_cast(input.GetData());

	// Use the utility function from BaseUUID
	result = BaseUUID::FromBlob(data);

	return true;
}

//===--------------------------------------------------------------------===//
// Cast From UUID To UHUGEINT
//===--------------------------------------------------------------------===//
template <>
uhugeint_t CastFromUUIDToUHugeint::Operation(hugeint_t input) {
	return BaseUUID::ToUHugeint(input);
}

//===--------------------------------------------------------------------===//
// Cast From UHUGEINT To UUID
//===--------------------------------------------------------------------===//
template <>
hugeint_t CastFromUHugeintToUUID::Operation(uhugeint_t input) {
	return BaseUUID::FromUHugeint(input);
}

//===--------------------------------------------------------------------===//
// Cast To Geometry
//===--------------------------------------------------------------------===//
template <>
bool TryCastToGeometry::Operation(string_t input, string_t &result, Vector &result_vector, CastParameters &parameters) {
	auto &heap = StringVector::GetStringHeap(result_vector);
	if (!parameters.error_message) {
		// Pass the query location of the cast source if available.
		return Geometry::FromString(input, result, heap, parameters.strict,
		                            parameters.cast_source ? parameters.cast_source->GetQueryLocation()
		                                                   : QueryLocation());
	}
	// the caller collects errors (e.g. TRY_CAST) - report malformed WKT as a cast error
	string error_message;
	if (Geometry::TryFromString(input, result, heap, error_message)) {
		return true;
	}
	HandleCastError::AssignError(error_message, parameters);
	return false;
}

//===--------------------------------------------------------------------===//
// Cast To Date
//===--------------------------------------------------------------------===//
static DateCastResult TryConvertDateCast(string_t input, date_t &result, bool strict) {
	idx_t pos = 0;
	bool special = false;
	auto date_result = Date::TryConvertDate(input.GetData(), input.GetSize(), pos, result, special, strict);
	auto timestamp_suffix_pos = pos;
	if (date_result == DateCastResult::SUCCESS) {
		while (pos < input.GetSize() && StringUtil::CharacterIsSpace(input.GetData()[pos])) {
			pos++;
		}
		if (pos == input.GetSize()) {
			return date_result;
		}
	}
	if (strict || date_result != DateCastResult::SUCCESS) {
		return date_result;
	}

	timestamp_t timestamp;
	auto timestamp_result =
	    Timestamp::TryConvertTimestamp(input.GetData(), input.GetSize(), timestamp, /*use_offset=*/false, nullptr,
	                                   /*strict=*/false);
	if (timestamp_result == TimestampCastResult::SUCCESS) {
		return DateCastResult::SUCCESS;
	}
	if (timestamp_result == TimestampCastResult::ERROR_RANGE) {
		string timestamp_input = "2000-01-01"; // placeholder
		timestamp_input.append(input.GetData() + timestamp_suffix_pos, input.GetSize() - timestamp_suffix_pos);
		timestamp_result = Timestamp::TryConvertTimestamp(timestamp_input.c_str(), timestamp_input.size(), timestamp,
		                                                  /*use_offset=*/false, /*nanos=*/nullptr, /*strict=*/false);
		if (timestamp_result == TimestampCastResult::SUCCESS) {
			return DateCastResult::SUCCESS;
		}
	}
	return timestamp_result == TimestampCastResult::ERROR_RANGE ? DateCastResult::ERROR_RANGE
	                                                            : DateCastResult::ERROR_INCORRECT_FORMAT;
}

template <>
bool TryCastErrorMessage::Operation(string_t input, date_t &result, CastParameters &parameters) {
	switch (TryConvertDateCast(input, result, parameters.strict)) {
	case DateCastResult::SUCCESS:
		break;
	case DateCastResult::ERROR_INCORRECT_FORMAT:
		HandleCastError::AssignError(Date::FormatError(input), parameters);
		return false;
	case DateCastResult::ERROR_RANGE:
		HandleCastError::AssignError(Date::RangeError(input), parameters);
		return false;
	}
	return true;
}

template <>
bool TryCast::Operation(string_t input, date_t &result, bool strict) {
	return TryConvertDateCast(input, result, strict) == DateCastResult::SUCCESS;
}

template <>
date_t Cast::Operation(string_t input) {
	date_t result;
	switch (TryConvertDateCast(input, result, false)) {
	case DateCastResult::SUCCESS:
		return result;
	case DateCastResult::ERROR_INCORRECT_FORMAT:
		throw ConversionException(Date::FormatError(input));
	case DateCastResult::ERROR_RANGE:
		throw ConversionException(Date::RangeError(input));
	}
	throw InternalException("Unknown DateCastResult");
}

//===--------------------------------------------------------------------===//
// Cast To Time
//===--------------------------------------------------------------------===//
template <>
bool TryCastErrorMessage::Operation(string_t input, dtime_t &result, CastParameters &parameters) {
	if (!TryCast::Operation<string_t, dtime_t>(input, result, parameters.strict)) {
		HandleCastError::AssignError(Time::ConversionError(input), parameters);
		return false;
	}
	return true;
}

template <>
bool TryCast::Operation(string_t input, dtime_t &result, bool strict) {
	idx_t pos;
	return Time::TryConvertTime(input.GetData(), input.GetSize(), pos, result, strict);
}

template <>
dtime_t Cast::Operation(string_t input) {
	return Time::FromCString(input.GetData(), input.GetSize());
}

//===--------------------------------------------------------------------===//
// Cast To Time (ns)
//===--------------------------------------------------------------------===//
template <>
bool TryCastErrorMessage::Operation(string_t input, dtime_ns_t &result, CastParameters &parameters) {
	if (!TryCast::Operation<string_t, dtime_ns_t>(input, result, parameters.strict)) {
		HandleCastError::AssignError(Time::ConversionError(input), parameters);
		return false;
	}
	return true;
}

template <>
bool TryCast::Operation(string_t input, dtime_ns_t &result, bool strict) {
	idx_t pos;
	dtime_t micros;
	int32_t nanos = 0;
	if (!Time::TryConvertTime(input.GetData(), input.GetSize(), pos, micros, strict, &nanos)) {
		return false;
	}
	if (!TryCast::Operation(micros, result)) {
		return false;
	}
	if (!TryAddOperator::Operation<int64_t, int64_t, int64_t>(result.value, nanos, result.value)) {
		return false;
	}
	// the nanoseconds can push e.g. 24:00:00 past the end of the day
	return result.value <= Interval::NANOS_PER_DAY;
}

template <>
dtime_ns_t Cast::Operation(string_t input) {
	dtime_ns_t result;
	if (!TryCast::Operation(input, result, false)) {
		throw ConversionException(Time::ConversionError(input));
	}
	return result;
}

//===--------------------------------------------------------------------===//
// Cast To TimeTZ
//===--------------------------------------------------------------------===//
template <>
bool TryCastErrorMessage::Operation(string_t input, dtime_tz_t &result, CastParameters &parameters) {
	if (!TryCast::Operation<string_t, dtime_tz_t>(input, result, parameters.strict)) {
		HandleCastError::AssignError(Time::ConversionError(input), parameters);
		return false;
	}
	return true;
}

template <>
bool TryCast::Operation(string_t input, dtime_tz_t &result, bool strict) {
	idx_t pos;
	bool has_offset;
	return Time::TryConvertTimeTZ(input.GetData(), input.GetSize(), pos, result, has_offset, strict);
}

template <>
dtime_tz_t Cast::Operation(string_t input) {
	dtime_tz_t result;
	if (!TryCast::Operation(input, result, false)) {
		throw ConversionException(Time::ConversionError(input));
	}
	return result;
}

//===--------------------------------------------------------------------===//
// Cast To Timestamp
//===--------------------------------------------------------------------===//
template <>
bool TryCastErrorMessage::Operation(string_t input, timestamp_t &result, CastParameters &parameters) {
	switch (Timestamp::TryConvertTimestamp(input.GetData(), input.GetSize(), result, false)) {
	case TimestampCastResult::SUCCESS:
	case TimestampCastResult::STRICT_UTC:
		return true;
	case TimestampCastResult::ERROR_INCORRECT_FORMAT:
		HandleCastError::AssignError(Timestamp::FormatError(input), parameters);
		break;
	case TimestampCastResult::ERROR_NON_UTC_TIMEZONE:
		HandleCastError::AssignError(Timestamp::UnsupportedTimezoneError(input), parameters);
		break;
	case TimestampCastResult::ERROR_RANGE:
		HandleCastError::AssignError(Timestamp::RangeError(input), parameters);
		break;
	}
	return false;
}

template <>
bool TryCastErrorMessage::Operation(string_t input, timestamp_tz_t &result, CastParameters &parameters) {
	timestamp_t us;
	switch (Timestamp::TryConvertTimestamp(input.GetData(), input.GetSize(), us, true)) {
	case TimestampCastResult::SUCCESS:
	case TimestampCastResult::STRICT_UTC:
		return TryCastTimestampBase<timestamp_t, timestamp_tz_t>(us, result, true);
		;
	case TimestampCastResult::ERROR_INCORRECT_FORMAT:
		HandleCastError::AssignError(Timestamp::FormatError(input), parameters);
		break;
	case TimestampCastResult::ERROR_NON_UTC_TIMEZONE:
		HandleCastError::AssignError(Timestamp::UnsupportedTimezoneError(input), parameters);
		break;
	case TimestampCastResult::ERROR_RANGE:
		HandleCastError::AssignError(Timestamp::RangeError(input), parameters);
		break;
	}
	return false;
}

template <>
bool TryCastErrorMessage::Operation(string_t input, timestamp_tz_ns_t &result, CastParameters &parameters) {
	timestamp_ns_t ns;
	switch (Timestamp::TryConvertTimestamp(input.GetData(), input.GetSize(), ns, true)) {
	case TimestampCastResult::SUCCESS:
	case TimestampCastResult::STRICT_UTC:
		return TryCastTimestampBase<timestamp_ns_t, timestamp_tz_ns_t>(ns, result, true);
	case TimestampCastResult::ERROR_INCORRECT_FORMAT:
		HandleCastError::AssignError(Timestamp::FormatError(input), parameters);
		break;
	case TimestampCastResult::ERROR_NON_UTC_TIMEZONE:
		HandleCastError::AssignError(Timestamp::UnsupportedTimezoneError(input), parameters);
		break;
	case TimestampCastResult::ERROR_RANGE:
		HandleCastError::AssignError(Timestamp::RangeError(input), parameters);
		break;
	}
	return false;
}

template <>
bool TryCast::Operation(string_t input, timestamp_t &result, bool strict) {
	return Timestamp::TryConvertTimestamp(input.GetData(), input.GetSize(), result, false) ==
	       TimestampCastResult::SUCCESS;
}

template <>
bool TryCast::Operation(string_t input, timestamp_ns_t &result, bool strict) {
	return Timestamp::TryConvertTimestamp(input.GetData(), input.GetSize(), result, false) ==
	       TimestampCastResult::SUCCESS;
}

template <>
bool TryCast::Operation(string_t input, timestamp_tz_t &result, bool strict) {
	timestamp_t us;
	return Timestamp::TryConvertTimestamp(input.GetData(), input.GetSize(), us, true) == TimestampCastResult::SUCCESS &&
	       TryCastTimestampBase<timestamp_t, timestamp_tz_t>(us, result, strict);
}

template <>
bool TryCast::Operation(string_t input, timestamp_tz_ns_t &result, bool strict) {
	timestamp_ns_t ns;
	return Timestamp::TryConvertTimestamp(input.GetData(), input.GetSize(), ns, true) == TimestampCastResult::SUCCESS &&
	       TryCastTimestampBase<timestamp_ns_t, timestamp_tz_ns_t>(ns, result, strict);
}

template <>
timestamp_t Cast::Operation(string_t input) {
	return Timestamp::FromCString(input.GetData(), input.GetSize(), false);
}

template <>
timestamp_tz_t Cast::Operation(string_t input) {
	const auto us = Timestamp::FromCString(input.GetData(), input.GetSize(), true);
	return CastTimestampPrecisionOperation<timestamp_t, timestamp_tz_t>(us);
}

template <>
timestamp_tz_ns_t Cast::Operation(string_t input) {
	int32_t nanos;
	const auto us = Timestamp::FromCString(input.GetData(), input.GetSize(), false, &nanos);
	timestamp_ns_t ns;
	if (!Timestamp::TryFromTimestampNanos(us, nanos, ns)) {
		throw ConversionException(Timestamp::RangeError(input));
	}
	return CastTimestampPrecisionOperation<timestamp_ns_t, timestamp_tz_ns_t>(ns);
}

template <>
timestamp_ns_t Cast::Operation(string_t input) {
	int32_t nanos;
	const auto ts = Timestamp::FromCString(input.GetData(), input.GetSize(), false, &nanos);
	timestamp_ns_t result;
	if (!Timestamp::TryFromTimestampNanos(ts, nanos, result)) {
		throw ConversionException(Timestamp::RangeError(input));
	}
	return result;
}

//===--------------------------------------------------------------------===//
// Cast From Interval
//===--------------------------------------------------------------------===//
template <>
bool TryCastErrorMessage::Operation(string_t input, interval_t &result, CastParameters &parameters) {
	return Interval::FromCString(input.GetData(), input.GetSize(), result, parameters.error_message, parameters.strict);
}

//===--------------------------------------------------------------------===//
// Cast to hugeint / uhugeint
//===--------------------------------------------------------------------===//
// parsing hugeint from string is done a bit differently for performance reasons
// for other integer types we keep track of a single value
// and multiply that value by 10 for every digit we read
// however, for hugeints, multiplication is very expensive (>20X as expensive as for int64)
// for that reason, we parse numbers first into an int64 value
// when that value is full, we perform a HUGEINT multiplication to flush it into the hugeint
// this takes the number of HUGEINT multiplications down from [0-38] to [0-2]

template <typename T, typename OP, typename INTERMEDIATE_T>
struct HugeIntCastData {
	using ResultType = T;
	using IntermediateType = INTERMEDIATE_T;
	using Operation = OP;
	ResultType result;
	IntermediateType intermediate;
	idx_t digits;

	ResultType decimal;
	idx_t decimal_total_digits;
	ResultType decimal_intermediate;
	idx_t decimal_intermediate_digits;

	bool Flush() {
		if (digits == 0 && intermediate == 0) {
			return true;
		}
		if (result.lower != 0 || result.upper != 0) {
			if (digits > 38) {
				return false;
			}
			if (!OP::TryMultiply(result, OP::POWERS_OF_TEN[digits], result)) {
				return false;
			}
		}
		if (!OP::TryAddInPlace(result, ResultType(intermediate))) {
			return false;
		}
		digits = 0;
		intermediate = 0;
		return true;
	}

	bool FlushDecimal() {
		if (decimal_intermediate_digits == 0 && decimal_intermediate == 0) {
			return true;
		}
		if (decimal.lower != 0 || decimal.upper != 0) {
			if (decimal_intermediate_digits > 38) {
				return false;
			}
			if (!OP::TryMultiply(decimal, OP::POWERS_OF_TEN[decimal_intermediate_digits], decimal)) {
				return false;
			}
		}
		if (!OP::TryAddInPlace(decimal, ResultType(decimal_intermediate))) {
			return false;
		}
		decimal_total_digits += decimal_intermediate_digits;
		decimal_intermediate_digits = 0;
		decimal_intermediate = 0;
		return true;
	}
};

struct HugeIntegerCastOperation {
	template <class T, bool NEGATIVE>
	static bool HandleDigit(T &state, uint8_t digit) {
		if (NEGATIVE) {
			if (DUCKDB_UNLIKELY(state.intermediate <
			                    (NumericLimits<typename T::IntermediateType>::Minimum() + digit) / 10)) {
				// intermediate is full: need to flush it
				if (!state.Flush()) {
					return false;
				}
			}
			state.intermediate = state.intermediate * 10 - digit;
		} else {
			if (DUCKDB_UNLIKELY(state.intermediate >
			                    (NumericLimits<typename T::IntermediateType>::Maximum() - digit) / 10)) {
				if (!state.Flush()) {
					return false;
				}
			}
			state.intermediate = state.intermediate * 10 + digit;
		}
		state.digits++;
		return true;
	}

	template <class T, bool NEGATIVE>
	static bool HandleHexDigit(T &state, uint8_t digit) {
		static_assert(!NEGATIVE, "HugeInt hex cast does not support negative values");
		const uint64_t upper = static_cast<uint64_t>(state.result.upper);
		const uint64_t lower = static_cast<uint64_t>(state.result.lower);
		if (DUCKDB_UNLIKELY(upper >> 60)) {
			return false;
		}
		state.result.upper = static_cast<decltype(state.result.upper)>((upper << 4) | (lower >> 60));
		state.result.lower = static_cast<decltype(state.result.lower)>((lower << 4) | digit);
		if constexpr (std::is_signed<decltype(state.result.upper)>::value) {
			if (DUCKDB_UNLIKELY(state.result.upper < 0)) {
				return false;
			}
		}
		return true;
	}

	template <class T, bool NEGATIVE>
	static bool HandleBinaryDigit(T &state, uint8_t digit) {
		static_assert(!NEGATIVE, "HugeInt binary cast does not support negative values");
		const uint64_t upper = static_cast<uint64_t>(state.result.upper);
		const uint64_t lower = static_cast<uint64_t>(state.result.lower);
		if (DUCKDB_UNLIKELY(upper >> 63)) {
			return false;
		}
		state.result.upper = static_cast<decltype(state.result.upper)>((upper << 1) | (lower >> 63));
		state.result.lower = static_cast<decltype(state.result.lower)>((lower << 1) | digit);
		if constexpr (std::is_signed<decltype(state.result.upper)>::value) {
			if (DUCKDB_UNLIKELY(state.result.upper < 0)) {
				return false;
			}
		}
		return true;
	}

	template <class T, bool NEGATIVE>
	static bool HandleExponent(T &state, int32_t exponent) {
		using result_t = typename T::ResultType;
		if (!state.Flush()) {
			return false;
		}

		int32_t e = exponent;
		if (e < -38) {
			state.result = 0;
			return true;
		}

		// Negative Exponent
		result_t remainder = 0;
		if (e < 0) {
			state.result = T::Operation::DivMod(state.result, T::Operation::POWERS_OF_TEN[-e], remainder);
			if (remainder < 0) {
				result_t negate_result;
				if (!T::Operation::TryNegate(remainder, negate_result)) {
					return false;
				}
				remainder = negate_result;
			}
			state.decimal = remainder;
			state.decimal_total_digits = static_cast<idx_t>(-e);
			state.decimal_intermediate = 0;
			state.decimal_intermediate_digits = 0;
			return Finalize<T, NEGATIVE>(state);
		}

		// Positive Exponent
		if (state.result != 0) {
			if (e > 38 || !TryMultiplyOperator::Operation(state.result, T::Operation::POWERS_OF_TEN[e], state.result)) {
				return false;
			}
		}
		if (!state.FlushDecimal()) {
			return false;
		}
		if (state.decimal == 0) {
			return Finalize<T, NEGATIVE>(state);
		}

		const auto shift = static_cast<int64_t>(exponent) - static_cast<int64_t>(state.decimal_total_digits);
		if (shift < 0) {
			if (shift < -38) {
				// all of decimal is fractional
				remainder = state.decimal;
				state.decimal = 0;
			} else {
				state.decimal = T::Operation::DivMod(state.decimal, T::Operation::POWERS_OF_TEN[-shift], remainder);
			}
			state.decimal_total_digits -= static_cast<idx_t>(exponent);
		} else {
			if (shift > 38 ||
			    !TryMultiplyOperator::Operation(state.decimal, T::Operation::POWERS_OF_TEN[shift], state.decimal)) {
				return false;
			}
		}

		if (NEGATIVE) {
			if (!TrySubtractOperator::Operation(state.result, state.decimal, state.result)) {
				return false;
			}
		} else if (!TryAddOperator::Operation(state.result, state.decimal, state.result)) {
			return false;
		}
		state.decimal = remainder;
		return Finalize<T, NEGATIVE>(state);
	}

	template <class T, bool NEGATIVE, bool ALLOW_EXPONENT>
	static bool HandleDecimal(T &state, uint8_t digit) {
		if (!state.Flush()) {
			return false;
		}
		// digits past this position cannot affect the result for any int16_t exponent. Ignore them!
		static constexpr uint16_t MAX_DECIMAL_DIGITS = -NumericLimits<int16_t>::Minimum();
		if (DUCKDB_UNLIKELY(state.decimal_total_digits + state.decimal_intermediate_digits == MAX_DECIMAL_DIGITS)) {
			return true;
		}
		if (DUCKDB_UNLIKELY(state.decimal_intermediate > (NumericLimits<int64_t>::Maximum() - digit) / 10)) {
			if (!state.FlushDecimal()) {
				return false;
			}
		}
		state.decimal_intermediate = state.decimal_intermediate * 10 + digit;
		state.decimal_intermediate_digits++;
		return true;
	}

	template <class T, bool NEGATIVE>
	static bool Finalize(T &state) {
		using result_t = typename T::ResultType;
		if (!state.Flush() || !state.FlushDecimal()) {
			return false;
		}

		if (state.decimal == 0 || state.decimal_total_digits == 0) {
			return true;
		}

		// Get the first (left-most) digit of the decimals
		constexpr auto MAX_DIGITS = T::Operation::CACHED_POWERS_OF_TEN - 1;
		while (state.decimal_total_digits > MAX_DIGITS) {
			state.decimal /= T::Operation::POWERS_OF_TEN[MAX_DIGITS];
			state.decimal_total_digits -= MAX_DIGITS;
		}
		D_ASSERT(state.decimal_total_digits >= 1 && state.decimal_total_digits - 1 <= MAX_DIGITS);
		state.decimal /= T::Operation::POWERS_OF_TEN[state.decimal_total_digits - 1];

		if (state.decimal >= 5) {
			if (NEGATIVE) {
				return TrySubtractOperator::Operation(state.result, result_t(1), state.result);
			} else {
				return TryAddOperator::Operation(state.result, result_t(1), state.result);
			}
		}
		return true;
	}
};

template <>
bool TryCast::Operation(string_t input, hugeint_t &result, bool strict) {
	HugeIntCastData<hugeint_t, Hugeint, int64_t> state {};
	if (!TryIntegerCast<HugeIntCastData<hugeint_t, Hugeint, int64_t>, true, true, HugeIntegerCastOperation>(
	        input.GetData(), input.GetSize(), state, strict)) {
		return false;
	}
	result = state.result;
	return true;
}

template <>
bool TryCast::Operation(string_t input, uhugeint_t &result, bool strict) {
	HugeIntCastData<uhugeint_t, Uhugeint, uint64_t> state {};
	if (!TryIntegerCast<HugeIntCastData<uhugeint_t, Uhugeint, uint64_t>, false, true, HugeIntegerCastOperation>(
	        input.GetData(), input.GetSize(), state, strict)) {
		return false;
	}
	result = state.result;
	return true;
}

//===--------------------------------------------------------------------===//
// Decimal String Cast
//===--------------------------------------------------------------------===//

template <>
bool TryCastToDecimal::Operation(string_t input, int16_t &result, CastParameters &parameters, uint8_t width,
                                 uint8_t scale) {
	return TryDecimalStringCast<int16_t>(input, result, parameters, width, scale);
}

template <>
bool TryCastToDecimal::Operation(string_t input, int32_t &result, CastParameters &parameters, uint8_t width,
                                 uint8_t scale) {
	return TryDecimalStringCast<int32_t>(input, result, parameters, width, scale);
}

template <>
bool TryCastToDecimal::Operation(string_t input, int64_t &result, CastParameters &parameters, uint8_t width,
                                 uint8_t scale) {
	return TryDecimalStringCast<int64_t>(input, result, parameters, width, scale);
}

template <>
bool TryCastToDecimal::Operation(string_t input, hugeint_t &result, CastParameters &parameters, uint8_t width,
                                 uint8_t scale) {
	return TryDecimalStringCast<hugeint_t>(input, result, parameters, width, scale);
}

template <>
bool TryCastToDecimalCommaSeparated::Operation(string_t input, int16_t &result, CastParameters &parameters,
                                               uint8_t width, uint8_t scale) {
	return TryDecimalStringCast<int16_t, ','>(input, result, parameters, width, scale);
}

template <>
bool TryCastToDecimalCommaSeparated::Operation(string_t input, int32_t &result, CastParameters &parameters,
                                               uint8_t width, uint8_t scale) {
	return TryDecimalStringCast<int32_t, ','>(input, result, parameters, width, scale);
}

template <>
bool TryCastToDecimalCommaSeparated::Operation(string_t input, int64_t &result, CastParameters &parameters,
                                               uint8_t width, uint8_t scale) {
	return TryDecimalStringCast<int64_t, ','>(input, result, parameters, width, scale);
}

template <>
bool TryCastToDecimalCommaSeparated::Operation(string_t input, hugeint_t &result, CastParameters &parameters,
                                               uint8_t width, uint8_t scale) {
	return TryDecimalStringCast<hugeint_t, ','>(input, result, parameters, width, scale);
}

template <>
string_t StringCastFromDecimal::Operation(int16_t input, uint8_t width, uint8_t scale, StringHeap &heap) {
	return DecimalToString::Format<int16_t>(input, width, scale, heap);
}

template <>
string_t StringCastFromDecimal::Operation(int32_t input, uint8_t width, uint8_t scale, StringHeap &heap) {
	return DecimalToString::Format<int32_t>(input, width, scale, heap);
}

template <>
string_t StringCastFromDecimal::Operation(int64_t input, uint8_t width, uint8_t scale, StringHeap &heap) {
	return DecimalToString::Format<int64_t>(input, width, scale, heap);
}

template <>
string_t StringCastFromDecimal::Operation(hugeint_t input, uint8_t width, uint8_t scale, StringHeap &heap) {
	return DecimalToString::Format<hugeint_t>(input, width, scale, heap);
}

//===--------------------------------------------------------------------===//
// Decimal Casts
//===--------------------------------------------------------------------===//
// Decimal <-> Bool
//===--------------------------------------------------------------------===//
template <class T, class OP = NumericHelper>
bool TryCastBoolToDecimal(bool input, T &result, CastParameters &parameters, uint8_t width, uint8_t scale) {
	// a decimal without integer digits cannot hold true
	if (input && width <= scale) {
		string error = StringUtil::Format("Could not cast value true to DECIMAL(%d,%d)", width, scale);
		HandleCastError::AssignError(error, parameters);
		return false;
	}
	result = UnsafeNumericCast<T>(input ? OP::POWERS_OF_TEN[scale] : 0);
	return true;
}

template <>
bool TryCastToDecimal::Operation(bool input, int16_t &result, CastParameters &parameters, uint8_t width,
                                 uint8_t scale) {
	return TryCastBoolToDecimal<int16_t>(input, result, parameters, width, scale);
}

template <>
bool TryCastToDecimal::Operation(bool input, int32_t &result, CastParameters &parameters, uint8_t width,
                                 uint8_t scale) {
	return TryCastBoolToDecimal<int32_t>(input, result, parameters, width, scale);
}

template <>
bool TryCastToDecimal::Operation(bool input, int64_t &result, CastParameters &parameters, uint8_t width,
                                 uint8_t scale) {
	return TryCastBoolToDecimal<int64_t>(input, result, parameters, width, scale);
}

template <>
bool TryCastToDecimal::Operation(bool input, hugeint_t &result, CastParameters &parameters, uint8_t width,
                                 uint8_t scale) {
	return TryCastBoolToDecimal<hugeint_t, Hugeint>(input, result, parameters, width, scale);
}

template <>
bool TryCastFromDecimal::Operation(int16_t input, bool &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale) {
	return TryCast::Operation<int16_t, bool>(input, result);
}

template <>
bool TryCastFromDecimal::Operation(int32_t input, bool &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale) {
	return TryCast::Operation<int32_t, bool>(input, result);
}

template <>
bool TryCastFromDecimal::Operation(int64_t input, bool &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale) {
	return TryCast::Operation<int64_t, bool>(input, result);
}

template <>
bool TryCastFromDecimal::Operation(hugeint_t input, bool &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale) {
	return TryCast::Operation<hugeint_t, bool>(input, result);
}

//===--------------------------------------------------------------------===//
// Numeric -> Decimal Cast
//===--------------------------------------------------------------------===//
struct SignedToDecimalOperator {
	template <class SRC, class DST>
	static bool Operation(SRC input, DST max_width) {
		return int64_t(input) >= int64_t(max_width) || int64_t(input) <= int64_t(-max_width);
	}
};

struct UnsignedToDecimalOperator {
	template <class SRC, class DST>
	static bool Operation(SRC input, DST max_width) {
		return uint64_t(input) >= uint64_t(max_width);
	}
};

template <class SRC, class DST, class OP = SignedToDecimalOperator>
bool StandardNumericToDecimalCast(SRC input, DST &result, CastParameters &parameters, uint8_t width, uint8_t scale) {
	// check for overflow
	DST max_width = UnsafeNumericCast<DST>(NumericHelper::POWERS_OF_TEN[width - scale]);
	if (OP::template Operation<SRC, DST>(input, max_width)) {
		string error = StringUtil::Format("Could not cast value %d to DECIMAL(%d,%d)", input, width, scale);
		HandleCastError::AssignError(error, parameters);
		return false;
	}
	result = UnsafeNumericCast<DST>(DST(input) * NumericHelper::POWERS_OF_TEN[scale]);
	return true;
}

template <class SRC>
bool NumericToHugeDecimalCast(SRC input, hugeint_t &result, CastParameters &parameters, uint8_t width, uint8_t scale) {
	// check for overflow
	hugeint_t max_width = Hugeint::POWERS_OF_TEN[width - scale];
	hugeint_t hinput = Hugeint::Convert(input);
	if (hinput >= max_width || hinput <= -max_width) {
		string error = StringUtil::Format("Could not cast value %s to DECIMAL(%d,%d)", hinput.ToString(), width, scale);
		HandleCastError::AssignError(error, parameters);
		return false;
	}
	result = hinput * Hugeint::POWERS_OF_TEN[scale];
	return true;
}

//===--------------------------------------------------------------------===//
// Cast int8_t -> Decimal
//===--------------------------------------------------------------------===//
template <>
bool TryCastToDecimal::Operation(int8_t input, int16_t &result, CastParameters &parameters, uint8_t width,
                                 uint8_t scale) {
	return StandardNumericToDecimalCast<int8_t, int16_t>(input, result, parameters, width, scale);
}
template <>
bool TryCastToDecimal::Operation(int8_t input, int32_t &result, CastParameters &parameters, uint8_t width,
                                 uint8_t scale) {
	return StandardNumericToDecimalCast<int8_t, int32_t>(input, result, parameters, width, scale);
}
template <>
bool TryCastToDecimal::Operation(int8_t input, int64_t &result, CastParameters &parameters, uint8_t width,
                                 uint8_t scale) {
	return StandardNumericToDecimalCast<int8_t, int64_t>(input, result, parameters, width, scale);
}
template <>
bool TryCastToDecimal::Operation(int8_t input, hugeint_t &result, CastParameters &parameters, uint8_t width,
                                 uint8_t scale) {
	return NumericToHugeDecimalCast<int8_t>(input, result, parameters, width, scale);
}

//===--------------------------------------------------------------------===//
// Cast int16_t -> Decimal
//===--------------------------------------------------------------------===//
template <>
bool TryCastToDecimal::Operation(int16_t input, int16_t &result, CastParameters &parameters, uint8_t width,
                                 uint8_t scale) {
	return StandardNumericToDecimalCast<int16_t, int16_t>(input, result, parameters, width, scale);
}
template <>
bool TryCastToDecimal::Operation(int16_t input, int32_t &result, CastParameters &parameters, uint8_t width,
                                 uint8_t scale) {
	return StandardNumericToDecimalCast<int16_t, int32_t>(input, result, parameters, width, scale);
}
template <>
bool TryCastToDecimal::Operation(int16_t input, int64_t &result, CastParameters &parameters, uint8_t width,
                                 uint8_t scale) {
	return StandardNumericToDecimalCast<int16_t, int64_t>(input, result, parameters, width, scale);
}
template <>
bool TryCastToDecimal::Operation(int16_t input, hugeint_t &result, CastParameters &parameters, uint8_t width,
                                 uint8_t scale) {
	return NumericToHugeDecimalCast<int16_t>(input, result, parameters, width, scale);
}

//===--------------------------------------------------------------------===//
// Cast int32_t -> Decimal
//===--------------------------------------------------------------------===//
template <>
bool TryCastToDecimal::Operation(int32_t input, int16_t &result, CastParameters &parameters, uint8_t width,
                                 uint8_t scale) {
	return StandardNumericToDecimalCast<int32_t, int16_t>(input, result, parameters, width, scale);
}
template <>
bool TryCastToDecimal::Operation(int32_t input, int32_t &result, CastParameters &parameters, uint8_t width,
                                 uint8_t scale) {
	return StandardNumericToDecimalCast<int32_t, int32_t>(input, result, parameters, width, scale);
}
template <>
bool TryCastToDecimal::Operation(int32_t input, int64_t &result, CastParameters &parameters, uint8_t width,
                                 uint8_t scale) {
	return StandardNumericToDecimalCast<int32_t, int64_t>(input, result, parameters, width, scale);
}
template <>
bool TryCastToDecimal::Operation(int32_t input, hugeint_t &result, CastParameters &parameters, uint8_t width,
                                 uint8_t scale) {
	return NumericToHugeDecimalCast<int32_t>(input, result, parameters, width, scale);
}

//===--------------------------------------------------------------------===//
// Cast int64_t -> Decimal
//===--------------------------------------------------------------------===//
template <>
bool TryCastToDecimal::Operation(int64_t input, int16_t &result, CastParameters &parameters, uint8_t width,
                                 uint8_t scale) {
	return StandardNumericToDecimalCast<int64_t, int16_t>(input, result, parameters, width, scale);
}
template <>
bool TryCastToDecimal::Operation(int64_t input, int32_t &result, CastParameters &parameters, uint8_t width,
                                 uint8_t scale) {
	return StandardNumericToDecimalCast<int64_t, int32_t>(input, result, parameters, width, scale);
}
template <>
bool TryCastToDecimal::Operation(int64_t input, int64_t &result, CastParameters &parameters, uint8_t width,
                                 uint8_t scale) {
	return StandardNumericToDecimalCast<int64_t, int64_t>(input, result, parameters, width, scale);
}
template <>
bool TryCastToDecimal::Operation(int64_t input, hugeint_t &result, CastParameters &parameters, uint8_t width,
                                 uint8_t scale) {
	return NumericToHugeDecimalCast<int64_t>(input, result, parameters, width, scale);
}

//===--------------------------------------------------------------------===//
// Cast uint8_t -> Decimal
//===--------------------------------------------------------------------===//
template <>
bool TryCastToDecimal::Operation(uint8_t input, int16_t &result, CastParameters &parameters, uint8_t width,
                                 uint8_t scale) {
	return StandardNumericToDecimalCast<uint8_t, int16_t, UnsignedToDecimalOperator>(input, result, parameters, width,
	                                                                                 scale);
}
template <>
bool TryCastToDecimal::Operation(uint8_t input, int32_t &result, CastParameters &parameters, uint8_t width,
                                 uint8_t scale) {
	return StandardNumericToDecimalCast<uint8_t, int32_t, UnsignedToDecimalOperator>(input, result, parameters, width,
	                                                                                 scale);
}
template <>
bool TryCastToDecimal::Operation(uint8_t input, int64_t &result, CastParameters &parameters, uint8_t width,
                                 uint8_t scale) {
	return StandardNumericToDecimalCast<uint8_t, int64_t, UnsignedToDecimalOperator>(input, result, parameters, width,
	                                                                                 scale);
}
template <>
bool TryCastToDecimal::Operation(uint8_t input, hugeint_t &result, CastParameters &parameters, uint8_t width,
                                 uint8_t scale) {
	return NumericToHugeDecimalCast<uint8_t>(input, result, parameters, width, scale);
}

//===--------------------------------------------------------------------===//
// Cast uint16_t -> Decimal
//===--------------------------------------------------------------------===//
template <>
bool TryCastToDecimal::Operation(uint16_t input, int16_t &result, CastParameters &parameters, uint8_t width,
                                 uint8_t scale) {
	return StandardNumericToDecimalCast<uint16_t, int16_t, UnsignedToDecimalOperator>(input, result, parameters, width,
	                                                                                  scale);
}
template <>
bool TryCastToDecimal::Operation(uint16_t input, int32_t &result, CastParameters &parameters, uint8_t width,
                                 uint8_t scale) {
	return StandardNumericToDecimalCast<uint16_t, int32_t, UnsignedToDecimalOperator>(input, result, parameters, width,
	                                                                                  scale);
}
template <>
bool TryCastToDecimal::Operation(uint16_t input, int64_t &result, CastParameters &parameters, uint8_t width,
                                 uint8_t scale) {
	return StandardNumericToDecimalCast<uint16_t, int64_t, UnsignedToDecimalOperator>(input, result, parameters, width,
	                                                                                  scale);
}
template <>
bool TryCastToDecimal::Operation(uint16_t input, hugeint_t &result, CastParameters &parameters, uint8_t width,
                                 uint8_t scale) {
	return NumericToHugeDecimalCast<uint16_t>(input, result, parameters, width, scale);
}

//===--------------------------------------------------------------------===//
// Cast uint32_t -> Decimal
//===--------------------------------------------------------------------===//
template <>
bool TryCastToDecimal::Operation(uint32_t input, int16_t &result, CastParameters &parameters, uint8_t width,
                                 uint8_t scale) {
	return StandardNumericToDecimalCast<uint32_t, int16_t, UnsignedToDecimalOperator>(input, result, parameters, width,
	                                                                                  scale);
}
template <>
bool TryCastToDecimal::Operation(uint32_t input, int32_t &result, CastParameters &parameters, uint8_t width,
                                 uint8_t scale) {
	return StandardNumericToDecimalCast<uint32_t, int32_t, UnsignedToDecimalOperator>(input, result, parameters, width,
	                                                                                  scale);
}
template <>
bool TryCastToDecimal::Operation(uint32_t input, int64_t &result, CastParameters &parameters, uint8_t width,
                                 uint8_t scale) {
	return StandardNumericToDecimalCast<uint32_t, int64_t, UnsignedToDecimalOperator>(input, result, parameters, width,
	                                                                                  scale);
}
template <>
bool TryCastToDecimal::Operation(uint32_t input, hugeint_t &result, CastParameters &parameters, uint8_t width,
                                 uint8_t scale) {
	return NumericToHugeDecimalCast<uint32_t>(input, result, parameters, width, scale);
}

//===--------------------------------------------------------------------===//
// Cast uint64_t -> Decimal
//===--------------------------------------------------------------------===//
template <>
bool TryCastToDecimal::Operation(uint64_t input, int16_t &result, CastParameters &parameters, uint8_t width,
                                 uint8_t scale) {
	return StandardNumericToDecimalCast<uint64_t, int16_t, UnsignedToDecimalOperator>(input, result, parameters, width,
	                                                                                  scale);
}
template <>
bool TryCastToDecimal::Operation(uint64_t input, int32_t &result, CastParameters &parameters, uint8_t width,
                                 uint8_t scale) {
	return StandardNumericToDecimalCast<uint64_t, int32_t, UnsignedToDecimalOperator>(input, result, parameters, width,
	                                                                                  scale);
}
template <>
bool TryCastToDecimal::Operation(uint64_t input, int64_t &result, CastParameters &parameters, uint8_t width,
                                 uint8_t scale) {
	return StandardNumericToDecimalCast<uint64_t, int64_t, UnsignedToDecimalOperator>(input, result, parameters, width,
	                                                                                  scale);
}
template <>
bool TryCastToDecimal::Operation(uint64_t input, hugeint_t &result, CastParameters &parameters, uint8_t width,
                                 uint8_t scale) {
	return NumericToHugeDecimalCast<uint64_t>(input, result, parameters, width, scale);
}

//===--------------------------------------------------------------------===//
// Hugeint -> Decimal Cast
//===--------------------------------------------------------------------===//
template <class DST>
bool HugeintToDecimalCast(hugeint_t input, DST &result, CastParameters &parameters, uint8_t width, uint8_t scale) {
	// check for overflow
	hugeint_t max_width = Hugeint::POWERS_OF_TEN[width - scale];
	if (input >= max_width || input <= -max_width) {
		string error = StringUtil::Format("Could not cast value %s to DECIMAL(%d,%d)", input.ToString(), width, scale);
		HandleCastError::AssignError(error, parameters);
		return false;
	}
	result = Hugeint::Cast<DST>(input * Hugeint::POWERS_OF_TEN[scale]);
	return true;
}

template <>
bool TryCastToDecimal::Operation(hugeint_t input, int16_t &result, CastParameters &parameters, uint8_t width,
                                 uint8_t scale) {
	return HugeintToDecimalCast<int16_t>(input, result, parameters, width, scale);
}

template <>
bool TryCastToDecimal::Operation(hugeint_t input, int32_t &result, CastParameters &parameters, uint8_t width,
                                 uint8_t scale) {
	return HugeintToDecimalCast<int32_t>(input, result, parameters, width, scale);
}

template <>
bool TryCastToDecimal::Operation(hugeint_t input, int64_t &result, CastParameters &parameters, uint8_t width,
                                 uint8_t scale) {
	return HugeintToDecimalCast<int64_t>(input, result, parameters, width, scale);
}

template <>
bool TryCastToDecimal::Operation(hugeint_t input, hugeint_t &result, CastParameters &parameters, uint8_t width,
                                 uint8_t scale) {
	return HugeintToDecimalCast<hugeint_t>(input, result, parameters, width, scale);
}

//===--------------------------------------------------------------------===//
// Uhugeint -> Decimal Cast
//===--------------------------------------------------------------------===//
template <class DST>
bool UhugeintToDecimalCast(uhugeint_t input, DST &result, CastParameters &parameters, uint8_t width, uint8_t scale) {
	// check for overflow
	uhugeint_t max_width = Uhugeint::POWERS_OF_TEN[width - scale];
	if (input >= max_width) {
		string error = StringUtil::Format("Could not cast value %s to DECIMAL(%d,%d)", input.ToString(), width, scale);
		HandleCastError::AssignError(error, parameters);
		return false;
	}
	result = Uhugeint::Cast<DST>(input * Uhugeint::POWERS_OF_TEN[scale]);
	return true;
}

template <>
bool TryCastToDecimal::Operation(uhugeint_t input, int16_t &result, CastParameters &parameters, uint8_t width,
                                 uint8_t scale) {
	return UhugeintToDecimalCast<int16_t>(input, result, parameters, width, scale);
}

template <>
bool TryCastToDecimal::Operation(uhugeint_t input, int32_t &result, CastParameters &parameters, uint8_t width,
                                 uint8_t scale) {
	return UhugeintToDecimalCast<int32_t>(input, result, parameters, width, scale);
}

template <>
bool TryCastToDecimal::Operation(uhugeint_t input, int64_t &result, CastParameters &parameters, uint8_t width,
                                 uint8_t scale) {
	return UhugeintToDecimalCast<int64_t>(input, result, parameters, width, scale);
}

template <>
bool TryCastToDecimal::Operation(uhugeint_t input, hugeint_t &result, CastParameters &parameters, uint8_t width,
                                 uint8_t scale) {
	return UhugeintToDecimalCast<hugeint_t>(input, result, parameters, width, scale);
}

//===--------------------------------------------------------------------===//
// Float/Double -> Decimal Cast
//===--------------------------------------------------------------------===//
template <class SRC, class DST>
bool DoubleToDecimalCast(SRC input, DST &result, CastParameters &parameters, uint8_t width, uint8_t scale) {
	double value = input * NumericHelper::DOUBLE_POWERS_OF_TEN[scale];
	double roundedValue = round(value);
	if (roundedValue <= -NumericHelper::DOUBLE_POWERS_OF_TEN[width] ||
	    roundedValue >= NumericHelper::DOUBLE_POWERS_OF_TEN[width] || !Value::IsFinite(roundedValue)) {
		string error = StringUtil::Format("Could not cast value %f to DECIMAL(%d,%d)", input, width, scale);
		HandleCastError::AssignError(error, parameters);
		return false;
	}
	// For some reason PG does not use statistical rounding here (even though it _does_ for integers...)
	result = Cast::Operation<double, DST>(roundedValue);
	return true;
}

template <>
bool TryCastToDecimal::Operation(float input, int16_t &result, CastParameters &parameters, uint8_t width,
                                 uint8_t scale) {
	return DoubleToDecimalCast<float, int16_t>(input, result, parameters, width, scale);
}

template <>
bool TryCastToDecimal::Operation(float input, int32_t &result, CastParameters &parameters, uint8_t width,
                                 uint8_t scale) {
	return DoubleToDecimalCast<float, int32_t>(input, result, parameters, width, scale);
}

template <>
bool TryCastToDecimal::Operation(float input, int64_t &result, CastParameters &parameters, uint8_t width,
                                 uint8_t scale) {
	return DoubleToDecimalCast<float, int64_t>(input, result, parameters, width, scale);
}

template <>
bool TryCastToDecimal::Operation(float input, hugeint_t &result, CastParameters &parameters, uint8_t width,
                                 uint8_t scale) {
	return DoubleToDecimalCast<float, hugeint_t>(input, result, parameters, width, scale);
}

template <>
bool TryCastToDecimal::Operation(double input, int16_t &result, CastParameters &parameters, uint8_t width,
                                 uint8_t scale) {
	return DoubleToDecimalCast<double, int16_t>(input, result, parameters, width, scale);
}

template <>
bool TryCastToDecimal::Operation(double input, int32_t &result, CastParameters &parameters, uint8_t width,
                                 uint8_t scale) {
	return DoubleToDecimalCast<double, int32_t>(input, result, parameters, width, scale);
}

template <>
bool TryCastToDecimal::Operation(double input, int64_t &result, CastParameters &parameters, uint8_t width,
                                 uint8_t scale) {
	return DoubleToDecimalCast<double, int64_t>(input, result, parameters, width, scale);
}

template <>
bool TryCastToDecimal::Operation(double input, hugeint_t &result, CastParameters &parameters, uint8_t width,
                                 uint8_t scale) {
	return DoubleToDecimalCast<double, hugeint_t>(input, result, parameters, width, scale);
}

//===--------------------------------------------------------------------===//
// Decimal -> Numeric Cast
//===--------------------------------------------------------------------===//
template <class SRC, class DST>
bool TryCastDecimalToNumeric(SRC input, DST &result, CastParameters &parameters, uint8_t scale) {
	// Round away from 0.
	const auto power = NumericHelper::POWERS_OF_TEN[scale];
	// https://graphics.stanford.edu/~seander/bithacks.html#ConditionalNegate
	const auto fNegate = int64_t(input < 0);
	const auto rounding = ((power ^ -fNegate) + fNegate) / 2;
	const auto scaled_value = (input + rounding) / power;
	if (!TryCast::Operation<SRC, DST>(UnsafeNumericCast<SRC>(scaled_value), result)) {
		string error = StringUtil::Format("Failed to cast decimal value %d to type %s", scaled_value, GetTypeId<DST>());
		HandleCastError::AssignError(error, parameters);
		return false;
	}
	return true;
}

template <class DST>
bool TryCastHugeDecimalToNumeric(hugeint_t input, DST &result, CastParameters &parameters, uint8_t scale) {
	const auto power = Hugeint::POWERS_OF_TEN[scale];
	const auto rounding = ((input < 0) ? -power : power) / 2;
	auto scaled_value = (input + rounding) / power;
	if (!TryCast::Operation<hugeint_t, DST>(scaled_value, result)) {
		string error = StringUtil::Format("Failed to cast decimal value %s to type %s",
		                                  ConvertToString::Operation(scaled_value), GetTypeId<DST>());
		HandleCastError::AssignError(error, parameters);
		return false;
	}
	return true;
}

//===--------------------------------------------------------------------===//
// Cast Decimal -> int8_t
//===--------------------------------------------------------------------===//
template <>
bool TryCastFromDecimal::Operation(int16_t input, int8_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale) {
	return TryCastDecimalToNumeric<int16_t, int8_t>(input, result, parameters, scale);
}
template <>
bool TryCastFromDecimal::Operation(int32_t input, int8_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale) {
	return TryCastDecimalToNumeric<int32_t, int8_t>(input, result, parameters, scale);
}
template <>
bool TryCastFromDecimal::Operation(int64_t input, int8_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale) {
	return TryCastDecimalToNumeric<int64_t, int8_t>(input, result, parameters, scale);
}
template <>
bool TryCastFromDecimal::Operation(hugeint_t input, int8_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale) {
	return TryCastHugeDecimalToNumeric<int8_t>(input, result, parameters, scale);
}

//===--------------------------------------------------------------------===//
// Cast Decimal -> int16_t
//===--------------------------------------------------------------------===//
template <>
bool TryCastFromDecimal::Operation(int16_t input, int16_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale) {
	return TryCastDecimalToNumeric<int16_t, int16_t>(input, result, parameters, scale);
}
template <>
bool TryCastFromDecimal::Operation(int32_t input, int16_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale) {
	return TryCastDecimalToNumeric<int32_t, int16_t>(input, result, parameters, scale);
}
template <>
bool TryCastFromDecimal::Operation(int64_t input, int16_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale) {
	return TryCastDecimalToNumeric<int64_t, int16_t>(input, result, parameters, scale);
}
template <>
bool TryCastFromDecimal::Operation(hugeint_t input, int16_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale) {
	return TryCastHugeDecimalToNumeric<int16_t>(input, result, parameters, scale);
}

//===--------------------------------------------------------------------===//
// Cast Decimal -> int32_t
//===--------------------------------------------------------------------===//
template <>
bool TryCastFromDecimal::Operation(int16_t input, int32_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale) {
	return TryCastDecimalToNumeric<int16_t, int32_t>(input, result, parameters, scale);
}
template <>
bool TryCastFromDecimal::Operation(int32_t input, int32_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale) {
	return TryCastDecimalToNumeric<int32_t, int32_t>(input, result, parameters, scale);
}
template <>
bool TryCastFromDecimal::Operation(int64_t input, int32_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale) {
	return TryCastDecimalToNumeric<int64_t, int32_t>(input, result, parameters, scale);
}
template <>
bool TryCastFromDecimal::Operation(hugeint_t input, int32_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale) {
	return TryCastHugeDecimalToNumeric<int32_t>(input, result, parameters, scale);
}

//===--------------------------------------------------------------------===//
// Cast Decimal -> int64_t
//===--------------------------------------------------------------------===//
template <>
bool TryCastFromDecimal::Operation(int16_t input, int64_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale) {
	return TryCastDecimalToNumeric<int16_t, int64_t>(input, result, parameters, scale);
}
template <>
bool TryCastFromDecimal::Operation(int32_t input, int64_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale) {
	return TryCastDecimalToNumeric<int32_t, int64_t>(input, result, parameters, scale);
}
template <>
bool TryCastFromDecimal::Operation(int64_t input, int64_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale) {
	return TryCastDecimalToNumeric<int64_t, int64_t>(input, result, parameters, scale);
}
template <>
bool TryCastFromDecimal::Operation(hugeint_t input, int64_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale) {
	return TryCastHugeDecimalToNumeric<int64_t>(input, result, parameters, scale);
}

//===--------------------------------------------------------------------===//
// Cast Decimal -> uint8_t
//===--------------------------------------------------------------------===//
template <>
bool TryCastFromDecimal::Operation(int16_t input, uint8_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale) {
	return TryCastDecimalToNumeric<int16_t, uint8_t>(input, result, parameters, scale);
}
template <>
bool TryCastFromDecimal::Operation(int32_t input, uint8_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale) {
	return TryCastDecimalToNumeric<int32_t, uint8_t>(input, result, parameters, scale);
}
template <>
bool TryCastFromDecimal::Operation(int64_t input, uint8_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale) {
	return TryCastDecimalToNumeric<int64_t, uint8_t>(input, result, parameters, scale);
}
template <>
bool TryCastFromDecimal::Operation(hugeint_t input, uint8_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale) {
	return TryCastHugeDecimalToNumeric<uint8_t>(input, result, parameters, scale);
}

//===--------------------------------------------------------------------===//
// Cast Decimal -> uint16_t
//===--------------------------------------------------------------------===//
template <>
bool TryCastFromDecimal::Operation(int16_t input, uint16_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale) {
	return TryCastDecimalToNumeric<int16_t, uint16_t>(input, result, parameters, scale);
}
template <>
bool TryCastFromDecimal::Operation(int32_t input, uint16_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale) {
	return TryCastDecimalToNumeric<int32_t, uint16_t>(input, result, parameters, scale);
}
template <>
bool TryCastFromDecimal::Operation(int64_t input, uint16_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale) {
	return TryCastDecimalToNumeric<int64_t, uint16_t>(input, result, parameters, scale);
}
template <>
bool TryCastFromDecimal::Operation(hugeint_t input, uint16_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale) {
	return TryCastHugeDecimalToNumeric<uint16_t>(input, result, parameters, scale);
}

//===--------------------------------------------------------------------===//
// Cast Decimal -> uint32_t
//===--------------------------------------------------------------------===//
template <>
bool TryCastFromDecimal::Operation(int16_t input, uint32_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale) {
	return TryCastDecimalToNumeric<int16_t, uint32_t>(input, result, parameters, scale);
}
template <>
bool TryCastFromDecimal::Operation(int32_t input, uint32_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale) {
	return TryCastDecimalToNumeric<int32_t, uint32_t>(input, result, parameters, scale);
}
template <>
bool TryCastFromDecimal::Operation(int64_t input, uint32_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale) {
	return TryCastDecimalToNumeric<int64_t, uint32_t>(input, result, parameters, scale);
}
template <>
bool TryCastFromDecimal::Operation(hugeint_t input, uint32_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale) {
	return TryCastHugeDecimalToNumeric<uint32_t>(input, result, parameters, scale);
}

//===--------------------------------------------------------------------===//
// Cast Decimal -> uint64_t
//===--------------------------------------------------------------------===//
template <>
bool TryCastFromDecimal::Operation(int16_t input, uint64_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale) {
	return TryCastDecimalToNumeric<int16_t, uint64_t>(input, result, parameters, scale);
}
template <>
bool TryCastFromDecimal::Operation(int32_t input, uint64_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale) {
	return TryCastDecimalToNumeric<int32_t, uint64_t>(input, result, parameters, scale);
}
template <>
bool TryCastFromDecimal::Operation(int64_t input, uint64_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale) {
	return TryCastDecimalToNumeric<int64_t, uint64_t>(input, result, parameters, scale);
}
template <>
bool TryCastFromDecimal::Operation(hugeint_t input, uint64_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale) {
	return TryCastHugeDecimalToNumeric<uint64_t>(input, result, parameters, scale);
}

//===--------------------------------------------------------------------===//
// Cast Decimal -> hugeint_t
//===--------------------------------------------------------------------===//
template <>
bool TryCastFromDecimal::Operation(int16_t input, hugeint_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale) {
	return TryCastDecimalToNumeric<int16_t, hugeint_t>(input, result, parameters, scale);
}
template <>
bool TryCastFromDecimal::Operation(int32_t input, hugeint_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale) {
	return TryCastDecimalToNumeric<int32_t, hugeint_t>(input, result, parameters, scale);
}
template <>
bool TryCastFromDecimal::Operation(int64_t input, hugeint_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale) {
	return TryCastDecimalToNumeric<int64_t, hugeint_t>(input, result, parameters, scale);
}
template <>
bool TryCastFromDecimal::Operation(hugeint_t input, hugeint_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale) {
	return TryCastHugeDecimalToNumeric<hugeint_t>(input, result, parameters, scale);
}

//===--------------------------------------------------------------------===//
// Cast Decimal -> uhugeint_t
//===--------------------------------------------------------------------===//
template <>
bool TryCastFromDecimal::Operation(int16_t input, uhugeint_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale) {
	return TryCastDecimalToNumeric<int16_t, uhugeint_t>(input, result, parameters, scale);
}
template <>
bool TryCastFromDecimal::Operation(int32_t input, uhugeint_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale) {
	return TryCastDecimalToNumeric<int32_t, uhugeint_t>(input, result, parameters, scale);
}
template <>
bool TryCastFromDecimal::Operation(int64_t input, uhugeint_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale) {
	return TryCastDecimalToNumeric<int64_t, uhugeint_t>(input, result, parameters, scale);
}
template <>
bool TryCastFromDecimal::Operation(hugeint_t input, uhugeint_t &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale) {
	return TryCastHugeDecimalToNumeric<uhugeint_t>(input, result, parameters, scale);
}

//===--------------------------------------------------------------------===//
// Decimal -> Float/Double Cast
//===--------------------------------------------------------------------===//
template <class SRC, class DST>
static bool IsRepresentableExactly(SRC input, DST);

template <>
bool IsRepresentableExactly(int16_t input, float dst) {
	return true;
}

const int64_t MAX_INT_REPRESENTABLE_IN_FLOAT = 0x001000000LL;
const int64_t MAX_INT_REPRESENTABLE_IN_DOUBLE = 0x0020000000000000LL;

template <>
bool IsRepresentableExactly(int32_t input, float dst) {
	return (input <= MAX_INT_REPRESENTABLE_IN_FLOAT && input >= -MAX_INT_REPRESENTABLE_IN_FLOAT);
}

template <>
bool IsRepresentableExactly(int64_t input, float dst) {
	return (input <= MAX_INT_REPRESENTABLE_IN_FLOAT && input >= -MAX_INT_REPRESENTABLE_IN_FLOAT);
}

template <>
bool IsRepresentableExactly(hugeint_t input, float dst) {
	return (input <= MAX_INT_REPRESENTABLE_IN_FLOAT && input >= -MAX_INT_REPRESENTABLE_IN_FLOAT);
}

template <>
bool IsRepresentableExactly(int16_t input, double dst) {
	return true;
}

template <>
bool IsRepresentableExactly(int32_t input, double dst) {
	return true;
}

template <>
bool IsRepresentableExactly(int64_t input, double dst) {
	return (input <= MAX_INT_REPRESENTABLE_IN_DOUBLE && input >= -MAX_INT_REPRESENTABLE_IN_DOUBLE);
}

template <>
bool IsRepresentableExactly(hugeint_t input, double dst) {
	return (input <= MAX_INT_REPRESENTABLE_IN_DOUBLE && input >= -MAX_INT_REPRESENTABLE_IN_DOUBLE);
}

template <class SRC, class DST>
static bool CanUseDecimalFloatingPointFastPath(SRC input, uint8_t scale) {
	return duckdb_fast_float::binary_format<DST>::min_exponent_fast_path() <= -scale &&
	       -scale <= duckdb_fast_float::binary_format<DST>::max_exponent_fast_path() &&
	       IsRepresentableExactly<SRC, DST>(input, DST(0.0));
}

template <class UNSIGNED>
static void FillDecimalDigits(UNSIGNED input, duckdb_fast_float::decimal &decimal) {
	uint8_t digits[DecimalWidth<hugeint_t>::max];
	while (input > 0) {
		digits[decimal.num_digits++] = UnsafeNumericCast<uint8_t>(input % 10);
		input /= 10;
	}
	for (uint32_t i = 0; i < decimal.num_digits; i++) {
		decimal.digits[i] = digits[decimal.num_digits - i - 1];
	}
}

template <class SRC>
static void FillDecimalDigits(SRC input, duckdb_fast_float::decimal &decimal, bool &negative) {
	using UNSIGNED = typename MakeUnsigned<SRC>::type;
	if (input < 0) {
		negative = true;
		auto unsigned_input = UnsafeNumericCast<UNSIGNED>(-(input + 1));
		FillDecimalDigits<UNSIGNED>(unsigned_input + 1, decimal);
	} else {
		negative = false;
		FillDecimalDigits<UNSIGNED>(UnsafeNumericCast<UNSIGNED>(input), decimal);
	}
}

static void FillDecimalDigits(hugeint_t input, duckdb_fast_float::decimal &decimal, bool &negative) {
	negative = false;
	if (input == 0) {
		return;
	}

	if (input < 0) {
		negative = true;
		Hugeint::NegateInPlace(input);
	}

	// a hugeint can have one more digit than the maximum decimal width
	char buffer[DecimalWidth<hugeint_t>::max + 1];
	auto end = buffer + sizeof(buffer);
	auto begin = NumericHelper::FormatUnsigned(input, end);

	decimal.num_digits = UnsafeNumericCast<uint32_t>(end - begin);
	for (uint32_t i = 0; i < decimal.num_digits; i++) {
		decimal.digits[i] = UnsafeNumericCast<uint8_t>(begin[i] - '0');
	}
}

template <class SRC, class DST>
bool TryCastDecimalToFloatingPoint(SRC input, DST &result, uint8_t width, uint8_t scale) {
	if (scale == 0 || CanUseDecimalFloatingPointFastPath<SRC, DST>(input, scale)) {
		// Fast path, integer and decimal exponent are representable exactly as a float/double
		result = Cast::Operation<SRC, DST>(input) / DST(NumericHelper::DOUBLE_POWERS_OF_TEN[scale]);
		return true;
	}

	duckdb_fast_float::decimal decimal;
	bool negative = false;
	FillDecimalDigits(input, decimal, negative);
	decimal.decimal_point = UnsafeNumericCast<int32_t>(decimal.num_digits) - UnsafeNumericCast<int32_t>(scale);
	while (decimal.num_digits > 0 && decimal.digits[decimal.num_digits - 1] == 0) {
		decimal.num_digits--;
	}
	for (uint32_t i = decimal.num_digits; i < duckdb_fast_float::max_digit_without_overflow; i++) {
		decimal.digits[i] = 0;
	}

	auto adjusted_mantissa = duckdb_fast_float::compute_float<duckdb_fast_float::binary_format<DST>>(decimal);
	duckdb_fast_float::detail::to_float(negative, adjusted_mantissa, result);
	return true;
}

// DECIMAL -> FLOAT
template <>
bool TryCastFromDecimal::Operation(int16_t input, float &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale) {
	return TryCastDecimalToFloatingPoint<int16_t, float>(input, result, width, scale);
}

template <>
bool TryCastFromDecimal::Operation(int32_t input, float &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale) {
	return TryCastDecimalToFloatingPoint<int32_t, float>(input, result, width, scale);
}

template <>
bool TryCastFromDecimal::Operation(int64_t input, float &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale) {
	return TryCastDecimalToFloatingPoint<int64_t, float>(input, result, width, scale);
}

template <>
bool TryCastFromDecimal::Operation(hugeint_t input, float &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale) {
	return TryCastDecimalToFloatingPoint<hugeint_t, float>(input, result, width, scale);
}

// DECIMAL -> DOUBLE
template <>
bool TryCastFromDecimal::Operation(int16_t input, double &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale) {
	return TryCastDecimalToFloatingPoint<int16_t, double>(input, result, width, scale);
}

template <>
bool TryCastFromDecimal::Operation(int32_t input, double &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale) {
	return TryCastDecimalToFloatingPoint<int32_t, double>(input, result, width, scale);
}

template <>
bool TryCastFromDecimal::Operation(int64_t input, double &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale) {
	return TryCastDecimalToFloatingPoint<int64_t, double>(input, result, width, scale);
}

template <>
bool TryCastFromDecimal::Operation(hugeint_t input, double &result, CastParameters &parameters, uint8_t width,
                                   uint8_t scale) {
	return TryCastDecimalToFloatingPoint<hugeint_t, double>(input, result, width, scale);
}

} // namespace duckdb
