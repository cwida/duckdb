#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/function/cast/default_casts.hpp"
#include "duckdb/function/cast/cast_statistics.hpp"
#include "duckdb/function/cast/vector_cast_helpers.hpp"
#include "duckdb/common/operator/string_cast.hpp"
#include "duckdb/common/operator/numeric_cast.hpp"
#include "duckdb/common/smaller_binary.hpp"
#include "duckdb/common/types/bignum.hpp"
#include "duckdb/common/types/cast_helpers.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"

#include <cmath>
#include <type_traits>

namespace duckdb {

#if !DUCKDB_SMALLER_BINARY(unchecked_numeric_casts)
struct UncheckedNumericCast {
	template <class SRC, class DST>
	static DST Operation(SRC input) {
		return static_cast<DST>(input);
	}
};

template <class SRC, class DST>
static unique_ptr<BaseStatistics> PropagateNumericCastStatistics(CastStatisticsInput &input) {
	input.SetFunction(&VectorCastHelpers::TryCastLoop<SRC, DST, duckdb::NumericTryCast>);
	auto result = CastStatistics::TryPropagate(input.child_stats, input.source_type, input.target_type);
	if (result) {
		input.SetFunction(&VectorCastHelpers::TemplatedCastLoop<SRC, DST, UncheckedNumericCast>);
	}
	return result;
}
#endif

template <class SRC, class DST>
static BoundCastInfo GetNumericCast() {
	auto result = BoundCastInfo(&VectorCastHelpers::TryCastLoop<SRC, DST, duckdb::NumericTryCast>);
#if !DUCKDB_SMALLER_BINARY(unchecked_numeric_casts)
	if constexpr (std::is_integral<SRC>::value && std::is_integral<DST>::value && !std::is_same<SRC, bool>::value &&
	              !std::is_same<DST, bool>::value) {
		result.SetStatisticsCallback(PropagateNumericCastStatistics<SRC, DST>);
	}
#endif
	return result;
}

//===--------------------------------------------------------------------===//
// FLOAT/DOUBLE -> DECIMAL
//===--------------------------------------------------------------------===//
// A vectorizable loop that cannot fail; rows out of range are cast again by TryCastToDecimal
template <class DST>
struct FloatToDecimalConvert {
	// |rounded| < 10^width, so the conversion is exact
	static inline DST Operation(double rounded) {
		return static_cast<DST>(static_cast<int64_t>(rounded));
	}
};

template <>
struct FloatToDecimalConvert<hugeint_t> {
	// the halves of an integral |rounded| < 2^127 are exact, without fmod
	static inline hugeint_t Operation(double rounded) {
		constexpr double TWO_POW_64 = 18446744073709551616.0;
		const double magnitude = std::fabs(rounded);
		const auto upper = static_cast<uint64_t>(magnitude * (1.0 / TWO_POW_64));
		const auto lower = static_cast<uint64_t>(magnitude - static_cast<double>(upper) * TWO_POW_64);
		// branch-free negation: -(upper:lower) = (~upper + (lower == 0)) : -lower
		const uint64_t negative = uint64_t(0) - uint64_t(rounded < 0);
		const int64_t negative_signed = -static_cast<int64_t>(rounded < 0);
		const auto upper_signed = static_cast<int64_t>(upper);
		hugeint_t result;
		result.lower = (lower ^ negative) - negative;
		result.upper = (upper_signed ^ negative_signed) + (negative_signed & static_cast<int64_t>(lower == 0));
		return result;
	}
};

template <class SRC, class DST>
static bool TemplatedFloatToDecimalCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters,
                                        uint8_t width, uint8_t scale) {
	const double multiplier = NumericHelper::DOUBLE_POWERS_OF_TEN[scale];
	const double limit = NumericHelper::DOUBLE_POWERS_OF_TEN[width];
	bool all_in_range = true;
	UnaryExecutor::Execute<SRC, DST>(
	    source, result, count,
	    [multiplier, limit, &all_in_range](SRC input) {
		    const double rounded = RoundToNearestAwayFromZero(static_cast<double>(input) * multiplier);
		    // false for NaN, as in DoubleToDecimalCast
		    const bool in_range = (rounded > -limit) & (rounded < limit);
		    all_in_range &= in_range;
		    return FloatToDecimalConvert<DST>::Operation(in_range ? rounded : 0.0);
	    },
	    FunctionErrors::CAN_THROW_RUNTIME_ERROR);
	if (DUCKDB_LIKELY(all_in_range)) {
		return true;
	}
	if (source.GetVectorType() == VectorType::FLAT_VECTOR && result.GetVectorType() == VectorType::FLAT_VECTOR) {
		// cast the failed rows again, in row order, for the same errors and NULLs
		auto input_data = FlatVector::GetData<SRC>(source);
		auto result_writer = FlatVector::ScatterWriter<DST>(result);
		auto &result_validity = FlatVector::ValidityMutable(result);
		// the result mask may share the input's buffer
		const ValidityMask processed = result_validity;
		if (processed.CanHaveNull()) {
			result_validity.Copy(processed, count);
		}
		VectorDecimalCastData data(result, parameters, width, scale);
		for (idx_t i = 0; i < count; i++) {
			if (!processed.RowIsValid(i)) {
				continue;
			}
			const SRC input = input_data[i];
			const double scaled = static_cast<double>(input) * multiplier;
			// rounding moves a value by at most 0.5; NaN fails this check
			if (std::fabs(scaled) < limit - 1.0) {
				continue;
			}
			const double rounded = RoundToNearestAwayFromZero(scaled);
			if ((rounded > -limit) & (rounded < limit)) {
				continue;
			}
			result_writer[i] = VectorDecimalCastOperator<TryCastToDecimal>::template Operation<SRC, DST>(
			    input, result_validity, i, data);
		}
		return data.vector_cast_data.all_converted;
	}
	return VectorCastHelpers::TemplatedDecimalCast<SRC, DST, TryCastToDecimal>(source, result, count, parameters, width,
	                                                                           scale);
}

template <class SRC>
static bool FloatToDecimalCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	auto &result_type = result.GetType();
	auto width = DecimalType::GetWidth(result_type);
	auto scale = DecimalType::GetScale(result_type);
	switch (result_type.InternalType()) {
	case PhysicalType::INT16:
		return TemplatedFloatToDecimalCast<SRC, int16_t>(source, result, count, parameters, width, scale);
	case PhysicalType::INT32:
		return TemplatedFloatToDecimalCast<SRC, int32_t>(source, result, count, parameters, width, scale);
	case PhysicalType::INT64:
		return TemplatedFloatToDecimalCast<SRC, int64_t>(source, result, count, parameters, width, scale);
	case PhysicalType::INT128:
		return TemplatedFloatToDecimalCast<SRC, hugeint_t>(source, result, count, parameters, width, scale);
	default:
		InternalException::Throw("Unimplemented internal type for decimal");
	}
}

template <class SRC>
static BoundCastInfo InternalNumericCastSwitch(const LogicalType &source, const LogicalType &target) {
	// now switch on the result type
	switch (target.id()) {
	case LogicalTypeId::BOOLEAN:
		return GetNumericCast<SRC, bool>();
	case LogicalTypeId::TINYINT:
		return GetNumericCast<SRC, int8_t>();
	case LogicalTypeId::SMALLINT:
		return GetNumericCast<SRC, int16_t>();
	case LogicalTypeId::INTEGER:
		return GetNumericCast<SRC, int32_t>();
	case LogicalTypeId::BIGINT:
		return GetNumericCast<SRC, int64_t>();
	case LogicalTypeId::UTINYINT:
		return GetNumericCast<SRC, uint8_t>();
	case LogicalTypeId::USMALLINT:
		return GetNumericCast<SRC, uint16_t>();
	case LogicalTypeId::UINTEGER:
		return GetNumericCast<SRC, uint32_t>();
	case LogicalTypeId::UBIGINT:
		return GetNumericCast<SRC, uint64_t>();
	case LogicalTypeId::HUGEINT:
		return BoundCastInfo(&VectorCastHelpers::TryCastLoop<SRC, hugeint_t, duckdb::NumericTryCast>);
	case LogicalTypeId::UHUGEINT:
		return BoundCastInfo(&VectorCastHelpers::TryCastLoop<SRC, uhugeint_t, duckdb::NumericTryCast>);
	case LogicalTypeId::FLOAT:
		return BoundCastInfo(&VectorCastHelpers::TryCastLoop<SRC, float, duckdb::NumericTryCast>);
	case LogicalTypeId::DOUBLE:
		return BoundCastInfo(&VectorCastHelpers::TryCastLoop<SRC, double, duckdb::NumericTryCast>);
	case LogicalTypeId::DECIMAL:
		if constexpr (std::is_same<SRC, float>::value || std::is_same<SRC, double>::value) {
			return BoundCastInfo(&FloatToDecimalCast<SRC>);
		}
		return BoundCastInfo(&VectorCastHelpers::ToDecimalCast<SRC>);
	case LogicalTypeId::VARCHAR:
		return BoundCastInfo(&VectorCastHelpers::StringCast<SRC, duckdb::StringCast>);
	case LogicalTypeId::BIT:
		return BoundCastInfo(&VectorCastHelpers::StringCast<SRC, duckdb::NumericTryCastToBit>);
	case LogicalTypeId::BIGNUM:
		return Bignum::NumericToBignumCastSwitch(source);
	case LogicalTypeId::UUID:
		if (source.id() == LogicalTypeId::UHUGEINT) {
			return BoundCastInfo(&VectorCastHelpers::TemplatedCastLoop<SRC, hugeint_t, duckdb::CastFromUHugeintToUUID>);
		}
		return DefaultCasts::TryVectorNullCast;
	default:
		return DefaultCasts::TryVectorNullCast;
	}
}

BoundCastInfo DefaultCasts::NumericCastSwitch(BindCastInput &input, const LogicalType &source,
                                              const LogicalType &target) {
	switch (source.id()) {
	case LogicalTypeId::BOOLEAN:
		return InternalNumericCastSwitch<bool>(source, target);
	case LogicalTypeId::TINYINT:
		return InternalNumericCastSwitch<int8_t>(source, target);
	case LogicalTypeId::SMALLINT:
		return InternalNumericCastSwitch<int16_t>(source, target);
	case LogicalTypeId::INTEGER:
		return InternalNumericCastSwitch<int32_t>(source, target);
	case LogicalTypeId::BIGINT:
		return InternalNumericCastSwitch<int64_t>(source, target);
	case LogicalTypeId::UTINYINT:
		return InternalNumericCastSwitch<uint8_t>(source, target);
	case LogicalTypeId::USMALLINT:
		return InternalNumericCastSwitch<uint16_t>(source, target);
	case LogicalTypeId::UINTEGER:
		return InternalNumericCastSwitch<uint32_t>(source, target);
	case LogicalTypeId::UBIGINT:
		return InternalNumericCastSwitch<uint64_t>(source, target);
	case LogicalTypeId::HUGEINT:
		return InternalNumericCastSwitch<hugeint_t>(source, target);
	case LogicalTypeId::UHUGEINT:
		return InternalNumericCastSwitch<uhugeint_t>(source, target);
	case LogicalTypeId::FLOAT:
		return InternalNumericCastSwitch<float>(source, target);
	case LogicalTypeId::DOUBLE:
		return InternalNumericCastSwitch<double>(source, target);
	default:
		throw InternalException("NumericCastSwitch called with non-numeric argument");
	}
}

} // namespace duckdb
