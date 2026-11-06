#include "duckdb/common/vector/map_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/storage/statistics/struct_stats.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/storage/statistics/stats_struct_fields.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/vector.hpp"

#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/storage/storage_index.hpp"
#include "duckdb/function/cast/cast_statistics.hpp"

namespace duckdb {

void StructStats::Construct(BaseStatistics &stats) {
	auto &child_types = StructType::GetChildTypes(stats.GetType());
	stats.child_stats = unsafe_unique_array<BaseStatistics>(new BaseStatistics[child_types.size()]);
	for (idx_t i = 0; i < child_types.size(); i++) {
		BaseStatistics::Construct(stats.child_stats[i], child_types[i].second);
	}
}

BaseStatistics StructStats::CreateUnknown(LogicalType type) {
	auto &child_types = StructType::GetChildTypes(type);
	BaseStatistics result(std::move(type));
	result.InitializeUnknown();
	for (idx_t i = 0; i < child_types.size(); i++) {
		result.child_stats[i].Copy(BaseStatistics::CreateUnknown(child_types[i].second));
	}
	return result;
}

BaseStatistics StructStats::CreateEmpty(LogicalType type) {
	auto &child_types = StructType::GetChildTypes(type);
	BaseStatistics result(std::move(type));
	result.InitializeEmpty();
	for (idx_t i = 0; i < child_types.size(); i++) {
		result.child_stats[i].Copy(BaseStatistics::CreateEmpty(child_types[i].second));
	}
	return result;
}

const BaseStatistics *StructStats::GetChildStats(const BaseStatistics &stats) {
	if (stats.GetStatsType() != StatisticsType::STRUCT_STATS) {
		throw InternalException("Calling StructStats::GetChildStats on stats that is not a struct");
	}
	return stats.child_stats.get();
}

const BaseStatistics &StructStats::GetChildStats(const BaseStatistics &stats, idx_t i) {
	D_ASSERT(stats.GetStatsType() == StatisticsType::STRUCT_STATS);
	if (i >= StructType::GetChildCount(stats.GetType())) {
		throw InternalException("Calling StructStats::GetChildStats but there are no stats for this index");
	}
	return stats.child_stats[i];
}

BaseStatistics &StructStats::GetChildStats(BaseStatistics &stats, idx_t i) {
	D_ASSERT(stats.GetStatsType() == StatisticsType::STRUCT_STATS);
	if (i >= StructType::GetChildCount(stats.GetType())) {
		throw InternalException("Calling StructStats::GetChildStats but there are no stats for this index");
	}
	return stats.child_stats[i];
}

void StructStats::SetChildStats(BaseStatistics &stats, idx_t i, const BaseStatistics &new_stats) {
	D_ASSERT(stats.GetStatsType() == StatisticsType::STRUCT_STATS);
	D_ASSERT(i < StructType::GetChildCount(stats.GetType()));
	stats.child_stats[i].Copy(new_stats);
}

void StructStats::SetChildStats(BaseStatistics &stats, idx_t i, unique_ptr<BaseStatistics> new_stats) {
	D_ASSERT(stats.GetStatsType() == StatisticsType::STRUCT_STATS);
	if (!new_stats) {
		StructStats::SetChildStats(stats, i,
		                           BaseStatistics::CreateUnknown(StructType::GetChildType(stats.GetType(), i)));
	} else {
		StructStats::SetChildStats(stats, i, *new_stats);
	}
}

void StructStats::Copy(BaseStatistics &stats, const BaseStatistics &other) {
	auto count = StructType::GetChildCount(stats.GetType());
	for (idx_t i = 0; i < count; i++) {
		stats.child_stats[i].Copy(other.child_stats[i]);
	}
}

void StructStats::Merge(BaseStatistics &stats, const BaseStatistics &other, StatsMergeType merge_type) {
	if (other.GetType().id() == LogicalTypeId::VALIDITY) {
		return;
	}
	D_ASSERT(stats.GetType().id() == other.GetType().id());
	D_ASSERT(StructType::GetChildCount(stats.GetType()) == StructType::GetChildCount(other.GetType()));
	auto child_count = StructType::GetChildCount(stats.GetType());
	for (idx_t i = 0; i < child_count; i++) {
		stats.child_stats[i].Merge(other.child_stats[i], merge_type);
	}
}

void StructStats::Serialize(const BaseStatistics &stats, Serializer &serializer) {
	auto child_stats = StructStats::GetChildStats(stats);
	auto child_count = StructType::GetChildCount(stats.GetType());

	serializer.WriteList(200, "child_stats", child_count,
	                     [&](Serializer::List &list, idx_t i) { list.WriteElement(child_stats[i]); });
}

void StructStats::Deserialize(Deserializer &deserializer, BaseStatistics &base) {
	auto &type = base.GetType();
	D_ASSERT(type.InternalType() == PhysicalType::STRUCT);

	auto &child_types = StructType::GetChildTypes(type);

	deserializer.ReadList(200, "child_stats", [&](Deserializer::List &list, idx_t i) {
		deserializer.Set<const LogicalType &>(child_types[i].second);
		auto stat = list.ReadElement<BaseStatistics>();
		base.child_stats[i].Copy(stat);
		deserializer.Unset<LogicalType>();
	});
}

child_list_t<Value> StructStats::ToStruct(const BaseStatistics &stats, bool exact) {
	child_list_t<Value> result;
	child_list_t<Value> child_info;
	auto &child_types = StructType::GetChildTypes(stats.GetType());
	for (idx_t i = 0; i < child_types.size(); i++) {
		child_info.emplace_back(child_types[i].first, stats.child_stats[i].ToStruct(exact));
	}
	result.emplace_back("child_stats", Value::STRUCT(std::move(child_info)));
	return result;
}

//! The children of a TUPLE's `child_stats` are positional: a TUPLE as ToStruct renders it, or a LIST as stats() does
static void SetTupleChildrenFromValue(BaseStatistics &stats, const Value &children, StatsStructFields &fields) {
	auto plain = StatsStructFields::Plain(children);
	optional_ptr<const vector<Value>> values;
	switch (plain.type().id()) {
	case LogicalTypeId::TUPLE:
		values = StructValue::GetChildren(plain);
		break;
	case LogicalTypeId::LIST:
		values = ListValue::GetChildren(plain);
		break;
	default:
		fields.Throw("child_stats", StringUtil::Format("expected a tuple of statistics, got a value of type %s",
		                                               plain.type().ToString()));
	}
	auto count = StructType::GetChildCount(stats.GetType());
	if (values->size() != count) {
		fields.Throw("child_stats",
		             StringUtil::Format("expected the statistics of %llu elements, got %llu", count, values->size()));
	}
	auto path = fields.FieldPath("child_stats");
	for (idx_t i = 0; i < count; i++) {
		auto &child = (*values)[i];
		if (child.IsNull()) {
			continue;
		}
		StructStats::GetChildStats(stats, i).SetFromStruct(child, path + "." + TupleType::GetChildName(i));
	}
}

void StructStats::SetFromStruct(BaseStatistics &stats, StatsStructFields &fields) {
	auto children = fields.Get("child_stats");
	if (!children) {
		return;
	}
	if (stats.GetType().id() == LogicalTypeId::TUPLE) {
		SetTupleChildrenFromValue(stats, *children, fields);
		return;
	}
	StatsStructFields child_fields(*children, fields.FieldPath("child_stats"));
	auto &child_types = StructType::GetChildTypes(stats.GetType());
	for (idx_t i = 0; i < child_types.size(); i++) {
		auto &name = child_types[i].first.GetIdentifierName();
		auto child = child_fields.Get(name);
		if (child) {
			stats.child_stats[i].SetFromStruct(*child, child_fields.FieldPath(name));
		}
	}
	child_fields.VerifyAllRead(stats.GetType());
}

void StructStats::Verify(const BaseStatistics &stats, const Vector &vector, const SelectionVector &sel, idx_t count) {
	const auto &child_entries = StructVector::GetEntries(vector);
	for (idx_t i = 0; i < child_entries.size(); i++) {
		stats.child_stats[i].Verify(child_entries[i], sel, count, true);
	}
}

unique_ptr<BaseStatistics> StructStats::PushdownExtract(const BaseStatistics &stats, const StorageIndex &index) {
	D_ASSERT(index.GetPrimaryIndex() < StructType::GetChildCount(stats.type));
	auto child_index = index.GetPrimaryIndex();
	auto &child_types = StructType::GetChildTypes(stats.type);

	auto &child_stats = GetChildStats(stats, child_index);
	auto &child_type = child_types[child_index].second;

	auto &child_indexes = index.GetChildIndexes();
	if (child_indexes.empty()) {
		D_ASSERT(child_stats.type == child_type);
		if (index.GetType() != child_type) {
			//! FIXME: support try_cast
			return CastStatistics::TryPropagate(child_stats, child_type, index.GetType());
		} else {
			return child_stats.ToUnique();
		}
	} else {
		D_ASSERT(child_indexes.size() == 1);
		auto &child_index = child_indexes[0];
		return child_stats.PushdownExtract(child_index);
	}
}

} // namespace duckdb
