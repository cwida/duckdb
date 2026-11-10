#include "duckdb/common/limits.hpp"
#include "duckdb/common/types/bignum_decimal.hpp"

#include "duckdb/common/array.hpp"
#include "duckdb/common/bignum.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/once_flag.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/bignum.hpp"
#include "duckdb/common/types/string_heap.hpp"
#include "duckdb/common/types/uhugeint.hpp"
#include "duckdb/common/types/vector.hpp"

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

//! An intermediate over a blob that lives elsewhere (the blob must stay alive as long as the intermediate is used). The
//! bignum_t is kept as a member: the intermediate points into it when the blob is short enough to be stored inline.
struct BorrowedBignum {
	explicit BorrowedBignum(const bignum_t &value_p) : value(value_p), intermediate(value) {
	}
	explicit BorrowedBignum(const string &blob) : BorrowedBignum(bignum_t(string_t(blob))) {
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

//! The number of significant bits of the magnitude of a value
idx_t MagnitudeBits(const BignumIntermediate &value) {
	const idx_t start = value.GetStartDataPos();
	if (start >= value.size) {
		return 0;
	}
	idx_t top_bits = 0;
	for (auto byte = value.GetAbsoluteByte(NumericCast<int64_t>(start)); byte != 0; byte >>= 1) {
		top_bits++;
	}
	return (value.size - start - 1) * 8 + top_bits;
}

bool FitsWidthInternal(const BignumIntermediate &value, uint32_t width) {
	// 10^width has floor(width * log2(10)) + 1 bits: a value with clearly fewer bits fits and one with clearly more
	// does not, only a bit length near the boundary compares against the power itself (the margin covers the
	// rounding of the logarithm), so the power is only ever built for values of about the width itself
	const auto power_bits = static_cast<idx_t>(static_cast<long double>(width) * 3.3219280948873623478703194L);
	const auto bits = MagnitudeBits(value);
	if (bits + 1 < power_bits) {
		return true;
	}
	if (bits > power_bits + 2) {
		return false;
	}
	const auto power = DecimalHelper<bignum_t>::PowerOfTen(width);
	BorrowedBignum limit(power);
	return value.IsAbsoluteBigger(limit.intermediate) == SMALLER;
}

//! The BIGNUM blob of a magnitude given as little-endian bignum_digit_t digits
string MagnitudeToBlob(const unsafe_vector<bignum_digit_t> &digits) {
	idx_t top = digits.size();
	while (top > 0 && digits[top - 1] == 0) {
		top--;
	}
	if (top == 0) {
		return Bignum::InitializeBignumZero();
	}
	idx_t top_bytes = 0;
	for (auto digit = digits[top - 1]; digit != 0; digit >>= 8) {
		top_bytes++;
	}
	const idx_t byte_count = (top - 1) * BignumLimb::DIGIT_BYTES + top_bytes;
	if (byte_count > Bignum::MAX_DATA_SIZE) {
		throw OutOfRangeException("A power of %llu bytes does not fit a BIGNUM, which holds at most %llu bytes",
		                          byte_count, idx_t(Bignum::MAX_DATA_SIZE));
	}
	vector<uint8_t> bytes(byte_count);
	for (idx_t i = 0; i < byte_count; i++) {
		// byte i is the i-th most significant byte
		const idx_t from_lsb = byte_count - 1 - i;
		bytes[i] = static_cast<uint8_t>(digits[from_lsb / BignumLimb::DIGIT_BYTES] >>
		                                (8 * (from_lsb % BignumLimb::DIGIT_BYTES)));
	}
	return Bignum::FromByteArray(bytes.data(), byte_count, false);
}

//! 10^exponent as a BIGNUM blob, by binary exponentiation
string ComputePowerOfTen(idx_t exponent, optional_ptr<ClientContext> context) {
	BignumArithmeticState state(context);
	unsafe_vector<bignum_digit_t> result;
	result.push_back(1);
	unsafe_vector<bignum_digit_t> base;
	base.push_back(10);
	unsafe_vector<bignum_digit_t> product;
	while (exponent > 0) {
		if (exponent & 1) {
			BignumIntermediate::MultiplyMagnitudes(state, result, base, product);
			std::swap(result, product);
		}
		exponent >>= 1;
		if (exponent > 0) {
			BignumIntermediate::MultiplyMagnitudes(state, base, base, product);
			std::swap(base, product);
		}
	}
	return MagnitudeToBlob(result);
}

//! The small constants are built once; their blobs outlive the intermediates that point into them
const OwnedBignum &BignumConstant(int32_t value) {
	static const OwnedBignum TWO(Bignum::VarcharToBignum(string_t("2")));
	static const OwnedBignum ONE(Bignum::VarcharToBignum(string_t("1")));
	static const OwnedBignum MINUS_ONE(Bignum::VarcharToBignum(string_t("-1")));
	switch (value) {
	case 2:
		return TWO;
	case 1:
		return ONE;
	default:
		D_ASSERT(value == -1);
		return MINUS_ONE;
	}
}

//! Whether the magnitude of a value is odd
bool IsOdd(const BignumIntermediate &value) {
	return value.GetAbsoluteByte(NumericCast<int64_t>(value.size) - 1) & 1;
}

} // namespace

string DecimalHelper<bignum_t>::PowerOfTen(idx_t exponent, optional_ptr<ClientContext> context) {
	// the powers of everyday widths are computed once and read without locking afterwards
	static constexpr idx_t CACHED_POWERS = 512;
	static array<string, CACHED_POWERS> small_powers;
	static array<OnceFlag, CACHED_POWERS> small_flags;
	if (exponent < CACHED_POWERS) {
		CallOnce(small_flags[exponent], [&]() { small_powers[exponent] = ComputePowerOfTen(exponent, context); });
		return small_powers[exponent];
	}
	// wider powers are kept in a cache bounded by its total size
	static constexpr idx_t MAX_CACHED_POWER_BYTES = 16ULL * 1024 * 1024;
	static mutex wide_lock;
	static unordered_map<idx_t, string> wide_powers;
	static idx_t wide_power_bytes = 0;
	{
		lock_guard<mutex> guard(wide_lock);
		auto entry = wide_powers.find(exponent);
		if (entry != wide_powers.end()) {
			return entry->second;
		}
	}
	// computed outside the lock, so that a long computation does not block the other threads
	auto power = ComputePowerOfTen(exponent, context);
	lock_guard<mutex> guard(wide_lock);
	if (wide_power_bytes + power.size() > MAX_CACHED_POWER_BYTES) {
		wide_powers.clear();
		wide_power_bytes = 0;
	}
	if (wide_powers.emplace(exponent, power).second) {
		wide_power_bytes += power.size();
	}
	return power;
}

string DecimalHelper<bignum_t>::PowerOfTwo(idx_t exponent) {
	// big-endian: the top byte holds the single set bit, followed by exponent / 8 zero bytes
	vector<uint8_t> bytes(exponent / 8 + 1, 0);
	bytes[0] = static_cast<uint8_t>(1 << (exponent % 8));
	return Bignum::FromByteArray(bytes.data(), bytes.size(), false);
}

bool DecimalHelper<bignum_t>::FitsWidth(const bignum_t &value, uint32_t width) {
	BorrowedBignum borrowed(value);
	return FitsWidthInternal(borrowed.intermediate, width);
}

bool DecimalHelper<bignum_t>::FitsWidth(const BignumIntermediate &value, uint32_t width) {
	return FitsWidthInternal(value, width);
}

bool DecimalHelper<bignum_t>::IsNegative(const bignum_t &value) {
	return (value.data.GetData()[0] & 0x80) == 0;
}

bool DecimalHelper<bignum_t>::IsZero(const bignum_t &value) {
	return value.data.GetSize() == Bignum::BIGNUM_HEADER_SIZE + 1 && !IsNegative(value) &&
	       value.data.GetData()[Bignum::BIGNUM_HEADER_SIZE] == 0;
}

//! The unscaled value formatted with the given scale, with at least one digit before the decimal point
static string FormatScaled(const bignum_t &value, uint32_t scale, optional_ptr<ClientContext> context) {
	auto digits = Bignum::BignumToVarchar(value, context);
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

string DecimalHelper<bignum_t>::ToString(const bignum_t &value, uint32_t width, uint32_t scale,
                                         optional_ptr<ClientContext> context) {
	auto result = FormatScaled(value, scale, context);
	if (width == scale && scale > 0) {
		// like the fixed-size decimals, a decimal without integer digits prints no leading zero: .100, -.100
		auto point = result.find('.');
		D_ASSERT(point != string::npos && point > 0 && result[point - 1] == '0');
		result.erase(point - 1, 1);
	}
	return result;
}

bool DecimalHelper<bignum_t>::TryParse(const char *data, idx_t size, uint32_t width, uint32_t scale, string &result,
                                       char decimal_separator, optional_ptr<ClientContext> context) {
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
	result = Bignum::VarcharToBignum(string_t(unscaled), context);
	return true;
}

string DecimalHelper<bignum_t>::Rescale(const bignum_t &value, uint32_t source_scale, uint32_t target_scale,
                                        optional_ptr<ClientContext> context) {
	if (target_scale == source_scale) {
		return value.data.GetString();
	}
	BorrowedBignum source(value);
	StringHeap heap;
	BignumArithmeticState state(context);
	if (target_scale > source_scale) {
		const auto power = PowerOfTen(target_scale - source_scale, context);
		BorrowedBignum factor(power);
		return BignumIntermediate::Multiply(heap, state, source.intermediate, factor.intermediate).GetString();
	}
	// scaling down divides by the power of ten and rounds half away from zero
	const auto power = PowerOfTen(source_scale - target_scale, context);
	BorrowedBignum divisor(power);
	string_t quotient;
	string_t remainder;
	if (!BignumIntermediate::TryDivMod(heap, state, source.intermediate, divisor.intermediate, quotient, remainder)) {
		throw InternalException("DecimalHelper<bignum_t>::Rescale: division by a power of ten failed");
	}
	const bignum_t remainder_bignum(remainder);
	BorrowedBignum remainder_value(remainder_bignum);
	auto twice_remainder =
	    BignumIntermediate::Multiply(heap, state, remainder_value.intermediate, BignumConstant(2).intermediate);
	const bignum_t twice_remainder_bignum(twice_remainder);
	BorrowedBignum twice_remainder_value(twice_remainder_bignum);
	if (twice_remainder_value.intermediate.IsAbsoluteBigger(divisor.intermediate) != SMALLER) {
		// |remainder| >= divisor / 2 rounds away from zero
		auto &one = BignumConstant(source.intermediate.is_negative ? -1 : 1);
		const bignum_t quotient_bignum(quotient);
		BorrowedBignum quotient_value(quotient_bignum);
		quotient = BignumIntermediate::Add(heap, quotient_value.intermediate, one.intermediate);
	}
	return quotient.GetString();
}

bool DecimalHelper<bignum_t>::TryRescale(const bignum_t &value, uint32_t source_scale, uint32_t target_width,
                                         uint32_t target_scale, string &result, optional_ptr<ClientContext> context) {
	auto rescaled = Rescale(value, source_scale, target_scale, context);
	if (!FitsWidth(bignum_t(string_t(rescaled)), target_width)) {
		return false;
	}
	result = std::move(rescaled);
	return true;
}

DecimalHelper<bignum_t>::RoundState::RoundState(Vector &result, optional_ptr<ClientContext> context)
    : heap(StringVector::GetStringHeap(result)), state(context) {
}

bignum_t DecimalHelper<bignum_t>::Store(Vector &result, const string &owned) {
	return bignum_t(StringVector::AddStringOrBlob(result, owned.data(), owned.size()));
}

bool DecimalHelper<bignum_t>::TryRound(const bignum_t &value, idx_t drop_digits, idx_t restore_digits,
                                       DecimalRounding rounding, uint32_t width, RoundState &round_state,
                                       bignum_t &result) {
	auto &heap = round_state.heap;
	auto &state = round_state.state;
	BorrowedBignum source(value);
	string_t rounded = value.data;
	if (drop_digits > 0) {
		const auto power = PowerOfTen(drop_digits, state.context);
		BorrowedBignum divisor(power);
		string_t quotient;
		string_t remainder;
		if (!BignumIntermediate::TryDivMod(heap, state, source.intermediate, divisor.intermediate, quotient,
		                                   remainder)) {
			throw InternalException("DecimalHelper<bignum_t>::TryRound: division by a power of ten failed");
		}
		// the remainder has the sign of the value: the quotient is truncated towards zero
		const bool negative = source.intermediate.is_negative;
		int32_t adjustment = 0;
		if (!IsZero(bignum_t(remainder))) {
			switch (rounding) {
			case DecimalRounding::TRUNCATE:
				break;
			case DecimalRounding::FLOOR:
				adjustment = negative ? -1 : 0;
				break;
			case DecimalRounding::CEIL:
				adjustment = negative ? 0 : 1;
				break;
			default: {
				const bignum_t remainder_bignum(remainder);
				BorrowedBignum remainder_value(remainder_bignum);
				auto twice_remainder = BignumIntermediate::Multiply(heap, state, remainder_value.intermediate,
				                                                    BignumConstant(2).intermediate);
				const bignum_t twice_remainder_bignum(twice_remainder);
				BorrowedBignum twice_remainder_value(twice_remainder_bignum);
				const bignum_t quotient_bignum(quotient);
				BorrowedBignum quotient_value(quotient_bignum);
				auto comparison = twice_remainder_value.intermediate.IsAbsoluteBigger(divisor.intermediate);
				// above the half the value rounds away from zero, on the half the rounding mode decides
				const bool away = comparison == GREATER ||
				                  (comparison == EQUAL && (rounding == DecimalRounding::HALF_AWAY_FROM_ZERO ||
				                                           IsOdd(quotient_value.intermediate)));
				adjustment = away ? (negative ? -1 : 1) : 0;
				break;
			}
			}
		}
		rounded = quotient;
		if (adjustment != 0) {
			const bignum_t quotient_bignum(quotient);
			BorrowedBignum quotient_value(quotient_bignum);
			rounded =
			    BignumIntermediate::Add(heap, quotient_value.intermediate, BignumConstant(adjustment).intermediate);
		}
	}
	if (restore_digits > 0) {
		const bignum_t rounded_bignum(rounded);
		BorrowedBignum rounded_value(rounded_bignum);
		const auto power = PowerOfTen(restore_digits, state.context);
		BorrowedBignum factor(power);
		rounded = BignumIntermediate::Multiply(heap, state, rounded_value.intermediate, factor.intermediate);
	}
	if (!FitsWidth(bignum_t(rounded), width)) {
		return false;
	}
	result = bignum_t(rounded);
	return true;
}

string DecimalHelper<bignum_t>::FromInteger(uhugeint_t value) {
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

string DecimalHelper<bignum_t>::FromInteger(hugeint_t value) {
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

bool DecimalHelper<bignum_t>::TryToUhugeint(const bignum_t &value, uhugeint_t &result) {
	bool strict = false;
	return Bignum::BignumToInt(value, result, strict);
}

bool DecimalHelper<bignum_t>::TryToHugeint(const bignum_t &value, hugeint_t &result) {
	bool strict = false;
	return Bignum::BignumToInt(value, result, strict);
}

template <class T>
static bool TryToFloatingInternal(const bignum_t &value, uint32_t scale, T &result,
                                  optional_ptr<ClientContext> context) {
	// the decimal text parses directly into the target type, so the value is rounded once
	auto text = FormatScaled(value, scale, context);
	if (!TryCast::Operation(string_t(text), result, false)) {
		throw InternalException("Could not convert wide decimal '%s' to a floating point number", text);
	}
	// an overflowing text parses to infinity
	return std::isfinite(result);
}

bool DecimalHelper<bignum_t>::TryToFloating(const bignum_t &value, uint32_t scale, float &result,
                                            optional_ptr<ClientContext> context) {
	return TryToFloatingInternal(value, scale, result, context);
}

bool DecimalHelper<bignum_t>::TryToFloating(const bignum_t &value, uint32_t scale, double &result,
                                            optional_ptr<ClientContext> context) {
	return TryToFloatingInternal(value, scale, result, context);
}

double DecimalHelper<bignum_t>::ToDouble(const bignum_t &value, uint32_t scale, optional_ptr<ClientContext> context) {
	double result;
	TryToFloating(value, scale, result, context);
	return result;
}

string DecimalHelper<bignum_t>::FromTwosComplement(const_data_ptr_t data, idx_t size, bool big_endian) {
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

void DecimalHelper<bignum_t>::ToTwosComplement(const bignum_t &value, data_ptr_t result, idx_t size, bool big_endian) {
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

bool DecimalHelper<bignum_t>::TryFromDouble(double value, uint32_t width, uint32_t scale, string &result,
                                            optional_ptr<ClientContext> context) {
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
	return TryParse(text.c_str(), text.size(), width, scale, result, '.', context);
}

string DecimalHelper<bignum_t>::FromDouble(double value) {
	D_ASSERT(std::isfinite(value) && std::trunc(value) == value);
	int exponent;
	const double fraction = std::frexp(std::fabs(value), &exponent);
	const auto mantissa = static_cast<uint64_t>(std::ldexp(fraction, 53));
	const int shift = exponent - 53;
	string text = value < 0 ? "-" : "";
	if (shift >= 0) {
		text += IntegerDigits(mantissa, static_cast<idx_t>(shift));
	} else {
		// an integral value below 2^53 keeps no fraction bits
		text += to_string(-shift >= 64 ? 0 : mantissa >> static_cast<idx_t>(-shift));
	}
	return Bignum::VarcharToBignum(string_t(text));
}

} // namespace duckdb
