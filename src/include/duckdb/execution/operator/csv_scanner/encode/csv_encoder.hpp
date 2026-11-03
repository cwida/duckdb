//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/csv_scanner/encode/csv_encoder.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/function/encoding_function.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {

struct DBConfig;

//! Struct that holds encoder buffers
struct CSVEncoderBuffer {
	CSVEncoderBuffer() : encoded_buffer_size(0) {};
	void Initialize(idx_t encoded_buffer_size);

	char *Ptr() const;

	idx_t GetCapacity() const;

	idx_t GetSize() const;

	void SetSize(const idx_t buffer_size);

	bool HasDataToRead() const;

	void Reset();
	idx_t cur_pos = 0;
	//! The actual encoded buffer size, from the last file_handle read.
	idx_t actual_encoded_buffer_size = 0;
	//! If this is the last buffer
	bool last_buffer = false;

private:
	//! The encoded buffer, we only have one per file, so we cache it and make sure to pass over unused bytes.
	duckdb::unique_ptr<char[]> encoded_buffer;
	//! The encoded buffer size is defined as buffer_size/GetRatio()
	idx_t encoded_buffer_size;
};

class CSVEncoder {
public:
	//! Constructor, basically takes an encoding and the output buffer size
	CSVEncoder(ClientContext &context, const string &encoding_name, idx_t buffer_size);
	//! Main encode function, it reads the file into an encoded buffer and converts it to the output buffer
	idx_t Encode(FileHandle &file_handle_input, char *output_buffer, const idx_t decoded_buffer_size);
	//! Resets the encoder so the file can be read again from the beginning
	void Reset();
	//! The encoded bytes read from the file
	idx_t EncodedBytesRead() const {
		return encoded_bytes_read;
	}
	string encoding_name;

private:
	QueryContext context;

	//! The actual encoded buffer
	CSVEncoderBuffer encoded_buffer;
	//! Potential remaining bytes
	CSVEncoderBuffer remaining_bytes_buffer;
	//! Actual Encoding Function
	optional_ptr<EncodingFunction> encoding_function;
	//! Pass-on Byte, used to check if we are done with the file, but must be appended to next buffer
	char pass_on_byte;
	bool has_pass_on_byte = false;
	idx_t encoded_bytes_read = 0;
};
} // namespace duckdb
