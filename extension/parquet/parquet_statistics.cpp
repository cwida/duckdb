#include "parquet_statistics.hpp"
#include "parquet_timestamp.hpp"

#include "duckdb/common/types/value.hpp"
#include "duckdb/storage/statistics/string_statistics.hpp"
#include "duckdb/storage/statistics/numeric_statistics.hpp"

namespace duckdb {

using parquet::format::ConvertedType;
using parquet::format::Type;

template <Value (*FUNC)(const_data_ptr_t input)>
static unique_ptr<BaseStatistics> templated_get_numeric_stats(const LogicalType &type,
                                                              const parquet::format::Statistics &parquet_stats) {
	auto stats = make_unique<NumericStatistics>(type);

	// for reasons unknown to science, Parquet defines *both* `min` and `min_value` as well as `max` and
	// `max_value`. All are optional. such elegance.
	if (parquet_stats.__isset.min) {
		stats->min = FUNC((const_data_ptr_t)parquet_stats.min.data());
	} else if (parquet_stats.__isset.min_value) {
		stats->min = FUNC((const_data_ptr_t)parquet_stats.min_value.data());
	} else {
		stats->min.is_null = true;
	}
	if (parquet_stats.__isset.max) {
		stats->max = FUNC((const_data_ptr_t)parquet_stats.max.data());
	} else if (parquet_stats.__isset.max_value) {
		stats->max = FUNC((const_data_ptr_t)parquet_stats.max_value.data());
	} else {
		stats->max.is_null = true;
	}
	// GCC 4.x insists on a move() here
	return move(stats);
}

template <class T> static Value transform_statistics_plain(const_data_ptr_t input) {
	return Value(Load<T>(input));
}

static Value transform_statistics_timestamp_ms(const_data_ptr_t input) {
	return Value::TIMESTAMP(parquet_timestamp_ms_to_timestamp(Load<int64_t>(input)));
}

static Value transform_statistics_timestamp_micros(const_data_ptr_t input) {
	return Value::TIMESTAMP(parquet_timestamp_micros_to_timestamp(Load<int64_t>(input)));
}

static Value transform_statistics_timestamp_impala(const_data_ptr_t input) {
	return Value::TIMESTAMP(impala_timestamp_to_timestamp_t(Load<Int96>(input)));
}

unique_ptr<BaseStatistics> parquet_transform_column_statistics(const SchemaElement &s_ele, const LogicalType &type,
                                                               const ColumnChunk &column_chunk) {
	if (!column_chunk.__isset.meta_data || !column_chunk.meta_data.__isset.statistics) {
		// no stats present for row group
		return nullptr;
	}
	auto &parquet_stats = column_chunk.meta_data.statistics;
	unique_ptr<BaseStatistics> row_group_stats;

	switch (type.id()) {
	case LogicalTypeId::INTEGER:
		row_group_stats = templated_get_numeric_stats<transform_statistics_plain<int32_t>>(type, parquet_stats);
		break;

	case LogicalTypeId::BIGINT:
		row_group_stats = templated_get_numeric_stats<transform_statistics_plain<int64_t>>(type, parquet_stats);
		break;

	case LogicalTypeId::FLOAT:
		row_group_stats = templated_get_numeric_stats<transform_statistics_plain<float>>(type, parquet_stats);
		break;

	case LogicalTypeId::DOUBLE:
		row_group_stats = templated_get_numeric_stats<transform_statistics_plain<double>>(type, parquet_stats);
		break;

		// here we go, our favorite type
	case LogicalTypeId::TIMESTAMP: {
		switch (s_ele.type) {
		case Type::INT64:
			// arrow timestamp
			switch (s_ele.converted_type) {
			case ConvertedType::TIMESTAMP_MICROS:
				row_group_stats =
				    templated_get_numeric_stats<transform_statistics_timestamp_micros>(type, parquet_stats);
				break;
			case ConvertedType::TIMESTAMP_MILLIS:
				row_group_stats = templated_get_numeric_stats<transform_statistics_timestamp_ms>(type, parquet_stats);
				break;
			default:
				return nullptr;
			}
			break;
		case Type::INT96:
			// impala timestamp
			row_group_stats = templated_get_numeric_stats<transform_statistics_timestamp_impala>(type, parquet_stats);
			break;
		default:
			return nullptr;
		}
		break;
	}
	case LogicalTypeId::VARCHAR: {
		auto string_stats = make_unique<StringStatistics>(type);
		if (parquet_stats.__isset.min) {
			memcpy(string_stats->min, (data_ptr_t)parquet_stats.min.data(),
			       MinValue<idx_t>(parquet_stats.min.size(), StringStatistics::MAX_STRING_MINMAX_SIZE));
		} else if (parquet_stats.__isset.min_value) {
			memcpy(string_stats->min, (data_ptr_t)parquet_stats.min_value.data(),
			       MinValue<idx_t>(parquet_stats.min_value.size(), StringStatistics::MAX_STRING_MINMAX_SIZE));
		} else {
			return nullptr;
		}
		if (parquet_stats.__isset.max) {
			memcpy(string_stats->max, (data_ptr_t)parquet_stats.max.data(),
			       MinValue<idx_t>(parquet_stats.max.size(), StringStatistics::MAX_STRING_MINMAX_SIZE));
		} else if (parquet_stats.__isset.max_value) {
			memcpy(string_stats->max, (data_ptr_t)parquet_stats.max_value.data(),
			       MinValue<idx_t>(parquet_stats.max_value.size(), StringStatistics::MAX_STRING_MINMAX_SIZE));
		} else {
			return nullptr;
		}

		string_stats->has_unicode = true; // we dont know better
		row_group_stats = move(string_stats);
		break;
	}
	default:
		// no stats for you
		break;
	} // end of type switch

	// null count is generic
	if (row_group_stats) {
		if (parquet_stats.__isset.null_count) {
			row_group_stats->has_null = parquet_stats.null_count != 0;
		} else {
			row_group_stats->has_null = true;
		}
	} else {
		// if stats are missing from any row group we know squat
		return nullptr;
	}

	return row_group_stats;
}

} // namespace duckdb
