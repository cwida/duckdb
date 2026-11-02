//===----------------------------------------------------------------------===//
//                         DuckDB
//
// writer/parquet_write_operators.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "writer/parquet_write_stats.hpp"
#include "parquet_interval.hpp"
#include "parquet_timestamp.hpp"
#include "zstd/common/xxhash.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/uhugeint.hpp"
#include "duckdb/common/types/uuid.hpp"

namespace duckdb {

struct BaseParquetOperator {
	template <class SRC, class TGT>
	static void WriteToStream(const TGT &input, WriteStream &ser) {
		ser.WriteData(const_data_ptr_cast(&input), sizeof(TGT));
	}

	template <class SRC, class TGT>
	static constexpr idx_t WriteSize(const TGT &input) {
		return sizeof(TGT);
	}

	template <class SRC, class TGT>
	static uint64_t XXHash64(const TGT &target_value) {
		return duckdb_zstd::XXH64(&target_value, sizeof(target_value), 0);
	}

	template <class SRC, class TGT>
	static idx_t BloomFilterEntriesPerValue() {
		return 1;
	}

	template <class SRC, class TGT>
	static optional<uint64_t> GetExtraBloomFilterHash(const SRC &, const TGT &) {
		return nullopt;
	}

	template <class SRC, class TGT>
	static unique_ptr<ColumnWriterStatistics> InitializeStats() {
		return nullptr;
	}

	template <class SRC, class TGT>
	static void HandleStats(ColumnWriterStatistics *stats, TGT target_value) {
	}

	template <class SRC, class TGT>
	static idx_t GetRowSize(const Vector &, idx_t) {
		return sizeof(TGT);
	}
};

struct ParquetCastOperator : public BaseParquetOperator {
	template <class SRC, class TGT>
	static TGT Operation(SRC input) {
		return TGT(input);
	}
	template <class SRC, class TGT>
	static unique_ptr<ColumnWriterStatistics> InitializeStats() {
		return make_uniq<NumericStatisticsState<SRC, TGT, BaseParquetOperator>>();
	}

	template <class SRC, class TGT>
	static void HandleStats(ColumnWriterStatistics *stats, TGT target_value) {
		auto &numeric_stats = stats->Cast<NumericStatisticsState<SRC, TGT, BaseParquetOperator>>();
		if (LessThan::Operation(target_value, numeric_stats.min)) {
			numeric_stats.min = target_value;
		}
		if (GreaterThan::Operation(target_value, numeric_stats.max)) {
			numeric_stats.max = target_value;
		}
	}
};

struct FloatingPointOperator : public BaseParquetOperator {
	template <class SRC, class TGT>
	static TGT Operation(SRC input) {
		return TGT(input);
	}

	template <class SRC, class TGT>
	static unique_ptr<ColumnWriterStatistics> InitializeStats() {
		return make_uniq<FloatingPointStatisticsState<SRC, TGT, BaseParquetOperator>>();
	}

	template <class SRC, class TGT>
	static void HandleStats(ColumnWriterStatistics *stats, TGT target_value) {
		auto &numeric_stats = stats->Cast<FloatingPointStatisticsState<SRC, TGT, BaseParquetOperator>>();
		if (Value::IsNan(target_value)) {
			numeric_stats.nan_count++;
			return;
		}
		if (LessThan::Operation(target_value, numeric_stats.min)) {
			numeric_stats.min = target_value;
		}
		if (GreaterThan::Operation(target_value, numeric_stats.max)) {
			numeric_stats.max = target_value;
		}
	}
};

struct ParquetTimestampNSOperator : public ParquetCastOperator {
	template <class SRC, class TGT>
	static TGT Operation(SRC input) {
		return TGT(input);
	}
};

struct ParquetTimestampSOperator : public ParquetCastOperator {
	template <class SRC, class TGT>
	static TGT Operation(SRC input) {
		return Timestamp::FromEpochSecondsPossiblyInfinite(input).value;
	}
};

struct ParquetTimestampInt96Operator : public BaseParquetOperator {
	template <class SRC, class TGT>
	static TGT Operation(SRC input) {
		timestamp_t ts(input);
		return TimestampToImpalaTimestamp(ts);
	}
};

struct ParquetTimestampSInt96Operator : public BaseParquetOperator {
	template <class SRC, class TGT>
	static TGT Operation(SRC input) {
		auto ts = Timestamp::FromEpochSecondsPossiblyInfinite(input);
		return TimestampToImpalaTimestamp(ts);
	}
};

struct ParquetTimestampMSInt96Operator : public BaseParquetOperator {
	template <class SRC, class TGT>
	static TGT Operation(SRC input) {
		auto ts = Timestamp::FromEpochMsPossiblyInfinite(input);
		return TimestampToImpalaTimestamp(ts);
	}
};

struct ParquetTimestampNSInt96Operator : public BaseParquetOperator {
	template <class SRC, class TGT>
	static TGT Operation(SRC input) {
		auto ts = Timestamp::FromEpochNanoSecondsPossiblyInfinite(input);
		return TimestampToImpalaTimestamp(ts);
	}
};

// We will need a different operator for GEOGRAPHY later, so we define a base geo operator
struct ParquetBaseGeoOperator : public BaseParquetOperator {
	template <class SRC, class TGT>
	static TGT Operation(SRC input) {
		return input;
	}

	template <class SRC, class TGT>
	static void HandleStats(ColumnWriterStatistics *stats, TGT target_value) {
		auto &geo_stats = stats->Cast<GeoStatisticsState>();
		geo_stats.Update(target_value);
	}

	template <class SRC, class TGT>
	static void WriteToStream(const TGT &target_value, WriteStream &ser) {
		ser.Write<uint32_t>(target_value.GetSize());
		ser.WriteData(const_data_ptr_cast(target_value.GetData()), target_value.GetSize());
	}

	template <class SRC, class TGT>
	static idx_t WriteSize(const TGT &target_value) {
		return sizeof(uint32_t) + target_value.GetSize();
	}

	template <class SRC, class TGT>
	static uint64_t XXHash64(const TGT &target_value) {
		return duckdb_zstd::XXH64(target_value.GetData(), target_value.GetSize(), 0);
	}

	template <class SRC, class TGT>
	static idx_t GetRowSize(const Vector &vector, idx_t index) {
		// This needs to add the 4 bytes (just like WriteSize) otherwise we underestimate and we have to realloc
		// This seriously harms performance, mostly by making it very inconsistent (see internal issue #4990)
		return sizeof(uint32_t) + FlatVector::GetData<string_t>(vector)[index].GetSize();
	}
};

struct ParquetGeometryOperator : public ParquetBaseGeoOperator {
	template <class SRC, class TGT>
	static unique_ptr<ColumnWriterStatistics> InitializeStats() {
		return make_uniq<GeoStatisticsState>();
	}
};

struct ParquetBaseStringOperator : public BaseParquetOperator {
	template <class SRC, class TGT>
	static TGT Operation(SRC input) {
		return input;
	}

	template <class SRC, class TGT>
	static void HandleStats(ColumnWriterStatistics *stats, TGT target_value) {
		auto &string_stats = stats->Cast<StringStatisticsState>();
		string_stats.Update(target_value);
	}

	template <class SRC, class TGT>
	static void WriteToStream(const TGT &target_value, WriteStream &ser) {
		ser.Write<uint32_t>(target_value.GetSize());
		ser.WriteData(const_data_ptr_cast(target_value.GetData()), target_value.GetSize());
	}

	template <class SRC, class TGT>
	static idx_t WriteSize(const TGT &target_value) {
		return sizeof(uint32_t) + target_value.GetSize();
	}

	template <class SRC, class TGT>
	static uint64_t XXHash64(const TGT &target_value) {
		return duckdb_zstd::XXH64(target_value.GetData(), target_value.GetSize(), 0);
	}

	template <class SRC, class TGT>
	static idx_t GetRowSize(const Vector &vector, idx_t index) {
		// This needs to add the 4 bytes (just like WriteSize) otherwise we underestimate and we have to realloc
		// This seriously harms performance, mostly by making it very inconsistent (see internal issue #4990)
		return sizeof(uint32_t) + FlatVector::GetData<string_t>(vector)[index].GetSize();
	}
};

struct ParquetBlobOperator : public ParquetBaseStringOperator {
	template <class SRC, class TGT>
	static unique_ptr<ColumnWriterStatistics> InitializeStats() {
		return make_uniq<StringStatisticsState>(LogicalTypeId::BLOB);
	}
};

struct ParquetStringOperator : public ParquetBaseStringOperator {
	template <class SRC, class TGT>
	static unique_ptr<ColumnWriterStatistics> InitializeStats() {
		return make_uniq<StringStatisticsState>();
	}
};

struct ParquetIntervalTargetType {
	static constexpr const idx_t PARQUET_INTERVAL_SIZE = ParquetIntervalUtils::PARQUET_INTERVAL_SIZE;
	data_t bytes[PARQUET_INTERVAL_SIZE];
};

struct ParquetIntervalOperator : public BaseParquetOperator {
	template <class SRC, class TGT>
	static TGT Operation(SRC input) {
		if (input.days < 0 || input.months < 0 || input.micros < 0) {
			throw IOException("Parquet files do not support negative intervals");
		}
		TGT result;
		ParquetIntervalUtils::Encode(input, result.bytes);
		return result;
	}

	template <class SRC, class TGT>
	static void WriteToStream(const TGT &target_value, WriteStream &ser) {
		ser.WriteData(target_value.bytes, ParquetIntervalTargetType::PARQUET_INTERVAL_SIZE);
	}

	template <class SRC, class TGT>
	static constexpr idx_t WriteSize(const TGT &target_value) {
		return ParquetIntervalTargetType::PARQUET_INTERVAL_SIZE;
	}

	template <class SRC, class TGT>
	static uint64_t XXHash64(const TGT &target_value) {
		return duckdb_zstd::XXH64(target_value.bytes, ParquetIntervalTargetType::PARQUET_INTERVAL_SIZE, 0);
	}

	template <class SRC, class TGT>
	static idx_t BloomFilterEntriesPerValue() {
		return 2;
	}

	template <class SRC, class TGT>
	static optional<uint64_t> GetExtraBloomFilterHash(const SRC &source_value, const TGT &) {
		// Preserve the Parquet plain-encoding hash and add the hash used by DuckDB comparisons.
		return ParquetIntervalUtils::TryHashNormalized(source_value);
	}
};

struct ParquetUUIDOperator : public BaseParquetOperator {
	template <class SRC, class TGT>
	static TGT Operation(SRC input) {
		TGT result;
		// Use the utility function from BaseUUID
		BaseUUID::ToBlob(input, result.bytes);
		return result;
	}

	template <class SRC, class TGT>
	static void WriteToStream(const TGT &target_value, WriteStream &ser) {
		ser.WriteData(target_value.bytes, ParquetUUIDTargetType::PARQUET_UUID_SIZE);
	}

	template <class SRC, class TGT>
	static constexpr idx_t WriteSize(const TGT &target_value) {
		return ParquetUUIDTargetType::PARQUET_UUID_SIZE;
	}

	template <class SRC, class TGT>
	static uint64_t XXHash64(const TGT &target_value) {
		return duckdb_zstd::XXH64(target_value.bytes, ParquetUUIDTargetType::PARQUET_UUID_SIZE, 0);
	}

	template <class SRC, class TGT>
	static unique_ptr<ColumnWriterStatistics> InitializeStats() {
		return make_uniq<UUIDStatisticsState>();
	}

	template <class SRC, class TGT>
	static void HandleStats(ColumnWriterStatistics *stats_p, TGT target_value) {
		auto &stats = stats_p->Cast<UUIDStatisticsState>();
		if (!stats.has_stats ||
		    memcmp(target_value.bytes, stats.min.bytes, ParquetUUIDTargetType::PARQUET_UUID_SIZE) < 0) {
			stats.min = target_value;
		}
		if (!stats.has_stats ||
		    memcmp(target_value.bytes, stats.max.bytes, ParquetUUIDTargetType::PARQUET_UUID_SIZE) > 0) {
			stats.max = target_value;
		}
		stats.has_stats = true;
	}
};

struct ParquetTimeTZOperator : public BaseParquetOperator {
	template <class SRC, class TGT>
	static TGT Operation(SRC input) {
		return ParquetTimeTZToUTCMicros(input);
	}

	template <class SRC, class TGT>
	static unique_ptr<ColumnWriterStatistics> InitializeStats() {
		return make_uniq<NumericStatisticsState<TGT, TGT, BaseParquetOperator>>();
	}

	template <class SRC, class TGT>
	static void HandleStats(ColumnWriterStatistics *stats, TGT target_value) {
		auto &numeric_stats = stats->Cast<NumericStatisticsState<TGT, TGT, BaseParquetOperator>>();
		if (LessThan::Operation(target_value, numeric_stats.min)) {
			numeric_stats.min = target_value;
		}
		if (GreaterThan::Operation(target_value, numeric_stats.max)) {
			numeric_stats.max = target_value;
		}
	}
};

struct ParquetHugeintOperator : public BaseParquetOperator {
	template <class SRC, class TGT>
	static TGT Operation(SRC input) {
		return Hugeint::Cast<double>(input);
	}

	template <class SRC, class TGT>
	static unique_ptr<ColumnWriterStatistics> InitializeStats() {
		return make_uniq<ColumnWriterStatistics>();
	}

	template <class SRC, class TGT>
	static void HandleStats(ColumnWriterStatistics *stats, TGT target_value) {
	}
};

struct ParquetUhugeintOperator : public BaseParquetOperator {
	template <class SRC, class TGT>
	static TGT Operation(SRC input) {
		return Uhugeint::Cast<double>(input);
	}

	template <class SRC, class TGT>
	static unique_ptr<ColumnWriterStatistics> InitializeStats() {
		return make_uniq<ColumnWriterStatistics>();
	}

	template <class SRC, class TGT>
	static void HandleStats(ColumnWriterStatistics *stats, TGT target_value) {
	}
};

} // namespace duckdb
