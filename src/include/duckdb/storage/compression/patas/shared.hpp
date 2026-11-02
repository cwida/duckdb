#pragma once

namespace duckdb {

class PatasPrimitives {
public:
	using METADATA_POINTER_TYPE = uint32_t;
	using GROUP_OFFSET_TYPE = uint32_t;
	using PACKED_DATA_TYPE = uint16_t;

	static constexpr uint32_t PATAS_GROUP_SIZE = 1024;
	static constexpr uint8_t HEADER_SIZE = sizeof(METADATA_POINTER_TYPE);
	static constexpr uint8_t GROUP_OFFSET_SIZE = sizeof(GROUP_OFFSET_TYPE);
	static constexpr uint8_t PACKED_DATA_SIZE = sizeof(PACKED_DATA_TYPE);
	static constexpr uint8_t BYTECOUNT_BITSIZE = 3;
	static constexpr uint8_t INDEX_BITSIZE = 7;
};

} // namespace duckdb
