#include "parquet_statistics.hpp"

#include "duckdb.hpp"
#include "parquet_decimal_utils.hpp"
#include "parquet_timestamp.hpp"
#include "parquet_float16.hpp"
#include "parquet_reader.hpp"
#include "reader/string_column_reader.hpp"
#include "reader/struct_column_reader.hpp"
#include "reader/variant_column_reader.hpp"
#include "zstd/common/xxhash.hpp"
#include "duckdb/common/types/blob.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/storage/statistics/struct_stats.hpp"
#include "duckdb/storage/statistics/list_stats.hpp"
#include "duckdb/planner/filter/constant_filter.hpp"
#include "reader/uuid_column_reader.hpp"
#include "duckdb/common/type_visitor.hpp"

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

Value ParquetStatisticsUtils::ConvertValue(const LogicalType &type, const ParquetColumnSchema &schema_ele,
                                           const std::string &stats) {
	Value result;
	string error;
	auto stats_val = ConvertValueInternal(type, schema_ele, stats);
	if (!stats_val.DefaultTryCastAs(type, result, &error)) {
		return Value(type);
	}
	return result;
}
Value ParquetStatisticsUtils::ConvertValueInternal(const LogicalType &type, const ParquetColumnSchema &schema_ele,
                                                   const std::string &stats) {
	auto stats_data = const_data_ptr_cast(stats.c_str());
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN: {
		if (stats.size() != sizeof(bool)) {
			throw InvalidInputException("Incorrect stats size for type BOOLEAN");
		}
		return Value::BOOLEAN(Load<bool>(stats_data));
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
			return Value::DECIMAL(Load<int32_t>(stats_data), width, scale);
		case ParquetExtraTypeInfo::DECIMAL_INT64:
			if (stats.size() != sizeof(int64_t)) {
				throw InvalidInputException("Incorrect stats size for type %s", type.ToString());
			}
			return Value::DECIMAL(Load<int64_t>(stats_data), width, scale);
		case ParquetExtraTypeInfo::DECIMAL_BYTE_ARRAY:
			switch (type.InternalType()) {
			case PhysicalType::INT16:
				return Value::DECIMAL(
				    ParquetDecimalUtils::ReadDecimalValue<int16_t>(stats_data, stats.size(), schema_ele), width, scale);
			case PhysicalType::INT32:
				return Value::DECIMAL(
				    ParquetDecimalUtils::ReadDecimalValue<int32_t>(stats_data, stats.size(), schema_ele), width, scale);
			case PhysicalType::INT64:
				return Value::DECIMAL(
				    ParquetDecimalUtils::ReadDecimalValue<int64_t>(stats_data, stats.size(), schema_ele), width, scale);
			case PhysicalType::INT128:
				return Value::DECIMAL(
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
			return Value::TIME(Time::FromTimeMs(val));
		case ParquetExtraTypeInfo::UNIT_NS:
			return Value::TIME(Time::FromTimeNs(val));
		case ParquetExtraTypeInfo::UNIT_MICROS:
		default:
			return Value::TIME(dtime_t(val));
		}
	}
	case LogicalTypeId::TIME_NS: {
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
			return Value::TIME_NS(ParquetMsIntToTimeNs(val));
		case ParquetExtraTypeInfo::UNIT_NS:
			return Value::TIME_NS(ParquetIntToTimeNs(val));
		case ParquetExtraTypeInfo::UNIT_MICROS:
		default:
			return Value::TIME_NS(dtime_ns_t(val));
		}
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
			case ParquetExtraTypeInfo::UNIT_NS:
				timestamp_value = ParquetTimestampNsToTimestamp(val);
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

static bool ConvertUnshreddedStats(BaseStatistics &result, optional_ptr<BaseStatistics> input_p) {
	D_ASSERT(result.GetType().id() == LogicalTypeId::UINTEGER);

	if (!input_p) {
		return false;
	}
	auto &input = *input_p;
	D_ASSERT(input.GetType().id() == LogicalTypeId::BLOB);
	result.CopyValidity(input);

	auto min = StringStats::Min(input);
	auto max = StringStats::Max(input);

	if (!result.CanHaveNoNull()) {
		return true;
	}

	if (min.empty() && max.empty()) {
		//! All non-shredded values are NULL or VARIANT_NULL, set the stats to indicate this
		NumericStats::SetMin<uint32_t>(result, 0);
		NumericStats::SetMax<uint32_t>(result, 0);
		result.SetHasNoNull();
	}
	return true;
}

static bool ConvertShreddedStats(BaseStatistics &result, optional_ptr<BaseStatistics> input_p);

static bool ConvertShreddedStatsItem(BaseStatistics &result, BaseStatistics &input) {
	D_ASSERT(result.GetType().id() == LogicalTypeId::STRUCT);
	D_ASSERT(input.GetType().id() == LogicalTypeId::STRUCT);

	// result variant stats
	auto &untyped_value_index_stats = StructStats::GetChildStats(result, VariantStats::UNTYPED_VALUE_INDEX);
	auto &typed_value_result = StructStats::GetChildStats(result, VariantStats::TYPED_VALUE_INDEX);

	// input parquet stats
	auto &value_stats = StructStats::GetChildStats(input, 0);
	auto &typed_value_input = StructStats::GetChildStats(input, 1);

	if (!ConvertUnshreddedStats(untyped_value_index_stats, value_stats)) {
		return false;
	}
	if (!ConvertShreddedStats(typed_value_result, typed_value_input)) {
		return false;
	}
	return true;
}

static bool ConvertShreddedStats(BaseStatistics &result, optional_ptr<BaseStatistics> input_p) {
	if (!input_p) {
		return false;
	}
	auto &input = *input_p;
	result.CopyValidity(input);

	auto type_id = result.GetType().id();
	if (type_id == LogicalTypeId::LIST) {
		auto &child_result = ListStats::GetChildStats(result);
		auto &child_input = ListStats::GetChildStats(input);
		return ConvertShreddedStatsItem(child_result, child_input);
	}
	if (type_id == LogicalTypeId::STRUCT) {
		auto field_count = StructType::GetChildCount(result.GetType());
		for (idx_t i = 0; i < field_count; i++) {
			auto &result_field = StructStats::GetChildStats(result, i);
			auto &input_field = StructStats::GetChildStats(input, i);
			if (!ConvertShreddedStatsItem(result_field, input_field)) {
				return false;
			}
		}
		return true;
	}
	result.Copy(input);
	return true;
}

unique_ptr<BaseStatistics> ParquetStatisticsUtils::TransformColumnStatistics(const ParquetColumnSchema &schema,
                                                                             const vector<ColumnChunk> &columns,
                                                                             bool can_have_nan) {
	// Not supported types
	auto &type = schema.type;
	if (type.id() == LogicalTypeId::ARRAY || type.id() == LogicalTypeId::MAP) {
		return nullptr;
	}

	unique_ptr<BaseStatistics> row_group_stats;

	if (type.id() == LogicalTypeId::LIST) {
		auto list_stats = ListStats::CreateUnknown(type);
		auto &child_schema = schema.children[0];
		auto child_stats = ParquetStatisticsUtils::TransformColumnStatistics(child_schema, columns, can_have_nan);
		ListStats::SetChildStats(list_stats, std::move(child_stats));
		row_group_stats = list_stats.ToUnique();
		return row_group_stats;
	}
	// Structs are handled differently (they dont have stats)
	if (type.id() == LogicalTypeId::STRUCT) {
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
		auto shredding_type = TypeVisitor::VisitReplace(logical_type, [](const LogicalType &type) {
			return LogicalType::STRUCT({{"typed_value", type}, {"untyped_value_index", LogicalType::UINTEGER}});
		});
		auto variant_stats = VariantStats::CreateShredded(shredding_type);

		//! Take the root stats
		auto &shredded_stats = VariantStats::GetShreddedStats(variant_stats);
		auto &untyped_value_index_stats = StructStats::GetChildStats(shredded_stats, VariantStats::UNTYPED_VALUE_INDEX);
		auto &typed_value_stats = StructStats::GetChildStats(shredded_stats, VariantStats::TYPED_VALUE_INDEX);

		//! Convert the root 'value' -> 'untyped_value_index'
		auto &value = schema.children[1];
		D_ASSERT(value.name == "value");
		auto value_stats = ParquetStatisticsUtils::TransformColumnStatistics(value, columns, can_have_nan);
		if (!ConvertUnshreddedStats(untyped_value_index_stats, value_stats.get())) {
			//! Couldn't convert the stats, or there are no stats
			return nullptr;
		}

		auto parquet_typed_value_stats =
		    ParquetStatisticsUtils::TransformColumnStatistics(typed_value, columns, can_have_nan);
		if (!ConvertShreddedStats(typed_value_stats, parquet_typed_value_stats.get())) {
			//! Couldn't convert the stats, or there are no stats
			return nullptr;
		}
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

	switch (type.id()) {
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
	case LogicalTypeId::TIME_TZ:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::DECIMAL:
		row_group_stats = CreateNumericStats(type, schema, parquet_stats);
		break;
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
		if (can_have_nan) {
			// Since parquet doesn't tell us if the column has NaN values, if the user has explicitly declared that it
			// does, we create stats without an upper max value, as NaN compares larger than anything else.
			row_group_stats = CreateFloatingPointStats(type, schema, parquet_stats);
		} else {
			// Otherwise we use the numeric stats as usual, which might lead to "wrong" pruning if the column contains
			// NaN values. The parquet spec is not clear on how to handle NaN values in statistics, and so this is
			// probably the best we can do for now.
			row_group_stats = CreateNumericStats(type, schema, parquet_stats);
		}
		break;
	case LogicalTypeId::BLOB:
	case LogicalTypeId::VARCHAR: {
		auto string_stats = StringStats::CreateUnknown(type);
		const bool is_varchar = type.id() == LogicalTypeId::VARCHAR;
		if (parquet_stats.__isset.min_value && StringColumnReader::IsValid(parquet_stats.min_value, is_varchar)) {
			StringStats::SetMin(string_stats, parquet_stats.min_value);
		} else if (parquet_stats.__isset.min && StringColumnReader::IsValid(parquet_stats.min, is_varchar)) {
			StringStats::SetMin(string_stats, parquet_stats.min);
		}
		if (parquet_stats.__isset.max_value && StringColumnReader::IsValid(parquet_stats.max_value, is_varchar)) {
			StringStats::SetMax(string_stats, parquet_stats.max_value);
		} else if (parquet_stats.__isset.max && StringColumnReader::IsValid(parquet_stats.max, is_varchar)) {
			StringStats::SetMax(string_stats, parquet_stats.max);
		}
		row_group_stats = string_stats.ToUnique();
		break;
	}
	case LogicalTypeId::GEOMETRY: {
		auto geo_stats = GeometryStats::CreateUnknown(type);
		if (column_chunk.meta_data.__isset.geospatial_statistics) {
			if (column_chunk.meta_data.geospatial_statistics.__isset.bbox) {
				auto &bbox = column_chunk.meta_data.geospatial_statistics.bbox;
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
			if (column_chunk.meta_data.geospatial_statistics.__isset.geospatial_types) {
				auto &types = column_chunk.meta_data.geospatial_statistics.geospatial_types;
				auto &stats_types = GeometryStats::GetTypes(geo_stats);

				// if types are set but empty, that still means "any type" - so we leave stats_types as-is (unknown)
				// otherwise, clear and set to the actual types

				if (!types.empty()) {
					stats_types.Clear();
					for (auto &geom_type : types) {
						stats_types.AddWKBType(geom_type);
					}
				}
			}
		}
		row_group_stats = geo_stats.ToUnique();
		break;
	}
	default:
		// no specific stats, only create unknown stats to hold validity information
		auto unknown_stats = BaseStatistics::CreateUnknown(type);
		row_group_stats = unknown_stats.ToUnique();
		break;
	} // end of type switch

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

static bool HasFilterConstants(const TableFilter &duckdb_filter) {
	switch (duckdb_filter.filter_type) {
	case TableFilterType::CONSTANT_COMPARISON: {
		auto &constant_filter = duckdb_filter.Cast<ConstantFilter>();
		return (constant_filter.comparison_type == ExpressionType::COMPARE_EQUAL && !constant_filter.constant.IsNull());
	}
	case TableFilterType::CONJUNCTION_AND: {
		auto &conjunction_and_filter = duckdb_filter.Cast<ConjunctionAndFilter>();
		bool child_has_constant = false;
		for (auto &child_filter : conjunction_and_filter.child_filters) {
			child_has_constant |= HasFilterConstants(*child_filter);
		}
		return child_has_constant;
	}
	case TableFilterType::CONJUNCTION_OR: {
		auto &conjunction_or_filter = duckdb_filter.Cast<ConjunctionOrFilter>();
		bool child_has_constant = false;
		for (auto &child_filter : conjunction_or_filter.child_filters) {
			child_has_constant |= HasFilterConstants(*child_filter);
		}
		return child_has_constant;
	}
	default:
		return false;
	}
}

template <class T>
static uint64_t ValueXH64FixedWidth(const Value &constant) {
	T val = constant.GetValue<T>();
	return duckdb_zstd::XXH64(&val, sizeof(val), 0);
}

// TODO we can only this if the parquet representation of the type exactly matches the duckdb rep!
// TODO TEST THIS!
// TODO perhaps we can re-use some writer infra here
static uint64_t ValueXXH64(const Value &constant) {
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
	case PhysicalType::FLOAT:
		return ValueXH64FixedWidth<float>(constant);
	case PhysicalType::DOUBLE:
		return ValueXH64FixedWidth<double>(constant);
	case PhysicalType::VARCHAR: {
		auto &val = StringValue::Get(constant);
		return duckdb_zstd::XXH64(val.data(), val.size(), 0);
	}
	default:
		return 0;
	}
}

static bool ApplyBloomFilter(const TableFilter &duckdb_filter, ParquetBloomFilter &bloom_filter) {
	switch (duckdb_filter.filter_type) {
	case TableFilterType::CONSTANT_COMPARISON: {
		auto &constant_filter = duckdb_filter.Cast<ConstantFilter>();
		auto is_compare_equal = constant_filter.comparison_type == ExpressionType::COMPARE_EQUAL;
		D_ASSERT(!constant_filter.constant.IsNull());
		auto hash = ValueXXH64(constant_filter.constant);
		return hash > 0 && !bloom_filter.FilterCheck(hash) && is_compare_equal;
	}
	case TableFilterType::CONJUNCTION_AND: {
		auto &conjunction_and_filter = duckdb_filter.Cast<ConjunctionAndFilter>();
		bool any_children_true = false;
		for (auto &child_filter : conjunction_and_filter.child_filters) {
			any_children_true |= ApplyBloomFilter(*child_filter, bloom_filter);
		}
		return any_children_true;
	}
	case TableFilterType::CONJUNCTION_OR: {
		auto &conjunction_or_filter = duckdb_filter.Cast<ConjunctionOrFilter>();
		bool all_children_true = true;
		for (auto &child_filter : conjunction_or_filter.child_filters) {
			all_children_true &= ApplyBloomFilter(*child_filter, bloom_filter);
		}
		return all_children_true;
	}
	default:
		return false;
	}
}

bool ParquetStatisticsUtils::BloomFilterSupported(const LogicalTypeId &type_id) {
	switch (type_id) {
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
		return true;
	default:
		return false;
	}
}

bool ParquetStatisticsUtils::BloomFilterExcludes(const TableFilter &duckdb_filter,
                                                 const duckdb_parquet::ColumnMetaData &column_meta_data,
                                                 TProtocol &file_proto, Allocator &allocator) {
	if (!HasFilterConstants(duckdb_filter) || !column_meta_data.__isset.bloom_filter_offset ||
	    column_meta_data.bloom_filter_offset <= 0) {
		return false;
	}
	// TODO check length against file length!

	auto &transport = reinterpret_cast<ThriftFileTransport &>(*file_proto.getTransport());
	transport.SetLocation(column_meta_data.bloom_filter_offset);
	if (column_meta_data.__isset.bloom_filter_length && column_meta_data.bloom_filter_length > 0) {
		transport.Prefetch(column_meta_data.bloom_filter_offset, column_meta_data.bloom_filter_length);
	}

	duckdb_parquet::BloomFilterHeader filter_header;
	// TODO the bloom filter could be encrypted, too, so need to double check that this is NOT the case
	filter_header.read(&file_proto);
	if (!filter_header.algorithm.__isset.BLOCK || !filter_header.compression.__isset.UNCOMPRESSED ||
	    !filter_header.hash.__isset.XXHASH) {
		return false;
	}

	auto new_buffer = make_uniq<ResizeableBuffer>(allocator, filter_header.numBytes);
	transport.read(new_buffer->ptr, filter_header.numBytes);
	ParquetBloomFilter bloom_filter(std::move(new_buffer));
	return ApplyBloomFilter(duckdb_filter, bloom_filter);
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
