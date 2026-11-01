#pragma once

#include "duckdb/common/algorithm.hpp"
#include "duckdb/common/types/variant.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/types/selection_vector.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/function/scalar/variant_utils.hpp"

#include <utility>

namespace duckdb {

//! An object field is only worth shredding if it occurs in at least 1/VARIANT_SHREDDED_FIELD_PRESENCE_DIVISOR of the
//! objects it belongs to - rarer fields cost more shred type than they save in the unshredded overlay.
constexpr idx_t VARIANT_SHREDDED_FIELD_PRESENCE_DIVISOR = 100;

//! At most this many fields are shredded per object - every shredded field adds a column, so a long tail of
//! moderately frequent fields costs more to read and write than it saves.
constexpr idx_t VARIANT_SHREDDED_FIELD_LIMIT = 32;

//! Whether a field occurring 'field_count' times in 'object_count' objects is frequent enough to shred on
inline bool VariantFieldIsWorthShredding(idx_t field_count, idx_t object_count) {
	return field_count * VARIANT_SHREDDED_FIELD_PRESENCE_DIVISOR >= object_count;
}

//! Returns the names of the fields to shred on for an object that occurs 'object_count' times.
//! Fields below the presence threshold are dropped, and only the VARIANT_SHREDDED_FIELD_LIMIT most frequent fields
//! are kept. Fields are returned ordered by decreasing occurrence, ties broken by name so the result is stable.
inline vector<string> VariantSelectShreddedFields(vector<pair<string, idx_t>> fields, idx_t object_count) {
	vector<pair<string, idx_t>> selected;
	for (auto &field : fields) {
		if (field.first.empty()) {
			// a STRUCT field cannot have an empty name, so the empty key stays in the 'value'
			continue;
		}
		if (!VariantFieldIsWorthShredding(field.second, object_count)) {
			continue;
		}
		selected.push_back(std::move(field));
	}
	std::sort(selected.begin(), selected.end(), [](const pair<string, idx_t> &a, const pair<string, idx_t> &b) {
		if (a.second != b.second) {
			return a.second > b.second;
		}
		return a.first < b.first;
	});
	if (selected.size() > VARIANT_SHREDDED_FIELD_LIMIT) {
		selected.resize(VARIANT_SHREDDED_FIELD_LIMIT);
	}
	vector<string> result;
	result.reserve(selected.size());
	for (auto &field : selected) {
		result.push_back(std::move(field.first));
	}
	return result;
}

struct VariantColumnStatsData {
public:
	explicit VariantColumnStatsData(idx_t index) : index(index) {
	}

public:
	void SetType(VariantLogicalType type);

public:
	//! The index in the 'columns' of the VariantShreddingStats
	idx_t index;
	//! Count of each variant type encountered
	idx_t type_counts[static_cast<uint8_t>(VariantLogicalType::ENUM_SIZE)] = {0};
	uint32_t decimal_width;
	uint32_t decimal_scale;
	bool decimal_consistent = false;

	idx_t total_count = 0;
	//! indices into the top-level 'columns' vector where the stats for the field/element live
	unordered_map<string, idx_t> field_stats;
	idx_t element_stats = DConstants::INVALID_INDEX;
};

struct VariantShreddingStats {
public:
	VariantShreddingStats() {
		columns.emplace_back(0);
	}

public:
	VariantColumnStatsData &GetOrCreateElement(idx_t parent_index);
	VariantColumnStatsData &GetOrCreateField(idx_t parent_index, const string &name);

	VariantColumnStatsData &GetColumnStats(idx_t index);
	const VariantColumnStatsData &GetColumnStats(idx_t index) const;

public:
	void Update(const Vector &input, idx_t count);
	//! If force_partial is set, every level keeps its 'untyped_value_index' (overlay) column even when the
	//! sampled values are fully consistent - allowing later inconsistent values to be partially shredded.
	LogicalType GetShreddedType(bool force_partial = false) const;

private:
	bool GetShreddedTypeInternal(const VariantColumnStatsData &column, LogicalType &out_type,
	                             optional_idx parent_count = optional_idx(), bool force_partial = false) const;

private:
	//! Nested type analysis
	vector<VariantColumnStatsData> columns;
};

struct VariantShredding {
public:
	VariantShredding() {
	}
	virtual ~VariantShredding() = default;

public:
	static LogicalType GetUnshreddedType() {
		return LogicalType::STRUCT(StructType::GetChildTypes(LogicalType::VARIANT()));
	}

public:
	virtual void WriteVariantValues(UnifiedVariantVectorData &variant, Vector &result,
	                                optional_ptr<const SelectionVector> sel,
	                                optional_ptr<const SelectionVector> value_index_sel,
	                                optional_ptr<const SelectionVector> result_sel, idx_t count) = 0;

protected:
	idx_t typed_value_index = VariantStats::TYPED_VALUE_INDEX;
	idx_t untyped_value_index = VariantStats::UNTYPED_VALUE_INDEX;

protected:
	void WriteTypedValues(UnifiedVariantVectorData &variant, Vector &result, const SelectionVector &sel,
	                      const SelectionVector &value_index_sel, const SelectionVector &result_sel, idx_t count);
	virtual void WriteMissingField(Vector &vector, idx_t index);

private:
	void WriteTypedObjectValues(UnifiedVariantVectorData &variant, Vector &result, const SelectionVector &sel,
	                            const SelectionVector &value_index_sel, const SelectionVector &result_sel, idx_t count);
	void WriteTypedArrayValues(UnifiedVariantVectorData &variant, Vector &result, const SelectionVector &sel,
	                           const SelectionVector &value_index_sel, const SelectionVector &result_sel, idx_t count);
	void WriteTypedPrimitiveValues(UnifiedVariantVectorData &variant, Vector &result, const SelectionVector &sel,
	                               const SelectionVector &value_index_sel, const SelectionVector &result_sel,
	                               idx_t count);
};

struct VariantShreddingState {
public:
	explicit VariantShreddingState(const LogicalType &type, idx_t total_count);
	virtual ~VariantShreddingState() {
	}

public:
	bool ValueIsShredded(UnifiedVariantVectorData &variant, idx_t row, uint32_t values_index);
	void SetShredded(uint32_t row, uint32_t values_index, uint32_t result_idx);
	unordered_set<string> ObjectFields() const;
	virtual const unordered_set<VariantLogicalType> &GetVariantTypes() = 0;

public:
	//! The type the field is shredded on
	const LogicalType &type;
	//! row that is shredded
	SelectionVector shredded_sel;
	//! 'values_index' of the shredded value
	SelectionVector values_index_sel;
	//! result row of the shredded value
	SelectionVector result_sel;
	//! The amount of rows that are shredded on
	idx_t count = 0;
};

} // namespace duckdb
