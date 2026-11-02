//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/compression/chimp/chimp.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/storage/compression/chimp/algorithm/chimp128.hpp"

namespace duckdb {

using byte_index_t = uint32_t;

template <class T>
struct ChimpType {};

template <>
struct ChimpType<double> {
	using TYPE = uint64_t;
};

template <>
struct ChimpType<float> {
	using TYPE = uint32_t;
};

class ChimpPrimitives {
public:
	using METADATA_POINTER_TYPE = uint32_t;

	static constexpr uint32_t CHIMP_SEQUENCE_SIZE = 1024;
	static constexpr uint8_t MAX_BYTES_PER_VALUE = sizeof(double) + 1; // extra wiggle room
	static constexpr uint8_t HEADER_SIZE = sizeof(METADATA_POINTER_TYPE);
	static constexpr uint8_t FLAG_BIT_SIZE = 2;
	static constexpr uint32_t LEADING_ZERO_BLOCK_BUFFERSIZE = 1 + (CHIMP_SEQUENCE_SIZE / 8) * 3;
};

//! Where all the magic happens
template <class T, bool EMPTY>
struct ChimpState {
public:
	using CHIMP_TYPE = typename ChimpType<T>::TYPE;

	ChimpState() : chimp() {
	}
	Chimp128CompressionState<CHIMP_TYPE, EMPTY> chimp;

public:
	void AssignDataBuffer(uint8_t *data_out) {
		chimp.output.SetStream(data_out);
	}

	void AssignFlagBuffer(uint8_t *flag_out) {
		chimp.flag_buffer.SetBuffer(flag_out);
	}

	void AssignPackedDataBuffer(uint16_t *packed_data_out) {
		chimp.packed_data_buffer.SetBuffer(packed_data_out);
	}

	void AssignLeadingZeroBuffer(uint8_t *leading_zero_out) {
		chimp.leading_zero_buffer.SetBuffer(leading_zero_out);
	}

	void Flush() {
		chimp.output.Flush();
	}
};

} // namespace duckdb
