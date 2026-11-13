//===----------------------------------------------------------------------===//
//                         DuckDB
//
// decoder/dictionary_decoder.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include <stdint.h>

#include "duckdb.hpp"
#include "parquet_rle_bp_decoder.hpp"
#include "resizable_buffer.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/selection_vector.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/planner/table_filter.hpp"
#include "duckdb/planner/table_filter_state.hpp"

namespace duckdb {
class ColumnReader;
struct TableFilterState;
class TableFilterCache;
class DictionaryEntry;
class ResizeableBuffer;
class Vector;

class DictionaryDecoder {
public:
	explicit DictionaryDecoder(ColumnReader &reader);

public:
	void InitializeDictionary(idx_t dictionary_size, optional_ptr<const TableFilter> filter,
	                          optional_ptr<TableFilterState> filter_state, bool has_defines);
	void InitializePage();
	void Rebase();
	idx_t Read(uint8_t *defines, idx_t read_count, Vector &result, idx_t result_offset);
	void Skip(uint8_t *defines, idx_t skip_count);
	bool CanFilter(const TableFilter &filter, TableFilterState &filter_state);
	void Filter(uint8_t *defines, idx_t read_count, Vector &result, SelectionVector &sel, idx_t &approved_tuple_count);
	void ResetFilter(const TableFilterState &state);
	bool HasFilter(TableFilterState &state);
	bool HasFilteredOutAllValues() const {
		return filter_cache && filter_cache->CanSkipAll();
	}

private:
	idx_t GetValidValues(uint8_t *defines, idx_t read_count, idx_t result_offset);
	void ConvertDictToSelVec(uint32_t *offsets, const SelectionVector &rows, idx_t count);

private:
	ColumnReader &reader;
	ResizeableBuffer &offset_buffer;
	unique_ptr<RleBpDecoder> dict_decoder;
	idx_t block_offset = 0;
	SelectionVector valid_sel;
	SelectionVector dictionary_selection_vector;
	idx_t dictionary_size;
	buffer_ptr<DictionaryEntry> dictionary;
	unique_ptr<TableFilterCache> filter_cache;
	bool can_have_nulls;
};

} // namespace duckdb
