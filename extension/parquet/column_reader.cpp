#include "column_reader.hpp"

#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/constant_vector.hpp"

#include <algorithm>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "parquet_statistics.hpp"

#include "reader/boolean_column_reader.hpp"
#include "brotli/decode.h"
#include "reader/callback_column_reader.hpp"
#include "reader/interval_column_reader.hpp"
#include "lz4.hpp"
#include "miniz_wrapper.hpp"
#include "reader/null_column_reader.hpp"
#include "parquet_reader.hpp"
#include "parquet_timestamp.hpp"
#include "parquet_float16.hpp"
#include "snappy.h"
#include "reader/string_column_reader.hpp"
#include "reader/templated_column_reader.hpp"
#include "reader/uuid_column_reader.hpp"
#include "zstd.h"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/storage/table/column_segment.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "parquet_crypto.hpp"
#include "decode_utils.hpp"
#include "duckdb/common/enums/vector_type.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/datetime.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector/unified_vector_format.hpp"
#include "duckdb/common/vector_size.hpp"
#include "parquet_decimal_utils.hpp"
#include "parquet_file_metadata_cache.hpp"
#include "parquet_rle_bp_decoder.hpp"
#include "thrift/protocol/TProtocol.h"
#include "thrift_tools.hpp"

namespace duckdb_apache {
namespace thrift {
class TBase;
} // namespace thrift
} // namespace duckdb_apache

namespace duckdb {
class Allocator;
struct hugeint_t;

using duckdb_parquet::CompressionCodec;
using duckdb_parquet::ConvertedType;
using duckdb_parquet::Encoding;
using duckdb_parquet::PageType;
using duckdb_parquet::Type;

const uint64_t ParquetDecodeUtils::BITPACK_MASKS[] = {0,
                                                      1,
                                                      3,
                                                      7,
                                                      15,
                                                      31,
                                                      63,
                                                      127,
                                                      255,
                                                      511,
                                                      1023,
                                                      2047,
                                                      4095,
                                                      8191,
                                                      16383,
                                                      32767,
                                                      65535,
                                                      131071,
                                                      262143,
                                                      524287,
                                                      1048575,
                                                      2097151,
                                                      4194303,
                                                      8388607,
                                                      16777215,
                                                      33554431,
                                                      67108863,
                                                      134217727,
                                                      268435455,
                                                      536870911,
                                                      1073741823,
                                                      2147483647,
                                                      4294967295,
                                                      8589934591,
                                                      17179869183,
                                                      34359738367,
                                                      68719476735,
                                                      137438953471,
                                                      274877906943,
                                                      549755813887,
                                                      1099511627775,
                                                      2199023255551,
                                                      4398046511103,
                                                      8796093022207,
                                                      17592186044415,
                                                      35184372088831,
                                                      70368744177663,
                                                      140737488355327,
                                                      281474976710655,
                                                      562949953421311,
                                                      1125899906842623,
                                                      2251799813685247,
                                                      4503599627370495,
                                                      9007199254740991,
                                                      18014398509481983,
                                                      36028797018963967,
                                                      72057594037927935,
                                                      144115188075855871,
                                                      288230376151711743,
                                                      576460752303423487,
                                                      1152921504606846975,
                                                      2305843009213693951,
                                                      4611686018427387903,
                                                      9223372036854775807,
                                                      18446744073709551615ULL};

const uint64_t ParquetDecodeUtils::BITPACK_MASKS_SIZE = sizeof(ParquetDecodeUtils::BITPACK_MASKS) / sizeof(uint64_t);

const uint8_t ParquetDecodeUtils::BITPACK_DLEN = 8;

ColumnReader::ColumnReader(const ParquetReader &reader, const ParquetColumnSchema &schema_p)
    : column_schema(schema_p), reader(reader), page_rows_available(0), dictionary_decoder(*this),
      delta_binary_packed_decoder(*this), rle_decoder(*this), delta_length_byte_array_decoder(*this),
      delta_byte_array_decoder(*this), byte_stream_split_decoder(*this), aad_crypto_metadata(reader.allocator),
      page_header_arena(reader.allocator) {
}

ColumnReader::~ColumnReader() {
}

Allocator &ColumnReader::GetAllocator() {
	return reader.allocator;
}

const ParquetReader &ColumnReader::Reader() {
	return reader;
}

//! A column chunk with at least this many small cached blocks is read whole instead of page by page ...
static constexpr idx_t PAGE_INDEX_WHOLE_CHUNK_AFTER_CACHED_BLOCKS = 4;
//! ... unless it is larger than this share of the memory limit
static constexpr idx_t PAGE_INDEX_WHOLE_CHUNK_MAX_MEMORY_SHARE = 4;

//! Whether a chunk the cache already holds several small pieces of should be fetched whole instead of the given
//! (offset, size) page ranges: only when a range misses the cache, and only when the chunk fits the memory budget
static bool ShouldReadChunkWhole(CachingFileHandle &handle, idx_t chunk_start, idx_t chunk_size,
                                 const vector<pair<idx_t, idx_t>> &ranges) {
	if (handle.CountCachedBlocks(chunk_start, chunk_size) < PAGE_INDEX_WHOLE_CHUNK_AFTER_CACHED_BLOCKS) {
		return false;
	}
	bool all_cached = true;
	for (auto &range : ranges) {
		if (!handle.IsCached(range.first, range.second)) {
			all_cached = false;
			break;
		}
	}
	if (all_cached) {
		// the probe needs no I/O at all
		return false;
	}
	return chunk_size <= handle.GetMaxMemory() / PAGE_INDEX_WHOLE_CHUNK_MAX_MEMORY_SHARE;
}

idx_t ColumnReader::PrefetchOffset() const {
	return prefetch_offset.IsValid() ? prefetch_offset.GetIndex() : FileOffset();
}

void ColumnReader::RegisterPrefetch(ThriftFileTransport &transport, bool allow_merge) {
	prefetch_offset = optional_idx();
	if (!chunk) {
		return;
	}
	if (!offset_index) {
		uint64_t size = chunk->meta_data.total_compressed_size;
		if (size == 0) {
			// nothing to prefetch
			return;
		}
		transport.RegisterPrefetch(FileOffset(), size, allow_merge);
		return;
	}
	// the page index tells which pages hold rows that may match: the dictionary page (and anything else in front of
	// the first data page) plus only those pages - adjacent pages are coalesced by the transport
	auto &locations = offset_index->page_locations;
	const auto chunk_start = FileOffset();
	const auto chunk_size = NumericCast<idx_t>(chunk->meta_data.total_compressed_size);
	const auto first_data_page = NumericCast<idx_t>(locations[0].offset);
	vector<pair<idx_t, idx_t>> ranges;
	if (chunk_start < first_data_page) {
		ranges.emplace_back(chunk_start, first_data_page - chunk_start);
	}
	optional_idx first_page_offset;
	const auto row_group_rows = NumericCast<idx_t>(chunk->meta_data.num_values);
	for (idx_t page_idx = 0; page_idx < locations.size(); page_idx++) {
		const auto begin = NumericCast<idx_t>(locations[page_idx].first_row_index);
		const auto end = page_idx + 1 < locations.size() ? NumericCast<idx_t>(locations[page_idx + 1].first_row_index)
		                                                 : row_group_rows;
		if (!PageIndexNeedsRows(begin, end)) {
			continue;
		}
		const auto page_offset = NumericCast<idx_t>(locations[page_idx].offset);
		if (!first_page_offset.IsValid() && chunk_start >= first_data_page) {
			first_page_offset = page_offset;
		}
		ranges.emplace_back(page_offset, NumericCast<idx_t>(locations[page_idx].compressed_page_size));
	}
	// a chunk the cache already holds several small pieces of is being probed repeatedly: once a probe misses a page,
	// fetch the chunk whole, so this and every later read of it is served from the cache, as it would be without the
	// page index. The read re-fetches the cached pieces with the gaps around them, so it sends about as many requests
	// as on a cold cache
	if (ShouldReadChunkWhole(transport.GetCachingFileHandle(), chunk_start, chunk_size, ranges)) {
		transport.RegisterPrefetch(chunk_start, chunk_size, allow_merge, true);
		return;
	}
	prefetch_offset = first_page_offset;
	for (auto &range : ranges) {
		transport.RegisterPrefetch(range.first, range.second, allow_merge);
	}
}

void ColumnReader::InitializePageIndex(const ParquetRowGroupPageIndex &page_index) {
	// pages of a repeated column hold several values per row, and lists are skipped by reading them: no page skipping
	if (!chunk || MaxRepeat() != 0 || reader.parquet_options.encryption_config) {
		return;
	}
	if (!page_index.HasOffsetIndex(ColumnIndex())) {
		return;
	}
	offset_index = *page_index.offset_indexes[ColumnIndex()];
	page_index_rows = page_index.row_ranges;
}

void ColumnReader::VerifyPageAgainstOffsetIndex(idx_t header_location, const PageHeader &page_hdr) {
	// the page index is only used for non-repeated columns, whose pages hold one value per row
	auto &locations = offset_index->page_locations;
	auto it = std::lower_bound(locations.begin(), locations.end(), header_location,
	                           [](const duckdb_parquet::PageLocation &location, idx_t offset) {
		                           return NumericCast<idx_t>(location.offset) < offset;
	                           });
	if (it == locations.end() || NumericCast<idx_t>(it->offset) != header_location) {
		return;
	}
	const auto page_idx = NumericCast<idx_t>(it - locations.begin());
	const auto index_rows =
	    ParquetPageIndexUtils::PageRowCount(*offset_index, page_idx, NumericCast<idx_t>(chunk->meta_data.num_values));
	const auto page_values = page_hdr.type == PageType::DATA_PAGE
	                             ? NumericCast<idx_t>(page_hdr.data_page_header.num_values)
	                             : NumericCast<idx_t>(page_hdr.data_page_header_v2.num_values);
	if (page_values != index_rows) {
		throw InvalidInputException(
		    "Failed to read file \"%s\": the OffsetIndex of column \"%s\" is corrupt. Page %llu "
		    "should hold %llu rows according to the index, but holds %llu values. The file can be read without "
		    "its page index: SET parquet_use_page_index = false",
		    Reader().GetFileName(), Schema().name, page_idx, index_rows, page_values);
	}
}

bool ColumnReader::PageIndexNeedsRows(idx_t begin, idx_t end) const {
	D_ASSERT(page_index_rows);
	return ParquetPageIndexUtils::PageOverlapsRowRanges(*page_index_rows, begin, end);
}

bool ColumnReader::TrySkipPagesWithOffsetIndex(idx_t skipped_so_far, idx_t &to_skip) {
	if (!offset_index || page_rows_available != 0) {
		// no page index, or still inside a page
		return false;
	}
	auto &trans = reinterpret_cast<ThriftFileTransport &>(*protocol->getTransport());
	auto &locations = offset_index->page_locations;
	while (trans.GetLocation() < NumericCast<idx_t>(locations[0].offset)) {
		// read the dictionary page (and anything else in front of the first data page) before jumping
		PrepareRead(nullptr, nullptr, 0);
		if (page_rows_available != 0) {
			// a data page where the OffsetIndex has none: leave the page index alone
			return false;
		}
	}
	// a non-repeated column has one value per row: the value count tells the row the next page starts at
	const auto chunk_rows = NumericCast<idx_t>(chunk->meta_data.num_values);
	const auto current_row = chunk_rows - group_rows_available + skipped_so_far;
	const auto target_row = current_row + to_skip;
	if (target_row >= chunk_rows) {
		// everything that is left is skipped: move past the last page
		auto &last = locations.back();
		trans.SetLocation(NumericCast<idx_t>(last.offset) + NumericCast<idx_t>(last.compressed_page_size));
		to_skip = 0;
		return true;
	}
	// the last page that starts at or before the target row
	auto it = std::upper_bound(locations.begin(), locations.end(), target_row,
	                           [](idx_t row, const duckdb_parquet::PageLocation &location) {
		                           return row < NumericCast<idx_t>(location.first_row_index);
	                           });
	D_ASSERT(it != locations.begin());
	auto &target_page = *(it - 1);
	const auto target_page_row = NumericCast<idx_t>(target_page.first_row_index);
	if (target_page_row <= current_row) {
		// the target lies in the page that starts here - read its header as usual
		return false;
	}
	trans.SetLocation(NumericCast<idx_t>(target_page.offset));
	to_skip -= target_page_row - current_row;
	return true;
}

unique_ptr<BaseStatistics> ColumnReader::Stats(idx_t row_group_idx_p, const duckdb_parquet::ColumnChunkList &columns) {
	return Schema().Stats(*reader.GetFileMetadata(), reader.parquet_options, row_group_idx_p, columns);
}

void ColumnReader::ValidateColumnMetadata(idx_t row_group_num_rows, const ColumnChunk &column) {
	Schema().ValidateColumnMetadata(column, NumericCast<int64_t>(row_group_num_rows), IsRoot(),
	                                Reader().GetFileName().c_str());
}

void ColumnReader::GatherColumnChunks(unordered_set<idx_t> &column_chunks) {
	if (IsSkipped()) {
		return;
	}
	column_chunks.insert(ColumnIndex());
}

// Note: It's not trivial to determine where all Column data is stored. Chunk->file_offset
// apparently is not the first page of the data. Therefore we determine the address of the first page by taking the
// minimum of all page offsets.
idx_t ColumnReader::FileOffset() const {
	if (IsSkipped()) {
		//! This column reader is skipped
		return 0;
	}
	auto min_offset = NumericLimits<idx_t>::Maximum();
	if (chunk->meta_data.__isset.dictionary_page_offset) {
		if (chunk->meta_data.dictionary_page_offset < 0) {
			throw InvalidInputException("Failed to read file \"%s\": metadata is corrupt. Column has invalid "
			                            "dictionary page offset (%lld)",
			                            reader.GetFileName(), chunk->meta_data.dictionary_page_offset);
		}
		min_offset = MinValue<idx_t>(min_offset, NumericCast<idx_t>(chunk->meta_data.dictionary_page_offset));
	}
	if (chunk->meta_data.__isset.index_page_offset) {
		if (chunk->meta_data.index_page_offset < 0) {
			throw InvalidInputException("Failed to read file \"%s\": metadata is corrupt. Column has invalid "
			                            "index page offset (%lld)",
			                            reader.GetFileName(), chunk->meta_data.index_page_offset);
		}
		min_offset = MinValue<idx_t>(min_offset, NumericCast<idx_t>(chunk->meta_data.index_page_offset));
	}
	if (chunk->meta_data.data_page_offset < 0) {
		throw InvalidInputException("Failed to read file \"%s\": metadata is corrupt. Column has invalid "
		                            "data page offset (%lld)",
		                            reader.GetFileName(), chunk->meta_data.data_page_offset);
	}
	min_offset = MinValue<idx_t>(min_offset, NumericCast<idx_t>(chunk->meta_data.data_page_offset));

	return min_offset;
}

idx_t ColumnReader::GroupRowsAvailable() {
	return group_rows_available;
}

bool ColumnReader::AllValuesAreNull() const {
	// for repeated columns the null_count/num_values statistics do not reliably indicate that every value is NULL
	// (num_values counts leaf slots, not rows), so we only trust this for non-repeated columns
	if (MaxRepeat() != 0 || !chunk || !chunk->__isset.meta_data) {
		return false;
	}
	auto &chunk_meta = chunk->meta_data;
	if (!chunk_meta.__isset.statistics || !chunk_meta.statistics.__isset.null_count) {
		return false;
	}
	return chunk_meta.statistics.null_count == chunk_meta.num_values;
}

void ColumnReader::PlainSkip(ByteBuffer &plain_data, uint8_t *defines, idx_t num_values) {
	throw NotImplementedException("PlainSkip not implemented");
}

void ColumnReader::Plain(ByteBuffer &plain_data, uint8_t *defines, idx_t num_values, // NOLINT
                         idx_t result_offset, Vector &result) {
	throw NotImplementedException("Plain not implemented");
}

void ColumnReader::Plain(shared_ptr<ResizeableBuffer> &plain_data, uint8_t *defines, idx_t num_values,
                         idx_t result_offset, Vector &result) {
	Plain(*plain_data, defines, num_values, result_offset, result);
}

void ColumnReader::PlainSelect(shared_ptr<ResizeableBuffer> &plain_data, uint8_t *defines, idx_t num_values,
                               Vector &result, const SelectionVector &sel, idx_t count) {
	throw NotImplementedException("PlainSelect not implemented");
}

void ColumnReader::InitializeRead(idx_t row_group_idx_p, idx_t row_group_num_rows,
                                  const duckdb_parquet::ColumnChunkList &columns, TProtocol &protocol_p) {
	D_ASSERT(ColumnIndex() < columns.size());
	chunk = &columns[ColumnIndex()];
	protocol = &protocol_p;
	D_ASSERT(chunk);
	if (chunk->__isset.crypto_metadata && !reader.metadata->crypto_metadata) {
		throw InvalidInputException("File is encrypted but no file crypto metadata is set");
	}
	if (!chunk->__isset.meta_data) {
		throw InvalidInputException("Failed to read file \"%s\": column chunk is missing its metadata",
		                            Reader().GetFileName());
	}

	if (chunk->__isset.file_path) {
		throw InvalidInputException("Failed to read file \"%s\": Only inlined data files are supported (no references)",
		                            Reader().GetFileName());
	}

	if (chunk->meta_data.data_page_offset < 0) {
		throw InvalidInputException("Failed to read file \"%s\": metadata is corrupt. Column has invalid "
		                            "data page offset (%lld)",
		                            Reader().GetFileName(), chunk->meta_data.data_page_offset);
	}
	offset_index = nullptr;
	page_index_rows = nullptr;
	// ugh. sometimes there is an extra offset for the dict. sometimes it's wrong.
	chunk_read_offset = NumericCast<idx_t>(chunk->meta_data.data_page_offset);
	if (chunk->meta_data.__isset.dictionary_page_offset && chunk->meta_data.dictionary_page_offset >= 4) {
		// this assumes the data pages follow the dict pages directly.
		chunk_read_offset = NumericCast<idx_t>(chunk->meta_data.dictionary_page_offset);
	}
	ValidateColumnMetadata(row_group_num_rows, *chunk);
	group_rows_available = NumericCast<idx_t>(chunk->meta_data.num_values);
}

bool ColumnReader::PageIsFilteredOut(PageHeader &page_hdr, optional_ptr<const TableFilter> filter) {
	if (page_hdr.type != PageType::DATA_PAGE && page_hdr.type != PageType::DATA_PAGE_V2) {
		// we can only filter out data pages
		return false;
	}
	bool is_v1 = page_hdr.type == PageType::DATA_PAGE;
	auto &v1_header = page_hdr.data_page_header;
	auto &v2_header = page_hdr.data_page_header_v2;
	auto page_encoding = is_v1 ? v1_header.encoding : v2_header.encoding;

	if (page_encoding == Encoding::PLAIN_DICTIONARY || page_encoding == Encoding::RLE_DICTIONARY) {
		if (!dictionary_decoder.HasFilteredOutAllValues()) {
			return false;
		}
		encoding = ColumnEncoding::DICTIONARY;
		page_is_filtered_out = true;
	} else if (filter) {
		// try to use page statistics to skip this page if could.
		const duckdb_parquet::Statistics *page_stats = nullptr;
		if (is_v1 && v1_header.__isset.statistics) {
			page_stats = &v1_header.statistics;
		} else if (!is_v1 && v2_header.__isset.statistics) {
			page_stats = &v2_header.statistics;
		}

		if (!page_stats || !((page_stats->__isset.min_value || page_stats->__isset.min) &&
		                     (page_stats->__isset.max_value || page_stats->__isset.max))) {
			return false;
		}
		auto stats =
		    ParquetStatisticsUtils::TransformParquetStatistics(Type(), Schema(), *page_stats, /*can_have_nan=*/true);
		auto &expr_filter = filter->Cast<ExpressionFilter>();
		if (stats) {
			auto prune_result = expr_filter.CheckStatistics(*stats);
			if (prune_result == FilterPropagateResult::FILTER_ALWAYS_FALSE ||
			    prune_result == FilterPropagateResult::FILTER_FALSE_OR_NULL) {
				page_is_filtered_out = true;
			}
		}
	}
	if (page_is_filtered_out) {
		// the page has been filtered out!
		// skip forward
		auto &trans = reinterpret_cast<ThriftFileTransport &>(*protocol->getTransport());
		trans.Skip(page_hdr.compressed_page_size);
		page_rows_available = is_v1 ? v1_header.num_values : v2_header.num_values;
	}

	return page_is_filtered_out;
}

void ColumnReader::ReadEncrypted(duckdb_apache::thrift::TBase &object) {
	aad_crypto_metadata.module = ParquetCrypto::GetModuleHeader(*chunk, aad_crypto_metadata.page_ordinal);
	aad_crypto_metadata.page_ordinal =
	    ParquetCrypto::GetFinalPageOrdinal(*chunk, aad_crypto_metadata.module, aad_crypto_metadata.page_ordinal);
	reader.ReadEncrypted(object, *protocol, aad_crypto_metadata);
}

void ColumnReader::ReadDataEncrypted(const data_ptr_t buffer, const uint32_t buffer_size, PageType::type page_type) {
	aad_crypto_metadata.module = ParquetCrypto::GetModule(*chunk, page_type, aad_crypto_metadata.page_ordinal);
	aad_crypto_metadata.page_ordinal =
	    ParquetCrypto::GetFinalPageOrdinal(*chunk, aad_crypto_metadata.module, aad_crypto_metadata.page_ordinal);
	reader.ReadDataEncrypted(*protocol, buffer, buffer_size, aad_crypto_metadata);
}

void ColumnReader::Read(PageHeader &page_hdr) {
	if (reader.parquet_options.encryption_config) {
		ReadEncrypted(page_hdr);
	} else {
		reader.Read(page_hdr, *protocol);
	}
}

void ColumnReader::ReadData(const data_ptr_t buffer, const uint32_t buffer_size, PageType::type page_type) {
	if (reader.parquet_options.encryption_config) {
		ReadDataEncrypted(buffer, buffer_size, page_type);
	} else {
		reader.ReadData(*protocol, buffer, buffer_size);
	}
}

void ColumnReader::PrepareRead(optional_ptr<const TableFilter> filter, optional_ptr<TableFilterState> filter_state,
                               idx_t rows_to_skip) {
	encoding = ColumnEncoding::INVALID;
	defined_decoder.reset();
	page_is_filtered_out = false;
	block.reset();
	page_header_arena.Reset();
	PageHeader page_hdr(page_header_arena);
	auto &trans = reinterpret_cast<ThriftFileTransport &>(*protocol->getTransport());
	const auto header_location = trans.GetLocation();

	if (trans.HasPrefetch()) {
		// Already has some data prefetched, let's not mess with it
		Read(page_hdr);
	} else {
		// No prefetch yet, prefetch the full header in one go (so thrift won't read byte-by-byte from storage)
		// 256 bytes should cover almost all headers (unless it's a V2 header with really LONG string statistics)
		static constexpr idx_t ASSUMED_HEADER_SIZE = 256;
		const auto prefetch_size = MinValue(trans.GetSize() - trans.GetLocation(), ASSUMED_HEADER_SIZE);
		trans.Prefetch(trans.GetLocation(), prefetch_size);
		Read(page_hdr);
		trans.ClearPrefetch();
	}
	// some basic sanity check
	if (page_hdr.compressed_page_size < 0 || page_hdr.uncompressed_page_size < 0) {
		throw InvalidInputException("Failed to read file \"%s\": Page sizes must be >= 0", Reader().GetFileName());
	}

	if (offset_index && (page_hdr.type == PageType::DATA_PAGE || page_hdr.type == PageType::DATA_PAGE_V2)) {
		VerifyPageAgainstOffsetIndex(header_location, page_hdr);
	}

	if (PageIsFilteredOut(page_hdr, filter)) {
		return;
	}

	if (rows_to_skip > 0 && (page_hdr.type == PageType::DATA_PAGE || page_hdr.type == PageType::DATA_PAGE_V2)) {
		bool is_v1 = page_hdr.type == PageType::DATA_PAGE;
		idx_t page_num_values =
		    NumericCast<idx_t>(is_v1 ? page_hdr.data_page_header.num_values : page_hdr.data_page_header_v2.num_values);
		if (rows_to_skip >= page_num_values) {
			trans.Skip(page_hdr.compressed_page_size);
			page_is_filtered_out = true;
			page_rows_available = page_num_values;
			return;
		}
	}

	switch (page_hdr.type) {
	case PageType::DATA_PAGE_V2:
		PreparePageV2(page_hdr);
		PrepareDataPage(page_hdr);
		break;
	case PageType::DATA_PAGE:
		PreparePage(page_hdr);
		PrepareDataPage(page_hdr);
		break;
	case PageType::DICTIONARY_PAGE: {
		PreparePage(page_hdr);
		auto dictionary_size = page_hdr.dictionary_page_header.num_values;
		if (dictionary_size < 0) {
			throw InvalidInputException("Failed to read file \"%s\": Invalid dictionary page header (num_values < 0)",
			                            Reader().GetFileName());
		}
		dictionary_decoder.InitializeDictionary(dictionary_size, filter, filter_state, HasDefines());
		break;
	}
	default:
		break; // ignore INDEX page type and any other custom extensions
	}
	ResetPage();
}

void ColumnReader::ResetPage() {
}

void ColumnReader::PreparePageV2(PageHeader &page_hdr) {
	D_ASSERT(page_hdr.type == PageType::DATA_PAGE_V2);

	AllocateBlock(NumericCast<idx_t>(page_hdr.uncompressed_page_size) + 1);
	const auto page_body_size = GetPageBodySize(page_hdr);
	bool uncompressed = false;
	if (page_hdr.data_page_header_v2.__isset.is_compressed && !page_hdr.data_page_header_v2.is_compressed) {
		uncompressed = true;
	}
	if (chunk->meta_data.codec == CompressionCodec::UNCOMPRESSED) {
		if (page_body_size != NumericCast<uint32_t>(page_hdr.uncompressed_page_size)) {
			// LCOV_EXCL_START
			const auto &file_name = Reader().GetFileName();
			throw InvalidInputException(
			    "Parquet file (%s) corrupted: uncompressed page size mismatch (expected %d, actual: %d)", file_name,
			    page_hdr.uncompressed_page_size, page_body_size);
			// LCOV_EXCL_STOP
		}
		uncompressed = true;
	}
	if (uncompressed) {
		ReadData(block->ptr, page_body_size, page_hdr.type);
		return;
	}

	// copy repeats & defines as-is because FOR SOME REASON they are uncompressed.
	// the page sizes are already validated >= 0 by the caller, but the level lengths are not, so guard
	// them here. with all four i32 header fields non-negative, the sum below cannot overflow once widened
	// to uint64_t, and the uint64_t casts in the comparisons below are safe.
	if (page_hdr.data_page_header_v2.repetition_levels_byte_length < 0 ||
	    page_hdr.data_page_header_v2.definition_levels_byte_length < 0) {
		// LCOV_EXCL_START
		throw InvalidInputException(
		    "Failed to read file \"%s\": header inconsistency, repetition_levels_byte_length and "
		    "definition_levels_byte_length must be >= 0",
		    Reader().GetFileName());
		// LCOV_EXCL_STOP
	}
	uint64_t uncompressed_bytes = static_cast<uint64_t>(page_hdr.data_page_header_v2.repetition_levels_byte_length) +
	                              page_hdr.data_page_header_v2.definition_levels_byte_length;
	if (uncompressed_bytes > static_cast<uint64_t>(page_hdr.uncompressed_page_size)) {
		// LCOV_EXCL_START
		throw InvalidInputException(
		    "Failed to read file \"%s\": header inconsistency, uncompressed_page_size needs to be larger than "
		    "repetition_levels_byte_length + definition_levels_byte_length",
		    Reader().GetFileName());
		// LCOV_EXCL_STOP
	}
	if (static_cast<uint64_t>(page_body_size) < uncompressed_bytes) {
		// LCOV_EXCL_START
		throw InvalidInputException(
		    "Failed to read file \"%s\": header inconsistency, compressed_page_size is smaller than "
		    "repetition_levels_byte_length + definition_levels_byte_length",
		    Reader().GetFileName());
		// LCOV_EXCL_STOP
	}

	// only the values are compressed, and an encrypted page body is a single module, so the body is read at once
	ResizeableBuffer page_buffer;
	page_buffer.resize(GetAllocator(), page_body_size + 1);
	ReadData(page_buffer.ptr, page_body_size, page_hdr.type);
	memcpy(block->ptr, page_buffer.ptr, uncompressed_bytes);

	auto compressed_bytes = page_body_size - uncompressed_bytes;

	if (compressed_bytes == 0 && static_cast<uint64_t>(page_hdr.uncompressed_page_size) > uncompressed_bytes) {
		// LCOV_EXCL_START
		throw InvalidInputException(
		    "Failed to read file \"%s\": header inconsistency, compressed_page_size is too small for the "
		    "declared value region",
		    Reader().GetFileName());
		// LCOV_EXCL_STOP
	}

	if (compressed_bytes > 0) {
		DecompressInternal(chunk->meta_data.codec, page_buffer.ptr + uncompressed_bytes, compressed_bytes,
		                   block->ptr + uncompressed_bytes, page_hdr.uncompressed_page_size - uncompressed_bytes);
	}
}

uint32_t ColumnReader::GetPageBodySize(const PageHeader &page_hdr) const {
	uint32_t page_body_size = page_hdr.compressed_page_size;
	if (chunk->__isset.crypto_metadata) {
		auto const file_aad = reader.GetUniqueFileIdentifier(reader.metadata->crypto_metadata->encryption_algorithm);
		if (!file_aad.empty()) {
			// If there is a file aad (identifier), this means that the Encrypted file is written by Arrow
			// Arrow adds the bytes for encryption (len + nonce + tag)
			// to the compressed page size
			const uint32_t framing =
			    ParquetCrypto::LENGTH_BYTES + ParquetCrypto::NONCE_BYTES + ParquetCrypto::TAG_BYTES;
			if (page_body_size < framing) {
				throw DataCorruptionException(
				    "Parquet file (%s) corrupted: compressed page size %d is smaller than the "
				    "encryption framing",
				    reader.GetFileName(), page_hdr.compressed_page_size);
			}
			page_body_size -= framing;
		}
	}
	return page_body_size;
}

void ColumnReader::AllocateBlock(idx_t size) {
	if (!block) {
		block = make_shared_ptr<ResizeableBuffer>(GetAllocator(), size);
	} else {
		block->resize(GetAllocator(), size);
	}
}

void ColumnReader::PreparePage(PageHeader &page_hdr) {
	AllocateBlock(NumericCast<idx_t>(page_hdr.uncompressed_page_size) + 1);
	const auto compressed_page_size = GetPageBodySize(page_hdr);

	if (chunk->meta_data.codec == CompressionCodec::UNCOMPRESSED) {
		if (compressed_page_size != NumericCast<uint32_t>(page_hdr.uncompressed_page_size)) {
			// LCOV_EXCL_START
			const auto &file_name = Reader().GetFileName();
			throw InvalidInputException(
			    "Parquet file (%s) corrupted: uncompressed page size mismatch (expected %d, actual: %d)", file_name,
			    page_hdr.uncompressed_page_size, compressed_page_size);
			// LCOV_EXCL_STOP
		}
		ReadData(block->ptr, compressed_page_size, page_hdr.type);
		return;
	}

	ResizeableBuffer compressed_buffer;
	compressed_buffer.resize(GetAllocator(), compressed_page_size + 1);
	ReadData(compressed_buffer.ptr, compressed_page_size, page_hdr.type);

	DecompressInternal(chunk->meta_data.codec, compressed_buffer.ptr, compressed_page_size, block->ptr,
	                   page_hdr.uncompressed_page_size);
}

static bool TryDecompressLZ4Hadoop(const_data_ptr_t src, idx_t src_size, data_ptr_t dst, idx_t dst_size) {
	// Hadoop framing stores a big-endian uncompressed block size, followed by one or more
	// chunks, each prefixed by its big-endian compressed size.
	while (src_size > 0) {
		if (src_size < sizeof(uint32_t)) {
			return false;
		}
		auto block_size = BSwapIfLE(Load<uint32_t>(src));
		src += sizeof(uint32_t);
		src_size -= sizeof(uint32_t);
		if (block_size > dst_size) {
			return false;
		}
		if (block_size == 0) {
			// Hadoop uses a zero-sized block to terminate an empty stream.
			return src_size == 0 && dst_size == 0;
		}
		idx_t remaining = block_size;
		while (remaining > 0) {
			if (src_size < sizeof(uint32_t)) {
				return false;
			}
			auto compressed_size = BSwapIfLE(Load<uint32_t>(src));
			src += sizeof(uint32_t);
			src_size -= sizeof(uint32_t);
			if (compressed_size == 0 || compressed_size > src_size) {
				return false;
			}
			auto decompressed_size =
			    duckdb_lz4::LZ4_decompress_safe(const_char_ptr_cast(src), char_ptr_cast(dst),
			                                    NumericCast<int>(compressed_size), NumericCast<int>(remaining));
			if (decompressed_size <= 0) {
				return false;
			}
			src += compressed_size;
			src_size -= compressed_size;
			dst += decompressed_size;
			dst_size -= decompressed_size;
			remaining -= decompressed_size;
		}
	}
	return dst_size == 0;
}

void ColumnReader::DecompressInternal(CompressionCodec::type codec, const_data_ptr_t src, idx_t src_size,
                                      data_ptr_t dst, idx_t dst_size) {
	switch (codec) {
	case CompressionCodec::UNCOMPRESSED:
		throw InternalException("Parquet data unexpectedly uncompressed");
	case CompressionCodec::GZIP: {
		MiniZStream s;
		s.Decompress(const_char_ptr_cast(src), src_size, char_ptr_cast(dst), dst_size);
		break;
	}
	case CompressionCodec::LZ4:
		if (TryDecompressLZ4Hadoop(src, src_size, dst, dst_size)) {
			break;
		}
		// Early parquet-cpp writers used raw LZ4 under the legacy codec identifier.
		DUCKDB_EXPLICIT_FALLTHROUGH;
	case CompressionCodec::LZ4_RAW: {
		auto res =
		    duckdb_lz4::LZ4_decompress_safe(const_char_ptr_cast(src), char_ptr_cast(dst),
		                                    UnsafeNumericCast<int32_t>(src_size), UnsafeNumericCast<int32_t>(dst_size));
		if (res != NumericCast<int>(dst_size)) {
			throw InvalidInputException("Failed to read file \"%s\": LZ4 decompression failure",
			                            Reader().GetFileName());
		}
		break;
	}
	case CompressionCodec::SNAPPY: {
		{
			size_t uncompressed_size = 0;
			auto res = duckdb_snappy::GetUncompressedLength(const_char_ptr_cast(src), src_size, &uncompressed_size);
			if (!res) {
				throw InvalidInputException("Failed to read file \"%s\": Snappy decompression failure",
				                            Reader().GetFileName());
			}
			if (uncompressed_size != dst_size) {
				throw InvalidInputException(
				    "Failed to read file \"%s\": Snappy decompression failure: Uncompressed data size mismatch",
				    Reader().GetFileName());
			}
		}
		auto res = duckdb_snappy::RawUncompress(const_char_ptr_cast(src), src_size, char_ptr_cast(dst));
		if (!res) {
			throw InvalidInputException("Failed to read file \"%s\": Snappy decompression failure",
			                            Reader().GetFileName());
		}
		break;
	}
	case CompressionCodec::ZSTD: {
		auto res = duckdb_zstd::ZSTD_decompress(dst, dst_size, src, src_size);
		if (duckdb_zstd::ZSTD_isError(res) || res != dst_size) {
			throw InvalidInputException("Failed to read file \"%s\": ZSTD Decompression failure",
			                            Reader().GetFileName());
		}
		break;
	}
	case CompressionCodec::BROTLI: {
		auto state = duckdb_brotli::BrotliDecoderCreateInstance(nullptr, nullptr, nullptr);
		size_t total_out = 0;
		auto src_size_size_t = NumericCast<size_t>(src_size);
		auto dst_size_size_t = NumericCast<size_t>(dst_size);

		auto res = duckdb_brotli::BrotliDecoderDecompressStream(state, &src_size_size_t, &src, &dst_size_size_t, &dst,
		                                                        &total_out);
		duckdb_brotli::BrotliDecoderDestroyInstance(state);
		if (res != duckdb_brotli::BROTLI_DECODER_RESULT_SUCCESS) {
			throw InvalidInputException("Failed to read file \"%s\": Brotli Decompression failure",
			                            Reader().GetFileName());
		}
		break;
	}

	default: { // LCOV_EXCL_START
		duckdb::stringstream codec_name;
		codec_name << codec;
		throw InvalidInputException("Failed to read file \"%s\": Unsupported compression codec \"%s\". Supported "
		                            "options are uncompressed, brotli, gzip, lz4, lz4_raw, snappy or zstd",
		                            Reader().GetFileName(), codec_name.str());
	} // LCOV_EXCL_STOP
	}
}

void ColumnReader::PrepareDataPage(PageHeader &page_hdr) {
	if (page_hdr.type == PageType::DATA_PAGE && !page_hdr.__isset.data_page_header) {
		throw InvalidInputException("Failed to read file \"%s\": Missing data page header from data page",
		                            Reader().GetFileName());
	}
	if (page_hdr.type == PageType::DATA_PAGE_V2 && !page_hdr.__isset.data_page_header_v2) {
		throw InvalidInputException("Failed to read file \"%s\": Missing data page header from data page v2",
		                            Reader().GetFileName());
	}

	bool is_v1 = page_hdr.type == PageType::DATA_PAGE;
	bool is_v2 = page_hdr.type == PageType::DATA_PAGE_V2;
	auto &v1_header = page_hdr.data_page_header;
	auto &v2_header = page_hdr.data_page_header_v2;

	page_rows_available = is_v1 ? v1_header.num_values : v2_header.num_values;
	auto page_encoding = is_v1 ? v1_header.encoding : v2_header.encoding;

	if (HasRepeats()) {
		uint32_t rep_length = is_v1 ? block->read<uint32_t>() : v2_header.repetition_levels_byte_length;
		block->available(rep_length);
		repeated_decoder =
		    make_uniq<RleBpDecoder>(block->ptr, rep_length, RleBpDecoder::ComputeBitWidthFromMaxValue(MaxRepeat()));
		block->inc(rep_length);
	} else if (is_v2 && v2_header.repetition_levels_byte_length > 0) {
		block->inc(v2_header.repetition_levels_byte_length);
	}

	if (HasDefines()) {
		uint32_t def_length = is_v1 ? block->read<uint32_t>() : v2_header.definition_levels_byte_length;
		block->available(def_length);
		defined_decoder =
		    make_uniq<RleBpDecoder>(block->ptr, def_length, RleBpDecoder::ComputeBitWidthFromMaxValue(MaxDefine()));
		block->inc(def_length);
	} else if (is_v2 && v2_header.definition_levels_byte_length > 0) {
		block->inc(v2_header.definition_levels_byte_length);
	}

	switch (page_encoding) {
	case Encoding::RLE_DICTIONARY:
	case Encoding::PLAIN_DICTIONARY: {
		encoding = ColumnEncoding::DICTIONARY;
		dictionary_decoder.InitializePage();
		break;
	}
	case Encoding::RLE: {
		encoding = ColumnEncoding::RLE;
		rle_decoder.InitializePage();
		break;
	}
	case Encoding::DELTA_BINARY_PACKED: {
		encoding = ColumnEncoding::DELTA_BINARY_PACKED;
		delta_binary_packed_decoder.InitializePage();
		break;
	}
	case Encoding::DELTA_LENGTH_BYTE_ARRAY: {
		encoding = ColumnEncoding::DELTA_LENGTH_BYTE_ARRAY;
		delta_length_byte_array_decoder.InitializePage();
		break;
	}
	case Encoding::DELTA_BYTE_ARRAY: {
		encoding = ColumnEncoding::DELTA_BYTE_ARRAY;
		delta_byte_array_decoder.InitializePage();
		break;
	}
	case Encoding::BYTE_STREAM_SPLIT: {
		encoding = ColumnEncoding::BYTE_STREAM_SPLIT;
		byte_stream_split_decoder.InitializePage();
		break;
	}
	case Encoding::PLAIN:
		// nothing to do here, will be read directly below
		encoding = ColumnEncoding::PLAIN;
		break;

	default:
		throw InvalidInputException("Failed to read file \"%s\": Unsupported page encoding", Reader().GetFileName());
	}
}

void ColumnReader::BeginRead(data_ptr_t define_out, data_ptr_t repeat_out) {
	// we need to reset the location because multiple column readers share the same protocol
	auto &trans = reinterpret_cast<ThriftFileTransport &>(*protocol->getTransport());
	trans.SetLocation(chunk_read_offset);

	// Perform any skips that were not applied yet.
	if (define_out && repeat_out) {
		ApplyPendingSkips(define_out, repeat_out);
	}
}

idx_t ColumnReader::ReadPageHeaders(idx_t max_read, optional_ptr<const TableFilter> filter,
                                    optional_ptr<TableFilterState> filter_state, idx_t rows_to_skip) {
	int8_t page_ordinal = 0;
	while (page_rows_available == 0) {
		aad_crypto_metadata.page_ordinal = page_ordinal;
		PrepareRead(filter, filter_state, rows_to_skip);
		page_ordinal++;
	}
	return MinValue<idx_t>(MinValue<idx_t>(max_read, page_rows_available), STANDARD_VECTOR_SIZE);
}

bool ColumnReader::PrepareRead(idx_t read_now, data_ptr_t define_out, data_ptr_t repeat_out, idx_t result_offset) {
	D_ASSERT(block);

	D_ASSERT(read_now + result_offset <= STANDARD_VECTOR_SIZE);
	D_ASSERT(!page_is_filtered_out);

	if (HasRepeats()) {
		D_ASSERT(repeated_decoder);
		repeated_decoder->GetBatch<uint8_t>(repeat_out + result_offset, read_now);
	}

	if (HasDefines()) {
		D_ASSERT(defined_decoder);
		const auto max_define = NumericCast<uint8_t>(MaxDefine());
		if (!HasRepeats() && defined_decoder->HasRepeatedBatch<uint8_t>(read_now, max_define)) {
			// Fast path: no repeats and all valid
			defined_decoder->GetRepeatedBatch<uint8_t>(read_now, max_define);
			return true;
		}
		defined_decoder->GetBatch<uint8_t>(define_out + result_offset, read_now);
		return false;
	}

	return true; // No defines, so everything is valid
}

void ColumnReader::ReadData(idx_t read_now, data_ptr_t define_out, data_ptr_t repeat_out, Vector &result,
                            idx_t result_offset) {
	// flatten the result vector if required
	if (result_offset != 0 && result.GetVectorType() != VectorType::FLAT_VECTOR) {
		result.Flatten();
		result.Reserve(STANDARD_VECTOR_SIZE);
	}
	if (page_is_filtered_out) {
		// page is filtered out - emit NULL for any rows
		auto &validity = FlatVector::ValidityMutable(result);
		for (idx_t i = 0; i < read_now; i++) {
			validity.SetInvalid(result_offset + i);
		}
		page_rows_available -= read_now;
		return;
	}
	// read the defines/repeats
	const auto all_valid = PrepareRead(read_now, define_out, repeat_out, result_offset);
	if (!IsRoot() && AllValuesAreNull()) {
		// every value is NULL: the parent still needs the define/repeat levels we just read, but there are no
		// values to decode - set the result to NULL and skip the encoding read
		if (result_offset == 0) {
			// we own the entire vector - emit a constant NULL
			ConstantVector::SetNull(result, count_t(read_now));
		} else {
			for (idx_t i = 0; i < read_now; i++) {
				FlatVector::SetNull(result, result_offset + i, true);
			}
		}
		page_rows_available -= read_now;
		return;
	}
	// read the data according to the encoder
	const auto define_ptr = all_valid ? nullptr : static_cast<uint8_t *>(define_out);
	switch (encoding) {
	case ColumnEncoding::DICTIONARY:
		dictionary_decoder.Read(define_ptr, read_now, result, result_offset);
		break;
	case ColumnEncoding::DELTA_BINARY_PACKED:
		delta_binary_packed_decoder.Read(define_ptr, read_now, result, result_offset);
		break;
	case ColumnEncoding::RLE:
		rle_decoder.Read(define_ptr, read_now, result, result_offset);
		break;
	case ColumnEncoding::DELTA_LENGTH_BYTE_ARRAY:
		delta_length_byte_array_decoder.Read(block, define_ptr, read_now, result, result_offset);
		break;
	case ColumnEncoding::DELTA_BYTE_ARRAY:
		delta_byte_array_decoder.Read(define_ptr, read_now, result, result_offset);
		break;
	case ColumnEncoding::BYTE_STREAM_SPLIT:
		byte_stream_split_decoder.Read(define_ptr, read_now, result, result_offset);
		break;
	default:
		Plain(block, define_ptr, read_now, result_offset, result);
		break;
	}
	page_rows_available -= read_now;
}

void ColumnReader::FinishRead(idx_t read_count) {
	auto &trans = reinterpret_cast<ThriftFileTransport &>(*protocol->getTransport());
	chunk_read_offset = trans.GetLocation();

	group_rows_available -= read_count;
}

idx_t ColumnReader::ReadInternal(ColumnReaderInput &input, Vector &result) {
	idx_t result_offset = 0;

	auto &num_values = input.num_values;
	auto &define_out = input.define_out;
	auto &repeat_out = input.repeat_out;

	auto to_read = num_values;
	D_ASSERT(to_read <= STANDARD_VECTOR_SIZE);

	while (to_read > 0) {
		auto read_now = ReadPageHeaders(to_read);

		ReadData(read_now, define_out, repeat_out, result, result_offset);

		result_offset += read_now;
		to_read -= read_now;
	}
	FinishRead(num_values);

	if (result.GetVectorType() == VectorType::FLAT_VECTOR) {
		FlatVector::SetSize(result, num_values);
	}

	return num_values;
}

idx_t ColumnReader::Read(ColumnReaderInput &input, Vector &result) {
	if (IsRoot() && AllValuesAreNull()) {
		// a top-level column that is entirely NULL - emit a constant NULL vector without reading anything.
		// (nested columns are handled in ReadData: they still need to emit their define/repeat levels)
		ConstantVector::SetNull(result, count_t(input.num_values));
		return input.num_values;
	}
	BeginRead(input.define_out, input.repeat_out);
	return ReadInternal(input, result);
}

void ColumnReader::Select(ColumnReaderInput &input, Vector &result, const SelectionVector &sel,
                          idx_t approved_tuple_count) {
	auto &num_values = input.num_values;
	if (SupportsDirectSelect() && approved_tuple_count < num_values && !(IsRoot() && AllValuesAreNull())) {
		DirectSelect(input, result, sel, approved_tuple_count);
		return;
	}
	Read(input, result);
}

void ColumnReader::DirectSelect(ColumnReaderInput &input, Vector &result, const SelectionVector &sel,
                                idx_t approved_tuple_count) {
	auto &num_values = input.num_values;
	auto &define_out = input.define_out;
	auto &repeat_out = input.repeat_out;

	auto to_read = num_values;

	// prepare the first read if we haven't yet
	BeginRead(define_out, repeat_out);
	auto read_now = ReadPageHeaders(to_read);

	// we can only push the filter into the decoder if we are reading the ENTIRE vector in one go
	if (read_now == to_read && encoding == ColumnEncoding::PLAIN) {
		const auto all_valid = PrepareRead(read_now, define_out, repeat_out, 0);
		const auto define_ptr = all_valid ? nullptr : static_cast<uint8_t *>(define_out);
		PlainSelect(block, define_ptr, read_now, result, sel, approved_tuple_count);

		page_rows_available -= read_now;
		FinishRead(to_read);
		return;
	}
	// fallback to regular read + filter
	ReadInternal(input, result);
}

void ColumnReader::Filter(ColumnReaderInput &input, Vector &result, const TableFilter &filter,
                          TableFilterState &filter_state, SelectionVector &sel, idx_t &approved_tuple_count,
                          bool is_first_filter) {
	auto &num_values = input.num_values;
	if (SupportsDirectFilter() && is_first_filter && !(IsRoot() && AllValuesAreNull())) {
		DirectFilter(input, result, filter, filter_state, sel, approved_tuple_count);
		return;
	}
	Select(input, result, sel, approved_tuple_count);
	ApplyFilter(result, filter, filter_state, num_values, sel, approved_tuple_count);
}

void ColumnReader::DirectFilter(ColumnReaderInput &input, Vector &result, const TableFilter &filter,
                                TableFilterState &filter_state, SelectionVector &sel, idx_t &approved_tuple_count) {
	auto &num_values = input.num_values;
	auto &define_out = input.define_out;
	auto &repeat_out = input.repeat_out;

	auto to_read = num_values;

	// prepare the first read if we haven't yet
	BeginRead(define_out, repeat_out);
	auto read_now = ReadPageHeaders(to_read, &filter, &filter_state);

	// we can only push the filter into the decoder if we are reading the ENTIRE vector in one go
	if (encoding == ColumnEncoding::DICTIONARY && read_now == to_read && dictionary_decoder.HasFilter()) {
		if (page_is_filtered_out) {
			// the page has been filtered out entirely - skip
			approved_tuple_count = 0;
		} else {
			// Push filter into dictionary directly
			// read the defines/repeats
			const auto all_valid = PrepareRead(read_now, define_out, repeat_out, 0);
			const auto define_ptr = all_valid ? nullptr : static_cast<uint8_t *>(define_out);
			dictionary_decoder.Filter(define_ptr, read_now, result, sel, approved_tuple_count);
		}
		page_rows_available -= read_now;
		FinishRead(to_read);
		return;
	}
	// fallback to regular read + filter
	ReadInternal(input, result);
	ApplyFilter(result, filter, filter_state, num_values, sel, approved_tuple_count);
}

void ColumnReader::ApplyFilter(Vector &v, const TableFilter &filter, TableFilterState &filter_state, idx_t scan_count,
                               SelectionVector &sel, idx_t &approved_tuple_count) {
	FlatVector::SetSize(v, count_t(scan_count));
	ColumnSegment::FilterSelection(sel, v, filter_state, scan_count, approved_tuple_count);
}

void ColumnReader::Skip(idx_t num_values) {
	pending_skips += num_values;
}

void ColumnReader::ApplyPendingSkips(data_ptr_t define_out, data_ptr_t repeat_out) {
	if (pending_skips == 0) {
		return;
	}
	idx_t num_values = pending_skips;
	pending_skips = 0;

	auto to_skip = num_values;
	data_t skip_defines[STANDARD_VECTOR_SIZE] = {};
	data_t skip_repeats[STANDARD_VECTOR_SIZE];
	data_ptr_t skip_define_out = HasDefines() ? skip_defines : define_out;
	data_ptr_t skip_repeat_out = HasRepeats() ? skip_repeats : repeat_out;
	// start reading but do not apply skips (we are skipping now)
	BeginRead(nullptr, nullptr);

	while (to_skip > 0) {
		if (TrySkipPagesWithOffsetIndex(num_values - to_skip, to_skip)) {
			// jumped over whole pages without reading them
			continue;
		}
		auto skip_now = ReadPageHeaders(to_skip, nullptr, nullptr, to_skip);
		if (page_is_filtered_out) {
			// the page has been filtered out entirely - skip
			page_rows_available -= skip_now;
			to_skip -= skip_now;
			continue;
		}
		const auto all_valid = PrepareRead(skip_now, skip_define_out, skip_repeat_out, 0);

		const auto define_ptr = all_valid ? nullptr : static_cast<uint8_t *>(skip_define_out);
		switch (encoding) {
		case ColumnEncoding::DICTIONARY:
			dictionary_decoder.Skip(define_ptr, skip_now);
			break;
		case ColumnEncoding::DELTA_BINARY_PACKED:
			delta_binary_packed_decoder.Skip(define_ptr, skip_now);
			break;
		case ColumnEncoding::RLE:
			rle_decoder.Skip(define_ptr, skip_now);
			break;
		case ColumnEncoding::DELTA_LENGTH_BYTE_ARRAY:
			delta_length_byte_array_decoder.Skip(define_ptr, skip_now);
			break;
		case ColumnEncoding::DELTA_BYTE_ARRAY:
			delta_byte_array_decoder.Skip(define_ptr, skip_now);
			break;
		case ColumnEncoding::BYTE_STREAM_SPLIT:
			byte_stream_split_decoder.Skip(define_ptr, skip_now);
			break;
		default:
			PlainSkip(*block, define_ptr, skip_now);
			break;
		}
		page_rows_available -= skip_now;
		to_skip -= skip_now;
	}
	FinishRead(num_values);
}

//===--------------------------------------------------------------------===//
// Create Column Reader
//===--------------------------------------------------------------------===//
template <class T>
static unique_ptr<ColumnReader> CreateDecimalReader(const ParquetReader &reader, const ParquetColumnSchema &schema) {
	switch (schema.type.InternalType()) {
	case PhysicalType::INT16:
		return make_uniq<TemplatedColumnReader<int16_t, TemplatedParquetValueConversion<T>>>(reader, schema);
	case PhysicalType::INT32:
		return make_uniq<TemplatedColumnReader<int32_t, TemplatedParquetValueConversion<T>>>(reader, schema);
	case PhysicalType::INT64:
		return make_uniq<TemplatedColumnReader<int64_t, TemplatedParquetValueConversion<T>>>(reader, schema);
	case PhysicalType::INT128:
		return make_uniq<TemplatedColumnReader<hugeint_t, TemplatedParquetValueConversion<T>>>(reader, schema);
	default:
		throw NotImplementedException("Unimplemented internal type for CreateDecimalReader");
	}
}

static_assert(ParquetTimestampLogicalType(ParquetExtraTypeInfo::UNIT_NS) != LogicalTypeId::TIMESTAMP);
static_assert(ParquetTimestampTzLogicalType(ParquetExtraTypeInfo::UNIT_NS) != LogicalTypeId::TIMESTAMP_TZ);

static_assert(ParquetTimestampLogicalType(ParquetExtraTypeInfo::IMPALA_TIMESTAMP) != LogicalTypeId::TIMESTAMP_NS);
static_assert(ParquetTimestampTzLogicalType(ParquetExtraTypeInfo::IMPALA_TIMESTAMP) != LogicalTypeId::TIMESTAMP_TZ_NS);
static_assert(ParquetTimestampLogicalType(ParquetExtraTypeInfo::UNIT_MS) != LogicalTypeId::TIMESTAMP_NS);
static_assert(ParquetTimestampTzLogicalType(ParquetExtraTypeInfo::UNIT_MS) != LogicalTypeId::TIMESTAMP_TZ_NS);
static_assert(ParquetTimestampLogicalType(ParquetExtraTypeInfo::UNIT_MICROS) != LogicalTypeId::TIMESTAMP_NS);
static_assert(ParquetTimestampTzLogicalType(ParquetExtraTypeInfo::UNIT_MICROS) != LogicalTypeId::TIMESTAMP_TZ_NS);

static_assert(ParquetTimeLogicalType(ParquetExtraTypeInfo::UNIT_NS) != LogicalTypeId::TIME);
static_assert(ParquetTimeLogicalType(ParquetExtraTypeInfo::UNIT_MS) != LogicalTypeId::TIME_NS);
static_assert(ParquetTimeLogicalType(ParquetExtraTypeInfo::UNIT_MICROS) != LogicalTypeId::TIME_NS);

unique_ptr<ColumnReader> ColumnReader::CreateReader(const ParquetReader &reader, const ParquetColumnSchema &schema) {
	switch (schema.type.id()) {
	case LogicalTypeId::BOOLEAN:
		return make_uniq<BooleanColumnReader>(reader, schema);
	case LogicalTypeId::UTINYINT:
		return make_uniq<TemplatedColumnReader<uint8_t, TemplatedParquetValueConversion<uint32_t>>>(reader, schema);
	case LogicalTypeId::USMALLINT:
		return make_uniq<TemplatedColumnReader<uint16_t, TemplatedParquetValueConversion<uint32_t>>>(reader, schema);
	case LogicalTypeId::UINTEGER:
		return make_uniq<TemplatedColumnReader<uint32_t, TemplatedParquetValueConversion<uint32_t>>>(reader, schema);
	case LogicalTypeId::UBIGINT:
		return make_uniq<TemplatedColumnReader<uint64_t, TemplatedParquetValueConversion<uint64_t>>>(reader, schema);
	case LogicalTypeId::TINYINT:
		return make_uniq<TemplatedColumnReader<int8_t, TemplatedParquetValueConversion<int32_t>>>(reader, schema);
	case LogicalTypeId::SMALLINT:
		return make_uniq<TemplatedColumnReader<int16_t, TemplatedParquetValueConversion<int32_t>>>(reader, schema);
	case LogicalTypeId::INTEGER:
		return make_uniq<TemplatedColumnReader<int32_t, TemplatedParquetValueConversion<int32_t>>>(reader, schema);
	case LogicalTypeId::BIGINT:
		return make_uniq<TemplatedColumnReader<int64_t, TemplatedParquetValueConversion<int64_t>>>(reader, schema);
	case LogicalTypeId::FLOAT:
		if (schema.type_info == ParquetExtraTypeInfo::FLOAT16) {
			return make_uniq<CallbackColumnReader<uint16_t, float, Float16ToFloat32>>(reader, schema);
		}
		return make_uniq<TemplatedColumnReader<float, TemplatedParquetValueConversion<float>>>(reader, schema);
	case LogicalTypeId::DOUBLE:
		if (schema.type_info == ParquetExtraTypeInfo::DECIMAL_BYTE_ARRAY) {
			return ParquetDecimalUtils::CreateReader(reader, schema);
		}
		return make_uniq<TemplatedColumnReader<double, TemplatedParquetValueConversion<double>>>(reader, schema);
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ:
		switch (schema.type_info) {
		case ParquetExtraTypeInfo::IMPALA_TIMESTAMP:
			return make_uniq<CallbackColumnReader<Int96, timestamp_t, ImpalaTimestampToTimestamp>>(reader, schema);
		case ParquetExtraTypeInfo::UNIT_MS:
			return make_uniq<CallbackColumnReader<int64_t, timestamp_t, ParquetTimestampMsToTimestamp>>(reader, schema);
		case ParquetExtraTypeInfo::UNIT_MICROS:
			return make_uniq<CallbackColumnReader<int64_t, timestamp_t, ParquetTimestampMicrosToTimestamp>>(reader,
			                                                                                                schema);
		default:
			throw InternalException("TIMESTAMP requires type info");
		}
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_TZ_NS:
		switch (schema.type_info) {
		case ParquetExtraTypeInfo::IMPALA_TIMESTAMP:
			return make_uniq<CallbackColumnReader<Int96, timestamp_ns_t, ImpalaTimestampToTimestampNS>>(reader, schema);
		case ParquetExtraTypeInfo::UNIT_MS:
			return make_uniq<CallbackColumnReader<int64_t, timestamp_ns_t, ParquetTimestampMsToTimestampNs>>(reader,
			                                                                                                 schema);
		case ParquetExtraTypeInfo::UNIT_MICROS:
			return make_uniq<CallbackColumnReader<int64_t, timestamp_ns_t, ParquetTimestampUsToTimestampNs>>(reader,
			                                                                                                 schema);
		case ParquetExtraTypeInfo::UNIT_NS:
			return make_uniq<CallbackColumnReader<int64_t, timestamp_ns_t, ParquetTimestampNsToTimestampNs>>(reader,
			                                                                                                 schema);
		default:
			throw InternalException("TIMESTAMP_NS requires type info");
		}
	case LogicalTypeId::DATE:
		return make_uniq<CallbackColumnReader<int32_t, date_t, ParquetIntToDate>>(reader, schema);
	case LogicalTypeId::TIME:
		switch (schema.type_info) {
		case ParquetExtraTypeInfo::UNIT_MS:
			return make_uniq<CallbackColumnReader<int32_t, dtime_t, ParquetMsIntToTime>>(reader, schema);
		case ParquetExtraTypeInfo::UNIT_MICROS:
			return make_uniq<CallbackColumnReader<int64_t, dtime_t, ParquetIntToTime>>(reader, schema);
		default:
			throw InternalException("TIME requires type info");
		}
	case LogicalTypeId::TIME_NS:
		switch (schema.type_info) {
		case ParquetExtraTypeInfo::UNIT_NS:
			return make_uniq<CallbackColumnReader<int64_t, dtime_ns_t, ParquetIntToTimeNs>>(reader, schema);
		default:
			throw InternalException("TIME requires type info");
		}
	case LogicalTypeId::TIME_TZ:
		switch (schema.type_info) {
		case ParquetExtraTypeInfo::UNIT_MS:
			return make_uniq<CallbackColumnReader<int32_t, dtime_tz_t, ParquetIntToTimeMsTZ>>(reader, schema);
		case ParquetExtraTypeInfo::UNIT_MICROS:
			return make_uniq<CallbackColumnReader<int64_t, dtime_tz_t, ParquetIntToTimeTZ>>(reader, schema);
		case ParquetExtraTypeInfo::UNIT_NS:
			return make_uniq<CallbackColumnReader<int64_t, dtime_tz_t, ParquetIntToTimeNsTZ>>(reader, schema);
		default:
			throw InternalException("TIME_TZ requires type info");
		}
	case LogicalTypeId::BLOB:
	case LogicalTypeId::VARCHAR:
		return make_uniq<StringColumnReader>(reader, schema);
	case LogicalTypeId::DECIMAL:
		// we have to figure out what kind of int we need
		switch (schema.type_info) {
		case ParquetExtraTypeInfo::DECIMAL_INT32:
			return CreateDecimalReader<int32_t>(reader, schema);
		case ParquetExtraTypeInfo::DECIMAL_INT64:
			return CreateDecimalReader<int64_t>(reader, schema);
		case ParquetExtraTypeInfo::DECIMAL_BYTE_ARRAY:
			return ParquetDecimalUtils::CreateReader(reader, schema);
		default:
			throw NotImplementedException("Unrecognized Parquet type for Decimal");
		}
	case LogicalTypeId::UUID:
		return make_uniq<UUIDColumnReader>(reader, schema);
	case LogicalTypeId::INTERVAL:
		return make_uniq<IntervalColumnReader>(reader, schema);
	case LogicalTypeId::SQLNULL:
		return make_uniq<NullColumnReader>(reader, schema);
	default:
		break;
	}
	throw NotImplementedException(schema.type.ToString());
}

} // namespace duckdb
