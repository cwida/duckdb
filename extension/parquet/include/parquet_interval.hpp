//===----------------------------------------------------------------------===//
//                         DuckDB
//
// parquet_interval.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/helper.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/optional.hpp"
#include "duckdb/common/types/interval.hpp"
#include "zstd/common/xxhash.hpp"

namespace duckdb {

struct ParquetIntervalUtils {
	static constexpr const idx_t PARQUET_INTERVAL_SIZE = 12;

	static void Encode(const interval_t &interval, data_ptr_t target) {
		auto days = int64_t(interval.days);
		auto micros = interval.micros;
		if (micros / Interval::MICROS_PER_MSEC > NumericLimits<uint32_t>::Maximum()) {
			// the milliseconds do not fit in 32 bits: move the whole days into the days field
			days += micros / Interval::MICROS_PER_DAY;
			micros %= Interval::MICROS_PER_DAY;
		}
		Store<uint32_t>(static_cast<uint32_t>(interval.months), target);
		Store<uint32_t>(NumericCast<uint32_t>(days), target + sizeof(uint32_t));
		Store<uint32_t>(NumericCast<uint32_t>(micros / Interval::MICROS_PER_MSEC), target + sizeof(uint32_t) * 2);
	}

	//! The hash DuckDB adds to the bloom filters of intervals (normalized-v1), of the normalized interval with its
	//! whole days in the days field. Normalizing can saturate and leave a part negative or above a day, so this does
	//! not use Encode. There is no hash for a negative part, as Parquet stores no negative intervals
	static optional<uint64_t> TryHashNormalized(const interval_t &interval) {
		const auto normalized = interval.Normalize();
		const auto days = int64_t(normalized.days) + normalized.micros / Interval::MICROS_PER_DAY;
		const auto micros = normalized.micros % Interval::MICROS_PER_DAY;
		if (normalized.months < 0 || days < 0 || micros < 0) {
			return nullopt;
		}
		data_t bytes[PARQUET_INTERVAL_SIZE];
		Store<uint32_t>(UnsafeNumericCast<uint32_t>(normalized.months), bytes);
		Store<uint32_t>(UnsafeNumericCast<uint32_t>(days), bytes + sizeof(uint32_t));
		Store<uint32_t>(UnsafeNumericCast<uint32_t>(micros / Interval::MICROS_PER_MSEC), bytes + sizeof(uint32_t) * 2);
		return duckdb_zstd::XXH64(bytes, sizeof(bytes), 0);
	}
};

} // namespace duckdb
