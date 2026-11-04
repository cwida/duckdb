//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/bignum.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/winapi.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include <stdint.h>
#include "duckdb/common/types/string_type.hpp"

namespace duckdb {
class StringHeap;
class ArenaAllocator;
class ClientContext;

struct bignum_t {
	string_t data;

	bignum_t() : data() {
	}

	explicit bignum_t(const string_t &data) : data(data) {
	}

	bignum_t(const bignum_t &rhs) = default;
	bignum_t(bignum_t &&other) = default;
	bignum_t &operator=(const bignum_t &rhs) = default;
	bignum_t &operator=(bignum_t &&rhs) = default;

	void Print() const;
};

enum AbsoluteNumberComparison : uint8_t {
	// If number is equal
	EQUAL = 0,
	// If compared number is greater
	GREATER = 1,
	// If compared number is smaller
	SMALLER = 2,
};

//! A bignum magnitude is handled as base 2^32 digits
using digit_t = uint32_t;
using twodigit_t = uint64_t;

//! Per-vector scratch digit buffers and interrupt-check state for BIGNUM multiplication and division
struct BignumArithmeticState {
	explicit BignumArithmeticState(optional_ptr<ClientContext> context = nullptr) : context(context) {
	}

	optional_ptr<ClientContext> context;
	//! Digit operations performed since the last interrupt check
	idx_t work_since_interrupt_check = 0;
	unsafe_vector<digit_t> lhs_digits;
	unsafe_vector<digit_t> rhs_digits;
	unsafe_vector<digit_t> result_digits;
	unsafe_vector<digit_t> remainder_digits;
	unsafe_vector<digit_t> normalized_dividend;
	unsafe_vector<digit_t> normalized_divisor;
};

struct BignumIntermediate {
	BignumIntermediate() : is_negative(false), size(0), data(nullptr) {};
	explicit BignumIntermediate(const bignum_t &value);
	BignumIntermediate(uint8_t *value, idx_t size);
	void Print() const;
	//! Information on the header
	bool is_negative;
	uint32_t size;
	//! The actual data
	data_ptr_t data;
	//! If the absolute number is  bigger than the absolute rhs
	//! 1 = true, 0 = equal, -1 = false
	AbsoluteNumberComparison IsAbsoluteBigger(const BignumIntermediate &rhs) const;
	//! Get the absolute value of a byte
	uint8_t GetAbsoluteByte(int64_t index) const;
	//! If the most significant bit of the first byte is set.
	bool IsMSBSet() const;
	//! Initializes our bignum to 0 and 1 byte
	void Initialize(ArenaAllocator &allocator);
	//! If necessary, we reallocate our intermediate to the next power of 2.
	void Reallocate(ArenaAllocator &allocator, idx_t min_size);
	static uint32_t GetStartDataPos(data_ptr_t data, idx_t size, bool is_negative);
	uint32_t GetStartDataPos() const;
	//! In case we have unnecessary extra 0's or 1's in our bignum we trim them
	static idx_t Trim(data_ptr_t data, uint32_t &size, bool is_negative);
	void Trim();
	//! Add a BignumIntermediate to another BignumIntermediate, equivalent of a +=
	void AddInPlace(ArenaAllocator &allocator, const BignumIntermediate &rhs);
	//! Adds two BignumIntermediates and returns a string_t result, equivalent of a +
	static string_t Add(StringHeap &heap, const BignumIntermediate &lhs, const BignumIntermediate &rhs);
	//! Multiplies two BignumIntermediates and returns a string_t result, equivalent of a *
	static string_t Multiply(StringHeap &heap, BignumArithmeticState &state, const BignumIntermediate &lhs,
	                         const BignumIntermediate &rhs);
	//! Truncating division (a // b) or, with want_remainder, the remainder with the sign of the dividend (a % b)
	//! Returns false without writing a result when the divisor is zero
	static bool TryDivMod(StringHeap &heap, BignumArithmeticState &state, const BignumIntermediate &lhs,
	                      const BignumIntermediate &rhs, bool want_remainder, string_t &result);
	//! Negates a value, e.g., -x
	string_t Negate(StringHeap &heap) const;
	void NegateInPlace();
	//! Exports to an arena-allocated bignum, trimmed on the way out; the intermediate itself is left as it is
	bignum_t ToBignum(ArenaAllocator &allocator) const;
	//! Check if an over/underflow has occurred
	static bool OverOrUnderflow(data_ptr_t data, idx_t size, bool is_negative);
	bool OverOrUnderflow() const;
};

} // namespace duckdb
