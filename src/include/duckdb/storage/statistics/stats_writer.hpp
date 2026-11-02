//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/statistics/stats_writer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/bswap.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"
#include "duckdb/storage/statistics/string_stats.hpp"
#include "duckdb/storage/statistics/geometry_stats.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "utf8proc_wrapper.hpp"
#include "duckdb/main/error_manager.hpp"

#include <limits>
#include <type_traits>

namespace duckdb {

struct BaseStatsWriter {
	void SetHasNull() {
		has_null = true;
	}
	void SetHasValid() {
		has_valid = true;
	}

	bool HasStats() const {
		return has_null || has_valid;
	}

	bool AnyValid() const {
		return has_valid;
	}

	void ClearBase() {
		has_null = false;
		has_valid = false;
	}

	void MergeBase(BaseStatistics &other) const {
		if (has_null) {
			other.SetHasNullFast();
		}
		if (has_valid) {
			other.SetHasNoNullFast();
		}
	}

private:
	bool has_null = false;
	bool has_valid = false;
};

template <class T>
struct StatsWriter : public BaseStatsWriter {
	explicit StatsWriter() {
		Clear();
	}

	inline void Clear() {
		ClearBase();
		if constexpr (std::is_floating_point_v<T>) {
			min = std::numeric_limits<T>::quiet_NaN();
		} else {
			min = NumericLimits<T>::Maximum();
		}
		max = NumericLimits<T>::Minimum();
	}

	void Update(T new_value) {
		SetHasValid();
		UpdateMinMax(new_value);
	}

	void UpdateMinMax(T new_value) {
		NumericStats::UpdateValue(new_value, min, max);
	}

	void Merge(BaseStatistics &target) const {
		MergeBase(target);
		if (AnyValid()) {
			target.UpdateNumericStats<T>(min);
			target.UpdateNumericStats<T>(max);
		}
	}

private:
	T min;
	T max;
};

template <>
struct StatsWriter<void> : public BaseStatsWriter {
	explicit StatsWriter() {
		Clear();
	}

	inline void Clear() {
		ClearBase();
	}

	void Merge(BaseStatistics &target) const {
		MergeBase(target);
	}
};

//! Lexicographic compare of two 12-byte string prefixes that are zero-padded past the string length, as the
//! inline area of a string_t is. Returns <0, 0 or >0 like memcmp.
static inline int CompareInlineBytes(const_data_ptr_t a, const_data_ptr_t b) {
	const uint32_t a_prefix = Load<uint32_t>(a);
	const uint32_t b_prefix = Load<uint32_t>(b);
	if (a_prefix != b_prefix) {
		return BSwapIfLE(a_prefix) < BSwapIfLE(b_prefix) ? -1 : 1;
	}
	const uint64_t a_rest = Load<uint64_t>(a + sizeof(uint32_t));
	const uint64_t b_rest = Load<uint64_t>(b + sizeof(uint32_t));
	if (a_rest != b_rest) {
		return BSwapIfLE(a_rest) < BSwapIfLE(b_rest) ? -1 : 1;
	}
	return 0;
}

template <>
struct StatsWriter<string_t> : public BaseStatsWriter {
	friend struct StringStats;

	explicit StatsWriter(const LogicalType &type)
	    : is_varchar(type.id() == LogicalTypeId::VARCHAR), is_geometry(type.id() == LogicalTypeId::GEOMETRY) {
		Clear();
	}

	inline void Clear() {
		ClearBase();
		if (is_geometry) {
			geometry_stats.SetEmpty();
		} else {
			is_set = false;
			min_size = 0;
			max_size = 0;
			has_unicode = false;
			max_string_length = 0;
			min_string_length = StringStatsData::MAXIMUM_MIN_STRING_LENGTH;
			total_string_length = 0;
		}
	}

	inline void Update(const string_t &value) {
		SetHasValid();
		if (is_geometry) {
			geometry_stats.Update(value);
			return;
		}
		auto data = const_data_ptr_cast(value.GetData());
		auto size = value.GetSize();
#ifdef DUCKDB_DEBUG_NO_INLINE
		// without inlining a short string only has `size` readable bytes
		auto copy_count = MinValue<idx_t>(size, StringStatsData::CURRENT_MAX_STRING_MINMAX_SIZE);
		if (is_set) {
			// compare to current min/max
			auto min_cmp_count = MinValue<idx_t>(copy_count, min_size);
			auto min_cmp = memcmp(data, min, min_cmp_count);
			if (min_cmp < 0 || (min_cmp == 0 && size < min_size)) {
				memcpy(min, data, copy_count);
				min_size = size;
			}
			auto max_cmp_count = MinValue<idx_t>(copy_count, max_size);
			int max_cmp = memcmp(data, max, max_cmp_count);
			if (max_cmp > 0 || (max_cmp == 0 && size > max_size)) {
				memcpy(max, data, copy_count);
				max_size = size;
			}
		} else {
			memcpy(min, data, copy_count);
			memcpy(max, data, copy_count);
			min_size = size;
			max_size = size;
			is_set = true;
		}
#else
		// a string_t always has CURRENT_MAX_STRING_MINMAX_SIZE readable bytes at its data pointer, zero-padded
		// past the length when inlined, so min and max are kept as zero-padded prefixes and compared as integers
		static_assert(StringStatsData::CURRENT_MAX_STRING_MINMAX_SIZE == string_t::INLINE_LENGTH,
		              "the string statistics prefix is the inline area of a string_t");
		if (is_set) {
			// the prefix compare covers the first min(size, min_size) bytes exactly because the shorter side is
			// zero-padded, and the length breaks a tie
			auto min_cmp = CompareInlineBytes(data, min);
			if (min_cmp < 0 || (min_cmp == 0 && size < min_size)) {
				memcpy(min, data, StringStatsData::CURRENT_MAX_STRING_MINMAX_SIZE);
				min_size = size;
			}
			auto max_cmp = CompareInlineBytes(data, max);
			if (max_cmp > 0 || (max_cmp == 0 && size > max_size)) {
				memcpy(max, data, StringStatsData::CURRENT_MAX_STRING_MINMAX_SIZE);
				max_size = size;
			}
		} else {
			memcpy(min, data, StringStatsData::CURRENT_MAX_STRING_MINMAX_SIZE);
			memcpy(max, data, StringStatsData::CURRENT_MAX_STRING_MINMAX_SIZE);
			min_size = size;
			max_size = size;
			is_set = true;
		}
#endif
		if (size > max_string_length) {
			max_string_length = UnsafeNumericCast<uint32_t>(size);
		}
		if (size < min_string_length) {
			min_string_length = UnsafeNumericCast<uint32_t>(size);
		}
		total_string_length += size;
		if (is_varchar && !has_unicode) {
			auto unicode = Utf8Proc::Analyze(const_char_ptr_cast(data), size);
			if (unicode == UnicodeType::UTF8) {
				has_unicode = true;
			} else if (unicode == UnicodeType::INVALID) {
				throw ErrorManager::InvalidUnicodeError(string(const_char_ptr_cast(data), size),
				                                        "segment statistics update");
			}
		}
	}

	//! Update stats for a value that was already passed to Update - only additive stats need to be re-counted
	inline void UpdateRepeated(const string_t &value) {
		if (is_geometry) {
			Update(value);
			return;
		}
		SetHasValid();
		total_string_length += value.GetSize();
	}

	void Merge(BaseStatistics &other) const {
		MergeBase(other);
		if (is_geometry) {
			GeometryStats::GetDataUnsafe(other).Merge(geometry_stats);
		} else {
			StringStats::Merge(other, *this);
		}
	}

private:
	data_t min[StringStatsData::CURRENT_MAX_STRING_MINMAX_SIZE];
	data_t max[StringStatsData::CURRENT_MAX_STRING_MINMAX_SIZE];
	idx_t min_size;
	idx_t max_size;
	bool is_set;
	bool has_unicode;
	uint32_t max_string_length;
	uint32_t min_string_length;
	idx_t total_string_length;
	bool is_varchar;
	bool is_geometry;
	GeometryStatsData geometry_stats;
};

} // namespace duckdb
