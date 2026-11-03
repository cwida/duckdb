#include "duckdb/common/bitpacking.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Unpacking
//===--------------------------------------------------------------------===//

static void UnpackSingle(const uint32_t *__restrict &in, uhugeint_t *__restrict out, uint16_t delta, uint16_t shr) {
	if (delta + shr < 32) {
		*out = ((static_cast<uhugeint_t>(in[0])) >> shr) & ((uhugeint_t(1) << delta) - 1);
	}

	else if (delta + shr >= 32 && delta + shr < 64) {
		*out = static_cast<uhugeint_t>(in[0]) >> shr;
		++in;

		if (delta + shr > 32) {
			const uint16_t NEXT_SHR = shr + delta - 32;
			*out |= static_cast<uhugeint_t>((*in) % (1U << NEXT_SHR)) << (32 - shr);
		}
	}

	else if (delta + shr >= 64 && delta + shr < 96) {
		*out = static_cast<uhugeint_t>(in[0]) >> shr;
		*out |= static_cast<uhugeint_t>(in[1]) << (32 - shr);
		in += 2;

		if (delta + shr > 64) {
			const uint16_t NEXT_SHR = delta + shr - 64;
			*out |= static_cast<uhugeint_t>((*in) % (1U << NEXT_SHR)) << (64 - shr);
		}
	}

	else if (delta + shr >= 96 && delta + shr < 128) {
		*out = static_cast<uhugeint_t>(in[0]) >> shr;
		*out |= static_cast<uhugeint_t>(in[1]) << (32 - shr);
		*out |= static_cast<uhugeint_t>(in[2]) << (64 - shr);
		in += 3;

		if (delta + shr > 96) {
			const uint16_t NEXT_SHR = delta + shr - 96;
			*out |= static_cast<uhugeint_t>((*in) % (1U << NEXT_SHR)) << (96 - shr);
		}
	}

	else if (delta + shr >= 128) {
		*out = static_cast<uhugeint_t>(in[0]) >> shr;
		*out |= static_cast<uhugeint_t>(in[1]) << (32 - shr);
		*out |= static_cast<uhugeint_t>(in[2]) << (64 - shr);
		*out |= static_cast<uhugeint_t>(in[3]) << (96 - shr);
		in += 4;

		if (delta + shr > 128) {
			const uint16_t NEXT_SHR = delta + shr - 128;
			*out |= static_cast<uhugeint_t>((*in) % (1U << NEXT_SHR)) << (128 - shr);
		}
	}
}

static void UnpackLast(const uint32_t *__restrict &in, uhugeint_t *__restrict out, uint16_t delta) {
	const uint8_t LAST_IDX = 31;
	const uint16_t SHIFT = (delta * 31) % 32;
	out[LAST_IDX] = in[0] >> SHIFT;
	if (delta > 32) {
		out[LAST_IDX] |= static_cast<uhugeint_t>(in[1]) << (32 - SHIFT);
	}
	if (delta > 64) {
		out[LAST_IDX] |= static_cast<uhugeint_t>(in[2]) << (64 - SHIFT);
	}
	if (delta > 96) {
		out[LAST_IDX] |= static_cast<uhugeint_t>(in[3]) << (96 - SHIFT);
	}
}

// Unpacks for specific deltas
static void UnpackDelta0(const uint32_t *__restrict in, uhugeint_t *__restrict out) {
	for (uint8_t i = 0; i < 32; ++i) {
		out[i] = 0;
	}
}

static void UnpackDelta32(const uint32_t *__restrict in, uhugeint_t *__restrict out) {
	for (uint8_t k = 0; k < 32; ++k) {
		out[k] = static_cast<uhugeint_t>(in[k]);
	}
}

static void UnpackDelta64(const uint32_t *__restrict in, uhugeint_t *__restrict out) {
	for (uint8_t i = 0; i < 32; ++i) {
		const uint8_t OFFSET = i * 2;
		out[i] = in[OFFSET];
		out[i] |= static_cast<uhugeint_t>(in[OFFSET + 1]) << 32;
	}
}

static void UnpackDelta96(const uint32_t *__restrict in, uhugeint_t *__restrict out) {
	for (uint8_t i = 0; i < 32; ++i) {
		const uint8_t OFFSET = i * 3;
		out[i] = in[OFFSET];
		out[i] |= static_cast<uhugeint_t>(in[OFFSET + 1]) << 32;
		out[i] |= static_cast<uhugeint_t>(in[OFFSET + 2]) << 64;
	}
}

static void UnpackDelta128(const uint32_t *__restrict in, uhugeint_t *__restrict out) {
	for (uint8_t i = 0; i < 32; ++i) {
		const uint8_t OFFSET = i * 4;
		out[i] = in[OFFSET];
		out[i] |= static_cast<uhugeint_t>(in[OFFSET + 1]) << 32;
		out[i] |= static_cast<uhugeint_t>(in[OFFSET + 2]) << 64;
		out[i] |= static_cast<uhugeint_t>(in[OFFSET + 3]) << 96;
	}
}

//===--------------------------------------------------------------------===//
// Packing
//===--------------------------------------------------------------------===//

static void PackSingle(const uhugeint_t in, uint32_t *__restrict &out, uint16_t delta, uint16_t shl, uhugeint_t mask) {
	if (delta + shl < 32) {
		if (shl == 0) {
			out[0] = static_cast<uint32_t>(in & mask);
		} else {
			out[0] |= static_cast<uint32_t>((in & mask) << shl);
		}

	} else if (delta + shl >= 32 && delta + shl < 64) {
		if (shl == 0) {
			out[0] = static_cast<uint32_t>(in & mask);
		} else {
			out[0] |= static_cast<uint32_t>((in & mask) << shl);
		}
		++out;

		if (delta + shl > 32) {
			*out = static_cast<uint32_t>((in & mask) >> (32 - shl));
		}
	}

	else if (delta + shl >= 64 && delta + shl < 96) {
		if (shl == 0) {
			out[0] = static_cast<uint32_t>(in & mask);
		} else {
			out[0] |= static_cast<uint32_t>(in << shl);
		}

		out[1] = static_cast<uint32_t>((in & mask) >> (32 - shl));
		out += 2;

		if (delta + shl > 64) {
			*out = static_cast<uint32_t>((in & mask) >> (64 - shl));
		}
	}

	else if (delta + shl >= 96 && delta + shl < 128) {
		if (shl == 0) {
			out[0] = static_cast<uint32_t>(in & mask);
		} else {
			out[0] |= static_cast<uint32_t>(in << shl);
		}

		out[1] = static_cast<uint32_t>((in & mask) >> (32 - shl));
		out[2] = static_cast<uint32_t>((in & mask) >> (64 - shl));
		out += 3;

		if (delta + shl > 96) {
			*out = static_cast<uint32_t>((in & mask) >> (96 - shl));
		}
	}

	else if (delta + shl >= 128) {
		// shl == 0 won't ever happen here considering a delta of 128 calls PackDelta128
		out[0] |= static_cast<uint32_t>(in << shl);
		out[1] = static_cast<uint32_t>((in & mask) >> (32 - shl));
		out[2] = static_cast<uint32_t>((in & mask) >> (64 - shl));
		out[3] = static_cast<uint32_t>((in & mask) >> (96 - shl));
		out += 4;

		if (delta + shl > 128) {
			*out = static_cast<uint32_t>((in & mask) >> (128 - shl));
		}
	}
}

static void PackLast(const uhugeint_t *__restrict in, uint32_t *__restrict out, uint16_t delta) {
	const uint8_t LAST_IDX = 31;
	const uint16_t SHIFT = (delta * 31) % 32;
	out[0] |= static_cast<uint32_t>(in[LAST_IDX] << SHIFT);
	if (delta > 32) {
		out[1] = static_cast<uint32_t>(in[LAST_IDX] >> (32 - SHIFT));
	}
	if (delta > 64) {
		out[2] = static_cast<uint32_t>(in[LAST_IDX] >> (64 - SHIFT));
	}
	if (delta > 96) {
		out[3] = static_cast<uint32_t>(in[LAST_IDX] >> (96 - SHIFT));
	}
}

// Packs for specific deltas
static void PackDelta32(const uhugeint_t *__restrict in, uint32_t *__restrict out) {
	for (uint8_t i = 0; i < 32; ++i) {
		out[i] = static_cast<uint32_t>(in[i]);
	}
}

static void PackDelta64(const uhugeint_t *__restrict in, uint32_t *__restrict out) {
	for (uint8_t i = 0; i < 32; ++i) {
		const uint8_t OFFSET = 2 * i;
		out[OFFSET] = static_cast<uint32_t>(in[i]);
		out[OFFSET + 1] = static_cast<uint32_t>(in[i] >> 32);
	}
}

static void PackDelta96(const uhugeint_t *__restrict in, uint32_t *__restrict out) {
	for (uint8_t i = 0; i < 32; ++i) {
		const uint8_t OFFSET = 3 * i;
		out[OFFSET] = static_cast<uint32_t>(in[i]);
		out[OFFSET + 1] = static_cast<uint32_t>(in[i] >> 32);
		out[OFFSET + 2] = static_cast<uint32_t>(in[i] >> 64);
	}
}

static void PackDelta128(const uhugeint_t *__restrict in, uint32_t *__restrict out) {
	for (uint8_t i = 0; i < 32; ++i) {
		const uint8_t OFFSET = 4 * i;
		out[OFFSET] = static_cast<uint32_t>(in[i]);
		out[OFFSET + 1] = static_cast<uint32_t>(in[i] >> 32);
		out[OFFSET + 2] = static_cast<uint32_t>(in[i] >> 64);
		out[OFFSET + 3] = static_cast<uint32_t>(in[i] >> 96);
	}
}

//===--------------------------------------------------------------------===//
// HugeIntPacker
//===--------------------------------------------------------------------===//

void HugeIntPacker::Pack(const uhugeint_t *__restrict in, uint32_t *__restrict out, bitpacking_width_t width) {
	D_ASSERT(width <= 128);
	switch (width) {
	case 0:
		break;
	case 32:
		PackDelta32(in, out);
		break;
	case 64:
		PackDelta64(in, out);
		break;
	case 96:
		PackDelta96(in, out);
		break;
	case 128:
		PackDelta128(in, out);
		break;
	default:
		for (idx_t oindex = 0; oindex < BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE - 1; ++oindex) {
			PackSingle(in[oindex], out, width, (width * oindex) % BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE,
			           (uhugeint_t(1) << width) - 1);
		}
		PackLast(in, out, width);
	}
}

void HugeIntPacker::Unpack(const uint32_t *__restrict in, uhugeint_t *__restrict out, bitpacking_width_t width) {
	D_ASSERT(width <= 128);
	switch (width) {
	case 0:
		UnpackDelta0(in, out);
		break;
	case 32:
		UnpackDelta32(in, out);
		break;
	case 64:
		UnpackDelta64(in, out);
		break;
	case 96:
		UnpackDelta96(in, out);
		break;
	case 128:
		UnpackDelta128(in, out);
		break;
	default:
		const uhugeint_t mask = (uhugeint_t(1) << width) - 1;
		uint32_t bit_pos = 0;
		for (idx_t oindex = 0; oindex < BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE - 1;
		     ++oindex, bit_pos += width) {
			const uint32_t idx = bit_pos >> 5;
			const uint16_t shr = static_cast<uint16_t>(bit_pos & 31U);
			const uint16_t span = width + shr;
			uhugeint_t val = static_cast<uhugeint_t>(in[idx]) >> shr;
			if (span > 32) {
				val |= static_cast<uhugeint_t>(in[idx + 1]) << (32 - shr);
			}
			if (span > 64) {
				val |= static_cast<uhugeint_t>(in[idx + 2]) << (64 - shr);
			}
			if (span > 96) {
				val |= static_cast<uhugeint_t>(in[idx + 3]) << (96 - shr);
			}
			if (span > 128) {
				val |= static_cast<uhugeint_t>(in[idx + 4]) << (128 - shr);
			}
			out[oindex] = val & mask;
		}
		in += (bit_pos >> 5);
		UnpackLast(in, out, width);
	}
}

} // namespace duckdb
