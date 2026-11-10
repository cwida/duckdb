#include "duckdb/storage/statistics/bignum_stats.hpp"

#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/types/bignum.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"

namespace duckdb {

//! The owned bytes of the min and max bounds
struct BignumStatsExtraData : public ExtraStatsData {
	string blobs[2];
};

//! The bytes a bound keeps: the header and the first magnitude bytes of the blob
static constexpr idx_t MAX_BOUND_SIZE = Bignum::BIGNUM_HEADER_SIZE + BignumStatsData::MAX_BOUND_DATA_BYTES;

//! The byte a cut bound omits: a min bound continues with 0x00 and a max bound with 0xFF
static data_t PadByte(bool is_min) {
	return is_min ? 0x00 : 0xFF;
}

//! The number of magnitude bytes the header of a blob records (see Bignum::SetHeader)
static idx_t HeaderDataSize(const string_t &blob) {
	auto data = const_data_ptr_cast(blob.GetData());
	uint32_t header = (uint32_t(data[0]) << 16) | (uint32_t(data[1]) << 8) | uint32_t(data[2]);
	if ((data[0] & 0x80) == 0) {
		// the header of a negative value is complemented
		header = ~header;
	}
	return header & 0x7FFFFF;
}

//! Compares a blob or bound with a bound, the bytes a cut bound omits read as its pad; the byte order of the BIGNUM
//! encoding is the numeric order
static int CompareBound(const string_t &lhs, const string_t &rhs, bool is_min) {
	const idx_t lhs_size = lhs.GetSize();
	const idx_t rhs_size = rhs.GetSize();
	const idx_t common = MinValue(lhs_size, rhs_size);
	const int cmp = memcmp(lhs.GetData(), rhs.GetData(), common);
	if (cmp != 0) {
		return cmp < 0 ? -1 : 1;
	}
	// the longer side continues against the pad of the shorter one
	const bool lhs_longer = lhs_size > rhs_size;
	auto &longer = lhs_longer ? lhs : rhs;
	const auto pad = PadByte(is_min);
	for (idx_t i = common; i < longer.GetSize(); i++) {
		const auto byte = static_cast<data_t>(longer.GetData()[i]);
		if (byte != pad) {
			return (byte > pad) == lhs_longer ? 1 : -1;
		}
	}
	return 0;
}

//! The complete blob of a bound: a cut bound is padded to the size its header records
static string PadBound(const string_t &bound, bool is_min) {
	string result(bound.GetData(), bound.GetSize());
	const idx_t full_size = Bignum::BIGNUM_HEADER_SIZE + HeaderDataSize(bound);
	if (result.size() < full_size) {
		result.append(full_size - result.size(), static_cast<char>(PadByte(is_min)));
	}
	return result;
}

BaseStatistics BignumStats::CreateUnknown(LogicalType type) {
	BaseStatistics result(std::move(type));
	result.InitializeUnknown();
	auto &data = GetDataUnsafe(result);
	data.has_min = false;
	data.has_max = false;
	data.min_exact = true;
	data.max_exact = true;
	return result;
}

BaseStatistics BignumStats::CreateEmpty(LogicalType type) {
	BaseStatistics result(std::move(type));
	result.InitializeEmpty();
	auto &data = GetDataUnsafe(result);
	data.has_min = true;
	data.has_max = true;
	data.min_exact = true;
	data.max_exact = true;
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

string_t BignumStats::AssignBound(BaseStatistics &stats, string_t blob, bool is_min, bool &exact) {
	if (!stats.extra_data) {
		stats.extra_data = make_uniq<BignumStatsExtraData>();
	}
	auto &owned = stats.extra_data->Cast<BignumStatsExtraData>().blobs[is_min ? 0 : 1];
	idx_t size = blob.GetSize();
	if (size > MAX_BOUND_SIZE) {
		// the cut bound reads as the blob with its tail replaced by the pad, which lies beyond the blob
		size = MAX_BOUND_SIZE;
		exact = false;
	}
	owned.assign(blob.GetData(), size);
	return string_t(owned);
}

bool BignumStats::HasMinMax(const BaseStatistics &stats) {
	if (stats.GetType().id() == LogicalTypeId::SQLNULL) {
		return false;
	}
	auto &data = GetDataUnsafe(stats);
	return data.has_min && data.has_max && data.min.GetSize() > 0 && data.max.GetSize() > 0;
}

bool BignumStats::MinIsExact(const BaseStatistics &stats) {
	return HasMinMax(stats) && GetDataUnsafe(stats).min_exact;
}

bool BignumStats::MaxIsExact(const BaseStatistics &stats) {
	return HasMinMax(stats) && GetDataUnsafe(stats).max_exact;
}

string BignumStats::MinBlob(const BaseStatistics &stats) {
	D_ASSERT(HasMinMax(stats));
	return PadBound(GetDataUnsafe(stats).min, true);
}

string BignumStats::MaxBlob(const BaseStatistics &stats) {
	D_ASSERT(HasMinMax(stats));
	return PadBound(GetDataUnsafe(stats).max, false);
}

Value BignumStats::Min(const BaseStatistics &stats) {
	if (!HasMinMax(stats)) {
		throw InternalException("BignumStats::Min called but no min value was found");
	}
	auto &type = stats.GetType();
	auto blob = MinBlob(stats);
	return Value::DECIMAL(bignum_t(string_t(blob)), DecimalType::GetWidth(type), DecimalType::GetScale(type));
}

Value BignumStats::Max(const BaseStatistics &stats) {
	if (!HasMinMax(stats)) {
		throw InternalException("BignumStats::Max called but no max value was found");
	}
	auto &type = stats.GetType();
	auto blob = MaxBlob(stats);
	return Value::DECIMAL(bignum_t(string_t(blob)), DecimalType::GetWidth(type), DecimalType::GetScale(type));
}

void BignumStats::SetMin(BaseStatistics &stats, string_t blob) {
	auto &data = GetDataUnsafe(stats);
	data.has_min = true;
	data.min_exact = true;
	data.min = AssignBound(stats, blob, true, data.min_exact);
}

void BignumStats::SetMax(BaseStatistics &stats, string_t blob) {
	auto &data = GetDataUnsafe(stats);
	data.has_max = true;
	data.max_exact = true;
	data.max = AssignBound(stats, blob, false, data.max_exact);
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

void BignumStats::MergeBound(BaseStatistics &stats, string_t blob, bool exact, bool is_min) {
	auto &data = GetDataUnsafe(stats);
	auto &bound = is_min ? data.min : data.max;
	auto &bound_exact = is_min ? data.min_exact : data.max_exact;
	if (bound.GetSize() > 0) {
		const int cmp = CompareBound(blob, bound, is_min);
		if (is_min ? cmp > 0 : cmp < 0) {
			return;
		}
		if (cmp == 0) {
			// the same bound: it is attained when either side attains it
			bound_exact = bound_exact || exact;
			return;
		}
	}
	bound = AssignBound(stats, blob, is_min, exact);
	bound_exact = exact;
}

void BignumStats::Update(BaseStatistics &stats, string_t blob) {
	Merge(stats, blob, blob);
}

void BignumStats::Merge(BaseStatistics &stats, string_t min, string_t max) {
	auto &data = GetDataUnsafe(stats);
	if (data.has_min && min.GetSize() > 0) {
		MergeBound(stats, min, true, true);
	}
	if (data.has_max && max.GetSize() > 0) {
		MergeBound(stats, max, true, false);
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
	if (data.has_min && other_data.min.GetSize() > 0) {
		MergeBound(stats, other_data.min, other_data.min_exact, true);
	}
	if (data.has_max && other_data.max.GetSize() > 0) {
		MergeBound(stats, other_data.max, other_data.max_exact, false);
	}
}

void BignumStats::Copy(BaseStatistics &stats, const BaseStatistics &other) {
	auto &data = GetDataUnsafe(stats);
	auto &other_data = GetDataUnsafe(other);
	// the bounds of the other statistics are within the bound size, so they are copied as they are
	bool exact = true;
	if (other_data.min.GetSize() > 0) {
		data.min = AssignBound(stats, other_data.min, true, exact);
	}
	if (other_data.max.GetSize() > 0) {
		data.max = AssignBound(stats, other_data.max, false, exact);
	}
	data.min_exact = other_data.min_exact;
	data.max_exact = other_data.max_exact;
}

//! The bounds enclose every value, so a comparison the bounds decide is decided for every value; a bound that is not
//! attained only loses pruning power, never correctness
static FilterPropagateResult CheckZonemapBound(const string_t &min, const string_t &max, ExpressionType comparison_type,
                                               const string_t &constant) {
	const int min_cmp = CompareBound(constant, min, true);
	const int max_cmp = CompareBound(constant, max, false);
	const bool in_range = min_cmp >= 0 && max_cmp <= 0;
	const bool exact_range = min_cmp == 0 && max_cmp == 0;
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
		if (min_cmp <= 0) {
			return FilterPropagateResult::FILTER_ALWAYS_TRUE;
		}
		return max_cmp <= 0 ? FilterPropagateResult::NO_PRUNING_POSSIBLE : FilterPropagateResult::FILTER_ALWAYS_FALSE;
	case ExpressionType::COMPARE_GREATERTHAN:
		if (min_cmp < 0) {
			return FilterPropagateResult::FILTER_ALWAYS_TRUE;
		}
		return max_cmp < 0 ? FilterPropagateResult::NO_PRUNING_POSSIBLE : FilterPropagateResult::FILTER_ALWAYS_FALSE;
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		if (max_cmp >= 0) {
			return FilterPropagateResult::FILTER_ALWAYS_TRUE;
		}
		return min_cmp >= 0 ? FilterPropagateResult::NO_PRUNING_POSSIBLE : FilterPropagateResult::FILTER_ALWAYS_FALSE;
	case ExpressionType::COMPARE_LESSTHAN:
		if (max_cmp > 0) {
			return FilterPropagateResult::FILTER_ALWAYS_TRUE;
		}
		return min_cmp > 0 ? FilterPropagateResult::NO_PRUNING_POSSIBLE : FilterPropagateResult::FILTER_ALWAYS_FALSE;
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
		auto prune_result = CheckZonemapBound(data.min, data.max, comparison_type, string_t(constant));
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
	serializer.WritePropertyWithDefault(204, "min_exact", data.min_exact, true);
	serializer.WritePropertyWithDefault(205, "max_exact", data.max_exact, true);
}

void BignumStats::Deserialize(Deserializer &deserializer, BaseStatistics &stats) {
	auto &data = GetDataUnsafe(stats);
	data.has_min = deserializer.ReadProperty<bool>(200, "has_min");
	data.has_max = deserializer.ReadProperty<bool>(201, "has_max");
	auto min = deserializer.ReadProperty<string>(202, "min");
	auto max = deserializer.ReadProperty<string>(203, "max");
	data.min_exact = deserializer.ReadPropertyWithExplicitDefault<bool>(204, "min_exact", true);
	data.max_exact = deserializer.ReadPropertyWithExplicitDefault<bool>(205, "max_exact", true);
	if (!min.empty()) {
		data.min = AssignBound(stats, string_t(min), true, data.min_exact);
	}
	if (!max.empty()) {
		data.max = AssignBound(stats, string_t(max), false, data.max_exact);
	}
}

child_list_t<Value> BignumStats::ToStruct(const BaseStatistics &stats) {
	child_list_t<Value> result;
	auto &data = GetDataUnsafe(stats);
	if (data.has_min && data.has_max) {
		result.emplace_back("min", data.min.GetSize() > 0 ? Min(stats) : Value(stats.GetType()));
		result.emplace_back("max", data.max.GetSize() > 0 ? Max(stats) : Value(stats.GetType()));
		result.emplace_back("min_exact", Value::BOOLEAN(data.min_exact));
		result.emplace_back("max_exact", Value::BOOLEAN(data.max_exact));
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
		if (data.has_min && (data.min.GetSize() == 0 || CompareBound(value, data.min, true) < 0)) {
			throw InternalException("Statistics mismatch: value is smaller than min.\nStatistics: %s\nVector: %s",
			                        stats.ToString(), vector.ToString());
		}
		if (data.has_max && (data.max.GetSize() == 0 || CompareBound(value, data.max, false) > 0)) {
			throw InternalException("Statistics mismatch: value is bigger than max.\nStatistics: %s\nVector: %s",
			                        stats.ToString(), vector.ToString());
		}
		// LCOV_EXCL_STOP
	}
}

} // namespace duckdb
