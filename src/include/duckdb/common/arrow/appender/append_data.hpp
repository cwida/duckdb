//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/arrow/appender/append_data.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/arrow/arrow.hpp"
#include "duckdb/common/arrow/arrow_buffer.hpp"
#include "duckdb/main/client_properties.hpp"
#include "duckdb/common/array.hpp"
#include "duckdb/function/table/arrow/arrow_duck_schema.hpp"

namespace duckdb {

struct ArrowAppendData;

//===--------------------------------------------------------------------===//
// Arrow append data
//===--------------------------------------------------------------------===//
typedef void (*initialize_t)(ArrowAppendData &result, const LogicalType &type, idx_t capacity);
// append_data: The arrow array we're appending into
// input: The data we're appending
// from: The offset into the input we're scanning
// to: The last index of the input we're scanning
// input_size: The total size of the 'input' Vector.
typedef void (*append_vector_t)(ArrowAppendData &append_data, const Vector &input, idx_t from, idx_t to,
                                idx_t input_size);
typedef void (*finalize_t)(ArrowAppendData &append_data, const LogicalType &type, ArrowArray *result);

// This struct is used to save state for appending a column
// afterwards the ownership is passed to the arrow array, as 'private_data'
// FIXME: we should separate the append state variables from the variables required by the ArrowArray into
// ArrowAppendState
struct ArrowAppendData {
	explicit ArrowAppendData(const ClientProperties &options_p) : options(options_p) {
		dictionary.release = nullptr;
		arrow_buffers.resize(3);
	}

public:
	//! Getters for the Buffers
	ArrowBuffer &GetValidityBuffer() {
		return arrow_buffers[0];
	}

	ArrowBuffer &GetMainBuffer() {
		return arrow_buffers[1];
	}

	ArrowBuffer &GetAuxBuffer() {
		return arrow_buffers[2];
	}

	ArrowBuffer &GetBufferSizeBuffer() {
		//! This is a special case, we resize it if necessary since it's a different size than set in the constructor
		if (arrow_buffers.size() == 3) {
			arrow_buffers.resize(4);
		}
		return arrow_buffers[3];
	}

public:
	static void GetBitPosition(idx_t row_idx, idx_t &current_byte, uint8_t &current_bit) {
		current_byte = row_idx / 8;
		current_bit = row_idx % 8;
	}

	static void UnsetBit(uint8_t *data, idx_t current_byte, uint8_t current_bit) {
		data[current_byte] &= ~((uint64_t)1 << current_bit);
	}

	static void NextBit(idx_t &current_byte, uint8_t &current_bit) {
		current_bit++;
		if (current_bit == 8) {
			current_byte++;
			current_bit = 0;
		}
	}

	static void ResizeValidity(ArrowBuffer &buffer, idx_t row_count) {
		auto byte_count = (row_count + 7) / 8;
		buffer.resize(byte_count, 0xFF);
	}

	void SetNull(uint8_t *validity_data, idx_t current_byte, uint8_t current_bit) {
		UnsetBit(validity_data, current_byte, current_bit);
		null_count++;
	}
	void AppendValidity(UnifiedVectorFormat &format, idx_t from, idx_t to);

	//! Append a (child) vector, routing it through the Arrow extension's duckdb_to_arrow
	//! conversion first when one is set. Container appenders must call this instead of
	//! append_vector so nested extension types (e.g. arrow.bool8 BOOLEAN) get the same
	//! conversion the top-level appender applies, keeping the data layout in sync with
	//! the schema declared by SetArrowFormat.
	void AppendChild(const Vector &input, idx_t from, idx_t to, idx_t input_size);

	//! The bytes appended so far: this column's buffers plus those of its children
	idx_t ByteSize() const {
		idx_t bytes = 0;
		for (auto &buffer : arrow_buffers) {
			bytes += buffer.size();
		}
		for (auto &child : child_data) {
			bytes += child->ByteSize();
		}
		return bytes;
	}

public:
	idx_t row_count = 0;
	idx_t null_count = 0;

	//! function pointers for construction
	initialize_t initialize = nullptr;
	append_vector_t append_vector = nullptr;
	//! Arrow Extension Type information
	shared_ptr<ArrowTypeExtensionData> extension_data = nullptr;
	finalize_t finalize = nullptr;

	//! child data (if any)
	vector<unique_ptr<ArrowAppendData>> child_data;

	//! the arrow array C API data, only set after Finalize
	unique_ptr<ArrowArray> array;
	duckdb::array<const void *, 4> buffers = {{nullptr, nullptr, nullptr, nullptr}};
	vector<ArrowArray *> child_pointers;
	//! Arrays so the children can be moved
	vector<ArrowArray> child_arrays;
	ArrowArray dictionary;

	ClientProperties options;
	//! Offset used to keep data positions when producing a mix of inlined and not-inlined arrow string views.
	idx_t offset = 0;

private:
	//! The buffers of the arrow vector
	vector<ArrowBuffer> arrow_buffers;
};

} // namespace duckdb
