//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/statistics/string_stats.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/common/enums/filter_propagate_result.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/array_ptr.hpp"
#include "duckdb/storage/statistics/stats_merge_type.hpp"

namespace duckdb {
class BaseStatistics;
struct SelectionVector;
class Vector;
class Value;
class StatsStructFields;
template <class T>
struct StatsWriter;

enum class StringStatsType {
	NO_STATS,       // no min/max stats available at all
	EMPTY_STATS,    // min/max is empty
	EXACT_STATS,    // min/max is exact
	TRUNCATED_STATS // min/max is truncated, so the min/max values here might be wider than the values in the dataset
};

struct StringStatsData {
	constexpr static uint32_t CURRENT_MAX_STRING_MINMAX_SIZE = 12;
	constexpr static uint32_t LEGACY_MAX_STRING_MINMAX_SIZE = 8;
	constexpr static uint32_t MAXIMUM_MIN_STRING_LENGTH = (1 << 21U) - 1;

	//! The minimum value of the segment, potentially truncated depending on min_type
	string_t min;
	//! The maximum value of the segment, potentially truncated depending on max_type
	string_t max;
	//! Whether or not the column can contain unicode characters
	bool has_unicode;
	//! Whether or not the maximum string length is known
	bool has_max_string_length;
	//! The maximum string length in bytes
	uint32_t max_string_length;
	//! Whether or not the maximum string length is known
	bool has_min_string_length;
	//! The minimum string length in bytes
	uint32_t min_string_length;
	//! WHether or not the total string length is known
	bool has_total_string_length;
	//! The total (summed) length of all strings
	uint64_t total_string_length;
	//! Min stats type
	StringStatsType min_type;
	//! Max stats type
	StringStatsType max_type;

public:
	bool HasMinStringLength() const;
};

struct StringStats {
	//! Unknown statistics - i.e. "has_unicode" is true, "max_string_length" is unknown, "min" is \0, max is \xFF
	DUCKDB_API static BaseStatistics CreateUnknown(LogicalType type);
	//! Empty statistics - i.e. "has_unicode" is false, "max_string_length" is 0, "min" is \xFF, max is \x00
	DUCKDB_API static BaseStatistics CreateEmpty(LogicalType type);
	//! Returns true if the stats has both a min and max value defined
	DUCKDB_API static bool HasMinMax(const BaseStatistics &stats);
	DUCKDB_API static bool HasMin(const BaseStatistics &stats);
	DUCKDB_API static bool HasMax(const BaseStatistics &stats);
	//! Whether or not the statistics have a maximum string length defined
	DUCKDB_API static bool HasMaxStringLength(const BaseStatistics &stats);
	//! Returns the maximum string length, or throws an exception if !HasMaxStringLength()
	DUCKDB_API static uint32_t MaxStringLength(const BaseStatistics &stats);
	//! Returns the minimum string length if it is known
	DUCKDB_API static optional_idx MinStringLength(const BaseStatistics &stats);
	//! Returns the total string length if known
	DUCKDB_API static optional_idx TotalStringLength(const BaseStatistics &stats);
	//! Whether or not the strings can contain unicode
	DUCKDB_API static bool CanContainUnicode(const BaseStatistics &stats);
	//! Returns the min value
	DUCKDB_API static string Min(const BaseStatistics &stats);
	//! Returns the max value
	DUCKDB_API static string Max(const BaseStatistics &stats);
	//! Returns a valid UTF-8 lower bound for the min value, or NULL if stats have no min
	DUCKDB_API static Value TryGetValidMin(const BaseStatistics &stats);
	//! Returns a valid UTF-8 upper bound for the max value, or NULL if no finite bound exists
	DUCKDB_API static Value TryGetValidMax(const BaseStatistics &stats);

	//! Construct string stats from a constant
	DUCKDB_API static void FromConstant(BaseStatistics &stats, string_t input);

	DUCKDB_API static void MergeInConstant(BaseStatistics &stats, string_t input);

	[[deprecated("StringStats::Update is deprecated. Use StringStats::MergeInConstant for non-performance-sensitive "
	             "code, or StatsWriter<string_t> instead")]] DUCKDB_API static void
	Update(BaseStatistics &stats, const string_t &value);

	DUCKDB_API static StringStatsType GetMinType(const BaseStatistics &stats);
	DUCKDB_API static StringStatsType GetMaxType(const BaseStatistics &stats);

	//! Resets the max string length so HasMaxStringLength() is false
	DUCKDB_API static void ResetMaxStringLength(BaseStatistics &stats);
	//! Sets the max string length
	DUCKDB_API static void SetMaxStringLength(BaseStatistics &stats, uint32_t length);
	//! FIXME: make this part of Set on statistics
	DUCKDB_API static void SetContainsUnicode(BaseStatistics &stats);

	DUCKDB_API static void Serialize(const BaseStatistics &stats, Serializer &serializer);
	DUCKDB_API static void Deserialize(Deserializer &deserializer, BaseStatistics &base);

	DUCKDB_API static child_list_t<Value> ToStruct(const BaseStatistics &stats, bool exact);
	//! Sets the parts of the statistics the struct fields describe (the inverse of ToStruct)
	DUCKDB_API static void SetFromStruct(BaseStatistics &stats, StatsStructFields &fields);

	DUCKDB_API static FilterPropagateResult CheckZonemap(const BaseStatistics &stats, ExpressionType comparison_type,
	                                                     array_ptr<const Value> constants);
	DUCKDB_API static int8_t CompareStringStats(string_t input, string_t stats, StringStatsType type);
	DUCKDB_API static FilterPropagateResult CheckZonemap(string_t min, StringStatsType min_type, string_t max,
	                                                     StringStatsType max_type, ExpressionType comparison_type,
	                                                     string_t constant);

	[[deprecated("StringStats::SetMin without specifying StringStatsType is deprecated - specify either "
	             "StringStatsType::TRUNCATED_STATS or StringStatsType::EXACT_STATS")]] DUCKDB_API static void
	SetMin(BaseStatistics &stats, const string_t &value);
	[[deprecated("StringStats::SetMax without specifying StringStatsType is deprecated - specify either "
	             "StringStatsType::TRUNCATED_STATS or StringStatsType::EXACT_STATS")]] DUCKDB_API static void
	SetMax(BaseStatistics &stats, const string_t &value);
	DUCKDB_API static void SetMin(BaseStatistics &stats, const string_t &value, StringStatsType type);
	DUCKDB_API static void SetMax(BaseStatistics &stats, const string_t &value, StringStatsType type);
	DUCKDB_API static void Merge(BaseStatistics &stats, const BaseStatistics &other,
	                             StatsMergeType merge_type = StatsMergeType::MERGE_STATS);
	DUCKDB_API static void Merge(BaseStatistics &stats, const StatsWriter<string_t> &other);
	DUCKDB_API static void Merge(BaseStatistics &stats, const StringStatsData &other_data,
	                             StatsMergeType merge_type = StatsMergeType::MERGE_STATS);
	DUCKDB_API static void Verify(const BaseStatistics &stats, const Vector &vector, const SelectionVector &sel,
	                              idx_t count);
	DUCKDB_API static void Copy(BaseStatistics &stats, const BaseStatistics &other);

private:
	static StringStatsData &GetDataUnsafe(BaseStatistics &stats);
	static const StringStatsData &GetDataUnsafe(const BaseStatistics &stats);
	static string_t AssignString(BaseStatistics &stats, const string_t &input, bool is_min);
	static void MergeStats(BaseStatistics &stats, string_t &target, StringStatsType &target_type,
	                       const string_t &source, StringStatsType source_type, bool is_min);
};

} // namespace duckdb
