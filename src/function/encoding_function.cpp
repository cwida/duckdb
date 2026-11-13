#include "duckdb/function/encoding_function.hpp"
#include "duckdb/main/config.hpp"

#include "duckdb/execution/operator/csv_scanner/encode/csv_encoder.hpp"

namespace duckdb {

struct DefaultEncodeMethod {
	string name;
	encode_t encode_function;
	idx_t ratio;
	idx_t bytes_per_iteration;
};

void DecodeUTF16ToUTF8(CSVEncoderBuffer &encoded_buffer, char *target_buffer, idx_t &target_buffer_current_position,
                       const idx_t target_buffer_size, char *remaining_bytes_buffer, idx_t &remaining_bytes_size,
                       EncodingFunction *encoding_function) {
	auto encoded_ptr = encoded_buffer.Ptr();
	for (; encoded_buffer.cur_pos < encoded_buffer.actual_encoded_buffer_size; encoded_buffer.cur_pos += 2) {
		if (target_buffer_current_position == target_buffer_size) {
			// We are done
			return;
		}
		if (encoded_buffer.cur_pos + 1 >= encoded_buffer.actual_encoded_buffer_size) {
			if (encoded_buffer.last_buffer) {
				throw InvalidInputException("File is not utf-16 encoded");
			}
			// the second byte is passed on to the next buffer
			return;
		}
		const uint16_t ch =
		    static_cast<uint16_t>(static_cast<unsigned char>(encoded_ptr[encoded_buffer.cur_pos]) |
		                          (static_cast<unsigned char>(encoded_ptr[encoded_buffer.cur_pos + 1]) << 8));
		if (ch >= 0xD800 && ch <= 0xDFFF) {
			if (ch >= 0xDC00) {
				// low surrogate without a preceding high surrogate
				throw InvalidInputException("File is not utf-16 encoded");
			}
			if (encoded_buffer.cur_pos + 3 >= encoded_buffer.actual_encoded_buffer_size) {
				if (encoded_buffer.last_buffer) {
					throw InvalidInputException("File is not utf-16 encoded");
				}
				// the low surrogate is passed on to the next buffer together with the high surrogate
				return;
			}
			const uint16_t low =
			    static_cast<uint16_t>(static_cast<unsigned char>(encoded_ptr[encoded_buffer.cur_pos + 2]) |
			                          (static_cast<unsigned char>(encoded_ptr[encoded_buffer.cur_pos + 3]) << 8));
			if (low < 0xDC00 || low > 0xDFFF) {
				throw InvalidInputException("File is not utf-16 encoded");
			}
			const uint32_t code_point = 0x10000 + ((static_cast<uint32_t>(ch - 0xD800)) << 10) + (low - 0xDC00);
			const char utf8[4] = {
			    static_cast<char>(0xF0 | (code_point >> 18)), static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)),
			    static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)), static_cast<char>(0x80 | (code_point & 0x3F))};
			idx_t written = 0;
			while (written < 4 && target_buffer_current_position < target_buffer_size) {
				target_buffer[target_buffer_current_position++] = utf8[written++];
			}
			if (written < 4) {
				// We are done, but we have to store the remaining bytes for the next chunk!
				encoded_buffer.cur_pos += 4;
				remaining_bytes_size = 0;
				for (; written < 4; written++) {
					remaining_bytes_buffer[remaining_bytes_size++] = utf8[written];
				}
				return;
			}
			// skip the low surrogate, the loop skips the high surrogate
			encoded_buffer.cur_pos += 2;
			continue;
		}
		if (ch <= 0x007F) {
			// 1-byte UTF-8 for ASCII characters
			target_buffer[target_buffer_current_position++] = static_cast<char>(ch & 0x7F);
		} else if (ch <= 0x07FF) {
			// 2-byte UTF-8
			target_buffer[target_buffer_current_position++] = static_cast<char>(0xC0 | (ch >> 6));
			if (target_buffer_current_position == target_buffer_size) {
				// We are done, but we have to store one byte for the next chunk!
				encoded_buffer.cur_pos += 2;
				remaining_bytes_buffer[0] = static_cast<char>(0x80 | (ch & 0x3F));
				remaining_bytes_size = 1;
				return;
			}
			target_buffer[target_buffer_current_position++] = static_cast<char>(0x80 | (ch & 0x3F));
		} else {
			// 3-byte UTF-8
			target_buffer[target_buffer_current_position++] = static_cast<char>(0xE0 | (ch >> 12));
			if (target_buffer_current_position == target_buffer_size) {
				// We are done, but we have to store two bytes for the next chunk!
				encoded_buffer.cur_pos += 2;
				remaining_bytes_buffer[0] = static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
				remaining_bytes_buffer[1] = static_cast<char>(0x80 | (ch & 0x3F));
				remaining_bytes_size = 2;
				return;
			}
			target_buffer[target_buffer_current_position++] = static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
			if (target_buffer_current_position == target_buffer_size) {
				// We are done, but we have to store one byte for the next chunk!
				encoded_buffer.cur_pos += 2;
				remaining_bytes_buffer[0] = static_cast<char>(0x80 | (ch & 0x3F));
				remaining_bytes_size = 1;
				return;
			}
			target_buffer[target_buffer_current_position++] = static_cast<char>(0x80 | (ch & 0x3F));
		}
	}
}

void DecodeLatin1ToUTF8(CSVEncoderBuffer &encoded_buffer, char *target_buffer, idx_t &target_buffer_current_position,
                        const idx_t target_buffer_size, char *remaining_bytes_buffer, idx_t &remaining_bytes_size,
                        EncodingFunction *encoding_function) {
	auto encoded_ptr = encoded_buffer.Ptr();
	for (; encoded_buffer.cur_pos < encoded_buffer.actual_encoded_buffer_size; encoded_buffer.cur_pos++) {
		if (target_buffer_current_position == target_buffer_size) {
			// We are done
			return;
		}
		const unsigned char ch = static_cast<unsigned char>(encoded_ptr[encoded_buffer.cur_pos]);
		if (ch > 0x7F && ch <= 0x9F) {
			throw InvalidInputException("File is not latin-1 encoded");
		}
		if (ch <= 0x7F) {
			// ASCII: 1 byte in UTF-8
			target_buffer[target_buffer_current_position++] = static_cast<char>(ch);
		} else {
			// Non-ASCII: 2 bytes in UTF-8
			target_buffer[target_buffer_current_position++] = static_cast<char>(0xc2 + (ch > 0xbf));
			if (target_buffer_current_position == target_buffer_size) {
				// We are done, but we have to store one byte for the next chunk!
				encoded_buffer.cur_pos++;
				remaining_bytes_buffer[0] = static_cast<char>((ch & 0x3f) + 0x80);
				remaining_bytes_size = 1;
				return;
			}
			target_buffer[target_buffer_current_position++] = static_cast<char>((ch & 0x3f) + 0x80);
		}
	}
}

void DecodeUTF8(CSVEncoderBuffer &encoded_buffer, char *target_buffer, idx_t &target_buffer_current_position,
                const idx_t target_buffer_size, char *remaining_bytes_buffer, idx_t &remaining_bytes_size,
                EncodingFunction *encoding_function) {
	throw InternalException("Decode UTF8 is not a valid function, and should be verified one level up.");
}

void EncodingFunctionSet::Initialize(DBConfig &config) {
	config.RegisterEncodeFunction({"utf-8", DecodeUTF8, 1, 1});
	config.RegisterEncodeFunction({"latin-1", DecodeLatin1ToUTF8, 2, 1});
	config.RegisterEncodeFunction({"utf-16", DecodeUTF16ToUTF8, 3, 4});
}

void DBConfig::RegisterEncodeFunction(const EncodingFunction &function) const {
	lock_guard<mutex> l(encoding_functions->lock);
	const auto name = function.GetName();
	if (encoding_functions->functions.find(name) != encoding_functions->functions.end()) {
		throw InvalidInputException("Decoding function with name %s already registered", name);
	}
	encoding_functions->functions[name] = function;
}

optional_ptr<EncodingFunction> DBConfig::GetEncodeFunction(const string &name) const {
	lock_guard<mutex> l(encoding_functions->lock);
	// Check if the function is already loaded into the global compression functions.
	if (encoding_functions->functions.find(name) != encoding_functions->functions.end()) {
		return &encoding_functions->functions[name];
	}
	return nullptr;
}

vector<reference<EncodingFunction>> DBConfig::GetLoadedEncodedFunctions() const {
	lock_guard<mutex> l(encoding_functions->lock);
	vector<reference<EncodingFunction>> result;
	for (auto &function : encoding_functions->functions) {
		result.push_back(function.second);
	}
	return result;
}
} // namespace duckdb
