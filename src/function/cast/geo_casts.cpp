#include "duckdb/common/types/geometry.hpp"
#include "duckdb/common/types/geometry_crs.hpp"
#include "duckdb/function/cast/default_casts.hpp"
#include "duckdb/function/cast/vector_cast_helpers.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/optional.hpp"

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

// BLOB -> GEOGRAPHY additionally validates the canonical coordinate ranges, like ST_GeogFromWKB
struct TryCastBlobToGeography {
	template <class SRC, class DST>
	static bool Operation(SRC input, DST &result, Vector &result_vector, CastParameters &parameters) {
		try {
			if (!Geometry::FromBinary(input, result, StringVector::GetStringHeap(result_vector), true)) {
				return false;
			}
		} catch (InvalidInputException &ex) {
			ErrorData error(ex);
			HandleCastError::AssignError("Could not convert BLOB to GEOGRAPHY: " + error.RawMessage(), parameters);
			return false;
		}
		if (!Geometry::IsValidGeography(result)) {
			HandleCastError::AssignError("Could not convert BLOB to GEOGRAPHY: coordinates are outside the canonical "
			                             "ranges (longitude/X must be within [-180, 180], latitude/Y within [-90, 90])",
			                             parameters);
			return false;
		}
		return true;
	}
};

BoundCastInfo DefaultCasts::BlobToGeoCast(BindCastInput &input, const LogicalType &source, const LogicalType &target) {
	if (target.id() == LogicalTypeId::GEOGRAPHY) {
		return BoundCastInfo(&VectorCastHelpers::TryCastStringLoop<string_t, string_t, TryCastBlobToGeography>);
	}
	return BoundCastInfo(&VectorCastHelpers::TryCastStringLoop<string_t, string_t, TryCastBlobToGeometry>);
}

// Validating reinterpret cast used for GEOMETRY -> GEOGRAPHY: the WKB representation is unchanged, but every
// geometry must lie within the canonical GEOGRAPHY coordinate ranges. Out-of-range rows error (CAST) or NULL
// (TRY_CAST).
static bool GeometryToGeographyCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	bool success = true;
	UnaryExecutor::Execute<string_t, string_t>(source, result, count, [&](const string_t &input) -> optional<string_t> {
		if (!Geometry::IsValidGeography(input)) {
			HandleCastError::AssignError(
			    "Cannot cast GEOMETRY to GEOGRAPHY: coordinates are outside the canonical ranges "
			    "(longitude/X must be within [-180, 180], latitude/Y within [-90, 90])",
			    parameters);
			success = false;
			return optional<string_t>();
		}
		return optional<string_t>(input);
	});
	// The result strings alias the source WKB blobs, so keep the source heap alive.
	StringVector::AddHeapReference(result, source);
	return success;
}

// GEOMETRY/GEOGRAPHY only carry CRS metadata; a CRS-erasing or CRS-preserving cast keeps the coordinates intact.
// We only reject when both sides carry an explicit, non-equal CRS.
static void CheckCRSCompatible(const LogicalType &source, const LogicalType &target) {
	if (GeoType::HasCRS(source) && GeoType::HasCRS(target)) {
		auto &source_crs = GeoType::GetCRS(source);
		auto &target_crs = GeoType::GetCRS(target);
		if (!source_crs.Equals(target_crs)) {
			const auto kind_name = [](const LogicalType &type) {
				return type.id() == LogicalTypeId::GEOGRAPHY ? "GEOGRAPHY" : "GEOMETRY";
			};
			throw BinderException("Cannot cast %s with CRS '%s' to %s with different CRS '%s'", kind_name(source),
			                      source_crs.GetIdentifier(), kind_name(target), target_crs.GetIdentifier());
		}
	}
}

BoundCastInfo DefaultCasts::GeoCastSwitch(BindCastInput &input, const LogicalType &source, const LogicalType &target) {
	// Dispatched when the source is GEOMETRY or GEOGRAPHY. Switch on the target type.
	switch (target.id()) {
	case LogicalTypeId::VARCHAR:
		return GeometryToVarcharCast;
	case LogicalTypeId::BLOB:
		return GeometryToBlobCast;
	case LogicalTypeId::GEOMETRY:
	case LogicalTypeId::GEOGRAPHY: {
		CheckCRSCompatible(source, target);
		if (source.id() == LogicalTypeId::GEOMETRY && target.id() == LogicalTypeId::GEOGRAPHY) {
			// Explicit GEOMETRY -> GEOGRAPHY: validate that coordinates are within the canonical ranges.
			return GeometryToGeographyCast;
		}
		// GEOMETRY -> GEOMETRY, GEOGRAPHY -> GEOGRAPHY, and GEOGRAPHY -> GEOMETRY: the data representation is
		// identical, so we can reinterpret. (GEOGRAPHY -> GEOMETRY needs no validation.)
		return DefaultCasts::ReinterpretCast;
	}
	default:
		return TryVectorNullCast;
	}
}

} // namespace duckdb
