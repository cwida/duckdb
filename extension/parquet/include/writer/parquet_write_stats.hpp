//===----------------------------------------------------------------------===//
//                         DuckDB
//
// writer/parquet_write_stats.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "column_writer.hpp"
#include "parquet_geometry.hpp"

namespace duckdb {

class ColumnWriterStatistics {
public:
	virtual ~ColumnWriterStatistics();

	virtual bool HasStats();
	virtual string GetMin();
	virtual string GetMax();
	virtual string GetMinValue();
	virtual string GetMaxValue();
	virtual bool CanHaveNaN();
	virtual bool HasNaN();
	virtual idx_t GetNaNCount();
	virtual bool MinIsExact();
	virtual bool MaxIsExact();

	virtual bool HasGeoStats();
	virtual optional_ptr<GeometryStatsData> GetGeoStats();
	virtual void WriteGeoStats(duckdb_parquet::GeospatialStatistics &stats);

	//! Folds the statistics of another accumulator of the same type into this one (page statistics -> column chunk)
	virtual void Merge(const ColumnWriterStatistics &other);
	//! Compares the minimum (maximum) of two accumulators of the same type that both have statistics:
	//! negative if this is smaller, zero if equal, positive if larger (used to derive the page index boundary order)
	virtual int CompareMin(const ColumnWriterStatistics &other) const;
	virtual int CompareMax(const ColumnWriterStatistics &other) const;

public:
	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}
	template <class TARGET>
	const TARGET &Cast() const {
		D_ASSERT(dynamic_cast<const TARGET *>(this));
		return reinterpret_cast<const TARGET &>(*this);
	}
};

template <class SRC, class T, class OP>
class NumericStatisticsState : public ColumnWriterStatistics {
public:
	NumericStatisticsState() : min(NumericLimits<T>::Maximum()), max(NumericLimits<T>::Minimum()) {
	}

	T min;
	T max;

public:
	bool HasStats() override {
		return min <= max;
	}

	string GetMin() override {
		return NumericLimits<SRC>::IsSigned() ? GetMinValue() : string();
	}
	string GetMax() override {
		return NumericLimits<SRC>::IsSigned() ? GetMaxValue() : string();
	}
	string GetMinValue() override {
		return HasStats() ? string(char_ptr_cast(&min), sizeof(T)) : string();
	}
	string GetMaxValue() override {
		return HasStats() ? string(char_ptr_cast(&max), sizeof(T)) : string();
	}

	void Merge(const ColumnWriterStatistics &other_p) override {
		auto &other = other_p.Cast<NumericStatisticsState<SRC, T, OP>>();
		if (!(other.min <= other.max)) {
			return;
		}
		if (LessThan::Operation(other.min, min)) {
			min = other.min;
		}
		if (GreaterThan::Operation(other.max, max)) {
			max = other.max;
		}
	}
	int CompareMin(const ColumnWriterStatistics &other_p) const override {
		auto &other = other_p.Cast<NumericStatisticsState<SRC, T, OP>>();
		return LessThan::Operation(min, other.min) ? -1 : (GreaterThan::Operation(min, other.min) ? 1 : 0);
	}
	int CompareMax(const ColumnWriterStatistics &other_p) const override {
		auto &other = other_p.Cast<NumericStatisticsState<SRC, T, OP>>();
		return LessThan::Operation(max, other.max) ? -1 : (GreaterThan::Operation(max, other.max) ? 1 : 0);
	}
};

template <class SRC, class T, class OP>
class FloatingPointStatisticsState : public NumericStatisticsState<SRC, T, OP> {
public:
	idx_t nan_count = 0;

public:
	bool CanHaveNaN() override {
		return true;
	}
	bool HasNaN() override {
		return nan_count != 0;
	}
	idx_t GetNaNCount() override {
		return nan_count;
	}
	void Merge(const ColumnWriterStatistics &other_p) override {
		NumericStatisticsState<SRC, T, OP>::Merge(other_p);
		nan_count += other_p.Cast<FloatingPointStatisticsState<SRC, T, OP>>().nan_count;
	}
};

class StringStatisticsState : public ColumnWriterStatistics {
	static constexpr const idx_t MAX_STRING_STATISTICS_SIZE = 256;

public:
	explicit StringStatisticsState(LogicalTypeId type = LogicalTypeId::VARCHAR)
	    : type(type), has_stats(false), min_truncated(false), max_truncated(false), min(), max() {
	}

	LogicalTypeId type;
	bool has_stats;
	bool min_truncated;
	bool max_truncated;
	bool failed_truncate = false;
	string min;
	string max;

public:
	bool HasStats() override {
		return has_stats;
	}

	void Update(const string_t &val) {
		if (failed_truncate) {
			return;
		}
		if (!has_stats || LessThan::Operation(val, string_t(min))) {
			if (val.GetSize() > MAX_STRING_STATISTICS_SIZE) {
				// string value exceeds our max string stats size - truncate
				min = TruncateMin(val, MAX_STRING_STATISTICS_SIZE);
				min_truncated = true;
			} else {
				min = val.GetString();
				min_truncated = false;
			}
		}
		if (!has_stats || GreaterThan::Operation(val, string_t(max))) {
			if (val.GetSize() > MAX_STRING_STATISTICS_SIZE) {
				// string value exceeds our max string stats size - truncate
				if (!TryTruncateMax(val, MAX_STRING_STATISTICS_SIZE, max)) {
					// we failed to truncate - this can happen in some edge cases
					// skip stats for this column
					failed_truncate = true;
					has_stats = false;
					min = string();
					max = string();
					return;
				}
				max_truncated = true;
			} else {
				max = val.GetString();
				max_truncated = false;
			}
		}
		has_stats = true;
	}

	static inline bool IsCharacter(char c) {
		return (c & 0xc0) != 0x80;
	}

	string TruncateMin(string_t str, idx_t max_size) {
		// truncate a string for the min value
		// since 'AAA' < 'AAAA', we can just truncate the string
		D_ASSERT(str.GetSize() > max_size);
		if (type == LogicalTypeId::BLOB) {
			// for blobs - just truncate directly
			return string(str.GetData(), max_size);
		}
		D_ASSERT(type == LogicalTypeId::VARCHAR);
		// for varchar we need to truncate to a valid UTF8 string - so we need to truncate to the last valid UTF8 byte
		auto str_data = str.GetData();
		for (; max_size > 0; max_size--) {
			if (IsCharacter(str_data[max_size])) {
				break;
			}
		}
		return string(str_data, max_size);
	}

	bool TryTruncateMax(string_t str, idx_t max_size, string &result, data_t max_byte) {
		auto data = const_data_ptr_cast(str.GetData());

		// find the last position in the string which we can increment for the truncation
		// if ALL characters are above the max byte we cannot truncate
		idx_t increment_pos;
		for (increment_pos = max_size; increment_pos > 0; increment_pos--) {
			idx_t str_idx = increment_pos - 1;
			if (data[str_idx] < max_byte) {
				// found the increment position
				break;
			}
		}
		if (increment_pos == 0) {
			// all characters are above the max byte - we cannot truncate - return false
			return false;
		}
		// set up the result string - we don't care about anything after the increment pos
		result = string(str.GetData(), increment_pos);
		// actually increment
		result[increment_pos - 1]++;
		return true;
	}

	bool TryTruncateMax(string_t str, idx_t max_size, string &result) {
		// truncate a string for the max value
		// since 'XXX' < 'XXXX', we need to "increment" a byte to get a correct max value
		// i.e. we need to generate 'XXY' as a string
		// note that this is not necessarily always possible
		D_ASSERT(str.GetSize() > max_size);
		if (type == LogicalTypeId::BLOB) {
			// for blobs we can always increment bytes - we just can't increment past the max of a single byte (2^8)
			return TryTruncateMax(str, max_size, result, static_cast<data_t>(0xFF));
		}
		D_ASSERT(type == LogicalTypeId::VARCHAR);
		// for varchar the situation is more complex - we need to truncate to a valid UTF8 string and increment
		// for now we only increment ASCII characters (characters below 0x7F)
		return TryTruncateMax(str, max_size, result, static_cast<data_t>(0x7F));
	}

	string GetMin() override {
		return GetMinValue();
	}
	string GetMax() override {
		return GetMaxValue();
	}
	string GetMinValue() override {
		return HasStats() ? min : string();
	}
	string GetMaxValue() override {
		return HasStats() ? max : string();
	}

	bool MinIsExact() override {
		return !min_truncated;
	}

	bool MaxIsExact() override {
		return !max_truncated;
	}

	void Merge(const ColumnWriterStatistics &other_p) override {
		auto &other = other_p.Cast<StringStatisticsState>();
		if (other.failed_truncate) {
			failed_truncate = true;
			has_stats = false;
			min = string();
			max = string();
			return;
		}
		if (failed_truncate || !other.has_stats) {
			return;
		}
		// a truncated bound stays a valid bound of the merged range, only its exactness is lost
		if (!has_stats || LessThan::Operation(string_t(other.min), string_t(min))) {
			min = other.min;
			min_truncated = other.min_truncated;
		}
		if (!has_stats || GreaterThan::Operation(string_t(other.max), string_t(max))) {
			max = other.max;
			max_truncated = other.max_truncated;
		}
		has_stats = true;
	}
	int CompareMin(const ColumnWriterStatistics &other_p) const override {
		auto &other = other_p.Cast<StringStatisticsState>();
		return LessThan::Operation(string_t(min), string_t(other.min))
		           ? -1
		           : (GreaterThan::Operation(string_t(min), string_t(other.min)) ? 1 : 0);
	}
	int CompareMax(const ColumnWriterStatistics &other_p) const override {
		auto &other = other_p.Cast<StringStatisticsState>();
		return LessThan::Operation(string_t(max), string_t(other.max))
		           ? -1
		           : (GreaterThan::Operation(string_t(max), string_t(other.max)) ? 1 : 0);
	}
};

struct ParquetUUIDTargetType {
	static constexpr const idx_t PARQUET_UUID_SIZE = 16;
	data_t bytes[PARQUET_UUID_SIZE];
};

class UUIDStatisticsState : public ColumnWriterStatistics {
public:
	bool has_stats = false;
	ParquetUUIDTargetType min;
	ParquetUUIDTargetType max;

public:
	bool HasStats() override {
		return has_stats;
	}

	string GetMin() override {
		return GetMinValue();
	}
	string GetMax() override {
		return GetMaxValue();
	}
	string GetMinValue() override {
		return HasStats() ? string(char_ptr_cast(min.bytes), ParquetUUIDTargetType::PARQUET_UUID_SIZE) : string();
	}
	string GetMaxValue() override {
		return HasStats() ? string(char_ptr_cast(max.bytes), ParquetUUIDTargetType::PARQUET_UUID_SIZE) : string();
	}

	void Merge(const ColumnWriterStatistics &other_p) override {
		auto &other = other_p.Cast<UUIDStatisticsState>();
		if (!other.has_stats) {
			return;
		}
		if (!has_stats || memcmp(other.min.bytes, min.bytes, ParquetUUIDTargetType::PARQUET_UUID_SIZE) < 0) {
			min = other.min;
		}
		if (!has_stats || memcmp(other.max.bytes, max.bytes, ParquetUUIDTargetType::PARQUET_UUID_SIZE) > 0) {
			max = other.max;
		}
		has_stats = true;
	}
	int CompareMin(const ColumnWriterStatistics &other_p) const override {
		auto &other = other_p.Cast<UUIDStatisticsState>();
		return memcmp(min.bytes, other.min.bytes, ParquetUUIDTargetType::PARQUET_UUID_SIZE);
	}
	int CompareMax(const ColumnWriterStatistics &other_p) const override {
		auto &other = other_p.Cast<UUIDStatisticsState>();
		return memcmp(max.bytes, other.max.bytes, ParquetUUIDTargetType::PARQUET_UUID_SIZE);
	}
};

class GeoStatisticsState final : public ColumnWriterStatistics {
public:
	explicit GeoStatisticsState() : has_stats(false) {
		geo_stats.SetEmpty();
	}

	bool has_stats;
	GeometryStatsData geo_stats;

public:
	void Update(const string_t &val) {
		geo_stats.Update(val);
		has_stats = true;
	}
	bool HasGeoStats() override {
		return has_stats;
	}
	optional_ptr<GeometryStatsData> GetGeoStats() override {
		return geo_stats;
	}
	void Merge(const ColumnWriterStatistics &other_p) override {
		auto &other = other_p.Cast<GeoStatisticsState>();
		if (!other.has_stats) {
			return;
		}
		geo_stats.Merge(other.geo_stats);
		has_stats = true;
	}
	void WriteGeoStats(duckdb_parquet::GeospatialStatistics &stats) override {
		const auto &types = geo_stats.types;
		const auto &bbox = geo_stats.extent;

		if (bbox.HasXY()) {
			stats.__isset.bbox = true;
			stats.bbox.xmin = bbox.x_min;
			stats.bbox.xmax = bbox.x_max;
			stats.bbox.ymin = bbox.y_min;
			stats.bbox.ymax = bbox.y_max;

			if (bbox.HasZ()) {
				stats.bbox.__isset.zmin = true;
				stats.bbox.__isset.zmax = true;
				stats.bbox.zmin = bbox.z_min;
				stats.bbox.zmax = bbox.z_max;
			}
			if (bbox.HasM()) {
				stats.bbox.__isset.mmin = true;
				stats.bbox.__isset.mmax = true;
				stats.bbox.mmin = bbox.m_min;
				stats.bbox.mmax = bbox.m_max;
			}
		}

		stats.__isset.geospatial_types = true;
		auto wkb_types = types.ToWKBList();
		stats.geospatial_types.assign(wkb_types.begin(), wkb_types.end());
	}
};

} // namespace duckdb
