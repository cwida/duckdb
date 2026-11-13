#include "duckdb/common/types/variant/variant_binary_decoder.hpp"
#include "utf8proc_wrapper.hpp"

#include <string.h>

static constexpr uint8_t VERSION_MASK = 0xF;
static constexpr uint8_t SORTED_STRINGS_MASK = 0x1;
static constexpr uint8_t SORTED_STRINGS_SHIFT = 4;
static constexpr uint8_t OFFSET_SIZE_MINUS_ONE_MASK = 0x3;
static constexpr uint8_t OFFSET_SIZE_MINUS_ONE_SHIFT = 6;

static constexpr uint8_t BASIC_TYPE_MASK = 0x3;
static constexpr uint8_t VALUE_HEADER_SHIFT = 2;

//! Object and Array header
static constexpr uint8_t FIELD_OFFSET_SIZE_MINUS_ONE_MASK = 0x3;

//! Object header
static constexpr uint8_t FIELD_ID_SIZE_MINUS_ONE_MASK = 0x3;
static constexpr uint8_t FIELD_ID_SIZE_MINUS_ONE_SHIFT = 2;

static constexpr uint8_t OBJECT_IS_LARGE_MASK = 0x1;
static constexpr uint8_t OBJECT_IS_LARGE_SHIFT = 4;

//! Array header
static constexpr uint8_t ARRAY_IS_LARGE_MASK = 0x1;
static constexpr uint8_t ARRAY_IS_LARGE_SHIFT = 2;

namespace duckdb {

namespace {

static idx_t ReadVariableLengthLittleEndian(idx_t length_in_bytes, const_data_ptr_t ptr, idx_t &offset,
                                            const idx_t capacity) {
	if (length_in_bytes > sizeof(idx_t)) {
		throw NotImplementedException("Can't read little-endian value of %d bytes", length_in_bytes);
	}
	if (offset + length_in_bytes > capacity) {
		throw IOException("Data corruption detected, read of length_in_bytes (%d) would exceed buffer capacity",
		                  length_in_bytes);
	}
	idx_t result = 0;
	memcpy(reinterpret_cast<uint8_t *>(&result), ptr + offset, length_in_bytes);
	offset += length_in_bytes;
	return result;
}

} // namespace

VariantMetadataHeader VariantMetadataHeader::FromHeaderByte(uint8_t byte) {
	VariantMetadataHeader header;
	header.version = byte & VERSION_MASK;
	header.sorted_strings = (byte >> SORTED_STRINGS_SHIFT) & SORTED_STRINGS_MASK;
	header.offset_size = ((byte >> OFFSET_SIZE_MINUS_ONE_SHIFT) & OFFSET_SIZE_MINUS_ONE_MASK) + 1;

	if (header.version != 1) {
		throw NotImplementedException("Only version 1 of the Variant encoding scheme is supported, found version: %d",
		                              header.version);
	}

	return header;
}

VariantMetadata::VariantMetadata(const string_t &metadata) : metadata(metadata) {
	auto metadata_data = reinterpret_cast<const_data_ptr_t>(metadata.GetData());
	const auto metadata_buffer_capacity = metadata.GetSize();
	if (!metadata_data || metadata.GetSize() < 1) {
		throw IOException("Corrupted VARIANT 'metadata' buffer, empty or nullptr");
	}

	idx_t metadata_offset = 0;
	header = VariantMetadataHeader::FromHeaderByte(metadata_data[metadata_offset]);
	metadata_offset += sizeof(uint8_t);

	idx_t dictionary_size =
	    ReadVariableLengthLittleEndian(header.offset_size, metadata_data, metadata_offset, metadata_buffer_capacity);

	auto data_start = metadata_offset + ((dictionary_size + 1) * header.offset_size);
	idx_t last_offset =
	    ReadVariableLengthLittleEndian(header.offset_size, metadata_data, metadata_offset, metadata_buffer_capacity);
	for (idx_t i = 0; i < dictionary_size; i++) {
		auto next_offset = ReadVariableLengthLittleEndian(header.offset_size, metadata_data, metadata_offset,
		                                                  metadata_buffer_capacity);
		if (next_offset < last_offset) {
			throw IOException(
			    "Corrupted VARIANT 'metadata' buffer, the next dictionary offset is smaller than the last offset");
		}
		const idx_t string_size = next_offset - last_offset;
		if (data_start + last_offset + string_size > metadata_buffer_capacity) {
			throw IOException("Corrupted VARIANT 'metadata' buffer");
		}
		auto key_data = reinterpret_cast<const char *>(metadata_data + data_start + last_offset);
		if (!Utf8Proc::IsValid(key_data, string_size)) {
			throw IOException("Corrupted VARIANT 'metadata' buffer, a dictionary key isn't valid UTF8");
		}
		strings.emplace_back(key_data, string_size);
		last_offset = next_offset;
	}
	//! header byte + offsets region + string bytes
	total_size = metadata_offset + last_offset;
}

VariantValueMetadata VariantValueMetadata::FromHeaderByte(uint8_t byte) {
	VariantValueMetadata result;
	result.basic_type = VariantBasicTypeFromByte(byte & BASIC_TYPE_MASK);
	uint8_t value_header = byte >> VALUE_HEADER_SHIFT;
	switch (result.basic_type) {
	case VariantBasicType::PRIMITIVE: {
		result.primitive_type = VariantPrimitiveTypeFromByte(value_header);
		break;
	}
	case VariantBasicType::SHORT_STRING: {
		result.string_size = value_header;
		break;
	}
	case VariantBasicType::OBJECT: {
		result.field_offset_size = (value_header & FIELD_OFFSET_SIZE_MINUS_ONE_MASK) + 1;
		result.field_id_size = ((value_header >> FIELD_ID_SIZE_MINUS_ONE_SHIFT) & FIELD_ID_SIZE_MINUS_ONE_MASK) + 1;
		result.is_large = (value_header >> OBJECT_IS_LARGE_SHIFT) & OBJECT_IS_LARGE_MASK;
		break;
	}
	case VariantBasicType::ARRAY: {
		result.field_offset_size = (value_header & FIELD_OFFSET_SIZE_MINUS_ONE_MASK) + 1;
		result.is_large = (value_header >> ARRAY_IS_LARGE_SHIFT) & ARRAY_IS_LARGE_MASK;
		break;
	}
	default:
		throw InternalException("VariantBasicType (%d) not handled", static_cast<uint8_t>(result.basic_type));
	}
	return result;
}

} // namespace duckdb
