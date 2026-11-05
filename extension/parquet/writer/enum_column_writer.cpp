#include "writer/enum_column_writer.hpp"

#include <utility>

#include "parquet_rle_bp_decoder.hpp"
#include "parquet_rle_bp_encoder.hpp"
#include "parquet_writer.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/common/allocator.hpp"
#include "duckdb/common/assert.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/serializer/write_stream.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/validity_mask.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "parquet_column_schema.hpp"

namespace duckdb {
class Vector;

using duckdb_parquet::Encoding;

namespace {
class EnumStatisticsState : public StringStatisticsState {
public:
	bool MinIsExact() override {
		return false;
	}
	bool MaxIsExact() override {
		return false;
	}
};

class EnumColumnWriterState : public PrimitiveColumnWriterState {
public:
	EnumColumnWriterState(ParquetWriter &writer, duckdb_parquet::RowGroup &row_group, idx_t col_idx, idx_t enum_count)
	    : PrimitiveColumnWriterState(writer, row_group, col_idx), seen_enum(enum_count, false) {
	}

	//! Tracks which enum values have been written in this row group
	vector<bool> seen_enum;
};
} // namespace

class EnumWriterPageState : public ColumnWriterPageState {
public:
	EnumWriterPageState(uint32_t bit_width, vector<bool> &seen_enum)
	    : encoder(bit_width), written_value(false), seen_enum(seen_enum) {
	}

	RleBpEncoder encoder;
	bool written_value;
	vector<bool> &seen_enum;
};

EnumColumnWriter::EnumColumnWriter(ParquetWriter &writer, ParquetColumnSchema &&column_schema,
                                   vector<Identifier> schema_path_p)
    : PrimitiveColumnWriter(writer, std::move(column_schema), std::move(schema_path_p)) {
	bit_width = RleBpDecoder::ComputeBitWidthFromValueCount(EnumType::GetSize(Type()));
}

unique_ptr<ColumnWriterState> EnumColumnWriter::InitializeWriteState(duckdb_parquet::RowGroup &row_group) {
	auto result =
	    make_uniq<EnumColumnWriterState>(writer, row_group, row_group.columns.size(), EnumType::GetSize(Type()));
	RegisterToRowGroup(row_group);
	return std::move(result);
}

unique_ptr<ColumnWriterStatistics> EnumColumnWriter::InitializeStatsState() {
	return make_uniq<EnumStatisticsState>();
}

template <class T>
void EnumColumnWriter::WriteEnumInternal(WriteStream &temp_writer, Vector &input_column, idx_t chunk_start,
                                         idx_t chunk_end, EnumWriterPageState &page_state) {
	auto &mask = FlatVector::ValidityMutable(input_column);
	auto *ptr = FlatVector::GetData<T>(input_column);
	for (idx_t r = chunk_start; r < chunk_end; r++) {
		if (mask.RowIsValid(r)) {
			if (!page_state.written_value) {
				// first value: write the bit-width as a one-byte entry and initialize writer
				temp_writer.Write<uint8_t>(bit_width);
				page_state.encoder.BeginWrite();
				page_state.written_value = true;
			}
			page_state.encoder.WriteValue(temp_writer, ptr[r]);
			page_state.seen_enum[ptr[r]] = true;
		}
	}
}

void EnumColumnWriter::WriteVector(WriteStream &temp_writer, ColumnWriterStatistics *stats_p,
                                   ColumnWriterPageState *page_state_p, Vector &input_column, idx_t chunk_start,
                                   idx_t chunk_end) {
	auto &page_state = page_state_p->Cast<EnumWriterPageState>();
	switch (Type().InternalType()) {
	case PhysicalType::UINT8:
		WriteEnumInternal<uint8_t>(temp_writer, input_column, chunk_start, chunk_end, page_state);
		break;
	case PhysicalType::UINT16:
		WriteEnumInternal<uint16_t>(temp_writer, input_column, chunk_start, chunk_end, page_state);
		break;
	case PhysicalType::UINT32:
		WriteEnumInternal<uint32_t>(temp_writer, input_column, chunk_start, chunk_end, page_state);
		break;
	default:
		throw InternalException("Unsupported internal enum type");
	}
}

unique_ptr<ColumnWriterPageState> EnumColumnWriter::InitializePageState(PrimitiveColumnWriterState &state_p,
                                                                        idx_t page_idx) {
	auto &state = state_p.Cast<EnumColumnWriterState>();
	return make_uniq<EnumWriterPageState>(bit_width, state.seen_enum);
}

void EnumColumnWriter::FlushPageState(WriteStream &temp_writer, ColumnWriterPageState *state_p) {
	auto &page_state = state_p->Cast<EnumWriterPageState>();
	if (!page_state.written_value) {
		// all values are null
		// just write the bit width
		temp_writer.Write<uint8_t>(bit_width);
		return;
	}
	page_state.encoder.FinishWrite(temp_writer);
}

duckdb_parquet::Encoding::type EnumColumnWriter::GetEncoding(PrimitiveColumnWriterState &state) {
	return Encoding::RLE_DICTIONARY;
}

bool EnumColumnWriter::HasDictionary(PrimitiveColumnWriterState &state) {
	return true;
}

idx_t EnumColumnWriter::DictionarySize(PrimitiveColumnWriterState &state_p) {
	auto &state = state_p.Cast<EnumColumnWriterState>();
	idx_t distinct_count = 0;
	for (auto seen : state.seen_enum) {
		distinct_count += seen;
	}
	return distinct_count;
}

void EnumColumnWriter::FlushDictionary(PrimitiveColumnWriterState &state_p, ColumnWriterStatistics *stats_p) {
	auto &state = state_p.Cast<EnumColumnWriterState>();
	auto &stats = stats_p->Cast<StringStatisticsState>();
	auto &enum_values = EnumType::GetValuesInsertOrder(Type());
	auto enum_count = EnumType::GetSize(Type());
	auto string_values = FlatVector::GetData<string_t>(enum_values);
	// first write the contents of the dictionary page to a temporary buffer
	auto temp_writer = make_uniq<MemoryStream>(BufferAllocator::Get(writer.GetContext()));
	for (idx_t r = 0; r < enum_count; r++) {
		D_ASSERT(!FlatVector::IsNull(enum_values, r));
		if (state.seen_enum[r]) {
			stats.Update(string_values[r]);
			temp_writer->Write<uint32_t>(string_values[r].GetSize());
			temp_writer->WriteData(const_data_ptr_cast(string_values[r].GetData()), string_values[r].GetSize());
		} else {
			temp_writer->Write<uint32_t>(0);
		}
	}
	// flush the dictionary page and add it to the to-be-written pages
	WriteDictionary(state, std::move(temp_writer), enum_count);
}

idx_t EnumColumnWriter::GetRowSize(const Vector &vector, const idx_t index,
                                   const PrimitiveColumnWriterState &state) const {
	return (bit_width + 7) / 8;
}

} // namespace duckdb
