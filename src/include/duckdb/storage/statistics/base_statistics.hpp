//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/statistics/base_statistics.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/common/operator/comparison_operators.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/storage/storage_index.hpp"

#include "duckdb/storage/statistics/numeric_stats.hpp"
#include "duckdb/storage/statistics/string_stats.hpp"
#include "duckdb/storage/statistics/list_stats.hpp"
#include "duckdb/storage/statistics/geometry_stats.hpp"
#include "duckdb/storage/statistics/variant_stats.hpp"
#include "duckdb/storage/statistics/stats_merge_type.hpp"

namespace duckdb {
struct SelectionVector;

class Serializer;
class Deserializer;

class Vector;

enum class StatsInfo : uint8_t {
	CAN_HAVE_NULL_VALUES = 0,
	CANNOT_HAVE_NULL_VALUES = 1,
	CAN_HAVE_VALID_VALUES = 2,
	CANNOT_HAVE_VALID_VALUES = 3,
	CAN_HAVE_NULL_AND_VALID_VALUES = 4
};

enum class StatisticsType : uint8_t {
	NUMERIC_STATS,
	STRING_STATS,
	LIST_STATS,
	STRUCT_STATS,
	BASE_STATS,
	ARRAY_STATS,
	GEOMETRY_STATS,
	VARIANT_STATS
};

struct ExtraStatsData {
	virtual ~ExtraStatsData() = default;

	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}
	template <class TARGET>
	const TARGET &Cast() const {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<const TARGET &>(*this);
	}
};

class BaseStatistics {
	friend struct NumericStats;
	friend struct StringStats;
	friend struct StructStats;
	friend struct ListStats;
	friend struct ArrayStats;
	friend struct GeometryStats;
	friend struct VariantStats;

public:
	DUCKDB_API ~BaseStatistics();
	// disable copy constructors
	BaseStatistics(const BaseStatistics &other) = delete;
	BaseStatistics &operator=(const BaseStatistics &) = delete;
	//! enable move constructors
	DUCKDB_API BaseStatistics(BaseStatistics &&other) noexcept;
	DUCKDB_API BaseStatistics &operator=(BaseStatistics &&) noexcept;

public:
	//! Creates a set of statistics for data that is unknown, i.e. "has_null" is true, "has_no_null" is true, etc
	//! This can be used in case nothing is known about the data - or can be used as a baseline when only a few things
	//! are known
	static BaseStatistics CreateUnknown(LogicalType type);
	//! Creates statistics for an empty database, i.e. "has_null" is false, "has_no_null" is false, etc
	//! This is used when incrementally constructing statistics by constantly adding new values
	static BaseStatistics CreateEmpty(LogicalType type);

	DUCKDB_API StatisticsType GetStatsType() const;
	DUCKDB_API static StatisticsType GetStatsType(const LogicalType &type);

	DUCKDB_API bool CanHaveNull() const;
	DUCKDB_API bool CanHaveNoNull() const;

	void SetDistinctCount(idx_t distinct_count);

	bool IsConstant() const;

	const LogicalType &GetType() const {
		return type;
	}

	void Set(StatsInfo info);
	void CombineValidity(const BaseStatistics &left, const BaseStatistics &right);
	void CopyValidity(const BaseStatistics &stats);
	//! Set that the CURRENT level can have null values
	//! Note that this is not correct for nested types unless this information is propagated in a different manner
	//! Use Set(StatsInfo::CAN_HAVE_NULL_VALUES) in the general case
	inline void SetHasNullFast() {
		has_null = true;
	}
	//! Set that the CURRENT level can have valid values
	//! Note that this is not correct for nested types unless this information is propagated in a different manner
	//! Use Set(StatsInfo::CAN_HAVE_VALID_VALUES) in the general case
	inline void SetHasNoNullFast() {
		has_no_null = true;
	}
	void SetHasNull();
	void SetHasNoNull();

	void Merge(const BaseStatistics &other, StatsMergeType merge_type = StatsMergeType::MERGE_STATS);

	void Copy(const BaseStatistics &other);
	void ResetAdditiveStatistics();

	unique_ptr<BaseStatistics> PushdownExtract(const StorageIndex &index) const;
	BaseStatistics Copy() const;
	unique_ptr<BaseStatistics> ToUnique() const;
	void CopyBase(const BaseStatistics &orig);

	void Serialize(Serializer &serializer) const;
	static BaseStatistics Deserialize(Deserializer &deserializer);

	//! Verify that a vector does not violate the statistics
	void Verify(const Vector &vector, const SelectionVector &sel, idx_t count, bool ignore_has_null = false) const;
	void Verify(const Vector &vector, idx_t count) const;

	//! The statistics as a struct, by default the rendering stats() shows. With exact, the statistics as they are:
	//! string bounds are VARCHAR / BLOB values holding their bytes instead of text with \xNN escapes, and geometry
	//! statistics list the geometry types the column holds
	Value ToStruct(bool exact = false) const;
	string ToString() const;
	//! Sets the parts of the statistics a struct (or VARIANT object) in the shape ToStruct produces describes;
	//! `path` prefixes the field names in error messages
	DUCKDB_API void SetFromStruct(const Value &value, const string &path = string());

	idx_t GetDistinctCount();
	static BaseStatistics FromConstant(const Value &input);

	template <class T>
	void UpdateNumericStats(T new_value) {
		D_ASSERT(GetStatsType() == StatisticsType::NUMERIC_STATS);
		NumericStats::Update(stats_union.numeric_data, new_value);
	}

private:
	BaseStatistics();
	explicit BaseStatistics(LogicalType type);

	static void Construct(BaseStatistics &stats, LogicalType type);

	void InitializeUnknown();
	void InitializeEmpty();

	static BaseStatistics CreateUnknownType(LogicalType type);
	static BaseStatistics CreateEmptyType(LogicalType type);
	static BaseStatistics FromConstantType(const Value &input);

private:
	//! The type of the logical segment
	LogicalType type = LogicalType::INVALID;
	//! Whether or not the segment can contain NULL values
	bool has_null;
	//! Whether or not the segment can contain values that are not null
	bool has_no_null;
	//! estimate that one may have even if distinct_stats==nullptr
	idx_t distinct_count;
	//! Numeric and String stats
	union {
		//! Numeric stats data, for numeric stats
		NumericStatsData numeric_data;
		//! String stats data, for string stats
		StringStatsData string_data;
		//! List stats data, for list stats
		ListStatsData list_data;
		//! Geometry stats data, for geometry stats
		GeometryStatsData geometry_data;
		//! Variant stats data, for variant stats
		VariantStatsData variant_data;
	} stats_union;
	//! Extra stats data, used for e.g. string data if required
	unique_ptr<ExtraStatsData> extra_data;
	//! Child stats (for LIST and STRUCT)
	unsafe_unique_array<BaseStatistics> child_stats;
};

template <>
inline void BaseStatistics::UpdateNumericStats<list_entry_t>(list_entry_t new_value) {
}

} // namespace duckdb
