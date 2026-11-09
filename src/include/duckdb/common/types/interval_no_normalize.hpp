#pragma once

#include "duckdb/common/types/hash.hpp"
#include "duckdb/common/types/interval.hpp"

namespace duckdb {

//! Dictionary-encoding wrapper for interval_t. interval_t's operators and hash normalize
//! months/days/micros first, so two spellings that are equal only after normalization land
//! in the same dictionary entry and a Parquet round trip rewrites one as the other.
struct interval_no_normalize {
	int32_t months;
	int32_t days;
	int64_t micros;

	interval_no_normalize() : months(0), days(0), micros(0) {
	}
	explicit interval_no_normalize(const interval_t &val) : months(val.months), days(val.days), micros(val.micros) {
	}
	// NOLINTNEXTLINE: allow implicit conversion to interval_t
	operator interval_t() const {
		return interval_t {months, days, micros};
	}

	bool operator==(const interval_no_normalize &right) const {
		return months == right.months && days == right.days && micros == right.micros;
	}
	bool operator!=(const interval_no_normalize &right) const {
		return !(*this == right);
	}
};

//! Deliberately not normalized: distinct (months, days, micros) are distinct keys, so a
//! Parquet dictionary does not merge intervals that are equal only after normalization.
inline hash_t Hash(interval_no_normalize val) {
	return Hash(val.months) ^ Hash(val.days) ^ Hash(val.micros);
}

} // namespace duckdb
