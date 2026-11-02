#include "parquet_statistics.hpp"

#include <cmath>
#include <memory>
#include <utility>
#include <vector>

#include "parquet_decimal_utils.hpp"
#include "parquet_interval.hpp"
#include "parquet_timestamp.hpp"
#include "parquet_float16.hpp"
#include "reader/string_column_reader.hpp"
#include "reader/variant_column_reader.hpp"
#include "zstd/common/xxhash.hpp"
#include "duckdb/common/types/blob.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/storage/statistics/struct_stats.hpp"
#include "duckdb/storage/statistics/list_stats.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "reader/uuid_column_reader.hpp"
#include "column_reader.hpp"
#include "duckdb/common/allocator.hpp"
#include "duckdb/common/constants.hpp"
#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/hugeint.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/optional.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/datetime.hpp"
#include "duckdb/common/types/geometry.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/planner/table_filter.hpp"
#include "duckdb/storage/statistics/geometry_stats.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"
#include "duckdb/storage/statistics/string_stats.hpp"
#include "duckdb/storage/statistics/variant_stats.hpp"
#include "parquet_column_schema.hpp"
#include "parquet_types.h"
#include "thrift/protocol/TProtocol.h"
#include "thrift_tools.hpp"

namespace duckdb {

using duckdb_parquet::ConvertedType;
using duckdb_parquet::Type;

unique_ptr<BaseStatistics> ParquetStatisticsUtils::CreateNumericStats(const LogicalType &type,
                                                                      const ParquetColumnSchema &schema_ele,
                                                                      const duckdb_parquet::Statistics &parquet_stats) {
	auto stats = NumericStats::CreateUnknown(type);

	// for reasons unknown to science, Parquet defines *both* `min` and `min_value` as well as `max` and
	// `max_value`. All are optional. such elegance.
	Value min;
	Value max;
	if (parquet_stats.__isset.min_value) {
		min = ParquetStatisticsUtils::ConvertValue(type, schema_ele, parquet_stats.min_value);
	} else if (parquet_stats.__isset.min) {
		min = ParquetStatisticsUtils::ConvertValue(type, schema_ele, parquet_stats.min);
	} else {
		min = Value(type);
	}
	if (parquet_stats.__isset.max_value) {
		max = ParquetStatisticsUtils::ConvertValue(type, schema_ele, parquet_stats.max_value);
	} else if (parquet_stats.__isset.max) {
		max = ParquetStatisticsUtils::ConvertValue(type, schema_ele, parquet_stats.max);
	} else {
		max = Value(type);
	}
	NumericStats::SetMin(stats, min);
	NumericStats::SetMax(stats, max);
	return stats.ToUnique();
}

static unique_ptr<BaseStatistics> CreateFloatingPointStats(const LogicalType &type,
                                                           const ParquetColumnSchema &schema_ele,
                                                           const duckdb_parquet::Statistics &parquet_stats) {
	auto stats = NumericStats::CreateUnknown(type);

	// floating point values can always have NaN values - hence we cannot use the max value from the file
	Value min;
	Value max;
	if (parquet_stats.__isset.min_value) {
		min = ParquetStatisticsUtils::ConvertValue(type, schema_ele, parquet_stats.min_value);
	} else if (parquet_stats.__isset.min) {
		min = ParquetStatisticsUtils::ConvertValue(type, schema_ele, parquet_stats.min);
	} else {
		min = Value(type);
	}
	max = Value("nan").DefaultCastAs(type);
	NumericStats::SetMin(stats, min);
	NumericStats::SetMax(stats, max);
	return stats.ToUnique();
}

bool ParquetStatisticsUtils::CanHaveNaN(const duckdb_parquet::Statistics &parquet_stats, bool can_have_nan) {
	if (parquet_stats.__isset.nan_count) {
		return parquet_stats.nan_count != 0;
	}
	return can_have_nan;
}

Value ParquetStatisticsUtils::ConvertValue(const LogicalType &type, const ParquetColumnSchema &schema_ele,
                                           const std::string &stats) {
	string error;
	auto stats_val = ConvertValueInternal(type, schema_ele, stats);
	auto result = stats_val.DefaultTryCastAs(type, &error);
	if (!result) {
		return Value(type);
	}
	return std::move(*result);
}
//! Statistics values that do not fit in the width of the decimal are ignored
template <class T>
static Value ParquetDecimalStatsValue(T value, uint8_t width, uint8_t scale) {
	const auto max_value = Hugeint::POWERS_OF_TEN[width];
	const auto hugeint_value = hugeint_t(value);
	if (hugeint_value >= max_value || hugeint_value <= -max_value) {
		return Value();
	}
	return Value::DECIMAL(value, width, scale);
}

Value ParquetStatisticsUtils::ConvertValueInternal(const LogicalType &type, const ParquetColumnSchema &schema_ele,
                                                   const std::string &stats) {
	auto stats_data = const_data_ptr_cast(stats.c_str());
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN: {
		if (stats.size() != sizeof(bool)) {
			throw InvalidInputException("Incorrect stats size for type BOOLEAN");
		}
		return Value::BOOLEAN(Load<uint8_t>(stats_data) != 0);
	}
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
		if (stats.size() != sizeof(uint32_t)) {
			throw InvalidInputException("Incorrect stats size for type UINTEGER");
		}
		return Value::UINTEGER(Load<uint32_t>(stats_data));
	case LogicalTypeId::UBIGINT:
		if (stats.size() != sizeof(uint64_t)) {
			throw InvalidInputException("Incorrect stats size for type UBIGINT");
		}
		return Value::UBIGINT(Load<uint64_t>(stats_data));
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
		if (stats.size() != sizeof(int32_t)) {
			throw InvalidInputException("Incorrect stats size for type INTEGER");
		}
		return Value::INTEGER(Load<int32_t>(stats_data));
	case LogicalTypeId::BIGINT:
		if (stats.size() != sizeof(int64_t)) {
			throw InvalidInputException("Incorrect stats size for type BIGINT");
		}
		return Value::BIGINT(Load<int64_t>(stats_data));
	case LogicalTypeId::FLOAT: {
		float val;
		if (schema_ele.type_info == ParquetExtraTypeInfo::FLOAT16) {
			if (stats.size() != sizeof(uint16_t)) {
				throw InvalidInputException("Incorrect stats size for type FLOAT16");
			}
			val = Float16ToFloat32(Load<uint16_t>(stats_data));
		} else {
			if (stats.size() != sizeof(float)) {
				throw InvalidInputException("Incorrect stats size for type FLOAT");
			}
			val = Load<float>(stats_data);
		}
		if (!Value::FloatIsFinite(val)) {
			return Value();
		}
		return Value::FLOAT(val);
	}
	case LogicalTypeId::DOUBLE: {
		if (schema_ele.type_info == ParquetExtraTypeInfo::DECIMAL_BYTE_ARRAY) {
			// decimals cast to double
			return Value::DOUBLE(ParquetDecimalUtils::ReadDecimalValue<double>(stats_data, stats.size(), schema_ele));
		}
		if (stats.size() != sizeof(double)) {
			throw InvalidInputException("Incorrect stats size for type DOUBLE");
		}
		auto val = Load<double>(stats_data);
		if (!Value::DoubleIsFinite(val)) {
			return Value();
		}
		return Value::DOUBLE(val);
	}
	case LogicalTypeId::DECIMAL: {
		auto width = DecimalType::GetWidth(type);
		auto scale = DecimalType::GetScale(type);
		switch (schema_ele.type_info) {
		case ParquetExtraTypeInfo::DECIMAL_INT32:
			if (stats.size() != sizeof(int32_t)) {
				throw InvalidInputException("Incorrect stats size for type %s", type.ToString());
			}
			return ParquetDecimalStatsValue(Load<int32_t>(stats_data), width, scale);
		case ParquetExtraTypeInfo::DECIMAL_INT64:
			if (stats.size() != sizeof(int64_t)) {
				throw InvalidInputException("Incorrect stats size for type %s", type.ToString());
			}
			return ParquetDecimalStatsValue(Load<int64_t>(stats_data), width, scale);
		case ParquetExtraTypeInfo::DECIMAL_BYTE_ARRAY:
			switch (type.InternalType()) {
			case PhysicalType::INT16:
				return ParquetDecimalStatsValue(
				    ParquetDecimalUtils::ReadDecimalValue<int16_t>(stats_data, stats.size(), schema_ele), width, scale);
			case PhysicalType::INT32:
				return ParquetDecimalStatsValue(
				    ParquetDecimalUtils::ReadDecimalValue<int32_t>(stats_data, stats.size(), schema_ele), width, scale);
			case PhysicalType::INT64:
				return ParquetDecimalStatsValue(
				    ParquetDecimalUtils::ReadDecimalValue<int64_t>(stats_data, stats.size(), schema_ele), width, scale);
			case PhysicalType::INT128:
				return ParquetDecimalStatsValue(
				    ParquetDecimalUtils::ReadDecimalValue<hugeint_t>(stats_data, stats.size(), schema_ele), width,
				    scale);
			default:
				throw InvalidInputException("Unsupported internal type for decimal");
			}
		default:
			throw NotImplementedException("Unrecognized Parquet type for Decimal");
		}
	}
	case LogicalTypeId::VARCHAR:
	case LogicalTypeId::BLOB:
		if (type.id() == LogicalTypeId::BLOB || !Value::StringIsValid(stats)) {
			return Value(Blob::ToString(string_t(stats)));
		}
		return Value(stats);
	case LogicalTypeId::DATE:
		if (stats.size() != sizeof(int32_t)) {
			throw InvalidInputException("Incorrect stats size for type DATE");
		}
		return Value::DATE(date_t(Load<int32_t>(stats_data)));
	case LogicalTypeId::TIME: {
		int64_t val;
		if (stats.size() == sizeof(int32_t)) {
			val = Load<int32_t>(stats_data);
		} else if (stats.size() == sizeof(int64_t)) {
			val = Load<int64_t>(stats_data);
		} else {
			throw InvalidInputException("Incorrect stats size for type TIME");
		}
		switch (schema_ele.type_info) {
		case ParquetExtraTypeInfo::UNIT_MS:
			if (!ParquetTimeIsValid(val, Interval::MSECS_PER_SEC * Interval::SECS_PER_DAY)) {
				return Value();
			}
			return Value::TIME(Time::FromTimeMs(val));
		case ParquetExtraTypeInfo::UNIT_MICROS:
		default:
			if (!ParquetTimeIsValid(val, Interval::MICROS_PER_DAY)) {
				return Value();
			}
			return Value::TIME(dtime_t(val));
		}
	}
	case LogicalTypeId::TIME_NS: {
		if (stats.size() != sizeof(int64_t)) {
			throw InvalidInputException("Incorrect stats size for type TIME_NS");
		}
		if (schema_ele.type_info != ParquetExtraTypeInfo::UNIT_NS) {
			throw InternalException("TIME_NS requires nanosecond type info");
		}
		const auto nanos = Load<int64_t>(stats_data);
		if (!ParquetTimeIsValid(nanos, Interval::NANOS_PER_DAY)) {
			return Value();
		}
		return Value::TIME_NS(ParquetIntToTimeNs(nanos));
	}
	case LogicalTypeId::TIME_TZ: {
		int64_t val;
		if (stats.size() == sizeof(int32_t)) {
			val = Load<int32_t>(stats_data);
		} else if (stats.size() == sizeof(int64_t)) {
			val = Load<int64_t>(stats_data);
		} else {
			throw InvalidInputException("Incorrect stats size for type TIMETZ");
		}
		switch (schema_ele.type_info) {
		case ParquetExtraTypeInfo::UNIT_MS:
			return Value::TIMETZ(ParquetIntToTimeMsTZ(NumericCast<int32_t>(val)));
		case ParquetExtraTypeInfo::UNIT_NS:
			return Value::TIMETZ(ParquetIntToTimeNsTZ(val));
		case ParquetExtraTypeInfo::UNIT_MICROS:
		default:
			return Value::TIMETZ(ParquetIntToTimeTZ(val));
		}
	}
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ: {
		timestamp_t timestamp_value;
		if (schema_ele.type_info == ParquetExtraTypeInfo::IMPALA_TIMESTAMP) {
			if (stats.size() != sizeof(Int96)) {
				throw InvalidInputException("Incorrect stats size for type TIMESTAMP");
			}
			timestamp_value = ImpalaTimestampToTimestamp(Load<Int96>(stats_data));
		} else {
			if (stats.size() != sizeof(int64_t)) {
				throw InvalidInputException("Incorrect stats size for type TIMESTAMP");
			}
			auto val = Load<int64_t>(stats_data);
			switch (schema_ele.type_info) {
			case ParquetExtraTypeInfo::UNIT_MS:
				timestamp_value = ParquetTimestampMsToTimestamp(val);
				break;
			case ParquetExtraTypeInfo::UNIT_MICROS:
			default:
				timestamp_value = timestamp_t(val);
				break;
			}
		}
		if (type.id() == LogicalTypeId::TIMESTAMP_TZ) {
			return Value::TIMESTAMPTZ(timestamp_tz_t(timestamp_value));
		}
		return Value::TIMESTAMP(timestamp_value);
	}
	case LogicalTypeId::TIMESTAMP_TZ_NS:
	case LogicalTypeId::TIMESTAMP_NS: {
		timestamp_ns_t timestamp_value;
		if (schema_ele.type_info == ParquetExtraTypeInfo::IMPALA_TIMESTAMP) {
			if (stats.size() != sizeof(Int96)) {
				throw InvalidInputException("Incorrect stats size for type TIMESTAMP_NS");
			}
			timestamp_value = ImpalaTimestampToTimestampNS(Load<Int96>(stats_data));
		} else {
			if (stats.size() != sizeof(int64_t)) {
				throw InvalidInputException("Incorrect stats size for type TIMESTAMP_NS");
			}
			auto val = Load<int64_t>(stats_data);
			switch (schema_ele.type_info) {
			case ParquetExtraTypeInfo::UNIT_MS:
				timestamp_value = ParquetTimestampMsToTimestampNs(val);
				break;
			case ParquetExtraTypeInfo::UNIT_NS:
				timestamp_value = ParquetTimestampNsToTimestampNs(val);
				break;
			case ParquetExtraTypeInfo::UNIT_MICROS:
			default:
				timestamp_value = ParquetTimestampUsToTimestampNs(val);
				break;
			}
		}
		if (type.id() == LogicalTypeId::TIMESTAMP_TZ_NS) {
			return Value::TIMESTAMPTZNS(timestamp_tz_ns_t(timestamp_value.value));
		}
		return Value::TIMESTAMPNS(timestamp_value);
	}
	case LogicalTypeId::UUID: {
		if (stats.size() != 16) {
			throw InvalidInputException("Incorrect stats size for type UUID");
		}
		auto uuid_val = UUIDValueConversion::ReadParquetUUID(const_data_ptr_cast(stats.c_str()));
		return Value::UUID(uuid_val);
	}
	default:
		throw InternalException("Unsupported type for stats %s", type.ToString());
	}
}

bool IsVariantNull(const string &str) {
	return str.size() == 1 && str[0] == '\0';
}

// The conversion is best-effort and non-fatal: when the statistics of a particular (sub)node cannot be
// converted, that node is left as UNKNOWN stats (which makes it "not fully shredded", so consumers fall
// back to a full scan for that field) instead of discarding the statistics of the entire variant.
static void ConvertUnshreddedStats(BaseStatistics &result, optional_ptr<BaseStatistics> input_p) {
	D_ASSERT(result.GetType().id() == LogicalTypeId::UINTEGER);

	if (!input_p) {
		//! No overlay statistics -> conservatively unknown (this node is not "fully shredded")
		result.Copy(BaseStatistics::CreateUnknown(LogicalType::UINTEGER));
		return;
	}
	auto &input = *input_p;
	D_ASSERT(input.GetType().id() == LogicalTypeId::BLOB || input.GetType().id() == LogicalTypeId::VARCHAR);
	result.CopyValidity(input);

	if (!result.CanHaveNoNull()) {
		//! The overlay is entirely NULL -> no overlay values -> fully shredded
		return;
	}
	if (!StringStats::HasMinMax(input)) {
		//! The overlay may contain values but we can't tell what they are (e.g. the writer dropped min/max for
		//! a large blob value) -> conservatively unknown so this node is treated as not fully shredded
		result.Copy(BaseStatistics::CreateUnknown(LogicalType::UINTEGER));
		return;
	}

	auto min = StringStats::Min(input);
	auto max = StringStats::Max(input);
	if (IsVariantNull(min) && IsVariantNull(max)) {
		//! All non-shredded values are NULL or VARIANT_NULL, set the stats to indicate this
		NumericStats::SetMin<uint32_t>(result, 0);
		NumericStats::SetMax<uint32_t>(result, 0);
		result.SetHasNoNull();
	}
	//! else: there are real overlay values -> leave min/max unset, so this node is not fully shredded
}

static void ConvertShreddedStats(BaseStatistics &result, optional_ptr<BaseStatistics> input_p);

static void ConvertShreddedStatsItem(BaseStatistics &result, BaseStatistics &input) {
	D_ASSERT(result.GetType().id() == LogicalTypeId::STRUCT);
	D_ASSERT(input.GetType().id() == LogicalTypeId::STRUCT);

	// result variant stats
	auto &untyped_value_index_stats = StructStats::GetChildStats(result, VariantStats::UNTYPED_VALUE_INDEX);
	auto &typed_value_result = StructStats::GetChildStats(result, VariantStats::TYPED_VALUE_INDEX);

	// input parquet stats
	auto &value_stats = StructStats::GetChildStats(input, 0);
	auto &typed_value_input = StructStats::GetChildStats(input, 1);

	ConvertUnshreddedStats(untyped_value_index_stats, value_stats);
	ConvertShreddedStats(typed_value_result, typed_value_input);
}

static void ConvertShreddedStats(BaseStatistics &result, optional_ptr<BaseStatistics> input_p) {
	if (!input_p) {
		//! No statistics for this shredded subtree -> leave it unknown (conservative)
		result.Copy(BaseStatistics::CreateUnknown(result.GetType()));
		return;
	}
	auto &input = *input_p;
	result.CopyValidity(input);

	auto type_id = result.GetType().id();
	if (type_id == LogicalTypeId::LIST) {
		ConvertShreddedStatsItem(ListStats::GetChildStats(result), ListStats::GetChildStats(input));
		return;
	}
	if (StructType::IsStruct(type_id)) {
		auto field_count = StructType::GetChildCount(result.GetType());
		for (idx_t i = 0; i < field_count; i++) {
			ConvertShreddedStatsItem(StructStats::GetChildStats(result, i), StructStats::GetChildStats(input, i));
		}
		return;
	}
	//! Primitive leaf - copy the parquet stats if the types line up, otherwise leave it unknown
	if (result.GetType() == input.GetType()) {
		result.Copy(input);
	} else {
		result.Copy(BaseStatistics::CreateUnknown(result.GetType()));
	}
}

bool StringStatsAreValid(const string &stats, bool is_varchar, StringStatsType stats_type) {
	if (stats_type == StringStatsType::TRUNCATED_STATS) {
		// truncated stats can contain invalid UTF8 due to truncation - this is fine
		return true;
	}
	// for exact stats we need the stats to be valid because we might emit them
	// we could optionally convert these into truncated stats...
	// but if a file has corrupt exact string stats it's likely these are bogus, so just ignore them
	return StringColumnReader::IsValid(stats, is_varchar);
}

static bool TryGetStringStats(const string &value, bool has_value, const string &fallback, bool has_fallback,
                              bool is_varchar, StringStatsType stats_type, string &result) {
	if (has_value && StringStatsAreValid(value, is_varchar, stats_type)) {
		result = value;
		return true;
	}
	if (has_fallback && StringStatsAreValid(fallback, is_varchar, stats_type)) {
		result = fallback;
		return true;
	}
	return false;
}

unique_ptr<BaseStatistics>
ParquetStatisticsUtils::TransformParquetStatistics(const LogicalType &type, const ParquetColumnSchema &schema,
                                                   const duckdb_parquet::Statistics &parquet_stats, bool can_have_nan,
                                                   optional_ptr<const ColumnChunk> column_chunk) {
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::DATE:
	case LogicalTypeId::TIME:
	case LogicalTypeId::TIME_NS:
	case LogicalTypeId::TIME_TZ:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::TIMESTAMP_TZ_NS:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::DECIMAL:
	case LogicalTypeId::UUID:
		return CreateNumericStats(type, schema, parquet_stats);
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
		if (CanHaveNaN(parquet_stats, can_have_nan)) {
			// The column can contain NaN values - create stats without an upper max value, as NaN compares larger than
			// anything else and is not included in the Parquet min/max
			return CreateFloatingPointStats(type, schema, parquet_stats);
		} else {
			// Otherwise we use the numeric stats as usual
			return CreateNumericStats(type, schema, parquet_stats);
		}
		break;
	case LogicalTypeId::BLOB:
	case LogicalTypeId::VARCHAR: {
		const bool is_varchar = type.id() == LogicalTypeId::VARCHAR;
		auto min_stats_type = parquet_stats.__isset.is_min_value_exact && parquet_stats.is_min_value_exact
		                          ? StringStatsType::EXACT_STATS
		                          : StringStatsType::TRUNCATED_STATS;
		auto max_stats_type = parquet_stats.__isset.is_max_value_exact && parquet_stats.is_max_value_exact
		                          ? StringStatsType::EXACT_STATS
		                          : StringStatsType::TRUNCATED_STATS;
		string min_stats;
		string max_stats;
		const bool has_min_stats =
		    TryGetStringStats(parquet_stats.min_value, parquet_stats.__isset.min_value, parquet_stats.min,
		                      parquet_stats.__isset.min, is_varchar, min_stats_type, min_stats);
		const bool has_max_stats =
		    TryGetStringStats(parquet_stats.max_value, parquet_stats.__isset.max_value, parquet_stats.max,
		                      parquet_stats.__isset.max, is_varchar, max_stats_type, max_stats);
		if (has_min_stats && has_max_stats && min_stats_type == StringStatsType::EXACT_STATS &&
		    max_stats_type == StringStatsType::EXACT_STATS && min_stats == max_stats) {
			auto constant_stats = StringStats::CreateEmpty(type);
			StringStats::FromConstant(constant_stats, string_t(min_stats));
			return constant_stats.ToUnique();
		}
		auto string_stats = StringStats::CreateUnknown(type);
		if (has_min_stats) {
			StringStats::SetMin(string_stats, string_t(min_stats), min_stats_type);
		}
		if (has_max_stats) {
			StringStats::SetMax(string_stats, string_t(max_stats), max_stats_type);
		}
		return string_stats.ToUnique();
	}
	case LogicalTypeId::GEOMETRY: {
		if (!column_chunk) {
			break;
		}
		auto geo_stats = GeometryStats::CreateUnknown(type);
		if (column_chunk->meta_data.__isset.geospatial_statistics) {
			if (column_chunk->meta_data.geospatial_statistics.__isset.bbox) {
				auto &bbox = column_chunk->meta_data.geospatial_statistics.bbox;
				auto &stats_bbox = GeometryStats::GetExtent(geo_stats);

				// xmin > xmax is allowed if the geometry crosses the antimeridian,
				// but we don't handle this right now
				if (bbox.xmin <= bbox.xmax) {
					stats_bbox.x_min = bbox.xmin;
					stats_bbox.x_max = bbox.xmax;
				}

				if (bbox.ymin <= bbox.ymax) {
					stats_bbox.y_min = bbox.ymin;
					stats_bbox.y_max = bbox.ymax;
				}

				if (bbox.__isset.zmin && bbox.__isset.zmax && bbox.zmin <= bbox.zmax) {
					stats_bbox.z_min = bbox.zmin;
					stats_bbox.z_max = bbox.zmax;
				}

				if (bbox.__isset.mmin && bbox.__isset.mmax && bbox.mmin <= bbox.mmax) {
					stats_bbox.m_min = bbox.mmin;
					stats_bbox.m_max = bbox.mmax;
				}
			}
			if (column_chunk->meta_data.geospatial_statistics.__isset.geospatial_types) {
				auto &types = column_chunk->meta_data.geospatial_statistics.geospatial_types;
				auto &stats_types = GeometryStats::GetTypes(geo_stats);

				// if types are set but empty, that still means "any type" - so we leave stats_types as-is (unknown)
				// otherwise, clear and set to the actual types

				if (!types.empty()) {
					stats_types.Clear();
					for (auto &geom_type : types) {
						if (!stats_types.TryAddWKBType(geom_type)) {
							throw DataCorruptionException("Invalid geometry type in Parquet file");
						}
					}
				}
			}
		}
		return geo_stats.ToUnique();
	}
	default:
		break;
	} // end of type switch

	// no specific stats, only create unknown stats to hold validity information
	auto unknown_stats = BaseStatistics::CreateUnknown(type);
	return unknown_stats.ToUnique();
}

unique_ptr<BaseStatistics>
ParquetStatisticsUtils::TransformColumnStatistics(const ParquetColumnSchema &schema,
                                                  const duckdb_parquet::ColumnChunkList &columns, bool can_have_nan) {
	// Not supported types
	auto &type = schema.type;
	if (type.id() == LogicalTypeId::ARRAY) {
		return nullptr;
	}

	unique_ptr<BaseStatistics> row_group_stats;

	if (type.id() == LogicalTypeId::LIST || type.id() == LogicalTypeId::MAP) {
		auto list_stats = ListStats::CreateUnknown(type);
		auto &child_schema = schema.children[0];
		auto child_stats = ParquetStatisticsUtils::TransformColumnStatistics(child_schema, columns, can_have_nan);
		ListStats::SetChildStats(list_stats, std::move(child_stats));
		row_group_stats = list_stats.ToUnique();
		return row_group_stats;
	}
	// Structs are handled differently (they dont have stats)
	if (StructType::IsStruct(type)) {
		auto struct_stats = StructStats::CreateUnknown(type);
		// Recurse into child readers
		for (idx_t i = 0; i < schema.children.size(); i++) {
			auto &child_schema = schema.children[i];
			auto child_stats = ParquetStatisticsUtils::TransformColumnStatistics(child_schema, columns, can_have_nan);
			StructStats::SetChildStats(struct_stats, i, std::move(child_stats));
		}
		row_group_stats = struct_stats.ToUnique();
		return row_group_stats;
	} else if (schema.schema_type == ParquetColumnSchemaType::VARIANT) {
		auto children_count = schema.children.size();
		if (children_count != 3) {
			return nullptr;
		}
		//! Create the VARIANT stats
		auto &typed_value = schema.children[2];
		LogicalType logical_type;
		if (!VariantColumnReader::TypedValueLayoutToType(typed_value.type, logical_type)) {
			//! We couldn't convert the parquet typed_value to a structured type (likely because a nested 'typed_value'
			//! field is missing)
			return nullptr;
		}
		auto variant_stats = VariantStats::CreateShredded(VariantStats::GetShreddingType(logical_type));

		//! Take the root stats
		auto &shredded_stats = VariantStats::GetShreddedStats(variant_stats);
		auto &untyped_value_index_stats = StructStats::GetChildStats(shredded_stats, VariantStats::UNTYPED_VALUE_INDEX);
		auto &typed_value_stats = StructStats::GetChildStats(shredded_stats, VariantStats::TYPED_VALUE_INDEX);

		//! Convert the root 'value' -> 'untyped_value_index'
		auto &value = schema.children[1];
		if (schema.children[0].name != "metadata" || value.name != "value") {
			return nullptr;
		}
		auto value_stats = ParquetStatisticsUtils::TransformColumnStatistics(value, columns, can_have_nan);
		//! Best-effort: nodes whose stats can't be converted are left UNKNOWN (not fully shredded) rather
		//! than discarding the statistics for the entire variant column
		ConvertUnshreddedStats(untyped_value_index_stats, value_stats.get());

		auto parquet_typed_value_stats =
		    ParquetStatisticsUtils::TransformColumnStatistics(typed_value, columns, can_have_nan);
		ConvertShreddedStats(typed_value_stats, parquet_typed_value_stats.get());
		//! Set validity to UNKNOWN
		variant_stats.SetHasNoNull();
		variant_stats.SetHasNull();
		return variant_stats.ToUnique();
	}

	// Otherwise, its a standard column with stats
	auto &column_chunk = columns[schema.column_index];
	if (!column_chunk.__isset.meta_data || !column_chunk.meta_data.__isset.statistics) {
		// no stats present for row group
		return nullptr;
	}
	auto &parquet_stats = column_chunk.meta_data.statistics;
	row_group_stats = TransformParquetStatistics(type, schema, parquet_stats, can_have_nan, &column_chunk);

	// null count is generic
	if (row_group_stats) {
		row_group_stats->Set(StatsInfo::CAN_HAVE_NULL_AND_VALID_VALUES);
		if (parquet_stats.__isset.null_count && parquet_stats.null_count == 0) {
			row_group_stats->Set(StatsInfo::CANNOT_HAVE_NULL_VALUES);
		}
		if (parquet_stats.__isset.null_count && parquet_stats.null_count == column_chunk.meta_data.num_values) {
			row_group_stats->Set(StatsInfo::CANNOT_HAVE_VALID_VALUES);
		}
	}
	return row_group_stats;
}

static bool UsesNormalizedIntervalHash(ParquetBloomFilterHashStrategy hash_strategy) {
	return hash_strategy == ParquetBloomFilterHashStrategy::NORMALIZED_INTERVAL_V1;
}

static bool IsBloomFilterColumn(const Expression &expr, ParquetBloomFilterHashStrategy hash_strategy) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_REF) {
		return expr.Cast<BoundReferenceExpression>().Index() == 0;
	}
	if (!UsesNormalizedIntervalHash(hash_strategy) || expr.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return false;
	}
	auto &function = expr.Cast<BoundFunctionExpression>();
	if (function.Function().GetName() != "normalized_interval" || function.GetChildren().size() != 1) {
		return false;
	}
	auto &child = *function.GetChildren()[0];
	return child.GetExpressionClass() == ExpressionClass::BOUND_REF &&
	       child.Cast<BoundReferenceExpression>().Index() == 0;
}

// Bloom filters can only probe IN lists over this column and constants of the same type.
static optional_ptr<const BoundOperatorExpression>
GetBloomFilterInExpression(const Expression &expr, ParquetBloomFilterHashStrategy hash_strategy) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_OPERATOR ||
	    expr.GetExpressionType() != ExpressionType::COMPARE_IN) {
		return nullptr;
	}
	auto &in_expr = expr.Cast<BoundOperatorExpression>();
	auto &children = in_expr.GetChildren();
	if (children.size() <= 1 || !IsBloomFilterColumn(*children[0], hash_strategy)) {
		return nullptr;
	}
	auto &column = *children[0];
	for (idx_t child_idx = 1; child_idx < children.size(); ++child_idx) {
		if (children[child_idx]->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
			return nullptr;
		}
		auto &constant = children[child_idx]->Cast<BoundConstantExpression>().GetValue();
		if (!constant.IsNull() && column.GetReturnType() != constant.type()) {
			return nullptr;
		}
	}
	return in_expr;
}

static optional_ptr<const BoundConstantExpression>
GetBloomFilterConstant(const Expression &expr, ParquetBloomFilterHashStrategy hash_strategy) {
	if (!BoundComparisonExpression::IsComparison(expr)) {
		return nullptr;
	}
	auto &comp = expr.Cast<BoundFunctionExpression>();
	if (comp.GetExpressionType() != ExpressionType::COMPARE_EQUAL &&
	    comp.GetExpressionType() != ExpressionType::COMPARE_NOT_DISTINCT_FROM) {
		return nullptr;
	}
	auto &left = BoundComparisonExpression::Left(comp);
	auto &right = BoundComparisonExpression::Right(comp);
	optional_ptr<const Expression> column;
	optional_ptr<const BoundConstantExpression> constant;
	if (IsBloomFilterColumn(left, hash_strategy) && right.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
		column = left;
		constant = right.Cast<BoundConstantExpression>();
	} else if (IsBloomFilterColumn(right, hash_strategy) &&
	           left.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
		column = right;
		constant = left.Cast<BoundConstantExpression>();
	} else {
		return nullptr;
	}
	if (constant->GetValue().IsNull() || column->GetReturnType() != constant->GetValue().type()) {
		return nullptr;
	}
	return constant;
}

static bool HasFilterConstants(const Expression &expr, ParquetBloomFilterHashStrategy hash_strategy) {
	if (BoundComparisonExpression::IsComparison(expr)) {
		return GetBloomFilterConstant(expr, hash_strategy) != nullptr;
	}
	if (GetBloomFilterInExpression(expr, hash_strategy)) {
		return true;
	}
	auto optional_filter_child = ExpressionFilter::GetOptionalFilterChild(expr);
	if (optional_filter_child) {
		return HasFilterConstants(*optional_filter_child, hash_strategy);
	}
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_CONJUNCTION) {
		return false;
	}
	bool child_has_constant = false;
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) {
		if (!child_has_constant) {
			child_has_constant = HasFilterConstants(child, hash_strategy);
		}
	});
	return child_has_constant;
}

static bool HasFilterConstants(const TableFilter &duckdb_filter, ParquetBloomFilterHashStrategy hash_strategy) {
	auto &expr_filter = ExpressionFilter::GetExpressionFilter(duckdb_filter, "ParquetStatistics::HasFilterConstants");
	return HasFilterConstants(*expr_filter.expr, hash_strategy);
}

template <class T>
static uint64_t ValueXH64FixedWidth(const Value &constant) {
	T val;
	if (constant.type().id() == LogicalTypeId::DECIMAL) {
		val = Hugeint::Cast<T>(IntegralValue::Get(constant));
	} else {
		val = constant.GetValue<T>();
	}
	return duckdb_zstd::XXH64(&val, sizeof(val), 0);
}

template <class T>
static uint64_t ValueXH64FixedWidth(T val) {
	return duckdb_zstd::XXH64(&val, sizeof(val), 0);
}

static optional<uint64_t> TryHashTime(const Value &constant, const ParquetColumnSchema &schema) {
	switch (constant.type().id()) {
	case LogicalTypeId::TIME: {
		auto value = constant.GetValue<dtime_t>().value;
		switch (schema.type_info) {
		case ParquetExtraTypeInfo::UNIT_MS:
			return ValueXH64FixedWidth(NumericCast<int32_t>(value / Interval::MICROS_PER_MSEC));
		case ParquetExtraTypeInfo::UNIT_MICROS:
			return ValueXH64FixedWidth(value);
		case ParquetExtraTypeInfo::UNIT_NS:
			// TIME loses sub-microsecond precision, so the Parquet hash cannot be recovered.
			return nullopt;
		default:
			return nullopt;
		}
	}
	case LogicalTypeId::TIME_NS: {
		auto value = constant.GetValue<dtime_ns_t>().value;
		switch (schema.type_info) {
		case ParquetExtraTypeInfo::UNIT_MS:
			return ValueXH64FixedWidth(NumericCast<int32_t>(value / Interval::NANOS_PER_MSEC));
		case ParquetExtraTypeInfo::UNIT_MICROS:
			return ValueXH64FixedWidth(value / Interval::NANOS_PER_MICRO);
		case ParquetExtraTypeInfo::UNIT_NS:
			return ValueXH64FixedWidth(value);
		default:
			return nullopt;
		}
	}
	case LogicalTypeId::TIME_TZ: {
		auto value = Time::NormalizeTimeTZ(constant.GetValue<dtime_tz_t>()).value;
		switch (schema.type_info) {
		case ParquetExtraTypeInfo::UNIT_MS:
			return ValueXH64FixedWidth(NumericCast<int32_t>(value / Interval::MICROS_PER_MSEC));
		case ParquetExtraTypeInfo::UNIT_MICROS:
			return ValueXH64FixedWidth(value);
		case ParquetExtraTypeInfo::UNIT_NS:
			// TIME_TZ loses sub-microsecond precision, so the Parquet hash cannot be recovered.
			return nullopt;
		default:
			return nullopt;
		}
	}
	default:
		return nullopt;
	}
}

// Recreate the Parquet millisecond hash input from DuckDB's microsecond timestamp representation.
static optional<uint64_t> TryHashTimestamp(const Value &constant, const ParquetColumnSchema &schema) {
	D_ASSERT(constant.type().id() == LogicalTypeId::TIMESTAMP || constant.type().id() == LogicalTypeId::TIMESTAMP_TZ);
	auto value = constant.GetValue<int64_t>();
	switch (schema.type_info) {
	case ParquetExtraTypeInfo::UNIT_MS:
		// DuckDB stores timestamps in microseconds, while the Parquet bloom filter hashes the stored milliseconds.
		if (timestamp_t {value}.IsFinite()) {
			value /= Interval::MICROS_PER_MSEC;
		}
		return ValueXH64FixedWidth(value);
	case ParquetExtraTypeInfo::UNIT_MICROS:
		return ValueXH64FixedWidth(value);
	default:
		return nullopt;
	}
}

// TODO we can only this if the parquet representation of the type exactly matches the duckdb rep!
// TODO TEST THIS!
// TODO perhaps we can re-use some writer infra here
static optional<uint64_t> ValueXXH64(const Value &constant, const ParquetColumnSchema &schema,
                                     ParquetBloomFilterHashStrategy hash_strategy) {
	// Handle logical types whose Parquet representation needs special hashing.
	switch (constant.type().id()) {
	case LogicalTypeId::UUID: {
		data_t bytes[16];
		BaseUUID::ToBlob(constant.GetValue<hugeint_t>(), bytes);
		return duckdb_zstd::XXH64(bytes, sizeof(bytes), 0);
	}
	case LogicalTypeId::TIME:
	case LogicalTypeId::TIME_NS:
	case LogicalTypeId::TIME_TZ:
		return TryHashTime(constant, schema);
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ:
		return TryHashTimestamp(constant, schema);
	case LogicalTypeId::INTERVAL: {
		if (!UsesNormalizedIntervalHash(hash_strategy)) {
			return nullopt;
		}
		const auto interval = constant.GetValue<interval_t>();
		// files of different versions hash an interval whose normalization saturates and keeps whole days in its
		// microseconds differently, so a filter on one cannot rely on the bloom filter
		if (interval.Normalize().micros >= Interval::MICROS_PER_DAY) {
			return nullopt;
		}
		return ParquetIntervalUtils::TryHashNormalized(interval);
	}
	case LogicalTypeId::DECIMAL:
		if (schema.parquet_type == duckdb_parquet::Type::INT32) {
			return ValueXH64FixedWidth<int32_t>(constant);
		} else if (schema.parquet_type == duckdb_parquet::Type::INT64) {
			return ValueXH64FixedWidth<int64_t>(constant);
		}
		// We do not support bloom filter hashing for FLBA/BYTE_ARRAY decimals yet.
		// Returning nullopt safely disables pruning for this column without throwing an error,
		// which is necessary for reading Parquet files generated by other systems.
		return nullopt;
	default:
		break;
	}

	switch (constant.type().InternalType()) {
	case PhysicalType::UINT8:
		return ValueXH64FixedWidth<int32_t>(constant);
	case PhysicalType::INT8:
		return ValueXH64FixedWidth<int32_t>(constant);
	case PhysicalType::UINT16:
		return ValueXH64FixedWidth<int32_t>(constant);
	case PhysicalType::INT16:
		return ValueXH64FixedWidth<int32_t>(constant);
	case PhysicalType::UINT32:
		return ValueXH64FixedWidth<uint32_t>(constant);
	case PhysicalType::INT32:
		return ValueXH64FixedWidth<int32_t>(constant);
	case PhysicalType::UINT64:
		return ValueXH64FixedWidth<uint64_t>(constant);
	case PhysicalType::INT64:
		return ValueXH64FixedWidth<int64_t>(constant);
	case PhysicalType::INT128:
		// We do not support bloom filter hashing for hugeints/large decimals yet.
		// Returning nullopt safely disables pruning. Throwing an exception would crash valid reads.
		return nullopt;
	case PhysicalType::FLOAT:
		return ValueXH64FixedWidth<float>(constant);
	case PhysicalType::DOUBLE:
		return ValueXH64FixedWidth<double>(constant);
	case PhysicalType::VARCHAR: {
		auto &val = StringValue::Get(constant);
		return duckdb_zstd::XXH64(val.data(), val.size(), 0);
	}
	default:
		return nullopt;
	}
}

static bool BloomFilterExcludes(const Value &constant, ParquetBloomFilter &bloom_filter,
                                const ParquetColumnSchema &schema, ParquetBloomFilterHashStrategy hash_strategy) {
	// Floating-point equality treats positive and negative zero as equal, but Parquet hashes their bit patterns.
	// Likewise, all NaN payloads compare equal, so no single hash can rule out a row group containing one.
	switch (constant.type().InternalType()) {
	case PhysicalType::FLOAT: {
		auto float_value = constant.GetValue<float>();
		if (Value::IsNan(float_value)) {
			return false;
		}
		if (float_value == 0.0f) {
			return !bloom_filter.FilterCheck(ValueXH64FixedWidth(0.0f)) &&
			       !bloom_filter.FilterCheck(ValueXH64FixedWidth(-0.0f));
		}
		break;
	}
	case PhysicalType::DOUBLE: {
		auto double_value = constant.GetValue<double>();
		if (Value::IsNan(double_value)) {
			return false;
		}
		if (double_value == 0.0) {
			return !bloom_filter.FilterCheck(ValueXH64FixedWidth(0.0)) &&
			       !bloom_filter.FilterCheck(ValueXH64FixedWidth(-0.0));
		}
		break;
	}
	default:
		break;
	}

	auto hash = ValueXXH64(constant, schema, hash_strategy);
	return hash && !bloom_filter.FilterCheck(*hash);
}

static bool ApplyBloomFilter(const Expression &expr, ParquetBloomFilter &bloom_filter,
                             const ParquetColumnSchema &schema, ParquetBloomFilterHashStrategy hash_strategy) {
	if (BoundComparisonExpression::IsComparison(expr)) {
		auto constant = GetBloomFilterConstant(expr, hash_strategy);
		if (!constant) {
			return false;
		}
		return BloomFilterExcludes(constant->GetValue(), bloom_filter, schema, hash_strategy);
	}
	auto in_expr = GetBloomFilterInExpression(expr, hash_strategy);
	if (in_expr) {
		auto &children = in_expr->GetChildren();
		// An IN filter is excluded only when every candidate is absent.
		for (idx_t child_idx = 1; child_idx < children.size(); ++child_idx) {
			auto &constant = children[child_idx]->Cast<BoundConstantExpression>().GetValue();
			if (constant.IsNull()) {
				continue;
			}
			if (!BloomFilterExcludes(constant, bloom_filter, schema, hash_strategy)) {
				return false;
			}
		}
		return true;
	}
	auto optional_filter_child = ExpressionFilter::GetOptionalFilterChild(expr);
	if (optional_filter_child) {
		return ApplyBloomFilter(*optional_filter_child, bloom_filter, schema, hash_strategy);
	}
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_CONJUNCTION) {
		return false;
	}
	switch (expr.GetExpressionType()) {
	case ExpressionType::CONJUNCTION_AND: {
		bool any_children_true = false;
		ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) {
			any_children_true |= ApplyBloomFilter(child, bloom_filter, schema, hash_strategy);
		});
		return any_children_true;
	}
	case ExpressionType::CONJUNCTION_OR: {
		bool all_children_true = true;
		ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) {
			all_children_true &= ApplyBloomFilter(child, bloom_filter, schema, hash_strategy);
		});
		return all_children_true;
	}
	default:
		return false;
	}
}

static bool ApplyBloomFilter(const TableFilter &duckdb_filter, ParquetBloomFilter &bloom_filter,
                             const ParquetColumnSchema &schema, ParquetBloomFilterHashStrategy hash_strategy) {
	auto &expr_filter = ExpressionFilter::GetExpressionFilter(duckdb_filter, "ParquetStatistics::ApplyBloomFilter");
	return ApplyBloomFilter(*expr_filter.expr, bloom_filter, schema, hash_strategy);
}

ParquetIntervalBloomFilterVersion
ParquetStatisticsUtils::GetIntervalBloomFilterVersion(const duckdb_parquet::FileMetaData &file_meta_data) {
	if (!file_meta_data.__isset.key_value_metadata) {
		return ParquetIntervalBloomFilterVersion::NONE;
	}
	for (const auto &kv : file_meta_data.key_value_metadata) {
		if (kv.key == INTERVAL_BLOOM_FILTER_KEY && kv.__isset.value && kv.value == INTERVAL_BLOOM_FILTER_VALUE) {
			return ParquetIntervalBloomFilterVersion::NORMALIZED_V1;
		}
	}
	return ParquetIntervalBloomFilterVersion::NONE;
}

optional<ParquetBloomFilterHashStrategy>
ParquetStatisticsUtils::GetBloomFilterHashStrategy(const ParquetColumnSchema &schema,
                                                   ParquetIntervalBloomFilterVersion interval_bloom_filter_version) {
	switch (schema.type.id()) {
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::VARCHAR:
	case LogicalTypeId::BLOB:
	case LogicalTypeId::DATE:
		return ParquetBloomFilterHashStrategy::STANDARD;
	case LogicalTypeId::INTERVAL:
		if (interval_bloom_filter_version == ParquetIntervalBloomFilterVersion::NORMALIZED_V1 &&
		    schema.parquet_type == duckdb_parquet::Type::FIXED_LEN_BYTE_ARRAY && schema.type_length == 12) {
			return ParquetBloomFilterHashStrategy::NORMALIZED_INTERVAL_V1;
		}
		return nullopt;
	case LogicalTypeId::UUID:
		if (schema.parquet_type == duckdb_parquet::Type::FIXED_LEN_BYTE_ARRAY && schema.type_length == 16) {
			return ParquetBloomFilterHashStrategy::STANDARD;
		}
		return nullopt;
	case LogicalTypeId::DECIMAL:
		// We currently only support decimal bloom filters backed by 32-bit or 64-bit integers.
		if (schema.parquet_type == duckdb_parquet::Type::INT32 || schema.parquet_type == duckdb_parquet::Type::INT64) {
			return ParquetBloomFilterHashStrategy::STANDARD;
		}
		return nullopt;
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ:
		// When type info is UNIT_NS, DuckDB type TIMESTAMP_NS/TIMESTAMP_TZ_NS is used.
		if (schema.parquet_type == duckdb_parquet::Type::INT64 &&
		    (schema.type_info == ParquetExtraTypeInfo::UNIT_MS ||
		     schema.type_info == ParquetExtraTypeInfo::UNIT_MICROS)) {
			return ParquetBloomFilterHashStrategy::STANDARD;
		}
		return nullopt;
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_TZ_NS:
		if (schema.parquet_type == duckdb_parquet::Type::INT64 && schema.type_info == ParquetExtraTypeInfo::UNIT_NS) {
			return ParquetBloomFilterHashStrategy::STANDARD;
		}
		return nullopt;
	case LogicalTypeId::TIME:
	case LogicalTypeId::TIME_TZ: {
		// Nanosecond values lose sub-microsecond precision when read as TIME or TIME_TZ.
		if (schema.parquet_type == duckdb_parquet::Type::INT32 && schema.type_info == ParquetExtraTypeInfo::UNIT_MS) {
			return ParquetBloomFilterHashStrategy::STANDARD;
		}
		if (schema.parquet_type == duckdb_parquet::Type::INT64 &&
		    schema.type_info == ParquetExtraTypeInfo::UNIT_MICROS) {
			return ParquetBloomFilterHashStrategy::STANDARD;
		}
		return nullopt;
	}
	case LogicalTypeId::TIME_NS: {
		if (schema.parquet_type == duckdb_parquet::Type::INT32 && schema.type_info == ParquetExtraTypeInfo::UNIT_MS) {
			return ParquetBloomFilterHashStrategy::STANDARD;
		}
		if (schema.parquet_type == duckdb_parquet::Type::INT64 &&
		    schema.type_info == ParquetExtraTypeInfo::UNIT_MICROS) {
			return ParquetBloomFilterHashStrategy::STANDARD;
		}
		if (schema.parquet_type == duckdb_parquet::Type::INT64 && schema.type_info == ParquetExtraTypeInfo::UNIT_NS) {
			return ParquetBloomFilterHashStrategy::STANDARD;
		}
		return nullopt;
	}
	default:
		return nullopt;
	}
}

bool ParquetStatisticsUtils::BloomFilterExcludes(const TableFilter &duckdb_filter,
                                                 const duckdb_parquet::ColumnMetaData &column_meta_data,
                                                 TProtocol &file_proto, Allocator &allocator,
                                                 const ParquetColumnSchema &schema,
                                                 ParquetBloomFilterHashStrategy hash_strategy) {
	if (!HasFilterConstants(duckdb_filter, hash_strategy) || !column_meta_data.__isset.bloom_filter_offset ||
	    column_meta_data.bloom_filter_offset <= 0) {
		return false;
	}

	auto &transport = reinterpret_cast<ThriftFileTransport &>(*file_proto.getTransport());
	auto bloom_filter_start = UnsafeNumericCast<idx_t>(column_meta_data.bloom_filter_offset);
	if (bloom_filter_start >= transport.GetSize()) {
		return false;
	}
	idx_t bloom_filter_length = 0;
	if (column_meta_data.__isset.bloom_filter_length) {
		if (column_meta_data.bloom_filter_length <= 0) {
			return false;
		}
		bloom_filter_length = UnsafeNumericCast<idx_t>(column_meta_data.bloom_filter_length);
		if (bloom_filter_length > transport.GetSize() ||
		    bloom_filter_start > transport.GetSize() - bloom_filter_length) {
			return false;
		}
	}

	transport.SetLocation(bloom_filter_start);
	if (bloom_filter_length > 0) {
		transport.Prefetch(bloom_filter_start, bloom_filter_length);
	}

	duckdb_parquet::BloomFilterHeader filter_header;
	// TODO the bloom filter could be encrypted, too, so need to double check that this is NOT the case
	filter_header.read(&file_proto);
	if (!filter_header.algorithm.__isset.BLOCK || !filter_header.compression.__isset.UNCOMPRESSED ||
	    !filter_header.hash.__isset.XXHASH) {
		return false;
	}
	if (filter_header.numBytes <= 0) {
		return false;
	}
	auto bloom_filter_data_start = transport.GetLocation();
	auto bloom_filter_data_size = UnsafeNumericCast<idx_t>(filter_header.numBytes);
	if (bloom_filter_data_size % sizeof(ParquetBloomBlock) != 0) {
		return false;
	}
	if (bloom_filter_data_start < bloom_filter_start || bloom_filter_data_size > transport.GetSize() ||
	    bloom_filter_data_start > transport.GetSize() - bloom_filter_data_size) {
		return false;
	}
	if (bloom_filter_length > 0) {
		auto bloom_filter_header_size = bloom_filter_data_start - bloom_filter_start;
		if (bloom_filter_header_size > bloom_filter_length ||
		    bloom_filter_data_size != bloom_filter_length - bloom_filter_header_size) {
			return false;
		}
	}

	auto new_buffer = make_uniq<ResizeableBuffer>(allocator, bloom_filter_data_size);
	transport.read(new_buffer->ptr, UnsafeNumericCast<uint32_t>(bloom_filter_data_size));
	ParquetBloomFilter bloom_filter(std::move(new_buffer));
	return ApplyBloomFilter(duckdb_filter, bloom_filter, schema, hash_strategy);
}

ParquetBloomFilter::ParquetBloomFilter(idx_t num_entries, double bloom_filter_false_positive_ratio) {
	// aim for hit ratio of 0.01%
	// see http://tfk.mit.edu/pdf/bloom.pdf
	double f = bloom_filter_false_positive_ratio;
	double k = 8.0;
	double n = LossyNumericCast<double>(num_entries);
	double m = -k * n / std::log(1 - std::pow(f, 1 / k));
	auto b = MaxValue<idx_t>(NextPowerOfTwo(LossyNumericCast<idx_t>(m / k)) / 32, 1);

	D_ASSERT(b > 0 && IsPowerOfTwo(b));

	data = make_uniq<ResizeableBuffer>(Allocator::DefaultAllocator(), sizeof(ParquetBloomBlock) * b);
	data->zero();
	block_count = data->len / sizeof(ParquetBloomBlock);
	D_ASSERT(data->len % sizeof(ParquetBloomBlock) == 0);
}

ParquetBloomFilter::ParquetBloomFilter(unique_ptr<ResizeableBuffer> data_p) {
	D_ASSERT(data_p->len % sizeof(ParquetBloomBlock) == 0);
	data = std::move(data_p);
	block_count = data->len / sizeof(ParquetBloomBlock);
	D_ASSERT(data->len % sizeof(ParquetBloomBlock) == 0);
}

void ParquetBloomFilter::FilterInsert(uint64_t x) {
	auto blocks = reinterpret_cast<ParquetBloomBlock *>(data->ptr);
	uint64_t i = ((x >> 32) * block_count) >> 32;
	auto &b = blocks[i];
	ParquetBloomBlock::BlockInsert(b, x);
}

bool ParquetBloomFilter::FilterCheck(uint64_t x) {
	auto blocks = reinterpret_cast<ParquetBloomBlock *>(data->ptr);
	auto i = ((x >> 32) * block_count) >> 32;
	return ParquetBloomBlock::BlockCheck(blocks[i], x);
}

// compiler optimizes this into a single instruction (popcnt)
static uint8_t PopCnt64(uint64_t n) {
	uint8_t c = 0;
	for (; n; ++c) {
		n &= n - 1;
	}
	return c;
}

double ParquetBloomFilter::OneRatio() {
	auto bloom_ptr = reinterpret_cast<uint64_t *>(data->ptr);
	idx_t one_count = 0;
	for (idx_t b_idx = 0; b_idx < data->len / sizeof(uint64_t); ++b_idx) {
		one_count += PopCnt64(bloom_ptr[b_idx]);
	}
	return LossyNumericCast<double>(one_count) / (LossyNumericCast<double>(data->len) * 8.0);
}

ResizeableBuffer *ParquetBloomFilter::Get() {
	return data.get();
}

} // namespace duckdb
