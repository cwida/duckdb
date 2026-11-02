//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/statistics/geometry_stats.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/enums/filter_propagate_result.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/geometry.hpp"

namespace duckdb {
class BaseStatistics;
struct SelectionVector;

class GeometryTypeSet {
public:
	static constexpr auto VERT_TYPES = 4;
	static constexpr auto PART_TYPES = 8;

	static GeometryTypeSet Unknown() {
		GeometryTypeSet result;
		for (idx_t i = 0; i < VERT_TYPES; i++) {
			result.sets[i] = 0xFF;
		}
		return result;
	}
	static GeometryTypeSet Empty() {
		GeometryTypeSet result;
		for (idx_t i = 0; i < VERT_TYPES; i++) {
			result.sets[i] = 0;
		}
		return result;
	}

	bool IsEmpty() const {
		for (idx_t i = 0; i < VERT_TYPES; i++) {
			if (sets[i] != 0) {
				return false;
			}
		}
		return true;
	}

	bool IsUnknown() const {
		for (idx_t i = 0; i < VERT_TYPES; i++) {
			if (sets[i] != 0xFF) {
				return false;
			}
		}
		return true;
	}

	void Add(GeometryType geom_type, VertexType vert_type) {
		const auto vert_idx = static_cast<uint8_t>(vert_type);
		const auto geom_idx = static_cast<uint8_t>(geom_type);
		D_ASSERT(vert_idx < VERT_TYPES);
		D_ASSERT(geom_idx < PART_TYPES);
		sets[vert_idx] |= (1 << geom_idx);
	}

	void Merge(const GeometryTypeSet &other) {
		for (idx_t i = 0; i < VERT_TYPES; i++) {
			sets[i] |= other.sets[i];
		}
	}

	void Clear() {
		for (idx_t i = 0; i < VERT_TYPES; i++) {
			sets[i] = 0;
		}
	}

	bool Has(GeometryType geom_type) const {
		const auto geom_idx = static_cast<uint8_t>(geom_type);
		D_ASSERT(geom_idx < PART_TYPES);
		for (idx_t i = 0; i < VERT_TYPES; i++) {
			if (sets[i] & (1 << geom_idx)) {
				return true;
			}
		}
		return false;
	}

	bool Has(VertexType vert_type) const {
		const auto vert_idx = static_cast<uint8_t>(vert_type);
		D_ASSERT(vert_idx < VERT_TYPES);
		return sets[vert_idx] != 0;
	}

	//! Check if only the given geometry and vertex type is present
	//! (all others are absent)
	bool HasOnly(GeometryType geom_type, VertexType vert_type) const {
		const auto vert_idx = static_cast<uint8_t>(vert_type);
		const auto geom_idx = static_cast<uint8_t>(geom_type);
		D_ASSERT(vert_idx < VERT_TYPES);
		D_ASSERT(geom_idx < PART_TYPES);
		for (uint8_t v_idx = 0; v_idx < VERT_TYPES; v_idx++) {
			for (uint8_t g_idx = 1; g_idx < PART_TYPES; g_idx++) {
				if (v_idx == vert_idx && g_idx == geom_idx) {
					if (!(sets[v_idx] & (1 << g_idx))) {
						return false;
					}
				} else {
					if (sets[v_idx] & (1 << g_idx)) {
						return false;
					}
				}
			}
		}
		return true;
	}

	//! Check if only the given geometry type is present (with any vertex type),
	//! and that at least one such geometry is present.
	bool HasOnly(GeometryType geom_type) const {
		const auto geom_idx = static_cast<uint8_t>(geom_type);
		D_ASSERT(geom_idx < PART_TYPES);
		bool found = false;
		for (uint8_t v_idx = 0; v_idx < VERT_TYPES; v_idx++) {
			for (uint8_t g_idx = 1; g_idx < PART_TYPES; g_idx++) {
				if (!(sets[v_idx] & (1 << g_idx))) {
					continue;
				}
				if (g_idx != geom_idx) {
					return false;
				}
				found = true;
			}
		}
		return found;
	}

	//! Check if only the given vertex type is present (with any geometry type),
	//! and that at least one such geometry is present.
	bool HasOnly(VertexType vert_type) const {
		const auto vert_idx = static_cast<uint8_t>(vert_type);
		D_ASSERT(vert_idx < VERT_TYPES);
		bool found = false;
		for (uint8_t v_idx = 0; v_idx < VERT_TYPES; v_idx++) {
			for (uint8_t g_idx = 1; g_idx < PART_TYPES; g_idx++) {
				if (!(sets[v_idx] & (1 << g_idx))) {
					continue;
				}
				if (v_idx != vert_idx) {
					return false;
				}
				found = true;
			}
		}
		return found;
	}

	bool HasSingleType() const {
		idx_t type_count = 0;
		for (uint8_t v_idx = 0; v_idx < VERT_TYPES; v_idx++) {
			for (uint8_t g_idx = 1; g_idx < PART_TYPES; g_idx++) {
				if (sets[v_idx] & (1 << g_idx)) {
					type_count++;
					if (type_count > 1) {
						return false;
					}
				}
			}
		}
		return type_count == 1;
	}

	bool TryGetSingleType(GeometryType &geom_type, VertexType &vert_type) const {
		auto result_geom = GeometryType::INVALID;
		auto result_vert = VertexType::XY;
		auto result_found = false;

		for (uint8_t v_idx = 0; v_idx < VERT_TYPES; v_idx++) {
			for (uint8_t g_idx = 1; g_idx < PART_TYPES; g_idx++) {
				if (sets[v_idx] & (1 << g_idx)) {
					if (result_found) {
						// Multiple types found
						return false;
					}
					result_found = true;
					result_geom = static_cast<GeometryType>(g_idx);
					result_vert = static_cast<VertexType>(v_idx);
				}
			}
		}
		if (result_found) {
			geom_type = result_geom;
			vert_type = result_vert;
		}
		return result_found;
	}

	void AddWKBType(int32_t wkb_type) {
		const auto vert_idx = static_cast<uint8_t>((wkb_type / 1000) % 10);
		const auto geom_idx = static_cast<uint8_t>(wkb_type % 1000);
		D_ASSERT(vert_idx < VERT_TYPES);
		D_ASSERT(geom_idx < PART_TYPES);
		sets[vert_idx] |= (1 << geom_idx);
	}

	vector<int32_t> ToWKBList() const {
		vector<int32_t> result;
		for (uint8_t vert_idx = 0; vert_idx < VERT_TYPES; vert_idx++) {
			for (uint8_t geom_idx = 1; geom_idx < PART_TYPES; geom_idx++) {
				if (sets[vert_idx] & (1 << geom_idx)) {
					result.push_back(geom_idx + vert_idx * 1000);
				}
			}
		}
		return result;
	}

	vector<string> ToString(bool snake_case) const;
	//! Add a type given its snake_case name
	DUCKDB_API bool TryAdd(const string &type_name);

	uint8_t sets[VERT_TYPES];
};

class GeometryStatsFlags {
public:
	// There are two types of "empty"
	// 1. Empty geometry: A geometry that contains no vertices (e.g., POINT EMPTY)
	// 2. Empty part: A geometry that contains at least one empty geometry (e.g., MULTIPOINT(POINT EMPTY, POINT(1 2)))

	static GeometryStatsFlags Unknown() {
		GeometryStatsFlags flags;
		flags.flags = 0xF; // All bits set
		return flags;
	}

	static GeometryStatsFlags Empty() {
		GeometryStatsFlags flags;
		flags.flags = 0x0; // No bits set
		return flags;
	}
	void Clear() {
		flags = 0x0;
	}
	bool HasEmptyGeometry() const {
		return (flags & HAS_EMPTY_GEOM) != 0;
	}
	bool HasNonEmptyGeometry() const {
		return (flags & HAS_NON_EMPTY_GEOM) != 0;
	}
	bool HasEmptyPart() const {
		return (flags & HAS_EMPTY_PART) != 0;
	}
	bool HasNonEmptyPart() const {
		return (flags & HAS_NON_EMPTY_PART) != 0;
	}
	void SetHasEmptyGeometry() {
		flags |= HAS_EMPTY_GEOM;
	}
	void SetHasNonEmptyGeometry() {
		flags |= HAS_NON_EMPTY_GEOM;
	}
	void SetHasEmptyPart() {
		flags |= HAS_EMPTY_PART;
	}
	void SetHasNonEmptyPart() {
		flags |= HAS_NON_EMPTY_PART;
	}

	void Merge(const GeometryStatsFlags &other) {
		flags |= other.flags;
	}

	uint8_t flags;

private:
	static constexpr auto HAS_EMPTY_GEOM = 0x1;
	static constexpr auto HAS_NON_EMPTY_GEOM = 0x2;
	static constexpr auto HAS_EMPTY_PART = 0x4;
	static constexpr auto HAS_NON_EMPTY_PART = 0x8;
};

struct GeometryStatsData {
	GeometryTypeSet types;
	GeometryExtent extent;
	GeometryStatsFlags flags;

	void SetEmpty() {
		types = GeometryTypeSet::Empty();
		extent = GeometryExtent::Empty();
		flags = GeometryStatsFlags::Empty();
	}

	void SetUnknown() {
		types = GeometryTypeSet::Unknown();
		extent = GeometryExtent::Unknown();
		flags = GeometryStatsFlags::Unknown();
	}

	void Merge(const GeometryStatsData &other) {
		types.Merge(other.types);
		extent.Merge(other.extent);
		flags.Merge(other.flags);
	}

	void Update(const string_t &geom_blob) {
		// Parse type
		const auto type_info = Geometry::GetType(geom_blob);
		types.Add(type_info.first, type_info.second);

		// Update extent
		bool has_any_empty = false;
		const auto vert_count = Geometry::GetExtent(geom_blob, extent, has_any_empty);

		// Update flags
		if (has_any_empty) {
			flags.SetHasEmptyPart();
		} else {
			flags.SetHasNonEmptyPart();
		}

		if (vert_count == 0) {
			flags.SetHasEmptyGeometry();
		} else {
			flags.SetHasNonEmptyGeometry();
		}
	}
};

struct GeometryStats {
	//! Unknown statistics
	DUCKDB_API static BaseStatistics CreateUnknown(LogicalType type);
	//! Empty statistics
	DUCKDB_API static BaseStatistics CreateEmpty(LogicalType type);

	DUCKDB_API static void Serialize(const BaseStatistics &stats, Serializer &serializer);
	DUCKDB_API static void Deserialize(Deserializer &deserializer, BaseStatistics &base);

	DUCKDB_API static child_list_t<Value> ToStruct(const BaseStatistics &stats);

	DUCKDB_API static void Update(BaseStatistics &stats, const string_t &value);
	DUCKDB_API static void Merge(BaseStatistics &stats, const BaseStatistics &other);
	DUCKDB_API static void Verify(const BaseStatistics &stats, const Vector &vector, const SelectionVector &sel,
	                              idx_t count);

	DUCKDB_API static GeometryExtent &GetExtent(BaseStatistics &stats);
	DUCKDB_API static const GeometryExtent &GetExtent(const BaseStatistics &stats);
	DUCKDB_API static GeometryTypeSet &GetTypes(BaseStatistics &stats);
	DUCKDB_API static const GeometryTypeSet &GetTypes(const BaseStatistics &stats);
	DUCKDB_API static GeometryStatsFlags &GetFlags(BaseStatistics &stats);
	DUCKDB_API static const GeometryStatsFlags &GetFlags(const BaseStatistics &stats);

	static GeometryStatsData &GetDataUnsafe(BaseStatistics &stats);
	static const GeometryStatsData &GetDataUnsafe(const BaseStatistics &stats);
};

} // namespace duckdb
