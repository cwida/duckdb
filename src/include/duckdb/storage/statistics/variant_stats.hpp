//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/statistics/variant_stats.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/variant.hpp"
#include "duckdb/common/types/selection_vector.hpp"
#include "duckdb/storage/storage_index.hpp"
#include "duckdb/common/column_index.hpp"

namespace duckdb {
class BaseStatistics;
class Value;

enum class VariantStatsShreddingState : uint8_t {
	//! Uninitialized, not unshredded/shredded
	UNINITIALIZED,
	//! No shredding applied yet
	NOT_SHREDDED,
	//! Shredded consistently
	SHREDDED,
	//! Result from combining incompatible shreddings
	INCONSISTENT
};

struct VariantStatsData {
	//! Whether the VARIANT is stored in shredded form
	VariantStatsShreddingState shredding_state;
};

struct VariantShreddedStats {
public:
	DUCKDB_API static bool IsFullyShredded(const BaseStatistics &stats);
	DUCKDB_API static optional_ptr<const BaseStatistics> FindChildStats(const BaseStatistics &stats,
	                                                                    const VariantPathComponent &component);
};

//! VARIANT as a type can hold arbitrarily typed values within the same column.
//! In storage, we apply shredding to the VARIANT column, this means that we find the most common type among all these
//! values. And for those values we store them separately from the rest of the values, in a structured way (like you
//! would store any other column).
struct VariantStats {
public:
	static constexpr idx_t TYPED_VALUE_INDEX = 0;
	static constexpr idx_t UNTYPED_VALUE_INDEX = 1;

public:
	DUCKDB_API static void Construct(BaseStatistics &stats);

public:
	DUCKDB_API static BaseStatistics CreateUnknown(LogicalType type);
	DUCKDB_API static BaseStatistics CreateEmpty(LogicalType type);
	DUCKDB_API static BaseStatistics CreateShredded(const LogicalType &shredded_type);
	//! Propagate statistics through a cast to VARIANT - builds fully-shredded VARIANT statistics describing
	//! a (possibly nested) non-variant value of `source_type` with statistics `child_stats`.
	//! Returns nullptr when the type can not be represented as a single consistent shredding.
	DUCKDB_API static unique_ptr<BaseStatistics> StatisticsPropagateToVariant(const LogicalType &source_type,
	                                                                          const BaseStatistics &child_stats);

public:
	//! Stats related to the 'unshredded' column, which holds all data that doesn't fit in the structure of the shredded
	//! column (if IsShredded())
	DUCKDB_API static void CreateUnshreddedStats(BaseStatistics &stats);
	DUCKDB_API static const BaseStatistics &GetUnshreddedStats(const BaseStatistics &stats);
	DUCKDB_API static BaseStatistics &GetUnshreddedStats(BaseStatistics &stats);

	//! Returns the typed_value stats of a shredded stats entry
	DUCKDB_API static const BaseStatistics &GetTypedStats(const BaseStatistics &stats);
	static const BaseStatistics &GetTypedStats(const BaseStatistics &&stats) = delete;

	//! Returns the untyped_value_index stats of a shredded stats entry - if there is any
	DUCKDB_API static optional_ptr<const BaseStatistics> GetUntypedStats(const BaseStatistics &stats);
	static optional_ptr<const BaseStatistics> GetUntypedStats(const BaseStatistics &&stats) = delete;

	DUCKDB_API static void SetUnshreddedStats(BaseStatistics &stats, unique_ptr<BaseStatistics> new_stats);
	DUCKDB_API static void SetUnshreddedStats(BaseStatistics &stats, const BaseStatistics &new_stats);
	DUCKDB_API static void MarkAsNotShredded(BaseStatistics &stats);

public:
	//! Stats related to the 'shredded' column, which holds all structured data created during shredding
	//! Returns the LogicalType that represents the shredding as a single DuckDB LogicalType (i.e STRUCT(col1 VARCHAR))
	DUCKDB_API static LogicalType GetShreddedStructuredType(const BaseStatistics &stats);
	//! Wraps each level of structured_type in typed_value
	DUCKDB_API static LogicalType GetShreddingType(const LogicalType &structured_type);
	DUCKDB_API static void CreateShreddedStats(BaseStatistics &stats, const LogicalType &shredded_type);
	DUCKDB_API static bool IsShredded(const BaseStatistics &stats);
	//! Determine if a given path inside the variant stats is shredded
	DUCKDB_API static bool IsShredded(const BaseStatistics &stats, const ColumnIndex &column_index);
	DUCKDB_API static const BaseStatistics &GetShreddedStats(const BaseStatistics &stats);
	DUCKDB_API static BaseStatistics &GetShreddedStats(BaseStatistics &stats);

	DUCKDB_API static void SetShreddedStats(BaseStatistics &stats, unique_ptr<BaseStatistics> new_stats);
	DUCKDB_API static void SetShreddedStats(BaseStatistics &stats, const BaseStatistics &new_stats);

	DUCKDB_API static bool MergeShredding(const BaseStatistics &stats, const BaseStatistics &other,
	                                      BaseStatistics &new_stats);
	DUCKDB_API static unique_ptr<BaseStatistics> WrapExtractedFieldAsVariant(const BaseStatistics &base_variant,
	                                                                         const BaseStatistics &extracted_field);

public:
	DUCKDB_API static void Serialize(const BaseStatistics &stats, Serializer &serializer);
	DUCKDB_API static void Deserialize(Deserializer &deserializer, BaseStatistics &base);

	DUCKDB_API static child_list_t<Value> ToStruct(const BaseStatistics &stats);

	DUCKDB_API static void Merge(BaseStatistics &stats, const BaseStatistics &other);
	DUCKDB_API static void Verify(const BaseStatistics &stats, const Vector &vector, const SelectionVector &sel,
	                              idx_t count);
	DUCKDB_API static void Copy(BaseStatistics &stats, const BaseStatistics &other);
	DUCKDB_API static unique_ptr<BaseStatistics> PushdownExtract(const BaseStatistics &stats,
	                                                             const StorageIndex &index);

private:
	static VariantStatsData &GetDataUnsafe(BaseStatistics &stats);
	static const VariantStatsData &GetDataUnsafe(const BaseStatistics &stats);
};

} // namespace duckdb
