//===----------------------------------------------------------------------===//
//                         DuckDB
//
// decode_utils.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/helper.hpp"
#include "duckdb/common/bitpacking.hpp"
#include "resizable_buffer.hpp"

namespace duckdb {

class ParquetDecodeUtils {
	//===--------------------------------------------------------------------===//
	// Bitpacking
	//===--------------------------------------------------------------------===//
private:
	static const uint64_t BITPACK_MASKS[];
	static const uint64_t BITPACK_MASKS_SIZE;
	static const uint8_t BITPACK_DLEN;

	static void CheckWidth(const uint8_t width) {
		if (width >= BITPACK_MASKS_SIZE) {
			throw InvalidInputException("The width (%d) of the bitpacked data exceeds the supported max width (%d), "
			                            "the file might be corrupted.",
			                            width, BITPACK_MASKS_SIZE);
		}
	}

public:
	template <class T>
	static void BitUnpack(ByteBuffer &src, bitpacking_width_t &bitpack_pos, T *dst, idx_t count,
	                      const bitpacking_width_t width) {
		CheckWidth(width);
		if (width > sizeof(T) * BITPACK_DLEN) {
			throw IOException("The width (%d) of the bitpacked data exceeds the maximum width (%d) for "
			                  "the target type, the file might be corrupted.",
			                  width, sizeof(T) * BITPACK_DLEN);
		}
		const auto mask = BITPACK_MASKS[width];
		src.available(count * width / BITPACK_DLEN); // check if buffer has enough space available once
		if (bitpack_pos == 0 && count >= BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE) {
			idx_t remainder = count % BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE;
			idx_t aligned_count = count - remainder;
			BitUnpackAlignedInternal(src, dst, aligned_count, width);
			dst += aligned_count;
			count = remainder;
		}
		for (idx_t i = 0; i < count; i++) {
			auto val = (src.unsafe_get<uint8_t>() >> bitpack_pos) & mask;
			bitpack_pos += width;
			while (bitpack_pos > BITPACK_DLEN) {
				src.unsafe_inc(1);
				val |= (static_cast<T>(src.unsafe_get<uint8_t>())
				        << static_cast<T>(BITPACK_DLEN - (bitpack_pos - width))) &
				       mask;
				bitpack_pos -= BITPACK_DLEN;
			}
			dst[i] = val;
		}
	}

	static void Skip(ByteBuffer &src, bitpacking_width_t &bitpack_pos, idx_t count, const bitpacking_width_t width) {
		CheckWidth(width);
		src.available(count * width / BITPACK_DLEN); // check if buffer has enough space available once
		if (bitpack_pos == 0 && count >= BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE) {
			idx_t remainder = count % BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE;
			idx_t aligned_count = count - remainder;
			SkipAligned(src, aligned_count, width);
			count = remainder;
		}
		const idx_t total_bits = bitpack_pos + count * width;
		if (total_bits <= BITPACK_DLEN) {
			bitpack_pos = UnsafeNumericCast<bitpacking_width_t>(total_bits);
		} else {
			const idx_t bytes_to_advance = (total_bits - 1) / BITPACK_DLEN;
			src.unsafe_inc(bytes_to_advance);
			bitpack_pos = UnsafeNumericCast<bitpacking_width_t>(total_bits - bytes_to_advance * BITPACK_DLEN);
		}
	}

	template <class T>
	static void BitPackAligned(T *src, data_ptr_t dst, const idx_t count, const bitpacking_width_t width) {
		D_ASSERT(width < BITPACK_MASKS_SIZE);
		D_ASSERT(count % BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE == 0);
		BitpackingPrimitives::PackBuffer<T, true>(dst, src, count, width);
	}

	template <class T>
	static void BitUnpackAlignedInternal(ByteBuffer &src, T *dst, const idx_t count, const bitpacking_width_t width) {
		D_ASSERT(count % BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE == 0);
		if (width > sizeof(T) * BITPACK_DLEN) {
			throw IOException("The width (%d) of the bitpacked data exceeds the maximum width (%d) for "
			                  "the target type, the file might be corrupted.",
			                  width, sizeof(T) * BITPACK_DLEN);
		}

		if (cast_pointer_to_uint64(src.ptr) % sizeof(T) == 0) {
			// Fast path: aligned
			BitpackingPrimitives::UnPackBuffer<T>(data_ptr_cast(dst), src.ptr, count, width);
			src.unsafe_inc(count * width / BITPACK_DLEN);
			return;
		}

		for (idx_t i = 0; i < count; i += BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE) {
			const auto next_read = BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE * width / BITPACK_DLEN;

			// Buffer for alignment
			T aligned_data[BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE];

			// Copy over to aligned buffer
			memcpy(aligned_data, src.ptr, next_read);

			// Unpack
			BitpackingPrimitives::UnPackBlock<T>(data_ptr_cast(dst), data_ptr_cast(aligned_data), width, true);

			src.unsafe_inc(next_read);
			dst += BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE;
		}
	}

	template <class T>
	static void BitUnpackAligned(ByteBuffer &src, T *dst, const idx_t count, const bitpacking_width_t width) {
		CheckWidth(width);
		if (count % BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE != 0) {
			throw InvalidInputException("Aligned bitpacking count must be a multiple of %llu",
			                            BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE);
		}
		const auto read_size = count * width / BITPACK_DLEN;
		src.available(read_size); // check if buffer has enough space available once
		BitUnpackAlignedInternal(src, dst, count, width);
	}

	static void SkipAligned(ByteBuffer &src, const idx_t count, const bitpacking_width_t width) {
		CheckWidth(width);
		if (count % BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE != 0) {
			throw InvalidInputException("Aligned bitpacking count must be a multiple of %llu",
			                            BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE);
		}
		const auto read_size = count * width / BITPACK_DLEN;
		src.inc(read_size);
	}

	//===--------------------------------------------------------------------===//
	// Zigzag
	//===--------------------------------------------------------------------===//
private:
	//! https://lemire.me/blog/2022/11/25/making-all-your-integers-positive-with-zigzag-encoding/
	template <class UNSIGNED>
	static typename std::enable_if<std::is_unsigned<UNSIGNED>::value, typename std::make_signed<UNSIGNED>::type>::type
	ZigzagToIntInternal(UNSIGNED x) {
		return (x >> 1) ^ (-(x & 1));
	}

	template <typename SIGNED>
	static typename std::enable_if<std::is_signed<SIGNED>::value, typename std::make_unsigned<SIGNED>::type>::type
	IntToZigzagInternal(SIGNED x) {
		using UNSIGNED = typename std::make_unsigned<SIGNED>::type;
		return (static_cast<UNSIGNED>(x) << 1) ^ static_cast<UNSIGNED>(x >> (sizeof(SIGNED) * 8 - 1));
	}

public:
	template <class UNSIGNED>
	static typename std::enable_if<std::is_unsigned<UNSIGNED>::value, typename std::make_signed<UNSIGNED>::type>::type
	ZigzagToInt(UNSIGNED x) {
		auto integer = ZigzagToIntInternal(x);
		D_ASSERT(x == IntToZigzagInternal(integer)); // test roundtrip
		return integer;
	}

	template <typename SIGNED>
	static typename std::enable_if<std::is_signed<SIGNED>::value, typename std::make_unsigned<SIGNED>::type>::type
	IntToZigzag(SIGNED x) {
		auto zigzag = IntToZigzagInternal(x);
		D_ASSERT(x == ZigzagToIntInternal(zigzag)); // test roundtrip
		return zigzag;
	}

	//===--------------------------------------------------------------------===//
	// Varint
	//===--------------------------------------------------------------------===//
public:
	template <class T>
	static uint8_t GetVarintSize(T val) {
		uint8_t res = 0;
		do {
			val >>= 7;
			res++;
		} while (val != 0);
		return res;
	}

	template <class T>
	static void VarintEncode(T val, WriteStream &ser) {
		do {
			uint8_t byte = val & 127;
			val >>= 7;
			if (val != 0) {
				byte |= 128;
			}
			ser.Write<uint8_t>(byte);
		} while (val != 0);
	}

	template <class T, bool CHECKED = true>
	static T VarintDecode(ByteBuffer &buf) {
		T result = 0;
		uint8_t shift = 0;
		while (true) {
			uint8_t byte;
			if (CHECKED) {
				byte = buf.read<uint8_t>();
			} else {
				byte = buf.unsafe_read<uint8_t>();
			}
			result |= T(byte & 127) << shift;
			if ((byte & 128) == 0) {
				break;
			}
			shift += 7;
			if (shift > sizeof(T) * 8) {
				throw std::runtime_error("Varint-decoding found too large number");
			}
		}
		return result;
	}
};
} // namespace duckdb
