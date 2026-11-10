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

static PageWriteInformation CreateDictionaryPageWriteInformation(idx_t uncompressed_size, idx_t row_count) {
	PageWriteInformation write_info;
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
	cdc_cut_pages = writer.ContentDefinedChunking();
	if (cdc_cut_pages) {
		ParquetContentDefinedChunker::Options options;
		options.min_chunk_size = writer.CdcMinChunkSize();
		options.max_chunk_size = writer.CdcMaxChunkSize();
		options.norm_level = writer.CdcNormLevel();
		cdc_chunker.Reset(options);
	}
}

unique_ptr<ColumnWriterState> PrimitiveColumnWriter::InitializeWriteState(duckdb_parquet::RowGroup &row_group) {
	auto result = make_uniq<PrimitiveColumnWriterState>(writer, row_group, row_group.columns.size());
	RegisterToRowGroup(row_group);
	return std::move(result);
}

void PrimitiveColumnWriter::RegisterToRowGroup(duckdb_parquet::RowGroup &row_group) {
	duckdb_parquet::ColumnChunk column_chunk;
	column_chunk.__isset.meta_data = true;
	column_chunk.meta_data.codec = writer.GetCodec();
	column_chunk.meta_data.path_in_schema = IdentifiersToStrings(schema_path);
	column_chunk.meta_data.num_values = 0;
	column_chunk.meta_data.type = writer.GetType(SchemaIndex());
	row_group.columns.push_back(std::move(column_chunk));
}

unique_ptr<ColumnWriterPageState> PrimitiveColumnWriter::InitializePageState(PrimitiveColumnWriterState &state,
                                                                             idx_t page_idx) {
	return nullptr;
}

void PrimitiveColumnWriter::FlushPageState(WriteStream &temp_writer, ColumnWriterPageState *state) {
}

//! The types that get content-defined boundaries, and the bytes they are hashed as, are in
//! writer/parquet_cdc_chunker.hpp - both are Arrow's, so that a column cut here and the same column cut by an
//! Arrow writer land between the same values.

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
	// Content-defined boundaries are only applied to top-level columns. A column under a list or a struct has its
	// page membership entangled with the parent's level/`is_empty` bookkeeping, and Arrow cuts those at record
	// boundaries (repetition level 0) rather than per value, which this loop does not track.
	const bool cut_by_content = cdc_cut_pages && parent == nullptr && ParquetCdcSupportedType(vector.GetType());
	if (!cut_by_content && !check_parent_empty && validity.CannotHaveNull() &&
	    TypeIsConstantSize(vector.GetType().InternalType()) &&
	    page_info_ref.get().estimated_page_size + GetRowSize(vector, vector_index, state) * vcount < page_size_limit) {
		// Fast path: fixed-size type, all valid, and it fits on the current page. Content-defined boundaries
		// cannot take it, because the whole point is to look at every value.
		auto &page_info = page_info_ref.get();
		page_info.row_count += vcount;
		page_info.estimated_page_size += GetRowSize(vector, vector_index, state) * vcount;
	} else {
		// A nullable column's definition levels are part of what the chunker sees, exactly as they are in Arrow -
		// which means a null moves the hash, so null density shifts boundaries - and a non-nullable column has no
		// levels to feed, in Arrow either.
		const bool feed_levels = MaxDefine() > 0;
		// boundaries may have been computed up front, in file order, by ComputeCdcBoundaries - then the chunker
		// has already rolled through this chunk and the cut positions below are its answers
		const duckdb::vector<idx_t> *precomputed_cuts = nullptr;
		idx_t precomputed_pos = 0;
		if (cut_by_content && state.cdc_cuts_per_chunk && state.cdc_cut_chunk_idx < state.cdc_cuts_per_chunk->size()) {
			precomputed_cuts = &(*state.cdc_cuts_per_chunk)[state.cdc_cut_chunk_idx];
		}
		for (idx_t i = 0; i < vcount; i++) {
			if (check_parent_empty && parent->is_empty[parent_index + i]) {
				auto &empty_page = page_info_ref.get();
				empty_page.row_count++;
				empty_page.empty_count++;
				continue;
			}
			const bool is_valid = validity.RowIsValid(vector_index);
			// A content boundary opens the next page with the value that completed it, which is Arrow's convention:
			// its chunker hands back chunks as [previous boundary, this level), boundary value included in the
			// chunk after. Matching it is not a detail - a value counted on one side and not the other moves every
			// following page's first value, which is the whole thing this is trying to keep still.
			bool cut_before = false;
			if (cut_by_content) {
				if (precomputed_cuts) {
					cut_before =
					    precomputed_pos < precomputed_cuts->size() && (*precomputed_cuts)[precomputed_pos] == i;
					if (cut_before) {
						precomputed_pos++;
					}
				} else {
					if (feed_levels) {
						cdc_chunker.RollLevel(state.definition_levels[parent_index + i]);
					}
					if (is_valid) {
						ParquetCdcHashValue(cdc_chunker, vector, vector_index);
					}
					// the minimum is enforced inside the chunker, where the bytes are counted, and the maximum is its
					// hard stop for the case where no boundary is hit at all - a long run of identical values, say
					cut_before = cdc_chunker.NeedNewChunk();
				}
			}
			// a vector that cannot span multiple pages stays on the page it started on, and a page that is still
			// empty is already at a boundary: the writer erases an empty page only at the end of the row group, so
			// opening a second one here would leave a hole in the middle of it. This is the one case where a
			// boundary goes unmatched, and it needs a single value bigger than max_chunk_size to happen - Arrow
			// emits a zero-level chunk for it, which is not something this writer can represent.
			if (cut_before && page_info_ref.get().row_count > 0 && (vector_can_span_multiple_pages || i == 0)) {
				PageInformation new_info;
				new_info.offset = page_info_ref.get().offset + page_info_ref.get().row_count;
				state.page_info.push_back(new_info);
				page_info_ref = state.page_info.back();
			}
			auto &page_info = page_info_ref.get();
			page_info.row_count++;
			bool cut_after = false;
			if (is_valid) {
				page_info.estimated_page_size += GetRowSize(vector, vector_index, state);
				// DuckDB's own page size limit, which closes a page after the value that overflowed it - a
				// different convention from the boundary above on purpose, because this is not a boundary, it is
				// an overflow. It leaves the chunker alone, like Arrow's data_pagesize, so content boundaries stay put.
				cut_after = page_info.estimated_page_size >= page_size_limit;
			} else {
				page_info.null_count++;
			}
			if (cut_after && (vector_can_span_multiple_pages || i == 0)) {
				PageInformation new_info;
				new_info.offset = page_info.offset + page_info.row_count;
				state.page_info.push_back(new_info);
				page_info_ref = state.page_info.back();
			}
			vector_index++;
		}
	}
	if (parent == nullptr && state.cdc_cuts_per_chunk) {
		state.cdc_cut_chunk_idx++;
	}
}

vector<idx_t> PrimitiveColumnWriter::RollCdcChunk(Vector &vector, idx_t count) {
	duckdb::vector<idx_t> cuts;
	// the same predicate as Prepare's cut_by_content, on the same chunk, gives the same answer
	if (!ParquetCdcSupportedType(vector.GetType())) {
		return cuts;
	}
	auto &validity = FlatVector::ValidityMutable(vector);
	const bool feed_levels = MaxDefine() > 0;
	for (idx_t i = 0; i < count; i++) {
		const bool is_valid = validity.RowIsValid(i);
		if (feed_levels) {
			cdc_chunker.RollLevel(is_valid ? MaxDefine() : MaxDefine() - 1);
		}
		if (is_valid) {
			ParquetCdcHashValue(cdc_chunker, vector, i);
		}
		if (cdc_chunker.NeedNewChunk()) {
			cuts.push_back(i);
		}
	}
	return cuts;
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
		PageWriteInformation write_info;
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

	// now that we have finished writing the data we know the uncompressed size
	if (temp_writer.GetPosition() > idx_t(NumericLimits<int32_t>::Maximum())) {
		throw InternalException("Parquet writer: %d uncompressed page size out of range for type integer",
		                        temp_writer.GetPosition());
	}
	hdr.uncompressed_page_size = UnsafeNumericCast<int32_t>(temp_writer.GetPosition());

	// compress the data
	CompressPage(temp_writer, write_info.compressed_size, write_info.compressed_data, write_info.compressed_buf);
	hdr.compressed_page_size = UnsafeNumericCast<int32_t>(write_info.compressed_size);
	D_ASSERT(hdr.uncompressed_page_size > 0);
	D_ASSERT(hdr.compressed_page_size > 0);

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

		WriteVector(temp_writer, state.stats_state.get(), write_info.page_state.get(), vector, offset,
		            offset + write_count);

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
		duckdb_parquet::PageEncodingStats stat;
		stat.page_type = page_type;
		stat.encoding = encoding;
		stat.count = 1;
		column_chunk.meta_data.encoding_stats.push_back(stat);
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
		D_ASSERT(write_info.page_header.uncompressed_page_size > 0);
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
	idx_t total_uncompressed_size = 0;
	for (auto &write_info : state.write_info) {
		// set the data page offset whenever we see the *first* data page
		if (column_chunk.meta_data.data_page_offset == 0 && (write_info.page_header.type == PageType::DATA_PAGE ||
		                                                     write_info.page_header.type == PageType::DATA_PAGE_V2)) {
			column_chunk.meta_data.data_page_offset = UnsafeNumericCast<int64_t>(column_writer.GetTotalWritten());
		}
		D_ASSERT(write_info.page_header.uncompressed_page_size > 0);
		D_ASSERT(write_info.prepared_header);
		D_ASSERT(write_info.prepared_payload);
		// total uncompressed size in the column chunk includes the header size (!)
		total_uncompressed_size += write_info.prepared_header->Size();
		total_uncompressed_size += write_info.page_header.uncompressed_page_size;
		writer.WriteData(std::move(write_info.prepared_header));
		writer.WriteData(std::move(write_info.prepared_payload));
	}
	column_chunk.meta_data.total_compressed_size =
	    UnsafeNumericCast<int64_t>(column_writer.GetTotalWritten() - start_offset);
	column_chunk.meta_data.total_uncompressed_size = UnsafeNumericCast<int64_t>(total_uncompressed_size);
	state.row_group.total_byte_size += column_chunk.meta_data.total_uncompressed_size;

	if (state.bloom_filter) {
		writer.BufferBloomFilter(state.col_idx, std::move(state.bloom_filter));
	}
	// finalize the stats
	writer.FlushColumnStats(state.col_idx, column_chunk, state.stats_state.get());
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
	D_ASSERT(temp_writer->GetPosition() > 0);

	auto write_info = CreateDictionaryPageWriteInformation(temp_writer->GetPosition(), row_count);
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

	auto write_info = CreateDictionaryPageWriteInformation(target_data.size, row_count);

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

idx_t PrimitiveColumnWriter::FinalizeSchema(vector<duckdb_parquet::SchemaElement> &schemas) {
	idx_t schema_idx = schemas.size();

	auto &schema = column_schema;
	schema.SetSchemaIndex(schema_idx);

	auto &repetition_type = schema.repetition_type;
	auto &name = schema.name;
	auto &field_id = schema.field_id;
	auto &type = schema.type;
	auto allow_geometry = schema.allow_geometry;

	duckdb_parquet::SchemaElement schema_element;
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
	schemas.push_back(std::move(schema_element));

	D_ASSERT(child_writers.empty());
	return 1;
}

} // namespace duckdb
