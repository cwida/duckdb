#include "duckdb/function/cast/default_casts.hpp"
#include "duckdb/function/cast/cast_statistics.hpp"
#include "duckdb/function/cast/vector_cast_helpers.hpp"
#include "duckdb/common/operator/string_cast.hpp"
#include "duckdb/common/operator/numeric_cast.hpp"
#include "duckdb/common/smaller_binary.hpp"
#include "duckdb/common/types/bignum.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"

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
		return DefaultCasts::WideDecimalCast(BoundCastInfo(&VectorCastHelpers::ToDecimalCast<SRC>), source, target);
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
