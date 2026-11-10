//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/operator/comparison_operators.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/helper.hpp"
#include "duckdb/common/likely.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/types/string_type.hpp"

#include <cmath>
#include <cstring>

namespace duckdb {

//===--------------------------------------------------------------------===//
// Comparison Operations
//===--------------------------------------------------------------------===//
struct Equals {
	template <class T>
	static inline bool Operation(const T &left, const T &right) {
		return left == right;
	}
};
struct NotEquals {
	template <class T>
	static inline bool Operation(const T &left, const T &right) {
		return !Equals::Operation(left, right);
	}
};

struct GreaterThan {
	template <class T>
	static inline bool Operation(const T &left, const T &right) {
		return left > right;
	}
};

struct GreaterThanEquals {
	template <class T>
	static inline bool Operation(const T &left, const T &right) {
		return !GreaterThan::Operation(right, left);
	}
};

struct LessThan {
	template <class T>
	static inline bool Operation(const T &left, const T &right) {
		return GreaterThan::Operation(right, left);
	}
};

struct LessThanEquals {
	template <class T>
	static inline bool Operation(const T &left, const T &right) {
		return !GreaterThan::Operation(left, right);
	}
};

struct Comparator {
	static constexpr int8_t LEFT_IS_GREATER = 1;
	static constexpr int8_t RIGHT_IS_GREATER = -1;
	static constexpr int8_t VALUES_ARE_EQUAL = 0;

	template <class T>
	static inline int8_t Operation(const T &left, const T &right) {
		if (GreaterThan::Operation(left, right)) {
			return LEFT_IS_GREATER;
		}
		if (GreaterThan::Operation(right, left)) {
			return RIGHT_IS_GREATER;
		}
		return VALUES_ARE_EQUAL;
	}
};

//===--------------------------------------------------------------------===//
// Floating Point Comparisons
//===--------------------------------------------------------------------===//
template <class T>
inline bool EqualsFloat(T left, T right) {
	if (DUCKDB_UNLIKELY(std::isnan(left) && std::isnan(right))) {
		return true;
	}
	return left == right;
}

template <>
DUCKDB_API inline bool Equals::Operation(const float &left, const float &right) {
	return EqualsFloat<float>(left, right);
}

template <>
DUCKDB_API inline bool Equals::Operation(const double &left, const double &right) {
	return EqualsFloat<double>(left, right);
}

template <class T>
inline bool GreaterThanFloat(T left, T right) {
	// handle nans
	// nan is always bigger than everything else
	bool left_is_nan = std::isnan(left);
	bool right_is_nan = std::isnan(right);
	// if right is nan, there is no number that is bigger than right
	if (DUCKDB_UNLIKELY(right_is_nan)) {
		return false;
	}
	// if left is nan, but right is not, left is always bigger
	if (DUCKDB_UNLIKELY(left_is_nan)) {
		return true;
	}
	return left > right;
}

template <>
DUCKDB_API inline bool GreaterThan::Operation(const float &left, const float &right) {
	return GreaterThanFloat<float>(left, right);
}

template <>
DUCKDB_API inline bool GreaterThan::Operation(const double &left, const double &right) {
	return GreaterThanFloat<double>(left, right);
}

template <class T>
inline bool GreaterThanEqualsFloat(T left, T right) {
	// handle nans
	// nan is always bigger than everything else
	bool left_is_nan = std::isnan(left);
	bool right_is_nan = std::isnan(right);
	// if right is nan, there is no bigger number
	// we only return true if left is also nan (in which case the numbers are equal)
	if (DUCKDB_UNLIKELY(right_is_nan)) {
		return left_is_nan;
	}
	// if left is nan, but right is not, left is always bigger
	if (DUCKDB_UNLIKELY(left_is_nan)) {
		return true;
	}
	return left >= right;
}

template <>
DUCKDB_API inline bool GreaterThanEquals::Operation(const float &left, const float &right) {
	return GreaterThanEqualsFloat<float>(left, right);
}

template <>
DUCKDB_API inline bool GreaterThanEquals::Operation(const double &left, const double &right) {
	return GreaterThanEqualsFloat<double>(left, right);
}

template <class T>
inline int8_t ComparatorFloat(T left, T right) {
	bool left_is_nan = std::isnan(left);
	bool right_is_nan = std::isnan(right);
	if (DUCKDB_UNLIKELY(left_is_nan || right_is_nan)) {
		if (left_is_nan && right_is_nan) {
			return Comparator::VALUES_ARE_EQUAL;
		}
		// NaN is bigger than anything
		return left_is_nan ? Comparator::LEFT_IS_GREATER : Comparator::RIGHT_IS_GREATER;
	}
	if (left < right) {
		return Comparator::RIGHT_IS_GREATER;
	}
	if (left > right) {
		return Comparator::LEFT_IS_GREATER;
	}
	return Comparator::VALUES_ARE_EQUAL;
}

template <>
DUCKDB_API inline int8_t Comparator::Operation(const float &left, const float &right) {
	return ComparatorFloat<float>(left, right);
}

template <>
DUCKDB_API inline int8_t Comparator::Operation(const double &left, const double &right) {
	return ComparatorFloat<double>(left, right);
}

//===--------------------------------------------------------------------===//
// Distinct Comparisons
//===--------------------------------------------------------------------===//
// Distinct semantics are from Postgres record sorting. NULL = NULL and not-NULL < NULL
// Deferring to the non-distinct operations removes the need for further specialisation.
// TODO: To reverse the semantics, swap left_null and right_null for comparisons
struct DistinctFrom {
	template <class T>
	static inline bool Operation(const T &left, const T &right, bool left_null, bool right_null) {
		if (left_null || right_null) {
			return left_null != right_null;
		}
		return NotEquals::Operation(left, right);
	}
};

struct NotDistinctFrom {
	template <class T>
	static inline bool Operation(const T &left, const T &right, bool left_null, bool right_null) {
		return !DistinctFrom::Operation(left, right, left_null, right_null);
	}
};

struct DistinctGreaterThan {
	template <class T>
	static inline bool Operation(const T &left, const T &right, bool left_null, bool right_null) {
		if (left_null || right_null) {
			return !right_null;
		}
		return GreaterThan::Operation(left, right);
	}
};

struct DistinctGreaterThanNullsFirst {
	template <class T>
	static inline bool Operation(const T &left, const T &right, bool left_null, bool right_null) {
		return DistinctGreaterThan::Operation(left, right, right_null, left_null);
	}
};

struct DistinctGreaterThanEquals {
	template <class T>
	static inline bool Operation(const T &left, const T &right, bool left_null, bool right_null) {
		return !DistinctGreaterThan::Operation(right, left, right_null, left_null);
	}
};

struct DistinctLessThan {
	template <class T>
	static inline bool Operation(const T &left, const T &right, bool left_null, bool right_null) {
		return DistinctGreaterThan::Operation(right, left, right_null, left_null);
	}
};

struct DistinctLessThanNullsFirst {
	template <class T>
	static inline bool Operation(const T &left, const T &right, bool left_null, bool right_null) {
		return DistinctGreaterThan::Operation(right, left, left_null, right_null);
	}
};

struct DistinctLessThanEquals {
	template <class T>
	static inline bool Operation(const T &left, const T &right, bool left_null, bool right_null) {
		return !DistinctGreaterThan::Operation(left, right, left_null, right_null);
	}
};

struct DistinctComparator {
	template <class T>
	static inline int8_t Operation(const T &left, const T &right, bool left_null, bool right_null) {
		if (DUCKDB_UNLIKELY(left_null || right_null)) {
			if (left_null && right_null) {
				return 0;
			}
			// NULLS LAST: NULL is greater than any non-NULL value
			return left_null ? 1 : -1;
		}
		return Comparator::Operation<T>(left, right);
	}
};

struct DistinctComparatorNullsFirst {
	template <class T>
	static inline int8_t Operation(const T &left, const T &right, bool left_null, bool right_null) {
		if (DUCKDB_UNLIKELY(left_null || right_null)) {
			if (left_null && right_null) {
				return 0;
			}
			// NULLS FIRST: NULL is smaller than any non-NULL value
			return left_null ? -1 : 1;
		}
		return Comparator::Operation<T>(left, right);
	}
};

//===--------------------------------------------------------------------===//
// Comparison Operator Wrappers (so (Not)DistinctFrom have the same API)
//===--------------------------------------------------------------------===//
template <class OP>
struct ComparisonOperationWrapper {
	static constexpr const bool COMPARE_NULL = false;

	template <class T>
	static inline bool Operation(const T &left, const T &right, bool left_null, bool right_null) {
		if (right_null || left_null) {
			return false;
		}
		return OP::template Operation<T>(left, right);
	}
};

template <>
struct ComparisonOperationWrapper<DistinctFrom> {
	static constexpr const bool COMPARE_NULL = true;

	template <class T>
	static inline bool Operation(const T &left, const T &right, bool left_null, bool right_null) {
		return DistinctFrom::template Operation<T>(left, right, left_null, right_null);
	}
};

template <>
struct ComparisonOperationWrapper<NotDistinctFrom> {
	static constexpr const bool COMPARE_NULL = true;

	template <class T>
	static inline bool Operation(const T &left, const T &right, bool left_null, bool right_null) {
		return NotDistinctFrom::template Operation<T>(left, right, left_null, right_null);
	}
};

//===--------------------------------------------------------------------===//
// Specialized Boolean Comparison Operators
//===--------------------------------------------------------------------===//
template <>
inline bool GreaterThan::Operation(const bool &left, const bool &right) {
	return !right && left;
}
//===--------------------------------------------------------------------===//
// Specialized String Comparison Operations
//===--------------------------------------------------------------------===//
template <>
inline bool Equals::Operation(const string_t &left, const string_t &right) {
	return left == right;
}

template <>
inline bool GreaterThan::Operation(const string_t &left, const string_t &right) {
	return left > right;
}

//===--------------------------------------------------------------------===//
// Specialized Interval Comparison Operators
//===--------------------------------------------------------------------===//
template <>
inline bool Equals::Operation(const interval_t &left, const interval_t &right) {
	return Interval::Equals(left, right);
}
template <>
inline bool GreaterThan::Operation(const interval_t &left, const interval_t &right) {
	return Interval::GreaterThan(left, right);
}

} // namespace duckdb
