#include "duckdb/function/cast/default_casts.hpp"
#include "duckdb/function/cast/vector_cast_helpers.hpp"

namespace duckdb {

BoundCastInfo DefaultCasts::BlobCastSwitch(BindCastInput &input, const LogicalType &source, const LogicalType &target) {
	// now switch on the result type
	switch (target.id()) {
	case LogicalTypeId::VARCHAR:
		// blob to varchar
		return BoundCastInfo(&VectorCastHelpers::StringCast<string_t, duckdb::CastFromBlob>);
	case LogicalTypeId::UUID:
		// blob to uuid
		return BoundCastInfo(&VectorCastHelpers::TryCastStrictLoop<string_t, hugeint_t, duckdb::TryCastBlobToUUID>);
	case LogicalTypeId::LEGACY_AGGREGATE_STATE:
		return DefaultCasts::ReinterpretCast;
	case LogicalTypeId::BIT:
		return BoundCastInfo(&VectorCastHelpers::TryCastStringLoop<string_t, string_t, duckdb::TryCastBlobToBit>);
	case LogicalTypeId::GEOMETRY:
		return BlobToGeoCast(input, source, target);
	default:
		return DefaultCasts::TryVectorNullCast;
	}
}

} // namespace duckdb
