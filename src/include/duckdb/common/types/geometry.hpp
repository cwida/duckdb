//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/types/geometry.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/pair.hpp"
#include "duckdb/common/query_location.hpp"
#include "duckdb/storage/storage_info.hpp"
#include <limits>
#include <cmath>

namespace duckdb {

struct GeometryStatsData;
class StringHeap;

enum class GeometryType : uint8_t {
	INVALID = 0,
	POINT = 1,
	LINESTRING = 2,
	POLYGON = 3,
	MULTIPOINT = 4,
	MULTILINESTRING = 5,
	MULTIPOLYGON = 6,
	GEOMETRYCOLLECTION = 7,
};

enum class VertexType : uint8_t { XY = 0, XYZ = 1, XYM = 2, XYZM = 3 };

//! How coordinates are interpreted when computing and comparing extents
enum class GeometryModel : uint8_t {
	//! Exact vertex min/max on a plane (GEOMETRY)
	PLANAR = 0,
	//! Coverage extent on the sphere, with an antimeridian-aware longitude axis (GEOGRAPHY)
	GEODETIC = 1,
};

struct VertexXY {
	static constexpr auto TYPE = VertexType::XY;
	static constexpr auto HAS_Z = false;
	static constexpr auto HAS_M = false;
	static constexpr auto WIDTH = 2;
	using STRUCT_TYPE = VectorStructType<double, double>;

	double x;
	double y;

	bool AllNan() const {
		return std::isnan(x) && std::isnan(y);
	}
};

struct VertexXYZ {
	static constexpr auto TYPE = VertexType::XYZ;
	static constexpr auto HAS_Z = true;
	static constexpr auto HAS_M = false;
	static constexpr auto WIDTH = 3;
	using STRUCT_TYPE = VectorStructType<double, double, double>;

	double x;
	double y;
	double z;

	bool AllNan() const {
		return std::isnan(x) && std::isnan(y) && std::isnan(z);
	}
};
struct VertexXYM {
	static constexpr auto TYPE = VertexType::XYM;
	static constexpr auto HAS_M = true;
	static constexpr auto HAS_Z = false;
	static constexpr auto WIDTH = 3;
	using STRUCT_TYPE = VectorStructType<double, double, double>;

	double x;
	double y;
	double m;

	bool AllNan() const {
		return std::isnan(x) && std::isnan(y) && std::isnan(m);
	}
};

struct VertexXYZM {
	static constexpr auto TYPE = VertexType::XYZM;
	static constexpr auto HAS_Z = true;
	static constexpr auto HAS_M = true;
	static constexpr auto WIDTH = 4;
	using STRUCT_TYPE = VectorStructType<double, double, double, double>;

	double x;
	double y;
	double z;
	double m;

	bool AllNan() const {
		return std::isnan(x) && std::isnan(y) && std::isnan(z) && std::isnan(m);
	}
};

class GeometryExtent {
public:
	static constexpr auto UNKNOWN_MIN = -std::numeric_limits<double>::infinity();
	static constexpr auto UNKNOWN_MAX = +std::numeric_limits<double>::infinity();

	static constexpr auto EMPTY_MIN = +std::numeric_limits<double>::infinity();
	static constexpr auto EMPTY_MAX = -std::numeric_limits<double>::infinity();

	// "Unknown" extent means we don't know the bounding box.
	// Merging with an unknown extent results in an unknown extent.
	// Everything intersects with an unknown extent.
	static GeometryExtent Unknown() {
		return GeometryExtent {UNKNOWN_MIN, UNKNOWN_MIN, UNKNOWN_MIN, UNKNOWN_MIN,
		                       UNKNOWN_MAX, UNKNOWN_MAX, UNKNOWN_MAX, UNKNOWN_MAX};
	}

	// "Empty" extent means the smallest possible bounding box.
	// Merging with an empty extent has no effect.
	// Nothing intersects with an empty extent.
	static GeometryExtent Empty() {
		return GeometryExtent {EMPTY_MIN, EMPTY_MIN, EMPTY_MIN, EMPTY_MIN, EMPTY_MAX, EMPTY_MAX, EMPTY_MAX, EMPTY_MAX};
	}

	// Does this extent have the X axis set?
	// In other words, is the range of the x-axis not empty and not unknown?
	bool HasX() const {
		return std::isfinite(x_min) && std::isfinite(x_max);
	}
	// Does this extent have the Y axis set?
	// In other words, is the range of the y-axis not empty and not unknown?
	bool HasY() const {
		return std::isfinite(y_min) && std::isfinite(y_max);
	}
	// Does this extent have both X and Y axes set?
	// In other words, are the ranges of both the x and y axes not empty and not unknown?
	// Used to gate serialization, where a non-finite axis cannot be represented.
	bool HasXY() const {
		return HasX() && HasY();
	}
	// Can this extent be used for X/Y zonemap pruning?
	// A single finite axis is enough: an unknown axis is treated as an infinite range,
	// which intersects everything, so pruning simply degrades to the finite axis.
	bool CanPruneXY() const {
		return HasX() || HasY();
	}
	// Does this extent have any Z values set?
	// In other words, is the range of the Z-axis not empty and not unknown?
	bool HasZ() const {
		return std::isfinite(z_min) && std::isfinite(z_max);
	}
	// Does this extent have any M values set?
	// In other words, is the range of the M-axis not empty and not unknown?
	bool HasM() const {
		return std::isfinite(m_min) && std::isfinite(m_max);
	}

	// NOTE: planar (GEOMETRY) extents are exact vertex min/max. Geodetic (GEOGRAPHY) extents are
	// *coverage* extents: Geometry::GetExtent additionally bounds the geodesic edges between the
	// vertices (great-circle arcs bulge poleward beyond their endpoints) and the polygon interiors
	// under the interior-on-the-left rule (S2's convention): a shell winding east around the globe
	// encloses the north pole, one winding west the south pole, a shell that is counterclockwise on
	// the sphere encloses its bounded side, and a clockwise one encloses the complement - covering
	// (almost) the whole globe, so ESRI-style clockwise shells get full-globe extents. Orientation is
	// decided on the sphere, not in planar lon/lat. This follows the precedent of other spherical
	// engines that write coverage bounds into Parquet geospatial statistics; the bounds are wider than
	// the Parquet spec's coordinate-based bbox definition, which is safe for any reader.
	//
	// The orientation rule is a convention, not a standard. The Parquet GEOGRAPHY spec says nothing
	// about ring orientation, OGC Simple Features only describes it for planar polygons, and
	// GeoParquet's optional `orientation` metadata only RECOMMENDS counterclockwise shells for
	// spherical edges, noting that readers otherwise typically take the smaller of the two regions.
	// BigQuery also takes the smaller region by default, and applies interior-on-the-left only with
	// `oriented => TRUE`. Under the smaller-region reading, a shell whose left side is larger than a
	// hemisphere (e.g. a ring along 30S winding east) has the other side as its interior, which these
	// extents do not cover. GEOGRAPHY predicates must therefore use the same rule as the extents.

	// GeometryModel::GEODETIC enables antimeridian-aware longitude (X axis) math used by GEOGRAPHY:
	// the X axis is treated as a circular [-180,180] degree axis where an arc may wrap (x_min > x_max),
	// meaning the longitude range is [x_min, 180] u [-180, x_max]. The Y axis is never wrapped.
	// GeometryModel::PLANAR is exact min/max, as used by GEOMETRY.
	//
	// The antimeridian seam is the only point identity the geodetic math models: +180 and -180 are
	// treated as the same longitude. The poles are NOT identified - at y = +/-90 every longitude is
	// physically the same point, but e.g. POINT(0 90) and POINT(90 90) still have disjoint coordinate
	// extents (consistent with the coordinate-extent contract above). A geometric predicate that
	// understands pole identity must account for this itself for pole-touching data.

	void Extend(const VertexXY &vertex, GeometryModel model = GeometryModel::PLANAR) {
		ExtendX(vertex.x, model);
		ExtendAxis(y_min, y_max, vertex.y);
	}

	void Extend(const VertexXYZ &vertex, GeometryModel model = GeometryModel::PLANAR) {
		ExtendX(vertex.x, model);
		ExtendAxis(y_min, y_max, vertex.y);
		ExtendAxis(z_min, z_max, vertex.z);
	}

	void Extend(const VertexXYM &vertex, GeometryModel model = GeometryModel::PLANAR) {
		ExtendX(vertex.x, model);
		ExtendAxis(y_min, y_max, vertex.y);
		ExtendAxis(m_min, m_max, vertex.m);
	}

	void Extend(const VertexXYZM &vertex, GeometryModel model = GeometryModel::PLANAR) {
		ExtendX(vertex.x, model);
		ExtendAxis(y_min, y_max, vertex.y);
		ExtendAxis(z_min, z_max, vertex.z);
		ExtendAxis(m_min, m_max, vertex.m);
	}

	// Extend the geodetic X (longitude) axis to cover the eastward arc from lo to hi
	void ExtendXArc(double lo, double hi) {
		if (!LonInRange(lo) || !LonInRange(hi)) {
			// NaN or out-of-contract longitudes make the whole axis unknown, as in ExtendX
			x_min = UNKNOWN_MIN;
			x_max = UNKNOWN_MAX;
			return;
		}
		if (!std::isfinite(x_min) || !std::isfinite(x_max)) {
			if (x_min == EMPTY_MIN) {
				x_min = lo;
				x_max = hi;
			}
			// Otherwise the X axis is unknown; keep it unknown.
			return;
		}
		LonArcMerge(x_min, x_max, lo, hi, x_min, x_max);
	}

	void Merge(const GeometryExtent &other, GeometryModel model = GeometryModel::PLANAR) {
		MergeAxis(y_min, y_max, other.y_min, other.y_max);
		MergeAxis(z_min, z_max, other.z_min, other.z_max);
		MergeAxis(m_min, m_max, other.m_min, other.m_max);

		// Only the X axis is special-cased for geodetic merging, and only when both arcs are finite.
		// A non-finite axis is "empty" or "unknown" and MergeAxis correctly propagates both.
		const bool both_finite =
		    std::isfinite(x_min) && std::isfinite(x_max) && std::isfinite(other.x_min) && std::isfinite(other.x_max);
		if (model == GeometryModel::GEODETIC && both_finite) {
			if (LonInRange(x_min) && LonInRange(x_max) && LonInRange(other.x_min) && LonInRange(other.x_max)) {
				LonArcMerge(x_min, x_max, other.x_min, other.x_max, x_min, x_max);
			} else {
				// Out-of-contract longitudes (only possible for data that bypassed validation): the arc
				// math is undefined for them, so degrade to unknown rather than risk an under-covering arc.
				x_min = UNKNOWN_MIN;
				x_max = UNKNOWN_MAX;
			}
		} else {
			MergeAxis(x_min, x_max, other.x_min, other.x_max);
		}
	}

	bool IntersectsXY(const GeometryExtent &other, GeometryModel model = GeometryModel::PLANAR) const {
		if (y_min > other.y_max || y_max < other.y_min) {
			return false;
		}
		const bool both_finite =
		    std::isfinite(x_min) && std::isfinite(x_max) && std::isfinite(other.x_min) && std::isfinite(other.x_max);
		if (model == GeometryModel::GEODETIC && both_finite) {
			if (!LonInRange(x_min) || !LonInRange(x_max) || !LonInRange(other.x_min) || !LonInRange(other.x_max)) {
				// Out-of-contract longitudes: cannot decide, err on the side of intersecting.
				return true;
			}
			// Widened by LON_EPSILON: a false "no intersection" here prunes rows, so round toward "intersects".
			return LonArcContains(x_min, x_max, other.x_min, LON_EPSILON) ||
			       LonArcContains(other.x_min, other.x_max, x_min, LON_EPSILON);
		}
		// Plain (and the non-finite empty/unknown cases): the inequalities handle +/- inf and NaN
		// conservatively (any NaN comparison is false, so NaN yields "intersects").
		return !(x_min > other.x_max || x_max < other.x_min);
	}

	bool IntersectsXYZM(const GeometryExtent &other) const {
		return !(x_min > other.x_max || x_max < other.x_min || y_min > other.y_max || y_max < other.y_min ||
		         z_min > other.z_max || z_max < other.z_min || m_min > other.m_max || m_max < other.m_min);
	}

	bool ContainsXY(const GeometryExtent &other, GeometryModel model = GeometryModel::PLANAR) const {
		if (!(y_min <= other.y_min && y_max >= other.y_max)) {
			return false;
		}
		const bool both_finite =
		    std::isfinite(x_min) && std::isfinite(x_max) && std::isfinite(other.x_min) && std::isfinite(other.x_max);
		if (model == GeometryModel::GEODETIC && both_finite) {
			if (!LonInRange(x_min) || !LonInRange(x_max) || !LonInRange(other.x_min) || !LonInRange(other.x_max)) {
				// Out-of-contract longitudes: cannot decide, err on the side of not containing.
				return false;
			}
			// Narrowed by LON_EPSILON: a false "contains" can elide a filter entirely, so round toward
			// "does not contain".
			return LonArcContainsArc(x_min, x_max, other.x_min, other.x_max, -LON_EPSILON);
		}
		return x_min <= other.x_min && x_max >= other.x_max;
	}

	// The X axis as a plain numeric [min, max] range over the raw coordinate values, e.g. for numeric
	// statistics of the extracted X ordinate. A geodetic arc is widened to the full range when it wraps
	// or touches the seam (a coordinate of either sign may be stored for the same longitude), and is
	// otherwise padded by LON_EPSILON to cover coordinates the rounded arc math absorbed.
	void GetNumericXRange(GeometryModel model, double &out_min, double &out_max) const {
		out_min = x_min;
		out_max = x_max;
		if (model != GeometryModel::GEODETIC || !HasX()) {
			return;
		}
		if (x_min > x_max || x_min == -180.0 || x_max == 180.0) {
			out_min = -180.0;
			out_max = 180.0;
			return;
		}
		out_min = MaxValue(x_min - LON_EPSILON, -180.0);
		out_max = MinValue(x_max + LON_EPSILON, 180.0);
	}

private:
	// The X axis of a geodetic extent is an eastward arc on the longitude circle: it covers the
	// longitudes traveled going east from x_min to x_max, so x_min > x_max means the arc crosses
	// the antimeridian. [-180, 180] is the canonical full-circle arc, and +180/-180 denote the same
	// physical longitude. Arc endpoints are always actual input coordinates (or +/-180 for the full
	// circle) and are never synthesized with arithmetic. The containment tests still round at ~1 ulp
	// of 360 degrees, so a merge can absorb a coordinate up to that far beyond an endpoint without
	// widening the arc: the public predicates guard this with LON_EPSILON in the safe direction, and
	// GetNumericXRange pads by it.
	static constexpr double LON_EPSILON = 1e-9; // ~0.1mm of longitude at the equator

	// Is this longitude within the canonical [-180, 180] range the arc math requires?
	static bool LonInRange(double x) {
		return x >= -180.0 && x <= 180.0;
	}

	//! A NaN coordinate cannot be placed on the axis, so it makes the range of the axis unknown
	static void ExtendAxis(double &min, double &max, double value) {
		if (std::isnan(value)) {
			min = UNKNOWN_MIN;
			max = UNKNOWN_MAX;
			return;
		}
		min = MinValue(min, value);
		max = MaxValue(max, value);
	}

	static void MergeAxis(double &min, double &max, double other_min, double other_max) {
		if (std::isnan(other_min) || std::isnan(other_max)) {
			min = UNKNOWN_MIN;
			max = UNKNOWN_MAX;
			return;
		}
		min = MinValue(min, other_min);
		max = MaxValue(max, other_max);
	}

	// Canonicalize the antimeridian seam: +180 and -180 are the same longitude.
	static double LonCanon(double x) {
		return x == 180.0 ? -180.0 : x;
	}

	// Eastward angular distance (degrees) from a to b, in [0, 360). Inputs finite, in [-180, 180].
	static double LonEastDist(double a, double b) {
		double d = LonCanon(b) - LonCanon(a);
		if (d < 0.0) {
			d += 360.0;
		}
		return d;
	}

	// Eastward width of the arc [lo, hi], in [0, 360]. Only the full circle [-180, 180] has width 360.
	static double LonArcWidth(double lo, double hi) {
		if (lo == -180.0 && hi == 180.0) {
			return 360.0;
		}
		return LonEastDist(lo, hi);
	}

	// Does the arc [lo, hi] contain the longitude p, with the given slack (may be negative)?
	static bool LonArcContains(double lo, double hi, double p, double slack = 0.0) {
		return LonEastDist(lo, p) <= LonArcWidth(lo, hi) + slack;
	}

	// Does the outer arc fully contain the inner arc, with the given slack (may be negative)?
	static bool LonArcContainsArc(double olo, double ohi, double ilo, double ihi, double slack = 0.0) {
		const double ow = LonArcWidth(olo, ohi);
		if (ow >= 360.0) {
			return true;
		}
		const double off = LonEastDist(olo, ilo);
		return off <= ow + slack && off + LonArcWidth(ilo, ihi) <= ow + slack;
	}

	// Merge two arcs into a covering arc, writing the result to out_lo/out_hi (which may alias the
	// inputs). The narrowest covering arc starts at one arc's start and ends at one arc's end: try
	// those four candidates with their exact endpoint values and keep the narrowest one that covers
	// both inputs. If none does (the arcs jointly cover the whole circle), the result is the full circle.
	static void LonArcMerge(double also, double ahi, double blo, double bhi, double &out_lo, double &out_hi) {
		const double cand_lo[4] = {also, also, blo, blo};
		const double cand_hi[4] = {ahi, bhi, ahi, bhi};
		double best_lo = -180.0;
		double best_hi = 180.0;
		double best_width = 360.0;
		for (idx_t i = 0; i < 4; i++) {
			if (!LonArcContainsArc(cand_lo[i], cand_hi[i], also, ahi) ||
			    !LonArcContainsArc(cand_lo[i], cand_hi[i], blo, bhi)) {
				continue;
			}
			const double width = LonArcWidth(cand_lo[i], cand_hi[i]);
			if (width < best_width) {
				best_width = width;
				best_lo = cand_lo[i];
				best_hi = cand_hi[i];
			}
		}
		out_lo = best_lo;
		out_hi = best_hi;
	}

	// Extend the X (longitude) axis to include x. For geodetic this uses circular arc math.
	void ExtendX(double x, GeometryModel model) {
		if (model == GeometryModel::PLANAR) {
			ExtendAxis(x_min, x_max, x);
			return;
		}
		if (std::isnan(x) || !LonInRange(x)) {
			// A NaN or out-of-contract longitude (only possible for data that bypassed validation)
			// makes the whole axis unknown.
			x_min = UNKNOWN_MIN;
			x_max = UNKNOWN_MAX;
			return;
		}
		if (!std::isfinite(x_min) || !std::isfinite(x_max)) {
			if (x_min == EMPTY_MIN) {
				// First vertex into an empty extent.
				x_min = x;
				x_max = x;
			}
			// Otherwise the X axis is "unknown" (infinite); keep it unknown.
			return;
		}
		LonArcMerge(x_min, x_max, x, x, x_min, x_max);
	}

public:
	double x_min;
	double y_min;
	double z_min;
	double m_min;

	double x_max;
	double y_max;
	double z_max;
	double m_max;
};

enum class GeometryStorageType : uint8_t {

	SPATIAL = 0,
	WKB = 1,

	// Base: 16
	POINT_XY = 17,
	LINESTRING_XY = 18,
	POLYGON_XY = 19,
	MULTIPOINT_XY = 20,
	MULTILINESTRING_XY = 21,
	MULTIPOLYGON_XY = 22,

	// Base: 32
	POINT_XYZ = 33,
	LINESTRING_XYZ = 34,
	POLYGON_XYZ = 35,
	MULTIPOINT_XYZ = 36,
	MULTILINESTRING_XYZ = 37,
	MULTIPOLYGON_XYZ = 38,

	// Base: 64
	POINT_XYM = 65,
	LINESTRING_XYM = 66,
	POLYGON_XYM = 67,
	MULTIPOINT_XYM = 68,
	MULTILINESTRING_XYM = 69,
	MULTIPOLYGON_XYM = 70,

	// Base: 96
	POINT_XYZM = 97,
	LINESTRING_XYZM = 98,
	POLYGON_XYZM = 99,
	MULTIPOINT_XYZM = 100,
	MULTILINESTRING_XYZM = 101,
	MULTIPOLYGON_XYZM = 102,
};

class Geometry {
public:
	static constexpr idx_t MAX_RECURSION_DEPTH = 16;
	static constexpr StorageVersion VERSION_ADDED = StorageVersion::V1_5_0; // Added to core in DuckDB v1.5.0
	//! GEOGRAPHY shares the WKB representation, but was only added to core in DuckDB v2.1.0
	static constexpr StorageVersion GEOGRAPHY_VERSION_ADDED = StorageVersion::V2_1_0;

	//! Check for legayc geometry type (pre v1.5)
	static bool IsSpatialGeometryType(const LogicalType &type);
	//! Get legacy geometry type (pre v1.5)
	static LogicalType GetSpatialGeometryType();

	//! Convert from WKT, returns false and sets error_message if the text is not valid WKT
	DUCKDB_API static bool TryFromString(const string_t &wkt_text, string_t &result, StringHeap &heap,
	                                     string &error_message);
	//! Convert from WKT
	DUCKDB_API static bool FromString(const string_t &wkt_text, string_t &result, StringHeap &heap, bool strict,
	                                  QueryLocation query_location);
	DUCKDB_API static bool FromString(const string_t &wkt_text, string_t &result, Vector &result_vector, bool strict);

	//! Convert to WKT
	DUCKDB_API static string_t ToString(StringHeap &heap, const string_t &geom);

	//! Convert from WKB
	DUCKDB_API static bool FromBinary(const string_t &wkb, string_t &result, StringHeap &heap, bool strict);
	DUCKDB_API static bool FromBinary(const Vector &source, Vector &result, idx_t count, bool strict);

	//! Convert to WKB
	DUCKDB_API static void ToBinary(const Vector &source, Vector &result);

	//! Get the geometry type and vertex type from the WKB
	DUCKDB_API static pair<GeometryType, VertexType> GetType(const string_t &wkb);

	//! The geometry model used for statistics and extent predicates of the given GEOMETRY or GEOGRAPHY type
	static GeometryModel GetModel(const LogicalType &type) {
		return type.id() == LogicalTypeId::GEOGRAPHY ? GeometryModel::GEODETIC : GeometryModel::PLANAR;
	}

	//! Update the bounding box, return number of vertices processed
	DUCKDB_API static uint32_t GetExtent(const string_t &wkb, GeometryExtent &extent,
	                                     GeometryModel model = GeometryModel::PLANAR);
	DUCKDB_API static uint32_t GetExtent(const string_t &wkb, GeometryExtent &extent, bool &has_any_empty,
	                                     GeometryModel model = GeometryModel::PLANAR);
	//! Whether the geometry's coordinates lie within the canonical GEOGRAPHY ranges
	//! (X in [-180, 180], Y in [-90, 90]). Empty geometries are always valid.
	DUCKDB_API static bool IsValidGeography(const string_t &wkb);

	//! Convert to vectorized format
	DUCKDB_API static void ToVectorizedFormat(const Vector &source, Vector &target, idx_t count, GeometryType geom_type,
	                                          VertexType vert_type);
	DUCKDB_API static void ToVectorizedFormat(const Vector &source, Vector &target, idx_t count,
	                                          GeometryStorageType type);
	//! Convert from vectorized format
	DUCKDB_API static void FromVectorizedFormat(const Vector &source, Vector &target, idx_t count,
	                                            GeometryType geom_type, VertexType vert_type, idx_t result_offset);
	DUCKDB_API static void FromVectorizedFormat(const Vector &source, Vector &target, idx_t count,
	                                            GeometryStorageType type, idx_t result_offset);

	//! Get the vectorized logical type for a given geometry and vertex type
	DUCKDB_API static LogicalType GetVectorizedType(GeometryStorageType type);
	DUCKDB_API static LogicalType GetVectorizedType(GeometryType geom_type, VertexType vert_type);

	DUCKDB_API static pair<GeometryType, VertexType> GetSpecializedType(GeometryStorageType type);

	DUCKDB_API static void FromSpatialGeometry(const string_t &source, string_t &target, Vector &vector);
	DUCKDB_API static void FromSpatialGeometry(const Vector &source, Vector &target, idx_t count, idx_t result_offset);
	DUCKDB_API static void FromSpatialGeometry(const string_t &source, string &target);

	DUCKDB_API static void ToSpatialGeometry(const string_t &source, string_t &target, Vector &vector);
	DUCKDB_API static void ToSpatialGeometry(const Vector &source, Vector &target, idx_t count);
	DUCKDB_API static void ToSpatialGeometry(const string_t &source, string &target);
};

} // namespace duckdb
