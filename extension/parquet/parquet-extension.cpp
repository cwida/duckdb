#include <string>
#include <vector>
#include <bitset>
#include <fstream>
#include <cstring>
#include <iostream>

#include "parquet-extension.hpp"

#ifndef DUCKDB_AMALGAMATION
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "utf8proc_wrapper.hpp"

#include "thrift/protocol/TCompactProtocol.h"
#include "thrift/transport/TBufferTransports.h"
#include "parquet_types.h"
#include "snappy.h"
#endif

using namespace duckdb;
using namespace std;

using namespace parquet;
using namespace parquet::format;
using namespace apache::thrift;
using namespace apache::thrift::protocol;
using namespace apache::thrift::transport;

struct Int96 {
	uint32_t value[3];
};

class ByteBuffer { // on to the 10 thousandth impl
public:
	char *ptr = nullptr;
	uint64_t len = 0;

	ByteBuffer(){};
	ByteBuffer(char *ptr, uint64_t len) : ptr(ptr), len(len){};

	void inc(uint64_t increment) {
		available(increment);
		len -= increment;
		ptr += increment;
	}

	template <class T> T read() {
		available(sizeof(T));
		T val = *(T *)ptr;
		inc(sizeof(T));
		return val;
	}

	void copy_to(char *dest, uint64_t len) {
		available(len);
		memcpy(dest, ptr, len);
	}

	void available(uint64_t req_len) {
		if (req_len > len) {
			throw runtime_error("Out of buffer");
		}
	}
};

class ResizeableBuffer : public ByteBuffer {
public:
	void resize(uint64_t new_size) {
		if (new_size > len) {
			auto new_holder = std::unique_ptr<char[]>(new char[new_size]);
			holder = move(new_holder);
			ptr = holder.get();
		}
		len = new_size;
	}

private:
	std::unique_ptr<char[]> holder = nullptr;
};

static TCompactProtocolFactoryT<TMemoryBuffer> tproto_factory;

template <class T> static void thrift_unpack(const uint8_t *buf, uint32_t *len, T *deserialized_msg) {
	shared_ptr<TMemoryBuffer> tmem_transport(new TMemoryBuffer(const_cast<uint8_t *>(buf), *len));
	shared_ptr<TProtocol> tproto = tproto_factory.getProtocol(tmem_transport);
	try {
		deserialized_msg->read(tproto.get());
	} catch (std::exception &e) {
		std::stringstream ss;
		ss << "Couldn't deserialize thrift: " << e.what() << "\n";
		throw std::runtime_error(ss.str());
	}
	uint32_t bytes_left = tmem_transport->available_read();
	*len = *len - bytes_left;
}

// adapted from arrow parquet reader
class RleBpDecoder {

public:
	/// Create a decoder object. buffer/buffer_len is the decoded data.
	/// bit_width is the width of each value (before encoding).
	RleBpDecoder(const uint8_t *buffer, uint32_t buffer_len, uint32_t bit_width)
	    : buffer(buffer), bit_width_(bit_width), current_value_(0), repeat_count_(0), literal_count_(0) {

		if (bit_width >= 64) {
			throw runtime_error("Decode bit width too large");
		}
		byte_encoded_len = ((bit_width_ + 7) / 8);
		max_val = (1 << bit_width_) - 1;
	}

	/// Gets a batch of values.  Returns the number of decoded elements.
	template <typename T> void GetBatch(T *values, uint32_t batch_size) {
		uint32_t values_read = 0;

		while (values_read < batch_size) {
			if (repeat_count_ > 0) {
				int repeat_batch = std::min(batch_size - values_read, static_cast<uint32_t>(repeat_count_));
				std::fill(values + values_read, values + values_read + repeat_batch, static_cast<T>(current_value_));
				repeat_count_ -= repeat_batch;
				values_read += repeat_batch;
			} else if (literal_count_ > 0) {
				uint32_t literal_batch = std::min(batch_size - values_read, static_cast<uint32_t>(literal_count_));
				uint32_t actual_read = BitUnpack<T>(values + values_read, literal_batch);
				if (literal_batch != actual_read) {
					throw runtime_error("Did not find enough values");
				}
				literal_count_ -= literal_batch;
				values_read += literal_batch;
			} else {
				if (!NextCounts<T>()) {
					if (values_read != batch_size) {
						throw runtime_error("RLE decode did not find enough values");
					}
					return;
				}
			}
		}
		if (values_read != batch_size) {
			throw runtime_error("RLE decode did not find enough values");
		}
	}

private:
	const uint8_t *buffer;

	/// Number of bits needed to encode the value. Must be between 0 and 64.
	int bit_width_;
	uint64_t current_value_;
	uint32_t repeat_count_;
	uint32_t literal_count_;
	uint8_t byte_encoded_len;
	uint32_t max_val;

	// this is slow but whatever, calls are rare
	static uint8_t VarintDecode(const uint8_t *source, uint32_t *result_out) {
		uint32_t result = 0;
		uint8_t shift = 0;
		uint8_t len = 0;
		while (true) {
			auto byte = *source++;
			len++;
			result |= (byte & 127) << shift;
			if ((byte & 128) == 0)
				break;
			shift += 7;
			if (shift > 32) {
				throw runtime_error("Varint-decoding found too large number");
			}
		}
		*result_out = result;
		return len;
	}

	/// Fills literal_count_ and repeat_count_ with next values. Returns false if there
	/// are no more.
	template <typename T> bool NextCounts() {
		// Read the next run's indicator int, it could be a literal or repeated run.
		// The int is encoded as a vlq-encoded value.
		uint32_t indicator_value;

		buffer += VarintDecode(buffer, &indicator_value);

		// lsb indicates if it is a literal run or repeated run
		bool is_literal = indicator_value & 1;
		if (is_literal) {
			literal_count_ = (indicator_value >> 1) * 8;
		} else {
			repeat_count_ = indicator_value >> 1;
			// (ARROW-4018) this is not big-endian compatible, lol
			current_value_ = 0;
			for (auto i = 0; i < byte_encoded_len; i++) {
				current_value_ |= ((uint8_t)*buffer++) << (i * 8);
			}
			// sanity check
			if (repeat_count_ > 0 && current_value_ > max_val) {
				throw runtime_error("Payload value bigger than allowed. Corrupted file?");
			}
		}
		// TODO complain if we run out of buffer
		return true;
	}

	// somewhat optimized implementation that avoids non-alignment

	static const uint32_t BITPACK_MASKS[];
	static const uint8_t BITPACK_DLEN;

	template <typename T> uint32_t BitUnpack(T *dest, uint32_t count) {
		assert(bit_width_ < 32);

		int8_t bitpack_pos = 0;
		auto source = buffer;
		auto mask = BITPACK_MASKS[bit_width_];

		for (uint32_t i = 0; i < count; i++) {
			T val = (*source >> bitpack_pos) & mask;
			bitpack_pos += bit_width_;
			while (bitpack_pos > BITPACK_DLEN) {
				val |= (*++source << (BITPACK_DLEN - (bitpack_pos - bit_width_))) & mask;
				bitpack_pos -= BITPACK_DLEN;
			}
			dest[i] = val;
		}

		buffer += bit_width_ * count / 8;
		return count;
	}
};

const uint32_t RleBpDecoder::BITPACK_MASKS[] = {
    0,       1,       3,        7,        15,       31,        63,        127,       255,        511,       1023,
    2047,    4095,    8191,     16383,    32767,    65535,     131071,    262143,    524287,     1048575,   2097151,
    4194303, 8388607, 16777215, 33554431, 67108863, 134217727, 268435455, 536870911, 1073741823, 2147483647};

const uint8_t RleBpDecoder::BITPACK_DLEN = 8;

// surely they are joking
static constexpr int64_t kJulianToUnixEpochDays = 2440588LL;
static constexpr int64_t kMillisecondsInADay = 86400000LL;
static constexpr int64_t kNanosecondsInADay = kMillisecondsInADay * 1000LL * 1000LL;

static int64_t impala_timestamp_to_nanoseconds(const Int96 &impala_timestamp) {
	int64_t days_since_epoch = impala_timestamp.value[2] - kJulianToUnixEpochDays;
	int64_t nanoseconds = *(reinterpret_cast<const int64_t *>(&(impala_timestamp.value)));
	return days_since_epoch * kNanosecondsInADay + nanoseconds;
}

struct ParquetScanColumnData {
	idx_t chunk_offset;

	idx_t page_offset;
	idx_t page_value_count = 0;

	idx_t dict_size;

	ResizeableBuffer buf;
	ResizeableBuffer decompressed_buf; // only used for compressed files
	ResizeableBuffer dict;
	ResizeableBuffer offset_buf;

	ByteBuffer payload;

	Encoding::type page_encoding;
	// these point into buf or decompressed_buf
	unique_ptr<RleBpDecoder> defined_decoder;
	unique_ptr<RleBpDecoder> dict_decoder;

	unique_ptr<ChunkCollection> string_collection;
};

struct ParquetScanFunctionData : public TableFunctionData {
	int64_t current_group;
	int64_t group_offset;

	ifstream pfile;

	FileMetaData file_meta_data;
	vector<SQLType> sql_types;
	vector<ParquetScanColumnData> column_data;
	bool finished;
};

class ParquetScanFunction : public TableFunction {
public:
	ParquetScanFunction()
	    : TableFunction("parquet_scan", {SQLType::VARCHAR}, parquet_scan_bind, parquet_scan_function, nullptr){};

private:
	static unique_ptr<FunctionData> parquet_scan_bind(ClientContext &context, vector<Value> inputs,
	                                                  vector<SQLType> &return_types, vector<string> &names) {

		auto file_name = inputs[0].GetValue<string>();
		auto res = make_unique<ParquetScanFunctionData>();

		auto &pfile = res->pfile;
		auto &file_meta_data = res->file_meta_data;

		pfile.open(file_name, std::ios::binary);

		ResizeableBuffer buf;
		buf.resize(4);
		memset(buf.ptr, '\0', 4);
		// check for magic bytes at start of file
		pfile.read(buf.ptr, 4);
		if (strncmp(buf.ptr, "PAR1", 4) != 0) {
			throw runtime_error("File not found or missing magic bytes");
		}

		// check for magic bytes at end of file
		pfile.seekg(-4, ios_base::end);
		pfile.read(buf.ptr, 4);
		if (strncmp(buf.ptr, "PAR1", 4) != 0) {
			throw runtime_error("No magic bytes found at end of file");
		}

		// read four-byte footer length from just before the end magic bytes
		pfile.seekg(-8, ios_base::end);
		pfile.read(buf.ptr, 4);
		int32_t footer_len = *(uint32_t *)buf.ptr;
		if (footer_len == 0) {
			throw runtime_error("Footer length can't be 0");
		}

		// read footer into buffer and de-thrift
		buf.resize(footer_len);
		pfile.seekg(-(footer_len + 8), ios_base::end);
		pfile.read(buf.ptr, footer_len);
		if (!pfile) {
			throw runtime_error("Could not read footer");
		}

		thrift_unpack((const uint8_t *)buf.ptr, (uint32_t *)&footer_len, &file_meta_data);

		if (file_meta_data.__isset.encryption_algorithm) {
			throw runtime_error("Encrypted Parquet files are not supported");
		}

		// check if we like this schema
		if (file_meta_data.schema.size() < 2) {
			throw runtime_error("Need at least one column in the file");
		}
		if (file_meta_data.schema[0].num_children != (int32_t)(file_meta_data.schema.size() - 1)) {
			throw runtime_error("Only flat tables are supported (no nesting)");
		}

		// skip the first column its the root and otherwise useless
		for (uint64_t col_idx = 1; col_idx < file_meta_data.schema.size(); col_idx++) {
			auto &s_ele = file_meta_data.schema[col_idx];

			if (!s_ele.__isset.type || s_ele.num_children > 0) {
				throw runtime_error("Only flat tables are supported (no nesting)");
			}
			// if this is REQUIRED, there are no defined levels in file, seems unused
			// if field is REPEATED, no bueno
			if (s_ele.repetition_type != FieldRepetitionType::OPTIONAL) {
				throw runtime_error("Only OPTIONAL fields support");
			}

			names.push_back(s_ele.name);
			SQLType type;
			switch (s_ele.type) {
			case Type::BOOLEAN:
				type = SQLType::BOOLEAN;
				break;
			case Type::INT32:
				type = SQLType::INTEGER;
				break;
			case Type::INT64:
				type = SQLType::BIGINT;
				break;
			case Type::INT96: // always a timestamp?
				type = SQLType::TIMESTAMP;
				break;
			case Type::FLOAT:
				type = SQLType::FLOAT;
				break;
			case Type::DOUBLE:
				type = SQLType::DOUBLE;
				break;
				//			case parquet::format::Type::FIXED_LEN_BYTE_ARRAY: {
				// TODO some decimals yuck
			case Type::BYTE_ARRAY:
				type = SQLType::VARCHAR;
				break;

			default:
				throw NotImplementedException("Invalid type");
				break;
			}

			return_types.push_back(type);
			res->sql_types.push_back(type);
		}
		res->group_offset = 0;
		res->current_group = -1;
		res->column_data.resize(return_types.size());
		res->finished = false;
		return move(res);
	}

	template <class T>
	static void _fill_from_dict(ParquetScanColumnData &col_data, const char *defined_ptr, idx_t count, Vector &target,
	                            idx_t target_offset) {
		for (idx_t i = 0; i < count; i++) {
			if (defined_ptr[i]) {
				auto offset = col_data.offset_buf.read<int32_t>();
				((T *)FlatVector::GetData(target))[i + target_offset] = ((const T *)col_data.dict.ptr)[offset];
			} else {
				FlatVector::SetNull(target, i + target_offset, true);
			}
		}
	}

	// TODO make payload a buffer object
	template <class T>
	static void _fill_from_plain(ParquetScanColumnData &col_data, const char *defined_ptr, idx_t count, Vector &target,
	                             idx_t target_offset) {
		for (idx_t i = 0; i < count; i++) {
			if (defined_ptr[i]) {
				((T *)FlatVector::GetData(target))[i + target_offset] = col_data.payload.read<T>();
			} else {
				FlatVector::SetNull(target, i + target_offset, true);
			}
		}
	}

	static bool _prepare_page_buffers(ParquetScanFunctionData &data, idx_t col_idx) {
		auto &col_data = data.column_data[col_idx];
		auto &chunk = data.file_meta_data.row_groups[data.current_group].columns[col_idx];

		// clean up a bit to avoid surprises
		col_data.payload.ptr = nullptr;
		col_data.payload.len = 0;

		auto page_header_len = col_data.buf.len;
		if (page_header_len < 1) {
			throw runtime_error("Ran out of bytes to read header from. File corrupt?");
		}
		PageHeader page_hdr;
		thrift_unpack((const uint8_t *)col_data.buf.ptr + col_data.chunk_offset, (uint32_t *)&page_header_len,
		              &page_hdr);

		// the payload starts behind the header, obvsl.
		col_data.buf.inc(page_header_len);

		col_data.payload.len = page_hdr.uncompressed_page_size;

		// handle compression, in the end we expect a pointer to uncompressed parquet data in payload_ptr
		switch (chunk.meta_data.codec) {
		case CompressionCodec::UNCOMPRESSED:
			col_data.payload.ptr = col_data.buf.ptr;
			break;

		case CompressionCodec::SNAPPY: {
			size_t decompressed_size;

			snappy::GetUncompressedLength(col_data.buf.ptr, page_hdr.compressed_page_size, &decompressed_size);
			col_data.decompressed_buf.resize(decompressed_size);

			auto res =
			    snappy::RawUncompress(col_data.buf.ptr, page_hdr.compressed_page_size, col_data.decompressed_buf.ptr);
			if (!res) {
				throw runtime_error("Decompression failure");
			}
			col_data.payload.ptr = col_data.decompressed_buf.ptr;
			break;
		}
		default:
			throw runtime_error("Unsupported compression codec. Try uncompressed or snappy");
		}
		col_data.buf.inc(page_hdr.compressed_page_size);

		// handle page contents

		switch (page_hdr.type) {
		case PageType::DICTIONARY_PAGE: {
			// fill the dictionary vector

			if (page_hdr.__isset.data_page_header || !page_hdr.__isset.dictionary_page_header) {
				throw runtime_error("Dictionary page header mismatch");
			}

			// make sure we like the encoding
			switch (page_hdr.dictionary_page_header.encoding) {
			case Encoding::PLAIN:
			case Encoding::PLAIN_DICTIONARY: // deprecated
				break;

			default:
				throw runtime_error("Dictionary page has unsupported/invalid encoding");
			}

			col_data.dict_size = page_hdr.dictionary_page_header.num_values;
			auto dict_byte_size = col_data.dict_size * GetTypeIdSize(GetInternalType(data.sql_types[col_idx]));

			col_data.dict.resize(dict_byte_size);

			switch (data.sql_types[col_idx].id) {
			case SQLTypeId::BOOLEAN:
			case SQLTypeId::INTEGER:
			case SQLTypeId::BIGINT:
			case SQLTypeId::FLOAT:
			case SQLTypeId::DOUBLE:
				col_data.payload.available(dict_byte_size);
				// TODO this copy could be avoided if we use different buffers for dicts
				col_data.payload.copy_to(col_data.dict.ptr, dict_byte_size);
				break;
			case SQLTypeId::TIMESTAMP:
				col_data.payload.available(dict_byte_size);
				// immediately convert timestamps to duckdb format, potentially fewer conversions
				for (idx_t dict_index = 0; dict_index < col_data.dict_size; dict_index++) {
					auto impala_ns = impala_timestamp_to_nanoseconds(((Int96 *)col_data.payload.ptr)[dict_index]);

					auto ms = impala_ns / 1000000; // nanoseconds
					auto ms_per_day = (int64_t)60 * 60 * 24 * 1000;
					date_t date = Date::EpochToDate(ms / 1000);
					dtime_t time = (dtime_t)(ms % ms_per_day);
					((timestamp_t *)col_data.dict.ptr)[dict_index] = Timestamp::FromDatetime(date, time);
				}

				break;
			case SQLTypeId::VARCHAR: {
				// strings we directly fill a string heap that we can use for the vectors later
				col_data.string_collection = make_unique<ChunkCollection>();

				// we hand-roll a chunk collection to avoid copying strings
				auto append_chunk = make_unique<DataChunk>();
				vector<TypeId> types = {TypeId::VARCHAR};
				col_data.string_collection->types = types;
				append_chunk->Initialize(types);

				for (idx_t dict_index = 0; dict_index < col_data.dict_size; dict_index++) {
					uint32_t str_len = col_data.payload.read<uint32_t>();
					col_data.payload.available(str_len);

					if (append_chunk->size() == STANDARD_VECTOR_SIZE) {
						col_data.string_collection->count += append_chunk->size();
						col_data.string_collection->chunks.push_back(move(append_chunk));
						append_chunk = make_unique<DataChunk>();
						append_chunk->Initialize(types);
					}

					auto utf_type = Utf8Proc::Analyze(col_data.payload.ptr, str_len);
					switch (utf_type) {
					case UnicodeType::ASCII:
						FlatVector::GetData<string_t>(append_chunk->data[0])[append_chunk->size()] =
						    StringVector::AddString(append_chunk->data[0], col_data.payload.ptr, str_len);
						break;
					case UnicodeType::UNICODE:
						// this regrettably copies to normalize
						FlatVector::GetData<string_t>(append_chunk->data[0])[append_chunk->size()] =
						    StringVector::AddString(append_chunk->data[0],
						                            Utf8Proc::Normalize(string(col_data.payload.ptr, str_len)));

						break;
					case UnicodeType::INVALID:
						throw runtime_error("invalid string encoding");
					}

					append_chunk->SetCardinality(append_chunk->size() + 1);
					col_data.payload.inc(str_len);
				}
				// FLUSH last chunk!
				if (append_chunk->size() > 0) {
					col_data.string_collection->count += append_chunk->size();
					col_data.string_collection->chunks.push_back(move(append_chunk));
				}
				col_data.string_collection->Verify();
			} break;
			default:
				throw runtime_error(SQLTypeToString(data.sql_types[col_idx]));
			}
			// important, move to next page which should be a data page
			return false;
		}
		case PageType::DATA_PAGE: {
			if (!page_hdr.__isset.data_page_header || page_hdr.__isset.dictionary_page_header) {
				throw runtime_error("Data page header mismatch");
			}

			if (page_hdr.__isset.data_page_header_v2) {
				throw runtime_error("v2 data page format is not supported");
			}

			col_data.page_value_count = page_hdr.data_page_header.num_values;
			col_data.page_encoding = page_hdr.data_page_header.encoding;

			// we have to first decode the define levels
			switch (page_hdr.data_page_header.definition_level_encoding) {
			case Encoding::RLE: {
				// read length of define payload, always
				uint32_t def_length = col_data.payload.read<uint32_t>();
				col_data.payload.available(def_length);
				col_data.dict_decoder = make_unique<RleBpDecoder>((const uint8_t *)col_data.payload.ptr, def_length, 1);
				col_data.payload.inc(def_length);
			} break;
			default:
				throw runtime_error("Definition levels have unsupported/invalid encoding");
			}

			switch (page_hdr.data_page_header.encoding) {
			case Encoding::RLE_DICTIONARY:
			case Encoding::PLAIN_DICTIONARY: {
				auto enc_length = col_data.payload.read<uint8_t>();
				col_data.offset_buf.resize(col_data.page_value_count * sizeof(uint32_t));
				RleBpDecoder dict_decoder((const uint8_t *)col_data.payload.ptr, col_data.payload.len, enc_length);
				dict_decoder.GetBatch<uint32_t>((unsigned int *)col_data.offset_buf.ptr, col_data.page_value_count);

				break;
			}
			case Encoding::PLAIN:
				// nothing here, see below
				break;

			default:
				throw runtime_error("Data page has unsupported/invalid encoding");
			}

			break;
		}
		case PageType::DATA_PAGE_V2:
			throw runtime_error("v2 data page format is not supported");

		default:
			break; // ignore INDEX page type and any other custom extensions
		}
		return true;
	}

	static void _prepare_chunk_buffer(ParquetScanFunctionData &data, idx_t col_idx) {
		auto &chunk = data.file_meta_data.row_groups[data.current_group].columns[col_idx];
		if (chunk.__isset.file_path) {
			throw runtime_error("Only inlined data files are supported (no references)");
		}

		if (chunk.meta_data.path_in_schema.size() != 1) {
			throw runtime_error("Only flat tables are supported (no nesting)");
		}

		// ugh. sometimes there is an extra offset for the dict. sometimes it's wrong.
		auto chunk_start = chunk.meta_data.data_page_offset;
		if (chunk.meta_data.__isset.dictionary_page_offset && chunk.meta_data.dictionary_page_offset >= 4) {
			// this assumes the data pages follow the dict pages directly.
			chunk_start = chunk.meta_data.dictionary_page_offset;
		}
		auto chunk_len = chunk.meta_data.total_compressed_size;

		// read entire chunk into RAM
		data.pfile.seekg(chunk_start);
		data.column_data[col_idx].buf.resize(chunk_len);
		data.pfile.read(data.column_data[col_idx].buf.ptr, chunk_len);
		if (!data.pfile) {
			throw runtime_error("Could not read chunk. File corrupt?");
		}
	}

	static void parquet_scan_function(ClientContext &context, vector<Value> &input, DataChunk &output,
	                                  FunctionData *dataptr) {
		auto &data = *((ParquetScanFunctionData *)dataptr);

		if (data.finished) {
			return;
		}

		// see if we have to switch to the next row group in the parquet file
		if (data.current_group < 0 ||
		    data.group_offset >= data.file_meta_data.row_groups[data.current_group].num_rows) {

			data.current_group++;
			data.group_offset = 0;

			if (data.current_group == data.file_meta_data.row_groups.size()) {
				data.finished = true;
				return;
			}

			for (idx_t out_col_idx = 0; out_col_idx < output.column_count(); out_col_idx++) {
				auto file_col_idx = data.column_ids[out_col_idx];

				// this is a special case where we are not interested in the actual contents of the file
				if (file_col_idx == COLUMN_IDENTIFIER_ROW_ID) {
					continue;
				}

				_prepare_chunk_buffer(data, file_col_idx);
				// trigger the reading of a new page below
				data.column_data[file_col_idx].page_value_count = 0;
			}
		}

		auto current_group = data.file_meta_data.row_groups[data.current_group];
		output.SetCardinality(std::min((int64_t)STANDARD_VECTOR_SIZE, current_group.num_rows - data.group_offset));
		assert(output.size() > 0);

		// TODO assert that we are reading a page sequentially and not omit anything

		for (idx_t out_col_idx = 0; out_col_idx < output.column_count(); out_col_idx++) {
			auto file_col_idx = data.column_ids[out_col_idx];
			if (file_col_idx == COLUMN_IDENTIFIER_ROW_ID) {
				Value constant_42 = Value::BIGINT(42);
				output.data[out_col_idx].Reference(constant_42);
				continue;
			}

			auto &col_data = data.column_data[file_col_idx];

			// we might need to read multiple pages to fill the data chunk
			idx_t output_offset = 0;
			while (output_offset < output.size()) {
				// do this unpack business only if we run out of stuff from the current page
				if (col_data.page_offset >= col_data.page_value_count) {

					// read dictionaries and data page headers so that we are ready to go for scan
					if (!_prepare_page_buffers(data, file_col_idx)) {
						continue;
					}
					col_data.page_offset = 0;
				}

				auto current_batch_size =
				    std::min(col_data.page_value_count - col_data.page_offset, output.size() - output_offset);

				assert(current_batch_size > 0);

				ResizeableBuffer defined;
				defined.resize(current_batch_size * sizeof(uint32_t));
				col_data.dict_decoder->GetBatch(defined.ptr, current_batch_size);

				switch (col_data.page_encoding) {
				case Encoding::RLE_DICTIONARY:
				case Encoding::PLAIN_DICTIONARY: {

					// TODO ensure we had seen a dict page IN THIS CHUNK before getting here

					switch (data.sql_types[file_col_idx].id) {
					case SQLTypeId::BOOLEAN:
						_fill_from_dict<bool>(col_data, defined.ptr, current_batch_size, output.data[out_col_idx],
						                      output_offset);
						break;
					case SQLTypeId::INTEGER:
						_fill_from_dict<int32_t>(col_data, defined.ptr, current_batch_size, output.data[out_col_idx],
						                         output_offset);
						break;
					case SQLTypeId::BIGINT:
						_fill_from_dict<int64_t>(col_data, defined.ptr, current_batch_size, output.data[out_col_idx],
						                         output_offset);
						break;
					case SQLTypeId::FLOAT:
						_fill_from_dict<float>(col_data, defined.ptr, current_batch_size, output.data[out_col_idx],
						                       output_offset);
						break;
					case SQLTypeId::DOUBLE:
						_fill_from_dict<double>(col_data, defined.ptr, current_batch_size, output.data[out_col_idx],
						                        output_offset);
						break;
					case SQLTypeId::TIMESTAMP:
						_fill_from_dict<timestamp_t>(col_data, defined.ptr, current_batch_size,
						                             output.data[out_col_idx], output_offset);
						break;
					case SQLTypeId::VARCHAR: {
						if (!col_data.string_collection) {
							throw runtime_error("Did not see a dictionary for strings. Corrupt file?");
						}

						// the strings can be anywhere in the collection so just reference it all
						for (auto &chunk : col_data.string_collection->chunks) {
							StringVector::AddHeapReference(output.data[out_col_idx], chunk->data[0]);
						}

						auto out_data_ptr = FlatVector::GetData<string_t>(output.data[out_col_idx]);
						for (idx_t i = 0; i < current_batch_size; i++) {
							if (defined.ptr[i]) {
								auto offset = col_data.offset_buf.read<int32_t>();
								if (offset >= col_data.string_collection->count) {
									throw runtime_error("string dictionary offset out of bounds");
								}
								auto &chunk = col_data.string_collection->chunks[offset / STANDARD_VECTOR_SIZE];
								auto &vec = chunk->data[0];

								out_data_ptr[i + output_offset] =
								    FlatVector::GetData<string_t>(vec)[offset % STANDARD_VECTOR_SIZE];
							} else {
								FlatVector::SetNull(output.data[out_col_idx], i + output_offset, true);
							}
						}
					} break;
					default:
						throw runtime_error(SQLTypeToString(data.sql_types[file_col_idx]));
					}

					break;
				}
				case Encoding::PLAIN:
					assert(col_data.payload.ptr);
					switch (data.sql_types[file_col_idx].id) {
					case SQLTypeId::BOOLEAN: {
						// bit packed this
						auto target_ptr = FlatVector::GetData<bool>(output.data[out_col_idx]);
						int byte_pos = 0;
						for (int32_t i = 0; i < current_batch_size; i++) {
							if (!defined.ptr[i]) {
								FlatVector::SetNull(output.data[out_col_idx], i + output_offset, true);
								continue;
							}
							col_data.payload.available(1);
							target_ptr[i + output_offset] = (*col_data.payload.ptr >> byte_pos) & 1;
							byte_pos++;
							if (byte_pos == 8) {
								byte_pos = 0;
								col_data.payload.inc(1);
							}
						}
						break;
					}
					case SQLTypeId::INTEGER:
						_fill_from_plain<int32_t>(col_data, defined.ptr, current_batch_size, output.data[out_col_idx],
						                          output_offset);
						break;
					case SQLTypeId::BIGINT:
						_fill_from_plain<int64_t>(col_data, defined.ptr, current_batch_size, output.data[out_col_idx],
						                          output_offset);
						break;
					case SQLTypeId::FLOAT:
						_fill_from_plain<float>(col_data, defined.ptr, current_batch_size, output.data[out_col_idx],
						                        output_offset);
						break;
					case SQLTypeId::DOUBLE:
						_fill_from_plain<double>(col_data, defined.ptr, current_batch_size, output.data[out_col_idx],
						                         output_offset);
						break;
					case SQLTypeId::VARCHAR: {
						for (idx_t i = 0; i < current_batch_size; i++) {
							if (defined.ptr[i]) {
								uint32_t str_len = col_data.payload.read<uint32_t>();
								col_data.payload.available(str_len);
								FlatVector::GetData<string_t>(output.data[out_col_idx])[i + output_offset] =
								    StringVector::AddString(output.data[out_col_idx], col_data.payload.ptr, str_len);
								col_data.payload.inc(str_len);
							} else {
								FlatVector::SetNull(output.data[out_col_idx], i + output_offset, true);
							}
						}
						break;
					}
					default:
						throw runtime_error(SQLTypeToString(data.sql_types[file_col_idx]));
					}

					break;

				default:
					throw runtime_error("Data page has unsupported/invalid encoding");
				}

				output_offset += current_batch_size;
				col_data.page_offset += current_batch_size;
			}
		}
		data.group_offset += output.size();
	}
};

void ParquetExtension::Load(DuckDB &db) {
	ParquetScanFunction scan_fun;
	CreateTableFunctionInfo info(scan_fun, true);

	Connection conn(db);
	conn.context->transaction.BeginTransaction();
	conn.context->catalog.CreateTableFunction(*conn.context, &info);
	info.name = "read_parquet"; // ok we will have this alias
	conn.context->catalog.CreateTableFunction(*conn.context, &info);

	conn.context->transaction.Commit();
}
