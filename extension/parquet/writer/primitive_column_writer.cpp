#include "writer/primitive_column_writer.hpp"

#include <functional>
#include <utility>
#include <vector>

#include "parquet_rle_bp_decoder.hpp"
#include "parquet_rle_bp_encoder.hpp"
#include "parquet_writer.hpp"
#include "duckdb/common/assert.hpp"
#include "duckdb/common/constants.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/primitive_dictionary.hpp"
#include "duckdb/common/serializer/write_stream.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/validity_mask.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "parquet_column_schema.hpp"
#include "parquet_geometry.hpp"

namespace duckdb {
using duckdb_parquet::Encoding;
using duckdb_parquet::PageType;

constexpr const idx_t PrimitiveColumnWriter::MAX_UNCOMPRESSED_PAGE_SIZE;
constexpr const idx_t PrimitiveColumnWriter::DEFAULT_DATA_PAGE_SIZE_LIMIT;
constexpr const idx_t PrimitiveColumnWriter::MAX_UNCOMPRESSED_DICT_PAGE_SIZE;

class ParquetPagePayloadBuffer : public AsyncWriteBuffer {
public:
	ParquetPagePayloadBuffer(idx_t size_p, unique_ptr<MemoryStream> temp_writer_p, AllocatedData compressed_buf_p)
	    : size(size_p), temp_writer(std::move(temp_writer_p)), compressed_buf(std::move(compressed_buf_p)) {
		D_ASSERT(temp_writer || compressed_buf.IsSet());
	}

	data_ptr_t Ptr() override {
		if (compressed_buf.IsSet()) {
			return compressed_buf.get();
		}
		D_ASSERT(temp_writer);
		return temp_writer->GetData();
	}

	idx_t Size() const override {
		return size;
	}

private:
	idx_t size;
	unique_ptr<MemoryStream> temp_writer;
	AllocatedData compressed_buf;
};

static PageWriteInformation CreateDictionaryPageWriteInformation(PrimitiveColumnWriterState &state,
                                                                 idx_t uncompressed_size, idx_t row_count) {
	PageWriteInformation write_info(state.row_group.columns.get_allocator());
	auto &hdr = write_info.page_header;
	hdr.uncompressed_page_size = UnsafeNumericCast<int32_t>(uncompressed_size);
	hdr.type = PageType::DICTIONARY_PAGE;
	hdr.__isset.dictionary_page_header = true;

	hdr.dictionary_page_header.encoding = Encoding::PLAIN;
	hdr.dictionary_page_header.is_sorted = false;
	hdr.dictionary_page_header.num_values = UnsafeNumericCast<int32_t>(row_count);

	write_info.write_count = 0;
	write_info.max_write_count = 0;
	return write_info;
}

PrimitiveColumnWriter::PrimitiveColumnWriter(ParquetWriter &writer, ParquetColumnSchema &&column_schema,
                                             vector<Identifier> schema_path)
    : ColumnWriter(writer, std::move(column_schema), std::move(schema_path)) {
}

unique_ptr<ColumnWriterState> PrimitiveColumnWriter::InitializeWriteState(duckdb_parquet::RowGroup &row_group) {
	auto result = make_uniq<PrimitiveColumnWriterState>(writer, row_group, row_group.columns.size());
	RegisterToRowGroup(row_group);
	return std::move(result);
}

void PrimitiveColumnWriter::RegisterToRowGroup(duckdb_parquet::RowGroup &row_group) {
	row_group.columns.emplace_back();
	auto &column_chunk = row_group.columns.back();
	column_chunk.__isset.meta_data = true;
	column_chunk.meta_data.codec = writer.GetCodec();
	for (auto &path_part : IdentifiersToStrings(schema_path)) {
		column_chunk.meta_data.path_in_schema.emplace_back().assign(path_part.data(), path_part.size());
	}
	column_chunk.meta_data.num_values = 0;
	column_chunk.meta_data.type = writer.GetType(SchemaIndex());
}

unique_ptr<ColumnWriterPageState> PrimitiveColumnWriter::InitializePageState(PrimitiveColumnWriterState &state,
                                                                             idx_t page_idx) {
	return nullptr;
}

void PrimitiveColumnWriter::FlushPageState(WriteStream &temp_writer, ColumnWriterPageState *state) {
}

void PrimitiveColumnWriter::Prepare(ColumnWriterState &state_p, ColumnWriterState *parent, Vector &vector, idx_t count,
                                    bool vector_can_span_multiple_pages) {
	auto &state = state_p.Cast<PrimitiveColumnWriterState>();
	auto &col_chunk = state.row_group.columns[state.col_idx];

	idx_t vcount = parent ? parent->definition_levels.size() - state.definition_levels.size() : count;
	idx_t parent_index = state.definition_levels.size();
	auto &validity = FlatVector::ValidityMutable(vector);
	HandleRepeatLevels(state, parent, count);
	HandleDefineLevels(state, parent, validity, count, MaxDefine(), MaxDefine() - 1);

	idx_t vector_index = 0;
	reference<PageInformation> page_info_ref = state.page_info.back();
	col_chunk.meta_data.num_values += NumericCast<int64_t>(vcount);

	const idx_t page_size_limit = writer.DataPageSizeLimit();
	const bool check_parent_empty = parent && !parent->is_empty.empty();
	if (!check_parent_empty && validity.CannotHaveNull() && TypeIsConstantSize(vector.GetType().InternalType()) &&
	    page_info_ref.get().estimated_page_size + GetRowSize(vector, vector_index, state) * vcount < page_size_limit) {
		// Fast path: fixed-size type, all valid, and it fits on the current page
		auto &page_info = page_info_ref.get();
		page_info.row_count += vcount;
		page_info.estimated_page_size += GetRowSize(vector, vector_index, state) * vcount;
	} else {
		for (idx_t i = 0; i < vcount; i++) {
			if (i == 0 && !vector_can_span_multiple_pages && writer.WritePageIndex()) {
				// a vector that cannot span multiple pages (the values of lists) starts on a new page when the current
				// page is full, so that every page begins at a row boundary as the OffsetIndex requires
				auto &page_info = page_info_ref.get();
				if (page_info.row_count > 0 && page_info.estimated_page_size >= page_size_limit) {
					PageInformation new_info;
					new_info.offset = page_info.offset + page_info.row_count;
					state.page_info.push_back(new_info);
					page_info_ref = state.page_info.back();
				}
			}
			auto &page_info = page_info_ref.get();
			page_info.row_count++;
			if (check_parent_empty && parent->is_empty[parent_index + i]) {
				page_info.empty_count++;
				continue;
			}
			if (validity.RowIsValid(vector_index)) {
				page_info.estimated_page_size += GetRowSize(vector, vector_index, state);
				// a vector that cannot span pages splits after its first value, unless the page index made it start
				// on a fresh page above
				if (page_info.estimated_page_size >= page_size_limit &&
				    (vector_can_span_multiple_pages || (i == 0 && !writer.WritePageIndex()))) {
					PageInformation new_info;
					new_info.offset = page_info.offset + page_info.row_count;
					state.page_info.push_back(new_info);
					page_info_ref = state.page_info.back();
				}
			} else {
				page_info.null_count++;
			}
			vector_index++;
		}
	}
}

duckdb_parquet::Encoding::type PrimitiveColumnWriter::GetEncoding(PrimitiveColumnWriterState &state) {
	return Encoding::PLAIN;
}

void PrimitiveColumnWriter::BeginWrite(ColumnWriterState &state_p) {
	auto &state = state_p.Cast<PrimitiveColumnWriterState>();

	// set up the page write info
	state.stats_state = InitializeStatsState();
	for (idx_t page_idx = 0; page_idx < state.page_info.size(); page_idx++) {
		auto &page_info = state.page_info[page_idx];
		if (page_info.row_count == 0) {
			D_ASSERT(page_idx + 1 == state.page_info.size());
			state.page_info.erase_at(page_idx);
			break;
		}
		PageWriteInformation write_info(state.row_group.columns.get_allocator());
		// set up the header
		auto &hdr = write_info.page_header;
		hdr.compressed_page_size = 0;
		hdr.uncompressed_page_size = 0;
		hdr.type = PageType::DATA_PAGE;
		hdr.__isset.data_page_header = true;

		hdr.data_page_header.num_values = NumericCast<int32_t>(page_info.row_count);
		hdr.data_page_header.encoding = GetEncoding(state);
		hdr.data_page_header.definition_level_encoding = Encoding::RLE;
		hdr.data_page_header.repetition_level_encoding = Encoding::RLE;

		write_info.temp_writer = make_uniq<MemoryStream>(
		    BufferAllocator::Get(writer.GetContext()),
		    MaxValue<idx_t>(NextPowerOfTwo(page_info.estimated_page_size), MemoryStream::DEFAULT_INITIAL_CAPACITY));
		write_info.write_count = page_info.empty_count;
		write_info.max_write_count = page_info.row_count;
		write_info.page_state = InitializePageState(state, page_idx);

		write_info.compressed_size = 0;
		write_info.compressed_data = nullptr;

		if (writer.WritePageIndex()) {
			// the page index needs the statistics, the row count and the null count of every page
			write_info.page_stats = InitializeStatsState();
			write_info.num_rows = page_info.row_count;
			if (MaxRepeat() > 0) {
				// a repeated column has several values per row: a row starts at every repetition level of zero
				write_info.num_rows = 0;
				for (idx_t i = page_info.offset; i < page_info.offset + page_info.row_count; i++) {
					write_info.num_rows += state.repetition_levels[i] == 0;
				}
				if (state.repetition_levels[page_info.offset] != 0) {
					// the OffsetIndex requires pages to begin at row boundaries
					state.pages_start_at_rows = false;
				}
			}
			if (MaxDefine() > 0) {
				// every value below the maximum definition level is a NULL (of this column or of a parent)
				const auto max_define = NumericCast<uint16_t>(MaxDefine());
				for (idx_t i = page_info.offset; i < page_info.offset + page_info.row_count; i++) {
					write_info.null_count += state.definition_levels[i] != max_define;
				}
			}
		}

		state.write_info.push_back(std::move(write_info));
	}

	// start writing the first page
	NextPage(state);
}

void PrimitiveColumnWriter::WriteLevels(Allocator &allocator, WriteStream &temp_writer,
                                        const unsafe_vector<uint16_t> &levels, idx_t max_value, idx_t offset,
                                        idx_t count, optional_idx null_count) {
	// For definition levels: the column is REQUIRED, nothing to encode.
	// For repetition levels: the column is not repeated, every value appears exactly once, nothing to encode.
	if (max_value == 0) {
		return;
	}

	if (levels.empty() || count == 0) {
		return;
	}

	// write the levels using the RLE-BP encoding
	const auto bit_width = RleBpDecoder::ComputeBitWidthFromMaxValue(max_value);
	RleBpEncoder rle_encoder(bit_width);

	// have to write to an intermediate stream first because we need to know the size
	MemoryStream intermediate_stream(allocator);

	rle_encoder.BeginWrite();
	if (null_count.IsValid() && null_count.GetIndex() == 0) {
		// Fast path: no nulls
		rle_encoder.WriteMany(intermediate_stream, levels[0], count);
	} else {
		for (idx_t i = offset; i < offset + count; i++) {
			rle_encoder.WriteValue(intermediate_stream, levels[i]);
		}
	}
	rle_encoder.FinishWrite(intermediate_stream);

	// start off by writing the byte count as a uint32_t
	temp_writer.Write(NumericCast<uint32_t>(intermediate_stream.GetPosition()));
	// copy over the written data
	temp_writer.WriteData(intermediate_stream.GetData(), intermediate_stream.GetPosition());
}

void PrimitiveColumnWriter::NextPage(PrimitiveColumnWriterState &state) {
	if (state.current_page > 0) {
		// need to flush the current page
		FlushPage(state);
	}
	if (state.current_page >= state.write_info.size()) {
		state.current_page = state.write_info.size() + 1;
		return;
	}
	auto &page_info = state.page_info[state.current_page];
	auto &write_info = state.write_info[state.current_page];
	state.current_page++;

	auto &temp_writer = *write_info.temp_writer;

	// write the repetition levels
	auto &allocator = BufferAllocator::Get(writer.GetContext());
	WriteLevels(allocator, temp_writer, state.repetition_levels, MaxRepeat(), page_info.offset, page_info.row_count);

	// write the definition levels
	WriteLevels(allocator, temp_writer, state.definition_levels, MaxDefine(), page_info.offset, page_info.row_count,
	            state.null_count + state.parent_null_count);
}

void PrimitiveColumnWriter::FlushPage(PrimitiveColumnWriterState &state) {
	D_ASSERT(state.current_page > 0);
	if (state.current_page > state.write_info.size()) {
		return;
	}

	// compress the page info
	auto &write_info = state.write_info[state.current_page - 1];
	auto &temp_writer = *write_info.temp_writer;
	auto &hdr = write_info.page_header;

	FlushPageState(temp_writer, write_info.page_state.get());
	if (write_info.page_stats && state.stats_state) {
		// the statistics of the column chunk are the merged statistics of its pages
		state.stats_state->Merge(*write_info.page_stats);
	}

	// now that we have finished writing the data we know the uncompressed size
	if (temp_writer.GetPosition() > idx_t(NumericLimits<int32_t>::Maximum())) {
		throw InternalException("Parquet writer: %d uncompressed page size out of range for type integer",
		                        temp_writer.GetPosition());
	}
	hdr.uncompressed_page_size = UnsafeNumericCast<int32_t>(temp_writer.GetPosition());

	// compress the data
	CompressPage(temp_writer, write_info.compressed_size, write_info.compressed_data, write_info.compressed_buf);
	hdr.compressed_page_size = UnsafeNumericCast<int32_t>(write_info.compressed_size);

	if (write_info.compressed_buf.IsSet()) {
		// if the data has been compressed, we no longer need the uncompressed data
		D_ASSERT(write_info.compressed_buf.get() == write_info.compressed_data);
		write_info.temp_writer.reset();
	}
}

unique_ptr<ColumnWriterStatistics> PrimitiveColumnWriter::InitializeStatsState() {
	return make_uniq<ColumnWriterStatistics>();
}

idx_t PrimitiveColumnWriter::GetRowSize(const Vector &vector, const idx_t index,
                                        const PrimitiveColumnWriterState &state) const {
	throw InternalException("GetRowSize unsupported for struct/list column writers");
}

void PrimitiveColumnWriter::Write(ColumnWriterState &state_p, Vector &vector, idx_t count) {
	auto &state = state_p.Cast<PrimitiveColumnWriterState>();

	idx_t remaining = count;
	idx_t offset = 0;
	while (remaining > 0) {
		auto &write_info = state.write_info[state.current_page - 1];
		if (!write_info.temp_writer) {
			throw InternalException("Writes are not correctly aligned!?");
		}
		auto &temp_writer = *write_info.temp_writer;
		idx_t write_count = MinValue<idx_t>(remaining, write_info.max_write_count - write_info.write_count);
		D_ASSERT(write_count > 0);

		// with a page index every page tracks its own statistics, which are merged into the chunk when it is flushed
		auto stats = write_info.page_stats ? write_info.page_stats.get() : state.stats_state.get();
		WriteVector(temp_writer, stats, write_info.page_state.get(), vector, offset, offset + write_count);

		write_info.write_count += write_count;
		if (write_info.write_count == write_info.max_write_count) {
			NextPage(state);
		}
		offset += write_count;
		remaining -= write_count;
	}
}

void PrimitiveColumnWriter::SetParquetStatistics(PrimitiveColumnWriterState &state,
                                                 duckdb_parquet::ColumnChunk &column_chunk) {
	auto add_encoding = [&](duckdb_parquet::Encoding::type encoding) {
		for (const auto &existing_encoding : column_chunk.meta_data.encodings) {
			if (existing_encoding == encoding) {
				return;
			}
		}
		column_chunk.meta_data.encodings.push_back(encoding);
	};

	auto add_encoding_stat = [&](duckdb_parquet::PageType::type page_type, duckdb_parquet::Encoding::type encoding) {
		for (auto &existing_stat : column_chunk.meta_data.encoding_stats) {
			if (existing_stat.page_type == page_type && existing_stat.encoding == encoding) {
				existing_stat.count++;
				return;
			}
		}
		column_chunk.meta_data.encoding_stats.emplace_back();
		auto &stat = column_chunk.meta_data.encoding_stats.back();
		stat.page_type = page_type;
		stat.encoding = encoding;
		stat.count = 1;
	};

	for (const auto &write_info : state.write_info) {
		// only care about data page encodings, data_page_header.encoding is meaningless for dict
		switch (write_info.page_header.type) {
		case PageType::DATA_PAGE:
			add_encoding(write_info.page_header.data_page_header.encoding);
			add_encoding_stat(write_info.page_header.type, write_info.page_header.data_page_header.encoding);
			break;
		case PageType::DATA_PAGE_V2:
			add_encoding(write_info.page_header.data_page_header_v2.encoding);
			add_encoding_stat(write_info.page_header.type, write_info.page_header.data_page_header_v2.encoding);
			break;
		case PageType::DICTIONARY_PAGE:
			add_encoding_stat(write_info.page_header.type, write_info.page_header.dictionary_page_header.encoding);
			break;
		default:
			break;
		}
	}
	// write_info holds the dictionary page (if any) first, so encoding_stats lists it first -
	// the physical page order, and the order other writers (parquet-cpp, parquet-rs) produce
	if (!column_chunk.meta_data.encoding_stats.empty()) {
		column_chunk.meta_data.__isset.encoding_stats = true;
	}

	if (!state.stats_state) {
		return;
	}
	// a NULL ancestor counts as a NULL of this column, the children count it once as a parent NULL
	column_chunk.meta_data.statistics.null_count = NumericCast<int64_t>(state.null_count + state.parent_null_count);
	column_chunk.meta_data.statistics.__isset.null_count = true;
	column_chunk.meta_data.__isset.statistics = true;
	if (column_chunk.meta_data.type == duckdb_parquet::Type::FLOAT ||
	    column_chunk.meta_data.type == duckdb_parquet::Type::DOUBLE) {
		column_chunk.meta_data.statistics.nan_count = NumericCast<int64_t>(state.stats_state->GetNaNCount());
		column_chunk.meta_data.statistics.__isset.nan_count = true;
	}

	// if we have NaN values - don't write the min/max here
	if (!state.stats_state->HasNaN()) {
		// set min/max/min_value/max_value
		// this code is not going to win any beauty contests, but well
		auto min = state.stats_state->GetMin();
		if (!min.empty()) {
			column_chunk.meta_data.statistics.min = std::move(min);
			column_chunk.meta_data.statistics.__isset.min = true;
			column_chunk.meta_data.__isset.statistics = true;
		}
		auto max = state.stats_state->GetMax();
		if (!max.empty()) {
			column_chunk.meta_data.statistics.max = std::move(max);
			column_chunk.meta_data.statistics.__isset.max = true;
			column_chunk.meta_data.__isset.statistics = true;
		}

		if (state.stats_state->HasStats()) {
			column_chunk.meta_data.statistics.min_value = state.stats_state->GetMinValue();
			column_chunk.meta_data.statistics.__isset.min_value = true;
			column_chunk.meta_data.__isset.statistics = true;
			column_chunk.meta_data.statistics.is_min_value_exact = state.stats_state->MinIsExact();
			column_chunk.meta_data.statistics.__isset.is_min_value_exact = true;

			column_chunk.meta_data.statistics.max_value = state.stats_state->GetMaxValue();
			column_chunk.meta_data.statistics.__isset.max_value = true;
			column_chunk.meta_data.__isset.statistics = true;
			column_chunk.meta_data.statistics.is_max_value_exact = state.stats_state->MaxIsExact();
			column_chunk.meta_data.statistics.__isset.is_max_value_exact = true;
		}
	}
	if (HasDictionary(state)) {
		column_chunk.meta_data.statistics.distinct_count = UnsafeNumericCast<int64_t>(DictionarySize(state));
		column_chunk.meta_data.statistics.__isset.distinct_count = true;
		column_chunk.meta_data.__isset.statistics = true;
	}

	if (state.stats_state->HasGeoStats()) {
		auto gpq_version = writer.GetGeoParquetVersion();

		const auto has_real_stats = gpq_version == GeoParquetVersion::NONE || gpq_version == GeoParquetVersion::BOTH ||
		                            gpq_version == GeoParquetVersion::V2;
		const auto has_json_stats = gpq_version == GeoParquetVersion::V1 || gpq_version == GeoParquetVersion::BOTH ||
		                            gpq_version == GeoParquetVersion::V2;

		if (has_real_stats) {
			// Write the parquet native geospatial statistics
			column_chunk.meta_data.__isset.geospatial_statistics = true;
			state.stats_state->WriteGeoStats(column_chunk.meta_data.geospatial_statistics);
		}
		if (has_json_stats) {
			// Add the geospatial statistics to the extra GeoParquet metadata
			writer.GetGeoParquetData().AddGeoParquetStats(writer.GetContext(), column_schema.name, column_schema.type,
			                                              *state.stats_state->GetGeoStats(), gpq_version);
		}
	}
}

void PrimitiveColumnWriter::PrepareWrite(ColumnWriterState &state_p) {
	auto &state = state_p.Cast<PrimitiveColumnWriterState>();

	// Flush the last page and materialize any dictionary page before the row-group flush path.
	FlushPage(state);
	if (HasDictionary(state)) {
		FlushDictionary(state, state.stats_state.get());
	}

	for (auto &write_info : state.write_info) {
		D_ASSERT(!write_info.prepared_header);
		D_ASSERT(!write_info.prepared_payload);

		write_info.prepared_header = writer.PrepareWrite(write_info.page_header);
		auto payload_buffer = make_uniq<ParquetPagePayloadBuffer>(
		    write_info.compressed_size, std::move(write_info.temp_writer), std::move(write_info.compressed_buf));
		write_info.prepared_payload = writer.PrepareWriteData(std::move(payload_buffer));
	}
}

void PrimitiveColumnWriter::FinalizeWrite(ColumnWriterState &state_p) {
	auto &state = state_p.Cast<PrimitiveColumnWriterState>();
	auto &column_chunk = state.row_group.columns[state.col_idx];

	auto &column_writer = writer.GetWriter();
	auto start_offset = column_writer.GetTotalWritten();
	if (HasDictionary(state)) {
		column_chunk.meta_data.statistics.distinct_count = UnsafeNumericCast<int64_t>(DictionarySize(state));
		column_chunk.meta_data.statistics.__isset.distinct_count = true;
		column_chunk.meta_data.dictionary_page_offset = UnsafeNumericCast<int64_t>(column_writer.GetTotalWritten());
		column_chunk.meta_data.__isset.dictionary_page_offset = true;
	}

	// record the start position of the pages for this column
	column_chunk.meta_data.data_page_offset = 0;
	SetParquetStatistics(state, column_chunk);

	// write the individual pages to disk
	const bool write_page_index = writer.WritePageIndex();
	ParquetPageIndexEntry page_index(writer.GetMetadataArena());
	idx_t first_row_index = 0;
	idx_t total_uncompressed_size = 0;
	for (auto &write_info : state.write_info) {
		const bool is_data_page =
		    write_info.page_header.type == PageType::DATA_PAGE || write_info.page_header.type == PageType::DATA_PAGE_V2;
		const auto page_offset = column_writer.GetTotalWritten();
		// set the data page offset whenever we see the *first* data page
		if (column_chunk.meta_data.data_page_offset == 0 && is_data_page) {
			column_chunk.meta_data.data_page_offset = UnsafeNumericCast<int64_t>(page_offset);
		}
		D_ASSERT(write_info.prepared_header);
		D_ASSERT(write_info.prepared_payload);
		const auto header_size = write_info.prepared_header->Size();
		if (write_page_index && is_data_page) {
			AddPageToPageIndex(state, write_info, page_offset, header_size, page_index, first_row_index);
		}
		// total uncompressed size in the column chunk includes the header size (!)
		total_uncompressed_size += header_size;
		total_uncompressed_size += write_info.page_header.uncompressed_page_size;
		writer.WriteData(std::move(write_info.prepared_header));
		writer.WriteData(std::move(write_info.prepared_payload));
	}
	column_chunk.meta_data.total_compressed_size =
	    UnsafeNumericCast<int64_t>(column_writer.GetTotalWritten() - start_offset);
	column_chunk.meta_data.total_uncompressed_size = UnsafeNumericCast<int64_t>(total_uncompressed_size);
	state.row_group.total_byte_size += column_chunk.meta_data.total_uncompressed_size;

	// a chunk with a single page carries no page index: the footer statistics already describe that page, and the
	// index would only add bytes to small files
	if (write_page_index && state.pages_start_at_rows && page_index.offset_index.page_locations.size() > 1) {
		FinalizePageIndex(state, page_index);
		writer.BufferPageIndex(state.col_idx, std::move(page_index));
	}
	if (state.bloom_filter) {
		writer.BufferBloomFilter(state.col_idx, std::move(state.bloom_filter));
	}
	// finalize the stats
	writer.FlushColumnStats(state.col_idx, column_chunk, state.stats_state.get());
}

void PrimitiveColumnWriter::AddPageToPageIndex(PrimitiveColumnWriterState &state,
                                               const PageWriteInformation &write_info, idx_t page_offset,
                                               idx_t header_size, ParquetPageIndexEntry &page_index,
                                               idx_t &first_row_index) {
	// OffsetIndex: where the page is, and the first row it holds
	duckdb_parquet::PageLocation location;
	location.offset = NumericCast<int64_t>(page_offset);
	location.compressed_page_size = NumericCast<int32_t>(header_size + write_info.compressed_size);
	location.first_row_index = NumericCast<int64_t>(first_row_index);
	page_index.offset_index.page_locations.push_back(location);
	first_row_index += write_info.num_rows;

	// ColumnIndex: the bounds of the page - a page without any value has none (and neither has a type without
	// statistics, which rules out the ColumnIndex in FinalizePageIndex)
	auto &column_index = page_index.column_index;
	optional_ptr<ColumnWriterStatistics> stats = write_info.page_stats.get();
	const bool null_page = !stats || !stats->HasStats();
	column_index.null_pages.push_back(null_page);
	const string min_value = null_page ? string() : stats->GetMinValue();
	const string max_value = null_page ? string() : stats->GetMaxValue();
	column_index.min_values.emplace_back().assign(min_value.data(), min_value.size());
	column_index.max_values.emplace_back().assign(max_value.data(), max_value.size());
	column_index.null_counts.push_back(NumericCast<int64_t>(write_info.null_count));
	column_index.__isset.null_counts = true;
}

void PrimitiveColumnWriter::FinalizePageIndex(PrimitiveColumnWriterState &state, ParquetPageIndexEntry &page_index) {
	auto &column_index = page_index.column_index;
	// the ColumnIndex needs min/max for every page that has values: types without statistics get none, and neither
	// do floating point columns with NaN values (the ColumnIndex cannot express them, unlike the chunk statistics)
	page_index.has_column_index = true;
	optional_ptr<const ColumnWriterStatistics> previous;
	bool ascending = true;
	bool descending = true;
	idx_t page_idx = 0;
	for (auto &write_info : state.write_info) {
		if (write_info.page_header.type != PageType::DATA_PAGE &&
		    write_info.page_header.type != PageType::DATA_PAGE_V2) {
			continue;
		}
		optional_ptr<ColumnWriterStatistics> stats = write_info.page_stats.get();
		const bool null_page = column_index.null_pages[page_idx++];
		// a page with values but without statistics, or with NaN values, rules out the ColumnIndex
		const bool page_has_values = write_info.null_count < write_info.max_write_count;
		if ((null_page && page_has_values) || (stats && stats->HasNaN())) {
			page_index.has_column_index = false;
			return;
		}
		if (null_page) {
			continue;
		}
		if (previous) {
			ascending = ascending && previous->CompareMin(*stats) <= 0 && previous->CompareMax(*stats) <= 0;
			descending = descending && previous->CompareMin(*stats) >= 0 && previous->CompareMax(*stats) >= 0;
		}
		previous = stats.get();
	}
	if (ascending) {
		column_index.boundary_order = duckdb_parquet::BoundaryOrder::ASCENDING;
	} else if (descending) {
		column_index.boundary_order = duckdb_parquet::BoundaryOrder::DESCENDING;
	} else {
		column_index.boundary_order = duckdb_parquet::BoundaryOrder::UNORDERED;
	}
}

void PrimitiveColumnWriter::FlushDictionary(PrimitiveColumnWriterState &state, ColumnWriterStatistics *stats) {
	throw InternalException("This page does not have a dictionary");
}

idx_t PrimitiveColumnWriter::DictionarySize(PrimitiveColumnWriterState &state) {
	throw InternalException("This page does not have a dictionary");
}

void PrimitiveColumnWriter::WriteDictionary(PrimitiveColumnWriterState &state, unique_ptr<MemoryStream> temp_writer,
                                            idx_t row_count) {
	D_ASSERT(temp_writer);

	auto write_info = CreateDictionaryPageWriteInformation(state, temp_writer->GetPosition(), row_count);
	write_info.temp_writer = std::move(temp_writer);

	// compress the contents of the dictionary page
	CompressPage(*write_info.temp_writer, write_info.compressed_size, write_info.compressed_data,
	             write_info.compressed_buf);
	write_info.page_header.compressed_page_size = UnsafeNumericCast<int32_t>(write_info.compressed_size);

	if (write_info.compressed_buf.IsSet()) {
		// if the data has been compressed, we no longer need the uncompressed data
		D_ASSERT(write_info.compressed_buf.get() == write_info.compressed_data);
		write_info.temp_writer.reset();
	}

	// insert the dictionary page as the first page to write for this column
	state.write_info.insert(state.write_info.begin(), std::move(write_info));
}

void PrimitiveColumnWriter::WriteDictionary(PrimitiveColumnWriterState &state,
                                            PrimitiveDictionaryTargetData target_data, idx_t row_count) {
	D_ASSERT(target_data.data.IsSet());
	D_ASSERT(target_data.size > 0);

	auto write_info = CreateDictionaryPageWriteInformation(state, target_data.size, row_count);

	// compress the contents of the dictionary page
	MemoryStream temp_writer(target_data.data.get(), target_data.data.GetSize());
	temp_writer.SetPosition(target_data.size);
	CompressPage(temp_writer, write_info.compressed_size, write_info.compressed_data, write_info.compressed_buf);
	write_info.page_header.compressed_page_size = UnsafeNumericCast<int32_t>(write_info.compressed_size);

	if (!write_info.compressed_buf.IsSet()) {
		D_ASSERT(write_info.compressed_data == target_data.data.get());
		write_info.compressed_buf = std::move(target_data.data);
	}

	// insert the dictionary page as the first page to write for this column
	state.write_info.insert(state.write_info.begin(), std::move(write_info));
}

idx_t PrimitiveColumnWriter::FinalizeSchema(duckdb_parquet::SchemaElementList &schemas) {
	idx_t schema_idx = schemas.size();

	auto &schema = column_schema;
	schema.SetSchemaIndex(schema_idx);

	auto &repetition_type = schema.repetition_type;
	auto &name = schema.name;
	auto &field_id = schema.field_id;
	auto &type = schema.type;
	auto allow_geometry = schema.allow_geometry;

	schemas.emplace_back();
	auto &schema_element = schemas.back();
	schema_element.type = ParquetWriter::DuckDBTypeToParquetType(type, writer.WriteTimestampAsInt96());
	schema_element.repetition_type = repetition_type;
	schema_element.__isset.num_children = false;
	schema_element.__isset.type = true;
	schema_element.__isset.repetition_type = true;
	schema_element.name = name;
	if (field_id.IsValid()) {
		schema_element.__isset.field_id = true;
		schema_element.field_id = NumericCast<int32_t>(field_id.GetIndex());
	}
	ParquetWriter::SetSchemaProperties(type, schema_element, allow_geometry, writer.GetContext(),
	                                   writer.WriteTimestampAsInt96(), writer.TimestampIsAdjustedToUTC());

	D_ASSERT(child_writers.empty());
	return 1;
}

} // namespace duckdb
