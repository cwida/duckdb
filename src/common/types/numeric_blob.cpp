#include "duckdb/common/types/numeric_blob.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/conversion_exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/numeric_type.hpp"
#include "duckdb/common/types/uhugeint.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/vector_writer.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"

namespace duckdb {

static constexpr data_t NUMERIC_CLASS_NEGATIVE = 0x00;
static constexpr data_t NUMERIC_CLASS_ZERO = 0x40;
static constexpr data_t NUMERIC_CLASS_POSITIVE = 0x80;
static constexpr int64_t NUMERIC_EXPONENT_BIAS = 32768;
static constexpr idx_t NUMERIC_MAX_BLOB_SIZE = NumericBlob::MAX_BLOB_SIZE;
//! The digits of a 128-bit unscaled value
static constexpr idx_t NUMERIC_MAX_INT128_DIGITS = 39;

//===--------------------------------------------------------------------===//
// Blob layout
//===--------------------------------------------------------------------===//
namespace {

//! The header of a blob: the class byte and the biased adjusted exponent, complemented for negative values
struct BlobHeader {
	static idx_t Write(data_ptr_t buffer, bool negative, int64_t adjusted) {
		buffer[0] = negative ? NUMERIC_CLASS_NEGATIVE : NUMERIC_CLASS_POSITIVE;
		auto biased = uint16_t(adjusted + NUMERIC_EXPONENT_BIAS);
		buffer[1] = data_t(biased >> 8);
		buffer[2] = data_t(biased & 0xFF);
		if (negative) {
			buffer[1] = data_t(~buffer[1]);
			buffer[2] = data_t(~buffer[2]);
		}
		return 3;
	}
};

//! A parsed blob: the digits are read in place (stored as digit + 1, complemented for negative values)
struct BlobView {
	bool zero = false;
	bool negative = false;
	int64_t adjusted = 0;
	const_data_ptr_t digits = nullptr;
	idx_t digit_count = 0;

	inline data_t Digit(idx_t i) const {
		auto b = digits[i];
		return data_t((negative ? data_t(~b) : b) - 1);
	}
};

BlobView ParseBlob(const string_t &blob) {
	BlobView view;
	auto data = const_data_ptr_cast(blob.GetData());
	auto size = blob.GetSize();
	if (size == 0) {
		throw InternalException("Empty NUMERIC blob");
	}
	if (data[0] == NUMERIC_CLASS_ZERO) {
		view.zero = true;
		return view;
	}
	view.negative = data[0] == NUMERIC_CLASS_NEGATIVE;
	if (size < 4) {
		throw InternalException("Truncated NUMERIC blob");
	}
	auto high = view.negative ? data_t(~data[1]) : data[1];
	auto low = view.negative ? data_t(~data[2]) : data[2];
	auto biased = uint16_t((uint16_t(high) << 8) | low);
	view.adjusted = int64_t(biased) - NUMERIC_EXPONENT_BIAS;
	view.digits = data + 3;
	auto terminator = view.negative ? data_t(0xFF) : data_t(0);
	idx_t count = 0;
	while (3 + count < size && data[3 + count] != terminator) {
		count++;
	}
	view.digit_count = count;
	return view;
}

//! The powers of ten that fit an unsigned 128-bit integer
struct UnsignedPowersOfTen {
	UnsignedPowersOfTen() {
		for (idx_t i = 0; i < Hugeint::CACHED_POWERS_OF_TEN; i++) {
			auto &power = Hugeint::POWERS_OF_TEN[i];
			values[i] = uhugeint_t(uint64_t(power.upper), power.lower);
		}
	}
	uhugeint_t values[Hugeint::CACHED_POWERS_OF_TEN];
};

const uhugeint_t &PowerOfTen(idx_t exponent) {
	static const UnsignedPowersOfTen powers;
	D_ASSERT(exponent < Hugeint::CACHED_POWERS_OF_TEN);
	return powers.values[exponent];
}

//! Writes the decimal digits of a 64-bit value in front of position `pos` of `digits`, two at a time
inline void WriteDigits(uint64_t value, data_t *digits, idx_t &pos) {
	while (value >= 100) {
		auto quotient = value / 100;
		auto pair = uint32_t(value - quotient * 100);
		digits[--pos] = data_t(pair % 10);
		digits[--pos] = data_t(pair / 10);
		value = quotient;
	}
	if (value >= 10) {
		digits[--pos] = data_t(value % 10);
		digits[--pos] = data_t(value / 10);
	} else {
		digits[--pos] = data_t(value);
	}
}

//! Writes the blob of magnitude * 10^-scale into the buffer and returns its length
idx_t EncodeMagnitude(uhugeint_t magnitude, bool negative, uint32_t scale, data_ptr_t buffer) {
	if (magnitude.upper == 0 && magnitude.lower == 0) {
		buffer[0] = NUMERIC_CLASS_ZERO;
		return 1;
	}
	// the decimal digits of the magnitude, filled from the back
	data_t digits[NUMERIC_MAX_INT128_DIGITS];
	idx_t pos = NUMERIC_MAX_INT128_DIGITS;
	if (magnitude.upper == 0) {
		WriteDigits(magnitude.lower, digits, pos);
	} else {
		static constexpr uint64_t TEN18 = 1000000000000000000ULL;
		while (magnitude.upper != 0) {
			auto quotient = magnitude / uhugeint_t(0, TEN18);
			auto remainder = (magnitude - quotient * uhugeint_t(0, TEN18)).lower;
			// 18 digits, zero padded
			auto group_end = pos;
			WriteDigits(remainder, digits, pos);
			while (group_end - pos < 18) {
				digits[--pos] = 0;
			}
			magnitude = quotient;
		}
		WriteDigits(magnitude.lower, digits, pos);
		// the magnitude is not zero: skip the leading zeros of the highest group
		while (digits[pos] == 0) {
			pos++;
		}
	}
	idx_t digit_count = NUMERIC_MAX_INT128_DIGITS - pos;
	int64_t adjusted = int64_t(digit_count) - 1 - int64_t(scale);
	idx_t last = NUMERIC_MAX_INT128_DIGITS;
	while (digits[last - 1] == 0) {
		last--;
	}
	auto length = BlobHeader::Write(buffer, negative, adjusted);
	if (negative) {
		for (idx_t i = pos; i < last; i++) {
			buffer[length++] = data_t(~data_t(digits[i] + 1));
		}
		buffer[length++] = data_t(0xFF);
	} else {
		for (idx_t i = pos; i < last; i++) {
			buffer[length++] = data_t(digits[i] + 1);
		}
		buffer[length++] = data_t(0);
	}
	return length;
}

template <class T>
struct Magnitude {
	static inline uhugeint_t Get(T value, bool &negative) {
		negative = value < 0;
		auto wide = uint64_t(value);
		return uhugeint_t(0, negative ? uint64_t(0) - wide : wide);
	}
};

template <>
struct Magnitude<hugeint_t> {
	static inline uhugeint_t Get(hugeint_t value, bool &negative) {
		negative = value.upper < 0;
		uhugeint_t bits(uint64_t(value.upper), value.lower);
		return negative ? uhugeint_t(0, 0) - bits : bits;
	}
};

template <>
struct Magnitude<uhugeint_t> {
	static inline uhugeint_t Get(uhugeint_t value, bool &negative) {
		negative = false;
		return value;
	}
};

//! Every value below 10000 as four digit bytes (digit + 1, leading zeros included), plain and complemented for
//! negative values, with the number of trailing zero digits of the group
struct DigitQuads {
	DigitQuads() {
		for (idx_t value = 0; value < 10000; value++) {
			idx_t rest = value;
			for (idx_t k = 4; k > 0; k--) {
				digits[value][k - 1] = data_t(rest % 10 + 1);
				complemented[value][k - 1] = data_t(~digits[value][k - 1]);
				rest /= 10;
			}
			uint8_t zeros = 0;
			while (zeros < 4 && digits[value][3 - zeros] == 1) {
				zeros++;
			}
			trailing[value] = zeros;
		}
	}
	data_t digits[10000][4];
	data_t complemented[10000][4];
	uint8_t trailing[10000];
};

const DigitQuads &Quads() {
	static const DigitQuads quads;
	return quads;
}

//! A 64-bit magnitude split into groups of four decimal digits, lowest group first
struct DigitGroups {
	uint16_t groups[5];
	idx_t group_count;
	idx_t top_digits;

	explicit DigitGroups(uint64_t magnitude) : group_count(0) {
		do {
			auto quotient = magnitude / 10000;
			groups[group_count++] = uint16_t(magnitude - quotient * 10000);
			magnitude = quotient;
		} while (magnitude != 0);
		auto top = groups[group_count - 1];
		top_digits = top >= 1000 ? 4 : top >= 100 ? 3 : top >= 10 ? 2 : 1;
	}

	idx_t DigitCount() const {
		return 4 * (group_count - 1) + top_digits;
	}

	//! The trailing zero digits (the top group is never zero)
	idx_t TrailingZeros(const DigitQuads &quads) const {
		idx_t zeros = 0;
		idx_t g = 0;
		while (g < group_count - 1 && groups[g] == 0) {
			zeros += 4;
			g++;
		}
		return zeros + quads.trailing[groups[g]];
	}

	//! Writes all digits (digit + 1), returns the number written
	template <bool NEGATIVE>
	idx_t Write(const DigitQuads &quads, data_ptr_t digits) const {
		auto table = NEGATIVE ? quads.complemented : quads.digits;
		idx_t pos = 0;
		auto &top = table[groups[group_count - 1]];
		for (idx_t k = 4 - top_digits; k < 4; k++) {
			digits[pos++] = top[k];
		}
		for (idx_t g = group_count - 1; g > 0; g--) {
			memcpy(digits + pos, table[groups[g - 1]], 4);
			pos += 4;
		}
		return pos;
	}
};

//! Writes the blob of a 64-bit magnitude (not zero) into `blob`, which must have room for the digits of all its
//! groups plus the header and terminator; returns the length
template <bool NEGATIVE>
inline idx_t EncodeSmall(const DigitQuads &quads, const DigitGroups &groups, uint32_t scale, data_ptr_t blob) {
	auto digit_count = groups.DigitCount();
	auto significant = digit_count - groups.TrailingZeros(quads);
	BlobHeader::Write(blob, NEGATIVE, int64_t(digit_count) - 1 - int64_t(scale));
	auto digits = blob + 3;
	groups.Write<NEGATIVE>(quads, digits);
	digits[significant] = NEGATIVE ? data_t(0xFF) : data_t(0);
	// the bytes of the trailing zeros that were written past the terminator
	for (idx_t k = significant + 1; k <= digit_count; k++) {
		digits[k] = 0;
	}
	return 3 + significant + 1;
}

//! The three header bytes as a little-endian word: class, biased adjusted exponent high and low byte
inline uint32_t HeaderWord(bool negative, int64_t adjusted) {
	auto biased = uint32_t(uint16_t(adjusted + NUMERIC_EXPONENT_BIAS));
	uint32_t word = (biased >> 8) << 8 | (biased & 0xFF) << 16;
	if (negative) {
		return NUMERIC_CLASS_NEGATIVE | (~word & 0xFFFF00);
	}
	return NUMERIC_CLASS_POSITIVE | word;
}

//! An inlined string_t as its two 64-bit words: length and the first four bytes, then the remaining eight
struct InlineWords {
	uint64_t first;
	uint64_t second;
};

//! The trailing zero digits of a four-digit group loaded as a word (the least significant digit in the top byte)
inline idx_t TrailingZeroDigits(uint32_t group_word) {
	// a zero digit is the byte 0x01; the group is never all zeros here
	auto marked = group_word ^ 0x01010101u;
	return idx_t(__builtin_clz(marked)) / 8;
}

//! Encodes a magnitude of at most eight digits (not zero) as the words of an inlined string_t, assembled in registers
template <bool NEGATIVE>
inline InlineWords EncodeInline(const DigitQuads &quads, uint64_t high, uint64_t low, uint32_t scale) {
	auto top = high != 0 ? high : low;
	idx_t top_digits = top >= 1000 ? 4 : top >= 100 ? 3 : top >= 10 ? 2 : 1;
	idx_t digit_count = top_digits;
	auto top_word = Load<uint32_t>(quads.digits[top]);
	// the top group without its leading zeros: the most significant digit is the lowest byte
	uint64_t digits = top_word >> (8 * (4 - top_digits));
	idx_t trailing;
	if (high != 0) {
		digit_count += 4;
		auto low_word = Load<uint32_t>(quads.digits[low]);
		digits |= uint64_t(low_word) << (8 * top_digits);
		trailing = low == 0 ? 4 + TrailingZeroDigits(top_word) : TrailingZeroDigits(low_word);
	} else {
		trailing = TrailingZeroDigits(top_word);
	}
	auto significant = digit_count - trailing;
	if (NEGATIVE) {
		digits = ~digits;
	}
	// cut the trailing zeros and terminate; eight significant digits put the terminator in the last byte
	auto terminator = NEGATIVE ? uint64_t(0xFF) : uint64_t(0);
	uint64_t last_byte = 0;
	if (significant < 8) {
		digits &= (uint64_t(1) << (8 * significant)) - 1;
		digits |= terminator << (8 * significant);
	} else {
		last_byte = terminator << 56;
	}
	uint64_t length = 3 + significant + 1;
	auto header = uint64_t(HeaderWord(NEGATIVE, int64_t(digit_count) - 1 - int64_t(scale)));
	InlineWords words;
	words.first = length | header << 32 | (digits & 0xFF) << 56;
	words.second = digits >> 8 | last_byte;
	return words;
}

//! The slot of a string_t in the result data
inline void StoreWords(string_t *slot, InlineWords words) {
	auto target = data_ptr_cast(slot);
	Store<uint64_t>(words.first, target);
	Store<uint64_t>(words.second, target + sizeof(uint64_t));
}

//! Encodes one value into its slot; the writer provides the heap for blobs that do not inline
template <class T>
inline void EncodeValue(const DigitQuads &quads, T value, uint32_t scale, string_t *slot,
                        VectorWriter<string_t> &writer) {
	bool negative;
	auto magnitude = Magnitude<T>::Get(value, negative);
#ifndef DUCKDB_DEBUG_NO_INLINE
	// blobs of up to twelve bytes are written as the inlined words of the string_t; a build without inlined strings
	// stores every blob in the heap below
	if (magnitude.upper == 0) {
		auto small = magnitude.lower;
		if (small == 0) {
			InlineWords words;
			words.first = 1 | uint64_t(NUMERIC_CLASS_ZERO) << 32;
			words.second = 0;
			StoreWords(slot, words);
			return;
		}
		auto high = small / 10000;
		if (high < 10000) {
			auto low = small - high * 10000;
			StoreWords(slot, negative ? EncodeInline<true>(quads, high, low, scale)
			                          : EncodeInline<false>(quads, high, low, scale));
			return;
		}
		// more than eight digits
		alignas(8) data_t scratch[32];
		DigitGroups groups(small);
		auto length = negative ? EncodeSmall<true>(quads, groups, scale, scratch)
		                       : EncodeSmall<false>(quads, groups, scale, scratch);
		if (length <= string_t::INLINE_LENGTH) {
			*slot = string_t(const_char_ptr_cast(scratch), UnsafeNumericCast<uint32_t>(length));
			return;
		}
		*slot = writer.GetHeap().EmptyString(length);
		memcpy(slot->GetDataWriteable(), scratch, length);
		slot->Finalize();
		return;
	}
#endif
	data_t buffer[NUMERIC_MAX_BLOB_SIZE];
	auto length = EncodeMagnitude(magnitude, negative, scale, buffer);
	*slot = writer.GetHeap().EmptyString(length);
	memcpy(slot->GetDataWriteable(), buffer, length);
	slot->Finalize();
}

//! Encodes the values of a fixed-size representation
template <class T>
void EncodeTyped(Vector &typed, idx_t count, uint32_t scale, Vector &result) {
	auto &quads = Quads();
	UnifiedVectorFormat vdata;
	typed.ToUnifiedFormat(vdata);
	auto data = UnifiedVectorFormat::GetData<T>(vdata);
	// the writer sizes the result and owns its heap; the slots are written directly
	auto writer = FlatVector::Writer<string_t>(result, count);
	auto slots = FlatVector::GetDataMutable<string_t>(result);
	if (!vdata.sel->IsSet() && vdata.validity.CannotHaveNull()) {
		for (idx_t i = 0; i < count; i++) {
			EncodeValue<T>(quads, data[i], scale, slots + i, writer);
		}
		writer.Truncate();
		return;
	}
	for (idx_t i = 0; i < count; i++) {
		auto idx = vdata.sel->get_index(i);
		if (!vdata.validity.RowIsValid(idx)) {
			FlatVector::SetNull(result, i, true);
			continue;
		}
		EncodeValue<T>(quads, data[idx], scale, slots + i, writer);
	}
	writer.Truncate();
}

//! The digits of a blob as an unscaled 128-bit value at the given scale; false when the value does not fit the scale
//! or 128 bits
bool TryGetMagnitude(const BlobView &view, uint32_t scale, uhugeint_t &magnitude) {
	// the digits form an integer that is scaled by 10^(adjusted + 1 - digit_count): bring it to the target scale
	int64_t shift = view.adjusted + 1 - int64_t(view.digit_count) + int64_t(scale);
	if (shift < 0 || view.digit_count + idx_t(shift) > NUMERIC_MAX_INT128_DIGITS) {
		return false;
	}
	idx_t i = 0;
	uint64_t low = 0;
	for (; i < view.digit_count && i < 19; i++) {
		low = low * 10 + view.Digit(i);
	}
	magnitude = uhugeint_t(0, low);
	for (; i < view.digit_count; i++) {
		// 39 digits can exceed 128 bits: the arithmetic has to report the overflow instead of wrapping
		if (!Uhugeint::TryMultiply(magnitude, uhugeint_t(0, 10), magnitude) ||
		    !Uhugeint::TryAddInPlace(magnitude, uhugeint_t(0, view.Digit(i)))) {
			return false;
		}
		if (magnitude.upper > uint64_t(INT64_MAX)) {
			return false;
		}
	}
	if (shift == 0) {
		return true;
	}
	auto &power = PowerOfTen(idx_t(shift));
	// the magnitude does not exceed 10^39 by the digit check; the product may still exceed 128 bits
	uhugeint_t product;
	if (!Uhugeint::TryMultiply(magnitude, power, product)) {
		return false;
	}
	magnitude = product;
	return true;
}

template <class T>
struct FromMagnitude {
	static inline bool Get(uhugeint_t magnitude, bool negative, T &result) {
		if (magnitude.upper > uint64_t(INT64_MAX)) {
			return false;
		}
		hugeint_t signed_value(int64_t(magnitude.upper), magnitude.lower);
		if (negative) {
			signed_value = -signed_value;
		}
		return Hugeint::TryCast(signed_value, result);
	}
};

template <>
struct FromMagnitude<uhugeint_t> {
	static inline bool Get(uhugeint_t magnitude, bool negative, uhugeint_t &result) {
		if (negative) {
			return false;
		}
		result = magnitude;
		return true;
	}
};

//! Decodes into a fixed-size representation; a DECIMAL target also bounds the magnitude by its width
template <class T>
bool TryDecodeTyped(Vector &canonical, idx_t count, const LogicalType &type, Vector &typed) {
	UnifiedVectorFormat vdata;
	canonical.ToUnifiedFormat(vdata);
	auto data = UnifiedVectorFormat::GetData<string_t>(vdata);
	uint32_t scale = 0;
	bool bounded = false;
	uhugeint_t limit;
	if (type.id() == LogicalTypeId::DECIMAL) {
		scale = DecimalType::GetScale(type);
		auto width = DecimalType::GetWidth(type);
		if (width < Hugeint::CACHED_POWERS_OF_TEN) {
			bounded = true;
			limit = PowerOfTen(width);
		}
	}
	typed.SetVectorType(VectorType::FLAT_VECTOR);
	auto writer = FlatVector::Writer<T>(typed, count);
	for (idx_t i = 0; i < count; i++) {
		auto idx = vdata.sel->get_index(i);
		if (!vdata.validity.RowIsValid(idx)) {
			writer.WriteNull();
			continue;
		}
		auto view = ParseBlob(data[idx]);
		if (view.zero) {
			writer.WriteValue(T(0));
			continue;
		}
		uhugeint_t magnitude;
		T value;
		if (!TryGetMagnitude(view, scale, magnitude) || (bounded && magnitude >= limit) ||
		    !FromMagnitude<T>::Get(magnitude, view.negative, value)) {
			// a value that does not fit: the rows written so far are abandoned
			writer.Truncate();
			return false;
		}
		writer.WriteValue(value);
	}
	return true;
}

} // namespace

//===--------------------------------------------------------------------===//
// Layout
//===--------------------------------------------------------------------===//
idx_t NumericBlob::BlobSize(const_data_ptr_t data, idx_t available) {
	if (available == 0) {
		throw InternalException("Empty NUMERIC blob");
	}
	if (data[0] == NUMERIC_CLASS_ZERO) {
		return 1;
	}
	auto terminator = data[0] == NUMERIC_CLASS_NEGATIVE ? data_t(0xFF) : data_t(0);
	idx_t pos = 3;
	while (pos < available && data[pos] != terminator) {
		pos++;
	}
	if (pos >= available) {
		throw InternalException("Truncated NUMERIC blob");
	}
	return pos + 1;
}

//===--------------------------------------------------------------------===//
// Text
//===--------------------------------------------------------------------===//
bool NumericBlob::TryEncode(const char *text, idx_t size, string &result) {
	idx_t begin = 0;
	idx_t end = size;
	while (begin < end && StringUtil::CharacterIsSpace(text[begin])) {
		begin++;
	}
	while (end > begin && StringUtil::CharacterIsSpace(text[end - 1])) {
		end--;
	}
	bool negative = false;
	if (begin < end && (text[begin] == '-' || text[begin] == '+')) {
		negative = text[begin] == '-';
		begin++;
	}
	string all_digits;
	int64_t integer_digits = 0;
	bool seen_point = false;
	for (idx_t i = begin; i < end; i++) {
		auto c = text[i];
		if (StringUtil::CharacterIsDigit(c)) {
			all_digits.push_back(c);
			if (!seen_point) {
				integer_digits++;
			}
		} else if (c == '.' && !seen_point) {
			seen_point = true;
		} else if ((c == 'e' || c == 'E') && !all_digits.empty()) {
			// scientific notation moves the decimal point
			int64_t exponent;
			if (!NumericType::TryParseExponent(text, i + 1, end, exponent)) {
				return false;
			}
			integer_digits += exponent;
			break;
		} else {
			return false;
		}
	}
	if (all_digits.empty()) {
		return false;
	}
	idx_t first = 0;
	while (first < all_digits.size() && all_digits[first] == '0') {
		first++;
	}
	if (first == all_digits.size()) {
		result.assign(1, char(NUMERIC_CLASS_ZERO));
		return true;
	}
	idx_t last = all_digits.size();
	while (last > first && all_digits[last - 1] == '0') {
		last--;
	}
	// the decimal exponent of the first significant digit: "123.45" -> 2, "0.0012" -> -3
	int64_t adjusted = integer_digits - 1 - int64_t(first);
	if (adjusted < -NUMERIC_EXPONENT_BIAS || adjusted >= NUMERIC_EXPONENT_BIAS) {
		return false;
	}
	data_t header[3];
	BlobHeader::Write(header, negative, adjusted);
	result.clear();
	result.reserve(4 + (last - first));
	result.append(const_char_ptr_cast(header), 3);
	for (idx_t i = first; i < last; i++) {
		auto b = data_t(all_digits[i] - '0' + 1);
		result.push_back(char(negative ? data_t(~b) : b));
	}
	result.push_back(char(negative ? data_t(0xFF) : data_t(0)));
	return true;
}

string NumericBlob::Encode(const string &text) {
	string result;
	if (!TryEncode(text.c_str(), text.size(), result)) {
		throw ConversionException("Cannot convert '%s' to NUMERIC: not an exact number", text);
	}
	return result;
}

string NumericBlob::ToText(const string_t &blob) {
	auto view = ParseBlob(blob);
	if (view.zero) {
		return "0";
	}
	string text;
	if (view.negative) {
		text.push_back('-');
	}
	auto digit = [&](idx_t i) {
		return char('0' + view.Digit(i));
	};
	if (view.adjusted >= 0) {
		auto integer_length = idx_t(view.adjusted) + 1;
		for (idx_t i = 0; i < view.digit_count && i < integer_length; i++) {
			text.push_back(digit(i));
		}
		if (view.digit_count <= integer_length) {
			text.append(integer_length - view.digit_count, '0');
		} else {
			text.push_back('.');
			for (idx_t i = integer_length; i < view.digit_count; i++) {
				text.push_back(digit(i));
			}
		}
	} else {
		text += "0.";
		text.append(idx_t(-view.adjusted) - 1, '0');
		for (idx_t i = 0; i < view.digit_count; i++) {
			text.push_back(digit(i));
		}
	}
	return text;
}

void NumericBlob::Digits(const string_t &blob, idx_t &integer_digits, idx_t &fractional_digits) {
	auto view = ParseBlob(blob);
	if (view.zero) {
		integer_digits = 0;
		fractional_digits = 0;
		return;
	}
	if (view.adjusted >= 0) {
		integer_digits = idx_t(view.adjusted) + 1;
		fractional_digits = view.digit_count > integer_digits ? view.digit_count - integer_digits : 0;
	} else {
		integer_digits = 0;
		fractional_digits = idx_t(-view.adjusted) - 1 + view.digit_count;
	}
}

bool NumericBlob::TryDecodeUnscaled(const string_t &blob, hugeint_t &unscaled, uint32_t &scale) {
	auto view = ParseBlob(blob);
	if (view.zero) {
		unscaled = 0;
		scale = 0;
		return true;
	}
	idx_t integer_digits, fractional_digits;
	Digits(blob, integer_digits, fractional_digits);
	if (fractional_digits > Decimal::MAX_WIDTH_INT128) {
		return false;
	}
	scale = NumericCast<uint32_t>(fractional_digits);
	uhugeint_t magnitude;
	if (!TryGetMagnitude(view, scale, magnitude)) {
		return false;
	}
	return FromMagnitude<hugeint_t>::Get(magnitude, view.negative, unscaled);
}

idx_t NumericBlob::EncodeUnscaled(hugeint_t unscaled, uint32_t scale, data_ptr_t buffer) {
	bool negative;
	auto magnitude = Magnitude<hugeint_t>::Get(unscaled, negative);
	return EncodeMagnitude(magnitude, negative, scale, buffer);
}

string_t NumericBlob::AddUnscaled(Vector &result, hugeint_t unscaled, uint32_t scale) {
	D_ASSERT(result.GetType().id() == LogicalTypeId::NUMERIC);
	data_t buffer[NUMERIC_MAX_BLOB_SIZE];
	auto length = EncodeUnscaled(unscaled, scale, buffer);
	return StringVector::AddStringOrBlob(result, const_char_ptr_cast(buffer), length);
}

bool NumericBlob::TryRangeType(Vector &canonical, idx_t count, LogicalType &type) {
	UnifiedVectorFormat vdata;
	canonical.ToUnifiedFormat(vdata);
	auto data = UnifiedVectorFormat::GetData<string_t>(vdata);
	idx_t integer_digits = 0;
	idx_t fractional_digits = 0;
	bool found = false;
	for (idx_t i = 0; i < count; i++) {
		auto idx = vdata.sel->get_index(i);
		if (!vdata.validity.RowIsValid(idx)) {
			continue;
		}
		idx_t value_integer_digits, value_fractional_digits;
		Digits(data[idx], value_integer_digits, value_fractional_digits);
		integer_digits = MaxValue(integer_digits, value_integer_digits);
		fractional_digits = MaxValue(fractional_digits, value_fractional_digits);
		found = true;
	}
	if (!found) {
		type = NumericType::DefaultType();
		return true;
	}
	return NumericType::TryForDigits(integer_digits, fractional_digits, type);
}

LogicalType NumericBlob::RangeType(Vector &canonical, idx_t count) {
	LogicalType type;
	if (!TryRangeType(canonical, count, type)) {
		throw OutOfRangeException("NUMERIC values with no common representation: they need more than %d digits",
		                          Decimal::MAX_WIDTH_DECIMAL);
	}
	return type;
}

string NumericBlob::NormalizeText(const string &text) {
	auto point = text.find('.');
	if (point == string::npos) {
		return text;
	}
	idx_t end = text.size();
	while (end > point + 1 && text[end - 1] == '0') {
		end--;
	}
	if (end == point + 1) {
		end = point;
	}
	auto result = text.substr(0, end);
	if (result == "-0") {
		return "0";
	}
	return result;
}

//===--------------------------------------------------------------------===//
// Vectors
//===--------------------------------------------------------------------===//
static uint32_t ScaleOf(const LogicalType &type) {
	return type.id() == LogicalTypeId::DECIMAL ? DecimalType::GetScale(type) : 0;
}

//! Wide decimals (a BIGNUM blob per value) go through their decimal text
static void EncodeThroughText(Vector &typed, idx_t count, Vector &result) {
	Vector text(LogicalType::VARCHAR, count);
	VectorOperations::DefaultCast(typed, text, count, true);
	UnifiedVectorFormat vdata;
	text.ToUnifiedFormat(vdata);
	auto text_data = UnifiedVectorFormat::GetData<string_t>(vdata);
	auto writer = FlatVector::Writer<string_t>(result, count);
	string blob;
	for (idx_t i = 0; i < count; i++) {
		auto idx = vdata.sel->get_index(i);
		if (!vdata.validity.RowIsValid(idx)) {
			writer.WriteNull();
			continue;
		}
		if (!NumericBlob::TryEncode(text_data[idx].GetData(), text_data[idx].GetSize(), blob)) {
			throw InternalException("Exact value '%s' does not encode as NUMERIC", text_data[idx].GetString());
		}
		writer.WriteValue(StringVector::AddStringOrBlob(result, blob.c_str(), blob.size()));
	}
}

void NumericBlob::Encode(Vector &typed, idx_t count, Vector &result) {
	D_ASSERT(result.GetType().id() == LogicalTypeId::NUMERIC);
	D_ASSERT(NumericType::IsExact(typed.GetType()));
	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto scale = ScaleOf(typed.GetType());
	switch (typed.GetType().InternalType()) {
	case PhysicalType::INT8:
		return EncodeTyped<int8_t>(typed, count, scale, result);
	case PhysicalType::INT16:
		return EncodeTyped<int16_t>(typed, count, scale, result);
	case PhysicalType::INT32:
		return EncodeTyped<int32_t>(typed, count, scale, result);
	case PhysicalType::INT64:
		return EncodeTyped<int64_t>(typed, count, scale, result);
	case PhysicalType::INT128:
		return EncodeTyped<hugeint_t>(typed, count, scale, result);
	case PhysicalType::UINT8:
		return EncodeTyped<uint8_t>(typed, count, scale, result);
	case PhysicalType::UINT16:
		return EncodeTyped<uint16_t>(typed, count, scale, result);
	case PhysicalType::UINT32:
		return EncodeTyped<uint32_t>(typed, count, scale, result);
	case PhysicalType::UINT64:
		return EncodeTyped<uint64_t>(typed, count, scale, result);
	case PhysicalType::UINT128:
		return EncodeTyped<uhugeint_t>(typed, count, scale, result);
	default:
		return EncodeThroughText(typed, count, result);
	}
}

static void DecodeThroughText(Vector &canonical, idx_t count, Vector &typed) {
	UnifiedVectorFormat vdata;
	canonical.ToUnifiedFormat(vdata);
	auto data = UnifiedVectorFormat::GetData<string_t>(vdata);
	Vector text(LogicalType::VARCHAR, count);
	{
		auto writer = FlatVector::Writer<string_t>(text, count);
		for (idx_t i = 0; i < count; i++) {
			auto idx = vdata.sel->get_index(i);
			if (!vdata.validity.RowIsValid(idx)) {
				writer.WriteNull();
				continue;
			}
			auto value_text = NumericBlob::ToText(data[idx]);
			writer.WriteValue(StringVector::AddString(text, value_text.c_str(), value_text.size()));
		}
	}
	VectorOperations::DefaultCast(text, typed, count, false);
}

bool NumericBlob::TryDecode(Vector &canonical, idx_t count, Vector &typed) {
	D_ASSERT(canonical.GetType().id() == LogicalTypeId::NUMERIC);
	if (canonical.GetVectorType() == VectorType::CONSTANT_VECTOR && count > 1) {
		if (!TryDecode(canonical, 1, typed)) {
			return false;
		}
		typed.SetVectorType(VectorType::CONSTANT_VECTOR);
		return true;
	}
	auto &type = typed.GetType();
	switch (type.InternalType()) {
	case PhysicalType::INT8:
		return TryDecodeTyped<int8_t>(canonical, count, type, typed);
	case PhysicalType::INT16:
		return TryDecodeTyped<int16_t>(canonical, count, type, typed);
	case PhysicalType::INT32:
		return TryDecodeTyped<int32_t>(canonical, count, type, typed);
	case PhysicalType::INT64:
		return TryDecodeTyped<int64_t>(canonical, count, type, typed);
	case PhysicalType::INT128:
		return TryDecodeTyped<hugeint_t>(canonical, count, type, typed);
	case PhysicalType::UINT8:
		return TryDecodeTyped<uint8_t>(canonical, count, type, typed);
	case PhysicalType::UINT16:
		return TryDecodeTyped<uint16_t>(canonical, count, type, typed);
	case PhysicalType::UINT32:
		return TryDecodeTyped<uint32_t>(canonical, count, type, typed);
	case PhysicalType::UINT64:
		return TryDecodeTyped<uint64_t>(canonical, count, type, typed);
	case PhysicalType::UINT128:
		return TryDecodeTyped<uhugeint_t>(canonical, count, type, typed);
	default:
		return false;
	}
}

void NumericBlob::Decode(Vector &canonical, idx_t count, Vector &typed) {
	if (TryDecode(canonical, count, typed)) {
		return;
	}
	// values that do not fit the representation (or wide decimals) take the checked text cast
	DecodeThroughText(canonical, count, typed);
}

} // namespace duckdb
