//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/types/bignum.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/winapi.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/common/bignum.hpp"
#include "duckdb/common/bswap.hpp"
#include "duckdb/common/vector.hpp"

namespace duckdb {

struct BoundCastInfo;
struct CastParameters;

//! Scratch space of the VARCHAR <-> BIGNUM conversions, reused across the values of a vector
struct BignumCastScratch {
	//! Base 2^64 limbs of a magnitude, least significant first
	unsafe_vector<uint64_t> binary;
	//! Base 10^19 limbs of a magnitude, least significant first
	unsafe_vector<uint64_t> decimal;
	//! powers[k] holds 2^(64 * 2^k) in base 10^19 limbs, for the divide and conquer conversion of large values
	vector<unsafe_vector<uint64_t>> powers;
};

//! The Bignum class is a static class that holds helper functions for the Bignum type.
class Bignum {
public:
	//! This is the maximum number of bytes a BIGNUM can have on it's data size
	//! i.e., 2^(8*3-1) - 1.
	DUCKDB_API static constexpr uint32_t MAX_DATA_SIZE = 8388607;
	//! Header size of a Bignum is always 3 bytes.
	DUCKDB_API static constexpr uint8_t BIGNUM_HEADER_SIZE = 3;
	//! Max(e such that 10**e fits in a 64-bit limb): the decimal conversions use limbs of DECIMAL_SHIFT digits
	DUCKDB_API static constexpr uint8_t DECIMAL_SHIFT = 19;
	//! 10 ** DECIMAL_SHIFT
	DUCKDB_API static constexpr uint64_t DECIMAL_BASE = 10000000000000000000ULL;
	//! Bytes of a digit_t
	DUCKDB_API static constexpr uint8_t DIGIT_BYTES = sizeof(digit_t);
	//! Bits of a digit_t
	DUCKDB_API static constexpr uint8_t DIGIT_BITS = DIGIT_BYTES * 8;
	//! Verifies if a Bignum is valid. i.e., if it has 3 header bytes. The header correctly represents the number of
	//! data bytes, and the data bytes has no leading zero bytes.
	DUCKDB_API static void Verify(const bignum_t &input);

	//! Sets the header of a bignum (i.e., char* blob), depending on the number of bytes that bignum needs and if it's a
	//! negative number
	DUCKDB_API static void SetHeader(char *blob, uint64_t number_of_bytes, bool is_negative);
	//! Initializes and returns a blob with value 0, allocated in Vector& result
	DUCKDB_API static bignum_t InitializeBignumZero(Vector &result);
	DUCKDB_API static string InitializeBignumZero();

	//! Switch Case of To Bignum Conversion
	DUCKDB_API static BoundCastInfo NumericToBignumCastSwitch(const LogicalType &source);

	//! ----------------------------------- Varchar Cast ----------------------------------- //
	//! Function to prepare a varchar for conversion. We trim zero's, check for negative values, and what-not
	//! Returns false if this is an invalid varchar
	DUCKDB_API static bool VarcharFormatting(const string_t &value, idx_t &start_pos, idx_t &end_pos, bool &is_negative,
	                                         bool &is_zero, bool &should_round_up);
	//! Encodes a VARCHAR into a BIGNUM blob using formatting information returned by VarcharFormatting.
	DUCKDB_API static string EncodeBignum(const string_t &value, idx_t start_pos, idx_t end_pos, bool is_negative,
	                                      bool is_zero);
	//! Encodes a VARCHAR into a BIGNUM blob, applying decimal rounding if requested.
	DUCKDB_API static string EncodeVarcharBignum(const string_t &value, idx_t start_pos, idx_t end_pos,
	                                             bool is_negative, bool is_zero, bool should_round_up);
	//! Converts a VARCHAR into a BIGNUM allocated in the result vector, returns false if the VARCHAR is invalid
	DUCKDB_API static bool TryVarcharToBignum(const string_t &value, bignum_t &result_value, Vector &result,
	                                          BignumCastScratch &scratch);
	//! The VARCHAR -> BIGNUM cast function
	DUCKDB_API static bool VarcharToBignumCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters);
	//! Converts a char to a Digit
	DUCKDB_API static int CharToDigit(char c);
	//! Converts a Digit to a char
	DUCKDB_API static char DigitToChar(int digit);
	//! Function to convert a string_t into a vector of bytes
	DUCKDB_API static void GetByteArray(vector<uint8_t> &byte_array, bool &is_negative, const string_t &blob);
	//! Function to create a BIGNUM blob from a byte array containing the absolute value, plus an is_negative bool
	DUCKDB_API static string FromByteArray(uint8_t *data, idx_t size, bool is_negative);
	//! Function to convert BIGNUM blob to a VARCHAR
	DUCKDB_API static string BignumToVarchar(const bignum_t &blob);
	//! Writes the decimal representation of a BIGNUM into the string heap of the result vector
	DUCKDB_API static string_t BignumToVarchar(const bignum_t &blob, Vector &result, BignumCastScratch &scratch);
	//! The BIGNUM -> VARCHAR cast function
	DUCKDB_API static bool BignumToVarcharCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters);
	//! Function to convert Varchar to BIGNUM blob
	DUCKDB_API static string VarcharToBignum(const string_t &value);
	//! ----------------------------------- Double Cast ----------------------------------- //
	DUCKDB_API static bool BignumToDouble(const bignum_t &blob, double &result, bool &strict);
	[[noreturn]] DUCKDB_API static void ThrowInvalidBlobSize(idx_t size);
	//! Converts a BIGNUM into an integer type, returns false if the value is out of range
	template <class T>
	static bool BignumToInt(const bignum_t &blob, T &result, bool &strict) {
		const idx_t blob_size = blob.data.GetSize();
		if (blob_size < BIGNUM_HEADER_SIZE + 1) {
			ThrowInvalidBlobSize(blob_size);
		}
		const char *data = blob.data.GetData();
		const char *end = data + blob_size;
		const bool is_negative = (data[0] & 0x80) == 0;
		idx_t data_byte_size = blob_size - BIGNUM_HEADER_SIZE;
		if (data_byte_size > 2 * sizeof(uint64_t)) {
			// a well-formed BIGNUM carries no leading zero bytes, so this never fits a 128-bit integer; we tolerate
			// leading zero bytes anyway (0xFF under the complement of a negative value)
			const char zero = is_negative ? static_cast<char>(0xFF) : 0;
			for (const char *first = data + BIGNUM_HEADER_SIZE; data_byte_size > 2 * sizeof(uint64_t);
			     data_byte_size--, first++) {
				if (*first != zero) {
					return false;
				}
			}
		}
		// load the magnitude as two 64-bit limbs
		uint64_t upper = 0;
		uint64_t lower;
		if (data_byte_size > sizeof(uint64_t)) {
			upper = BignumCastLoadLeadingBytes(end - sizeof(uint64_t), data_byte_size - sizeof(uint64_t), is_negative);
			lower = BignumCastLoadLimb(end, is_negative);
		} else {
			lower = BignumCastLoadLeadingBytes(end, data_byte_size, is_negative);
		}
		return BignumCastFromMagnitude<T>(upper, lower, is_negative, result);
	}

	//! Converts a 128-bit magnitude and a sign into an integer type, returns false if the value is out of range
	template <class T>
	static bool BignumCastFromMagnitude(uint64_t upper, uint64_t lower, bool is_negative, T &result) {
		if (upper == 0 && lower == 0) {
			result = 0;
			return true;
		}
		if (upper != 0) {
			return false;
		}
		if (is_negative) {
			if (!NumericLimits<T>::IsSigned()) {
				return false;
			}
			// the magnitude may be one larger than the maximum
			const uint64_t limit = static_cast<uint64_t>(NumericLimits<T>::Maximum()) + 1;
			if (lower > limit) {
				return false;
			}
			if (lower == limit) {
				result = NumericLimits<T>::Minimum();
				return true;
			}
			result = static_cast<T>(-static_cast<int64_t>(lower));
			return true;
		}
		if (lower > static_cast<uint64_t>(NumericLimits<T>::Maximum())) {
			return false;
		}
		result = static_cast<T>(lower);
		return true;
	}

	//! Byte order conversion for the big-endian magnitude bytes of a BIGNUM
	static inline uint64_t BignumCastBigEndianToHost(uint64_t value) {
#if DUCKDB_IS_BIG_ENDIAN
		return value;
#else
		return BSwap(value);
#endif
	}
	static inline uint32_t BignumCastBigEndianToHost(uint32_t value) {
#if DUCKDB_IS_BIG_ENDIAN
		return value;
#else
		return BSwap(value);
#endif
	}

	//! Loads the eight big-endian magnitude bytes that end at "end", undoing the complement of a negative value
	static inline uint64_t BignumCastLoadLimb(const char *end, bool is_negative) {
		uint64_t value;
		memcpy(&value, end - sizeof(uint64_t), sizeof(uint64_t));
		value = BignumCastBigEndianToHost(value);
		return is_negative ? ~value : value;
	}

	//! Loads the n (1 <= n <= 8) most significant magnitude bytes, which end at "end", of a BIGNUM blob. The three
	//! header bytes precede them, so a four byte load (n <= 4) or an eight byte load (n >= 5) ending at "end" stays
	//! within the blob; the bytes that do not belong to the magnitude are masked off.
	static inline uint64_t BignumCastLoadLeadingBytes(const char *end, idx_t n, bool is_negative) {
		D_ASSERT(n >= 1 && n <= sizeof(uint64_t));
		if (n <= sizeof(uint32_t)) {
			uint32_t value;
			memcpy(&value, end - sizeof(uint32_t), sizeof(uint32_t));
			value = BignumCastBigEndianToHost(value);
			if (is_negative) {
				value = ~value;
			}
			return value & (NumericLimits<uint32_t>::Maximum() >> (32 - 8 * n));
		}
		uint64_t value;
		memcpy(&value, end - sizeof(uint64_t), sizeof(uint64_t));
		value = BignumCastBigEndianToHost(value);
		if (is_negative) {
			value = ~value;
		}
		return value & (NumericLimits<uint64_t>::Maximum() >> (64 - 8 * n));
	}

	//! Writes the n (1 <= n <= 8) least significant bytes of value as a big-endian magnitude, complemented for a
	//! negative value
	static inline void BignumCastStoreLeadingBytes(char *destination, uint64_t value, idx_t n, bool is_negative) {
		D_ASSERT(n >= 1 && n <= sizeof(uint64_t));
		if (is_negative) {
			value = ~value;
		}
		value = BignumCastBigEndianToHost(value);
		memcpy(destination, reinterpret_cast<const char *>(&value) + (sizeof(uint64_t) - n), n);
	}

	//! Writes a 64-bit limb as eight big-endian magnitude bytes ending at "end", complemented for a negative value
	static inline void BignumCastStoreLimb(char *end, uint64_t value, bool is_negative) {
		if (is_negative) {
			value = ~value;
		}
		value = BignumCastBigEndianToHost(value);
		memcpy(end - sizeof(uint64_t), &value, sizeof(uint64_t));
	}
};

template <>
inline bool Bignum::BignumCastFromMagnitude(uint64_t upper, uint64_t lower, bool is_negative, hugeint_t &result) {
	constexpr uint64_t SIGN_BIT = static_cast<uint64_t>(1) << 63;
	if (upper == 0 && lower == 0) {
		result = hugeint_t(0);
		return true;
	}
	if (is_negative) {
		if (upper > SIGN_BIT || (upper == SIGN_BIT && lower != 0)) {
			return false;
		}
		if (upper == SIGN_BIT) {
			result = NumericLimits<hugeint_t>::Minimum();
			return true;
		}
		// two's complement of the magnitude
		result.lower = ~lower + 1;
		result.upper = static_cast<int64_t>(~upper + (lower == 0 ? 1 : 0));
		return true;
	}
	if (upper >= SIGN_BIT) {
		return false;
	}
	result = hugeint_t(static_cast<int64_t>(upper), lower);
	return true;
}

template <>
inline bool Bignum::BignumCastFromMagnitude(uint64_t upper, uint64_t lower, bool is_negative, uhugeint_t &result) {
	if (is_negative && (upper != 0 || lower != 0)) {
		return false;
	}
	result = uhugeint_t(upper, lower);
	return true;
}

//! ----------------------------------- (u)Integral Cast ----------------------------------- //
struct IntCastToBignum {
	template <class SRC>
	static inline bignum_t Operation(SRC input, StringHeap &heap) {
		return IntToBignum(heap, input);
	}
};

//! ----------------------------------- (u)HugeInt Cast ----------------------------------- //
struct HugeintCastToBignum {
	template <class SRC>
	static inline bignum_t Operation(SRC input, StringHeap &heap) {
		InternalException::Throw("Unsupported type for cast to BIGNUM");
	}
};

struct TryCastToBignum {
	template <class SRC, class DST>
	static inline bool Operation(SRC input, DST &result, Vector &result_vector, CastParameters &parameters) {
		InternalException::Throw("Unsupported type for try cast to BIGNUM");
	}
};

template <>
DUCKDB_API bool TryCastToBignum::Operation(double double_value, bignum_t &result_value, Vector &result,
                                           CastParameters &parameters);

template <>
DUCKDB_API bool TryCastToBignum::Operation(float float_value, bignum_t &result_value, Vector &result,
                                           CastParameters &parameters);

template <>
DUCKDB_API bool TryCastToBignum::Operation(string_t input_value, bignum_t &result_value, Vector &result,
                                           CastParameters &parameters);

struct BignumCastToVarchar {
	template <class SRC>
	DUCKDB_API static inline string_t Operation(SRC input, StringHeap &heap) {
		return heap.AddBlob(Bignum::BignumToVarchar(input));
	}
};

struct BignumToDoubleCast {
	template <class SRC, class DST>
	DUCKDB_API static inline bool Operation(SRC input, DST &result, bool strict = false) {
		return Bignum::BignumToDouble(input, result, strict);
	}
};

struct BignumToIntCast {
	template <class SRC, class DST>
	DUCKDB_API static inline bool Operation(SRC input, DST &result, bool strict = false) {
		return Bignum::BignumToInt(input, result, strict);
	}
};

} // namespace duckdb
