#include "duckdb/main/context.hpp"
#include "duckdb/common/bignum.hpp"
#include "duckdb/common/bignum_limbs.hpp"
#include "duckdb/common/types/bignum.hpp"
#include "duckdb/common/bit_utils.hpp"
#include "duckdb/common/bswap.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/common/printer.hpp"
#include "duckdb/common/to_string.hpp"
#include "duckdb/common/vector.hpp"
#include <algorithm>

namespace duckdb {
void PrintBits(const char value) {
	string result;
	for (int i = 7; i >= 0; --i) {
		result += to_string((value >> i) & 1);
	}
	Printer::RawPrint(OutputStream::STREAM_STDOUT, result);
}

void bignum_t::Print() const {
	auto ptr = data.GetData();
	auto length = data.GetSize();
	string result;
	for (idx_t i = 0; i < length; ++i) {
		for (int j = 7; j >= 0; --j) {
			result += to_string((ptr[i] >> j) & 1);
		}
		result += "  ";
	}
	Printer::Print(OutputStream::STREAM_STDOUT, result);
}

void BignumIntermediate::Print() const {
	string result;
	for (idx_t i = 0; i < size; ++i) {
		for (int j = 7; j >= 0; --j) {
			result += to_string((data[i] >> j) & 1);
		}
		result += "  ";
	}
	Printer::Print(OutputStream::STREAM_STDOUT, result);
}

BignumIntermediate::BignumIntermediate(const bignum_t &value) {
	if (value.data.GetSize() < Bignum::BIGNUM_HEADER_SIZE + 1) {
		throw DataCorruptionException("Invalid BIGNUM blob: size %llu is smaller than the minimum of %llu bytes",
		                              static_cast<uint64_t>(value.data.GetSize()),
		                              static_cast<uint64_t>(Bignum::BIGNUM_HEADER_SIZE + 1));
	}
	is_negative = (value.data.GetData()[0] & 0x80) == 0;
	data = reinterpret_cast<data_ptr_t>(value.data.GetDataWriteable() + Bignum::BIGNUM_HEADER_SIZE);
	size = static_cast<uint32_t>(value.data.GetSize()) - Bignum::BIGNUM_HEADER_SIZE;
}

BignumIntermediate::BignumIntermediate(uint8_t *value, idx_t ptr_size) {
	if (ptr_size < Bignum::BIGNUM_HEADER_SIZE + 1) {
		throw DataCorruptionException("Invalid BIGNUM blob: size %llu is smaller than the minimum of %llu bytes",
		                              static_cast<uint64_t>(ptr_size),
		                              static_cast<uint64_t>(Bignum::BIGNUM_HEADER_SIZE + 1));
	}
	is_negative = (value[0] & 0x80) == 0;
	data = value + Bignum::BIGNUM_HEADER_SIZE;
	size = static_cast<uint32_t>(ptr_size) - Bignum::BIGNUM_HEADER_SIZE;
}

uint8_t BignumIntermediate::GetAbsoluteByte(int64_t index) const {
	if (index < 0) {
		// byte-extension
		return 0;
	}
	return is_negative ? static_cast<uint8_t>(~data[index]) : static_cast<uint8_t>(data[index]);
}

//! The absolute-value limb ending just before end_idx of a value, see BignumLimb::LoadAbsolute
static inline uint64_t LoadAbsoluteLimb(const BignumIntermediate &value, int64_t end_idx) {
	return BignumLimb::LoadAbsolute(value.data, value.is_negative, end_idx);
}

//! Stores the low min(end_idx, 8) bytes of a limb as big-endian bytes ending just before end_idx
static inline void StoreLimb(data_ptr_t result, int64_t end_idx, uint64_t limb) {
	if (end_idx >= 8) {
		BignumLimb::StoreBE(result + end_idx - 8, limb);
		return;
	}
	BignumLimb::StoreBEPartial(result, static_cast<idx_t>(end_idx), limb);
}

//! The number of leading bytes of data[0, count) equal to the fill byte (0x00 or 0xFF), eight bytes at a time
static idx_t CountLeadingFillBytes(const_data_ptr_t data, idx_t count, bool fill_is_ones) {
	const uint8_t fill_byte = fill_is_ones ? 0xFF : 0x00;
	if (count == 0 || data[0] != fill_byte) {
		// the common case: a trimmed value has no leading fill bytes at all
		return 0;
	}
	const uint64_t fill = fill_is_ones ? ~uint64_t(0) : 0;
	idx_t pos = 0;
	for (; pos + 8 <= count; pos += 8) {
		const uint64_t limb = BignumLimb::LoadBE(data + pos);
		if (limb != fill) {
			// the first byte that differs is the most significant differing byte of the limb
			return pos + BignumLimb::LeadingZeros(limb ^ fill) / 8;
		}
	}
	while (pos < count && data[pos] == fill_byte) {
		pos++;
	}
	return pos;
}

AbsoluteNumberComparison BignumIntermediate::IsAbsoluteBigger(const BignumIntermediate &rhs) const {
	const int64_t actual_size = size - GetStartDataPos();
	const int64_t rhs_actual_size = rhs.size - rhs.GetStartDataPos();

	// we have opposing signs, gotta do a bunch of checks to figure out who is the biggest
	// check sizes
	if (actual_size > rhs_actual_size) {
		return GREATER;
	}
	if (actual_size < rhs_actual_size) {
		return SMALLER;
	}
	// same magnitude length: compare limbs from the most significant end; both values are right-aligned, so the limb
	// ending `end` bytes after the start of the magnitude covers the same digits in both (bytes before the magnitude
	// read as zero in LoadAbsoluteLimb, so the first, partial limb needs no special casing)
	const int64_t lhs_pad = size - actual_size;
	const int64_t rhs_pad = rhs.size - rhs_actual_size;
	const int64_t top_limb_bytes = actual_size % 8 == 0 ? 8 : actual_size % 8;
	for (int64_t end = top_limb_bytes; end <= actual_size; end += 8) {
		const uint64_t lhs_limb = LoadAbsoluteLimb(*this, lhs_pad + end);
		const uint64_t rhs_limb = LoadAbsoluteLimb(rhs, rhs_pad + end);
		if (lhs_limb != rhs_limb) {
			return lhs_limb > rhs_limb ? GREATER : SMALLER;
		}
	}
	// If we got here, the values are equal.
	return EQUAL;
}

bool BignumIntermediate::IsMSBSet() const {
	if (is_negative) {
		return (data[0] & 0x80) == 0;
	}
	return (data[0] & 0x80) != 0;
}

void BignumIntermediate::Initialize(ArenaAllocator &allocator) {
	is_negative = false;
	size = 1;
	data = allocator.Allocate(size);
	// initialize the data
	data[0] = 0;
}

uint32_t BignumIntermediate::GetStartDataPos(data_ptr_t data, idx_t size, bool is_negative) {
	return static_cast<uint32_t>(CountLeadingFillBytes(data, size, is_negative));
}

uint32_t BignumIntermediate::GetStartDataPos() const {
	return GetStartDataPos(data, size, is_negative);
}

void BignumIntermediate::Reallocate(ArenaAllocator &allocator, idx_t min_size) {
	if (min_size < size) {
		return;
	}
	uint32_t new_size = size;
	while (new_size <= min_size) {
		new_size *= 2;
	}
	auto new_data = allocator.Allocate(new_size);
	// Then we initialize to 0's until we have valid data again
	memset(new_data, is_negative ? 0xFF : 0x00, new_size - size);
	// Copy the old data to the new data
	memcpy(new_data + new_size - size, data, size);
	// Set size and pointer
	data = new_data;
	size = new_size;
}

idx_t BignumIntermediate::Trim(data_ptr_t data, uint32_t &size, bool is_negative) {
	auto actual_start = GetStartDataPos(data, size, is_negative);
	if (actual_start == 0) {
		return 0;
	}
	// This bad-boy is wearing shoe lifts, time to prune it.
	D_ASSERT(actual_start <= size);
	size -= actual_start;
	if (size == 0) {
		// Always keep at least one byte
		actual_start = 0;
		size++;
	}
	memmove(data, data + actual_start, size);
	return actual_start;
}

void BignumIntermediate::Trim() {
	Trim(data, size, is_negative);
}

bool BignumIntermediate::OverOrUnderflow(data_ptr_t data, idx_t size, bool is_negative) {
	if (size <= Bignum::MAX_DATA_SIZE) {
		return false;
	}
	// the bytes above the maximum size must all be fill bytes, otherwise a bit is set that cannot be represented
	const idx_t excess = size - Bignum::MAX_DATA_SIZE;
	return CountLeadingFillBytes(data, excess, is_negative) < excess;
}

bool BignumIntermediate::OverOrUnderflow() const {
	return OverOrUnderflow(data, size, is_negative);
}

bignum_t BignumIntermediate::ToBignum(ArenaAllocator &allocator) const {
	// The result is trimmed like Trim() would, but the intermediate is only read: aggregate states are exported
	// while other threads may read them and while later updates still rely on the current representation
	auto trimmed_start = GetStartDataPos(data, size, is_negative);
	uint32_t trimmed_size = size - trimmed_start;
	if (trimmed_size == 0) {
		// Always keep at least one byte
		trimmed_start = 0;
		trimmed_size = 1;
	}
	bignum_t result;
	uint32_t bignum_size = Bignum::BIGNUM_HEADER_SIZE + trimmed_size;
	auto ptr = reinterpret_cast<char *>(allocator.Allocate(bignum_size));
	// Set Header
	Bignum::SetHeader(ptr, trimmed_size, is_negative);
	// Copy data
	memcpy(ptr + Bignum::BIGNUM_HEADER_SIZE, data + trimmed_start, trimmed_size);
	result.data = string_t(ptr, bignum_size);
	return result;
}

//! result = |lhs| +- |rhs| as a magnitude of result_end bytes, written in the encoding of the result sign.
//! rhs_negative is the sign rhs takes part with (it differs from rhs.is_negative when rhs is subtracted); the
//! magnitude bytes of rhs are still decoded with the sign they are stored with
static void BignumAddition(data_ptr_t result, int64_t result_end, bool is_target_absolute_bigger,
                           const BignumIntermediate &lhs, const BignumIntermediate &rhs, bool rhs_negative) {
	const bool is_result_negative = is_target_absolute_bigger ? lhs.is_negative : rhs_negative;
	const bool same_sign = lhs.is_negative == rhs_negative;
	// the operands are right-aligned with the result: the byte ending at result index e ends at e - offset in them
	const int64_t lhs_offset = result_end - static_cast<int64_t>(lhs.size);
	const int64_t rhs_offset = result_end - static_cast<int64_t>(rhs.size);
	const uint64_t flip = is_result_negative ? ~uint64_t(0) : 0;

	uint64_t carry = 0;
	// walk the magnitudes from the least significant end, 8 bytes at a time
	for (int64_t end = result_end; end > 0; end -= 8) {
		const uint64_t a = LoadAbsoluteLimb(lhs, end - lhs_offset);
		const uint64_t b = LoadAbsoluteLimb(rhs, end - rhs_offset);
		uint64_t sum;
		if (same_sign) {
			sum = a + b;
			const uint64_t c1 = sum < a;
			sum += carry;
			carry = c1 | (sum < carry);
		} else {
			const uint64_t big = is_target_absolute_bigger ? a : b;
			const uint64_t small = is_target_absolute_bigger ? b : a;
			sum = big - small;
			const uint64_t b1 = big < small;
			const uint64_t b2 = sum < carry;
			sum -= carry;
			carry = b1 | b2;
		}
		StoreLimb(result, end, sum ^ flip);
	}
}

//! Writes the bitwise complement of count bytes, eight at a time
static void ComplementBytes(data_ptr_t target, const_data_ptr_t source, idx_t count) {
	idx_t pos = 0;
	for (; pos + 8 <= count; pos += 8) {
		uint64_t limb;
		memcpy(&limb, source + pos, sizeof(limb));
		limb = ~limb;
		memcpy(target + pos, &limb, sizeof(limb));
	}
	for (; pos < count; pos++) {
		target[pos] = static_cast<data_t>(~source[pos]);
	}
}

string_t BignumIntermediate::Negate(StringHeap &heap) const {
	auto target = heap.EmptyString(size + Bignum::BIGNUM_HEADER_SIZE);
	auto ptr = target.GetDataWriteable();
	auto target_data = reinterpret_cast<data_ptr_t>(ptr + Bignum::BIGNUM_HEADER_SIZE);

	if (!is_negative && size == 1 && data[0] == 0x00) {
		// If we have a zero, we just do a copy
		Bignum::SetHeader(ptr, size, is_negative);
		memcpy(target_data, data, size);
	} else {
		// Otherwise, we set the header with a flip on the signal and flip all the data bits
		Bignum::SetHeader(ptr, size, !is_negative);
		ComplementBytes(target_data, data, size);
	}
	target.Finalize();
	return target;
}

void BignumIntermediate::NegateInPlace() {
	if (!is_negative && size == 1 && data[0] == 0x00) {
		// this is a zero, there is no negation
		return;
	}
	is_negative = !is_negative;
	ComplementBytes(data, data, size);
}

string ProduceOverUnderFlowError(bool is_result_negative, idx_t actual_start, idx_t data_size) {
	// We must throw an error, usually we should print the numbers, but I have a feeling that it won't be possible
	// here.
	std::ostringstream error;
	if (is_result_negative) {
		error << "Underflow ";
	} else {
		error << "Overflow ";
	}
	error << "in Bignum Operation. A Bignum can hold max " << Bignum::MAX_DATA_SIZE
	      << " data bytes. Current bignum has " << data_size - actual_start << " bytes.";
	return error.str();
}

//! Operands of at most this many magnitude bytes are added in registers
static constexpr uint32_t SMALL_ADD_MAX_BYTES = 2 * BignumLimb::BYTES;

//! Writes a positive zero as a BIGNUM blob into the heap
static string_t WriteZero(StringHeap &heap) {
	auto target = heap.EmptyString(1 + Bignum::BIGNUM_HEADER_SIZE);
	auto ptr = target.GetDataWriteable();
	Bignum::SetHeader(ptr, 1, false);
	ptr[Bignum::BIGNUM_HEADER_SIZE] = 0;
	target.Finalize();
	return target;
}

//! Adds two values of at most SMALL_ADD_MAX_BYTES magnitude bytes each: both magnitudes fit two limbs, so the sum is
//! computed in registers and encoded once, trimmed, without the general path's scratch result and trim pass
static string_t AddSmall(StringHeap &heap, const BignumIntermediate &lhs, const BignumIntermediate &rhs,
                         bool rhs_negative) {
	D_ASSERT(lhs.size <= SMALL_ADD_MAX_BYTES && rhs.size <= SMALL_ADD_MAX_BYTES);
	const uint64_t a_lo = LoadAbsoluteLimb(lhs, lhs.size);
	const uint64_t a_hi = LoadAbsoluteLimb(lhs, static_cast<int64_t>(lhs.size) - 8);
	const uint64_t b_lo = LoadAbsoluteLimb(rhs, rhs.size);
	const uint64_t b_hi = LoadAbsoluteLimb(rhs, static_cast<int64_t>(rhs.size) - 8);

	bool is_negative = lhs.is_negative;
	uint64_t lo;
	uint64_t hi;
	// the sum of two 128-bit magnitudes needs a 129th bit
	uint64_t top = 0;
	if (lhs.is_negative == rhs_negative) {
		lo = a_lo + b_lo;
		const uint64_t carry = lo < a_lo;
		hi = a_hi + b_hi;
		top = hi < a_hi;
		hi += carry;
		top |= hi < carry;
	} else {
		if (a_hi == b_hi && a_lo == b_lo) {
			return WriteZero(heap);
		}
		const bool lhs_bigger = a_hi > b_hi || (a_hi == b_hi && a_lo > b_lo);
		const uint64_t big_lo = lhs_bigger ? a_lo : b_lo;
		const uint64_t big_hi = lhs_bigger ? a_hi : b_hi;
		const uint64_t small_lo = lhs_bigger ? b_lo : a_lo;
		const uint64_t small_hi = lhs_bigger ? b_hi : a_hi;
		lo = big_lo - small_lo;
		hi = big_hi - small_hi - (big_lo < small_lo);
		is_negative = lhs_bigger ? lhs.is_negative : rhs_negative;
	}

	// encode the magnitude in as many bytes as it needs, complemented for a negative result
	const idx_t byte_count = top ? 2 * BignumLimb::BYTES + 1 : BignumLimb::ByteCount(hi, lo);
	auto target = heap.EmptyString(byte_count + Bignum::BIGNUM_HEADER_SIZE);
	auto ptr = target.GetDataWriteable();
	Bignum::SetHeader(ptr, byte_count, is_negative);
	auto out = reinterpret_cast<data_ptr_t>(ptr + Bignum::BIGNUM_HEADER_SIZE);
	if (top) {
		// the 129th bit is the most significant byte, the two limbs follow in full
		*out++ = static_cast<data_t>(is_negative ? ~top : top);
		BignumLimb::StoreMagnitude(out, 2 * BignumLimb::BYTES, hi, lo, is_negative);
	} else {
		BignumLimb::StoreMagnitude(out, byte_count, hi, lo, is_negative);
	}
	target.Finalize();
	return target;
}

string_t BignumIntermediate::Add(StringHeap &heap, const BignumIntermediate &lhs, const BignumIntermediate &rhs,
                                 bool negate_rhs) {
	const bool rhs_negative = rhs.is_negative != negate_rhs;
	if (lhs.size <= SMALL_ADD_MAX_BYTES && rhs.size <= SMALL_ADD_MAX_BYTES) {
		return AddSmall(heap, lhs, rhs, rhs_negative);
	}
	const bool same_sign = lhs.is_negative == rhs_negative;
	const uint32_t actual_size = lhs.size - lhs.GetStartDataPos();
	const uint32_t actual_rhs_size = rhs.size - rhs.GetStartDataPos();
	uint32_t result_size = actual_size;
	if (actual_size < actual_rhs_size || (same_sign && (lhs.IsMSBSet() || (rhs.IsMSBSet() && lhs.size == rhs.size)))) {
		result_size = actual_size < actual_rhs_size ? actual_rhs_size + 1 : actual_size + 1;
	}
	bool is_target_absolute_bigger = true;
	if (result_size == 0) {
		result_size++;
	}
	result_size += Bignum::BIGNUM_HEADER_SIZE;
	if (!same_sign) {
		auto is_absolute_bigger = lhs.IsAbsoluteBigger(rhs);
		if (is_absolute_bigger == EQUAL) {
			// We set this value to 0
			auto target = heap.EmptyString(result_size);
			auto target_data = target.GetDataWriteable();
			Bignum::SetHeader(target_data, 1, false);
			target_data[Bignum::BIGNUM_HEADER_SIZE] = 0;
			target.SetSizeAndFinalize(1 + Bignum::BIGNUM_HEADER_SIZE, result_size);
			return target;

		} else if (is_absolute_bigger == SMALLER) {
			is_target_absolute_bigger = false;
		}
	}

	auto target = heap.EmptyString(result_size);
	auto result_size_data = result_size - Bignum::BIGNUM_HEADER_SIZE;

	auto target_data = target.GetDataWriteable();
	BignumAddition(reinterpret_cast<data_ptr_t>(target_data + Bignum::BIGNUM_HEADER_SIZE), result_size_data,
	               is_target_absolute_bigger, lhs, rhs, rhs_negative);
	bool is_result_negative = is_target_absolute_bigger ? lhs.is_negative : rhs_negative;
	if (OverOrUnderflow(reinterpret_cast<data_ptr_t>(target_data + Bignum::BIGNUM_HEADER_SIZE), result_size_data,
	                    is_result_negative)) {
		auto actual_start = GetStartDataPos(reinterpret_cast<data_ptr_t>(target_data + Bignum::BIGNUM_HEADER_SIZE),
		                                    result_size_data, is_result_negative);
		throw OutOfRangeException(ProduceOverUnderFlowError(is_result_negative, actual_start, result_size_data));
	}
	Trim(reinterpret_cast<data_ptr_t>(target_data + Bignum::BIGNUM_HEADER_SIZE), result_size_data, is_result_negative);
	Bignum::SetHeader(target_data, result_size_data, is_result_negative);
	target.SetSizeAndFinalize(result_size_data + Bignum::BIGNUM_HEADER_SIZE, result_size);
	return target;
}

void BignumIntermediate::AddInPlace(ArenaAllocator &allocator, const BignumIntermediate &rhs, bool negate_rhs) {
	const bool rhs_negative = rhs.is_negative != negate_rhs;
	const bool same_sign = is_negative == rhs_negative;
	idx_t actual_size = size - GetStartDataPos();
	idx_t actual_rhs_size = rhs.size - rhs.GetStartDataPos();
	if (actual_size < actual_rhs_size || (same_sign && (IsMSBSet() || (rhs.IsMSBSet() && size == rhs.size)))) {
		// We must reallocate
		idx_t min_size = actual_size < actual_rhs_size ? actual_rhs_size + 1 : size + 1;
		Reallocate(allocator, min_size);
	}
	bool is_target_absolute_bigger = true;
	if (!same_sign) {
		auto is_absolute_bigger = IsAbsoluteBigger(rhs);
		if (is_absolute_bigger == EQUAL) {
			// We set this value to 0
			*this = BignumIntermediate();
			Initialize(allocator);
			return;
		} else if (is_absolute_bigger == SMALLER) {
			is_target_absolute_bigger = false;
		}
	}

	bool is_result_negative = is_target_absolute_bigger ? is_negative : rhs_negative;
	BignumAddition(data, size, is_target_absolute_bigger, *this, rhs, rhs_negative);
	if (is_result_negative != is_negative) {
		is_negative = is_result_negative;
	}
	if (OverOrUnderflow()) {
		// We must throw an error, usually we should print the numbers, but I have a feeling that it won't be possible
		// here.
		throw OutOfRangeException(ProduceOverUnderFlowError(is_result_negative, GetStartDataPos(), size));
	}
}

//===--------------------------------------------------------------------===//
// Multiplication / Division
//===--------------------------------------------------------------------===//
namespace {

//! Little-endian base 2^DIGIT_BITS digits of an absolute value without leading zero digits; zero is the empty vector
using bignum_magnitude_t = unsafe_vector<bignum_digit_t>;

constexpr idx_t DIGIT_BYTES = BignumLimb::DIGIT_BYTES;
constexpr idx_t DIGIT_BITS = BignumLimb::DIGIT_BITS;
constexpr bignum_twodigit_t BASE = static_cast<bignum_twodigit_t>(1) << DIGIT_BITS;
constexpr bignum_twodigit_t DIGIT_MASK = BASE - 1;

//! Digit operations between two interrupt checks, roughly a millisecond of work
constexpr idx_t INTERRUPT_CHECK_INTERVAL = idx_t(1) << 20;

void AccountWork(BignumArithmeticState &state, idx_t work) {
	state.work_since_interrupt_check += work;
	if (state.work_since_interrupt_check >= INTERRUPT_CHECK_INTERVAL) {
		state.work_since_interrupt_check = 0;
		state.context.InterruptCheck();
	}
}

void TrimMagnitude(bignum_magnitude_t &digits) {
	while (!digits.empty() && digits.back() == 0) {
		digits.pop_back();
	}
}

void GetMagnitude(const BignumIntermediate &value, bignum_magnitude_t &digits) {
	digits.clear();
	const int64_t start = value.GetStartDataPos();
	const int64_t byte_count = static_cast<int64_t>(value.size) - start;
	digits.reserve((byte_count + DIGIT_BYTES - 1) / DIGIT_BYTES);
	// walk the big-endian magnitude bytes from the least significant end, a digit at a time; the most significant
	// digit may be partial, LoadAbsolute reads the bytes before the magnitude as zero and truncation keeps the low
	// DIGIT_BYTES bytes of the loaded limb
	for (int64_t end = value.size; end > start; end -= DIGIT_BYTES) {
		digits.push_back(static_cast<bignum_digit_t>(BignumLimb::LoadAbsolute(value.data, value.is_negative, end)));
	}
	TrimMagnitude(digits);
}

//! The number of significant bits of a magnitude, 0 for zero
idx_t MagnitudeBitCount(const bignum_magnitude_t &digits) {
	if (digits.empty()) {
		return 0;
	}
	return digits.size() * DIGIT_BITS - BignumLimb::LeadingZeros(digits.back());
}

idx_t BitsToBytes(idx_t bits) {
	return (bits + 7) / 8;
}

//! Writes a (sign, magnitude) pair as a BIGNUM blob into the heap, checking for overflow
string_t WriteMagnitude(StringHeap &heap, const bignum_magnitude_t &digits, bool is_negative) {
	if (digits.empty()) {
		// zero is always encoded as a single positive zero byte
		return WriteZero(heap);
	}
	const idx_t byte_count = BitsToBytes(MagnitudeBitCount(digits));
	if (byte_count > Bignum::MAX_DATA_SIZE) {
		throw OutOfRangeException(ProduceOverUnderFlowError(is_negative, 0, byte_count));
	}
	auto target = heap.EmptyString(byte_count + Bignum::BIGNUM_HEADER_SIZE);
	auto target_data = target.GetDataWriteable();
	Bignum::SetHeader(target_data, byte_count, is_negative);
	auto result = reinterpret_cast<data_ptr_t>(target_data + Bignum::BIGNUM_HEADER_SIZE);
	// the most significant digit only gets the bytes it needs, the others are written in full
	idx_t digit_idx = digits.size() - 1;
	const idx_t top_bytes = byte_count - digit_idx * DIGIT_BYTES;
	BignumLimb::StoreMagnitudePartial(result, top_bytes, digits[digit_idx], is_negative);
	result += top_bytes;
	while (digit_idx > 0) {
		digit_idx--;
		BignumLimb::StoreMagnitudePartial(result, DIGIT_BYTES, digits[digit_idx], is_negative);
		result += DIGIT_BYTES;
	}
	target.Finalize();
	return target;
}

//! Below this many digits in the shorter operand, magnitudes are multiplied with the schoolbook algorithm
constexpr idx_t KARATSUBA_THRESHOLD = 16;

//! out[0..a_size] = a * digit
inline void MultiplyByDigit(BignumArithmeticState &state, const bignum_digit_t *a, idx_t a_size, bignum_digit_t digit,
                            bignum_digit_t *out) {
	AccountWork(state, a_size);
	bignum_twodigit_t carry = 0;
	for (idx_t j = 0; j < a_size; j++) {
		const bignum_twodigit_t product = static_cast<bignum_twodigit_t>(digit) * a[j] + carry;
		out[j] = static_cast<bignum_digit_t>(product);
		carry = product >> DIGIT_BITS;
	}
	out[a_size] = static_cast<bignum_digit_t>(carry);
}

//! Schoolbook multiplication: out[0..a_size + b_size) = a * b for two non-empty operands, with the shorter one in the
//! outer loop; the first row of partial products is written, the others are added
void SchoolbookMultiply(BignumArithmeticState &state, const bignum_digit_t *a, idx_t a_size, const bignum_digit_t *b,
                        idx_t b_size, bignum_digit_t *out) {
	if (a_size < b_size) {
		std::swap(a, b);
		std::swap(a_size, b_size);
	}
	D_ASSERT(b_size >= 1);
	MultiplyByDigit(state, a, a_size, b[0], out);
	for (idx_t i = 1; i < b_size; i++) {
		AccountWork(state, a_size);
		bignum_twodigit_t carry = 0;
		const bignum_twodigit_t b_digit = b[i];
		for (idx_t j = 0; j < a_size; j++) {
			// (BASE - 1)^2 + 2 * (BASE - 1) = BASE^2 - 1, so this cannot overflow
			const bignum_twodigit_t product = b_digit * a[j] + out[i + j] + carry;
			out[i + j] = static_cast<bignum_digit_t>(product);
			carry = product >> DIGIT_BITS;
		}
		out[i + a_size] = static_cast<bignum_digit_t>(carry);
	}
}

//! out[0..a_size) = a + b, where a_size >= b_size; the carry out of the top digit is returned
bignum_digit_t AddDigits(BignumArithmeticState &state, const bignum_digit_t *a, idx_t a_size, const bignum_digit_t *b,
                         idx_t b_size, bignum_digit_t *out) {
	D_ASSERT(a_size >= b_size);
	AccountWork(state, a_size);
	bignum_twodigit_t carry = 0;
	idx_t i = 0;
	for (; i < b_size; i++) {
		const bignum_twodigit_t sum = static_cast<bignum_twodigit_t>(a[i]) + b[i] + carry;
		out[i] = static_cast<bignum_digit_t>(sum);
		carry = sum >> DIGIT_BITS;
	}
	for (; i < a_size; i++) {
		const bignum_twodigit_t sum = static_cast<bignum_twodigit_t>(a[i]) + carry;
		out[i] = static_cast<bignum_digit_t>(sum);
		carry = sum >> DIGIT_BITS;
	}
	return static_cast<bignum_digit_t>(carry);
}

//! out += addend, where the sum fits in out_size digits
void AddInPlace(BignumArithmeticState &state, bignum_digit_t *out, idx_t out_size, const bignum_digit_t *addend,
                idx_t addend_size) {
	D_ASSERT(addend_size <= out_size);
	AccountWork(state, addend_size);
	bignum_twodigit_t carry = 0;
	idx_t i = 0;
	for (; i < addend_size; i++) {
		const bignum_twodigit_t sum = static_cast<bignum_twodigit_t>(out[i]) + addend[i] + carry;
		out[i] = static_cast<bignum_digit_t>(sum);
		carry = sum >> DIGIT_BITS;
	}
	for (; carry; i++) {
		D_ASSERT(i < out_size);
		out[i]++;
		carry = out[i] == 0 ? 1 : 0;
	}
}

//! out -= subtrahend, where out >= subtrahend
void SubtractInPlace(BignumArithmeticState &state, bignum_digit_t *out, idx_t out_size,
                     const bignum_digit_t *subtrahend, idx_t subtrahend_size) {
	D_ASSERT(subtrahend_size <= out_size);
	AccountWork(state, subtrahend_size);
	bignum_digit_t borrow = 0;
	idx_t i = 0;
	for (; i < subtrahend_size; i++) {
		const bignum_digit_t difference = out[i] - subtrahend[i];
		// at most one of the two steps underflows: the difference is non-zero when the first one did
		const bignum_digit_t next_borrow = (out[i] < subtrahend[i] || difference < borrow) ? 1 : 0;
		out[i] = difference - borrow;
		borrow = next_borrow;
	}
	for (; borrow; i++) {
		D_ASSERT(i < out_size);
		borrow = out[i] == 0 ? 1 : 0;
		out[i]--;
	}
}

//! The scratch digits MultiplyDigits needs for operands of up to digit_count digits
idx_t KaratsubaScratchSize(idx_t digit_count) {
	idx_t total = 0;
	while (digit_count >= KARATSUBA_THRESHOLD) {
		// a balanced split sums halves of at most half digits (with their carry digit) and multiplies the sums
		const idx_t half = (digit_count + 1) / 2 + 1;
		total += 4 * half;
		digit_count = half;
	}
	return total;
}

//! out[0..a_size + b_size) = a * b: Karatsuba multiplication down to KARATSUBA_THRESHOLD digits, schoolbook below
//! scratch holds KaratsubaScratchSize(max(a_size, b_size)) digits and may alias neither operand nor out
void MultiplyDigits(BignumArithmeticState &state, const bignum_digit_t *a, idx_t a_size, const bignum_digit_t *b,
                    idx_t b_size, bignum_digit_t *out, bignum_digit_t *scratch) {
	if (a_size < b_size) {
		std::swap(a, b);
		std::swap(a_size, b_size);
	}
	if (b_size < KARATSUBA_THRESHOLD) {
		SchoolbookMultiply(state, a, a_size, b, b_size, out);
		return;
	}
	if (2 * b_size <= a_size) {
		// unbalanced: multiply b with b_size digit chunks of a and add up the partial products
		std::fill(out, out + a_size + b_size, 0);
		auto part = scratch;
		for (idx_t offset = 0; offset < a_size; offset += b_size) {
			const idx_t chunk_size = MinValue<idx_t>(b_size, a_size - offset);
			MultiplyDigits(state, a + offset, chunk_size, b, b_size, part, scratch + 2 * b_size);
			AddInPlace(state, out + offset, a_size + b_size - offset, part, chunk_size + b_size);
		}
		return;
	}
	// a = a1 * BASE^m + a0 and b = b1 * BASE^m + b0, where b1 is not empty because 2 * b_size > a_size
	const idx_t m = a_size / 2;
	const idx_t a1_size = a_size - m;
	const idx_t b1_size = b_size - m;
	// z0 = a0 * b0 and z2 = a1 * b1 fill the low and the high digits of the result
	MultiplyDigits(state, a, m, b, m, out, scratch);
	MultiplyDigits(state, a + m, a1_size, b + m, b1_size, out + 2 * m, scratch);
	// a_sum = a0 + a1 and b_sum = b0 + b1, each followed by its carry digit when there is one
	auto a_sum = scratch;
	idx_t a_sum_size = a1_size;
	a_sum[a_sum_size] = AddDigits(state, a + m, a1_size, a, m, a_sum);
	a_sum_size += a_sum[a_sum_size] ? 1 : 0;
	auto b_sum = a_sum + a1_size + 1;
	idx_t b_sum_size = MaxValue<idx_t>(m, b1_size);
	b_sum[b_sum_size] =
	    b1_size >= m ? AddDigits(state, b + m, b1_size, b, m, b_sum) : AddDigits(state, b, m, b + m, b1_size, b_sum);
	b_sum_size += b_sum[b_sum_size] ? 1 : 0;
	// z1 = (a0 + a1) * (b0 + b1) - z0 - z2 = a0 * b1 + a1 * b0 goes in at digit m
	auto z1 = b_sum + MaxValue<idx_t>(m, b1_size) + 1;
	idx_t z1_size = a_sum_size + b_sum_size;
	MultiplyDigits(state, a_sum, a_sum_size, b_sum, b_sum_size, z1, z1 + z1_size);
	SubtractInPlace(state, z1, z1_size, out, 2 * m);
	SubtractInPlace(state, z1, z1_size, out + 2 * m, a1_size + b1_size);
	while (z1_size > 0 && z1[z1_size - 1] == 0) {
		z1_size--;
	}
	AddInPlace(state, out + m, a_size + b_size - m, z1, z1_size);
}

//! result = lhs * rhs, for two non-empty magnitudes
void MultiplyMagnitudes(BignumArithmeticState &state, const bignum_magnitude_t &lhs, const bignum_magnitude_t &rhs,
                        bignum_magnitude_t &result) {
	result.resize(lhs.size() + rhs.size());
	if (rhs.size() == 1) {
		MultiplyByDigit(state, lhs.data(), lhs.size(), rhs[0], result.data());
	} else if (lhs.size() == 1) {
		MultiplyByDigit(state, rhs.data(), rhs.size(), lhs[0], result.data());
	} else if (MinValue<idx_t>(lhs.size(), rhs.size()) < KARATSUBA_THRESHOLD) {
		SchoolbookMultiply(state, lhs.data(), lhs.size(), rhs.data(), rhs.size(), result.data());
	} else {
		auto &scratch = state.karatsuba_scratch;
		const idx_t scratch_size = KaratsubaScratchSize(MaxValue<idx_t>(lhs.size(), rhs.size()));
		if (scratch.size() < scratch_size) {
			scratch.resize(scratch_size);
		}
		MultiplyDigits(state, lhs.data(), lhs.size(), rhs.data(), rhs.size(), result.data(), scratch.data());
	}
	TrimMagnitude(result);
}

//! Division by a single digit; the remainder is returned
bignum_digit_t DivideByDigit(const bignum_magnitude_t &dividend, bignum_digit_t divisor, bignum_magnitude_t &quotient) {
	const idx_t m = dividend.size();
	quotient.assign(m, 0);
	bignum_digit_t rem = 0;
	constexpr idx_t HALF_BITS = DIGIT_BITS / 2;
	if (divisor >> HALF_BITS == 0) {
		// a divisor that fits half a digit: halve the digits so that every step is a native division of digit width
		// instead of a double-width one, which has no hardware instruction on most platforms
		const bignum_digit_t half_mask = (static_cast<bignum_digit_t>(1) << HALF_BITS) - 1;
		for (idx_t i = m; i > 0; i--) {
			const bignum_digit_t digit = dividend[i - 1];
			const bignum_digit_t high = (rem << HALF_BITS) | (digit >> HALF_BITS);
			const bignum_digit_t q_high = high / divisor;
			const bignum_digit_t low = ((high - q_high * divisor) << HALF_BITS) | (digit & half_mask);
			const bignum_digit_t q_low = low / divisor;
			rem = low - q_low * divisor;
			quotient[i - 1] = (q_high << HALF_BITS) | q_low;
		}
	} else {
		for (idx_t i = m; i > 0; i--) {
			quotient[i - 1] = BignumLimb::DivRem(rem, dividend[i - 1], divisor, rem);
		}
	}
	TrimMagnitude(quotient);
	return rem;
}

//! Long division following Knuth's Algorithm D (TAOCP 4.3.1); the remainder is only produced on request
void DivModMagnitudes(BignumArithmeticState &state, const bignum_magnitude_t &dividend,
                      const bignum_magnitude_t &divisor, bignum_magnitude_t &quotient, bignum_magnitude_t &remainder,
                      bool want_remainder) {
	D_ASSERT(!divisor.empty());
	const idx_t m = dividend.size();
	const idx_t n = divisor.size();
	if (m < n) {
		quotient.clear();
		if (want_remainder) {
			remainder = dividend;
		}
		return;
	}
	if (n == 1) {
		AccountWork(state, m);
		const auto rem = DivideByDigit(dividend, divisor[0], quotient);
		remainder.clear();
		if (want_remainder && rem != 0) {
			remainder.push_back(rem);
		}
		return;
	}

	// D1: normalize so that the most significant divisor digit has its top bit set
	const auto shift = static_cast<uint8_t>(BignumLimb::LeadingZeros(divisor[n - 1]));
	auto &vn = state.normalized_divisor;
	auto &un = state.normalized_dividend;
	vn.assign(n, 0);
	un.assign(m + 1, 0);
	if (shift > 0) {
		for (idx_t i = n - 1; i > 0; i--) {
			vn[i] = (divisor[i] << shift) | (divisor[i - 1] >> (DIGIT_BITS - shift));
		}
		vn[0] = divisor[0] << shift;
		un[m] = dividend[m - 1] >> (DIGIT_BITS - shift);
		for (idx_t i = m - 1; i > 0; i--) {
			un[i] = (dividend[i] << shift) | (dividend[i - 1] >> (DIGIT_BITS - shift));
		}
		un[0] = dividend[0] << shift;
	} else {
		std::copy(divisor.begin(), divisor.end(), vn.begin());
		std::copy(dividend.begin(), dividend.end(), un.begin());
	}

	quotient.assign(m - n + 1, 0);
	for (idx_t j = m - n + 1; j > 0; j--) {
		AccountWork(state, n);
		const idx_t pos = j - 1;
		// D3: estimate the quotient digit; the top dividend digit never exceeds the top divisor digit, so when they
		// are equal the estimate is BASE - 1 (the true digit is at most that) and the remainder follows from it
		bignum_twodigit_t qhat;
		bignum_twodigit_t rhat;
		if (un[pos + n] >= vn[n - 1]) {
			const bignum_twodigit_t numerator =
			    (static_cast<bignum_twodigit_t>(un[pos + n]) << DIGIT_BITS) | un[pos + n - 1];
			qhat = DIGIT_MASK;
			rhat = numerator - qhat * vn[n - 1];
		} else {
			bignum_digit_t rem;
			qhat = BignumLimb::DivRem(un[pos + n], un[pos + n - 1], vn[n - 1], rem);
			rhat = rem;
		}
		while (rhat < BASE && qhat * vn[n - 2] > ((rhat << DIGIT_BITS) | un[pos + n - 2])) {
			qhat--;
			rhat += vn[n - 1];
		}
		// D4: multiply and subtract
		// the borrow is at most BASE: the high half of a product is at most BASE - 2 and we borrow at most 2 units
		bignum_twodigit_t borrow = 0;
		for (idx_t i = 0; i < n; i++) {
			const bignum_twodigit_t product = qhat * vn[i];
			const bignum_twodigit_t subtrahend = (product & DIGIT_MASK) + borrow;
			const bignum_twodigit_t current = un[i + pos];
			if (current >= subtrahend) {
				un[i + pos] = static_cast<bignum_digit_t>(current - subtrahend);
				borrow = product >> DIGIT_BITS;
			} else {
				// the subtrahend is below 2 * BASE, so we need to borrow one or two units from the next digit
				const bignum_twodigit_t deficit = subtrahend - current;
				const bignum_twodigit_t units = (deficit + BASE - 1) >> DIGIT_BITS;
				un[i + pos] = static_cast<bignum_digit_t>(current + units * BASE - subtrahend);
				borrow = (product >> DIGIT_BITS) + units;
			}
		}
		const bignum_twodigit_t top = un[pos + n];
		const bool went_negative = top < borrow;
		un[pos + n] = static_cast<bignum_digit_t>(went_negative ? top + BASE - borrow : top - borrow);
		quotient[pos] = static_cast<bignum_digit_t>(qhat);
		if (went_negative) {
			// D6: the estimate was one too large, add the divisor back
			quotient[pos]--;
			bignum_twodigit_t carry = 0;
			for (idx_t i = 0; i < n; i++) {
				const bignum_twodigit_t sum = static_cast<bignum_twodigit_t>(un[i + pos]) + vn[i] + carry;
				un[i + pos] = static_cast<bignum_digit_t>(sum);
				carry = sum >> DIGIT_BITS;
			}
			un[pos + n] = static_cast<bignum_digit_t>(un[pos + n] + carry);
		}
	}
	TrimMagnitude(quotient);
	if (!want_remainder) {
		remainder.clear();
		return;
	}

	// D8: denormalize the remainder
	remainder.assign(n, 0);
	if (shift > 0) {
		for (idx_t i = 0; i < n; i++) {
			remainder[i] = (un[i] >> shift) | (un[i + 1] << (DIGIT_BITS - shift));
		}
	} else {
		std::copy(un.begin(), un.begin() + static_cast<int64_t>(n), remainder.begin());
	}
	TrimMagnitude(remainder);
}

} // namespace

string_t BignumIntermediate::Multiply(StringHeap &heap, BignumArithmeticState &state, const BignumIntermediate &lhs,
                                      const BignumIntermediate &rhs) {
	const bool is_result_negative = lhs.is_negative != rhs.is_negative;
	auto &lhs_digits = state.lhs_digits;
	auto &rhs_digits = state.rhs_digits;
	auto &product = state.result_digits;
	GetMagnitude(lhs, lhs_digits);
	GetMagnitude(rhs, rhs_digits);
	if (lhs_digits.empty() || rhs_digits.empty()) {
		product.clear();
		return WriteMagnitude(heap, product, false);
	}
	// the product has a + b - 1 or a + b bits: bail out early when even the lower bound overflows
	const idx_t min_result_bytes = BitsToBytes(MagnitudeBitCount(lhs_digits) + MagnitudeBitCount(rhs_digits) - 1);
	if (min_result_bytes > Bignum::MAX_DATA_SIZE) {
		throw OutOfRangeException(ProduceOverUnderFlowError(is_result_negative, 0, min_result_bytes));
	}
	MultiplyMagnitudes(state, lhs_digits, rhs_digits, product);
	return WriteMagnitude(heap, product, is_result_negative);
}

bool BignumIntermediate::TryDivMod(StringHeap &heap, BignumArithmeticState &state, const BignumIntermediate &lhs,
                                   const BignumIntermediate &rhs, bool want_remainder, string_t &result) {
	auto &lhs_digits = state.lhs_digits;
	auto &rhs_digits = state.rhs_digits;
	auto &quotient = state.result_digits;
	auto &remainder = state.remainder_digits;
	GetMagnitude(lhs, lhs_digits);
	GetMagnitude(rhs, rhs_digits);
	if (rhs_digits.empty()) {
		return false;
	}
	DivModMagnitudes(state, lhs_digits, rhs_digits, quotient, remainder, want_remainder);
	if (want_remainder) {
		// the remainder takes the sign of the dividend
		result = WriteMagnitude(heap, remainder, lhs.is_negative);
	} else {
		// truncating division: the quotient is negative when the signs differ
		result = WriteMagnitude(heap, quotient, lhs.is_negative != rhs.is_negative);
	}
	return true;
}

} // namespace duckdb
