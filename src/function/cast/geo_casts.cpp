#include "duckdb/common/types/geometry.hpp"
#include "duckdb/common/types/geometry_crs.hpp"
#include "duckdb/function/cast/default_casts.hpp"
#include "duckdb/function/cast/vector_cast_helpers.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/error_data.hpp"

namespace duckdb {

static bool GeometryToVarcharCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	auto &heap = StringVector::GetStringHeap(result);
	UnaryExecutor::Execute<string_t, string_t>(
	    source, result, count, [&](const string_t &input) -> string_t { return Geometry::ToString(heap, input); });
	return true;
}

static bool GeometryToBlobCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	Geometry::ToBinary(source, result);
	return true;
}

struct TryCastBlobToGeometry {
	template <class SRC, class DST>
	static bool Operation(SRC input, DST &result, Vector &result_vector, CastParameters &parameters) {
		try {
			return Geometry::FromBinary(input, result, StringVector::GetStringHeap(result_vector), true);
		} catch (InvalidInputException &ex) {
			ErrorData error(ex);
			HandleCastError::AssignError("Could not convert BLOB to GEOMETRY: " + error.RawMessage(), parameters);
			return false;
		}
	}
};

BoundCastInfo DefaultCasts::BlobToGeoCast(BindCastInput &input, const LogicalType &source, const LogicalType &target) {
	return BoundCastInfo(&VectorCastHelpers::TryCastStringLoop<string_t, string_t, TryCastBlobToGeometry>);
}

BoundCastInfo DefaultCasts::GeoCastSwitch(BindCastInput &input, const LogicalType &source, const LogicalType &target) {
	// now switch on the result type
	switch (target.id()) {
	case LogicalTypeId::VARCHAR:
		return GeometryToVarcharCast;
	case LogicalTypeId::BLOB:
		return GeometryToBlobCast;
	case LogicalTypeId::GEOMETRY: {
		// If the coordinate reference systems differ, we may not be able to cast
		if (GeoType::HasCRS(source) && GeoType::HasCRS(target)) {
			auto &target_crs = GeoType::GetCRS(target);
			auto &source_crs = GeoType::GetCRS(source);

			// TODO: Use client context (and allow extensions) to determine if the cast is okay (crs's are equivalent)

			if (!source_crs.Equals(target_crs)) {
				throw BinderException("Cannot cast GEOMETRY with CRS '" + source_crs.GetIdentifier() +
				                      "' to GEOMETRY with different CRS '" + target_crs.GetIdentifier() + "'");
			}
		}
		// The actual data representation stays the same, so we can do a reinterpret cast
		return DefaultCasts::ReinterpretCast;
	}
	default:
		return TryVectorNullCast;
	}
}

} // namespace duckdb
