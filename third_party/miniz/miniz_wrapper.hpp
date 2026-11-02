//===----------------------------------------------------------------------===//
//                         DuckDB
//
// miniz_wrapper.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "miniz.hpp"
#include <string>
#include <stdexcept>

namespace duckdb {

enum class MiniZStreamType { MINIZ_TYPE_NONE, MINIZ_TYPE_INFLATE, MINIZ_TYPE_DEFLATE };

struct MiniZStream {
	static constexpr uint8_t GZIP_HEADER_MINSIZE = 10;
	static constexpr uint8_t GZIP_FOOTER_SIZE = 8;
	static constexpr uint8_t GZIP_COMPRESSION_DEFLATE = 0x08;
	static constexpr unsigned char GZIP_FLAG_UNSUPPORTED = 0x1 | 0x2 | 0x4 | 0x10 | 0x20;

public:
	MiniZStream() : type(MiniZStreamType::MINIZ_TYPE_NONE) {
		ResetStreamInternal();
	}

	~MiniZStream() {
		switch (type) {
		case MiniZStreamType::MINIZ_TYPE_INFLATE:
			duckdb_miniz::mz_inflateEnd(&stream);
			break;
		case MiniZStreamType::MINIZ_TYPE_DEFLATE:
			duckdb_miniz::mz_deflateEnd(&stream);
			break;
		default:
			break;
		}
	}

	void FormatException(const std::string &error_msg) {
		throw std::runtime_error(error_msg);
	}

	void FormatException(const char *error_msg, int mz_ret) {
		auto err = duckdb_miniz::mz_error(mz_ret);
		FormatException(error_msg + std::string(": ") + (err ? err : "Unknown error code"));
	}

	void Decompress(const char *compressed_data, size_t compressed_size, char *out_data, size_t out_size) {
		type = MiniZStreamType::MINIZ_TYPE_INFLATE;

		// Loop over blocks
		while (compressed_size > 0) {
			// Read block header
			if (compressed_size < GZIP_HEADER_MINSIZE) {
				FormatException("Failed to decompress GZIP block: compressed size is less than gzip header size");
			}
			auto gzip_hdr = reinterpret_cast<const unsigned char *>(compressed_data);
			if (gzip_hdr[0] != 0x1F || gzip_hdr[1] != 0x8B || gzip_hdr[2] != GZIP_COMPRESSION_DEFLATE ||
			    gzip_hdr[3] & GZIP_FLAG_UNSUPPORTED) {
				FormatException("Input is invalid/unsupported GZIP stream");
			}
			compressed_data += GZIP_HEADER_MINSIZE;
			compressed_size -= GZIP_HEADER_MINSIZE;

			// Initialize stream
			auto mz_ret = mz_inflateInit2(&stream, -MZ_DEFAULT_WINDOW_BITS);
			if (mz_ret != duckdb_miniz::MZ_OK) {
				FormatException("Failed to initialize miniz", mz_ret);
			}

			// Set up in/out
			stream.next_in = reinterpret_cast<const unsigned char *>(compressed_data);
			stream.avail_in = static_cast<unsigned int>(compressed_size);
			stream.next_out = reinterpret_cast<unsigned char *>(out_data);
			stream.avail_out = static_cast<unsigned int>(out_size);

			// Decompress and uninitialize stream
			mz_ret = mz_inflate(&stream, duckdb_miniz::MZ_FINISH);
			if (mz_ret != duckdb_miniz::MZ_OK && mz_ret != duckdb_miniz::MZ_STREAM_END) {
				FormatException("Failed to decompress GZIP block", mz_ret);
			}
			mz_inflateEnd(&stream);

			// Update indices
			if (stream.total_in > compressed_size || compressed_size - stream.total_in < GZIP_FOOTER_SIZE) {
				FormatException("Failed to decompress GZIP block: incomplete GZIP member footer");
			}
			compressed_data += GZIP_FOOTER_SIZE + stream.total_in;
			compressed_size -= GZIP_FOOTER_SIZE + stream.total_in;
			out_data += stream.total_out;
			out_size -= stream.total_out;

			ResetStreamInternal();
		}
	}

	static size_t MaxCompressedLength(size_t input_size) {
		return duckdb_miniz::mz_compressBound(input_size) + GZIP_HEADER_MINSIZE + GZIP_FOOTER_SIZE;
	}

	static void InitializeGZIPHeader(unsigned char *gzip_header) {
		memset(gzip_header, 0, GZIP_HEADER_MINSIZE);
		gzip_header[0] = 0x1F;
		gzip_header[1] = 0x8B;
		gzip_header[2] = GZIP_COMPRESSION_DEFLATE;
		gzip_header[3] = 0;
		gzip_header[4] = 0;
		gzip_header[5] = 0;
		gzip_header[6] = 0;
		gzip_header[7] = 0;
		gzip_header[8] = 0;
		gzip_header[9] = 0xFF;
	}

	static void InitializeGZIPFooter(unsigned char *gzip_footer, duckdb_miniz::mz_ulong crc, idx_t uncompressed_size) {
		gzip_footer[0] = crc & 0xFF;
		gzip_footer[1] = (crc >> 8) & 0xFF;
		gzip_footer[2] = (crc >> 16) & 0xFF;
		gzip_footer[3] = (crc >> 24) & 0xFF;
		gzip_footer[4] = uncompressed_size & 0xFF;
		gzip_footer[5] = (uncompressed_size >> 8) & 0xFF;
		gzip_footer[6] = (uncompressed_size >> 16) & 0xFF;
		gzip_footer[7] = (uncompressed_size >> 24) & 0xFF;
	}

	void Compress(const char *uncompressed_data, size_t uncompressed_size, char *out_data, size_t *out_size) {
		auto mz_ret =
		    mz_deflateInit2(&stream, duckdb_miniz::MZ_DEFAULT_LEVEL, MZ_DEFLATED, -MZ_DEFAULT_WINDOW_BITS, 1, 0);
		if (mz_ret != duckdb_miniz::MZ_OK) {
			FormatException("Failed to initialize miniz", mz_ret);
		}
		type = MiniZStreamType::MINIZ_TYPE_DEFLATE;

		auto gzip_header = reinterpret_cast<unsigned char *>(out_data);
		InitializeGZIPHeader(gzip_header);

		auto gzip_body = gzip_header + GZIP_HEADER_MINSIZE;

		stream.next_in = reinterpret_cast<const unsigned char *>(uncompressed_data);
		stream.avail_in = static_cast<unsigned int>(uncompressed_size);
		stream.next_out = gzip_body;
		stream.avail_out = static_cast<unsigned int>(*out_size - GZIP_HEADER_MINSIZE);

		mz_ret = mz_deflate(&stream, duckdb_miniz::MZ_FINISH);
		if (mz_ret != duckdb_miniz::MZ_OK && mz_ret != duckdb_miniz::MZ_STREAM_END) {
			FormatException("Failed to compress GZIP block", mz_ret);
		}
		auto gzip_footer = gzip_body + stream.total_out;
		auto crc = duckdb_miniz::mz_crc32(MZ_CRC32_INIT, reinterpret_cast<const unsigned char *>(uncompressed_data),
		                                  uncompressed_size);
		InitializeGZIPFooter(gzip_footer, crc, uncompressed_size);

		*out_size = stream.total_out + GZIP_HEADER_MINSIZE + GZIP_FOOTER_SIZE;
	}

private:
	void ResetStreamInternal() {
		memset(&stream, 0, sizeof(duckdb_miniz::mz_stream));
	}

private:
	duckdb_miniz::mz_stream stream;
	MiniZStreamType type;
};

} // namespace duckdb
