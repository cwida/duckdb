#include "duckdb/common/operator/add.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/storage/statistics/list_stats.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/vector.hpp"

#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/storage/storage_info.hpp"

namespace duckdb {

bool ListStatsData::HasMinListLength() const {
	return has_min_list_length && min_list_length != NumericLimits<uint64_t>::Maximum();
}

optional_idx ListStatsData::MinLength() const {
	return HasMinListLength() ? optional_idx(min_list_length) : optional_idx();
}

optional_idx ListStatsData::MaxLength() const {
	return has_max_list_length ? optional_idx(max_list_length) : optional_idx();
}

optional_idx ListStatsData::TotalLength() const {
	return has_total_list_length ? optional_idx(total_list_length) : optional_idx();
}

void ListStats::Construct(BaseStatistics &stats) {
	stats.child_stats = unsafe_unique_array<BaseStatistics>(new BaseStatistics[1]);
	BaseStatistics::Construct(stats.child_stats[0], ListType::GetChildType(stats.GetType()));
}

BaseStatistics ListStats::CreateUnknown(LogicalType type) {
	auto &child_type = ListType::GetChildType(type);
	BaseStatistics result(std::move(type));
	result.InitializeUnknown();
	auto &list_data = GetDataUnsafe(result);
	list_data.has_min_list_length = false;
	list_data.has_max_list_length = false;
	list_data.has_total_list_length = false;
	list_data.min_list_length = 0;
	list_data.max_list_length = 0;
	list_data.total_list_length = 0;
	result.child_stats[0].Copy(BaseStatistics::CreateUnknown(child_type));
	return result;
}

BaseStatistics ListStats::CreateEmpty(LogicalType type) {
	auto &child_type = ListType::GetChildType(type);
	BaseStatistics result(std::move(type));
	result.InitializeEmpty();
	ResetLengthData(result);
	result.child_stats[0].Copy(BaseStatistics::CreateEmpty(child_type));
	return result;
}

void ListStats::Copy(BaseStatistics &stats, const BaseStatistics &other) {
	D_ASSERT(stats.child_stats);
	D_ASSERT(other.child_stats);
	GetDataUnsafe(stats) = GetDataUnsafe(other);
	stats.child_stats[0].Copy(other.child_stats[0]);
}

ListStatsData &ListStats::GetDataUnsafe(BaseStatistics &stats) {
	D_ASSERT(stats.GetStatsType() == StatisticsType::LIST_STATS);
	return stats.stats_union.list_data;
}

const ListStatsData &ListStats::GetDataUnsafe(const BaseStatistics &stats) {
	D_ASSERT(stats.GetStatsType() == StatisticsType::LIST_STATS);
	return stats.stats_union.list_data;
}

optional_idx ListStats::MinListLength(const BaseStatistics &stats) {
	auto &data = GetDataUnsafe(stats);
	return data.HasMinListLength() ? optional_idx(data.min_list_length) : optional_idx();
}

optional_idx ListStats::MaxListLength(const BaseStatistics &stats) {
	auto &data = GetDataUnsafe(stats);
	return data.has_max_list_length ? optional_idx(data.max_list_length) : optional_idx();
}

optional_idx ListStats::TotalListLength(const BaseStatistics &stats) {
	auto &data = GetDataUnsafe(stats);
	return data.has_total_list_length ? optional_idx(data.total_list_length) : optional_idx();
}

void ListStats::ResetLengthData(BaseStatistics &stats) {
	auto &data = GetDataUnsafe(stats);
	data.has_min_list_length = true;
	data.has_max_list_length = true;
	data.has_total_list_length = true;
	data.min_list_length = NumericLimits<uint64_t>::Maximum();
	data.max_list_length = 0;
	data.total_list_length = 0;
}

void ListStats::SetLengthBounds(BaseStatistics &stats, optional_idx min_length, optional_idx max_length) {
	auto &data = GetDataUnsafe(stats);
	data.has_min_list_length = min_length.IsValid();
	data.min_list_length = min_length.IsValid() ? min_length.GetIndex() : 0;
	data.has_max_list_length = max_length.IsValid();
	data.max_list_length = max_length.IsValid() ? max_length.GetIndex() : 0;
	data.has_total_list_length = false;
	data.total_list_length = 0;
}

void ListStats::MergeInLengths(BaseStatistics &stats, idx_t min_length, idx_t max_length, idx_t total_length) {
	auto &data = GetDataUnsafe(stats);
	data.min_list_length = MinValue<uint64_t>(data.min_list_length, min_length);
	data.max_list_length = MaxValue<uint64_t>(data.max_list_length, max_length);
	if (!TryAddOperator::Operation<uint64_t, uint64_t, uint64_t>(data.total_list_length, total_length,
	                                                             data.total_list_length)) {
		// the sum of the lengths overflows - the total is no longer known
		data.has_total_list_length = false;
		data.total_list_length = 0;
	}
}

ListStatsData ListStats::GetLengthData(const BaseStatistics &stats) {
	return GetDataUnsafe(stats);
}

void ListStats::SetLengthData(BaseStatistics &stats, const ListStatsData &data) {
	GetDataUnsafe(stats) = data;
}

void ListStats::ResetTotalListLength(BaseStatistics &stats) {
	GetDataUnsafe(stats).has_total_list_length = false;
}

const BaseStatistics &ListStats::GetChildStats(const BaseStatistics &stats) {
	if (stats.GetStatsType() != StatisticsType::LIST_STATS) {
		throw InternalException("ListStats::GetChildStats called on stats that is not a list");
	}
	D_ASSERT(stats.child_stats);
	return stats.child_stats[0];
}
BaseStatistics &ListStats::GetChildStats(BaseStatistics &stats) {
	if (stats.GetStatsType() != StatisticsType::LIST_STATS) {
		throw InternalException("ListStats::GetChildStats called on stats that is not a list");
	}
	D_ASSERT(stats.child_stats);
	return stats.child_stats[0];
}

void ListStats::SetChildStats(BaseStatistics &stats, unique_ptr<BaseStatistics> new_stats) {
	if (!new_stats) {
		stats.child_stats[0].Copy(BaseStatistics::CreateUnknown(ListType::GetChildType(stats.GetType())));
	} else {
		stats.child_stats[0].Copy(*new_stats);
	}
}

void ListStats::Merge(BaseStatistics &stats, const BaseStatistics &other, StatsMergeType merge_type) {
	if (other.GetType().id() == LogicalTypeId::VALIDITY) {
		return;
	}

	auto &list_data = GetDataUnsafe(stats);
	auto &other_data = GetDataUnsafe(other);
	list_data.has_min_list_length = list_data.has_min_list_length && other_data.has_min_list_length;
	list_data.min_list_length = MinValue<uint64_t>(list_data.min_list_length, other_data.min_list_length);
	list_data.has_max_list_length = list_data.has_max_list_length && other_data.has_max_list_length;
	list_data.max_list_length = MaxValue<uint64_t>(list_data.max_list_length, other_data.max_list_length);
	if (merge_type == StatsMergeType::EXPAND_BOUNDS) {
		// the other side may overlap with our data, so the total cannot be summed
		list_data.has_total_list_length = false;
	} else {
		list_data.has_total_list_length = list_data.has_total_list_length && other_data.has_total_list_length;
		if (!TryAddOperator::Operation<uint64_t, uint64_t, uint64_t>(
		        list_data.total_list_length, other_data.total_list_length, list_data.total_list_length)) {
			// the sum of the totals overflows - the total is no longer known
			list_data.has_total_list_length = false;
			list_data.total_list_length = 0;
		}
	}

	auto &child_stats = ListStats::GetChildStats(stats);
	auto &other_child_stats = ListStats::GetChildStats(other);
	child_stats.Merge(other_child_stats, merge_type);
}

void ListStats::Serialize(const BaseStatistics &stats, Serializer &serializer) {
	auto &child_stats = ListStats::GetChildStats(stats);
	serializer.WriteProperty(200, "child_stats", child_stats);
	if (serializer.ShouldSerialize(StorageVersion::V2_1_0)) {
		serializer.WritePropertyWithDefault(201, "min_list_length", MinListLength(stats));
		serializer.WritePropertyWithDefault(202, "max_list_length", MaxListLength(stats));
		serializer.WritePropertyWithDefault(203, "total_list_length", TotalListLength(stats));
	}
}

void ListStats::Deserialize(Deserializer &deserializer, BaseStatistics &base) {
	auto &type = base.GetType();
	D_ASSERT(type.InternalType() == PhysicalType::LIST);
	auto &child_type = ListType::GetChildType(type);

	// Push the logical type of the child type to the deserialization context
	deserializer.Set<const LogicalType &>(child_type);
	base.child_stats[0].Copy(deserializer.ReadProperty<BaseStatistics>(200, "child_stats"));
	deserializer.Unset<LogicalType>();

	auto &list_data = GetDataUnsafe(base);
	auto min_list_length = deserializer.ReadPropertyWithDefault<optional_idx>(201, "min_list_length");
	auto max_list_length = deserializer.ReadPropertyWithDefault<optional_idx>(202, "max_list_length");
	auto total_list_length = deserializer.ReadPropertyWithDefault<optional_idx>(203, "total_list_length");
	list_data.has_min_list_length = min_list_length.IsValid();
	list_data.min_list_length = min_list_length.IsValid() ? min_list_length.GetIndex() : 0;
	list_data.has_max_list_length = max_list_length.IsValid();
	list_data.max_list_length = max_list_length.IsValid() ? max_list_length.GetIndex() : 0;
	list_data.has_total_list_length = total_list_length.IsValid();
	list_data.total_list_length = total_list_length.IsValid() ? total_list_length.GetIndex() : 0;
}

child_list_t<Value> ListStats::ToStruct(const BaseStatistics &stats) {
	auto &child_stats = ListStats::GetChildStats(stats);
	child_list_t<Value> result;
	auto &list_data = GetDataUnsafe(stats);
	if (list_data.HasMinListLength()) {
		result.emplace_back("min_list_length", Value::UBIGINT(list_data.min_list_length));
	}
	if (list_data.has_max_list_length) {
		result.emplace_back("max_list_length", Value::UBIGINT(list_data.max_list_length));
	}
	if (list_data.has_total_list_length) {
		result.emplace_back("total_list_length", Value::UBIGINT(list_data.total_list_length));
	}
	result.emplace_back("child_stats", child_stats.ToStruct());
	return result;
}

void ListStats::Verify(const BaseStatistics &stats, const Vector &vector, const SelectionVector &sel, idx_t count) {
	auto &child_stats = ListStats::GetChildStats(stats);
	const auto &child_entry = ListVector::GetChild(vector);
	auto entries = vector.Values<list_entry_t>();
	auto &list_data = GetDataUnsafe(stats);

	idx_t total_list_count = 0;
	for (idx_t i = 0; i < count; i++) {
		auto idx = sel.get_index(i);
		auto entry = entries[idx];
		if (entry.IsValid()) {
			auto length = entry.GetValue().length;
			// LCOV_EXCL_START
			if (list_data.has_max_list_length && length > list_data.max_list_length) {
				throw InternalException("Statistics mismatch: list length exceeds maximum list length.\nStatistics: "
				                        "%s\nVector: %s",
				                        stats.ToString(), vector.ToString());
			}
			if (list_data.HasMinListLength() && length < list_data.min_list_length) {
				throw InternalException("Statistics mismatch: list length is below minimum list length.\nStatistics: "
				                        "%s\nVector: %s",
				                        stats.ToString(), vector.ToString());
			}
			// LCOV_EXCL_STOP
			total_list_count += length;
		}
	}
	SelectionVector list_sel(total_list_count);
	idx_t list_count = 0;
	for (idx_t i = 0; i < count; i++) {
		auto idx = sel.get_index(i);
		auto entry = entries[idx];
		if (entry.IsValid()) {
			auto list = entry.GetValue();
			for (idx_t list_idx = 0; list_idx < list.length; list_idx++) {
				list_sel.set_index(list_count++, list.offset + list_idx);
			}
		}
	}

	child_stats.Verify(child_entry, list_sel, list_count);
}

} // namespace duckdb
