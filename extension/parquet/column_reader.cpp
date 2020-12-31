#include "column_reader.hpp"
#include "parquet_timestamp.hpp"
#include "utf8proc_wrapper.hpp"

#include "snappy.h"
#include "miniz_wrapper.hpp"
#include "zstd.h"

namespace duckdb {

using parquet::format::CompressionCodec;
using parquet::format::ConvertedType;
using parquet::format::Encoding;
using parquet::format::PageType;
using parquet::format::Type;

const uint32_t RleBpDecoder::BITPACK_MASKS[] = {
    0,       1,       3,        7,        15,       31,        63,        127,       255,        511,       1023,
    2047,    4095,    8191,     16383,    32767,    65535,     131071,    262143,    524287,     1048575,   2097151,
    4194303, 8388607, 16777215, 33554431, 67108863, 134217727, 268435455, 536870911, 1073741823, 2147483647};

const uint8_t RleBpDecoder::BITPACK_DLEN = 8;

ColumnReader::~ColumnReader() {
}

void ColumnReader::PrepareRead(parquet_filter_t &filter) {

	dict_decoder.reset();
	defined_decoder.reset();
	block.reset();

	PageHeader page_hdr;
	page_hdr.read(&protocol);

	PreparePage(page_hdr.compressed_page_size, page_hdr.uncompressed_page_size);

	switch (page_hdr.type) {
	case PageType::DATA_PAGE_V2:
	case PageType::DATA_PAGE:
		PrepareDataPage(page_hdr);
		break;
	case PageType::DICTIONARY_PAGE:
		Dictionary(move(block), page_hdr.dictionary_page_header.num_values);
		break;
	default:
		break; // ignore INDEX page type and any other custom extensions
	}
}

void ColumnReader::PreparePage(idx_t compressed_page_size, idx_t uncompressed_page_size) {
	auto trans = (DuckdbFileTransport *)protocol.getTransport().get();

	block = make_shared<ResizeableBuffer>(compressed_page_size + 1);
	trans->read((uint8_t *)block->ptr, compressed_page_size);

	//			page_hdr.printTo(std::cout);
	//			std::cout << '\n';

	shared_ptr<ResizeableBuffer> unpacked_block;
	if (chunk.meta_data.codec != CompressionCodec::UNCOMPRESSED) {
		unpacked_block = make_shared<ResizeableBuffer>(uncompressed_page_size + 1);
	}

	switch (chunk.meta_data.codec) {
	case CompressionCodec::UNCOMPRESSED:
		break;
	case CompressionCodec::GZIP: {
		MiniZStream s;

		s.Decompress((const char *)block->ptr, compressed_page_size, (char *)unpacked_block->ptr,
		             uncompressed_page_size);
		block = move(unpacked_block);

		break;
	}
	case CompressionCodec::SNAPPY: {
		auto res = snappy::RawUncompress((const char *)block->ptr, compressed_page_size, (char *)unpacked_block->ptr);
		if (!res) {
			// TODO throw FormatException("Decompression failure");
		}
		block = move(unpacked_block);
		break;
	}
	case CompressionCodec::ZSTD: {
		auto res = duckdb_zstd::ZSTD_decompress((char *)unpacked_block->ptr, uncompressed_page_size,
		                                        (const char *)block->ptr, compressed_page_size);
		if (duckdb_zstd::ZSTD_isError(res) || res != (size_t)uncompressed_page_size) {
			// throw FormatException("ZSTD Decompression failure");
		}
		block = move(unpacked_block);
		break;
	}

	default: {
		std::stringstream codec_name;
		codec_name << chunk.meta_data.codec;
		D_ASSERT(0);
		//        throw FormatException("Unsupported compression codec \"" + codec_name.str() +
		//                              "\". Supported options are uncompressed, gzip or snappy");
		break;
	}
	}
}

void ColumnReader::PrepareDataPage(PageHeader &page_hdr) {

	if (page_hdr.type == PageType::DATA_PAGE) {
		D_ASSERT(page_hdr.__isset.data_page_header);
	}
	if (page_hdr.type == PageType::DATA_PAGE_V2) {
		D_ASSERT(page_hdr.__isset.data_page_header_v2);
	}

	rows_available = page_hdr.type == PageType::DATA_PAGE ? page_hdr.data_page_header.num_values
	                                                      : page_hdr.data_page_header_v2.num_values;
	auto page_encoding = page_hdr.type == PageType::DATA_PAGE ? page_hdr.data_page_header.encoding
	                                                          : page_hdr.data_page_header_v2.encoding;

	if (can_have_nulls) {
		D_ASSERT(page_hdr.data_page_header.definition_level_encoding == Encoding::RLE);
		uint32_t def_length = block->read<uint32_t>();
		block->available(def_length);
		defined_decoder = make_unique<RleBpDecoder>((const uint8_t *)block->ptr, def_length, 1);
		block->inc(def_length);
	}

	switch (page_encoding) {
	case Encoding::RLE_DICTIONARY:
	case Encoding::PLAIN_DICTIONARY: {
		auto dict_width = block->read<uint8_t>();
		// TODO somehow dict_width can be 0 ?
		dict_decoder = make_unique<RleBpDecoder>((const uint8_t *)block->ptr, block->len, dict_width);
		block->inc(block->len);
		break;
	}
	case Encoding::PLAIN:
		// nothing to do here, will be read directly below
		break;

	default:
		D_ASSERT(0);
		break;
	}
}

void ColumnReader::Read(uint64_t num_values, parquet_filter_t &filter, Vector &result) {
	auto trans = (DuckdbFileTransport *)protocol.getTransport().get();
	trans->SetLocation(chunk_read_offset);

	idx_t to_read = num_values;
	idx_t result_offset = 0;

	while (to_read > 0) {
		while (rows_available == 0) {
			PrepareRead(filter);
		}

		D_ASSERT(block);
		auto read_now = MinValue<idx_t>(to_read, rows_available);

		if (can_have_nulls) {
			D_ASSERT(defined_decoder);
			defined_buffer.resize(sizeof(uint8_t) * read_now);
			defined_decoder->GetBatch<uint8_t>(defined_buffer.ptr, read_now);
		}

		if (dict_decoder) {
			// TODO computing this here is a wee bit ugly
			// we need the null count because the offsets have no entries for nulls
			idx_t null_count = 0;
			if (can_have_nulls) {
				for (idx_t i = 0; i < read_now; i++) {
					if (!defined_buffer.ptr[i]) {
						null_count++;
					}
				}
			}

			offset_buffer.resize(sizeof(uint32_t) * (read_now - null_count));
			dict_decoder->GetBatch<uint32_t>(offset_buffer.ptr, read_now - null_count);
			DictReference(result);
			Offsets((uint32_t *)offset_buffer.ptr, (uint8_t *)defined_buffer.ptr, read_now, filter, result_offset,
			        result);
		} else {
			PlainReference(block, result);
			Plain(block, (uint8_t *)defined_buffer.ptr, read_now, filter, result_offset, result);
		}
		result_offset += read_now;
		rows_available -= read_now;
		to_read -= read_now;
	}
	chunk_read_offset = trans->GetLocation();
}

void StringColumnReader::VerifyString(LogicalTypeId id, const char *str_data, idx_t str_len) {
	if (id != LogicalTypeId::VARCHAR) {
		return;
	}
	// verify if a string is actually UTF8, and if there are no null bytes in the middle of the string
	// technically Parquet should guarantee this, but reality is often disappointing
	auto utf_type = Utf8Proc::Analyze(str_data, str_len);
	if (utf_type == UnicodeType::INVALID) {
		throw InternalException("Invalid string encoding found in Parquet file: value is not valid UTF8!");
	}
}

void StringColumnReader::Dictionary(shared_ptr<ByteBuffer> data, idx_t num_entries) {
	dict = move(data);
	dict_strings = unique_ptr<string_t[]>(new string_t[num_entries]);
	for (idx_t dict_idx = 0; dict_idx < num_entries; dict_idx++) {
		// TODO we can apply filters here already and put a marker into dict
		uint32_t str_len = dict->read<uint32_t>();
		dict->available(str_len);

		VerifyString(type.id(), dict->ptr, str_len);
		dict_strings[dict_idx] = string_t(dict->ptr, str_len);
		dict->inc(str_len);
	}
}

class ParquetStringVectorBuffer : public VectorBuffer {
public:
	ParquetStringVectorBuffer(shared_ptr<ByteBuffer> buffer_p)
	    : VectorBuffer(VectorBufferType::OPAQUE_BUFFER), buffer(move(buffer_p)) {
	}

private:
	shared_ptr<ByteBuffer> buffer;
};

void StringColumnReader::DictReference(Vector &result) {
	StringVector::AddBuffer(result, make_unique<ParquetStringVectorBuffer>(dict));
}
void StringColumnReader::PlainReference(shared_ptr<ByteBuffer> plain_data, Vector &result) {
	StringVector::AddBuffer(result, make_unique<ParquetStringVectorBuffer>(move(plain_data)));
}

string_t StringColumnReader::DictRead(uint32_t &offset) {
	return dict_strings[offset];
}

string_t StringColumnReader::PlainRead(ByteBuffer &plain_data) {
	uint32_t str_len = plain_data.read<uint32_t>();
	plain_data.available(str_len);
	VerifyString(type.id(), plain_data.ptr, str_len);
	auto ret_str = string_t(plain_data.ptr, str_len);
	plain_data.inc(str_len);
	return ret_str;
}

void StringColumnReader::PlainSkip(ByteBuffer &plain_data) {
	uint32_t str_len = plain_data.read<uint32_t>();
	plain_data.available(str_len);
	plain_data.inc(str_len);
}

void StringColumnReader::Skip(idx_t num_values) {
	D_ASSERT(0);
}

} // namespace duckdb
