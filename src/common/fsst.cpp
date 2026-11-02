#include "duckdb/common/fsst.hpp"
#include "duckdb/common/exception.hpp"

namespace duckdb {

void FSSTPrimitives::ThrowInvalidCompressedString() {
	throw DataCorruptionException("Failed to decompress FSST string - escape code has no following byte");
}

[[noreturn]] static void ThrowFSSTDecodedStringTooLarge() {
	throw DataCorruptionException("Failed to decompress FSST string - decoded size exceeds the output buffer");
}

string FSSTPrimitives::DecompressValue(void *duckdb_fsst_decoder, const char *compressed_string,
                                       const idx_t compressed_string_len, vector<unsigned char> &decompress_buffer) {
	D_ASSERT(!decompress_buffer.empty());
	auto compressed_string_ptr = reinterpret_cast<const unsigned char *>(compressed_string);
	auto fsst_decoder = static_cast<duckdb_fsst_decoder_t *>(duckdb_fsst_decoder);
	auto decompressed_string_size = duckdb_fsst_decompress(fsst_decoder, compressed_string_len, compressed_string_ptr,
	                                                       decompress_buffer.size(), decompress_buffer.data());
	if (decompressed_string_size == DUCKDB_FSST_DECOMPRESS_INVALID_INPUT) {
		ThrowInvalidCompressedString();
	}

	// The decoder reports the full string length even when the output buffer is too small.
	if (decompressed_string_size > decompress_buffer.size()) {
		ThrowFSSTDecodedStringTooLarge();
	}
	return string(char_ptr_cast(decompress_buffer.data()), decompressed_string_size);
}

} // namespace duckdb
