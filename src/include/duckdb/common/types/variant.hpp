//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/types/variant.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/storage/storage_info.hpp"

namespace duckdb_yyjson {
struct yyjson_mut_doc;
struct yyjson_mut_val;
} // namespace duckdb_yyjson

namespace duckdb {
class Vector;
class VariantNode;
struct ValidityMask;
struct UnifiedVariantVector;
struct RecursiveUnifiedVectorFormat;
struct UnifiedVectorFormat;

enum class VariantChildLookupMode : uint8_t { INVALID, BY_KEY, BY_INDEX };

struct Variant {
public:
	static constexpr StorageVersion VERSION_ADDED = StorageVersion::V1_5_0; // Added to core in DuckDB v1.5.0
};

struct VariantPathComponent {
public:
	explicit VariantPathComponent() : lookup_mode(VariantChildLookupMode::INVALID) {
	}
	explicit VariantPathComponent(const string &key) : lookup_mode(VariantChildLookupMode::BY_KEY), key(key) {
	}
	explicit VariantPathComponent(uint32_t index) : lookup_mode(VariantChildLookupMode::BY_INDEX), index(index) {
	}

	bool operator==(const VariantPathComponent &other) const {
		if (lookup_mode != other.lookup_mode) {
			return false;
		}
		switch (lookup_mode) {
		case VariantChildLookupMode::BY_KEY:
			return key == other.key;
		case VariantChildLookupMode::BY_INDEX:
			return index == other.index;
		default:
			return false;
		}
	}
	bool operator!=(const VariantPathComponent &other) const {
		return !(*this == other);
	}

public:
	VariantChildLookupMode lookup_mode;
	string key;
	uint32_t index;
};

struct VariantNestedData {
	//! The amount of children in the nested structure
	uint32_t child_count;
	//! Index of the first child
	uint32_t children_idx;
};

//! The (width, scale) of a DECIMAL value - the physical storage type (and hence the payload) follows
//! from the width (see VariantDecimalData::GetPhysicalType)
struct VariantDecimalProperties {
	VariantDecimalProperties(uint32_t width, uint32_t scale) : width(width), scale(scale) {
	}

	uint32_t width;
	uint32_t scale;
};

struct VariantDecimalData {
public:
	VariantDecimalData(uint32_t width, uint32_t scale, const_data_ptr_t value_ptr)
	    : width(width), scale(scale), value_ptr(value_ptr) {
	}

public:
	PhysicalType GetPhysicalType() const;

public:
	uint32_t width;
	uint32_t scale;
	const_data_ptr_t value_ptr = nullptr;
};

struct VariantVectorData {
public:
	explicit VariantVectorData(Vector &variant);

public:
	Vector &variant;

	//! value
	string_t *blob_data;

	//! values
	uint8_t *type_ids_data;
	uint32_t *byte_offset_data;

	//! children
	uint32_t *keys_index_data;
	uint32_t *values_index_data;
	ValidityMask &keys_index_validity;

	//! values | children | keys
	list_entry_t *values_data;
	list_entry_t *children_data;
	list_entry_t *keys_data;

	Vector &keys;
};

enum class VariantLogicalType : uint8_t {
	VARIANT_NULL = 0,
	BOOL_TRUE = 1,
	BOOL_FALSE = 2,
	INT8 = 3,
	INT16 = 4,
	INT32 = 5,
	INT64 = 6,
	INT128 = 7,
	UINT8 = 8,
	UINT16 = 9,
	UINT32 = 10,
	UINT64 = 11,
	UINT128 = 12,
	FLOAT = 13,
	DOUBLE = 14,
	DECIMAL = 15,
	VARCHAR = 16,
	BLOB = 17,
	UUID = 18,
	DATE = 19,
	TIME_MICROS = 20,
	TIME_NANOS = 21,
	TIMESTAMP_SEC = 22,
	TIMESTAMP_MILIS = 23,
	TIMESTAMP_MICROS = 24,
	TIMESTAMP_NANOS = 25,
	TIME_MICROS_TZ = 26,
	TIMESTAMP_MICROS_TZ = 27,
	INTERVAL = 28,
	OBJECT = 29,
	ARRAY = 30,
	BIGNUM = 31,
	BITSTRING = 32,
	GEOMETRY = 33,
	TIMESTAMP_NANOS_TZ = 34,
	ENUM_SIZE /* always kept as last item of the enum */
};

struct UnifiedVariantVectorData {
public:
	explicit UnifiedVariantVectorData(const RecursiveUnifiedVectorFormat &variant);

public:
	bool RowIsValid(idx_t row) const;
	bool KeysIndexIsValid(idx_t row, idx_t index) const;
	list_entry_t GetChildrenListEntry(idx_t row) const;
	list_entry_t GetValuesListEntry(idx_t row) const;
	const string_t &GetKey(idx_t row, idx_t index) const;
	idx_t GetKeysCount(idx_t row) const;
	uint32_t GetKeysIndex(idx_t row, idx_t child_index) const;
	uint32_t GetValuesIndex(idx_t row, idx_t child_index) const;
	VariantLogicalType GetTypeId(idx_t row, idx_t value_index) const;
	uint32_t GetByteOffset(idx_t row, idx_t value_index) const;
	const string_t &GetData(idx_t row) const;

private:
	const RecursiveUnifiedVectorFormat &variant;
	const UnifiedVectorFormat &keys;
	const UnifiedVectorFormat &keys_entry;
	const UnifiedVectorFormat &children;
	const UnifiedVectorFormat &keys_index;
	const UnifiedVectorFormat &values_index;
	const UnifiedVectorFormat &values;
	const UnifiedVectorFormat &type_id;
	const UnifiedVectorFormat &byte_offset;
	const UnifiedVectorFormat &data;

	const list_entry_t *keys_data = nullptr;
	const string_t *keys_entry_data = nullptr;
	const list_entry_t *children_data = nullptr;
	const uint32_t *keys_index_data = nullptr;
	const uint32_t *values_index_data = nullptr;
	const list_entry_t *values_data = nullptr;
	const uint8_t *type_id_data = nullptr;
	const uint32_t *byte_offset_data = nullptr;
	const string_t *blob_data = nullptr;

	const ValidityMask &keys_index_validity;
};

struct VariantCasts {
	static duckdb_yyjson::yyjson_mut_val *ConvertVariantToJSON(duckdb_yyjson::yyjson_mut_doc *doc,
	                                                           const UnifiedVariantVectorData &source, idx_t row,
	                                                           uint32_t values_idx);
	static duckdb_yyjson::yyjson_mut_val *ConvertVariantToJSON(duckdb_yyjson::yyjson_mut_doc *doc,
	                                                           const VariantNode &source, bool sort_object_keys = true);
};

} // namespace duckdb
