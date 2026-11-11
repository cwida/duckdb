//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/scalar/variant_utils.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/function/built_in_functions.hpp"
#include "duckdb/common/types/variant.hpp"
#include "duckdb/common/owning_string_map.hpp"

namespace duckdb {
class VariantIterator;

struct VariantPathBindData : public FunctionData {
public:
	explicit VariantPathBindData();
	explicit VariantPathBindData(const string &input_path);
	explicit VariantPathBindData(const vector<string> &input_paths);
	VariantPathBindData(const VariantPathBindData &other) = default;

public:
	unique_ptr<FunctionData> Copy() const override;
	bool Equals(const FunctionData &other) const override;

public:
	vector<vector<VariantPathComponent>> paths;
};

struct VariantExtractBindData : public FunctionData {
public:
	explicit VariantExtractBindData(const string &str);
	explicit VariantExtractBindData(uint32_t index);
	VariantExtractBindData(const VariantExtractBindData &other) = default;

public:
	unique_ptr<FunctionData> Copy() const override;
	bool Equals(const FunctionData &other) const override;

public:
	VariantPathComponent component;
};

struct VariantNestedDataCollectionResult {
public:
	VariantNestedDataCollectionResult() : success(true) {
	}
	explicit VariantNestedDataCollectionResult(VariantLogicalType wrong_type) : success(false), wrong_type(wrong_type) {
	}

public:
	bool success;
	//! If success is false, the type that was encountered that caused the collection failure
	VariantLogicalType wrong_type;
};

struct VariantChildDataCollectionResult {
public:
	enum class Type : uint8_t { SUCCESS, INDEX_ZERO, COMPONENT_NOT_FOUND };

public:
	VariantChildDataCollectionResult() : type(Type::SUCCESS) {
	}

public:
	static VariantChildDataCollectionResult IndexZero() {
		return VariantChildDataCollectionResult(Type::INDEX_ZERO);
	}
	static VariantChildDataCollectionResult NotFound(idx_t nested_index) {
		return VariantChildDataCollectionResult(Type::COMPONENT_NOT_FOUND, nested_index);
	}

public:
	bool Success() const {
		return type == Type::SUCCESS;
	}

private:
	explicit VariantChildDataCollectionResult(Type type, idx_t index = DConstants::INVALID_INDEX)
	    : type(type), nested_data_index(index) {
	}

public:
	Type type;
	idx_t nested_data_index;
};

struct VariantPathSelection {
	explicit VariantPathSelection(const idx_t count) {
		value_index_sel.Initialize(count);
		new_value_index_sel.Initialize(count);

		// We start at values[0] for every row.
		for (idx_t i = 0; i < count; i++) {
			value_index_sel[i] = 0;
		}
	}

	SelectionVector &Input(const idx_t depth) {
		return depth % 2 == 0 ? value_index_sel : new_value_index_sel;
	}

	SelectionVector &Output(const idx_t depth) {
		return depth % 2 == 0 ? new_value_index_sel : value_index_sel;
	}

	//! Input and output buffers used during the object walk, switched per iteration
	SelectionVector value_index_sel, new_value_index_sel;
};

struct VariantUtils {
	//! Throws a DataCorruptionException unless 'size' bytes are available in a blob of 'blob_size' starting at
	//! 'offset'; shared by every call site that decodes a byte_offset/varint-length pair out of a VARIANT blob
	DUCKDB_API static void Available(idx_t blob_size, idx_t offset, idx_t size);
	DUCKDB_API static bool IsNestedType(const UnifiedVariantVectorData &variant, idx_t row, uint32_t value_index);
	DUCKDB_API static VariantDecimalData DecodeDecimalData(const UnifiedVariantVectorData &variant, idx_t row,
	                                                       uint32_t value_index);
	DUCKDB_API static VariantNestedData DecodeNestedData(const UnifiedVariantVectorData &variant, idx_t row,
	                                                     uint32_t value_index);
	DUCKDB_API static string_t DecodeStringData(const UnifiedVariantVectorData &variant, idx_t row,
	                                            uint32_t value_index);
	DUCKDB_API static vector<string> GetObjectKeys(const UnifiedVariantVectorData &variant, idx_t row,
	                                               const VariantNestedData &nested_data);
	DUCKDB_API static void FindChildValues(const UnifiedVariantVectorData &variant,
	                                       const VariantPathComponent &component,
	                                       optional_ptr<const SelectionVector> sel, SelectionVector &res,
	                                       ValidityMask &res_validity, const array_ptr<VariantNestedData> &nested_data,
	                                       const ValidityMask &validity, idx_t count);
	DUCKDB_API static VariantNestedDataCollectionResult
	CollectNestedData(const UnifiedVariantVectorData &variant, VariantLogicalType expected_type,
	                  const SelectionVector &value_index_sel, idx_t count, optional_idx row, idx_t offset,
	                  array_ptr<VariantNestedData> child_data, ValidityMask &validity);
	//! Generic unshredded traversal over a variant using a provided path
	DUCKDB_API static void TraversePath(const UnifiedVariantVectorData &variant,
	                                    const vector<VariantPathComponent> &components, idx_t count,
	                                    array_ptr<VariantNestedData> nested_data, ValidityMask &validity,
	                                    VariantPathSelection &path_selection);
	DUCKDB_API static vector<uint32_t> ValueIsNull(const UnifiedVariantVectorData &variant, const SelectionVector &sel,
	                                               idx_t count, optional_idx row);
	DUCKDB_API static Value ConvertVariantToValue(const UnifiedVariantVectorData &variant, idx_t row,
	                                              uint32_t values_idx);
	DUCKDB_API static bool Verify(const Vector &variant, const SelectionVector &sel_p, idx_t count);
	DUCKDB_API static void FinalizeVariantKeys(Vector &variant, OrderedOwningStringMap<uint32_t> &dictionary,
	                                           SelectionVector &sel, idx_t sel_size);
	DUCKDB_API static void VariantExtract(const Vector &input, const vector<VariantPathComponent> &components,
	                                      Vector &result, idx_t count);
	DUCKDB_API static void UnshredVariantData(Vector &input, Vector &output, idx_t count);
	//! Returns the type of a shredded vector that is shredded on "type"
	DUCKDB_API static LogicalType ShreddedType(const LogicalType &type);

	//! Whether or not a type is natively supported in variant
	DUCKDB_API static bool VariantSupportsType(const LogicalType &type);
	//! Whether casting a value shredded as "shredded_type" to "target" gives the same result as casting the VARIANT
	DUCKDB_API static bool ShreddedCastMatchesVariantCast(const LogicalType &shredded_type, const LogicalType &target);

	//! Build a canonical (unshredded) VARIANT vector by traversing a variant directly through a
	//! VariantIterator - avoids materializing the intermediate vector<VariantValue> tree
	DUCKDB_API static void ToVariant(const VariantIterator &state, idx_t count, Vector &result);
};

struct VariantBindUtils {
	DUCKDB_API static unique_ptr<FunctionData> VariantPathBind(BindScalarFunctionInput &input);
};

} // namespace duckdb
