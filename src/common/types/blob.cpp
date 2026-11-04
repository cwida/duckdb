#include "duckdb/common/types/blob.hpp"
#include "duckdb/function/cast/default_casts.hpp"

#include "duckdb/common/assert.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/string_type.hpp"

namespace duckdb {

constexpr const char *Blob::HEX_TABLE;
const int Blob::HEX_MAP[256] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0,  1,  2,  3,  4,  5,  6,  7,  8,  9,
    -1, -1, -1, -1, -1, -1, -1, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};

bool IsRegularCharacter(data_t c) {
	return c >= 32 && c <= 126 && c != '\\' && c != '\'' && c != '"';
}

idx_t Blob::GetStringSize(string_t blob) {
	auto data = const_data_ptr_cast(blob.GetData());
	auto len = blob.GetSize();
	idx_t str_len = 0;
	for (idx_t i = 0; i < len; i++) {
		if (IsRegularCharacter(data[i])) {
			// ascii characters are rendered as-is
			str_len++;
		} else {
			// non-ascii characters are rendered as hexadecimal (e.g. \x00)
			str_len += 4;
		}
	}
	return str_len;
}

void Blob::ToString(string_t blob, char *output) {
	auto data = const_data_ptr_cast(blob.GetData());
	auto len = blob.GetSize();
	idx_t str_idx = 0;
	for (idx_t i = 0; i < len; i++) {
		if (IsRegularCharacter(data[i])) {
			// ascii characters are rendered as-is
			output[str_idx++] = UnsafeNumericCast<char>(data[i]);
		} else {
			auto byte_a = data[i] >> 4;
			auto byte_b = data[i] & 0x0F;
			D_ASSERT(byte_a >= 0 && byte_a < 16);
			D_ASSERT(byte_b >= 0 && byte_b < 16);
			// non-ascii characters are rendered as hexadecimal (e.g. \x00)
			output[str_idx++] = '\\';
			output[str_idx++] = 'x';
			output[str_idx++] = Blob::HEX_TABLE[byte_a];
			output[str_idx++] = Blob::HEX_TABLE[byte_b];
		}
	}
	D_ASSERT(str_idx == GetStringSize(blob));
}

string Blob::ToString(string_t blob) {
	auto str_len = GetStringSize(blob);
	auto buffer = make_unsafe_uniq_array_uninitialized<char>(str_len);
	Blob::ToString(blob, buffer.get());
	return string(buffer.get(), str_len);
}

bool Blob::TryGetBlobSize(string_t str, idx_t &str_len, CastParameters &parameters) {
	auto data = const_data_ptr_cast(str.GetData());
	auto len = str.GetSize();
	str_len = 0;
	for (idx_t i = 0; i < len; i++) {
		if (data[i] == '\\') {
			if (i + 3 >= len) {
				string error = StringUtil::Format("Invalid hex escape code encountered in string -> blob conversion of "
				                                  "string \"%s\": unterminated escape code at end of blob",
				                                  str.GetString());
				HandleCastError::AssignError(error, parameters);
				return false;
			}
			if (data[i + 1] != 'x' || Blob::HEX_MAP[data[i + 2]] < 0 || Blob::HEX_MAP[data[i + 3]] < 0) {
				string error = StringUtil::Format(
				    "Invalid hex escape code encountered in string -> blob conversion of string \"%s\": %s",
				    str.GetString(), string(const_char_ptr_cast(data) + i, 4));
				HandleCastError::AssignError(error, parameters);
				return false;
			}
			str_len++;
			i += 3;
		} else if (data[i] <= 127) {
			str_len++;
		} else {
			string error = StringUtil::Format(
			    "Invalid byte encountered in STRING -> BLOB conversion of string \"%s\". All non-ascii characters "
			    "must be escaped with hex codes (e.g. \\xAA)",
			    str.GetString());
			HandleCastError::AssignError(error, parameters);
			return false;
		}
	}
	return true;
}

idx_t Blob::GetBlobSize(string_t str) {
	CastParameters parameters;
	return GetBlobSize(str, parameters);
}

idx_t Blob::GetBlobSize(string_t str, CastParameters &parameters) {
	idx_t str_len;
	auto result = Blob::TryGetBlobSize(str, str_len, parameters);
	if (!result) {
		throw InternalException("Blob::TryGetBlobSize failed but no exception was thrown!?");
	}
	return str_len;
}

void Blob::ToBlob(string_t str, data_ptr_t output) {
	auto data = const_data_ptr_cast(str.GetData());
	auto len = str.GetSize();
	idx_t blob_idx = 0;
	for (idx_t i = 0; i < len; i++) {
		if (data[i] == '\\') {
			int byte_a = Blob::HEX_MAP[data[i + 2]];
			int byte_b = Blob::HEX_MAP[data[i + 3]];
			D_ASSERT(i + 3 < len);
			D_ASSERT(byte_a >= 0 && byte_b >= 0);
			D_ASSERT(data[i + 1] == 'x');
			output[blob_idx++] = UnsafeNumericCast<data_t>((byte_a << 4) + byte_b);
			i += 3;
		} else if (data[i] <= 127) {
			output[blob_idx++] = data_t(data[i]);
		} else {
			throw ConversionException("Invalid byte encountered in STRING -> BLOB conversion. All non-ascii characters "
			                          "must be escaped with hex codes (e.g. \\xAA)");
		}
	}
	D_ASSERT(blob_idx == GetBlobSize(str));
}

string Blob::ToBlob(string_t str) {
	CastParameters parameters;
	return Blob::ToBlob(str, parameters);
}

string Blob::ToBlob(string_t str, CastParameters &parameters) {
	auto blob_len = GetBlobSize(str, parameters);
	auto buffer = make_unsafe_uniq_array_uninitialized<char>(blob_len);
	Blob::ToBlob(str, data_ptr_cast(buffer.get()));
	return string(buffer.get(), blob_len);
}

// base64 functions are adapted from https://gist.github.com/tomykaira/f0fd86b6c73063283afe550bc5d77594
string Blob::ToBase64(string_t blob) {
	auto base64_size = Blob::ToBase64Size(blob);
	auto data = make_uniq_array<char>(base64_size);
	Blob::ToBase64(blob, data.get());
	return string(data.get(), base64_size);
}

idx_t Blob::ToBase64Size(string_t blob) {
	// every 4 characters in base64 encode 3 bytes, plus (potential) padding at the end
	auto input_size = blob.GetSize();
	return ((input_size + 2) / 3) * 4;
}

void Blob::ToBase64(string_t blob, char *output) {
	auto input_data = const_data_ptr_cast(blob.GetData());
	auto input_size = blob.GetSize();
	idx_t out_idx = 0;
	idx_t i;
	// convert the bulk of the string to base64
	// this happens in steps of 3 bytes -> 4 output bytes
	for (i = 0; i + 2 < input_size; i += 3) {
		output[out_idx++] = Blob::BASE64_MAP[(input_data[i] >> 2) & 0x3F];
		output[out_idx++] = Blob::BASE64_MAP[((input_data[i] & 0x3) << 4) | ((input_data[i + 1] & 0xF0) >> 4)];
		output[out_idx++] = Blob::BASE64_MAP[((input_data[i + 1] & 0xF) << 2) | ((input_data[i + 2] & 0xC0) >> 6)];
		output[out_idx++] = Blob::BASE64_MAP[input_data[i + 2] & 0x3F];
	}

	if (i < input_size) {
		// there are one or two bytes left over: we have to insert padding
		// first write the first 6 bits of the first byte
		output[out_idx++] = Blob::BASE64_MAP[(input_data[i] >> 2) & 0x3F];
		// now check the character count
		if (i == input_size - 1) {
			// single byte left over: convert the remainder of that byte and insert padding
			output[out_idx++] = Blob::BASE64_MAP[((input_data[i] & 0x3) << 4)];
			output[out_idx++] = Blob::BASE64_PADDING;
		} else {
			// two bytes left over: convert the second byte as well
			output[out_idx++] = Blob::BASE64_MAP[((input_data[i] & 0x3) << 4) | ((input_data[i + 1] & 0xF0) >> 4)];
			output[out_idx++] = Blob::BASE64_MAP[((input_data[i + 1] & 0xF) << 2)];
		}
		output[out_idx++] = Blob::BASE64_PADDING;
	}
}

static constexpr int BASE64_DECODING_TABLE[256] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62, -1, -1, -1, 63, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61,
    -1, -1, -1, -1, -1, -1, -1, 0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21,
    22, 23, 24, 25, -1, -1, -1, -1, -1, -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44,
    45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};

string Blob::FromBase64(string_t blob) {
	auto decoded_size = Blob::FromBase64Size(blob);
	auto data = make_uniq_array<data_t>(decoded_size);
	Blob::FromBase64(blob, data.get(), decoded_size);
	return string(char_ptr_cast(data.get()), decoded_size);
}

idx_t Blob::FromBase64Size(string_t str) {
	auto input_data = str.GetData();
	auto input_size = str.GetSize();
	if (input_size % 4 != 0) {
		// valid base64 needs to always be cleanly divisible by 4
		throw ConversionException("Could not decode string \"%s\" as base64: length must be a multiple of 4",
		                          str.GetString());
	}
	if (input_size < 4) {
		// empty string
		return 0;
	}
	auto base_size = input_size / 4 * 3;
	// check for padding to figure out the length
	if (input_data[input_size - 2] == Blob::BASE64_PADDING) {
		// two bytes of padding
		return base_size - 2;
	}
	if (input_data[input_size - 1] == Blob::BASE64_PADDING) {
		// one byte of padding
		return base_size - 1;
	}
	// no padding
	return base_size;
}

template <bool ALLOW_PADDING>
uint32_t DecodeBase64Bytes(const string_t &str, const_data_ptr_t input_data, idx_t base_idx) {
	int decoded_bytes[4];
	if (ALLOW_PADDING && input_data[base_idx + 2] == Blob::BASE64_PADDING &&
	    input_data[base_idx + 3] != Blob::BASE64_PADDING) {
		// padding must be at the end: data cannot follow the padding
		throw ConversionException("Could not decode string \"%s\" as base64: data after the padding at position %d",
		                          str.GetString(), base_idx + 3);
	}
	for (idx_t decode_idx = 0; decode_idx < 4; decode_idx++) {
		if (ALLOW_PADDING && decode_idx >= 2 && input_data[base_idx + decode_idx] == Blob::BASE64_PADDING) {
			// the last two bytes of a base64 string can have padding: in this case we set the byte to 0
			decoded_bytes[decode_idx] = 0;
		} else {
			decoded_bytes[decode_idx] = BASE64_DECODING_TABLE[input_data[base_idx + decode_idx]];
		}
		if (decoded_bytes[decode_idx] < 0) {
			throw ConversionException(
			    "Could not decode string \"%s\" as base64: invalid byte value '%d' at position %d", str.GetString(),
			    input_data[base_idx + decode_idx], base_idx + decode_idx);
		}
	}
	return UnsafeNumericCast<uint32_t>((decoded_bytes[0] << 3 * 6) + (decoded_bytes[1] << 2 * 6) +
	                                   (decoded_bytes[2] << 1 * 6) + (decoded_bytes[3] << 0 * 6));
}

void Blob::FromBase64(string_t str, data_ptr_t output, idx_t output_size) {
	D_ASSERT(output_size == FromBase64Size(str));
	auto input_data = const_data_ptr_cast(str.GetData());
	auto input_size = str.GetSize();
	if (input_size == 0) {
		return;
	}
	idx_t out_idx = 0;
	idx_t i = 0;
	for (i = 0; i + 4 < input_size; i += 4) {
		auto combined = DecodeBase64Bytes<false>(str, input_data, i);
		output[out_idx++] = (combined >> 2 * 8) & 0xFF;
		output[out_idx++] = (combined >> 1 * 8) & 0xFF;
		output[out_idx++] = (combined >> 0 * 8) & 0xFF;
	}
	// decode the final four bytes: padding is allowed here
	auto combined = DecodeBase64Bytes<true>(str, input_data, i);
	output[out_idx++] = (combined >> 2 * 8) & 0xFF;
	if (out_idx < output_size) {
		output[out_idx++] = (combined >> 1 * 8) & 0xFF;
	}
	if (out_idx < output_size) {
		output[out_idx++] = (combined >> 0 * 8) & 0xFF;
	}
}

} // namespace duckdb
