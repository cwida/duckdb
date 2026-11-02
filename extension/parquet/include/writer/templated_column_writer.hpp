//===----------------------------------------------------------------------===//
//                         DuckDB
//
// writer/templated_column_writer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "writer/primitive_column_writer.hpp"
#include "writer/parquet_write_operators.hpp"
#include "parquet_bss_encoder.hpp"
#include "parquet_dbp_encoder.hpp"
#include "parquet_dlba_encoder.hpp"
#include "parquet_rle_bp_decoder.hpp"
#include "parquet_rle_bp_encoder.hpp"
#include "duckdb/common/primitive_dictionary.hpp"

namespace duckdb {

template <class SRC, class TGT, class OP = ParquetCastOperator, bool ALL_VALID>
static void TemplatedWritePlain(Vector &col, ColumnWriterStatistics *stats, const idx_t chunk_start,
                                const idx_t chunk_end, const ValidityMask &mask, WriteStream &ser) {
	static constexpr bool COPY_DIRECTLY_FROM_VECTOR = ALL_VALID && std::is_same<SRC, TGT>::value &&
	                                                  std::is_arithmetic<TGT>::value &&
	                                                  std::is_same<OP, ParquetCastOperator>::value;

	const auto *const ptr = FlatVector::GetData<SRC>(col);

	TGT local_write[STANDARD_VECTOR_SIZE];
	idx_t local_write_count = 0;

	for (idx_t r = chunk_start; r < chunk_end; r++) {
		if (!ALL_VALID && !mask.RowIsValid(r)) {
			continue;
		}

		TGT target_value = OP::template Operation<SRC, TGT>(ptr[r]);
		OP::template HandleStats<SRC, TGT>(stats, target_value);

		if (COPY_DIRECTLY_FROM_VECTOR) {
			continue;
		}

		if (std::is_arithmetic<TGT>::value) {
			local_write[local_write_count++] = target_value;
			if (local_write_count == STANDARD_VECTOR_SIZE) {
				ser.WriteData(data_ptr_cast(local_write), local_write_count * sizeof(TGT));
				local_write_count = 0;
			}
		} else {
			OP::template WriteToStream<SRC, TGT>(target_value, ser);
		}
	}

	if (COPY_DIRECTLY_FROM_VECTOR) {
		ser.WriteData(const_data_ptr_cast(&ptr[chunk_start]), (chunk_end - chunk_start) * sizeof(TGT));
		return;
	}

	if (std::is_arithmetic<TGT>::value) {
		ser.WriteData(data_ptr_cast(local_write), local_write_count * sizeof(TGT));
	}
	// Else we already wrote to stream
}

template <class SRC, class TGT, class OP>
class StandardColumnWriterState : public PrimitiveColumnWriterState {
public:
	StandardColumnWriterState(ParquetWriter &writer, duckdb_parquet::RowGroup &row_group, idx_t col_idx)
	    : PrimitiveColumnWriterState(writer, row_group, col_idx),
	      dictionary(BufferAllocator::Get(writer.GetContext()),
	                 writer.DictionarySizeLimit().IsValid() ? writer.DictionarySizeLimit().GetIndex()
	                                                        : NumericCast<idx_t>(row_group.num_rows) / 5,
	                 writer.StringDictionaryPageSizeLimit()),
	      encoding(duckdb_parquet::Encoding::PLAIN) {
	}
	~StandardColumnWriterState() override = default;

	uint32_t key_bit_width = 0;

	PrimitiveDictionary<SRC, TGT, OP> dictionary;
	duckdb_parquet::Encoding::type encoding;
};

template <class SRC, class TGT, class OP>
class StandardWriterPageState : public ColumnWriterPageState {
public:
	explicit StandardWriterPageState(const idx_t total_value_count, const idx_t estimated_page_size,
	                                 duckdb_parquet::Encoding::type encoding_p,
	                                 const PrimitiveDictionary<SRC, TGT, OP> &dictionary_p)
	    : encoding(encoding_p), dbp_initialized(false), dbp_encoder(total_value_count), dlba_initialized(false),
	      dlba_encoder(total_value_count, estimated_page_size), bss_initialized(false),
	      bss_encoder(total_value_count, sizeof(TGT)), dictionary(dictionary_p), dict_written_value(false),
	      dict_bit_width(RleBpDecoder::ComputeBitWidthFromValueCount(dictionary.GetSize())),
	      dict_encoder(dict_bit_width) {
	}
	duckdb_parquet::Encoding::type encoding;

	bool dbp_initialized;
	DbpEncoder dbp_encoder;

	bool dlba_initialized;
	DlbaEncoder dlba_encoder;

	bool bss_initialized;
	BssEncoder bss_encoder;

	const PrimitiveDictionary<SRC, TGT, OP> &dictionary;
	bool dict_written_value;
	uint32_t dict_bit_width;
	RleBpEncoder dict_encoder;
};

template <class SRC, class TGT, class OP = ParquetCastOperator>
class StandardColumnWriter : public PrimitiveColumnWriter {
public:
	StandardColumnWriter(ParquetWriter &writer, ParquetColumnSchema &&column_schema, vector<Identifier> schema_path_p)
	    : PrimitiveColumnWriter(writer, std::move(column_schema), std::move(schema_path_p)) {
	}
	~StandardColumnWriter() override = default;

	//! Dictionary encoding normally updates the statistics once per distinct dictionary value.
	//! Floating point statistics include nan_count, which counts every NaN value written, so they are updated per row.
	static constexpr bool STATS_PER_OCCURRENCE = std::is_same<OP, FloatingPointOperator>::value;

public:
	unique_ptr<ColumnWriterState> InitializeWriteState(duckdb_parquet::RowGroup &row_group) override {
		auto result = make_uniq<StandardColumnWriterState<SRC, TGT, OP>>(writer, row_group, row_group.columns.size());
		result->encoding = writer.GetParquetVersion() == ParquetVersion::V1 ? duckdb_parquet::Encoding::PLAIN_DICTIONARY
		                                                                    : duckdb_parquet::Encoding::RLE_DICTIONARY;
		RegisterToRowGroup(row_group);
		return std::move(result);
	}

	unique_ptr<ColumnWriterPageState> InitializePageState(PrimitiveColumnWriterState &state_p,
	                                                      idx_t page_idx) override {
		auto &state = state_p.Cast<StandardColumnWriterState<SRC, TGT, OP>>();
		const auto &page_info = state_p.page_info[page_idx];
		// The prepared page size bounds the string payload for DELTA_LENGTH_BYTE_ARRAY.
		auto result = make_uniq<StandardWriterPageState<SRC, TGT, OP>>(
		    page_info.row_count - (page_info.empty_count + page_info.null_count), page_info.estimated_page_size,
		    state.encoding, state.dictionary);
		return std::move(result);
	}

	void FlushPageState(WriteStream &temp_writer, ColumnWriterPageState *state_p) override {
		auto &page_state = state_p->Cast<StandardWriterPageState<SRC, TGT, OP>>();
		switch (page_state.encoding) {
		case duckdb_parquet::Encoding::DELTA_BINARY_PACKED:
			if (!page_state.dbp_initialized) {
				page_state.dbp_encoder.BeginWrite(temp_writer, 0);
			}
			page_state.dbp_encoder.FinishWrite(temp_writer);
			break;
		case duckdb_parquet::Encoding::PLAIN_DICTIONARY:
			// PLAIN_DICTIONARY can be treated the same as RLE_DICTIONARY
		case duckdb_parquet::Encoding::RLE_DICTIONARY:
			D_ASSERT(page_state.dict_bit_width != 0);
			if (!page_state.dict_written_value) {
				// all values are null
				// just write the bit width
				temp_writer.Write<uint8_t>(page_state.dict_bit_width);
				return;
			}
			page_state.dict_encoder.FinishWrite(temp_writer);
			break;
		case duckdb_parquet::Encoding::DELTA_LENGTH_BYTE_ARRAY:
			if (!page_state.dlba_initialized) {
				page_state.dlba_encoder.BeginWrite(BufferAllocator::Get(writer.GetContext()), temp_writer,
				                                   string_t(""));
			}
			page_state.dlba_encoder.FinishWrite(temp_writer);
			break;
		case duckdb_parquet::Encoding::BYTE_STREAM_SPLIT:
			if (!page_state.bss_initialized) {
				page_state.bss_encoder.BeginWrite(BufferAllocator::Get(writer.GetContext()));
			}
			page_state.bss_encoder.FinishWrite(temp_writer);
			break;
		case duckdb_parquet::Encoding::PLAIN:
			break;
		default:
			throw InternalException("Unknown encoding");
		}
	}

	duckdb_parquet::Encoding::type GetEncoding(PrimitiveColumnWriterState &state_p) override {
		auto &state = state_p.Cast<StandardColumnWriterState<SRC, TGT, OP>>();
		return state.encoding;
	}

	bool HasAnalyze() override {
		return true;
	}

	void Analyze(ColumnWriterState &state_p, ColumnWriterState *parent, Vector &vector, idx_t count) override {
		auto &state = state_p.Cast<StandardColumnWriterState<SRC, TGT, OP>>();

		auto data_ptr = FlatVector::GetData<SRC>(vector);
		idx_t vector_index = 0;

		const bool check_parent_empty = parent && !parent->is_empty.empty();
		const idx_t parent_index = state.definition_levels.size();
		D_ASSERT(!check_parent_empty || parent_index < parent->is_empty.size());

		const idx_t vcount =
		    check_parent_empty ? parent->definition_levels.size() - state.definition_levels.size() : count;

		const auto &validity = FlatVector::Validity(vector);

		if (!check_parent_empty && validity.CannotHaveNull()) {
			// Fast path
			for (; vector_index < vcount; vector_index++) {
				const auto &src_value = data_ptr[vector_index];
				state.dictionary.template Insert<true>(src_value);
			}
		} else {
			for (idx_t i = 0; i < vcount; i++) {
				if (check_parent_empty && parent->is_empty[parent_index + i]) {
					continue;
				}
				if (validity.RowIsValid(vector_index)) {
					const auto &src_value = data_ptr[vector_index];
					state.dictionary.template Insert<true>(src_value);
				}
				vector_index++;
			}
		}
	}

	void FinalizeAnalyze(ColumnWriterState &state_p) override {
		const auto type = writer.GetType(SchemaIndex());

		auto &state = state_p.Cast<StandardColumnWriterState<SRC, TGT, OP>>();
		if (state.dictionary.GetSize() == 0 || state.dictionary.IsFull()) {
			state.dictionary.Reset();
			if (writer.GetParquetVersion() == ParquetVersion::V1) {
				// Can't do the cool stuff for V1
				state.encoding = duckdb_parquet::Encoding::PLAIN;
			} else {
				// If we aren't doing dictionary encoding, these encodings are virtually always better than PLAIN
				switch (type) {
				case duckdb_parquet::Type::type::INT32:
				case duckdb_parquet::Type::type::INT64:
					state.encoding = duckdb_parquet::Encoding::DELTA_BINARY_PACKED;
					break;
				case duckdb_parquet::Type::type::BYTE_ARRAY:
					state.encoding = duckdb_parquet::Encoding::DELTA_LENGTH_BYTE_ARRAY;
					break;
				case duckdb_parquet::Type::type::FLOAT:
				case duckdb_parquet::Type::type::DOUBLE:
					state.encoding = duckdb_parquet::Encoding::BYTE_STREAM_SPLIT;
					break;
				default:
					state.encoding = duckdb_parquet::Encoding::PLAIN;
				}
			}
		} else {
			state.key_bit_width = RleBpDecoder::ComputeBitWidthFromValueCount(state.dictionary.GetSize());
		}
	}

	unique_ptr<ColumnWriterStatistics> InitializeStatsState() override {
		return OP::template InitializeStats<SRC, TGT>();
	}

	bool HasDictionary(PrimitiveColumnWriterState &state_p) override {
		auto &state = state_p.Cast<StandardColumnWriterState<SRC, TGT, OP>>();
		return state.encoding == duckdb_parquet::Encoding::RLE_DICTIONARY ||
		       state.encoding == duckdb_parquet::Encoding::PLAIN_DICTIONARY;
	}

	idx_t DictionarySize(PrimitiveColumnWriterState &state_p) override {
		auto &state = state_p.Cast<StandardColumnWriterState<SRC, TGT, OP>>();
		return state.dictionary.GetSize();
	}

	void WriteVector(WriteStream &temp_writer, ColumnWriterStatistics *stats, ColumnWriterPageState *page_state_p,
	                 Vector &input_column, idx_t chunk_start, idx_t chunk_end) override {
		const auto &mask = FlatVector::Validity(input_column);
		if (mask.CannotHaveNull()) {
			WriteVectorInternal<true>(temp_writer, stats, page_state_p, input_column, chunk_start, chunk_end);
		} else {
			WriteVectorInternal<false>(temp_writer, stats, page_state_p, input_column, chunk_start, chunk_end);
		}
	}

	void FlushDictionary(PrimitiveColumnWriterState &state_p, ColumnWriterStatistics *stats) override {
		auto &state = state_p.Cast<StandardColumnWriterState<SRC, TGT, OP>>();
		D_ASSERT(state.encoding == duckdb_parquet::Encoding::RLE_DICTIONARY ||
		         state.encoding == duckdb_parquet::Encoding::PLAIN_DICTIONARY);

		if (writer.EnableBloomFilters()) {
			auto bloom_filter_entries =
			    state.dictionary.GetSize() * OP::template BloomFilterEntriesPerValue<SRC, TGT>();
			state.bloom_filter =
			    make_uniq<ParquetBloomFilter>(bloom_filter_entries, writer.BloomFilterFalsePositiveRatio());
		}

		// the chunk statistics are the merged page statistics when the page index is written
		const bool stats_from_dictionary = !STATS_PER_OCCURRENCE && !writer.WritePageIndex();
		state.dictionary.IterateValues([&](const SRC &src_value, const TGT &tgt_value) {
			if (stats_from_dictionary) {
				// update the statistics
				OP::template HandleStats<SRC, TGT>(stats, tgt_value);
			}
			if (state.bloom_filter) {
				// update the bloom filter
				auto hash = OP::template XXHash64<SRC, TGT>(tgt_value);
				state.bloom_filter->FilterInsert(hash);
				auto extra_hash = OP::template GetExtraBloomFilterHash<SRC, TGT>(src_value, tgt_value);
				if (extra_hash) {
					state.bloom_filter->FilterInsert(*extra_hash);
				}
			}
		});

		// flush the dictionary page and add it to the to-be-written pages
		auto dictionary_size = state.dictionary.GetSize();
		WriteDictionary(state, state.dictionary.TakeTargetData(), dictionary_size);
		// bloom filter will be queued for writing in ParquetWriter::BufferBloomFilter one level up
	}

	idx_t GetRowSize(const Vector &vector, const idx_t index,
	                 const PrimitiveColumnWriterState &state_p) const override {
		auto &state = state_p.Cast<StandardColumnWriterState<SRC, TGT, OP>>();
		if (state.encoding == duckdb_parquet::Encoding::RLE_DICTIONARY ||
		    state.encoding == duckdb_parquet::Encoding::PLAIN_DICTIONARY) {
			return (state.key_bit_width + 7) / 8;
		} else {
			return OP::template GetRowSize<SRC, TGT>(vector, index);
		}
	}

private:
	template <bool ALL_VALID>
	void WriteVectorInternal(WriteStream &temp_writer, ColumnWriterStatistics *stats,
	                         ColumnWriterPageState *page_state_p, Vector &input_column, idx_t chunk_start,
	                         idx_t chunk_end) {
		auto &page_state = page_state_p->Cast<StandardWriterPageState<SRC, TGT, OP>>();

		const auto &mask = FlatVector::Validity(input_column);
		const auto *data_ptr = FlatVector::GetData<SRC>(input_column);

		switch (page_state.encoding) {
		case duckdb_parquet::Encoding::PLAIN_DICTIONARY:
			// PLAIN_DICTIONARY can be treated the same as RLE_DICTIONARY
		case duckdb_parquet::Encoding::RLE_DICTIONARY: {
			idx_t r = chunk_start;
			if (!page_state.dict_written_value) {
				// find first non-null value
				for (; r < chunk_end; r++) {
					if (!mask.RowIsValid(r)) {
						continue;
					}
					// write the bit-width as a one-byte entry and initialize writer
					temp_writer.Write<uint8_t>(page_state.dict_bit_width);
					page_state.dict_encoder.BeginWrite();
					page_state.dict_written_value = true;
					break;
				}
			}

			// the page index needs the bounds of every page, which the dictionary cannot provide
			const bool stats_per_occurrence = STATS_PER_OCCURRENCE || writer.WritePageIndex();
			for (; r < chunk_end; r++) {
				if (!ALL_VALID && !mask.RowIsValid(r)) {
					continue;
				}
				const auto &src_value = data_ptr[r];
				if (stats_per_occurrence) {
					OP::template HandleStats<SRC, TGT>(stats, OP::template Operation<SRC, TGT>(src_value));
				}
				const auto value_index = page_state.dictionary.GetIndex(src_value);
				page_state.dict_encoder.WriteValue(temp_writer, value_index);
			}
			break;
		}
		case duckdb_parquet::Encoding::DELTA_BINARY_PACKED: {
			idx_t r = chunk_start;
			if (!page_state.dbp_initialized) {
				// find first non-null value
				for (; r < chunk_end; r++) {
					if (!mask.RowIsValid(r)) {
						continue;
					}
					const TGT target_value = OP::template Operation<SRC, TGT>(data_ptr[r]);
					OP::template HandleStats<SRC, TGT>(stats, target_value);
					page_state.dbp_encoder.BeginWrite(temp_writer, target_value);
					page_state.dbp_initialized = true;
					r++; // skip over
					break;
				}
			}

			for (; r < chunk_end; r++) {
				if (!ALL_VALID && !mask.RowIsValid(r)) {
					continue;
				}
				const TGT target_value = OP::template Operation<SRC, TGT>(data_ptr[r]);
				OP::template HandleStats<SRC, TGT>(stats, target_value);
				page_state.dbp_encoder.WriteValue(temp_writer, target_value);
			}
			break;
		}
		case duckdb_parquet::Encoding::DELTA_LENGTH_BYTE_ARRAY: {
			idx_t r = chunk_start;
			if (!page_state.dlba_initialized) {
				// find first non-null value
				for (; r < chunk_end; r++) {
					if (!mask.RowIsValid(r)) {
						continue;
					}
					const TGT target_value = OP::template Operation<SRC, TGT>(data_ptr[r]);
					OP::template HandleStats<SRC, TGT>(stats, target_value);
					page_state.dlba_encoder.BeginWrite(BufferAllocator::Get(writer.GetContext()), temp_writer,
					                                   target_value);
					page_state.dlba_initialized = true;
					r++; // skip over
					break;
				}
			}

			for (; r < chunk_end; r++) {
				if (!ALL_VALID && !mask.RowIsValid(r)) {
					continue;
				}
				const TGT target_value = OP::template Operation<SRC, TGT>(data_ptr[r]);
				OP::template HandleStats<SRC, TGT>(stats, target_value);
				page_state.dlba_encoder.WriteValue(temp_writer, target_value);
			}
			break;
		}
		case duckdb_parquet::Encoding::BYTE_STREAM_SPLIT: {
			if (!page_state.bss_initialized) {
				page_state.bss_encoder.BeginWrite(BufferAllocator::Get(writer.GetContext()));
				page_state.bss_initialized = true;
			}
			for (idx_t r = chunk_start; r < chunk_end; r++) {
				if (!ALL_VALID && !mask.RowIsValid(r)) {
					continue;
				}
				const TGT target_value = OP::template Operation<SRC, TGT>(data_ptr[r]);
				OP::template HandleStats<SRC, TGT>(stats, target_value);
				page_state.bss_encoder.WriteValue(target_value);
			}
			break;
		}
		case duckdb_parquet::Encoding::PLAIN: {
			D_ASSERT(page_state.encoding == duckdb_parquet::Encoding::PLAIN);
			if (mask.CannotHaveNull()) {
				TemplatedWritePlain<SRC, TGT, OP, true>(input_column, stats, chunk_start, chunk_end, mask, temp_writer);
			} else {
				TemplatedWritePlain<SRC, TGT, OP, false>(input_column, stats, chunk_start, chunk_end, mask,
				                                         temp_writer);
			}
			break;
		}
		default:
			throw InternalException("Unknown encoding");
		}
	}
};

} // namespace duckdb
