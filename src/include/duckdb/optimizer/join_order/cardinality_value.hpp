//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/join_order/cardinality_value.hpp
//
//===----------------------------------------------------------------------===//
#pragma once

#include "duckdb/common/common.hpp"

#include <cmath>
#include <limits>

namespace duckdb {

//! Retain scaled numerator and denominator products until the final division.
//! Inputs are finite nonnegative row/domain counts or selectivity factors; only accumulated products need extra range.
class CardinalityValue {
public:
	explicit CardinalityValue(double value = 1) : CardinalityValue(Product(value), Product(1)) {
	}

	bool IsZero() const {
		return numerator.significand == 0;
	}

	double GetDouble() const {
		if (IsZero()) {
			return 0;
		}
		Product quotient(numerator.significand / denominator);
		quotient.exponent += numerator.exponent;
		if (quotient.exponent > std::numeric_limits<double>::max_exponent) {
			// A final overflow remains infinity. Saturating here would also tie costs and can overflow when adding
			// them.
			return std::numeric_limits<double>::infinity();
		}
		if (quotient.exponent < std::numeric_limits<double>::min_exponent - std::numeric_limits<double>::digits) {
			return 0;
		}
		return std::ldexp(quotient.significand, static_cast<int>(quotient.exponent));
	}

	idx_t GetIndex() const {
		auto value = GetDouble();
		// Keep a positive estimate from becoming an empty input in subsequent optimization.
		if (value < 1) {
			return IsZero() ? 0 : 1;
		}
		auto maximum = std::numeric_limits<idx_t>::max();
		return value >= static_cast<double>(maximum) ? maximum : static_cast<idx_t>(value);
	}

	CardinalityValue operator*(const CardinalityValue &other) const {
		return CardinalityValue(numerator * other.numerator, Product(denominator * other.denominator));
	}

	CardinalityValue operator/(const CardinalityValue &other) const {
		D_ASSERT(!other.IsZero());
		return CardinalityValue(numerator * other.denominator, other.numerator * denominator);
	}

	CardinalityValue &operator*=(const CardinalityValue &other) {
		*this = *this * other;
		return *this;
	}

	bool operator>(const CardinalityValue &other) const {
		return numerator * other.denominator > other.numerator * denominator;
	}

	bool operator<(const CardinalityValue &other) const {
		return other > *this;
	}

private:
	struct Product {
		explicit Product(double value) {
			D_ASSERT(std::isfinite(value) && value >= 0);
			int value_exponent;
			significand = std::frexp(value, &value_exponent);
			exponent = value_exponent;
		}

		Product operator*(const Product &other) const {
			auto result = *this * other.significand;
			if (result.significand != 0) {
				result.exponent += other.exponent;
			}
			return result;
		}

		Product operator*(double other) const {
			Product result(significand * other);
			if (result.significand != 0) {
				result.exponent += exponent;
			}
			return result;
		}

		bool operator>(const Product &other) const {
			if (significand == 0 || other.significand == 0) {
				return significand > other.significand;
			}
			return exponent == other.exponent ? significand > other.significand : exponent > other.exponent;
		}

		double significand;
		int64_t exponent;
	};

	CardinalityValue(Product numerator, Product denominator)
	    : numerator(numerator), denominator(denominator.significand) {
		// Remove common powers of two without dividing the significands.
		this->numerator.exponent -= denominator.exponent;
	}

	Product numerator;
	double denominator;
};

} // namespace duckdb
