#include "duckdb/common/types/uuid.hpp"
#include "duckdb/common/chrono.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/random_engine.hpp"

namespace duckdb {

//////////////////
// Base UUID
//////////////////
bool BaseUUID::FromString(string_t str, hugeint_t &result, bool strict) {
	auto hex2char = [](char ch) -> unsigned char {
		if (ch >= '0' && ch <= '9') {
			return UnsafeNumericCast<unsigned char>(ch - '0');
		}
		if (ch >= 'a' && ch <= 'f') {
			return UnsafeNumericCast<unsigned char>(10 + ch - 'a');
		}
		if (ch >= 'A' && ch <= 'F') {
			return UnsafeNumericCast<unsigned char>(10 + ch - 'A');
		}
		return 0;
	};
	auto is_hex = [](char ch) -> bool {
		return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
	};

	const auto data = str.GetData();
	const auto size = str.GetSize();
	if (size == 0) {
		return false;
	}
	idx_t has_braces = 0;
	if (data[0] == '{') {
		has_braces = 1;
	}
	if (has_braces && data[size - 1] != '}') {
		return false;
	}

	if (strict) {
		// 32 characters and 4 hyphens
		if (size != 36) {
			return false;
		}
		if (data[8] != '-' || data[13] != '-' || data[18] != '-' || data[23] != '-') {
			return false;
		}
	}

	result.lower = 0;
	result.upper = 0;
	size_t count = 0;
	for (size_t i = has_braces; i < size - has_braces; ++i) {
		if (data[i] == '-') {
			continue;
		}
		if (count >= 32 || !is_hex(data[i])) {
			return false;
		}
		if (count >= 16) {
			result.lower = (result.lower << 4) | hex2char(data[i]);
		} else {
			result.upper = (result.upper << 4) | hex2char(data[i]);
		}
		count++;
	}
	// Flip the first bit to make `order by uuid` same as `order by uuid::varchar`
	result.upper ^= NumericLimits<int64_t>::Minimum();
	return count == 32;
}

void BaseUUID::ToString(hugeint_t input, char *buf) {
	auto byte_to_hex = [](uint64_t byte_val, char *buf, idx_t &pos) {
		D_ASSERT(byte_val <= 0xFF);
		static char const HEX_DIGITS[] = "0123456789abcdef";
		buf[pos++] = HEX_DIGITS[(byte_val >> 4) & 0xf];
		buf[pos++] = HEX_DIGITS[byte_val & 0xf];
	};

	// Flip back before convert to string
	int64_t upper = int64_t(uint64_t(input.upper) ^ (uint64_t(1) << 63));
	idx_t pos = 0;
	byte_to_hex(upper >> 56 & 0xFF, buf, pos);
	byte_to_hex(upper >> 48 & 0xFF, buf, pos);
	byte_to_hex(upper >> 40 & 0xFF, buf, pos);
	byte_to_hex(upper >> 32 & 0xFF, buf, pos);
	buf[pos++] = '-';
	byte_to_hex(upper >> 24 & 0xFF, buf, pos);
	byte_to_hex(upper >> 16 & 0xFF, buf, pos);
	buf[pos++] = '-';
	byte_to_hex(upper >> 8 & 0xFF, buf, pos);
	byte_to_hex(upper & 0xFF, buf, pos);
	buf[pos++] = '-';
	byte_to_hex(input.lower >> 56 & 0xFF, buf, pos);
	byte_to_hex(input.lower >> 48 & 0xFF, buf, pos);
	buf[pos++] = '-';
	byte_to_hex(input.lower >> 40 & 0xFF, buf, pos);
	byte_to_hex(input.lower >> 32 & 0xFF, buf, pos);
	byte_to_hex(input.lower >> 24 & 0xFF, buf, pos);
	byte_to_hex(input.lower >> 16 & 0xFF, buf, pos);
	byte_to_hex(input.lower >> 8 & 0xFF, buf, pos);
	byte_to_hex(input.lower & 0xFF, buf, pos);
}

hugeint_t BaseUUID::FromUHugeint(uhugeint_t input) {
	hugeint_t result;
	result.lower = input.lower;
	if (input.upper > uint64_t(NumericLimits<int64_t>::Maximum())) {
		result.upper = int64_t(input.upper - uint64_t(NumericLimits<int64_t>::Maximum()) - 1);
	} else {
		result.upper = int64_t(input.upper) - NumericLimits<int64_t>::Maximum() - 1;
	}
	return result;
}

uhugeint_t BaseUUID::ToUHugeint(hugeint_t input) {
	uhugeint_t result;
	result.lower = input.lower;
	if (input.upper >= 0) {
		result.upper = uint64_t(input.upper) + uint64_t(NumericLimits<int64_t>::Maximum()) + 1;
	} else {
		result.upper = uint64_t(input.upper + NumericLimits<int64_t>::Maximum() + 1);
	}
	return result;
}

hugeint_t BaseUUID::Convert(const std::array<uint8_t, 16> &bytes) {
	hugeint_t result;
	result.upper = 0;
	result.upper |= ((int64_t)bytes[0] << 56);
	result.upper |= ((int64_t)bytes[1] << 48);
	result.upper |= ((int64_t)bytes[2] << 40);
	result.upper |= ((int64_t)bytes[3] << 32);
	result.upper |= ((int64_t)bytes[4] << 24);
	result.upper |= ((int64_t)bytes[5] << 16);
	result.upper |= ((int64_t)bytes[6] << 8);
	result.upper |= (int64_t)bytes[7];
	result.lower = 0;
	result.lower |= ((uint64_t)bytes[8] << 56);
	result.lower |= ((uint64_t)bytes[9] << 48);
	result.lower |= ((uint64_t)bytes[10] << 40);
	result.lower |= ((uint64_t)bytes[11] << 32);
	result.lower |= ((uint64_t)bytes[12] << 24);
	result.lower |= ((uint64_t)bytes[13] << 16);
	result.lower |= ((uint64_t)bytes[14] << 8);
	result.lower |= (uint64_t)bytes[15];
	return result;
}

//////////////////
// UUIDv4
//////////////////
hugeint_t UUIDv4::GenerateRandomUUID(RandomEngine &engine) {
	std::array<uint8_t, 16> bytes;
	for (int i = 0; i < 16; i += 4) {
		Store<uint32_t>(engine.NextRandomInteger(), bytes.data() + i);
	}
	// variant must be 10xxxxxx
	bytes[8] &= 0xBF;
	bytes[8] |= 0x80;
	// version must be 0100xxxx
	bytes[6] &= 0x4F;
	bytes[6] |= 0x40;

	return Convert(bytes);
}

hugeint_t UUIDv4::GenerateRandomUUID() {
	RandomEngine engine;
	return GenerateRandomUUID(engine);
}

//////////////////
// UUIDv7
//////////////////
hugeint_t UUIDv7::GenerateRandomUUID(RandomEngine &engine) {
	std::array<uint8_t, 16> bytes; // Intentionally no initialization.

	const auto now = std::chrono::system_clock::now();
	const auto time_ns = std::chrono::time_point_cast<std::chrono::nanoseconds>(now);
	const auto unix_ts_ns = static_cast<uint64_t>(time_ns.time_since_epoch().count());

	// Begins with a 48 bit big-endian Unix Epoch timestamp with millisecond granularity.
	static constexpr uint64_t kNanoToMilli = 1000000;
	const uint64_t unix_ts_ms = unix_ts_ns / kNanoToMilli;
	bytes[0] = static_cast<uint8_t>(unix_ts_ms >> 40);
	bytes[1] = static_cast<uint8_t>(unix_ts_ms >> 32);
	bytes[2] = static_cast<uint8_t>(unix_ts_ms >> 24);
	bytes[3] = static_cast<uint8_t>(unix_ts_ms >> 16);
	bytes[4] = static_cast<uint8_t>(unix_ts_ms >> 8);
	bytes[5] = static_cast<uint8_t>(unix_ts_ms);

	// Fill in random bits.
	const uint32_t random_a = engine.NextRandomInteger();
	const uint32_t random_b = engine.NextRandomInteger();
	const uint32_t random_c = engine.NextRandomInteger();
	bytes[6] = static_cast<uint8_t>(random_a >> 24);
	bytes[7] = static_cast<uint8_t>(random_a >> 16);
	bytes[8] = static_cast<uint8_t>(random_a >> 8);
	bytes[9] = static_cast<uint8_t>(random_a);
	bytes[10] = static_cast<uint8_t>(random_b >> 24);
	bytes[11] = static_cast<uint8_t>(random_b >> 16);
	bytes[12] = static_cast<uint8_t>(random_b >> 8);
	bytes[13] = static_cast<uint8_t>(random_b);
	bytes[14] = static_cast<uint8_t>(random_c >> 24);
	bytes[15] = static_cast<uint8_t>(random_c >> 16);

	// Fill in version number.
	constexpr uint8_t kVersion = 7;
	bytes[6] = (bytes[6] & 0x0f) | (kVersion << 4);

	// Fill in variant field.
	bytes[8] = (bytes[8] & 0x3f) | 0x80;

	// Flip the top byte
	auto result = Convert(bytes);
	result.upper ^= NumericLimits<int64_t>::Minimum();
	return result;
}

hugeint_t UUIDv7::GenerateRandomUUID() {
	RandomEngine engine;
	return GenerateRandomUUID(engine);
}

hugeint_t BaseUUID::FromBlob(const_data_ptr_t input) {
	// Based on UUIDValueConversion::ReadParquetUUID from parquet extension
	hugeint_t result;
	result.lower = 0;
	uint64_t unsigned_upper = 0;
	for (idx_t i = 0; i < sizeof(uint64_t); i++) {
		unsigned_upper <<= 8;
		unsigned_upper += static_cast<uint64_t>(input[i]);
	}
	for (idx_t i = sizeof(uint64_t); i < sizeof(hugeint_t); i++) {
		result.lower <<= 8;
		result.lower += static_cast<uint64_t>(input[i]);
	}
	result.upper = static_cast<int64_t>(unsigned_upper ^ (uint64_t(1) << 63));
	return result;
}

void BaseUUID::ToBlob(hugeint_t input, data_ptr_t output) {
	// Based on ParquetUUIDOperator::Operation from parquet extension
	uint64_t high_bytes = static_cast<uint64_t>(input.upper) ^ (uint64_t(1) << 63);
	uint64_t low_bytes = input.lower;
	for (idx_t i = 0; i < sizeof(uint64_t); i++) {
		auto shift_count = (sizeof(uint64_t) - i - 1) * 8;
		output[i] = static_cast<uint8_t>((high_bytes >> shift_count) & 0xFF);
	}
	for (idx_t i = 0; i < sizeof(uint64_t); i++) {
		auto shift_count = (sizeof(uint64_t) - i - 1) * 8;
		output[sizeof(uint64_t) + i] = static_cast<uint8_t>((low_bytes >> shift_count) & 0xFF);
	}
}

} // namespace duckdb
