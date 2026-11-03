#include "duckdb/storage/statistics/bignum_stats.hpp"

#include "duckdb/common/operator/comparison_operators.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/types/bignum.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"

namespace duckdb {

//! The owned bytes of the min and max blobs
struct BignumStatsExtraData : public ExtraStatsData {
	string blobs[2];
};

BaseStatistics BignumStats::CreateUnknown(LogicalType type) {
	BaseStatistics result(std::move(type));
	result.InitializeUnknown();
	auto &data = GetDataUnsafe(result);
	data.has_min = false;
	data.has_max = false;
	return result;
}

BaseStatistics BignumStats::CreateEmpty(LogicalType type) {
	BaseStatistics result(std::move(type));
	result.InitializeEmpty();
	auto &data = GetDataUnsafe(result);
	data.has_min = true;
	data.has_max = true;
	return result;
}

BignumStatsData &BignumStats::GetDataUnsafe(BaseStatistics &stats) {
	D_ASSERT(stats.GetStatsType() == StatisticsType::BIGNUM_STATS);
	return stats.stats_union.bignum_data;
}

const BignumStatsData &BignumStats::GetDataUnsafe(const BaseStatistics &stats) {
	D_ASSERT(stats.GetStatsType() == StatisticsType::BIGNUM_STATS);
	return stats.stats_union.bignum_data;
}

string_t BignumStats::AssignBlob(BaseStatistics &stats, string_t blob, bool is_min) {
	if (!stats.extra_data) {
		stats.extra_data = make_uniq<BignumStatsExtraData>();
	}
	auto &owned = stats.extra_data->Cast<BignumStatsExtraData>().blobs[is_min ? 0 : 1];
	owned.assign(blob.GetData(), blob.GetSize());
	return string_t(owned);
}

bool BignumStats::HasMinMax(const BaseStatistics &stats) {
	if (stats.GetType().id() == LogicalTypeId::SQLNULL) {
		return false;
	}
	auto &data = GetDataUnsafe(stats);
	return data.has_min && data.has_max && data.min.GetSize() > 0 && data.max.GetSize() > 0;
}

string_t BignumStats::MinBlob(const BaseStatistics &stats) {
	D_ASSERT(HasMinMax(stats));
	return GetDataUnsafe(stats).min;
}

string_t BignumStats::MaxBlob(const BaseStatistics &stats) {
	D_ASSERT(HasMinMax(stats));
	return GetDataUnsafe(stats).max;
}

Value BignumStats::Min(const BaseStatistics &stats) {
	if (!HasMinMax(stats)) {
		throw InternalException("BignumStats::Min called but no min value was found");
	}
	auto &type = stats.GetType();
	return Value::DECIMAL(bignum_t(GetDataUnsafe(stats).min), DecimalType::GetWidth(type), DecimalType::GetScale(type));
}

Value BignumStats::Max(const BaseStatistics &stats) {
	if (!HasMinMax(stats)) {
		throw InternalException("BignumStats::Max called but no max value was found");
	}
	auto &type = stats.GetType();
	return Value::DECIMAL(bignum_t(GetDataUnsafe(stats).max), DecimalType::GetWidth(type), DecimalType::GetScale(type));
}

void BignumStats::SetMin(BaseStatistics &stats, string_t blob) {
	auto &data = GetDataUnsafe(stats);
	data.has_min = true;
	data.min = AssignBlob(stats, blob, true);
}

void BignumStats::SetMax(BaseStatistics &stats, string_t blob) {
	auto &data = GetDataUnsafe(stats);
	data.has_max = true;
	data.max = AssignBlob(stats, blob, false);
}

void BignumStats::SetMin(BaseStatistics &stats, const Value &val) {
	if (val.IsNull()) {
		GetDataUnsafe(stats).has_min = false;
		return;
	}
	D_ASSERT(val.type() == stats.GetType());
	SetMin(stats, string_t(StringValue::Get(val)));
}

void BignumStats::SetMax(BaseStatistics &stats, const Value &val) {
	if (val.IsNull()) {
		GetDataUnsafe(stats).has_max = false;
		return;
	}
	D_ASSERT(val.type() == stats.GetType());
	SetMax(stats, string_t(StringValue::Get(val)));
}

void BignumStats::MergeBound(BaseStatistics &stats, string_t blob, bool is_min) {
	auto &data = GetDataUnsafe(stats);
	auto &bound = is_min ? data.min : data.max;
	if (bound.GetSize() > 0) {
		// the byte order of the BIGNUM encoding is the numeric order
		if (is_min ? GreaterThanEquals::Operation(blob, bound) : LessThanEquals::Operation(blob, bound)) {
			return;
		}
	}
	bound = AssignBlob(stats, blob, is_min);
}

void BignumStats::Update(BaseStatistics &stats, string_t blob) {
	Merge(stats, blob, blob);
}

void BignumStats::Merge(BaseStatistics &stats, string_t min, string_t max) {
	auto &data = GetDataUnsafe(stats);
	if (data.has_min && min.GetSize() > 0) {
		MergeBound(stats, min, true);
	}
	if (data.has_max && max.GetSize() > 0) {
		MergeBound(stats, max, false);
	}
}

void BignumStats::Merge(BaseStatistics &stats, const BaseStatistics &other) {
	if (other.GetType().id() == LogicalTypeId::VALIDITY || other.GetType().id() == LogicalTypeId::SQLNULL) {
		return;
	}
	auto &data = GetDataUnsafe(stats);
	auto &other_data = GetDataUnsafe(other);
	data.has_min = data.has_min && other_data.has_min;
	data.has_max = data.has_max && other_data.has_max;
	Merge(stats, other_data.min, other_data.max);
}

void BignumStats::Copy(BaseStatistics &stats, const BaseStatistics &other) {
	auto &data = GetDataUnsafe(stats);
	auto &other_data = GetDataUnsafe(other);
	if (other_data.min.GetSize() > 0) {
		data.min = AssignBlob(stats, other_data.min, true);
	}
	if (other_data.max.GetSize() > 0) {
		data.max = AssignBlob(stats, other_data.max, false);
	}
}

static FilterPropagateResult CheckZonemapBlob(string_t min, string_t max, ExpressionType comparison_type,
                                              string_t constant) {
	const bool in_range = GreaterThanEquals::Operation(constant, min) && LessThanEquals::Operation(constant, max);
	const bool exact_range = Equals::Operation(constant, min) && Equals::Operation(constant, max);
	switch (comparison_type) {
	case ExpressionType::COMPARE_EQUAL:
	case ExpressionType::COMPARE_NOT_DISTINCT_FROM:
		if (exact_range) {
			return FilterPropagateResult::FILTER_ALWAYS_TRUE;
		}
		return in_range ? FilterPropagateResult::NO_PRUNING_POSSIBLE : FilterPropagateResult::FILTER_ALWAYS_FALSE;
	case ExpressionType::COMPARE_NOTEQUAL:
	case ExpressionType::COMPARE_DISTINCT_FROM:
		if (!in_range) {
			return FilterPropagateResult::FILTER_ALWAYS_TRUE;
		}
		return exact_range ? FilterPropagateResult::FILTER_ALWAYS_FALSE : FilterPropagateResult::NO_PRUNING_POSSIBLE;
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		if (GreaterThanEquals::Operation(min, constant)) {
			return FilterPropagateResult::FILTER_ALWAYS_TRUE;
		}
		return GreaterThanEquals::Operation(max, constant) ? FilterPropagateResult::NO_PRUNING_POSSIBLE
		                                                   : FilterPropagateResult::FILTER_ALWAYS_FALSE;
	case ExpressionType::COMPARE_GREATERTHAN:
		if (GreaterThan::Operation(min, constant)) {
			return FilterPropagateResult::FILTER_ALWAYS_TRUE;
		}
		return GreaterThan::Operation(max, constant) ? FilterPropagateResult::NO_PRUNING_POSSIBLE
		                                             : FilterPropagateResult::FILTER_ALWAYS_FALSE;
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		if (LessThanEquals::Operation(max, constant)) {
			return FilterPropagateResult::FILTER_ALWAYS_TRUE;
		}
		return LessThanEquals::Operation(min, constant) ? FilterPropagateResult::NO_PRUNING_POSSIBLE
		                                                : FilterPropagateResult::FILTER_ALWAYS_FALSE;
	case ExpressionType::COMPARE_LESSTHAN:
		if (LessThan::Operation(max, constant)) {
			return FilterPropagateResult::FILTER_ALWAYS_TRUE;
		}
		return LessThan::Operation(min, constant) ? FilterPropagateResult::NO_PRUNING_POSSIBLE
		                                          : FilterPropagateResult::FILTER_ALWAYS_FALSE;
	default:
		throw InternalException("Expression type in zonemap check not implemented");
	}
}

FilterPropagateResult BignumStats::CheckZonemap(const BaseStatistics &stats, ExpressionType comparison_type,
                                                array_ptr<const Value> constants) {
	if (!HasMinMax(stats)) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	D_ASSERT(stats.CanHaveNoNull());
	auto &data = GetDataUnsafe(stats);
	for (auto &constant_value : constants) {
		D_ASSERT(constant_value.type() == stats.GetType());
		D_ASSERT(!constant_value.IsNull());
		auto &constant = StringValue::Get(constant_value);
		auto prune_result = CheckZonemapBlob(data.min, data.max, comparison_type, string_t(constant));
		if (prune_result == FilterPropagateResult::NO_PRUNING_POSSIBLE) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		} else if (prune_result == FilterPropagateResult::FILTER_ALWAYS_TRUE) {
			return FilterPropagateResult::FILTER_ALWAYS_TRUE;
		}
	}
	return FilterPropagateResult::FILTER_ALWAYS_FALSE;
}

void BignumStats::Serialize(const BaseStatistics &stats, Serializer &serializer) {
	auto &data = GetDataUnsafe(stats);
	serializer.WriteProperty(200, "has_min", data.has_min);
	serializer.WriteProperty(201, "has_max", data.has_max);
	serializer.WriteProperty(202, "min", data.min.GetString());
	serializer.WriteProperty(203, "max", data.max.GetString());
}

void BignumStats::Deserialize(Deserializer &deserializer, BaseStatistics &stats) {
	auto &data = GetDataUnsafe(stats);
	data.has_min = deserializer.ReadProperty<bool>(200, "has_min");
	data.has_max = deserializer.ReadProperty<bool>(201, "has_max");
	auto min = deserializer.ReadProperty<string>(202, "min");
	auto max = deserializer.ReadProperty<string>(203, "max");
	if (!min.empty()) {
		data.min = AssignBlob(stats, string_t(min), true);
	}
	if (!max.empty()) {
		data.max = AssignBlob(stats, string_t(max), false);
	}
}

child_list_t<Value> BignumStats::ToStruct(const BaseStatistics &stats) {
	child_list_t<Value> result;
	auto &data = GetDataUnsafe(stats);
	if (data.has_min && data.has_max) {
		result.emplace_back("min", data.min.GetSize() > 0 ? Min(stats) : Value(stats.GetType()));
		result.emplace_back("max", data.max.GetSize() > 0 ? Max(stats) : Value(stats.GetType()));
	}
	return result;
}

void BignumStats::Verify(const BaseStatistics &stats, const Vector &vector, const SelectionVector &sel, idx_t count) {
	auto &data = GetDataUnsafe(stats);
	auto entries = vector.Values<string_t>();
	for (idx_t i = 0; i < count; i++) {
		auto idx = sel.get_index(i);
		auto entry = entries[idx];
		if (!entry.IsValid()) {
			continue;
		}
		auto value = entry.GetValue();
		// LCOV_EXCL_START
		if (data.has_min && (data.min.GetSize() == 0 || LessThan::Operation(value, data.min))) {
			throw InternalException("Statistics mismatch: value is smaller than min.\nStatistics: %s\nVector: %s",
			                        stats.ToString(), vector.ToString());
		}
		if (data.has_max && (data.max.GetSize() == 0 || GreaterThan::Operation(value, data.max))) {
			throw InternalException("Statistics mismatch: value is bigger than max.\nStatistics: %s\nVector: %s",
			                        stats.ToString(), vector.ToString());
		}
		// LCOV_EXCL_STOP
	}
}

} // namespace duckdb
