#include "duckdb/common/bignum.hpp"
#include "duckdb/common/types/bignum.hpp"
#include "duckdb/common/bit_utils.hpp"
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

AbsoluteNumberComparison BignumIntermediate::IsAbsoluteBigger(const BignumIntermediate &rhs) const {
	idx_t actual_start_pos = GetStartDataPos();
	idx_t actual_size = size - actual_start_pos;

	idx_t rhs_actual_start_pos = rhs.GetStartDataPos();
	idx_t rhs_actual_size = rhs.size - rhs_actual_start_pos;

	// we have opposing signs, gotta do a bunch of checks to figure out who is the biggest
	// check sizes
	if (actual_size > rhs_actual_size) {
		return GREATER;
	}
	if (actual_size < rhs_actual_size) {
		return SMALLER;
	} else {
		// they have the same size then
		idx_t target_idx = actual_start_pos;
		idx_t source_idx = rhs_actual_start_pos;
		while (target_idx < size) {
			auto data_byte = GetAbsoluteByte(static_cast<int64_t>(target_idx));
			auto rhs_byte = rhs.GetAbsoluteByte(static_cast<int64_t>(source_idx));
			if (data_byte > rhs_byte) {
				return GREATER;
			} else if (data_byte < rhs_byte) {
				return SMALLER;
			}
			target_idx++;
			source_idx++;
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
	uint8_t non_initialized = is_negative ? 0xFF : 0x00;
	uint32_t actual_start = 0;
	for (idx_t i = 0; i < size; ++i) {
		if (data[i] == non_initialized) {
			actual_start++;
		} else {
			break;
		}
	}
	return actual_start;
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
	// variable that stores a fully unset byte can safely be ignored
	uint8_t byte_to_compare = is_negative ? 0xFF : 0x00;
	// we will basically check if any byte has any set bit up to Bignum::MAX_DATA_SIZE, if so, that's an under/overflow
	idx_t data_pos = 0;
	for (idx_t i = size; i > Bignum::MAX_DATA_SIZE; i--) {
		if (data[data_pos++] != byte_to_compare) {
			return true;
		}
	}
	return false;
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

void BignumAddition(data_ptr_t result, int64_t result_end, bool is_target_absolute_bigger,
                    const BignumIntermediate &lhs, const BignumIntermediate &rhs) {
	bool is_result_negative = is_target_absolute_bigger ? lhs.is_negative : rhs.is_negative;

	int64_t i_target = lhs.size - 1;   // last byte index in target
	int64_t i_source = rhs.size - 1;   // last byte index in source
	int64_t i_result = result_end - 1; // last byte index in result

	// Carry for addition
	uint16_t carry = 0;
	uint16_t borrow = 0;
	// Add bytes from right to left
	while (i_result >= 0) {
		// If the numbers are negative, we bit flip them
		uint8_t target_byte = lhs.GetAbsoluteByte(i_target);
		uint8_t source_byte = rhs.GetAbsoluteByte(i_source);
		// Add bytes and carry
		uint16_t sum;
		if (lhs.is_negative == rhs.is_negative) {
			sum = static_cast<uint16_t>(target_byte) + static_cast<uint16_t>(source_byte) + carry;
			carry = (sum >> 8) & 0xFF;
		} else {
			if (is_target_absolute_bigger) {
				sum = static_cast<uint16_t>(target_byte) - static_cast<uint16_t>(source_byte) - borrow;
				borrow = sum > static_cast<uint16_t>(target_byte) ? 1 : 0;
			} else {
				sum = static_cast<uint16_t>(source_byte) - static_cast<uint16_t>(target_byte) - borrow;
				borrow = sum > static_cast<uint16_t>(source_byte) ? 1 : 0;
			}
		}
		uint8_t result_byte = static_cast<uint8_t>(sum & 0xFF);
		// If the result is not positive, we must flip the bits again
		result[i_result] = is_result_negative ? static_cast<uint8_t>(~result_byte) : result_byte;
		i_target--;
		i_source--;
		i_result--;
	}

	if (is_result_negative != lhs.is_negative) {
		// If we are flipping the sign we must be sure that we are flipping all extra bits from our target
		for (int64_t i = 0; i < result_end - rhs.size; ++i) {
			result[i] = is_result_negative ? 0xFF : 0x00;
		}
	}
}

string_t BignumIntermediate::Negate(StringHeap &heap) const {
	auto target = heap.EmptyString(size + Bignum::BIGNUM_HEADER_SIZE);
	auto ptr = target.GetDataWriteable();

	if (!is_negative && size == 1 && data[0] == 0x00) {
		// If we have a zero, we just do a copy
		Bignum::SetHeader(ptr, size, is_negative);
		for (idx_t i = 0; i < size; ++i) {
			ptr[i + Bignum::BIGNUM_HEADER_SIZE] = static_cast<char>(data[i]);
		}
	} else {
		// Otherwise, we set the header with a flip on the signal
		Bignum::SetHeader(ptr, size, !is_negative);
		for (idx_t i = 0; i < size; ++i) {
			// And flip all the data bits
			ptr[i + Bignum::BIGNUM_HEADER_SIZE] = static_cast<char>(~data[i]);
		}
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
	for (size_t i = 0; i < size; i++) {
		data[i] = static_cast<data_t>(~data[i]); // flip each byte of the pointer
	}
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

string_t BignumIntermediate::Add(StringHeap &heap, const BignumIntermediate &lhs, const BignumIntermediate &rhs) {
	const bool same_sign = lhs.is_negative == rhs.is_negative;
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
	if (lhs.is_negative != rhs.is_negative) {
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
	               is_target_absolute_bigger, lhs, rhs);
	bool is_result_negative = is_target_absolute_bigger ? lhs.is_negative : rhs.is_negative;
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
void BignumIntermediate::AddInPlace(ArenaAllocator &allocator, const BignumIntermediate &rhs) {
	const bool same_sign = is_negative == rhs.is_negative;
	idx_t actual_size = size - GetStartDataPos();
	idx_t actual_rhs_size = rhs.size - rhs.GetStartDataPos();
	if (actual_size < actual_rhs_size || (same_sign && (IsMSBSet() || (rhs.IsMSBSet() && size == rhs.size)))) {
		// We must reallocate
		idx_t min_size = actual_size < actual_rhs_size ? actual_rhs_size + 1 : size + 1;
		Reallocate(allocator, min_size);
	}
	bool is_target_absolute_bigger = true;
	if (rhs.is_negative != is_negative) {
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

	bool is_result_negative = is_target_absolute_bigger ? is_negative : rhs.is_negative;
	BignumAddition(data, size, is_target_absolute_bigger, *this, rhs);
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

//! Little-endian base 2^32 digits of an absolute value without leading zero digits; zero is the empty vector
using bignum_magnitude_t = unsafe_vector<digit_t>;

//! Digit operations between two interrupt checks, roughly a millisecond of work
constexpr idx_t INTERRUPT_CHECK_INTERVAL = idx_t(1) << 20;

void AccountWork(BignumArithmeticState &state, idx_t work) {
	if (!state.context) {
		return;
	}
	state.work_since_interrupt_check += work;
	if (state.work_since_interrupt_check >= INTERRUPT_CHECK_INTERVAL) {
		state.work_since_interrupt_check = 0;
		state.context->InterruptCheck();
	}
}

void TrimMagnitude(bignum_magnitude_t &digits) {
	while (!digits.empty() && digits.back() == 0) {
		digits.pop_back();
	}
}

void GetMagnitude(const BignumIntermediate &value, bignum_magnitude_t &digits) {
	digits.clear();
	const idx_t start = value.GetStartDataPos();
	digits.reserve((value.size - start + Bignum::DIGIT_BYTES - 1) / Bignum::DIGIT_BYTES);
	digit_t current = 0;
	uint8_t shift = 0;
	// walk the big-endian magnitude bytes from the least significant byte upwards
	for (idx_t i = value.size; i > start; i--) {
		current |= static_cast<digit_t>(value.GetAbsoluteByte(static_cast<int64_t>(i - 1))) << shift;
		shift += 8;
		if (shift == Bignum::DIGIT_BITS) {
			digits.push_back(current);
			current = 0;
			shift = 0;
		}
	}
	if (shift != 0) {
		digits.push_back(current);
	}
	TrimMagnitude(digits);
}

//! The number of significant bits of a magnitude, 0 for zero
idx_t MagnitudeBitCount(const bignum_magnitude_t &digits) {
	if (digits.empty()) {
		return 0;
	}
	return digits.size() * Bignum::DIGIT_BITS - CountZeros<digit_t>::Leading(digits.back());
}

idx_t BitsToBytes(idx_t bits) {
	return (bits + 7) / 8;
}

//! Writes a (sign, magnitude) pair as a BIGNUM blob into the heap, checking for overflow
string_t WriteMagnitude(StringHeap &heap, const bignum_magnitude_t &digits, bool is_negative) {
	if (digits.empty()) {
		// zero is always encoded as a single positive zero byte
		const auto zero = Bignum::InitializeBignumZero();
		return heap.AddBlob(zero.data(), zero.size());
	}
	const idx_t byte_count = BitsToBytes(MagnitudeBitCount(digits));
	if (byte_count > Bignum::MAX_DATA_SIZE) {
		throw OutOfRangeException(ProduceOverUnderFlowError(is_negative, 0, byte_count));
	}
	auto target = heap.EmptyString(byte_count + Bignum::BIGNUM_HEADER_SIZE);
	auto target_data = target.GetDataWriteable();
	Bignum::SetHeader(target_data, byte_count, is_negative);
	auto result = target_data + Bignum::BIGNUM_HEADER_SIZE;
	for (idx_t i = 0; i < byte_count; i++) {
		// byte i is the i-th most significant byte of the result
		const idx_t byte_from_lsb = byte_count - 1 - i;
		D_ASSERT(byte_from_lsb / Bignum::DIGIT_BYTES < digits.size());
		const auto byte = static_cast<uint8_t>(digits[byte_from_lsb / Bignum::DIGIT_BYTES] >>
		                                       (8 * (byte_from_lsb % Bignum::DIGIT_BYTES)));
		result[i] = static_cast<char>(is_negative ? static_cast<uint8_t>(~byte) : byte);
	}
	target.Finalize();
	return target;
}

//! Schoolbook multiplication: result = lhs * rhs
void MultiplyMagnitudes(BignumArithmeticState &state, const bignum_magnitude_t &lhs, const bignum_magnitude_t &rhs,
                        bignum_magnitude_t &result) {
	result.assign(lhs.size() + rhs.size(), 0);
	for (idx_t i = 0; i < lhs.size(); i++) {
		AccountWork(state, rhs.size());
		twodigit_t carry = 0;
		const twodigit_t lhs_digit = lhs[i];
		for (idx_t j = 0; j < rhs.size(); j++) {
			// (2^32 - 1)^2 + 2 * (2^32 - 1) = 2^64 - 1, so this cannot overflow
			const twodigit_t product = lhs_digit * rhs[j] + result[i + j] + carry;
			result[i + j] = static_cast<digit_t>(product);
			carry = product >> Bignum::DIGIT_BITS;
		}
		result[i + rhs.size()] = static_cast<digit_t>(carry);
	}
	TrimMagnitude(result);
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
	constexpr twodigit_t BASE = static_cast<twodigit_t>(1) << Bignum::DIGIT_BITS;
	constexpr twodigit_t DIGIT_MASK = BASE - 1;
	if (n == 1) {
		// single digit divisor: simple short division
		AccountWork(state, m);
		const twodigit_t d = divisor[0];
		twodigit_t rem = 0;
		quotient.assign(m, 0);
		for (idx_t i = m; i > 0; i--) {
			const twodigit_t current = (rem << Bignum::DIGIT_BITS) | dividend[i - 1];
			quotient[i - 1] = static_cast<digit_t>(current / d);
			rem = current % d;
		}
		TrimMagnitude(quotient);
		remainder.clear();
		if (want_remainder && rem != 0) {
			remainder.push_back(static_cast<digit_t>(rem));
		}
		return;
	}

	// D1: normalize so that the most significant divisor digit has its top bit set
	const auto shift = static_cast<uint8_t>(CountZeros<digit_t>::Leading(divisor[n - 1]));
	auto &vn = state.normalized_divisor;
	auto &un = state.normalized_dividend;
	vn.assign(n, 0);
	un.assign(m + 1, 0);
	if (shift > 0) {
		for (idx_t i = n - 1; i > 0; i--) {
			vn[i] = (divisor[i] << shift) | (divisor[i - 1] >> (Bignum::DIGIT_BITS - shift));
		}
		vn[0] = divisor[0] << shift;
		un[m] = dividend[m - 1] >> (Bignum::DIGIT_BITS - shift);
		for (idx_t i = m - 1; i > 0; i--) {
			un[i] = (dividend[i] << shift) | (dividend[i - 1] >> (Bignum::DIGIT_BITS - shift));
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
		// D3: estimate the quotient digit
		const twodigit_t numerator = (static_cast<twodigit_t>(un[pos + n]) << Bignum::DIGIT_BITS) | un[pos + n - 1];
		twodigit_t qhat = numerator / vn[n - 1];
		twodigit_t rhat = numerator % vn[n - 1];
		while (qhat >= BASE || qhat * vn[n - 2] > ((rhat << Bignum::DIGIT_BITS) | un[pos + n - 2])) {
			qhat--;
			rhat += vn[n - 1];
			if (rhat >= BASE) {
				break;
			}
		}
		// D4: multiply and subtract
		// the borrow is at most BASE: the high half of a product is at most BASE - 2 and we borrow at most 2 units
		twodigit_t borrow = 0;
		for (idx_t i = 0; i < n; i++) {
			const twodigit_t product = qhat * vn[i];
			const twodigit_t subtrahend = (product & DIGIT_MASK) + borrow;
			const twodigit_t current = un[i + pos];
			if (current >= subtrahend) {
				un[i + pos] = static_cast<digit_t>(current - subtrahend);
				borrow = product >> Bignum::DIGIT_BITS;
			} else {
				// the subtrahend is below 2 * BASE, so we need to borrow one or two units from the next digit
				const twodigit_t deficit = subtrahend - current;
				const twodigit_t units = (deficit + BASE - 1) / BASE;
				un[i + pos] = static_cast<digit_t>(current + units * BASE - subtrahend);
				borrow = (product >> Bignum::DIGIT_BITS) + units;
			}
		}
		const twodigit_t top = un[pos + n];
		const bool went_negative = top < borrow;
		un[pos + n] = static_cast<digit_t>(went_negative ? top + BASE - borrow : top - borrow);
		quotient[pos] = static_cast<digit_t>(qhat);
		if (went_negative) {
			// D6: the estimate was one too large, add the divisor back
			quotient[pos]--;
			twodigit_t carry = 0;
			for (idx_t i = 0; i < n; i++) {
				const twodigit_t sum = static_cast<twodigit_t>(un[i + pos]) + vn[i] + carry;
				un[i + pos] = static_cast<digit_t>(sum);
				carry = sum >> Bignum::DIGIT_BITS;
			}
			un[pos + n] = static_cast<digit_t>(un[pos + n] + carry);
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
			remainder[i] = (un[i] >> shift) | (un[i + 1] << (Bignum::DIGIT_BITS - shift));
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
