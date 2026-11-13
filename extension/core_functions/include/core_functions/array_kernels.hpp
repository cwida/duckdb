#pragma once
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/algorithm.hpp"
#include <cmath>

namespace duckdb {

//-------------------------------------------------------------------------
// Folding Operations
//-------------------------------------------------------------------------
//! Number of independent partial sums used by ArrayFoldSum
static constexpr idx_t ARRAY_FOLD_LANES = 16;

//! Sums OP::Term over the element pairs, using independent partial sums so the compiler can vectorize the loop
template <class OP, class TYPE>
static TYPE ArrayFoldSum(const TYPE *lhs_data, const TYPE *rhs_data, const idx_t count) {
	TYPE result = 0;
	if (count < ARRAY_FOLD_LANES) {
		for (idx_t i = 0; i < count; i++) {
			result += OP::Term(lhs_data[i], rhs_data[i]);
		}
		return result;
	}

	TYPE lanes[ARRAY_FOLD_LANES] = {};
	idx_t i = 0;
	for (; i + ARRAY_FOLD_LANES <= count; i += ARRAY_FOLD_LANES) {
		for (idx_t lane = 0; lane < ARRAY_FOLD_LANES; lane++) {
			lanes[lane] += OP::Term(lhs_data[i + lane], rhs_data[i + lane]);
		}
	}
	for (idx_t lane = 0; lane < ARRAY_FOLD_LANES; lane++) {
		result += lanes[lane];
	}
	for (; i < count; i++) {
		result += OP::Term(lhs_data[i], rhs_data[i]);
	}
	return result;
}

struct ProductTerm {
	template <class TYPE>
	static TYPE Term(const TYPE x, const TYPE y) {
		return x * y;
	}
};

struct SquaredDifferenceTerm {
	template <class TYPE>
	static TYPE Term(const TYPE x, const TYPE y) {
		const auto diff = x - y;
		return diff * diff;
	}
};

struct InnerProductOp {
	static constexpr bool ALLOW_EMPTY = true;

	template <class TYPE>
	static TYPE Operation(const TYPE *lhs_data, const TYPE *rhs_data, const idx_t count) {
		return ArrayFoldSum<ProductTerm>(lhs_data, rhs_data, count);
	}
};

struct NegativeInnerProductOp {
	static constexpr bool ALLOW_EMPTY = true;

	template <class TYPE>
	static TYPE Operation(const TYPE *lhs_data, const TYPE *rhs_data, const idx_t count) {
		return -InnerProductOp::Operation(lhs_data, rhs_data, count);
	}
};

struct CosineSimilarityOp {
	static constexpr bool ALLOW_EMPTY = false;

	template <class TYPE>
	static TYPE Operation(const TYPE *lhs_data, const TYPE *rhs_data, const idx_t count) {
		TYPE distance = 0;
		TYPE norm_l = 0;
		TYPE norm_r = 0;
		if (count < ARRAY_FOLD_LANES) {
			for (idx_t i = 0; i < count; i++) {
				const auto x = lhs_data[i];
				const auto y = rhs_data[i];
				distance += x * y;
				norm_l += x * x;
				norm_r += y * y;
			}
		} else {
			// separate sums vectorize better than a single loop computing all three
			distance = ArrayFoldSum<ProductTerm>(lhs_data, rhs_data, count);
			norm_l = ArrayFoldSum<ProductTerm>(lhs_data, lhs_data, count);
			norm_r = ArrayFoldSum<ProductTerm>(rhs_data, rhs_data, count);
		}

		const auto norm_product = norm_l * norm_r;
		if (!std::isfinite(distance) || !std::isfinite(norm_product) ||
		    norm_product < std::numeric_limits<TYPE>::min()) {
			// the computation overflowed or underflowed (or there are NaN / infinite values)
			return ScaledSimilarity(lhs_data, rhs_data, count);
		}
		return Clamp(distance / std::sqrt(norm_product));
	}

private:
	template <class TYPE>
	static TYPE Clamp(TYPE similarity) {
		if (std::isnan(similarity)) {
			return similarity;
		}
		return std::max(static_cast<TYPE>(-1.0), std::min(similarity, static_cast<TYPE>(1.0)));
	}

	//! Computes the similarity with both vectors scaled by their largest absolute value, which does not change the
	//! similarity, but avoids overflow and underflow
	template <class TYPE>
	static TYPE ScaledSimilarity(const TYPE *lhs_data, const TYPE *rhs_data, const idx_t count) {
		TYPE max_l = 0;
		TYPE max_r = 0;
		for (idx_t i = 0; i < count; i++) {
			max_l = std::max(max_l, std::abs(lhs_data[i]));
			max_r = std::max(max_r, std::abs(rhs_data[i]));
		}
		if (max_l == 0 || max_r == 0 || !std::isfinite(max_l) || !std::isfinite(max_r)) {
			// the similarity is undefined for zero and infinite vectors
			return std::numeric_limits<TYPE>::quiet_NaN();
		}
		TYPE distance = 0;
		TYPE norm_l = 0;
		TYPE norm_r = 0;
		for (idx_t i = 0; i < count; i++) {
			const auto x = lhs_data[i] / max_l;
			const auto y = rhs_data[i] / max_r;
			distance += x * y;
			norm_l += x * x;
			norm_r += y * y;
		}
		return Clamp(distance / std::sqrt(norm_l * norm_r));
	}
};

struct CosineDistanceOp {
	static constexpr bool ALLOW_EMPTY = false;

	template <class TYPE>
	static TYPE Operation(const TYPE *lhs_data, const TYPE *rhs_data, const idx_t count) {
		return static_cast<TYPE>(1.0) - CosineSimilarityOp::Operation(lhs_data, rhs_data, count);
	}
};

struct DistanceSquaredOp {
	static constexpr bool ALLOW_EMPTY = true;

	template <class TYPE>
	static TYPE Operation(const TYPE *lhs_data, const TYPE *rhs_data, const idx_t count) {
		return ArrayFoldSum<SquaredDifferenceTerm>(lhs_data, rhs_data, count);
	}
};

struct DistanceOp {
	static constexpr bool ALLOW_EMPTY = true;

	template <class TYPE>
	static TYPE Operation(const TYPE *lhs_data, const TYPE *rhs_data, const idx_t count) {
		return std::sqrt(DistanceSquaredOp::Operation(lhs_data, rhs_data, count));
	}
};

} // namespace duckdb
