#include "duckdb/common/bignum.hpp"
#include "duckdb/common/types/bignum.hpp"
#include "duckdb/common/exception/conversion_exception.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/typedefs.hpp"
#include <cmath>

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

static void IncrementDecimalString(string &digits) {
	int carry = 1;
	for (int64_t i = static_cast<int64_t>(digits.size()) - 1; i >= 0 && carry; i--) {
		auto digit = static_cast<int>(digits[static_cast<idx_t>(i)] - '0') + carry;
		digits[static_cast<idx_t>(i)] = static_cast<char>('0' + (digit % 10));
		carry = digit / 10;
	}
	if (carry) {
		digits = "1" + digits;
	}
}

string Bignum::EncodeVarcharBignum(const string_t &value, idx_t start_pos, idx_t end_pos, bool is_negative,
                                   bool is_zero, bool should_round_up) {
	if (start_pos == end_pos && !should_round_up) {
		is_zero = true;
	}
	if (should_round_up) {
		string integer_digits(value.GetData() + start_pos, end_pos - start_pos);
		IncrementDecimalString(integer_digits);
		is_zero = false;
		string_t rounded_digits(integer_digits);
		return EncodeBignum(rounded_digits, 0, integer_digits.size(), is_negative, is_zero);
	}
	return EncodeBignum(value, start_pos, end_pos, is_negative, is_zero);
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
	if (is_zero) {
		// Return Value 0
		return InitializeBignumZero();
	}
	auto int_value_char = value.GetData();
	idx_t actual_size = end_pos - start_pos;

	// convert the decimal digits to base 2**32 digits (least significant first), DECIMAL_SHIFT digits at a time
	vector<digit_t> digits;
	digits.reserve(actual_size / DECIMAL_SHIFT + 1);
	idx_t chunk_size = actual_size % DECIMAL_SHIFT;
	if (chunk_size == 0) {
		chunk_size = DECIMAL_SHIFT;
	}
	digit_t chunk_base = 1;
	for (idx_t i = 0; i < chunk_size; i++) {
		chunk_base *= 10;
	}
	for (idx_t pos = start_pos; pos < end_pos; pos += chunk_size, chunk_size = DECIMAL_SHIFT) {
		digit_t carry = 0;
		for (idx_t i = pos; i < pos + chunk_size; i++) {
			carry = carry * 10 + static_cast<digit_t>(int_value_char[i] - '0');
		}
		// digits = digits * 10**chunk_size + chunk
		for (auto &digit : digits) {
			twodigit_t tmp = static_cast<twodigit_t>(digit) * chunk_base + carry;
			digit = static_cast<digit_t>(tmp);
			carry = static_cast<digit_t>(tmp >> DIGIT_BITS);
		}
		if (carry) {
			digits.push_back(carry);
		}
		chunk_base = DECIMAL_BASE;
	}

	// we initialize result with space for our header
	string result(BIGNUM_HEADER_SIZE, '0');
	result.reserve(BIGNUM_HEADER_SIZE + digits.size() * DIGIT_BYTES);
	// write the bytes least significant first, these are reversed afterwards
	for (idx_t digit_idx = 0; digit_idx < digits.size(); digit_idx++) {
		auto digit = digits[digit_idx];
		for (idx_t byte_idx = 0; byte_idx < DIGIT_BYTES; byte_idx++) {
			if (digit_idx + 1 == digits.size() && digit == 0) {
				// skip leading zero bytes of the most significant digit
				break;
			}
			auto byte = static_cast<uint8_t>(digit & 0xFF);
			result.push_back(static_cast<char>(is_negative ? ~byte : byte));
			digit >>= 8;
		}
	}
	std::reverse(result.begin() + BIGNUM_HEADER_SIZE, result.end());
	// Set header after we know the size of the bignum
	SetHeader(&result[0], result.size() - BIGNUM_HEADER_SIZE, is_negative);
	return result;
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

//! Below this number of limbs, numbers are multiplied with the schoolbook algorithm
static constexpr idx_t KARATSUBA_THRESHOLD = 48;
//! Below this number of binary limbs, numbers are converted to decimal with the schoolbook algorithm
static constexpr idx_t CONVERSION_THRESHOLD = 64;

static void TrimLimbs(vector<digit_t> &limbs) {
	while (!limbs.empty() && limbs.back() == 0) {
		limbs.pop_back();
	}
}

// Adds the decimal limbs of b, shifted by "shift" limbs, to a
static void AddDecimalLimbs(vector<digit_t> &a, const digit_t *b, idx_t b_size, idx_t shift) {
	while (b_size > 0 && b[b_size - 1] == 0) {
		b_size--;
	}
	if (a.size() < shift + b_size) {
		a.resize(shift + b_size, 0);
	}
	digit_t carry = 0;
	idx_t i = 0;
	for (; i < b_size; i++) {
		digit_t sum = a[shift + i] + b[i] + carry;
		carry = sum >= Bignum::DECIMAL_BASE;
		a[shift + i] = carry ? sum - Bignum::DECIMAL_BASE : sum;
	}
	for (idx_t idx = shift + i; carry; idx++) {
		if (idx == a.size()) {
			a.push_back(0);
		}
		digit_t sum = a[idx] + carry;
		carry = sum >= Bignum::DECIMAL_BASE;
		a[idx] = carry ? sum - Bignum::DECIMAL_BASE : sum;
	}
}

// Subtracts the decimal limbs of b from a, where a >= b
static void SubtractDecimalLimbs(vector<digit_t> &a, const vector<digit_t> &b) {
	digit_t borrow = 0;
	idx_t i = 0;
	for (; i < b.size(); i++) {
		if (i >= a.size()) {
			D_ASSERT(b[i] == 0);
			continue;
		}
		digit_t subtrahend = b[i] + borrow;
		borrow = a[i] < subtrahend;
		a[i] = borrow ? a[i] + Bignum::DECIMAL_BASE - subtrahend : a[i] - subtrahend;
	}
	for (; borrow; i++) {
		D_ASSERT(i < a.size());
		borrow = a[i] == 0;
		a[i] = borrow ? Bignum::DECIMAL_BASE - 1 : a[i] - 1;
	}
}

// Multiplies two numbers in decimal limbs using Karatsuba multiplication
static vector<digit_t> MultiplyDecimalLimbs(const digit_t *a, idx_t a_size, const digit_t *b, idx_t b_size) {
	if (a_size < b_size) {
		std::swap(a, b);
		std::swap(a_size, b_size);
	}
	vector<digit_t> result;
	if (b_size == 0) {
		return result;
	}
	result.resize(a_size + b_size, 0);
	if (b_size < KARATSUBA_THRESHOLD) {
		for (idx_t i = 0; i < b_size; i++) {
			twodigit_t carry = 0;
			for (idx_t j = 0; j < a_size; j++) {
				twodigit_t current = UnsafeNumericCast<twodigit_t>(b[i]) * a[j] + result[i + j] + carry;
				carry = current / Bignum::DECIMAL_BASE;
				result[i + j] = static_cast<digit_t>(current - carry * Bignum::DECIMAL_BASE);
			}
			result[i + a_size] = static_cast<digit_t>(carry);
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
	vector<digit_t> a_sum(a, a + m);
	AddDecimalLimbs(a_sum, a + m, a_size - m, 0);
	vector<digit_t> b_sum(b, b + m);
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

// Converts binary limbs (base 2^32) to decimal limbs (base 10^9), both little-endian
// powers[k] holds 2^(32 * 2^k) in decimal limbs
static vector<digit_t> BinaryToDecimalLimbs(const digit_t *binary, idx_t size, vector<vector<digit_t>> &powers) {
	vector<digit_t> digits;
	if (size <= CONVERSION_THRESHOLD) {
		// typos:ignore-next-line
		// Following CPython and Knuth (TAOCP, Volume 2 (3rd edn), section 4.4, Method 1b).
		for (idx_t i = size; i > 0; i--) {
			digit_t hi = binary[i - 1];
			for (idx_t j = 0; j < digits.size(); j++) {
				twodigit_t tmp = UnsafeNumericCast<twodigit_t>(digits[j]) << Bignum::DIGIT_BITS | hi;
				hi = static_cast<digit_t>(tmp / UnsafeNumericCast<twodigit_t>(Bignum::DECIMAL_BASE));
				digits[j] = static_cast<digit_t>(tmp - UnsafeNumericCast<twodigit_t>(Bignum::DECIMAL_BASE * hi));
			}
			while (hi) {
				digits.push_back(hi % Bignum::DECIMAL_BASE);
				hi /= Bignum::DECIMAL_BASE;
			}
		}
		return digits;
	}
	// split the number into high * 2^(32 * m) + low, where m is a power of two
	idx_t k = 0;
	idx_t m = 1;
	while (2 * m < size) {
		m *= 2;
		k++;
	}
	auto low = BinaryToDecimalLimbs(binary, m, powers);
	auto high = BinaryToDecimalLimbs(binary + m, size - m, powers);
	while (powers.size() <= k) {
		auto &last = powers.back();
		powers.push_back(MultiplyDecimalLimbs(last.data(), last.size(), last.data(), last.size()));
	}
	auto &power = powers[k];
	digits = MultiplyDecimalLimbs(high.data(), high.size(), power.data(), power.size());
	AddDecimalLimbs(digits, low.data(), low.size(), 0);
	TrimLimbs(digits);
	return digits;
}

string Bignum::BignumToVarchar(const bignum_t &blob) {
	string decimal_string;
	vector<uint8_t> byte_array;
	bool is_negative;
	GetByteArray(byte_array, is_negative, blob.data);
	// Rounding byte_array to digit_bytes multiple size, so that we can process every digit_bytes bytes
	// at a time without if check in the for loop
	idx_t padding_size = (-byte_array.size()) & (DIGIT_BYTES - 1);
	byte_array.insert(byte_array.begin(), padding_size, 0);
	idx_t limb_count = byte_array.size() / DIGIT_BYTES;
	vector<digit_t> binary(limb_count);
	for (idx_t i = 0; i < limb_count; i++) {
		digit_t limb = 0;
		for (idx_t j = 0; j < DIGIT_BYTES; j++) {
			limb |= UnsafeNumericCast<digit_t>(byte_array[i * DIGIT_BYTES + j]) << (8 * (DIGIT_BYTES - j - 1));
		}
		binary[limb_count - i - 1] = limb;
	}
	TrimLimbs(binary);
	// 2^32 in decimal limbs
	vector<vector<digit_t>> powers {{294967296, 4}};
	auto digits = BinaryToDecimalLimbs(binary.data(), binary.size(), powers);

	if (digits.empty()) {
		digits.push_back(0);
	}

	for (idx_t i = 0; i < digits.size() - 1; i++) {
		auto remain = digits[i];
		for (idx_t j = 0; j < DECIMAL_SHIFT; j++) {
			decimal_string += DigitToChar(static_cast<int>(remain % 10));
			remain /= 10;
		}
	}

	auto remain = digits.back();
	do {
		decimal_string += DigitToChar(static_cast<int>(remain % 10));
		remain /= 10;
	} while (remain != 0);

	if (is_negative) {
		decimal_string += '-';
	}
	// Reverse the string to get the correct decimal representation
	std::reverse(decimal_string.begin(), decimal_string.end());
	return decimal_string;
}

string Bignum::VarcharToBignum(const string_t &value) {
	idx_t start_pos, end_pos;
	bool is_negative, is_zero, should_round_up;
	if (!VarcharFormatting(value, start_pos, end_pos, is_negative, is_zero, should_round_up)) {
		throw ConversionException("Could not convert string \'%s\' to Bignum", value.GetString());
	}
	return EncodeVarcharBignum(value, start_pos, end_pos, is_negative, is_zero, should_round_up);
}

bool Bignum::BignumToDouble(const bignum_t &blob, double &result, bool &strict) {
	result = 0;

	if (blob.data.GetSize() < 4) {
		throw InvalidInputException("Invalid blob size.");
	}
	auto blob_ptr = blob.data.GetData();

	// Determine if the number is negative
	bool is_negative = (blob_ptr[0] & 0x80) == 0;
	idx_t byte_pos = 0;
	for (idx_t i = blob.data.GetSize() - 1; i > 2; i--) {
		if (is_negative) {
			result += static_cast<uint8_t>(~blob_ptr[i]) * pow(256, static_cast<double>(byte_pos));
		} else {
			result += static_cast<uint8_t>(blob_ptr[i]) * pow(256, static_cast<double>(byte_pos));
		}
		byte_pos++;
	}

	if (is_negative) {
		result *= -1;
	}
	// the value does not fit in a double
	return std::isfinite(result);
}

} // namespace duckdb
