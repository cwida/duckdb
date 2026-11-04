#include "decoder/dictionary_decoder.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "column_reader.hpp"
#include "parquet_reader.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/table_filter_state.hpp"
#include "duckdb/storage/table/column_segment.hpp"
#include "duckdb/common/assert.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/shared_ptr_ipp.hpp"
#include "duckdb/common/types/validity_mask.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/vector/dictionary_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/unified_vector_format.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/common/vector_size.hpp"
#include "resizable_buffer.hpp"

namespace duckdb {

DictionaryDecoder::DictionaryDecoder(ColumnReader &reader)
    : reader(reader), offset_buffer(reader.encoding_buffers[0]), valid_sel(STANDARD_VECTOR_SIZE),
      dictionary_selection_vector(STANDARD_VECTOR_SIZE), dictionary_size(0) {
}

void DictionaryDecoder::InitializeDictionary(idx_t new_dictionary_size, optional_ptr<const TableFilter> filter,
                                             optional_ptr<TableFilterState> filter_state, bool has_defines) {
	dictionary_size = new_dictionary_size;
	filter_result.reset();
	filter_count = 0;
	can_have_nulls = has_defines;

	// we use the last entry as a NULL, dictionary vectors don't have a separate validity mask
	const auto duckdb_dictionary_size = dictionary_size + can_have_nulls;
	dictionary = DictionaryVector::CreateReusableDictionary(reader.Type(), duckdb_dictionary_size);
	auto &dictionary_data = dictionary->data;
	auto &dict_validity = FlatVector::ValidityMutable(dictionary_data);
	dict_validity.Reset(duckdb_dictionary_size);
	if (can_have_nulls) {
		dict_validity.SetInvalid(dictionary_size);
	}

	// now read the non-NULL values from Parquet
	if (dictionary_size > 0) {
		reader.Plain(reader.block, nullptr, dictionary_size, 0, dictionary_data);
	}

	// immediately filter the dictionary, if applicable
	if (filter && CanFilter(*filter, *filter_state)) {
		// no filter result yet - apply filter to the dictionary
		// initialize the filter result - setting everything to false
		filter_result = make_unsafe_uniq_array<bool>(duckdb_dictionary_size);

		// apply the filter
		SelectionVector dict_sel;
		filter_count = duckdb_dictionary_size;
		ColumnSegment::FilterSelection(dict_sel, dictionary_data, *filter_state, duckdb_dictionary_size, filter_count);

		// now set all matching tuples to true
		for (idx_t i = 0; i < filter_count; i++) {
			auto idx = dict_sel.get_index(i);
			filter_result[idx] = true;
		}
	}
}

void DictionaryDecoder::InitializePage() {
	// where is it otherwise??
	auto &block = reader.block;
	auto dict_width = block->read<uint8_t>();
	dict_decoder = make_uniq<RleBpDecoder>(block->ptr, block->len, dict_width);
	block->inc(block->len);
}

void DictionaryDecoder::ConvertDictToSelVec(uint32_t *offsets, const SelectionVector &rows, idx_t count) {
	D_ASSERT(count <= STANDARD_VECTOR_SIZE);
	for (idx_t idx = 0; idx < count; idx++) {
		auto row_idx = rows.get_index(idx);
		auto offset = offsets[idx];
		if (offset >= dictionary_size) {
			throw std::runtime_error("Parquet file is likely corrupted, dictionary offset out of range");
		}
		dictionary_selection_vector.set_index(row_idx, offset);
	}
}

idx_t DictionaryDecoder::GetValidValues(uint8_t *defines, idx_t read_count, idx_t result_offset) {
	idx_t valid_count = read_count;
	if (defines) {
		D_ASSERT(can_have_nulls);
		valid_count = 0;
		for (idx_t i = 0; i < read_count; i++) {
			valid_sel.set_index(valid_count, i);
			dictionary_selection_vector.set_index(i, dictionary_size);
			valid_count += defines[result_offset + i] == reader.MaxDefine();
		}
	}
	return valid_count;
}

idx_t DictionaryDecoder::Read(uint8_t *defines, idx_t read_count, Vector &result, idx_t result_offset) {
	if (!dictionary) {
		throw std::runtime_error("Parquet file is likely corrupted, missing dictionary");
	}
	idx_t valid_count = GetValidValues(defines, read_count, result_offset);
	if (valid_count == read_count) {
		// all values are valid - we can directly decompress the offsets into the selection vector
		dict_decoder->GetBatch<uint32_t>(data_ptr_cast(dictionary_selection_vector.data()),
		                                 NumericCast<uint32_t>(valid_count));
		// we do still need to verify the offsets though
		uint32_t max_index = 0;
		for (idx_t idx = 0; idx < valid_count; idx++) {
			max_index = MaxValue(max_index, dictionary_selection_vector[idx]);
		}
		if (max_index >= dictionary_size) {
			throw std::runtime_error("Parquet file is likely corrupted, dictionary offset out of range");
		}
	} else if (valid_count > 0) {
		// for the valid entries - decode the offsets
		offset_buffer.resize(reader.reader.allocator, sizeof(uint32_t) * valid_count);
		dict_decoder->GetBatch<uint32_t>(offset_buffer.ptr, NumericCast<uint32_t>(valid_count));
		ConvertDictToSelVec(reinterpret_cast<uint32_t *>(offset_buffer.ptr), valid_sel, valid_count);
	}
#ifdef DEBUG
	dictionary_selection_vector.Verify(read_count, dictionary_size + can_have_nulls);
#endif
	if (result_offset == 0) {
		result.Dictionary(dictionary, dictionary_selection_vector, read_count);
		D_ASSERT(result.GetVectorType() == VectorType::DICTIONARY_VECTOR);
	} else {
		D_ASSERT(result.GetVectorType() == VectorType::FLAT_VECTOR);
		VectorOperations::Copy(dictionary->data, result, dictionary_selection_vector, read_count, 0, result_offset);
	}
	return valid_count;
}

void DictionaryDecoder::Skip(uint8_t *defines, idx_t skip_count) {
	if (!dictionary) {
		throw std::runtime_error("Parquet file is likely corrupted, missing dictionary");
	}
	idx_t valid_count = reader.GetValidCount(defines, skip_count);
	// skip past the valid offsets
	dict_decoder->Skip(NumericCast<uint32_t>(valid_count));
}

bool DictionaryDecoder::DictionarySupportsFilter(const TableFilter &filter, TableFilterState &filter_state) {
	auto &expr_filter = ExpressionFilter::GetExpressionFilter(filter, "DictionaryDecoder::DictionarySupportsFilter");
	auto &state = filter_state.Cast<ExpressionFilterState>();
	auto emits_nulls = expr_filter.EvaluateWithConstant(*state.executor, Value(reader.Type()));
	return !emits_nulls;
}

bool DictionaryDecoder::CanFilter(const TableFilter &filter, TableFilterState &filter_state) {
	if (dictionary_size == 0) {
		return false;
	}
	if (filter_state.can_throw) {
		// the dictionary holds the values of every row in the page - evaluating a filter that can throw over it
		// would raise errors for rows that are never returned
		return false;
	}
	// We can only push the filter if the filter removes NULL values
	if (!DictionarySupportsFilter(filter, filter_state)) {
		return false;
	}
	return true;
}

void DictionaryDecoder::Filter(uint8_t *defines, const idx_t read_count, Vector &result, SelectionVector &sel,
                               idx_t &approved_tuple_count) {
	if (!dictionary) {
		throw std::runtime_error("Parquet file is likely corrupted, missing dictionary");
	}
	D_ASSERT(filter_count > 0);
	// read the dictionary values
	const auto valid_count = Read(defines, read_count, result, 0);
	if (valid_count == 0) {
		// all values are NULL
		approved_tuple_count = 0;
		return;
	}

	// apply the filter by checking the dictionary offsets directly
	SelectionVector new_sel(valid_count);
	if (valid_count == read_count) {
		approved_tuple_count =
		    SelectionVector::SelectFromBoolean(filter_result.get(), &dictionary_selection_vector, read_count,
		                                       array_ptr<sel_t>(new_sel.data(), new_sel.Capacity()));
	} else {
		auto offsets = reinterpret_cast<uint32_t *>(offset_buffer.ptr);
		D_ASSERT(offsets);
		approved_tuple_count = 0;
		for (idx_t idx = 0; idx < valid_count; idx++) {
			auto row_idx = valid_sel.get_index(idx);
			auto offset = offsets[idx];
			// avoid a branch per row: always write, advance only on a match
			new_sel.set_index(approved_tuple_count, row_idx);
			approved_tuple_count += filter_result[offset];
		}
	}
	if (approved_tuple_count < read_count) {
		sel.Initialize(new_sel);
	}
}

} // namespace duckdb
