//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/numeric_utils.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cmath>
#include <type_traits>

#include "duckdb/common/hugeint.hpp"
#include "duckdb/common/limits.hpp"

namespace duckdb {

template <class T>
T RoundToNearestEven(T value) {
	static_assert(std::is_floating_point<T>::value, "RoundToNearestEven requires a floating point type");
	if (!std::isfinite(value)) {
		return value;
	}
	const T truncated = std::trunc(value);
	const T fraction = std::fabs(value - truncated);
	if (fraction < static_cast<T>(0.5)) {
		return truncated;
	}
	const T next = value < 0 ? truncated - static_cast<T>(1) : truncated + static_cast<T>(1);
	if (fraction > static_cast<T>(0.5)) {
		return next;
	}
	return std::fmod(truncated, static_cast<T>(2)) == 0 ? truncated : next;
}

//! Rounds halves away from zero like the C round, whose mingw-w64 version misrounds the double just below 0.5
template <class T>
T RoundToNearestAwayFromZero(T value) {
	static_assert(std::is_floating_point<T>::value, "RoundToNearestAwayFromZero requires a floating point type");
	if (!std::isfinite(value)) {
		return value;
	}
	const T truncated = std::trunc(value);
	// the fraction is exact, so the comparison with a half is exact
	if (std::fabs(value - truncated) < static_cast<T>(0.5)) {
		return truncated;
	}
	return value < 0 ? truncated - static_cast<T>(1) : truncated + static_cast<T>(1);
}

template <class T>
struct MakeSigned {
	using type = typename std::make_signed<T>::type;
};

template <>
struct MakeSigned<hugeint_t> {
	using type = hugeint_t;
};

template <>
struct MakeSigned<uhugeint_t> {
	using type = hugeint_t;
};

template <class T>
struct MakeUnsigned {
	using type = typename std::make_unsigned<T>::type;
};

template <>
struct MakeUnsigned<hugeint_t> {
	using type = uhugeint_t;
};

template <>
struct MakeUnsigned<uhugeint_t> {
	using type = uhugeint_t;
};

template <class TO, class FROM>
static void ThrowNumericCastError(FROM in, TO minval, TO maxval) {
	throw InternalException("Information loss on integer cast: value %d outside of target range [%d, %d]", in, minval,
	                        maxval);
}

template <class TO, class FROM, bool are_same_type>
struct NumericCastImpl;

template <class TO, class FROM>
struct NumericCastImpl<TO, FROM, true> {
	static TO Convert(FROM val) {
		return static_cast<TO>(val);
	}
};

template <class TO, class FROM>
struct NumericCastImpl<TO, FROM, false> {
	static TO Convert(FROM val) {
		// some dance around signed-unsigned integer comparison below
		auto minval = NumericLimits<TO>::Minimum();
		auto maxval = NumericLimits<TO>::Maximum();
		auto unsigned_in = static_cast<typename MakeUnsigned<FROM>::type>(val);
		auto unsigned_min = static_cast<typename MakeUnsigned<TO>::type>(minval);
		auto unsigned_max = static_cast<typename MakeUnsigned<TO>::type>(maxval);
		auto signed_in = static_cast<typename MakeSigned<FROM>::type>(val);
		auto signed_min = static_cast<typename MakeSigned<TO>::type>(minval);
		auto signed_max = static_cast<typename MakeSigned<TO>::type>(maxval);

		if (!NumericLimits<FROM>::IsSigned() && !NumericLimits<TO>::IsSigned() &&
		    (unsigned_in < unsigned_min || unsigned_in > unsigned_max)) {
			ThrowNumericCastError(val, static_cast<TO>(unsigned_min), static_cast<TO>(unsigned_max));
		}

		if (NumericLimits<FROM>::IsSigned() && NumericLimits<TO>::IsSigned() &&
		    (signed_in < signed_min || signed_in > signed_max)) {
			ThrowNumericCastError(val, static_cast<TO>(signed_min), static_cast<TO>(signed_max));
		}

		if (NumericLimits<FROM>::IsSigned() != NumericLimits<TO>::IsSigned() &&
		    (signed_in < signed_min || unsigned_in > unsigned_max)) {
			ThrowNumericCastError(val, static_cast<TO>(signed_min), static_cast<TO>(unsigned_max));
		}

		return static_cast<TO>(val);
	}
};

// NumericCast
// When: between same types, or when both types are integral
// Checks: perform checked casts on range
template <class TO, class FROM,
          class = typename std::enable_if<(NumericLimits<TO>::IsIntegral() && NumericLimits<FROM>::IsIntegral()) ||
                                          std::is_same<TO, FROM>::value>::type>
TO NumericCast(FROM val) {
	return NumericCastImpl<TO, FROM, std::is_same<TO, FROM>::value>::Convert(val);
}

// UnsafeNumericCast
// When: between same types, or when both types are integral
// Checks: perform checked casts on range (in DEBUG) otherwise no checks
template <class TO, class FROM,
          class = typename std::enable_if<(NumericLimits<TO>::IsIntegral() && NumericLimits<FROM>::IsIntegral()) ||
                                          std::is_same<TO, FROM>::value>::type>
TO UnsafeNumericCast(FROM in) {
#if defined(DEBUG) || defined(UNSAFE_NUMERIC_CAST)
	return NumericCast<TO, FROM>(in);
#endif
	return static_cast<TO>(in);
}

// LossyNumericCast
// When: between double/float to other convertible types
// Checks: no checks performed (at the moment, to be improved adding range checks)
template <class TO, class FROM>
TO LossyNumericCast(FROM val) {
	return static_cast<TO>(val);
}

// ExactNumericCast
// When: between double/float to other convertible types
// Checks: perform checks that casts are invertible (in DEBUG) otherwise no checks

template <class TO>
TO ExactNumericCast(double val) {
	auto res = LossyNumericCast<TO>(val);
#if defined(DEBUG) || defined(UNSAFE_NUMERIC_CAST)
	if (val != double(res)) {
		throw InternalException("Information loss on double cast: value %lf outside of target range [%lf, %lf]", val,
		                        double(res), double(res));
	}
#endif
	return res;
}

template <class TO>
TO ExactNumericCast(float val) {
	auto res = LossyNumericCast<TO>(val);
#if defined(DEBUG) || defined(UNSAFE_NUMERIC_CAST)
	if (val != float(res)) {
		throw InternalException("Information loss on float cast: value %f outside of target range [%f, %f]", val,
		                        float(res), float(res));
	}
#endif
	return res;
}

template <class T>
struct NextUnsigned {};

template <>
struct NextUnsigned<uint8_t> {
	using type = uint16_t;
};

template <>
struct NextUnsigned<uint16_t> {
	using type = uint32_t;
};

template <>
struct NextUnsigned<uint32_t> {
	using type = uint64_t;
};

template <>
struct NextUnsigned<uint64_t> {
#if ((__GNUC__ >= 5) || defined(__clang__)) && defined(__SIZEOF_INT128__)
	using type = __uint128_t;
#else
	using type = uhugeint_t;
#endif
};

template <class TYPE>
class FastMod {
	using NEXT_TYPE = typename NextUnsigned<TYPE>::type;
	static_assert(sizeof(NEXT_TYPE) != 0, "NextUnsigned not available for this type");

public:
	explicit FastMod(TYPE divisor_p) : divisor(divisor_p), multiplier((static_cast<TYPE>(-1) / divisor) + 1) {
	}

	TYPE Div(const TYPE &val) const {
		return static_cast<TYPE>((static_cast<NEXT_TYPE>(val) * multiplier) >> (sizeof(TYPE) * 8)); // NOLINT
	}

	TYPE Mod(const TYPE &val, const TYPE &quotient) const {
		return val - quotient * divisor;
	}

	TYPE Mod(const TYPE &val) const {
		return Mod(val, Div(val));
	}

	const TYPE &GetDivisor() const {
		return divisor;
	}

private:
	const TYPE divisor;
	const TYPE multiplier;
};

} // namespace duckdb
