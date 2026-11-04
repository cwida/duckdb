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

	void Extend(const VertexXY &vertex) {
		ExtendAxis(x_min, x_max, vertex.x);
		ExtendAxis(y_min, y_max, vertex.y);
	}

	void Extend(const VertexXYZ &vertex) {
		ExtendAxis(x_min, x_max, vertex.x);
		ExtendAxis(y_min, y_max, vertex.y);
		ExtendAxis(z_min, z_max, vertex.z);
	}

	void Extend(const VertexXYM &vertex) {
		ExtendAxis(x_min, x_max, vertex.x);
		ExtendAxis(y_min, y_max, vertex.y);
		ExtendAxis(m_min, m_max, vertex.m);
	}

	void Extend(const VertexXYZM &vertex) {
		ExtendAxis(x_min, x_max, vertex.x);
		ExtendAxis(y_min, y_max, vertex.y);
		ExtendAxis(z_min, z_max, vertex.z);
		ExtendAxis(m_min, m_max, vertex.m);
	}

	void Merge(const GeometryExtent &other) {
		MergeAxis(x_min, x_max, other.x_min, other.x_max);
		MergeAxis(y_min, y_max, other.y_min, other.y_max);
		MergeAxis(z_min, z_max, other.z_min, other.z_max);
		MergeAxis(m_min, m_max, other.m_min, other.m_max);
	}

	bool IntersectsXY(const GeometryExtent &other) const {
		return !(x_min > other.x_max || x_max < other.x_min || y_min > other.y_max || y_max < other.y_min);
	}

	bool IntersectsXYZM(const GeometryExtent &other) const {
		return !(x_min > other.x_max || x_max < other.x_min || y_min > other.y_max || y_max < other.y_min ||
		         z_min > other.z_max || z_max < other.z_min || m_min > other.m_max || m_max < other.m_min);
	}

	bool ContainsXY(const GeometryExtent &other) const {
		return x_min <= other.x_min && x_max >= other.x_max && y_min <= other.y_min && y_max >= other.y_max;
	}

private:
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

	//! Update the bounding box, return number of vertices processed
	DUCKDB_API static uint32_t GetExtent(const string_t &wkb, GeometryExtent &extent);
	DUCKDB_API static uint32_t GetExtent(const string_t &wkb, GeometryExtent &extent, bool &has_any_empty);

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
