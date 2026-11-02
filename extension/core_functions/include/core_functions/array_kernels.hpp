#pragma once
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/algorithm.hpp"
#include <cmath>

namespace duckdb {

//-------------------------------------------------------------------------
// Folding Operations
//-------------------------------------------------------------------------
struct InnerProductOp {
	static constexpr bool ALLOW_EMPTY = true;

	template <class TYPE>
	static TYPE Operation(const TYPE *lhs_data, const TYPE *rhs_data, const idx_t count) {
		TYPE result = 0;

		auto lhs_ptr = lhs_data;
		auto rhs_ptr = rhs_data;

		for (idx_t i = 0; i < count; i++) {
			const auto x = *lhs_ptr++;
			const auto y = *rhs_ptr++;
			result += x * y;
		}

		return result;
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

		auto l_ptr = lhs_data;
		auto r_ptr = rhs_data;

		for (idx_t i = 0; i < count; i++) {
			const auto x = *l_ptr++;
			const auto y = *r_ptr++;
			distance += x * y;
			norm_l += x * x;
			norm_r += y * y;
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
		TYPE distance = 0;

		auto l_ptr = lhs_data;
		auto r_ptr = rhs_data;

		for (idx_t i = 0; i < count; i++) {
			const auto x = *l_ptr++;
			const auto y = *r_ptr++;
			const auto diff = x - y;
			distance += diff * diff;
		}

		return distance;
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
