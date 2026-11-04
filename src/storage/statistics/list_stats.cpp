#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/storage/statistics/list_stats.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/storage/statistics/stats_struct_fields.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/vector.hpp"

#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/serializer/deserializer.hpp"

namespace duckdb {

void ListStats::Construct(BaseStatistics &stats) {
	stats.child_stats = unsafe_unique_array<BaseStatistics>(new BaseStatistics[1]);
	BaseStatistics::Construct(stats.child_stats[0], ListType::GetChildType(stats.GetType()));
}

BaseStatistics ListStats::CreateUnknown(LogicalType type) {
	auto &child_type = ListType::GetChildType(type);
	BaseStatistics result(std::move(type));
	result.InitializeUnknown();
	result.child_stats[0].Copy(BaseStatistics::CreateUnknown(child_type));
	return result;
}

BaseStatistics ListStats::CreateEmpty(LogicalType type) {
	auto &child_type = ListType::GetChildType(type);
	BaseStatistics result(std::move(type));
	result.InitializeEmpty();
	result.child_stats[0].Copy(BaseStatistics::CreateEmpty(child_type));
	return result;
}

void ListStats::Copy(BaseStatistics &stats, const BaseStatistics &other) {
	D_ASSERT(stats.child_stats);
	D_ASSERT(other.child_stats);
	stats.child_stats[0].Copy(other.child_stats[0]);
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

	auto &child_stats = ListStats::GetChildStats(stats);
	auto &other_child_stats = ListStats::GetChildStats(other);
	child_stats.Merge(other_child_stats, merge_type);
}

void ListStats::Serialize(const BaseStatistics &stats, Serializer &serializer) {
	auto &child_stats = ListStats::GetChildStats(stats);
	serializer.WriteProperty(200, "child_stats", child_stats);
}

void ListStats::Deserialize(Deserializer &deserializer, BaseStatistics &base) {
	auto &type = base.GetType();
	D_ASSERT(type.InternalType() == PhysicalType::LIST);
	auto &child_type = ListType::GetChildType(type);

	// Push the logical type of the child type to the deserialization context
	deserializer.Set<const LogicalType &>(child_type);
	base.child_stats[0].Copy(deserializer.ReadProperty<BaseStatistics>(200, "child_stats"));
	deserializer.Unset<LogicalType>();
}

child_list_t<Value> ListStats::ToStruct(const BaseStatistics &stats) {
	auto &child_stats = ListStats::GetChildStats(stats);
	child_list_t<Value> result;
	result.emplace_back("child_stats", child_stats.ToStruct());
	return result;
}

void ListStats::SetFromStruct(BaseStatistics &stats, StatsStructFields &fields) {
	auto child = fields.Get("child_stats");
	if (child) {
		GetChildStats(stats).SetFromStruct(*child, fields.FieldPath("child_stats"));
	}
}

void ListStats::Verify(const BaseStatistics &stats, const Vector &vector, const SelectionVector &sel, idx_t count) {
	auto &child_stats = ListStats::GetChildStats(stats);
	const auto &child_entry = ListVector::GetChild(vector);
	auto entries = vector.Values<list_entry_t>();

	idx_t total_list_count = 0;
	for (idx_t i = 0; i < count; i++) {
		auto idx = sel.get_index(i);
		auto entry = entries[idx];
		if (entry.IsValid()) {
			total_list_count += entry.GetValue().length;
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
