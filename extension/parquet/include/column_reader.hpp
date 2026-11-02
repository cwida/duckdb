//===----------------------------------------------------------------------===//
//                         DuckDB
//
// column_reader.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include <stdint.h>
#include <string.h>
#include <string>

#include "duckdb.hpp"
#include "parquet_bss_decoder.hpp"
#include "parquet_statistics.hpp"
#include "parquet_types.h"
#include "parquet_page_index.hpp"
#include "resizable_buffer.hpp"
#include "thrift_tools.hpp"
#include "decoder/byte_stream_split_decoder.hpp"
#include "decoder/delta_binary_packed_decoder.hpp"
#include "decoder/dictionary_decoder.hpp"
#include "decoder/rle_decoder.hpp"
#include "decoder/delta_length_byte_array_decoder.hpp"
#include "decoder/delta_byte_array_decoder.hpp"
#include "parquet_column_schema.hpp"
#include "parquet_crypto.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/common/types/vector_cache.hpp"
#include "duckdb/common/encryption_functions.hpp"
#include "duckdb/common/assert.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/shared_ptr_ipp.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/selection_vector.hpp"
#include "duckdb/common/types/validity_mask.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/planner/table_filter.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"

namespace duckdb_apache {
namespace thrift {
class TBase;

namespace protocol {
class TProtocol;
} // namespace protocol
} // namespace thrift
} // namespace duckdb_apache

namespace duckdb {
class ParquetReader;
struct TableFilterState;
class Allocator;
class RleBpDecoder;
class ThriftFileTransport;
class Vector;

using duckdb_apache::thrift::protocol::TProtocol;

using duckdb_parquet::ColumnChunk;
using duckdb_parquet::CompressionCodec;
using duckdb_parquet::FieldRepetitionType;
using duckdb_parquet::PageHeader;
using duckdb_parquet::SchemaElement;
using duckdb_parquet::Type;

enum class ColumnEncoding {
	INVALID,
	DICTIONARY,
	DELTA_BINARY_PACKED,
	RLE,
	DELTA_LENGTH_BYTE_ARRAY,
	DELTA_BYTE_ARRAY,
	BYTE_STREAM_SPLIT,
	PLAIN
};

struct ColumnReaderInput {
public:
	ColumnReaderInput(uint64_t num_values, data_ptr_t define_out, data_ptr_t repeat_out)
	    : num_values(num_values), define_out(define_out), repeat_out(repeat_out) {
	}

public:
	uint64_t num_values;
	data_ptr_t define_out;
	data_ptr_t repeat_out;
};

class ColumnReader {
	friend class ByteStreamSplitDecoder;
	friend class DeltaBinaryPackedDecoder;
	friend class DeltaByteArrayDecoder;
	friend class DeltaLengthByteArrayDecoder;
	friend class DictionaryDecoder;
	friend class RLEDecoder;

public:
	ColumnReader(const ParquetReader &reader, const ParquetColumnSchema &schema_p);
	virtual ~ColumnReader();

public:
	static unique_ptr<ColumnReader> CreateReader(const ParquetReader &reader, const ParquetColumnSchema &schema);
	virtual void InitializeRead(idx_t row_group_index, idx_t row_group_num_rows,
	                            const duckdb_parquet::ColumnChunkList &columns, TProtocol &protocol_p);
	virtual idx_t Read(ColumnReaderInput &input, Vector &result);
	virtual void Select(ColumnReaderInput &input, Vector &result, const SelectionVector &sel,
	                    idx_t approved_tuple_count);
	virtual void Filter(ColumnReaderInput &input, Vector &result, const TableFilter &filter,
	                    TableFilterState &filter_state, SelectionVector &sel, idx_t &approved_tuple_count,
	                    bool is_first_filter);
	static void ApplyFilter(Vector &v, const TableFilter &filter, TableFilterState &filter_state, idx_t scan_count,
	                        SelectionVector &sel, idx_t &approved_tuple_count);
	virtual void Skip(idx_t num_values);

	const ParquetReader &Reader();
	const LogicalType &Type() const {
		return column_schema.type;
	}
	const ParquetColumnSchema &Schema() const {
		return column_schema;
	}

	inline idx_t ColumnIndex() const {
		return column_schema.column_index;
	}
	inline idx_t MaxDefine() const {
		return column_schema.max_define;
	}
	idx_t MaxRepeat() const {
		return column_schema.max_repeat;
	}
	inline bool IsSkipped() const {
		return !chunk;
	}
	//! Set the parent reader (the composite reader this reader is a child of). Top-level readers have no parent.
	void SetParent(ColumnReader &parent_p) {
		parent = parent_p;
	}
	//! Whether this is a top-level reader (i.e. it has no parent reader)
	bool IsRoot() const {
		return !parent;
	}
	//! Whether every value in the current row group's column chunk is NULL (according to its statistics)
	bool AllValuesAreNull() const;

	void InitializeCryptoMetadata(const duckdb_parquet::EncryptionAlgorithm &encryption_algorithm,
	                              idx_t row_group_ordinal_p) {
		std::string unique_file_identifier;
		if (encryption_algorithm.__isset.AES_GCM_V1) {
			unique_file_identifier = encryption_algorithm.AES_GCM_V1.aad_file_unique;
		} else if (encryption_algorithm.__isset.AES_GCM_CTR_V1) {
			throw InvalidInputException("File is encrypted with AES_GCM_CTR_V1, but this is not supported by DuckDB");
		} else {
			throw InvalidInputException("File is encrypted but no encryption algorithm is set");
		}

		aad_crypto_metadata.Initialize(unique_file_identifier, row_group_ordinal_p, ColumnIndex());
	}

	virtual idx_t FileOffset() const;
	//! Adds the indexes of the row group's column chunks this reader reads
	virtual void GatherColumnChunks(unordered_set<idx_t> &column_chunks);
	virtual idx_t GroupRowsAvailable();

	// register the range this reader will touch for prefetching
	virtual void RegisterPrefetch(ThriftFileTransport &transport, bool allow_merge);
	//! The first offset RegisterPrefetch registered: the chunk start, or the first needed page with a page index
	idx_t PrefetchOffset() const;
	//! Hands the reader the page index of the row group, so it can prefetch and read only the pages whose rows may
	//! match the filters. Called after InitializeRead, only for row groups where the page index prunes pages.
	virtual void InitializePageIndex(const ParquetRowGroupPageIndex &page_index);

	virtual unique_ptr<BaseStatistics> Stats(idx_t row_group_idx_p, const duckdb_parquet::ColumnChunkList &columns);
	void ValidateColumnMetadata(idx_t row_group_num_rows, const ColumnChunk &column);

	template <class VALUE_TYPE, class CONVERSION, bool HAS_DEFINES>
	void PlainTemplatedDefines(ByteBuffer &plain_data, const uint8_t *defines, uint64_t num_values, idx_t result_offset,
	                           Vector &result) {
		if (CONVERSION::PlainAvailable(plain_data, num_values)) {
			PlainTemplatedInternal<VALUE_TYPE, CONVERSION, HAS_DEFINES, false>(plain_data, defines, num_values,
			                                                                   result_offset, result);
		} else {
			PlainTemplatedInternal<VALUE_TYPE, CONVERSION, HAS_DEFINES, true>(plain_data, defines, num_values,
			                                                                  result_offset, result);
		}
	}
	template <class VALUE_TYPE, class CONVERSION>
	void PlainTemplated(ByteBuffer &plain_data, const uint8_t *defines, uint64_t num_values, idx_t result_offset,
	                    Vector &result) {
		if (HasDefines() && defines) {
			PlainTemplatedDefines<VALUE_TYPE, CONVERSION, true>(plain_data, defines, num_values, result_offset, result);
		} else {
			PlainTemplatedDefines<VALUE_TYPE, CONVERSION, false>(plain_data, defines, num_values, result_offset,
			                                                     result);
		}
	}

	template <class CONVERSION, bool HAS_DEFINES>
	void PlainSkipTemplatedDefines(ByteBuffer &plain_data, const uint8_t *defines, uint64_t num_values) {
		if (CONVERSION::PlainAvailable(plain_data, num_values)) {
			PlainSkipTemplatedInternal<CONVERSION, HAS_DEFINES, false>(plain_data, defines, num_values);
		} else {
			PlainSkipTemplatedInternal<CONVERSION, HAS_DEFINES, true>(plain_data, defines, num_values);
		}
	}
	template <class CONVERSION>
	void PlainSkipTemplated(ByteBuffer &plain_data, const uint8_t *defines, uint64_t num_values) {
		if (HasDefines() && defines) {
			PlainSkipTemplatedDefines<CONVERSION, true>(plain_data, defines, num_values);
		} else {
			PlainSkipTemplatedDefines<CONVERSION, false>(plain_data, defines, num_values);
		}
	}

	template <class VALUE_TYPE, class CONVERSION>
	void PlainSelectTemplated(ByteBuffer &plain_data, const uint8_t *defines, uint64_t num_values, Vector &result,
	                          const SelectionVector &sel, idx_t approved_tuple_count) {
		if (HasDefines() && defines) {
			PlainSelectTemplatedInternal<VALUE_TYPE, CONVERSION, true, true>(plain_data, defines, num_values, result,
			                                                                 sel, approved_tuple_count);
		} else {
			PlainSelectTemplatedInternal<VALUE_TYPE, CONVERSION, false, true>(plain_data, defines, num_values, result,
			                                                                  sel, approved_tuple_count);
		}
	}

	idx_t GetValidCount(uint8_t *defines, idx_t count, idx_t offset = 0) const {
		if (!defines) {
			return count;
		}
		idx_t valid_count = 0;
		for (idx_t i = offset; i < offset + count; i++) {
			valid_count += defines[i] == MaxDefine();
		}
		return valid_count;
	}

protected:
	virtual bool SupportsDirectFilter() const {
		return false;
	}
	virtual bool SupportsDirectSelect() const {
		return false;
	}
	void DirectFilter(ColumnReaderInput &input, Vector &result, const TableFilter &filter,
	                  TableFilterState &filter_state, SelectionVector &sel, idx_t &approved_tuple_count);
	void DirectSelect(ColumnReaderInput &input, Vector &result, const SelectionVector &sel, idx_t approved_tuple_count);
	void ReadEncrypted(duckdb_apache::thrift::TBase &object);
	void ReadDataEncrypted(const data_ptr_t buffer, const uint32_t buffer_size, PageType::type module);
	void Read(PageHeader &page_hdr);
	void ReadData(const data_ptr_t buffer, const uint32_t buffer_size, PageType::type page_type);

private:
	//! this function tries to skip a page in the below conditions:
	//! 1. a previous table filter has filtered out this page
	//! 2. page statistics can be used to skip page
	bool PageIsFilteredOut(PageHeader &page_hdr, optional_ptr<const TableFilter> filter);
	void BeginRead(data_ptr_t define_out, data_ptr_t repeat_out);
	void FinishRead(idx_t read_count);
	idx_t ReadPageHeaders(idx_t max_read, optional_ptr<const TableFilter> filter = nullptr,
	                      optional_ptr<TableFilterState> filter_state = nullptr, idx_t rows_to_skip = 0);
	idx_t ReadInternal(ColumnReaderInput &input, Vector &result);
	//! Prepare a read of up to "max_read" rows and read the defines/repeats.
	//! Returns whether all values are valid (i.e., not NULL)
	bool PrepareRead(idx_t read_count, data_ptr_t define_out, data_ptr_t repeat_out, idx_t result_offset);
	void ReadData(idx_t read_now, data_ptr_t define_out, data_ptr_t repeat_out, Vector &result, idx_t result_offset);

	template <class VALUE_TYPE, class CONVERSION, bool HAS_DEFINES, bool CHECKED>
	void PlainTemplatedInternal(ByteBuffer &plain_data, const uint8_t *__restrict defines, const uint64_t num_values,
	                            const idx_t result_offset, Vector &result) {
		auto result_ptr = FlatVector::GetDataMutable<VALUE_TYPE>(result);
		if (!HAS_DEFINES && !CHECKED && CONVERSION::PlainConstantSize() == sizeof(VALUE_TYPE)) {
			// we can memcpy
			idx_t copy_count = num_values * CONVERSION::PlainConstantSize();
			if (copy_count > 0) {
				memcpy(result_ptr + result_offset, plain_data.ptr, copy_count);
				plain_data.unsafe_inc(copy_count);
			}
			return;
		}
		auto &result_mask = FlatVector::ValidityMutable(result);
		for (idx_t row_idx = result_offset; row_idx < result_offset + num_values; row_idx++) {
			if (HAS_DEFINES && defines[row_idx] != MaxDefine()) {
				result_mask.SetInvalid(row_idx);
				continue;
			}
			result_ptr[row_idx] = CONVERSION::template PlainRead<CHECKED>(plain_data, *this);
		}
	}

	template <class CONVERSION, bool HAS_DEFINES, bool CHECKED>
	void PlainSkipTemplatedInternal(ByteBuffer &plain_data, const uint8_t *__restrict defines,
	                                const uint64_t num_values, idx_t row_offset = 0) {
		if (!HAS_DEFINES && CONVERSION::PlainConstantSize() > 0) {
			if (CHECKED) {
				plain_data.inc(num_values * CONVERSION::PlainConstantSize());
			} else {
				plain_data.unsafe_inc(num_values * CONVERSION::PlainConstantSize());
			}
			return;
		}
		for (idx_t row_idx = row_offset; row_idx < row_offset + num_values; row_idx++) {
			if (HAS_DEFINES && defines[row_idx] != MaxDefine()) {
				continue;
			}
			CONVERSION::template PlainSkip<CHECKED>(plain_data, *this);
		}
	}

	//! The caller wraps the result in a dictionary that still spans the rows we skipped over, so those rows must hold
	//! a defined value instead of uninitialized memory - zero is an empty string for string_t, and 0 otherwise
	template <class VALUE_TYPE>
	static void ZeroSkippedRows(VALUE_TYPE *result_ptr, idx_t row_offset, idx_t skip_count) {
		memset(result_ptr + row_offset, 0, sizeof(VALUE_TYPE) * skip_count);
	}

	template <class VALUE_TYPE, class CONVERSION, bool HAS_DEFINES, bool CHECKED>
	void PlainSelectTemplatedInternal(ByteBuffer &plain_data, const uint8_t *__restrict defines,
	                                  const uint64_t num_values, Vector &result, const SelectionVector &sel,
	                                  idx_t approved_tuple_count) {
		auto result_ptr = FlatVector::GetDataMutable<VALUE_TYPE>(result);
		auto &result_mask = FlatVector::ValidityMutable(result);
		idx_t current_entry = 0;
		for (idx_t i = 0; i < approved_tuple_count; i++) {
			auto next_entry = sel.get_index(i);
			D_ASSERT(current_entry <= next_entry);
			// perform any skips forward if required
			PlainSkipTemplatedInternal<CONVERSION, HAS_DEFINES, CHECKED>(plain_data, defines,
			                                                             next_entry - current_entry, current_entry);
			ZeroSkippedRows<VALUE_TYPE>(result_ptr, current_entry, next_entry - current_entry);
			// read this row
			if (HAS_DEFINES && defines[next_entry] != MaxDefine()) {
				result_mask.SetInvalid(next_entry);
			} else {
				result_ptr[next_entry] = CONVERSION::template PlainRead<CHECKED>(plain_data, *this);
			}
			current_entry = next_entry + 1;
		}
		if (current_entry < num_values) {
			// skip forward to the end of where we are selecting
			PlainSkipTemplatedInternal<CONVERSION, HAS_DEFINES, CHECKED>(plain_data, defines,
			                                                             num_values - current_entry, current_entry);
			ZeroSkippedRows<VALUE_TYPE>(result_ptr, current_entry, num_values - current_entry);
		}
	}

protected:
	Allocator &GetAllocator();
	// readers that use the default Read() need to implement those
	virtual void PlainSkip(ByteBuffer &plain_data, uint8_t *defines, idx_t num_values);
	virtual void Plain(ByteBuffer &plain_data, uint8_t *defines, idx_t num_values, idx_t result_offset, Vector &result);
	virtual void Plain(shared_ptr<ResizeableBuffer> &plain_data, uint8_t *defines, idx_t num_values,
	                   idx_t result_offset, Vector &result);
	virtual void PlainSelect(shared_ptr<ResizeableBuffer> &plain_data, uint8_t *defines, idx_t num_values,
	                         Vector &result, const SelectionVector &sel, idx_t count);

	// applies any skips that were registered using Skip()
	virtual void ApplyPendingSkips(data_ptr_t define_out, data_ptr_t repeat_out);

	inline bool HasDefines() const {
		return MaxDefine() > 0;
	}

	inline bool HasRepeats() const {
		return MaxRepeat() > 0;
	}

protected:
	const ParquetColumnSchema &column_schema;

	const ParquetReader &reader;
	idx_t pending_skips = 0;
	bool page_is_filtered_out = false;

	virtual void ResetPage();

private:
	void AllocateBlock(idx_t size);
	void PrepareRead(optional_ptr<const TableFilter> filter, optional_ptr<TableFilterState> filter_state,
	                 idx_t rows_to_skip = 0);
	void PreparePage(PageHeader &page_hdr);
	void PrepareDataPage(PageHeader &page_hdr);
	void PreparePageV2(PageHeader &page_hdr);
	//! The size of the page body in the file, without the encryption framing that Arrow includes
	uint32_t GetPageBodySize(const PageHeader &page_hdr) const;
	void DecompressInternal(CompressionCodec::type codec, const_data_ptr_t src, idx_t src_size, data_ptr_t dst,
	                        idx_t dst_size);
	const ColumnChunk *chunk = nullptr;
	//! The composite reader this reader is a child of (struct/list/variant/expression). Null for top-level readers.
	optional_ptr<ColumnReader> parent;

	TProtocol *protocol;
	idx_t page_rows_available;
	idx_t group_rows_available;
	idx_t chunk_read_offset;

	//! The OffsetIndex of the column chunk and the rows of the row group that may match the filters (both owned by
	//! the scan state), set only when the page index can be used to skip pages of this column chunk
	optional_ptr<const duckdb_parquet::OffsetIndex> offset_index;
	optional_ptr<const vector<ParquetPageRowRange>> page_index_rows;
	//! The first offset registered for prefetching, when it is not the chunk start
	optional_idx prefetch_offset;
	//! Whether the rows [begin, end) of the row group are needed according to the page index
	bool PageIndexNeedsRows(idx_t begin, idx_t end) const;
	//! Skips pages using the OffsetIndex instead of reading their headers; returns false when no page can be skipped
	bool TrySkipPagesWithOffsetIndex(idx_t skipped_so_far, idx_t &to_skip);
	//! Throws when the row count the OffsetIndex gives for the page at this location disagrees with its header
	void VerifyPageAgainstOffsetIndex(idx_t header_location, const PageHeader &page_hdr);

	shared_ptr<ResizeableBuffer> block;

	ColumnEncoding encoding = ColumnEncoding::INVALID;
	unique_ptr<RleBpDecoder> defined_decoder;
	unique_ptr<RleBpDecoder> repeated_decoder;
	DictionaryDecoder dictionary_decoder;
	DeltaBinaryPackedDecoder delta_binary_packed_decoder;
	RLEDecoder rle_decoder;
	DeltaLengthByteArrayDecoder delta_length_byte_array_decoder;
	DeltaByteArrayDecoder delta_byte_array_decoder;
	ByteStreamSplitDecoder byte_stream_split_decoder;
	CryptoMetaData aad_crypto_metadata;
	//! Backs the Thrift page header of the page that is currently being read
	ArenaAllocator page_header_arena;

	//! Resizeable buffers used for the various encodings above
	ResizeableBuffer encoding_buffers[2];

public:
	template <class TARGET>
	TARGET &Cast() {
		if (TARGET::TYPE != PhysicalType::INVALID && Type().InternalType() != TARGET::TYPE) {
			throw InternalException("Failed to cast column reader to type - type mismatch");
		}
		return reinterpret_cast<TARGET &>(*this);
	}

	template <class TARGET>
	const TARGET &Cast() const {
		if (TARGET::TYPE != PhysicalType::INVALID && Type().InternalType() != TARGET::TYPE) {
			throw InternalException("Failed to cast column reader to type - type mismatch");
		}
		return reinterpret_cast<const TARGET &>(*this);
	}
};

} // namespace duckdb
