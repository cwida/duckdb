#include "duckdb/common/limits.hpp"
#include "duckdb/common/types/bignum_decimal.hpp"

#include "duckdb/common/bignum.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/bignum.hpp"
#include "duckdb/common/types/string_heap.hpp"
#include "duckdb/common/types/uhugeint.hpp"

#include <cmath>

namespace duckdb {

namespace {

//! A BIGNUM blob that owns its bytes together with an intermediate pointing into it
struct OwnedBignum {
	explicit OwnedBignum(string blob_p) : blob(std::move(blob_p)), value(string_t(blob)), intermediate(value) {
	}
	OwnedBignum(const OwnedBignum &) = delete;
	OwnedBignum &operator=(const OwnedBignum &) = delete;

	string blob;
	bignum_t value;
	BignumIntermediate intermediate;
};

//! An intermediate over a blob that lives elsewhere (the bignum_t must stay alive as long as the intermediate is used)
struct BorrowedBignum {
	explicit BorrowedBignum(const string_t &blob) : value(blob), intermediate(value) {
	}
	BorrowedBignum(const BorrowedBignum &) = delete;
	BorrowedBignum &operator=(const BorrowedBignum &) = delete;

	bignum_t value;
	BignumIntermediate intermediate;
};

void IncrementDigits(string &digits) {
	int carry = 1;
	for (idx_t i = digits.size(); i > 0 && carry; i--) {
		auto digit = digits[i - 1] - '0' + carry;
		digits[i - 1] = static_cast<char>('0' + digit % 10);
		carry = digit / 10;
	}
	if (carry) {
		digits.insert(digits.begin(), '1');
	}
}

//! Skips an underscore between two digits, returns false if no digit follows the underscore
bool SkipDigitSeparator(const char *data, idx_t size, idx_t &pos) {
	if (pos < size && data[pos] == '_') {
		pos++;
		return pos < size && StringUtil::CharacterIsDigit(data[pos]);
	}
	return true;
}

//! Appends the digits at pos to digits, allowing underscores between digits, returns false on a stray underscore
bool ReadDigits(const char *data, idx_t size, idx_t &pos, string &digits) {
	while (pos < size && StringUtil::CharacterIsDigit(data[pos])) {
		digits.push_back(data[pos++]);
		if (!SkipDigitSeparator(data, size, pos)) {
			return false;
		}
	}
	return true;
}

//! The decimal digits of a non-negative integer given as little-endian 32-bit limbs
string LimbsToDecimal(vector<uint32_t> limbs) {
	string digits;
	while (!limbs.empty()) {
		uint64_t remainder = 0;
		for (idx_t i = limbs.size(); i > 0; i--) {
			const uint64_t current = (remainder << 32) | limbs[i - 1];
			limbs[i - 1] = static_cast<uint32_t>(current / 1000000000);
			remainder = current % 1000000000;
		}
		while (!limbs.empty() && limbs.back() == 0) {
			limbs.pop_back();
		}
		auto chunk = to_string(remainder);
		if (!limbs.empty()) {
			chunk.insert(0, 9 - chunk.size(), '0');
		}
		digits.insert(0, chunk);
	}
	return digits.empty() ? "0" : digits;
}

//! The decimal digits of mantissa * 2^shift
string IntegerDigits(uint64_t mantissa, idx_t shift) {
	vector<uint32_t> limbs(shift / 32, 0);
	limbs.push_back(static_cast<uint32_t>(mantissa));
	limbs.push_back(static_cast<uint32_t>(mantissa >> 32));
	limbs.push_back(0);
	const auto offset = static_cast<uint32_t>(shift % 32);
	if (offset != 0) {
		for (idx_t i = limbs.size(); i > 0; i--) {
			limbs[i - 1] = (limbs[i - 1] << offset) | (i > 1 ? limbs[i - 2] >> (32 - offset) : 0);
		}
	}
	return LimbsToDecimal(std::move(limbs));
}

//! The first max_digits decimal digits of fraction / 2^bits, preceded by the decimal point (empty for zero)
string FractionDigits(uint64_t fraction, idx_t bits, idx_t max_digits) {
	vector<uint32_t> limbs((bits + 31) / 32, 0);
	limbs[0] = static_cast<uint32_t>(fraction);
	if (limbs.size() > 1) {
		limbs[1] = static_cast<uint32_t>(fraction >> 32);
	}
	const auto top_bits = static_cast<uint32_t>(bits % 32 == 0 ? 32 : bits % 32);
	string digits;
	bool non_zero = fraction != 0;
	while (non_zero && digits.size() < max_digits) {
		// multiply by ten, the digit is everything that overflows the fraction bits
		uint64_t carry = 0;
		non_zero = false;
		for (auto &limb : limbs) {
			const uint64_t current = uint64_t(limb) * 10 + carry;
			limb = static_cast<uint32_t>(current);
			carry = current >> 32;
		}
		uint32_t digit;
		if (top_bits == 32) {
			digit = static_cast<uint32_t>(carry);
		} else {
			digit = (static_cast<uint32_t>(carry) << (32 - top_bits)) | (limbs.back() >> top_bits);
			limbs.back() &= (uint32_t(1) << top_bits) - 1;
		}
		for (auto limb : limbs) {
			non_zero = non_zero || limb != 0;
		}
		digits.push_back(static_cast<char>('0' + digit));
	}
	return digits.empty() ? "" : "." + digits;
}

bool FitsWidthInternal(const BignumIntermediate &value, uint32_t width) {
	BorrowedBignum limit(string_t(BignumDecimal::PowerOfTen(width)));
	return value.IsAbsoluteBigger(limit.intermediate) == SMALLER;
}

} // namespace

const string &BignumDecimal::PowerOfTen(idx_t exponent) {
	static const vector<string> POWERS = []() {
		vector<string> result;
		result.reserve(idx_t(Decimal::MAX_WIDTH_DECIMAL) + 1);
		for (idx_t i = 0; i <= Decimal::MAX_WIDTH_DECIMAL; i++) {
			string digits = "1" + string(i, '0');
			result.push_back(Bignum::VarcharToBignum(string_t(digits)));
		}
		return result;
	}();
	D_ASSERT(exponent <= Decimal::MAX_WIDTH_DECIMAL);
	return POWERS[exponent];
}

bool BignumDecimal::FitsWidth(const string_t &value, uint32_t width) {
	BorrowedBignum borrowed(value);
	return FitsWidthInternal(borrowed.intermediate, width);
}

bool BignumDecimal::FitsWidth(const BignumIntermediate &value, uint32_t width) {
	return FitsWidthInternal(value, width);
}

bool BignumDecimal::IsNegative(const string_t &value) {
	return (value.GetData()[0] & 0x80) == 0;
}

bool BignumDecimal::IsZero(const string_t &value) {
	return value.GetSize() == Bignum::BIGNUM_HEADER_SIZE + 1 && !IsNegative(value) &&
	       value.GetData()[Bignum::BIGNUM_HEADER_SIZE] == 0;
}

string BignumDecimal::ToString(const string_t &value, uint32_t scale) {
	const bignum_t bignum(value);
	auto digits = Bignum::BignumToVarchar(bignum);
	if (scale == 0) {
		return digits;
	}
	bool negative = false;
	if (!digits.empty() && digits[0] == '-') {
		negative = true;
		digits.erase(digits.begin());
	}
	// make sure there is at least one digit in front of the decimal point
	if (digits.size() <= scale) {
		digits.insert(digits.begin(), scale + 1 - digits.size(), '0');
	}
	digits.insert(digits.begin() + static_cast<int64_t>(digits.size() - scale), '.');
	if (negative) {
		digits.insert(digits.begin(), '-');
	}
	return digits;
}

bool BignumDecimal::TryParse(const char *data, idx_t size, uint32_t width, uint32_t scale, string &result,
                             char decimal_separator) {
	idx_t pos = 0;
	// skip surrounding whitespace
	while (pos < size && StringUtil::CharacterIsSpace(data[pos])) {
		pos++;
	}
	while (size > pos && StringUtil::CharacterIsSpace(data[size - 1])) {
		size--;
	}
	if (pos >= size) {
		return false;
	}
	bool negative = false;
	if (data[pos] == '-' || data[pos] == '+') {
		negative = data[pos] == '-';
		pos++;
	}
	string digits;
	if (!ReadDigits(data, size, pos, digits)) {
		return false;
	}
	const idx_t integer_digits = digits.size();
	if (pos < size && data[pos] == decimal_separator) {
		pos++;
		if (!ReadDigits(data, size, pos, digits)) {
			return false;
		}
	}
	if (digits.empty()) {
		return false;
	}
	int64_t exponent = 0;
	if (pos < size && (data[pos] == 'e' || data[pos] == 'E')) {
		pos++;
		bool exponent_negative = false;
		if (pos < size && (data[pos] == '-' || data[pos] == '+')) {
			exponent_negative = data[pos] == '-';
			pos++;
		}
		string exponent_digits;
		if (!ReadDigits(data, size, pos, exponent_digits) || exponent_digits.empty()) {
			return false;
		}
		for (auto c : exponent_digits) {
			exponent = exponent * 10 + (c - '0');
			if (exponent > NumericLimits<int32_t>::Maximum()) {
				// the fixed-size decimals parse the exponent as a 32-bit integer
				return false;
			}
		}
		if (exponent_negative) {
			exponent = -exponent;
		}
	}
	if (pos != size) {
		return false;
	}
	// the unscaled value keeps "integer_digits + exponent + scale" digits, the next digit rounds half away from zero
	const int64_t keep = static_cast<int64_t>(integer_digits) + exponent + scale;
	string unscaled;
	bool round_up = false;
	if (keep >= 0) {
		const auto keep_count = static_cast<idx_t>(keep);
		if (keep_count > idx_t(width) + digits.size() + 1) {
			// far more digits than the width allows, unless every digit is zero
			for (auto c : digits) {
				if (c != '0') {
					return false;
				}
			}
			unscaled = "0";
		} else {
			unscaled = digits.substr(0, MinValue<idx_t>(keep_count, digits.size()));
			if (keep_count > digits.size()) {
				unscaled.append(keep_count - digits.size(), '0');
			} else if (keep_count < digits.size()) {
				round_up = digits[keep_count] >= '5';
			}
		}
	}
	// strip leading zeros
	idx_t first_non_zero = 0;
	while (first_non_zero < unscaled.size() && unscaled[first_non_zero] == '0') {
		first_non_zero++;
	}
	unscaled.erase(0, first_non_zero);
	if (round_up) {
		IncrementDigits(unscaled);
	}
	if (unscaled.size() > width) {
		return false;
	}
	if (unscaled.empty()) {
		unscaled = "0";
		negative = false;
	}
	if (negative) {
		unscaled.insert(unscaled.begin(), '-');
	}
	result = Bignum::VarcharToBignum(string_t(unscaled));
	return true;
}

bool BignumDecimal::TryRescale(const string_t &value, uint32_t source_scale, uint32_t target_width,
                               uint32_t target_scale, string &result) {
	BorrowedBignum source(value);
	if (target_scale == source_scale) {
		if (!FitsWidthInternal(source.intermediate, target_width)) {
			return false;
		}
		result = value.GetString();
		return true;
	}
	StringHeap heap;
	// the operands have at most Decimal::MAX_WIDTH_DECIMAL digits, so no interrupt polling is needed
	BignumArithmeticState state;
	if (target_scale > source_scale) {
		BorrowedBignum factor(string_t(PowerOfTen(target_scale - source_scale)));
		auto scaled = BignumIntermediate::Multiply(heap, state, source.intermediate, factor.intermediate);
		if (!FitsWidth(scaled, target_width)) {
			return false;
		}
		result = scaled.GetString();
		return true;
	}
	// scaling down divides by the power of ten and rounds half away from zero
	BorrowedBignum divisor(string_t(PowerOfTen(source_scale - target_scale)));
	string_t quotient;
	string_t remainder;
	if (!BignumIntermediate::TryDivMod(heap, state, source.intermediate, divisor.intermediate, quotient, remainder)) {
		throw InternalException("BignumDecimal::TryRescale: division by a power of ten failed");
	}
	// the small constants are built once; their blobs outlive the intermediates that point into them
	static const OwnedBignum TWO(Bignum::VarcharToBignum(string_t("2")));
	static const OwnedBignum ONE(Bignum::VarcharToBignum(string_t("1")));
	static const OwnedBignum MINUS_ONE(Bignum::VarcharToBignum(string_t("-1")));
	BorrowedBignum remainder_value(remainder);
	auto twice_remainder = BignumIntermediate::Multiply(heap, state, remainder_value.intermediate, TWO.intermediate);
	BorrowedBignum twice_remainder_value(twice_remainder);
	if (twice_remainder_value.intermediate.IsAbsoluteBigger(divisor.intermediate) != SMALLER) {
		// |remainder| >= divisor / 2 rounds away from zero
		auto &one = source.intermediate.is_negative ? MINUS_ONE : ONE;
		BorrowedBignum quotient_value(quotient);
		quotient = BignumIntermediate::Add(heap, quotient_value.intermediate, one.intermediate);
	}
	if (!FitsWidth(quotient, target_width)) {
		return false;
	}
	result = quotient.GetString();
	return true;
}

string BignumDecimal::FromInteger(uhugeint_t value) {
	uint8_t bytes[16];
	idx_t size = 0;
	for (idx_t i = 0; i < 16; i++) {
		// big-endian, the most significant byte first
		auto shift = (15 - i) * 8;
		uint8_t byte = static_cast<uint8_t>((shift >= 64 ? value.upper >> (shift - 64) : value.lower >> shift) & 0xFF);
		if (size == 0 && byte == 0 && i < 15) {
			continue;
		}
		bytes[size++] = byte;
	}
	return Bignum::FromByteArray(bytes, size, false);
}

string BignumDecimal::FromInteger(hugeint_t value) {
	if (value.upper >= 0) {
		return FromInteger(uhugeint_t(static_cast<uint64_t>(value.upper), value.lower));
	}
	// the magnitude of the minimum hugeint (2^127) fits a uhugeint, so negate in uhugeint two's complement space
	uhugeint_t bits(static_cast<uint64_t>(value.upper), value.lower);
	uhugeint_t magnitude = ~bits + uhugeint_t(1);
	auto blob = FromInteger(magnitude);
	// complementing the header and data bytes flips the sign
	for (auto &c : blob) {
		c = static_cast<char>(~c);
	}
	return blob;
}

bool BignumDecimal::TryToUhugeint(const string_t &value, uhugeint_t &result) {
	bool strict = false;
	return Bignum::BignumToInt(bignum_t(value), result, strict);
}

bool BignumDecimal::TryToHugeint(const string_t &value, hugeint_t &result) {
	bool strict = false;
	return Bignum::BignumToInt(bignum_t(value), result, strict);
}

double BignumDecimal::ToDouble(const string_t &value, uint32_t scale) {
	auto text = ToString(value, scale);
	double result;
	if (!TryCast::Operation(string_t(text), result, false)) {
		throw InternalException("Could not convert wide decimal '%s' to DOUBLE", text);
	}
	return result;
}

string BignumDecimal::FromTwosComplement(const_data_ptr_t data, idx_t size, bool big_endian) {
	if (size == 0) {
		return Bignum::InitializeBignumZero();
	}
	// normalize to big-endian magnitude bytes
	vector<uint8_t> magnitude(size);
	for (idx_t i = 0; i < size; i++) {
		magnitude[i] = big_endian ? data[i] : data[size - 1 - i];
	}
	const bool negative = (magnitude[0] & 0x80) != 0;
	if (negative) {
		// the magnitude of a negative two's complement number is the complement of every byte plus one
		for (idx_t i = 0; i < size; i++) {
			magnitude[i] = static_cast<uint8_t>(~magnitude[i]);
		}
		for (idx_t i = size; i > 0; i--) {
			if (++magnitude[i - 1] != 0) {
				break;
			}
		}
	}
	// strip leading zero bytes
	idx_t start = 0;
	while (start + 1 < size && magnitude[start] == 0) {
		start++;
	}
	if (size - start == 1 && magnitude[start] == 0) {
		return Bignum::InitializeBignumZero();
	}
	return Bignum::FromByteArray(magnitude.data() + start, size - start, negative);
}

void BignumDecimal::ToTwosComplement(const string_t &value, data_ptr_t result, idx_t size, bool big_endian) {
	BorrowedBignum borrowed(value);
	auto &intermediate = borrowed.intermediate;
	const idx_t start = intermediate.GetStartDataPos();
	const idx_t magnitude_bytes = intermediate.size - start;
	if (magnitude_bytes > size) {
		throw OutOfRangeException("Decimal value does not fit in %d bytes", size);
	}
	// big-endian magnitude, right-aligned
	vector<uint8_t> bytes(size, 0);
	for (idx_t i = 0; i < magnitude_bytes; i++) {
		bytes[size - magnitude_bytes + i] = intermediate.GetAbsoluteByte(static_cast<int64_t>(start + i));
	}
	if (intermediate.is_negative) {
		// two's complement negation complements every byte and adds one
		for (idx_t i = 0; i < size; i++) {
			bytes[i] = static_cast<uint8_t>(~bytes[i]);
		}
		for (idx_t i = size; i > 0; i--) {
			if (++bytes[i - 1] != 0) {
				break;
			}
		}
		// the only negative number whose magnitude has the top bit set is -2^(8 * size - 1)
		if (magnitude_bytes == size && (bytes[0] & 0x80) == 0) {
			throw OutOfRangeException("Decimal value does not fit in %d bytes", size);
		}
	} else if (magnitude_bytes == size && (bytes[0] & 0x80) != 0) {
		throw OutOfRangeException("Decimal value does not fit in %d bytes", size);
	}
	for (idx_t i = 0; i < size; i++) {
		result[i] = big_endian ? bytes[i] : bytes[size - 1 - i];
	}
}

bool BignumDecimal::TryFromDouble(double value, uint32_t width, uint32_t scale, string &result) {
	if (!std::isfinite(value)) {
		return false;
	}
	// |value| = mantissa * 2^shift with an integer mantissa of at most 53 bits
	int exponent;
	const double fraction = std::frexp(std::fabs(value), &exponent);
	const auto mantissa = static_cast<uint64_t>(std::ldexp(fraction, 53));
	const int shift = exponent - 53;
	string text = value < 0 ? "-" : "";
	if (shift >= 0) {
		text += IntegerDigits(mantissa, static_cast<idx_t>(shift));
	} else {
		const auto fraction_bits = static_cast<idx_t>(-shift);
		if (fraction_bits >= 64) {
			text += "0";
			text += FractionDigits(mantissa, fraction_bits, idx_t(scale) + 1);
		} else {
			text += to_string(mantissa >> fraction_bits);
			text += FractionDigits(mantissa & ((uint64_t(1) << fraction_bits) - 1), fraction_bits, idx_t(scale) + 1);
		}
	}
	return TryParse(text.c_str(), text.size(), width, scale, result);
}

} // namespace duckdb
