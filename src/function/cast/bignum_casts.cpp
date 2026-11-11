#include "duckdb/function/cast/default_casts.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/function/cast/vector_cast_helpers.hpp"
#include "duckdb/common/types/bignum.hpp"
#include "duckdb/common/bignum.hpp"
#include "duckdb/common/bit_utils.hpp"

#include <cmath>
#include <cstring>

namespace duckdb {

template <class T>
static bignum_t IntToBignum(StringHeap &heap, T int_value) {
	// Determine if the number is negative
	bool is_negative = int_value < 0;
	// Determine the number of data bytes
	uint64_t abs_value;
	if (is_negative) {
		if (int_value == std::numeric_limits<T>::min()) {
			abs_value = static_cast<uint64_t>(std::numeric_limits<T>::max()) + 1;
		} else {
			abs_value = static_cast<uint64_t>(std::abs(static_cast<int64_t>(int_value)));
		}
	} else {
		abs_value = static_cast<uint64_t>(int_value);
	}
	uint32_t data_byte_size;
	if (abs_value == 0) {
		data_byte_size = 1;
	} else {
		const uint32_t bits_needed = 64 - static_cast<uint32_t>(CountZeros<uint64_t>::Leading(abs_value));
		data_byte_size = (bits_needed + 7) / 8;
	}

	uint32_t blob_size = data_byte_size + Bignum::BIGNUM_HEADER_SIZE;
	auto blob = heap.EmptyString(blob_size);
	auto writable_blob = blob.GetDataWriteable();
	Bignum::SetHeader(writable_blob, data_byte_size, is_negative);

	// Add data bytes to the blob, starting off after header bytes
	idx_t wb_idx = Bignum::BIGNUM_HEADER_SIZE;
	for (int i = static_cast<int>(data_byte_size) - 1; i >= 0; --i) {
		if (is_negative) {
			writable_blob[wb_idx++] = static_cast<char>(~(abs_value >> i * 8 & 0xFF));
		} else {
			writable_blob[wb_idx++] = static_cast<char>(abs_value >> i * 8 & 0xFF);
		}
	}
	blob.Finalize();
	bignum_t result_bignum(blob);
	return result_bignum;
}

template <>
bignum_t HugeintCastToBignum::Operation(uhugeint_t int_value, StringHeap &heap) {
	uint32_t data_byte_size;
	if (int_value.upper == 0) {
		data_byte_size = 0;
	} else {
		const uint32_t bits_needed = 64 - static_cast<uint32_t>(CountZeros<uint64_t>::Leading(int_value.upper));
		data_byte_size = (bits_needed + 7) / 8;
	}

	uint32_t upper_byte_size = data_byte_size;
	if (data_byte_size > 0) {
		// If we have at least one byte on the upper side, the bottom side is complete
		data_byte_size += 8;
	} else {
		if (int_value.lower != 0) {
			const uint32_t bits_needed = 64 - static_cast<uint32_t>(CountZeros<uint64_t>::Leading(int_value.lower));
			data_byte_size += (bits_needed + 7) / 8;
		}
	}
	if (data_byte_size == 0) {
		data_byte_size++;
	}
	uint32_t blob_size = data_byte_size + Bignum::BIGNUM_HEADER_SIZE;
	auto blob = heap.EmptyString(blob_size);
	auto writable_blob = blob.GetDataWriteable();
	Bignum::SetHeader(writable_blob, data_byte_size, false);

	// Add data bytes to the blob, starting off after header bytes
	idx_t wb_idx = Bignum::BIGNUM_HEADER_SIZE;
	for (int i = static_cast<int>(upper_byte_size) - 1; i >= 0; --i) {
		writable_blob[wb_idx++] = static_cast<char>(int_value.upper >> i * 8 & 0xFF);
	}
	for (int i = static_cast<int>(data_byte_size - upper_byte_size) - 1; i >= 0; --i) {
		writable_blob[wb_idx++] = static_cast<char>(int_value.lower >> i * 8 & 0xFF);
	}
	blob.Finalize();
	bignum_t result_bignum(blob);
	return result_bignum;
}

template <>
bignum_t HugeintCastToBignum::Operation(hugeint_t int_value, StringHeap &heap) {
	// Determine if the number is negative
	bool is_negative = int_value.upper >> 63 & 1;
	if (is_negative) {
		// We must check if it's -170141183460469231731687303715884105728, since it's not possible to negate it
		// without overflowing
		if (int_value == NumericLimits<hugeint_t>::Minimum()) {
			uhugeint_t u_int_value {0x8000000000000000, 0};
			auto cast_value = Operation<uhugeint_t>(u_int_value, heap);
			// We have to do all the bit flipping.
			auto writable_value_ptr = cast_value.data.GetDataWriteable();
			Bignum::SetHeader(writable_value_ptr, cast_value.data.GetSize() - Bignum::BIGNUM_HEADER_SIZE, is_negative);
			for (idx_t i = Bignum::BIGNUM_HEADER_SIZE; i < cast_value.data.GetSize(); i++) {
				writable_value_ptr[i] = static_cast<char>(~writable_value_ptr[i]);
			}
			cast_value.data.Finalize();
			return cast_value;
		}
		int_value = -int_value;
	}
	// Determine the number of data bytes
	uint64_t abs_value_upper = static_cast<uint64_t>(int_value.upper);

	uint32_t data_byte_size;
	if (abs_value_upper == 0) {
		data_byte_size = 0;
	} else {
		const uint32_t bits_needed = 64 - static_cast<uint32_t>(CountZeros<uint64_t>::Leading(abs_value_upper));
		data_byte_size = (bits_needed + 7) / 8;
	}

	uint32_t upper_byte_size = data_byte_size;
	if (data_byte_size > 0) {
		// If we have at least one byte on the upper side, the bottom side is complete
		data_byte_size += 8;
	} else {
		if (int_value.lower != 0) {
			const uint32_t bits_needed = 64 - static_cast<uint32_t>(CountZeros<uint64_t>::Leading(int_value.lower));
			data_byte_size += (bits_needed + 7) / 8;
		}
	}

	if (data_byte_size == 0) {
		data_byte_size++;
	}
	uint32_t blob_size = data_byte_size + Bignum::BIGNUM_HEADER_SIZE;
	auto blob = heap.EmptyString(blob_size);
	auto writable_blob = blob.GetDataWriteable();
	Bignum::SetHeader(writable_blob, data_byte_size, is_negative);

	// Add data bytes to the blob, starting off after header bytes
	idx_t wb_idx = Bignum::BIGNUM_HEADER_SIZE;
	for (int i = static_cast<int>(upper_byte_size) - 1; i >= 0; --i) {
		if (is_negative) {
			writable_blob[wb_idx++] = static_cast<char>(~(abs_value_upper >> i * 8 & 0xFF));
		} else {
			writable_blob[wb_idx++] = static_cast<char>(abs_value_upper >> i * 8 & 0xFF);
		}
	}
	for (int i = static_cast<int>(data_byte_size - upper_byte_size) - 1; i >= 0; --i) {
		if (is_negative) {
			writable_blob[wb_idx++] = static_cast<char>(~(int_value.lower >> i * 8 & 0xFF));
		} else {
			writable_blob[wb_idx++] = static_cast<char>(int_value.lower >> i * 8 & 0xFF);
		}
	}
	blob.Finalize();
	bignum_t result_bignum(blob);
	return result_bignum;
}

// Varchar to Bignum
template <>
bool TryCastToBignum::Operation(string_t input_value, bignum_t &result_value, Vector &result,
                                CastParameters &parameters) {
	BignumCastScratch scratch;
	return Bignum::TryVarcharToBignum(input_value, result_value, result, scratch);
}

struct VarcharToBignumCastData {
	VarcharToBignumCastData(Vector &result, CastParameters &parameters) : vector_cast_data(result, parameters) {
	}

	VectorTryCastData vector_cast_data;
	BignumCastScratch scratch;
};

struct VarcharToBignumOperator {
	template <class INPUT_TYPE, class RESULT_TYPE>
	static RESULT_TYPE Operation(INPUT_TYPE input, ValidityMask &mask, idx_t idx, VarcharToBignumCastData &data) {
		RESULT_TYPE output;
		if (DUCKDB_LIKELY(Bignum::TryVarcharToBignum(input, output, data.vector_cast_data.result, data.scratch))) {
			return output;
		}
		return HandleVectorCastError::Operation<RESULT_TYPE>(
		    CastExceptionText<INPUT_TYPE>(input, data.vector_cast_data.result.GetType()), mask, idx,
		    data.vector_cast_data);
	}
};

bool Bignum::BignumToVarcharCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	D_ASSERT(result.GetType().InternalType() == PhysicalType::VARCHAR);
	BignumCastScratch scratch;
	UnaryExecutor::Execute<bignum_t, string_t>(
	    source, result, count, [&](bignum_t input) { return Bignum::BignumToVarchar(input, result, scratch); });
	return true;
}

bool Bignum::VarcharToBignumCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	VarcharToBignumCastData data(result, parameters);
	UnaryExecutor::GenericExecute<string_t, bignum_t, VarcharToBignumOperator>(source, result, count, data);
	return data.vector_cast_data.all_converted;
}

template <class T>
static bool DoubleToBignum(T double_value, bignum_t &result_value, Vector &result) {
	// Check if we can cast it
	if (!std::isfinite(double_value)) {
		// We can't cast inf -inf nan
		return false;
	}
	// Round half to even like the casts to BIGINT and HUGEINT; the byte loop below is only exact for whole numbers.
	double_value = std::nearbyint(double_value);
	// Determine if the number is negative
	bool is_negative = double_value < 0;
	// Determine the number of data bytes
	double abs_value = std::abs(static_cast<double>(double_value));

	if (abs_value == 0) {
		// Return Value 0
		result_value = Bignum::InitializeBignumZero(result);
		return true;
	}
	// abs_value = fraction * 2^exponent with 0.5 <= fraction < 1, i.e. mantissa * 2^(exponent - 53) with an integer
	// mantissa of exactly 53 bits
	int exponent;
	const double fraction = std::frexp(abs_value, &exponent);
	uint64_t top = 0;
	idx_t top_bytes = 1;
	idx_t zero_bytes = 0;
	if (exponent > 0) {
		constexpr int MANTISSA_BITS = 53;
		const auto mantissa = static_cast<uint64_t>(std::ldexp(fraction, MANTISSA_BITS));
		if (exponent <= MANTISSA_BITS) {
			// the fraction is dropped, this leaves "exponent" bits
			top = mantissa >> (MANTISSA_BITS - exponent);
			top_bytes = (static_cast<idx_t>(exponent) + 7) / 8;
		} else {
			// the mantissa is followed by (exponent - 53) zero bits: whole zero bytes, and a shift of the mantissa
			const int shift = exponent - MANTISSA_BITS;
			zero_bytes = static_cast<idx_t>(shift) / 8;
			top = mantissa << (shift % 8);
			top_bytes = (static_cast<idx_t>(exponent) + 7) / 8 - zero_bytes;
		}
	}
	// a magnitude below one has an integer part of zero, which keeps the sign of the input as before
	const idx_t data_byte_size = top_bytes + zero_bytes;
	const idx_t blob_size = data_byte_size + Bignum::BIGNUM_HEADER_SIZE;
	result_value.data = StringVector::EmptyString(result, blob_size);
	auto writable_blob = result_value.data.GetDataWriteable();
	Bignum::SetHeader(writable_blob, data_byte_size, is_negative);
	BignumLimb::StoreMagnitudePartial(data_ptr_cast(writable_blob + Bignum::BIGNUM_HEADER_SIZE), top_bytes, top,
	                                  is_negative);
	memset(writable_blob + Bignum::BIGNUM_HEADER_SIZE + top_bytes, is_negative ? 0xFF : 0x00, zero_bytes);
	result_value.data.Finalize();
	return true;
}

template <>
bool TryCastToBignum::Operation(double double_value, bignum_t &result_value, Vector &result,
                                CastParameters &parameters) {
	return DoubleToBignum(double_value, result_value, result);
}

template <>
bool TryCastToBignum::Operation(float double_value, bignum_t &result_value, Vector &result,
                                CastParameters &parameters) {
	return DoubleToBignum(double_value, result_value, result);
}

BoundCastInfo Bignum::NumericToBignumCastSwitch(const LogicalType &source) {
	// now switch on the result type
	switch (source.id()) {
	case LogicalTypeId::TINYINT:
		return BoundCastInfo(&VectorCastHelpers::StringCast<int8_t, IntCastToBignum, bignum_t>);
	case LogicalTypeId::UTINYINT:
		return BoundCastInfo(&VectorCastHelpers::StringCast<uint8_t, IntCastToBignum, bignum_t>);
	case LogicalTypeId::SMALLINT:
		return BoundCastInfo(&VectorCastHelpers::StringCast<int16_t, IntCastToBignum, bignum_t>);
	case LogicalTypeId::USMALLINT:
		return BoundCastInfo(&VectorCastHelpers::StringCast<uint16_t, IntCastToBignum, bignum_t>);
	case LogicalTypeId::INTEGER:
		return BoundCastInfo(&VectorCastHelpers::StringCast<int32_t, IntCastToBignum, bignum_t>);
	case LogicalTypeId::UINTEGER:
		return BoundCastInfo(&VectorCastHelpers::StringCast<uint32_t, IntCastToBignum, bignum_t>);
	case LogicalTypeId::BIGINT:
		return BoundCastInfo(&VectorCastHelpers::StringCast<int64_t, IntCastToBignum, bignum_t>);
	case LogicalTypeId::UBIGINT:
		return BoundCastInfo(&VectorCastHelpers::StringCast<uint64_t, IntCastToBignum, bignum_t>);
	case LogicalTypeId::UHUGEINT:
		return BoundCastInfo(&VectorCastHelpers::StringCast<uhugeint_t, HugeintCastToBignum, bignum_t>);
	case LogicalTypeId::FLOAT:
		return BoundCastInfo(&VectorCastHelpers::TryCastStringLoop<float, bignum_t, TryCastToBignum>);
	case LogicalTypeId::DOUBLE:
		return BoundCastInfo(&VectorCastHelpers::TryCastStringLoop<double, bignum_t, TryCastToBignum>);
	case LogicalTypeId::HUGEINT:
		return BoundCastInfo(&VectorCastHelpers::StringCast<hugeint_t, HugeintCastToBignum, bignum_t>);
	case LogicalTypeId::DECIMAL:
	default:
		return DefaultCasts::TryVectorNullCast;
	}
}

struct BignumToDoubleErrorCast {
	template <class SRC, class DST>
	static inline bool Operation(SRC input, DST &result, CastParameters &parameters) {
		if (Bignum::BignumToDouble(input, result, parameters.strict)) {
			return true;
		}
		HandleCastError::AssignError(
		    StringUtil::Format("Could not convert bignum '%s' to Double", Bignum::BignumToVarchar(input)), parameters);
		return false;
	}
};

struct BignumToIntErrorCast {
	template <class SRC, class DST>
	static inline bool Operation(SRC input, DST &result, CastParameters &parameters) {
		if (Bignum::BignumToInt(input, result, parameters.strict)) {
			return true;
		}
		HandleCastError::AssignError(StringUtil::Format("BIGNUM value %s is out of range for type %s",
		                                                Bignum::BignumToVarchar(input),
		                                                TypeIdToString(GetTypeId<DST>())),
		                             parameters);
		return false;
	}
};

BoundCastInfo DefaultCasts::BignumCastSwitch(BindCastInput &input, const LogicalType &source,
                                             const LogicalType &target) {
	D_ASSERT(source.id() == LogicalTypeId::BIGNUM);
	// now switch on the result type
	switch (target.id()) {
	case LogicalTypeId::TINYINT:
		return BoundCastInfo(&VectorCastHelpers::TryCastErrorLoop<bignum_t, int8_t, BignumToIntErrorCast>);
	case LogicalTypeId::UTINYINT:
		return BoundCastInfo(&VectorCastHelpers::TryCastErrorLoop<bignum_t, uint8_t, BignumToIntErrorCast>);

	case LogicalTypeId::SMALLINT:
		return BoundCastInfo(&VectorCastHelpers::TryCastErrorLoop<bignum_t, int16_t, BignumToIntErrorCast>);

	case LogicalTypeId::USMALLINT:
		return BoundCastInfo(&VectorCastHelpers::TryCastErrorLoop<bignum_t, uint16_t, BignumToIntErrorCast>);

	case LogicalTypeId::INTEGER:
		return BoundCastInfo(&VectorCastHelpers::TryCastErrorLoop<bignum_t, int32_t, BignumToIntErrorCast>);

	case LogicalTypeId::UINTEGER:
		return BoundCastInfo(&VectorCastHelpers::TryCastErrorLoop<bignum_t, uint32_t, BignumToIntErrorCast>);

	case LogicalTypeId::BIGINT:
		return BoundCastInfo(&VectorCastHelpers::TryCastErrorLoop<bignum_t, int64_t, BignumToIntErrorCast>);

	case LogicalTypeId::UBIGINT:
		return BoundCastInfo(&VectorCastHelpers::TryCastErrorLoop<bignum_t, uint64_t, BignumToIntErrorCast>);

	case LogicalTypeId::HUGEINT:
		return BoundCastInfo(&VectorCastHelpers::TryCastErrorLoop<bignum_t, hugeint_t, BignumToIntErrorCast>);

	case LogicalTypeId::UHUGEINT:
		return BoundCastInfo(&VectorCastHelpers::TryCastErrorLoop<bignum_t, uhugeint_t, BignumToIntErrorCast>);

	case LogicalTypeId::VARCHAR:
		return BoundCastInfo(&Bignum::BignumToVarcharCast);
	case LogicalTypeId::DOUBLE:
		return BoundCastInfo(&VectorCastHelpers::TryCastErrorLoop<bignum_t, double, BignumToDoubleErrorCast>);
	default:
		return TryVectorNullCast;
	}
}

} // namespace duckdb
