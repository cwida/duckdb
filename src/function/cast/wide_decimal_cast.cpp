#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/bignum.hpp"
#include "duckdb/common/types/bignum_decimal.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/common/vector_operations/general_cast.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/function/cast/default_casts.hpp"
#include "duckdb/function/cast/vector_cast_helpers.hpp"

//===--------------------------------------------------------------------===//
// Casts to and from wide decimals, whose unscaled value is a BIGNUM blob, and from BIGNUMs as scale 0 decimals
//===--------------------------------------------------------------------===//
namespace duckdb {

namespace {

struct WideDecimalCastInput {
	WideDecimalCastInput(Vector &result_p, CastParameters &parameters, const LogicalType &source_type, uint32_t width_p,
	                     uint32_t scale_p)
	    : result(result_p), vector_cast_data(result_p, parameters),
	      source_is_decimal(source_type.id() == LogicalTypeId::DECIMAL),
	      source_width(source_is_decimal ? DecimalType::GetWidth(source_type) : 0),
	      source_scale(source_is_decimal ? DecimalType::GetScale(source_type) : 0), width(width_p), scale(scale_p) {
	}

	Vector &result;
	VectorTryCastData vector_cast_data;
	//! Whether the source is a decimal, and its width and scale (a BIGNUM source has scale 0)
	bool source_is_decimal;
	uint32_t source_width;
	uint32_t source_scale;
	//! The width and scale of a decimal target
	uint32_t width;
	uint32_t scale;

	string_t StoreBlob(const string &blob) {
		return StringVector::AddStringOrBlob(result, blob.data(), blob.size());
	}

	template <class RESULT_TYPE>
	RESULT_TYPE Fail(const string &message, ValidityMask &mask, idx_t idx) {
		return HandleVectorCastError::Operation<RESULT_TYPE>(message, mask, idx, vector_cast_data);
	}

	string OutOfRange(const string &value) const {
		return StringUtil::Format("Casting value \"%s\" to type %s failed: value is out of range!", value,
		                          result.GetType().ToString());
	}
};

//===--------------------------------------------------------------------===//
// Unscaled integer blobs
//===--------------------------------------------------------------------===//
string UnscaledBlob(uhugeint_t value) {
	return BignumDecimal::FromInteger(value);
}

string UnscaledBlob(hugeint_t value) {
	return BignumDecimal::FromInteger(value);
}

string UnscaledBlob(bool value) {
	return BignumDecimal::FromInteger(hugeint_t(value ? 1 : 0));
}

template <class T>
string UnscaledBlob(T value) {
	return BignumDecimal::FromInteger(Hugeint::Convert(value));
}

//! The text of an integer source for an error message
template <class T>
string IntegerText(T value) {
	return BignumDecimal::ToString(string_t(UnscaledBlob(value)), 0);
}

string IntegerText(bool value) {
	return value ? "true" : "false";
}

//===--------------------------------------------------------------------===//
// To wide decimal
//===--------------------------------------------------------------------===//
struct StringToWideDecimal {
	template <class INPUT_TYPE, class RESULT_TYPE>
	static RESULT_TYPE Operation(INPUT_TYPE input, ValidityMask &mask, idx_t idx, WideDecimalCastInput &data) {
		string blob;
		if (!BignumDecimal::TryParse(input.GetData(), input.GetSize(), data.width, data.scale, blob)) {
			return data.Fail<RESULT_TYPE>(StringUtil::Format("Could not convert string \"%s\" to %s", input.GetString(),
			                                                 data.result.GetType().ToString()),
			                              mask, idx);
		}
		return data.StoreBlob(blob);
	}
};

//! A BIGNUM or wide decimal source is rescaled to the target scale
struct ScaledToWideDecimal {
	template <class INPUT_TYPE, class RESULT_TYPE>
	static RESULT_TYPE Operation(INPUT_TYPE input, ValidityMask &mask, idx_t idx, WideDecimalCastInput &data) {
		string blob;
		if (!BignumDecimal::TryRescale(input, data.source_scale, data.width, data.scale, blob)) {
			return data.Fail<RESULT_TYPE>(data.OutOfRange(BignumDecimal::ToString(input, data.source_scale)), mask,
			                              idx);
		}
		return data.StoreBlob(blob);
	}
};

//! An integer source, or a fixed-size decimal source (whose scale is data.source_scale)
struct IntegerToWideDecimal {
	template <class INPUT_TYPE, class RESULT_TYPE>
	static RESULT_TYPE Operation(INPUT_TYPE input, ValidityMask &mask, idx_t idx, WideDecimalCastInput &data) {
		auto unscaled = UnscaledBlob(input);
		string blob;
		if (!BignumDecimal::TryRescale(string_t(unscaled), data.source_scale, data.width, data.scale, blob)) {
			if (data.source_is_decimal) {
				return data.Fail<RESULT_TYPE>(
				    data.OutOfRange(BignumDecimal::ToString(string_t(unscaled), data.source_scale)), mask, idx);
			}
			// the message of the integer -> fixed-size decimal cast
			return data.Fail<RESULT_TYPE>(StringUtil::Format("Could not cast value %s to %s", IntegerText(input),
			                                                 data.result.GetType().ToString()),
			                              mask, idx);
		}
		return data.StoreBlob(blob);
	}
};

struct DoubleToWideDecimal {
	template <class INPUT_TYPE, class RESULT_TYPE>
	static RESULT_TYPE Operation(INPUT_TYPE input, ValidityMask &mask, idx_t idx, WideDecimalCastInput &data) {
		string blob;
		if (!BignumDecimal::TryFromDouble(static_cast<double>(input), data.width, data.scale, blob)) {
			return data.Fail<RESULT_TYPE>(StringUtil::Format("Could not cast value %s to %s",
			                                                 std::to_string(static_cast<double>(input)),
			                                                 data.result.GetType().ToString()),
			                              mask, idx);
		}
		return data.StoreBlob(blob);
	}
};

template <class SRC, class OP>
bool ExecuteToWideDecimal(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	auto &result_type = result.GetType();
	WideDecimalCastInput data(result, parameters, source.GetType(), DecimalType::GetWidth(result_type),
	                          DecimalType::GetScale(result_type));
	UnaryExecutor::GenericExecute<SRC, string_t, OP>(source, result, count, data, parameters.error_message);
	return data.vector_cast_data.all_converted;
}

//===--------------------------------------------------------------------===//
// From wide decimal (or BIGNUM)
//===--------------------------------------------------------------------===//
template <class T>
bool BlobToInteger(const string_t &blob, T &result) {
	bool strict = false;
	return Bignum::BignumToInt<T>(bignum_t(blob), result, strict);
}

struct WideDecimalToString {
	template <class INPUT_TYPE, class RESULT_TYPE>
	static RESULT_TYPE Operation(INPUT_TYPE input, ValidityMask &mask, idx_t idx, WideDecimalCastInput &data) {
		return StringVector::AddString(data.result, Decimal::ToString(input, data.source_width, data.source_scale));
	}
};

//! Rescales a wide decimal to a fixed-size decimal, the checked width guarantees the value fits the integer
struct WideDecimalToFixedDecimal {
	template <class INPUT_TYPE, class RESULT_TYPE>
	static RESULT_TYPE Operation(INPUT_TYPE input, ValidityMask &mask, idx_t idx, WideDecimalCastInput &data) {
		string blob;
		if (!BignumDecimal::TryRescale(input, data.source_scale, data.width, data.scale, blob)) {
			return data.Fail<RESULT_TYPE>(data.OutOfRange(BignumDecimal::ToString(input, data.source_scale)), mask,
			                              idx);
		}
		hugeint_t value;
		if (!BignumDecimal::TryToHugeint(string_t(blob), value)) {
			throw InternalException("Rescaled wide decimal does not fit a HUGEINT despite the width check");
		}
		return Cast::Operation<hugeint_t, RESULT_TYPE>(value);
	}
};

//! Rounds a wide decimal to scale 0 and range checks it against the integer type
struct WideDecimalToInteger {
	template <class INPUT_TYPE, class RESULT_TYPE>
	static RESULT_TYPE Operation(INPUT_TYPE input, ValidityMask &mask, idx_t idx, WideDecimalCastInput &data) {
		string blob;
		RESULT_TYPE result;
		if (!BignumDecimal::TryRescale(input, data.source_scale, Decimal::MAX_WIDTH_DECIMAL, 0, blob) ||
		    !BlobToInteger<RESULT_TYPE>(string_t(blob), result)) {
			// the message of the fixed-size decimal -> integer cast
			return data.Fail<RESULT_TYPE>(StringUtil::Format("Failed to cast decimal value %s to type %s",
			                                                 BignumDecimal::ToString(input, data.source_scale),
			                                                 data.result.GetType().ToString()),
			                              mask, idx);
		}
		return result;
	}
};

struct WideDecimalToBignum {
	template <class INPUT_TYPE, class RESULT_TYPE>
	static RESULT_TYPE Operation(INPUT_TYPE input, ValidityMask &mask, idx_t idx, WideDecimalCastInput &data) {
		string blob;
		if (!BignumDecimal::TryRescale(input, data.source_scale, Decimal::MAX_WIDTH_DECIMAL, 0, blob)) {
			return data.Fail<RESULT_TYPE>(data.OutOfRange(BignumDecimal::ToString(input, data.source_scale)), mask,
			                              idx);
		}
		return data.StoreBlob(blob);
	}
};

struct WideDecimalToFloating {
	template <class INPUT_TYPE, class RESULT_TYPE>
	static RESULT_TYPE Operation(INPUT_TYPE input, ValidityMask &mask, idx_t idx, WideDecimalCastInput &data) {
		auto value = BignumDecimal::ToDouble(input, data.source_scale);
		RESULT_TYPE result;
		if (!TryCast::Operation<double, RESULT_TYPE>(value, result, false)) {
			return data.Fail<RESULT_TYPE>(data.OutOfRange(BignumDecimal::ToString(input, data.source_scale)), mask,
			                              idx);
		}
		return result;
	}
};

struct WideDecimalToBool {
	template <class INPUT_TYPE, class RESULT_TYPE>
	static RESULT_TYPE Operation(INPUT_TYPE input, ValidityMask &mask, idx_t idx, WideDecimalCastInput &data) {
		return !BignumDecimal::IsZero(input);
	}
};

template <class RESULT, class OP>
bool ExecuteFromWideDecimal(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	auto &result_type = result.GetType();
	uint32_t width = 0;
	uint32_t scale = 0;
	if (result_type.id() == LogicalTypeId::DECIMAL) {
		width = DecimalType::GetWidth(result_type);
		scale = DecimalType::GetScale(result_type);
	}
	WideDecimalCastInput data(result, parameters, source.GetType(), width, scale);
	UnaryExecutor::GenericExecute<string_t, RESULT, OP>(source, result, count, data, parameters.error_message);
	return data.vector_cast_data.all_converted;
}

template <class RESULT>
bool FromWideDecimalToIntegerCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	return ExecuteFromWideDecimal<RESULT, WideDecimalToInteger>(source, result, count, parameters);
}

template <class RESULT>
bool FromWideDecimalToFloatingCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	return ExecuteFromWideDecimal<RESULT, WideDecimalToFloating>(source, result, count, parameters);
}

bool FromWideDecimalToBoolCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	return ExecuteFromWideDecimal<bool, WideDecimalToBool>(source, result, count, parameters);
}

bool FromWideDecimalToStringCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	return ExecuteFromWideDecimal<string_t, WideDecimalToString>(source, result, count, parameters);
}

bool FromWideDecimalToBignumCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	return ExecuteFromWideDecimal<string_t, WideDecimalToBignum>(source, result, count, parameters);
}

bool FromWideDecimalToDecimalCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	switch (result.GetType().InternalType()) {
	case PhysicalType::INT16:
		return ExecuteFromWideDecimal<int16_t, WideDecimalToFixedDecimal>(source, result, count, parameters);
	case PhysicalType::INT32:
		return ExecuteFromWideDecimal<int32_t, WideDecimalToFixedDecimal>(source, result, count, parameters);
	case PhysicalType::INT64:
		return ExecuteFromWideDecimal<int64_t, WideDecimalToFixedDecimal>(source, result, count, parameters);
	case PhysicalType::INT128:
		return ExecuteFromWideDecimal<hugeint_t, WideDecimalToFixedDecimal>(source, result, count, parameters);
	case PhysicalType::VARCHAR:
		return ExecuteToWideDecimal<string_t, ScaledToWideDecimal>(source, result, count, parameters);
	default:
		throw InternalException("Unimplemented internal type for decimal");
	}
}

//! Rounds a fixed-size decimal to scale 0 for the BIGNUM
struct FixedDecimalToBignum {
	template <class INPUT_TYPE, class RESULT_TYPE>
	static RESULT_TYPE Operation(INPUT_TYPE input, ValidityMask &mask, idx_t idx, WideDecimalCastInput &data) {
		auto unscaled = UnscaledBlob(hugeint_t(input));
		string blob;
		if (!BignumDecimal::TryRescale(string_t(unscaled), data.source_scale, Decimal::MAX_WIDTH_DECIMAL, 0, blob)) {
			throw InternalException("Rounding a fixed-size decimal to an integer cannot exceed the BIGNUM range");
		}
		return data.StoreBlob(blob);
	}
};

} // namespace

bool DefaultCasts::ToWideDecimalCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	D_ASSERT(result.GetType().id() == LogicalTypeId::DECIMAL &&
	         Decimal::IsWide(DecimalType::GetWidth(result.GetType())));
	auto &source_type = source.GetType();
	switch (source_type.id()) {
	case LogicalTypeId::BOOLEAN:
		return ExecuteToWideDecimal<bool, IntegerToWideDecimal>(source, result, count, parameters);
	case LogicalTypeId::TINYINT:
		return ExecuteToWideDecimal<int8_t, IntegerToWideDecimal>(source, result, count, parameters);
	case LogicalTypeId::SMALLINT:
		return ExecuteToWideDecimal<int16_t, IntegerToWideDecimal>(source, result, count, parameters);
	case LogicalTypeId::INTEGER:
		return ExecuteToWideDecimal<int32_t, IntegerToWideDecimal>(source, result, count, parameters);
	case LogicalTypeId::BIGINT:
		return ExecuteToWideDecimal<int64_t, IntegerToWideDecimal>(source, result, count, parameters);
	case LogicalTypeId::UTINYINT:
		return ExecuteToWideDecimal<uint8_t, IntegerToWideDecimal>(source, result, count, parameters);
	case LogicalTypeId::USMALLINT:
		return ExecuteToWideDecimal<uint16_t, IntegerToWideDecimal>(source, result, count, parameters);
	case LogicalTypeId::UINTEGER:
		return ExecuteToWideDecimal<uint32_t, IntegerToWideDecimal>(source, result, count, parameters);
	case LogicalTypeId::UBIGINT:
		return ExecuteToWideDecimal<uint64_t, IntegerToWideDecimal>(source, result, count, parameters);
	case LogicalTypeId::HUGEINT:
		return ExecuteToWideDecimal<hugeint_t, IntegerToWideDecimal>(source, result, count, parameters);
	case LogicalTypeId::UHUGEINT:
		return ExecuteToWideDecimal<uhugeint_t, IntegerToWideDecimal>(source, result, count, parameters);
	case LogicalTypeId::FLOAT:
		return ExecuteToWideDecimal<float, DoubleToWideDecimal>(source, result, count, parameters);
	case LogicalTypeId::DOUBLE:
		return ExecuteToWideDecimal<double, DoubleToWideDecimal>(source, result, count, parameters);
	case LogicalTypeId::VARCHAR:
		return ExecuteToWideDecimal<string_t, StringToWideDecimal>(source, result, count, parameters);
	case LogicalTypeId::BIGNUM:
		return ExecuteToWideDecimal<string_t, ScaledToWideDecimal>(source, result, count, parameters);
	case LogicalTypeId::DECIMAL:
		switch (source_type.InternalType()) {
		case PhysicalType::INT16:
			return ExecuteToWideDecimal<int16_t, IntegerToWideDecimal>(source, result, count, parameters);
		case PhysicalType::INT32:
			return ExecuteToWideDecimal<int32_t, IntegerToWideDecimal>(source, result, count, parameters);
		case PhysicalType::INT64:
			return ExecuteToWideDecimal<int64_t, IntegerToWideDecimal>(source, result, count, parameters);
		case PhysicalType::INT128:
			return ExecuteToWideDecimal<hugeint_t, IntegerToWideDecimal>(source, result, count, parameters);
		case PhysicalType::VARCHAR:
			return ExecuteToWideDecimal<string_t, ScaledToWideDecimal>(source, result, count, parameters);
		default:
			throw InternalException("Unimplemented internal type for decimal");
		}
	default:
		return TryVectorNullCast(source, result, count, parameters);
	}
}

bool DefaultCasts::DecimalToBignumCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	D_ASSERT(source.GetType().id() == LogicalTypeId::DECIMAL && result.GetType().id() == LogicalTypeId::BIGNUM);
	WideDecimalCastInput data(result, parameters, source.GetType(), 0, 0);
	switch (source.GetType().InternalType()) {
	case PhysicalType::INT16:
		UnaryExecutor::GenericExecute<int16_t, string_t, FixedDecimalToBignum>(source, result, count, data);
		return true;
	case PhysicalType::INT32:
		UnaryExecutor::GenericExecute<int32_t, string_t, FixedDecimalToBignum>(source, result, count, data);
		return true;
	case PhysicalType::INT64:
		UnaryExecutor::GenericExecute<int64_t, string_t, FixedDecimalToBignum>(source, result, count, data);
		return true;
	case PhysicalType::INT128:
		UnaryExecutor::GenericExecute<hugeint_t, string_t, FixedDecimalToBignum>(source, result, count, data);
		return true;
	case PhysicalType::VARCHAR:
		return FromWideDecimalToBignumCast(source, result, count, parameters);
	default:
		throw InternalException("Unimplemented internal type for decimal");
	}
}

BoundCastInfo DefaultCasts::FromWideDecimalCastSwitch(BindCastInput &input, const LogicalType &source,
                                                      const LogicalType &target) {
	D_ASSERT(source.id() == LogicalTypeId::BIGNUM ||
	         (source.id() == LogicalTypeId::DECIMAL && source.InternalType() == PhysicalType::VARCHAR));
	switch (target.id()) {
	case LogicalTypeId::BOOLEAN:
		return FromWideDecimalToBoolCast;
	case LogicalTypeId::TINYINT:
		return FromWideDecimalToIntegerCast<int8_t>;
	case LogicalTypeId::SMALLINT:
		return FromWideDecimalToIntegerCast<int16_t>;
	case LogicalTypeId::INTEGER:
		return FromWideDecimalToIntegerCast<int32_t>;
	case LogicalTypeId::BIGINT:
		return FromWideDecimalToIntegerCast<int64_t>;
	case LogicalTypeId::UTINYINT:
		return FromWideDecimalToIntegerCast<uint8_t>;
	case LogicalTypeId::USMALLINT:
		return FromWideDecimalToIntegerCast<uint16_t>;
	case LogicalTypeId::UINTEGER:
		return FromWideDecimalToIntegerCast<uint32_t>;
	case LogicalTypeId::UBIGINT:
		return FromWideDecimalToIntegerCast<uint64_t>;
	case LogicalTypeId::HUGEINT:
		return FromWideDecimalToIntegerCast<hugeint_t>;
	case LogicalTypeId::UHUGEINT:
		return FromWideDecimalToIntegerCast<uhugeint_t>;
	case LogicalTypeId::FLOAT:
		return FromWideDecimalToFloatingCast<float>;
	case LogicalTypeId::DOUBLE:
		return FromWideDecimalToFloatingCast<double>;
	case LogicalTypeId::DECIMAL:
		return FromWideDecimalToDecimalCast;
	case LogicalTypeId::VARCHAR:
		return FromWideDecimalToStringCast;
	case LogicalTypeId::BIGNUM:
		return FromWideDecimalToBignumCast;
	default:
		return DefaultCasts::TryVectorNullCast;
	}
}

} // namespace duckdb
