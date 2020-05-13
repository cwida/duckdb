#include "duckdb_miniparquet.hpp"

#include <string>
#include <vector>
#include <bitset>
#include <fstream>
#include <cstring>
#include <iostream>

//#include "miniparquet.h"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "utf8proc_wrapper.hpp"
#include "duckdb.hpp"

#include "parquet/parquet_types.h"
#include "protocol/TCompactProtocol.h"
#include "transport/TBufferTransports.h"

#include "snappy/snappy.h"

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

	void resize(uint64_t new_size) {
		if (new_size > len) {
			auto new_holder = std::unique_ptr<char[]>(new char[new_size]);
			holder = move(new_holder);
			ptr = holder.get();
		}
		len = new_size;
	}

	void inc(uint64_t increment) {
		len -= increment;
		ptr += increment;
	}

private:
	std::unique_ptr<char[]> holder = nullptr;
};

static TCompactProtocolFactoryT<TMemoryBuffer> tproto_factory;

template<class T>
static void thrift_unpack(const uint8_t *buf, uint32_t *len,
		T *deserialized_msg) {
	shared_ptr<TMemoryBuffer> tmem_transport(
			new TMemoryBuffer(const_cast<uint8_t*>(buf), *len));
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
	RleBpDecoder(const uint8_t *buffer, uint32_t buffer_len, uint32_t bit_width) :
			buffer(buffer), bit_width_(bit_width), current_value_(0), repeat_count_(
					0), literal_count_(0) {

		if (bit_width >= 64) {
			throw runtime_error("Decode bit width too large");
		}
		byte_encoded_len = ((bit_width_ + 7) / 8);
		max_val = (1 << bit_width_) - 1;

	}

	/// Gets a batch of values.  Returns the number of decoded elements.
	template<typename T>
	inline int GetBatch(T *values, uint32_t batch_size) {
		uint32_t values_read = 0;

		while (values_read < batch_size) {
			if (repeat_count_ > 0) {
				int repeat_batch = std::min(batch_size - values_read,
						static_cast<uint32_t>(repeat_count_));
				std::fill(values + values_read,
						values + values_read + repeat_batch,
						static_cast<T>(current_value_));
				repeat_count_ -= repeat_batch;
				values_read += repeat_batch;
			} else if (literal_count_ > 0) {
				uint32_t literal_batch = std::min(batch_size - values_read,
						static_cast<uint32_t>(literal_count_));
				uint32_t actual_read = BitUnpack<T>(values + values_read,
						literal_batch);
				if (literal_batch != actual_read) {
					throw runtime_error("Did not find enough values");
				}
				literal_count_ -= literal_batch;
				values_read += literal_batch;
			} else {
				if (!NextCounts<T>())
					return values_read;
			}
		}
		return values_read;
	}

	template<typename T>
	inline int GetBatchSpaced(uint32_t batch_size, uint32_t null_count,
			const uint8_t *defined, T *out) {
		//  DCHECK_GE(bit_width_, 0);

		// TODO call GetBatch instead if null_count == 0

		uint32_t values_read = 0;
		uint32_t remaining_nulls = null_count;

		uint32_t d_off = 0; // defined_offset

		while (values_read < batch_size) {
			bool is_valid = defined[d_off++];

			if (is_valid) {
				if ((repeat_count_ == 0) && (literal_count_ == 0)) {
					if (!NextCounts<T>())
						return values_read;
				}
				if (repeat_count_ > 0) {
					// The current index is already valid, we don't need to check that again
					uint32_t repeat_batch = 1;
					repeat_count_--;

					while (repeat_count_ > 0
							&& (values_read + repeat_batch) < batch_size) {
						if (defined[d_off]) {
							repeat_count_--;
						} else {
							remaining_nulls--;
						}
						repeat_batch++;

						d_off++;
					}
					std::fill(out, out + repeat_batch,
							static_cast<T>(current_value_));
					out += repeat_batch;
					values_read += repeat_batch;
				} else if (literal_count_ > 0) {
					uint32_t literal_batch = std::min(
							batch_size - values_read - remaining_nulls,
							static_cast<uint32_t>(literal_count_));

					// Decode the literals
					constexpr uint32_t kBufferSize = 1024;
					T indices[kBufferSize];
					literal_batch = std::min(literal_batch, kBufferSize);
					auto actual_read = BitUnpack<T>(indices, literal_batch);

					if (actual_read != literal_batch) {
						throw runtime_error("Did not find enough values");

					}

					uint32_t skipped = 0;
					uint32_t literals_read = 1;
					*out++ = indices[0];

					// Read the first bitset to the end
					while (literals_read < literal_batch) {
						if (defined[d_off]) {
							*out = indices[literals_read];
							literals_read++;
						} else {
							skipped++;
						}
						++out;
						d_off++;
					}
					literal_count_ -= literal_batch;
					values_read += literal_batch + skipped;
					remaining_nulls -= skipped;
				}
			} else {
				++out;
				values_read++;
				remaining_nulls--;
			}
		}

		return values_read;
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
	template<typename T>
	bool NextCounts() {
		// Read the next run's indicator int, it could be a literal or repeated run.
		// The int is encoded as a vlq-encoded value.
		uint32_t indicator_value;

		// TODO check in varint decode if we have enough buffer left
		buffer += VarintDecode(buffer, &indicator_value);

		// TODO check a bunch of lengths here against the standard

		// lsb indicates if it is a literal run or repeated run
		bool is_literal = indicator_value & 1;
		if (is_literal) {
			literal_count_ = (indicator_value >> 1) * 8;
		} else {
			repeat_count_ = indicator_value >> 1;
			// (ARROW-4018) this is not big-endian compatible, lol
			current_value_ = 0;
			for (auto i = 0; i < byte_encoded_len; i++) {
				current_value_ |= ((uint8_t) *buffer++) << (i * 8);
			}
			// sanity check
			if (repeat_count_ > 0 && current_value_ > max_val) {
				throw runtime_error(
						"Payload value bigger than allowed. Corrupted file?");
			}
		}
		// TODO complain if we run out of buffer
		return true;
	}

	// somewhat optimized implementation that avoids non-alignment

	static const uint32_t BITPACK_MASKS[];
	static const uint8_t BITPACK_DLEN;

	template<typename T>
	uint32_t BitUnpack(T *dest, uint32_t count) {
		assert(bit_width_ < 32);

		int8_t bitpack_pos = 0;
		auto source = buffer;
		auto mask = BITPACK_MASKS[bit_width_];

		for (uint32_t i = 0; i < count; i++) {
			T val = (*source >> bitpack_pos) & mask;
			bitpack_pos += bit_width_;
			while (bitpack_pos > BITPACK_DLEN) {
				val |=
						(*++source
								<< (BITPACK_DLEN - (bitpack_pos - bit_width_)))
								& mask;
				bitpack_pos -= BITPACK_DLEN;
			}
			dest[i] = val;
		}

		buffer += bit_width_ * count / 8;
		return count;
	}

};

const uint32_t RleBpDecoder::BITPACK_MASKS[] = { 0, 1, 3, 7, 15, 31, 63, 127,
		255, 511, 1023, 2047, 4095, 8191, 16383, 32767, 65535, 131071, 262143,
		524287, 1048575, 2097151, 4194303, 8388607, 16777215, 33554431,
		67108863, 134217727, 268435455, 536870911, 1073741823, 2147483647 };

const uint8_t RleBpDecoder::BITPACK_DLEN = 8;

// surely they are joking
static constexpr int64_t kJulianToUnixEpochDays = 2440588LL;
static constexpr int64_t kMillisecondsInADay = 86400000LL;
static constexpr int64_t kNanosecondsInADay = kMillisecondsInADay * 1000LL
		* 1000LL;

static int64_t impala_timestamp_to_nanoseconds(const Int96 &impala_timestamp) {
	int64_t days_since_epoch = impala_timestamp.value[2]
			- kJulianToUnixEpochDays;
	int64_t nanoseconds =
			*(reinterpret_cast<const int64_t*>(&(impala_timestamp.value)));
	return days_since_epoch * kNanosecondsInADay + nanoseconds;
}

struct ParquetScanColumnData {
	idx_t chunk_offset;

	idx_t page_offset;
	idx_t page_value_count = 0;

	idx_t dict_size;

	ByteBuffer buf;
	ByteBuffer decompressed_buf; // only used for compressed files

	ByteBuffer dict;

	Encoding::type page_encoding;
	// these point into buf or decompressed_buf
//	unique_ptr<RleBpDecoder> defined_decoder;
//	unique_ptr<RleBpDecoder> dict_decoder;
	ByteBuffer defined_buf;
	ByteBuffer offset_buf;
	const char *payload_ptr; // for plain pages

	unique_ptr<ChunkCollection> string_collection;
};

struct ParquetScanFunctionData: public TableFunctionData {
	int64_t current_group;
	int64_t group_offset;

	ifstream pfile;

	FileMetaData file_meta_data;
	vector<SQLType> sql_types;
	vector<ParquetScanColumnData> column_data;
	bool finished;
};

struct ParquetScanFunction: public TableFunction {
	ParquetScanFunction() :
			TableFunction("parquet_scan", { SQLType::VARCHAR },
					parquet_scan_bind, parquet_scan_function, nullptr) {
	}
	;

	static unique_ptr<FunctionData> parquet_scan_bind(ClientContext &context,
			vector<Value> inputs, vector<SQLType> &return_types,
			vector<string> &names) {

		auto file_name = inputs[0].GetValue<string>();
		auto res = make_unique<ParquetScanFunctionData>();

		auto &pfile = res->pfile;
		auto &file_meta_data = res->file_meta_data;

		ByteBuffer buf;
		pfile.open(file_name, std::ios::binary);

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
		int32_t footer_len = *(uint32_t*) buf.ptr;
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

		thrift_unpack((const uint8_t*) buf.ptr, (uint32_t*) &footer_len,
				&file_meta_data);

		//	file_meta_data.printTo(cerr);
		//	cerr << "\n";

		if (file_meta_data.__isset.encryption_algorithm) {
			throw runtime_error("Encrypted Parquet files are not supported");
		}

		// check if we like this schema
		if (file_meta_data.schema.size() < 2) {
			throw runtime_error("Need at least one column in the file");
		}
		if (file_meta_data.schema[0].num_children
				!= (int32_t) (file_meta_data.schema.size() - 1)) {
			throw runtime_error("Only flat tables are supported (no nesting)");
		}

		// TODO assert that the first col is root

		// skip the first column its the root and otherwise useless
		for (uint64_t col_idx = 1; col_idx < file_meta_data.schema.size();
				col_idx++) {
			auto &s_ele = file_meta_data.schema[col_idx];

			if (!s_ele.__isset.type || s_ele.num_children > 0) {
				throw runtime_error(
						"Only flat tables are supported (no nesting)");
			}
			// TODO if this is REQUIRED, there are no defined levels in file, support this
			// if field is REPEATED, no bueno
			if (s_ele.repetition_type != FieldRepetitionType::OPTIONAL) {
				throw runtime_error("Only OPTIONAL fields support for now");
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

	template<class T>
	static void fill_from_dict(const char *dict_ptr,
			const uint32_t *offsets_ptr, idx_t count, Vector &target,
			idx_t target_offset) {
		for (idx_t i = 0; i < count; i++) {
			((T*) FlatVector::GetData(target))[i + target_offset] =
					((const T*) dict_ptr)[offsets_ptr[i]];
		}

	}

	static bool prepare_buffers(ParquetScanFunctionData &data, idx_t col_idx) {
		auto &col_data = data.column_data[col_idx];
		auto &chunk =
				data.file_meta_data.row_groups[data.current_group].columns[col_idx];

		// clean up a bit to avoid surprises
		col_data.payload_ptr = nullptr;

		auto page_header_len = col_data.buf.len;
		// TODO this should not be 0

		PageHeader page_hdr;
		thrift_unpack((const uint8_t*) col_data.buf.ptr + col_data.chunk_offset,
				(uint32_t*) &page_header_len, &page_hdr);

		// the payload starts behind the header, obvsl.
		col_data.buf.inc(page_header_len);

//					page_hdr.printTo(cerr);
//					cerr << "\n";

		const char *payload_ptr = nullptr;

		// handle compression, in the end we expect a pointer to uncompressed parquet data in payload_ptr
		switch (chunk.meta_data.codec) {
		case CompressionCodec::UNCOMPRESSED:
			payload_ptr = col_data.buf.ptr;
			break;

		case CompressionCodec::SNAPPY: {
			size_t decompressed_size;

			snappy::GetUncompressedLength(col_data.buf.ptr,
					page_hdr.compressed_page_size, &decompressed_size);
			col_data.decompressed_buf.resize(decompressed_size);

			auto res = snappy::RawUncompress(col_data.buf.ptr,
					page_hdr.compressed_page_size,
					col_data.decompressed_buf.ptr);
			if (!res) {
				throw runtime_error("Decompression failure");
			}
			payload_ptr = col_data.decompressed_buf.ptr;
			break;
		}
		default:
			throw runtime_error(
					"Unsupported compression codec. Try uncompressed or snappy");
		}
		assert(payload_ptr);
		col_data.buf.inc(page_hdr.compressed_page_size);

		// handle page contents

		switch (page_hdr.type) {
		case PageType::DICTIONARY_PAGE: {
			// fill the dictionary vector

			if (page_hdr.__isset.data_page_header
					|| !page_hdr.__isset.dictionary_page_header) {
				throw runtime_error("Dictionary page header mismatch");
			}

			// make sure we like the encoding
			switch (page_hdr.dictionary_page_header.encoding) {
			case Encoding::PLAIN:
			case Encoding::PLAIN_DICTIONARY: // deprecated
				break;

			default:
				throw runtime_error(
						"Dictionary page has unsupported/invalid encoding");
			}

			col_data.dict_size = page_hdr.dictionary_page_header.num_values;
			auto dict_byte_size = col_data.dict_size
					* GetTypeIdSize(GetInternalType(data.sql_types[col_idx]));
			col_data.dict.resize(dict_byte_size);

			col_data.string_collection.reset();

			switch (data.sql_types[col_idx].id) {
			case SQLTypeId::BOOLEAN:
			case SQLTypeId::INTEGER:
			case SQLTypeId::BIGINT:
			case SQLTypeId::FLOAT:
			case SQLTypeId::DOUBLE:
				memcpy(col_data.dict.ptr, payload_ptr, dict_byte_size);
				break;
			case SQLTypeId::TIMESTAMP:
				// immediately convert timestamps to duckdb format, potentially fewer conversions
				for (idx_t dict_index = 0; dict_index < col_data.dict_size;
						dict_index++) {
					auto impala_ns = impala_timestamp_to_nanoseconds(
							((Int96*) payload_ptr)[dict_index]);

					auto ms = impala_ns / 1000000; // nanoseconds
					auto ms_per_day = (int64_t) 60 * 60 * 24 * 1000;
					date_t date = Date::EpochToDate(ms / 1000);
					dtime_t time = (dtime_t) (ms % ms_per_day);
					((timestamp_t*) col_data.dict.ptr)[dict_index] =
							Timestamp::FromDatetime(date, time);
				}

				break;
			case SQLTypeId::VARCHAR: {
				// strings we directly fill a string heap that we can use for the vectors later

				col_data.string_collection = make_unique<ChunkCollection>();

				// below we hand-roll a chunk collection to avoid copying strings
				auto append_chunk = make_unique<DataChunk>();
				vector<TypeId> types = { TypeId::VARCHAR };
				col_data.string_collection->types = types;
				append_chunk->Initialize(types);

				for (idx_t dict_index = 0; dict_index < col_data.dict_size;
						dict_index++) {
					uint32_t str_len = *((uint32_t*) payload_ptr);
					payload_ptr += sizeof(str_len);
					// TODO check for overflow

					if (append_chunk->size() == STANDARD_VECTOR_SIZE) {
						// TODO make this an AppendAligned method or so
						col_data.string_collection->count +=
								append_chunk->size();
						col_data.string_collection->chunks.push_back(
								move(append_chunk));
						append_chunk = make_unique<DataChunk>();
						append_chunk->Initialize(types);
					}

					auto utf_type = Utf8Proc::Analyze(payload_ptr, str_len);
					switch (utf_type) {
					case UnicodeType::ASCII:
						FlatVector::GetData<string_t>(append_chunk->data[0])[append_chunk->size()] =
								StringVector::AddString(append_chunk->data[0],
										payload_ptr, str_len);
						break;
					case UnicodeType::UNICODE:
						// this regrettably copies to normalize
						FlatVector::GetData<string_t>(append_chunk->data[0])[append_chunk->size()] =
								StringVector::AddString(append_chunk->data[0],
										Utf8Proc::Normalize(
												string(payload_ptr, str_len)));

						break;
					case UnicodeType::INVALID:
						assert(0); // FIXME throw exception
					}

					append_chunk->SetCardinality(append_chunk->size() + 1);
					payload_ptr += str_len;
				}
				// FLUSH last chunk!
				col_data.string_collection->count += append_chunk->size();
				col_data.string_collection->chunks.push_back(
						move(append_chunk));

				col_data.string_collection->Verify();
			}
				break;
			default:
				throw runtime_error(SQLTypeToString(data.sql_types[col_idx]));
			}
			// important, move to next page which should be a data page
			return false;
		}
		case PageType::DATA_PAGE: {
			if (!page_hdr.__isset.data_page_header
					|| page_hdr.__isset.dictionary_page_header) {
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
				// TODO why is this not just a load?
				uint32_t def_length;
				memcpy(&def_length, payload_ptr, sizeof(def_length));
				payload_ptr += sizeof(def_length);

				col_data.defined_buf.resize(col_data.page_value_count);
				RleBpDecoder define_decoder(
						(const uint8_t*) payload_ptr, def_length, 1);

				// TODO stream decode, but need to respect byte boundaries with this
				auto n_decode = define_decoder.GetBatch<uint8_t>((unsigned char *)col_data.defined_buf.ptr, col_data.page_value_count);
				assert(n_decode == col_data.page_value_count);

				payload_ptr += def_length;
			}
				break;
			default:
				throw runtime_error(
						"Definition levels have unsupported/invalid encoding");
			}

			switch (page_hdr.data_page_header.encoding) {
			case Encoding::RLE_DICTIONARY:
			case Encoding::PLAIN_DICTIONARY: {		// deprecated

				// TODO need to set this page size correctly too
				auto enc_length = *((uint8_t*) payload_ptr);
				//printf("col_idx=%llu\tenc_length = %d\n", col_idx, enc_length);
				payload_ptr += sizeof(uint8_t);

				col_data.offset_buf.resize(col_data.page_value_count * 4);
				RleBpDecoder dict_decoder(
						(const uint8_t*) payload_ptr,
						page_hdr.uncompressed_page_size, enc_length);
				// TODO fix for NULLs with GetBatchSpaced
				auto n_decode = dict_decoder.GetBatch<uint32_t>((unsigned int *)col_data.offset_buf.ptr, col_data.page_value_count);
				assert(n_decode == col_data.page_value_count);

				break;
			}
			case Encoding::PLAIN:
				// nothing here, see below
				col_data.payload_ptr = payload_ptr;
				break;

			default:
				throw runtime_error(
						"Data page has unsupported/invalid encoding");
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

	static void parquet_scan_function(ClientContext &context,
			vector<Value> &input, DataChunk &output, FunctionData *dataptr) {
		auto &data = *((ParquetScanFunctionData*) dataptr);

		if (data.finished) {
			return;
		}

		// see if we have to switch to the next row group in the parquet file
		if (data.current_group < 0
				|| data.group_offset
						>= data.file_meta_data.row_groups[data.current_group].num_rows) {

			data.current_group++;
			data.group_offset = 0;

			if (data.current_group == data.file_meta_data.row_groups.size()) {
				data.finished = true;
				return;
			}

			// TODO column projections and filters
			// read each column chunk into RAM
			for (idx_t col_idx = 0; col_idx < output.column_count();
					col_idx++) {
				auto &chunk =
						data.file_meta_data.row_groups[data.current_group].columns[col_idx];
				if (chunk.__isset.file_path) {
					throw runtime_error(
							"Only inlined data files are supported (no references)");
				}

				if (chunk.meta_data.path_in_schema.size() != 1) {
					throw runtime_error(
							"Only flat tables are supported (no nesting)");
				}

				// ugh. sometimes there is an extra offset for the dict. sometimes it's wrong.
				auto chunk_start = chunk.meta_data.data_page_offset;
				if (chunk.meta_data.__isset.dictionary_page_offset
						&& chunk.meta_data.dictionary_page_offset >= 4) {
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
				// trigger the reading of a new page below
				// TODO this is a bit dirty
				data.column_data[col_idx].page_value_count = 0;
			}
		}

		auto current_group = data.file_meta_data.row_groups[data.current_group];
		output.SetCardinality(std::min((int64_t) STANDARD_VECTOR_SIZE,
				current_group.num_rows - data.group_offset));
		assert(output.size() > 0);


		// TODO column projections and filters
		for (idx_t col_idx = 0; col_idx < output.column_count(); col_idx++) {
			auto &col_data = data.column_data[col_idx];

			// we might need to read multiple pages to fill the data chunk
			idx_t output_offset = 0;
			while (output_offset < output.size()) {
				// do this unpack business only if we run out of stuff from the current page
				if (col_data.page_offset >= col_data.page_value_count) {

					// read dictionaries and data page headers so that we are ready to go for scan
					if (!prepare_buffers(data, col_idx)) {
						continue;
					}
					col_data.page_offset = 0;
				}

				auto current_batch_size = std::min(col_data.page_value_count
						- col_data.page_offset, output.size() - output_offset);

				assert(current_batch_size > 0);

				auto defined_ptr = (uint8_t*) col_data.defined_buf.ptr + col_data.page_offset;


				// TODO get the null count from GetBatch as well
//				idx_t null_count = 0;
//				for (idx_t i = 0; i < current_batch_size; i++) {
//					if (!defined.ptr[i]) {
//						null_count++;
//					}
//				}

				switch (col_data.page_encoding) {
				case Encoding::RLE_DICTIONARY:
				case Encoding::PLAIN_DICTIONARY: {
					// TODO uuugly
					auto offsets_ptr = (uint32_t*) (col_data.offset_buf.ptr + col_data.page_offset * 4);


					// TODO ensure we had seen a dict page IN THIS CHUNK before getting here

					// TODO NULLs for primitive types

					switch (data.sql_types[col_idx].id) {
					case SQLTypeId::BOOLEAN:
						fill_from_dict<bool>(col_data.dict.ptr, offsets_ptr,
								current_batch_size, output.data[col_idx], output_offset);
						break;
					case SQLTypeId::INTEGER:
						fill_from_dict<int32_t>(col_data.dict.ptr, offsets_ptr,
								current_batch_size, output.data[col_idx], output_offset);
						break;
					case SQLTypeId::BIGINT:
						fill_from_dict<int64_t>(col_data.dict.ptr, offsets_ptr,
								current_batch_size, output.data[col_idx], output_offset);
						break;
					case SQLTypeId::FLOAT:
						fill_from_dict<float>(col_data.dict.ptr, offsets_ptr,
								current_batch_size, output.data[col_idx], output_offset);
						break;
					case SQLTypeId::DOUBLE:
						fill_from_dict<double>(col_data.dict.ptr, offsets_ptr,
								current_batch_size, output.data[col_idx], output_offset);
						break;
					case SQLTypeId::TIMESTAMP:
						fill_from_dict<timestamp_t>(col_data.dict.ptr,
								offsets_ptr, current_batch_size, output.data[col_idx],
								output_offset);
						break;
					case SQLTypeId::VARCHAR: {
						assert(col_data.string_collection); // TODO make this an exception

						// the strings can be anywhere in the collection so just reference it all
						for (auto &chunk : col_data.string_collection->chunks) {
							StringVector::AddHeapReference(output.data[col_idx],
									chunk->data[0]);
						}

						for (idx_t i = 0; i < current_batch_size; i++) {
							if (!defined_ptr[i]) {
								FlatVector::SetNull(output.data[col_idx], i + output_offset,
										true);
							} else {
								// TODO this should be an exception, too
								auto dict_offset = offsets_ptr[i];
								//FlatVector::SetNull(output.data[col_idx], i + output_offset, true);


								assert(dict_offset < col_data.string_collection->count);
								auto &chunk =
										col_data.string_collection->chunks[dict_offset
												/ STANDARD_VECTOR_SIZE];
								auto &vec = chunk->data[0];

								FlatVector::GetData<string_t>(
										output.data[col_idx])[i + output_offset] =
										FlatVector::GetData<string_t>(vec)[dict_offset
												% STANDARD_VECTOR_SIZE];
							}
						}
					}
						break;
					default:
						throw runtime_error(
								SQLTypeToString(data.sql_types[col_idx]));
					}

//					for (idx_t i = n_read; i < n_read + this_n; i++) {
//						FlatVector::SetNull(output.data[col_idx], i, true);
//					}
					break;
				}
				// TODO various issues remain here
				case Encoding::PLAIN:

					assert(col_data.payload_ptr); // TODO clean

					switch (data.sql_types[col_idx].id) {
					case SQLTypeId::BOOLEAN: {

						auto target_ptr = FlatVector::GetData<bool>(
								output.data[col_idx]);

						int byte_pos = 0;
						auto payload_ptr = col_data.payload_ptr;
						for (int32_t i = 0; i < current_batch_size; i++) {

							if (!defined_ptr[i]) {
								continue;
							}
							target_ptr[i + output_offset] = (*payload_ptr >> byte_pos)
									& 1;
							byte_pos++;
							if (byte_pos == 8) {
								byte_pos = 0;
								payload_ptr++;
							}
						}
					}

						break;
					case SQLTypeId::INTEGER: {
						auto plain_ptr = (int32_t*) col_data.payload_ptr;
						auto target_ptr = FlatVector::GetData<int32_t>(
								output.data[col_idx]);
						for (idx_t i = 0; i < current_batch_size; i++) {
							target_ptr[i + output_offset] = plain_ptr[i
									+ col_data.page_offset];
						}

						break;
					}
					case SQLTypeId::BIGINT:
					case SQLTypeId::FLOAT:
						break;
					case SQLTypeId::DOUBLE:
						for (idx_t i = 0; i < current_batch_size; i++) {
							FlatVector::GetData<double>(
															output.data[col_idx])[i + output_offset] = 0;//((double*) col_data.payload_ptr)[i
			//						+ col_data.page_offset];
						}


						break;
						/*	case SQLTypeId::TIMESTAMP:
						 // TODO immediately convert, saves us some work later
						 break; */
					case SQLTypeId::VARCHAR: {
						// except for strings we directly fill a string heap that we can use for the vectors later
						for (idx_t i = 0; i < current_batch_size; i++) {
							FlatVector::SetNull(output.data[col_idx], i + output_offset, true);
						}
					}
						break;
					default:
						throw runtime_error(
								SQLTypeToString(data.sql_types[col_idx]));
					}

					break;

				default:
					throw runtime_error(
							"Data page has unsupported/invalid encoding");
				}

				output_offset += current_batch_size;
				col_data.page_offset += current_batch_size;
			}
		}
		data.group_offset += output.size();
	}
};

void Parquet::Init(DuckDB &db) {
	ParquetScanFunction scan_fun;
	CreateTableFunctionInfo info(scan_fun);

	Connection conn(db);
	conn.context->transaction.BeginTransaction();
	conn.context->catalog.CreateTableFunction(*conn.context, &info);
	conn.context->transaction.Commit();
}
