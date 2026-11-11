#include "duckdb/common/bignum.hpp"
#include "duckdb/common/types/bignum.hpp"
#include "duckdb/common/exception/conversion_exception.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/types/cast_helpers.hpp"
#include <cmath>
#include <cstring>

namespace duckdb {

void Bignum::ThrowInvalidBlobSize(idx_t size) {
	throw DataCorruptionException("Invalid BIGNUM blob: size %llu is smaller than the minimum of %llu bytes",
	                              static_cast<uint64_t>(size), static_cast<uint64_t>(BIGNUM_HEADER_SIZE + 1));
}

void Bignum::Verify(const bignum_t &input) {
#ifdef DEBUG
	// Size must be >= 4
	idx_t bignum_bytes = input.data.GetSize();
	if (bignum_bytes < 4) {
		throw InternalException("Bignum number of bytes is invalid, current number of bytes is %d", bignum_bytes);
	}
	// Bytes in header must quantify the number of data bytes
	auto bignum_ptr = input.data.GetData();
	bool is_negative = (bignum_ptr[0] & 0x80) == 0;
	uint32_t number_of_bytes = 0;
	if (bignum_bytes == 4 && is_negative) {
		// There is only one invalid value, which is -0
		if (bignum_ptr[3] == static_cast<char>(0xFF)) {
			throw InternalException("Bignum value -0 is not allowed in the Bignum specification.");
		}
	}

	char mask = 0x7F;
	if (is_negative) {
		number_of_bytes |= static_cast<uint32_t>(~bignum_ptr[0] & mask) << 16 & 0xFF0000;
		number_of_bytes |= static_cast<uint32_t>(~bignum_ptr[1]) << 8 & 0xFF00;
		;
		number_of_bytes |= static_cast<uint32_t>(~bignum_ptr[2]) & 0xFF;
	} else {
		number_of_bytes |= static_cast<uint32_t>(bignum_ptr[0] & mask) << 16 & 0xFF0000;
		number_of_bytes |= static_cast<uint32_t>(bignum_ptr[1]) << 8 & 0xFF00;
		number_of_bytes |= static_cast<uint32_t>(bignum_ptr[2]) & 0xFF;
	}
	if (number_of_bytes != bignum_bytes - 3) {
		throw InternalException("The number of bytes set in the Bignum header: %d bytes. Does not "
		                        "match the number of bytes encountered as the bignum data: %d bytes.",
		                        number_of_bytes, bignum_bytes - 3);
	}
	//  No bytes between 4 and end can be 0, unless total size == 4
	if (bignum_bytes > 4) {
		if (is_negative) {
			if (static_cast<data_t>(~bignum_ptr[3]) == 0) {
				throw InternalException("Invalid top data bytes set to 0 for BIGNUM values");
			}
		} else {
			if (bignum_ptr[3] == 0) {
				throw InternalException("Invalid top data bytes set to 0 for BIGNUM values");
			}
		}
	}
#endif
}
void Bignum::SetHeader(char *blob, uint64_t number_of_bytes, bool is_negative) {
	uint32_t header = static_cast<uint32_t>(number_of_bytes);
	// Set MSBit of 3rd byte
	header |= 0x00800000;
	if (is_negative) {
		header = ~header;
	}
	// we ignore MSByte  of header.
	// write the 3 bytes to blob.
	blob[0] = static_cast<char>(header >> 16);
	blob[1] = static_cast<char>(header >> 8 & 0xFF);
	blob[2] = static_cast<char>(header & 0xFF);
}

// Creates a blob representing the value 0
bignum_t Bignum::InitializeBignumZero(Vector &result) {
	uint32_t blob_size = 1 + BIGNUM_HEADER_SIZE;
	auto blob = StringVector::EmptyString(result, blob_size);
	auto writable_blob = blob.GetDataWriteable();
	SetHeader(writable_blob, 1, false);
	writable_blob[3] = 0;
	blob.Finalize();
	const bignum_t result_bignum(blob);
	return result_bignum;
}

string Bignum::InitializeBignumZero() {
	uint32_t blob_size = 1 + BIGNUM_HEADER_SIZE;
	string result(blob_size, '0');
	SetHeader(&result[0], 1, false);
	result[3] = 0;
	return result;
}

int Bignum::CharToDigit(char c) {
	return c - '0';
}

char Bignum::DigitToChar(int digit) {
	// FIXME: this would be the proper solution:
	// return UnsafeNumericCast<char>(digit + '0');
	return static_cast<char>(digit + '0');
}

static bool ShouldRoundDecimalDigits(const char *data, idx_t decimal_start, idx_t decimal_end) {
	// the fraction is at least 0.5 exactly when its first digit is at least 5
	return decimal_start < decimal_end && data[decimal_start] >= '5';
}

//! 10^0 .. 10^19
static constexpr uint64_t BIGNUM_CAST_POWERS_OF_TEN[] = {1ULL,
                                                         10ULL,
                                                         100ULL,
                                                         1000ULL,
                                                         10000ULL,
                                                         100000ULL,
                                                         1000000ULL,
                                                         10000000ULL,
                                                         100000000ULL,
                                                         1000000000ULL,
                                                         10000000000ULL,
                                                         100000000000ULL,
                                                         1000000000000ULL,
                                                         10000000000000ULL,
                                                         100000000000000ULL,
                                                         1000000000000000ULL,
                                                         10000000000000000ULL,
                                                         100000000000000000ULL,
                                                         1000000000000000000ULL,
                                                         10000000000000000000ULL};

//! Parses eight decimal digits at once (Lemire, "Fast parsing of eight digits")
static inline uint64_t BignumCastParseEightDigits(const char *digits) {
	uint64_t value;
	memcpy(&value, digits, sizeof(value));
	// the ASCII offset of every digit, then two digits per 16-bit lane, then four, then eight
	value -= 0x3030303030303030ULL;
	value = (value * 10) + (value >> 8);
	return (((value & 0x000000FF000000FFULL) * (100 + (1000000ULL << 32))) +
	        (((value >> 16) & 0x000000FF000000FFULL) * (1 + (10000ULL << 32)))) >>
	       32;
}

//! Parses count (1 <= count <= 19) decimal digits into an integer
static inline uint64_t BignumCastParseDigits(const char *digits, idx_t count) {
	uint64_t value = 0;
	idx_t i = 0;
#if !DUCKDB_IS_BIG_ENDIAN
	for (; i + 8 <= count; i += 8) {
		value = value * 100000000ULL + BignumCastParseEightDigits(digits + i);
	}
#endif
	for (; i < count; i++) {
		value = value * 10 + static_cast<uint64_t>(digits[i] - '0');
	}
	return value;
}

//! Converts decimal digits to base 2^64 limbs, least significant first, without a zero top limb
static void BignumCastDecimalToBinary(const char *digits, idx_t count, unsafe_vector<uint64_t> &limbs) {
	limbs.clear();
	// the first chunk takes the digits that remain after cutting Bignum::DECIMAL_SHIFT digits at a time from
	// the end
	idx_t chunk_size = count % Bignum::DECIMAL_SHIFT;
	if (chunk_size == 0) {
		chunk_size = Bignum::DECIMAL_SHIFT;
	}
	for (idx_t pos = 0; pos < count; pos += chunk_size, chunk_size = Bignum::DECIMAL_SHIFT) {
		const uint64_t chunk = BignumCastParseDigits(digits + pos, chunk_size);
		// limbs = limbs * 10^chunk_size + chunk
		const uint64_t multiplier = BIGNUM_CAST_POWERS_OF_TEN[chunk_size];
		uint64_t carry = chunk;
		for (auto &limb : limbs) {
			limb = BignumLimb::MultiplyAdd(limb, multiplier, carry, carry);
		}
		if (carry) {
			limbs.push_back(carry);
		}
	}
}

//! Adds one to a magnitude in base 2^64 limbs
static void BignumCastIncrement(unsafe_vector<uint64_t> &limbs) {
	for (auto &limb : limbs) {
		if (++limb != 0) {
			return;
		}
	}
	limbs.push_back(1);
}

//! The number of magnitude bytes of a magnitude in base 2^64 limbs (one for zero)
static idx_t BignumCastMagnitudeBytes(const unsafe_vector<uint64_t> &limbs) {
	if (limbs.empty()) {
		return 1;
	}
	D_ASSERT(limbs.back() != 0);
	return (limbs.size() - 1) * BignumLimb::BYTES + BignumLimb::ByteCount(limbs.back());
}

//! Writes the header and the big-endian magnitude bytes of a BIGNUM blob from base 2^64 limbs
static void BignumCastWriteBlob(char *blob, const unsafe_vector<uint64_t> &limbs, idx_t data_bytes, bool is_negative) {
	Bignum::SetHeader(blob, data_bytes, is_negative);
	auto data = data_ptr_cast(blob + Bignum::BIGNUM_HEADER_SIZE);
	if (limbs.empty()) {
		data[0] = is_negative ? 0xFF : 0;
		return;
	}
	const idx_t top_bytes = data_bytes - (limbs.size() - 1) * BignumLimb::BYTES;
	BignumLimb::StoreMagnitudePartial(data, top_bytes, limbs.back(), is_negative);
	auto end = data + data_bytes;
	for (idx_t i = 0; i + 1 < limbs.size(); i++, end -= BignumLimb::BYTES) {
		BignumLimb::StoreMagnitude(end - BignumLimb::BYTES, limbs[i], is_negative);
	}
}

//! Converts the integer digits of a VARCHAR into base 2^64 limbs, rounded up if requested
static void BignumCastVarcharToLimbs(const string_t &value, idx_t start_pos, idx_t end_pos, bool should_round_up,
                                     unsafe_vector<uint64_t> &limbs) {
	BignumCastDecimalToBinary(value.GetData() + start_pos, end_pos - start_pos, limbs);
	if (should_round_up) {
		BignumCastIncrement(limbs);
	}
}

string Bignum::EncodeVarcharBignum(const string_t &value, idx_t start_pos, idx_t end_pos, bool is_negative,
                                   bool is_zero, bool should_round_up) {
	if (is_zero || (start_pos == end_pos && !should_round_up)) {
		return InitializeBignumZero();
	}
	unsafe_vector<uint64_t> limbs;
	BignumCastVarcharToLimbs(value, start_pos, end_pos, should_round_up, limbs);
	const idx_t data_bytes = BignumCastMagnitudeBytes(limbs);
	string result(BIGNUM_HEADER_SIZE + data_bytes, '\0');
	BignumCastWriteBlob(&result[0], limbs, data_bytes, is_negative);
	return result;
}

bool Bignum::TryVarcharToBignum(const string_t &value, bignum_t &result_value, Vector &result,
                                BignumCastScratch &scratch) {
	idx_t start_pos, end_pos;
	bool is_negative, is_zero, should_round_up;
	if (!VarcharFormatting(value, start_pos, end_pos, is_negative, is_zero, should_round_up)) {
		return false;
	}
	if (is_zero || (start_pos == end_pos && !should_round_up)) {
		result_value = InitializeBignumZero(result);
		return true;
	}
	auto &limbs = scratch.binary;
	BignumCastVarcharToLimbs(value, start_pos, end_pos, should_round_up, limbs);
	const idx_t data_bytes = BignumCastMagnitudeBytes(limbs);
	result_value = bignum_t(StringVector::EmptyString(result, BIGNUM_HEADER_SIZE + data_bytes));
	BignumCastWriteBlob(result_value.data.GetDataWriteable(), limbs, data_bytes, is_negative);
	result_value.data.Finalize();
	return true;
}

bool Bignum::VarcharFormatting(const string_t &value, idx_t &start_pos, idx_t &end_pos, bool &is_negative,
                               bool &is_zero, bool &should_round_up) {
	should_round_up = false;
	// If it's empty we error
	if (value.Empty()) {
		return false;
	}
	start_pos = 0;
	is_zero = false;

	auto int_value_char = value.GetData();
	end_pos = value.GetSize();

	// If first character is -, we have a negative number, if + we have a + number
	is_negative = int_value_char[0] == '-';
	if (is_negative) {
		start_pos++;
	}
	if (int_value_char[0] == '+') {
		start_pos++;
	}
	// Now lets trim 0s
	bool at_least_one_zero = false;
	while (start_pos < end_pos && int_value_char[start_pos] == '0') {
		start_pos++;
		at_least_one_zero = true;
	}
	if (start_pos == end_pos) {
		if (at_least_one_zero) {
			// This is a 0 value
			is_zero = true;
			return true;
		}
		// This is either a '+' or '-'. Hence, invalid.
		return false;
	}
	idx_t cur_pos = start_pos;
	// Verify all is numeric
	while (cur_pos < end_pos && StringUtil::CharacterIsDigit(int_value_char[cur_pos])) {
		cur_pos++;
	}
	if (cur_pos < end_pos) {
		idx_t possible_end = cur_pos;
		// Oh oh, this is not a digit, if it's a . we might be fine, otherwise, this is invalid.
		if (int_value_char[cur_pos] == '.') {
			cur_pos++;
		} else {
			return false;
		}

		// Now cur_pos points to the first digit after the decimal point.
		bool has_digit_after_decimal = false;
		auto decimal_start = cur_pos;
		while (cur_pos < end_pos) {
			if (StringUtil::CharacterIsDigit(int_value_char[cur_pos])) {
				has_digit_after_decimal = true;
				cur_pos++;
			} else {
				// By now we can only have numbers, otherwise this is invalid.
				return false;
			}
		}
		should_round_up = ShouldRoundDecimalDigits(int_value_char, decimal_start, end_pos);
		// No integer digits before the decimal (e.g. ".5", "0.5" after leading zero trim, "0.").
		if (possible_end == start_pos) {
			if (!at_least_one_zero && !has_digit_after_decimal) {
				return false;
			}
			end_pos = possible_end;
			if (should_round_up) {
				is_zero = false;
				return true;
			}
			is_zero = true;
			return true;
		}
		end_pos = possible_end;
	}
	return true;
}

string Bignum::EncodeBignum(const string_t &value, idx_t start_pos, idx_t end_pos, bool is_negative, bool is_zero) {
	return EncodeVarcharBignum(value, start_pos, end_pos, is_negative, is_zero, false);
}

void Bignum::GetByteArray(vector<uint8_t> &byte_array, bool &is_negative, const string_t &blob) {
	if (blob.GetSize() < 4) {
		throw InvalidInputException("Invalid blob size.");
	}
	auto blob_ptr = blob.GetData();

	// Determine if the number is negative
	is_negative = (blob_ptr[0] & 0x80) == 0;
	byte_array.reserve(blob.GetSize() - 3);
	if (is_negative) {
		for (idx_t i = 3; i < blob.GetSize(); i++) {
			byte_array.push_back(static_cast<uint8_t>(~blob_ptr[i]));
		}
	} else {
		for (idx_t i = 3; i < blob.GetSize(); i++) {
			byte_array.push_back(static_cast<uint8_t>(blob_ptr[i]));
		}
	}
}

string Bignum::FromByteArray(uint8_t *data, idx_t size, bool is_negative) {
	string result(BIGNUM_HEADER_SIZE + size, '0');
	SetHeader(&result[0], size, is_negative);
	uint8_t *result_data = reinterpret_cast<uint8_t *>(&result[BIGNUM_HEADER_SIZE]);
	if (is_negative) {
		for (idx_t i = 0; i < size; i++) {
			result_data[i] = static_cast<uint8_t>(~data[i]);
		}
	} else {
		for (idx_t i = 0; i < size; i++) {
			result_data[i] = data[i];
		}
	}
	return result;
}

//! Below this number of decimal limbs, numbers are multiplied with the schoolbook algorithm
static constexpr idx_t KARATSUBA_THRESHOLD = 16;
//! Below this number of binary limbs, numbers are converted to decimal with the schoolbook algorithm
static constexpr idx_t CONVERSION_THRESHOLD = 64;

//! Divides the 128-bit value (high, low), where high < 10^19, by 10^19; the quotient fits 64 bits.
//! Möller and Granlund, "Improved division by invariant integers", Algorithm 4, with the precomputed reciprocal of
//! the (normalized) divisor 10^19.
static inline uint64_t BignumCastDivRemDecimalBase(uint64_t high, uint64_t low, uint64_t &remainder) {
	D_ASSERT(high < Bignum::DECIMAL_BASE);
	//! floor((2^128 - 1) / 10^19) - 2^64
	constexpr uint64_t RECIPROCAL = 0xd83c94fb6d2ac34aULL;
	uint64_t q1;
	uint64_t q0 = BignumLimb::MultiplyAdd(RECIPROCAL, high, 0, q1);
	// (q1, q0) += (high, low)
	q0 += low;
	q1 += high + (q0 < low ? 1 : 0);
	q1 += 1;
	uint64_t r = low - q1 * Bignum::DECIMAL_BASE;
	if (r > q0) {
		q1--;
		r += Bignum::DECIMAL_BASE;
	}
	if (r >= Bignum::DECIMAL_BASE) {
		q1++;
		r -= Bignum::DECIMAL_BASE;
	}
	remainder = r;
	return q1;
}

static void TrimLimbs(unsafe_vector<uint64_t> &limbs) {
	while (!limbs.empty() && limbs.back() == 0) {
		limbs.pop_back();
	}
}

// Adds the decimal limbs of b, shifted by "shift" limbs, to a
static void AddDecimalLimbs(unsafe_vector<uint64_t> &a, const uint64_t *b, idx_t b_size, idx_t shift) {
	while (b_size > 0 && b[b_size - 1] == 0) {
		b_size--;
	}
	if (a.size() < shift + b_size) {
		a.resize(shift + b_size, 0);
	}
	uint64_t carry = 0;
	idx_t i = 0;
	for (; i < b_size; i++) {
		// a + b + carry can exceed 2^64, so compare against the complement instead
		const uint64_t x = a[shift + i];
		const uint64_t y = b[i] + carry;
		carry = x >= Bignum::DECIMAL_BASE - y ? 1 : 0;
		a[shift + i] = carry ? x - (Bignum::DECIMAL_BASE - y) : x + y;
	}
	for (idx_t idx = shift + i; carry; idx++) {
		if (idx == a.size()) {
			a.push_back(0);
		}
		const uint64_t sum = a[idx] + carry;
		carry = sum >= Bignum::DECIMAL_BASE ? 1 : 0;
		a[idx] = carry ? sum - Bignum::DECIMAL_BASE : sum;
	}
}

// Subtracts the decimal limbs of b from a, where a >= b
static void SubtractDecimalLimbs(unsafe_vector<uint64_t> &a, const unsafe_vector<uint64_t> &b) {
	uint64_t borrow = 0;
	idx_t i = 0;
	for (; i < b.size(); i++) {
		if (i >= a.size()) {
			D_ASSERT(b[i] == 0);
			continue;
		}
		const uint64_t subtrahend = b[i] + borrow;
		borrow = a[i] < subtrahend ? 1 : 0;
		a[i] = borrow ? a[i] + (Bignum::DECIMAL_BASE - subtrahend) : a[i] - subtrahend;
	}
	for (; borrow; i++) {
		D_ASSERT(i < a.size());
		borrow = a[i] == 0 ? 1 : 0;
		a[i] = borrow ? Bignum::DECIMAL_BASE - 1 : a[i] - 1;
	}
}

// Multiplies two numbers in decimal limbs using Karatsuba multiplication
static unsafe_vector<uint64_t> MultiplyDecimalLimbs(const uint64_t *a, idx_t a_size, const uint64_t *b, idx_t b_size) {
	if (a_size < b_size) {
		std::swap(a, b);
		std::swap(a_size, b_size);
	}
	unsafe_vector<uint64_t> result;
	if (b_size == 0) {
		return result;
	}
	result.resize(a_size + b_size, 0);
	if (b_size < KARATSUBA_THRESHOLD) {
		for (idx_t i = 0; i < b_size; i++) {
			uint64_t carry = 0;
			for (idx_t j = 0; j < a_size; j++) {
				// (high, low) = b[i] * a[j] + result[i + j] + carry, below 10^38 + 2 * 10^19, so high < 10^19
				uint64_t high;
				uint64_t low = BignumLimb::MultiplyAdd(b[i], a[j], result[i + j], high);
				low += carry;
				high += low < carry ? 1 : 0;
				carry = BignumCastDivRemDecimalBase(high, low, result[i + j]);
			}
			result[i + a_size] = carry;
		}
		TrimLimbs(result);
		return result;
	}
	if (2 * b_size <= a_size) {
		// unbalanced: multiply b with chunks of a
		for (idx_t offset = 0; offset < a_size; offset += b_size) {
			auto part = MultiplyDecimalLimbs(a + offset, MinValue<idx_t>(b_size, a_size - offset), b, b_size);
			AddDecimalLimbs(result, part.data(), part.size(), offset);
		}
		TrimLimbs(result);
		return result;
	}
	// a = a1 * BASE^m + a0, b = b1 * BASE^m + b0
	idx_t m = a_size / 2;
	auto z0 = MultiplyDecimalLimbs(a, m, b, m);
	auto z2 = MultiplyDecimalLimbs(a + m, a_size - m, b + m, b_size - m);
	unsafe_vector<uint64_t> a_sum(a, a + m);
	AddDecimalLimbs(a_sum, a + m, a_size - m, 0);
	unsafe_vector<uint64_t> b_sum(b, b + m);
	AddDecimalLimbs(b_sum, b + m, b_size - m, 0);
	// z1 = (a0 + a1) * (b0 + b1) - z0 - z2
	auto z1 = MultiplyDecimalLimbs(a_sum.data(), a_sum.size(), b_sum.data(), b_sum.size());
	SubtractDecimalLimbs(z1, z0);
	SubtractDecimalLimbs(z1, z2);
	AddDecimalLimbs(result, z0.data(), z0.size(), 0);
	AddDecimalLimbs(result, z1.data(), z1.size(), m);
	AddDecimalLimbs(result, z2.data(), z2.size(), 2 * m);
	TrimLimbs(result);
	return result;
}

// Converts binary limbs (base 2^64) to decimal limbs (base 10^19), both little-endian
// powers[k] holds 2^(64 * 2^k) in decimal limbs
static void BinaryToDecimalLimbs(const uint64_t *binary, idx_t size, vector<unsafe_vector<uint64_t>> &powers,
                                 unsafe_vector<uint64_t> &digits) {
	digits.clear();
	if (size <= CONVERSION_THRESHOLD) {
		// typos:ignore-next-line
		// Following CPython and Knuth (TAOCP, Volume 2 (3rd edn), section 4.4, Method 1b).
		for (idx_t i = size; i > 0; i--) {
			uint64_t hi = binary[i - 1];
			for (idx_t j = 0; j < digits.size(); j++) {
				// digits[j] * 2^64 + hi, divided by 10^19
				hi = BignumCastDivRemDecimalBase(digits[j], hi, digits[j]);
			}
			while (hi) {
				digits.push_back(hi % Bignum::DECIMAL_BASE);
				hi /= Bignum::DECIMAL_BASE;
			}
		}
		return;
	}
	// split the number into high * 2^(64 * m) + low, where m is a power of two
	idx_t k = 0;
	idx_t m = 1;
	while (2 * m < size) {
		m *= 2;
		k++;
	}
	unsafe_vector<uint64_t> low;
	unsafe_vector<uint64_t> high;
	BinaryToDecimalLimbs(binary, m, powers, low);
	BinaryToDecimalLimbs(binary + m, size - m, powers, high);
	if (powers.empty()) {
		// 2^64 in decimal limbs
		powers.push_back({8446744073709551616ULL, 1ULL});
	}
	while (powers.size() <= k) {
		auto &last = powers.back();
		powers.push_back(MultiplyDecimalLimbs(last.data(), last.size(), last.data(), last.size()));
	}
	auto &power = powers[k];
	digits = MultiplyDecimalLimbs(high.data(), high.size(), power.data(), power.size());
	AddDecimalLimbs(digits, low.data(), low.size(), 0);
	TrimLimbs(digits);
}

//! Loads the magnitude of a BIGNUM blob into base 2^64 limbs, least significant first
static void BignumCastLoadMagnitude(const bignum_t &blob, bool &is_negative, unsafe_vector<uint64_t> &limbs) {
	const idx_t blob_size = blob.data.GetSize();
	if (blob_size < Bignum::BIGNUM_HEADER_SIZE + 1) {
		throw InvalidInputException("Invalid blob size.");
	}
	auto data = const_data_ptr_cast(blob.data.GetData());
	is_negative = (data[0] & 0x80) == 0;
	const idx_t data_bytes = blob_size - Bignum::BIGNUM_HEADER_SIZE;
	const idx_t limb_count = (data_bytes + BignumLimb::BYTES - 1) / BignumLimb::BYTES;
	limbs.resize(limb_count);
	auto end = data + blob_size;
	for (idx_t i = 0; i + 1 < limb_count; i++, end -= BignumLimb::BYTES) {
		limbs[i] = BignumLimb::LoadMagnitude(end - BignumLimb::BYTES, is_negative);
	}
	limbs[limb_count - 1] = BignumLimb::LoadMagnitudePartial(
	    data + Bignum::BIGNUM_HEADER_SIZE, data_bytes - (limb_count - 1) * BignumLimb::BYTES, is_negative);
	TrimLimbs(limbs);
}

//! The number of decimal digits of a magnitude in base 10^19 limbs (one for zero)
static idx_t BignumCastDecimalLength(const unsafe_vector<uint64_t> &digits) {
	if (digits.empty()) {
		return 1;
	}
	idx_t top_digits = 1;
	while (top_digits < Bignum::DECIMAL_SHIFT && digits.back() >= BIGNUM_CAST_POWERS_OF_TEN[top_digits]) {
		top_digits++;
	}
	return (digits.size() - 1) * Bignum::DECIMAL_SHIFT + top_digits;
}

//! Writes the decimal representation of a magnitude in base 10^19 limbs, with a sign, ending at "end"
static void BignumCastWriteDecimal(char *end, const unsafe_vector<uint64_t> &digits, idx_t length, bool is_negative) {
	char *start = end - length;
	if (digits.empty()) {
		*start = '0';
	} else {
		for (idx_t i = 0; i + 1 < digits.size(); i++) {
			// every limb below the top one takes exactly DECIMAL_SHIFT digits
			end -= Bignum::DECIMAL_SHIFT;
			char *ptr = NumericHelper::FormatUnsigned<uint64_t>(digits[i], end + Bignum::DECIMAL_SHIFT);
			while (ptr > end) {
				*--ptr = '0';
			}
		}
		char *ptr = NumericHelper::FormatUnsigned<uint64_t>(digits.back(), end);
		D_ASSERT(ptr == start);
		(void)ptr;
	}
	if (is_negative) {
		start[-1] = '-';
	}
}

//! Converts a BIGNUM to decimal limbs, returns the length of its decimal representation including the sign
static idx_t BignumCastToDecimal(const bignum_t &blob, BignumCastScratch &scratch, bool &is_negative) {
	BignumCastLoadMagnitude(blob, is_negative, scratch.binary);
	BinaryToDecimalLimbs(scratch.binary.data(), scratch.binary.size(), scratch.powers, scratch.decimal);
	return BignumCastDecimalLength(scratch.decimal) + (is_negative ? 1 : 0);
}

string Bignum::BignumToVarchar(const bignum_t &blob) {
	BignumCastScratch scratch;
	bool is_negative;
	const idx_t length = BignumCastToDecimal(blob, scratch, is_negative);
	string result(length, '\0');
	BignumCastWriteDecimal(&result[0] + length, scratch.decimal, length - (is_negative ? 1 : 0), is_negative);
	return result;
}

string_t Bignum::BignumToVarchar(const bignum_t &blob, Vector &result, BignumCastScratch &scratch) {
	bool is_negative;
	const idx_t length = BignumCastToDecimal(blob, scratch, is_negative);
	auto target = StringVector::EmptyString(result, length);
	BignumCastWriteDecimal(target.GetDataWriteable() + length, scratch.decimal, length - (is_negative ? 1 : 0),
	                       is_negative);
	target.Finalize();
	return target;
}

string Bignum::VarcharToBignum(const string_t &value) {
	idx_t start_pos, end_pos;
	bool is_negative, is_zero, should_round_up;
	if (!VarcharFormatting(value, start_pos, end_pos, is_negative, is_zero, should_round_up)) {
		throw ConversionException("Could not convert string \'%s\' to Bignum", value.GetString());
	}
	return EncodeVarcharBignum(value, start_pos, end_pos, is_negative, is_zero, should_round_up);
}

//! Returns 2^exponent as a double, for 0 <= exponent <= 1023
static inline double BignumCastPowerOfTwo(idx_t exponent) {
	D_ASSERT(exponent <= 1023);
	const uint64_t bits = static_cast<uint64_t>(exponent + 1023) << 52;
	double result;
	memcpy(&result, &bits, sizeof(result));
	return result;
}

bool Bignum::BignumToDouble(const bignum_t &blob, double &result, bool &strict) {
	result = 0;
	const idx_t blob_size = blob.data.GetSize();
	if (blob_size < BIGNUM_HEADER_SIZE + 1) {
		throw InvalidInputException("Invalid blob size.");
	}
	auto data = const_data_ptr_cast(blob.data.GetData());
	const bool is_negative = (data[0] & 0x80) == 0;
	idx_t data_byte_size = blob_size - BIGNUM_HEADER_SIZE;
	auto first = data + BIGNUM_HEADER_SIZE;

	double magnitude;
	if (data_byte_size <= BignumLimb::BYTES) {
		// the conversion of a 64-bit integer rounds correctly
		magnitude = static_cast<double>(BignumLimb::LoadMagnitudePartial(first, data_byte_size, is_negative));
	} else {
		// a well-formed BIGNUM has no leading zero bytes (0xFF under the complement of a negative value), tolerate them
		const data_t zero = is_negative ? 0xFF : 0;
		while (data_byte_size > BignumLimb::BYTES && *first == zero) {
			first++;
			data_byte_size--;
		}
		// 2^1024 and above do not fit a double, that is at least 129 bytes
		constexpr idx_t MAX_DOUBLE_BYTES = 1024 / 8;
		if (data_byte_size > MAX_DOUBLE_BYTES) {
			return false;
		}
		// round the top 64 bits of the magnitude; the lower bytes only matter when they break a tie, which the sticky
		// bit folded into the lowest of the 64 bits takes care of (the top bits hold at least 57 significant bits, so
		// the sticky bit is always among the bits that the conversion to a 53-bit mantissa discards)
		uint64_t top = BignumLimb::LoadMagnitude(first, is_negative);
		const idx_t remaining_bytes = data_byte_size - BignumLimb::BYTES;
		auto rest = first + BignumLimb::BYTES;
		for (idx_t i = 0; i < remaining_bytes; i++) {
			if (rest[i] != zero) {
				top |= 1;
				break;
			}
		}
		// scaling by a power of two is exact, unless the value exceeds the range of a double
		magnitude = static_cast<double>(top) * BignumCastPowerOfTwo(8 * remaining_bytes);
	}
	result = is_negative ? -magnitude : magnitude;
	// the value does not fit in a double
	return std::isfinite(result);
}

} // namespace duckdb
