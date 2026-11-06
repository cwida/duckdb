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
#include "duckdb/common/bignum_limbs.hpp"
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
		auto data = const_data_ptr_cast(blob.data.GetData());
		const bool is_negative = (data[0] & 0x80) == 0;
		auto magnitude = data + BIGNUM_HEADER_SIZE;
		idx_t data_byte_size = blob_size - BIGNUM_HEADER_SIZE;
		if (data_byte_size > 2 * BignumLimb::BYTES) {
			// a well-formed BIGNUM carries no leading zero bytes, so this never fits a 128-bit integer; we tolerate
			// leading zero bytes anyway (0xFF under the complement of a negative value)
			const data_t zero = is_negative ? 0xFF : 0;
			for (; data_byte_size > 2 * BignumLimb::BYTES; data_byte_size--, magnitude++) {
				if (*magnitude != zero) {
					return false;
				}
			}
		}
		// load the magnitude as two 64-bit limbs
		uint64_t upper = 0;
		uint64_t lower;
		if (data_byte_size > BignumLimb::BYTES) {
			upper = BignumLimb::LoadMagnitudePartial(magnitude, data_byte_size - BignumLimb::BYTES, is_negative);
			lower = BignumLimb::LoadMagnitude(magnitude + data_byte_size - BignumLimb::BYTES, is_negative);
		} else {
			lower = BignumLimb::LoadMagnitudePartial(magnitude, data_byte_size, is_negative);
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
