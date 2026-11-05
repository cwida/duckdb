#pragma once

#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/function/scalar/variant_utils.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/types/variant.hpp"
#include "duckdb/common/types/selection_vector.hpp"
#include "duckdb/common/owning_string_map.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/serializer/varint.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/common/exception/conversion_exception.hpp"
#include "duckdb/common/checked_integer.hpp"

namespace duckdb {
namespace variant {

void InitializeOffsets(DataChunk &offsets, idx_t count);

struct OffsetData {
public:
	static uint32_t *GetKeys(DataChunk &offsets) {
		return FlatVector::GetDataMutable<uint32_t>(offsets.data[0]);
	}
	static uint32_t *GetChildren(DataChunk &offsets) {
		return FlatVector::GetDataMutable<uint32_t>(offsets.data[1]);
	}
	static uint32_t *GetValues(DataChunk &offsets) {
		return FlatVector::GetDataMutable<uint32_t>(offsets.data[2]);
	}
	static uint32_t *GetBlob(DataChunk &offsets) {
		return FlatVector::GetDataMutable<uint32_t>(offsets.data[3]);
	}
	//! Adds len to offset, throwing if the running total would exceed what a uint32_t blob offset can represent.
	static void AddBlobOffset(uint32_t &offset, uint32_t len) {
		try {
			offset = uinteger_t(offset) += len;
		} catch (OutOfRangeException &) {
			throw InvalidInputException(
			    "Cannot convert value to VARIANT: encoded row size exceeds the maximum supported %u bytes",
			    NumericLimits<uint32_t>::Maximum());
		}
	}
};

struct ToVariantGlobalResultData {
public:
	ToVariantGlobalResultData(VariantVectorData &variant, DataChunk &offsets,
	                          OrderedOwningStringMap<uint32_t> &dictionary, SelectionVector &keys_selvec,
	                          Allocator &allocator)
	    : variant(variant), offsets(offsets), dictionary(dictionary), keys_selvec(keys_selvec), allocator(allocator) {
	}

public:
	uint32_t GetOrCreateIndex(const string_t &key) {
		auto unsorted_idx = dictionary.size();
		//! This will later be remapped to the sorted idx (see FinalizeVariantKeys in 'to_variant.cpp')
		return dictionary.emplace(std::make_pair(key, unsorted_idx)).first->second;
	}

public:
	VariantVectorData &variant;
	DataChunk &offsets;
	//! The dictionary to populate with the (unique and sorted) keys
	OrderedOwningStringMap<uint32_t> &dictionary;
	//! The selection vector to populate with mapping from keys index -> dictionary index
	SelectionVector &keys_selvec;
	//! Allocator for temporary conversion memory (e.g. JSON parsing), tracked by DuckDB's memory accounting
	Allocator &allocator;
};

template <bool WRITE_DATA>
void WriteContainerData(VariantVectorData &result, idx_t result_index, uint32_t &blob_offset, idx_t length,
                        idx_t children_offset) {
	const auto length_varint_size = GetVarintSize(length);
	const auto child_offset_varint_size = NumericCast<uint32_t>(length ? GetVarintSize(children_offset) : 0);

	if (WRITE_DATA) {
		auto &blob_value = result.blob_data[result_index];
		auto blob_value_data = data_ptr_cast(blob_value.GetDataWriteable());
		VarintEncode(length, blob_value_data + blob_offset);
		if (length) {
			VarintEncode(children_offset, blob_value_data + blob_offset + length_varint_size);
		}
	}

	OffsetData::AddBlobOffset(blob_offset, length_varint_size + child_offset_varint_size);
}

struct ContainerSelectionVectors {
public:
	explicit ContainerSelectionVectors(idx_t max_size)
	    : new_selection(SelectionVector::Incremental(max_size)),
	      non_null_selection(SelectionVector::Incremental(max_size)),
	      children_selection(SelectionVector::Incremental(max_size)) {
	}

public:
	idx_t count = 0;
	//! Create a selection vector that maps to the right row in the 'result' for the child vector
	SelectionVector new_selection;
	//! Create a selection vector that maps to a subset (the rows where the parent isn't null) of rows of the child
	//! vector
	SelectionVector non_null_selection;
	//! Create a selection vector that maps to the children.values_index entry of the parent
	SelectionVector children_selection;
};

template <bool WRITE_DATA>
void WriteArrayChildren(VariantVectorData &result, uint64_t children_offset, uint32_t &children_offset_data,
                        const list_entry_t source_entry, idx_t result_index, ContainerSelectionVectors &sel) {
	for (idx_t child_idx = 0; child_idx < source_entry.length; child_idx++) {
		//! Set up the selection vector for the child of the list vector
		sel.new_selection.set_index(child_idx + sel.count, result_index);
		if (WRITE_DATA) {
			idx_t children_index = children_offset + children_offset_data;
			sel.children_selection.set_index(child_idx + sel.count, children_index + child_idx);
			result.keys_index_validity.SetInvalid(children_index + child_idx);
		}
		sel.non_null_selection.set_index(sel.count + child_idx, source_entry.offset + child_idx);
	}
	children_offset_data += source_entry.length;
	sel.count += source_entry.length;
}

template <bool WRITE_DATA>
void WriteVariantMetadata(ToVariantGlobalResultData &result, idx_t result_index, uint32_t *values_offsets,
                          uint32_t blob_offset, optional_ptr<const SelectionVector> value_index_selvec, idx_t i,
                          VariantLogicalType type_id) {
	auto &values_offset_data = values_offsets[result_index];
	if (WRITE_DATA) {
		auto &variant = result.variant;
		auto &values_list_entry = variant.values_data[result_index];
		auto values_offset = values_list_entry.offset;

		values_offset = values_list_entry.offset + values_offset_data;
		variant.type_ids_data[values_offset] = static_cast<uint8_t>(type_id);
		variant.byte_offset_data[values_offset] = blob_offset;
		if (value_index_selvec) {
			variant.values_index_data[value_index_selvec->get_index(i)] = values_offset_data;
		}
	}
	values_offset_data++;
}

template <bool WRITE_DATA>
void HandleVariantNull(ToVariantGlobalResultData &result, idx_t result_index, uint32_t *values_offsets,
                       uint32_t blob_offset, optional_ptr<const SelectionVector> value_index_selvec, idx_t i,
                       const bool is_root) {
	if (is_root) {
		if (WRITE_DATA) {
			FlatVector::SetNull(result.variant.variant, result_index, true);
		}
		return;
	}
	WriteVariantMetadata<WRITE_DATA>(result, result_index, values_offsets, blob_offset, value_index_selvec, i,
	                                 VariantLogicalType::VARIANT_NULL);
}

struct ToVariantSourceData {
public:
	ToVariantSourceData(Vector &source, idx_t source_size) : vec(source), source_size(source_size) {
		vec.ToUnifiedFormat(source_format);
	}
	ToVariantSourceData(Vector &source, idx_t source_size, const SelectionVector &sel)
	    : vec(source), source_size(source_size), source_sel(sel) {
		vec.ToUnifiedFormat(source_format);
	}
	ToVariantSourceData(Vector &source, idx_t source_size, optional_ptr<const SelectionVector> sel)
	    : vec(source), source_size(source_size), source_sel(sel) {
		vec.ToUnifiedFormat(source_format);
	}

public:
	uint32_t operator[](idx_t i) const {
		return static_cast<uint32_t>(source_format.sel->get_index(source_sel ? source_sel->get_index(i) : i));
	}
	uint32_t GetMappedIndex(idx_t i) {
		return static_cast<uint32_t>(source_sel ? source_sel->get_index(i) : i);
	}

public:
	Vector &vec;
	UnifiedVectorFormat source_format;
	idx_t source_size;
	optional_ptr<const SelectionVector> source_sel = nullptr;
};

template <bool WRITE_DATA, bool IGNORE_NULLS = false>
bool ConvertToVariant(ToVariantSourceData &source, ToVariantGlobalResultData &result, idx_t count,
                      optional_ptr<const SelectionVector> selvec,
                      optional_ptr<const SelectionVector> values_index_selvec, const bool is_root);

} // namespace variant
} // namespace duckdb
