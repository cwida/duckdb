#include "duckdb/common/vector/array_vector.hpp"
#include "duckdb/storage/statistics/array_stats.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/storage/statistics/stats_struct_fields.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/serializer/deserializer.hpp"

namespace duckdb {

void ArrayStats::Construct(BaseStatistics &stats) {
	stats.child_stats = unsafe_unique_array<BaseStatistics>(new BaseStatistics[1]);
	BaseStatistics::Construct(stats.child_stats[0], ArrayType::GetChildType(stats.GetType()));
}

BaseStatistics ArrayStats::CreateUnknown(LogicalType type) {
	auto &child_type = ArrayType::GetChildType(type);
	BaseStatistics result(std::move(type));
	result.InitializeUnknown();
	result.child_stats[0].Copy(BaseStatistics::CreateUnknown(child_type));
	return result;
}

BaseStatistics ArrayStats::CreateEmpty(LogicalType type) {
	auto &child_type = ArrayType::GetChildType(type);
	BaseStatistics result(std::move(type));
	result.InitializeEmpty();
	result.child_stats[0].Copy(BaseStatistics::CreateEmpty(child_type));
	return result;
}

void ArrayStats::Copy(BaseStatistics &stats, const BaseStatistics &other) {
	D_ASSERT(stats.child_stats);
	D_ASSERT(other.child_stats);
	stats.child_stats[0].Copy(other.child_stats[0]);
}

const BaseStatistics &ArrayStats::GetChildStats(const BaseStatistics &stats) {
	if (stats.GetStatsType() != StatisticsType::ARRAY_STATS) {
		throw InternalException("ArrayStats::GetChildStats called on stats that is not a array");
	}
	D_ASSERT(stats.child_stats);
	return stats.child_stats[0];
}
BaseStatistics &ArrayStats::GetChildStats(BaseStatistics &stats) {
	if (stats.GetStatsType() != StatisticsType::ARRAY_STATS) {
		throw InternalException("ArrayStats::GetChildStats called on stats that is not a array");
	}
	D_ASSERT(stats.child_stats);
	return stats.child_stats[0];
}

void ArrayStats::SetChildStats(BaseStatistics &stats, unique_ptr<BaseStatistics> new_stats) {
	if (!new_stats) {
		stats.child_stats[0].Copy(BaseStatistics::CreateUnknown(ArrayType::GetChildType(stats.GetType())));
	} else {
		stats.child_stats[0].Copy(*new_stats);
	}
}

void ArrayStats::Merge(BaseStatistics &stats, const BaseStatistics &other, StatsMergeType merge_type) {
	if (other.GetType().id() == LogicalTypeId::VALIDITY) {
		return;
	}

	auto &child_stats = ArrayStats::GetChildStats(stats);
	auto &other_child_stats = ArrayStats::GetChildStats(other);
	child_stats.Merge(other_child_stats, merge_type);
}

void ArrayStats::Serialize(const BaseStatistics &stats, Serializer &serializer) {
	auto &child_stats = ArrayStats::GetChildStats(stats);
	serializer.WriteProperty(200, "child_stats", child_stats);
}

void ArrayStats::Deserialize(Deserializer &deserializer, BaseStatistics &base) {
	auto &type = base.GetType();
	D_ASSERT(type.id() == LogicalTypeId::ARRAY);
	auto &child_type = ArrayType::GetChildType(type);

	// Push the logical type of the child type to the deserialization context
	deserializer.Set<const LogicalType &>(child_type);
	base.child_stats[0].Copy(deserializer.ReadProperty<BaseStatistics>(200, "child_stats"));
	deserializer.Unset<LogicalType>();
}

child_list_t<Value> ArrayStats::ToStruct(const BaseStatistics &stats, bool exact) {
	auto &child_stats = ArrayStats::GetChildStats(stats);
	child_list_t<Value> result;
	result.emplace_back("child_stats", child_stats.ToStruct(exact));
	return result;
}

void ArrayStats::SetFromStruct(BaseStatistics &stats, StatsStructFields &fields) {
	auto child = fields.Get("child_stats");
	if (child) {
		GetChildStats(stats).SetFromStruct(*child, fields.FieldPath("child_stats"));
	}
}

void ArrayStats::Verify(const BaseStatistics &stats, const Vector &vector, const SelectionVector &sel, idx_t count) {
	auto &child_stats = ArrayStats::GetChildStats(stats);
	const auto &child_entry = ArrayVector::GetChild(vector);
	auto array_size = ArrayType::GetSize(vector.GetType());

	UnifiedVectorFormat vdata;
	vector.ToUnifiedFormat(vdata);

	// Basically,
	// 1. Count the number of valid arrays
	// 2. Create a selection vector with the size of the number of valid arrays * array_size
	// 3. Fill the selection vector with the offsets of all the elements in the child vector
	//      that exist in each valid array
	// 4. Use that selection vector to verify the child stats

	idx_t valid_count = 0;
	for (idx_t i = 0; i < count; i++) {
		auto idx = sel.get_index(i);
		auto index = vdata.sel->get_index(idx);
		if (vdata.validity.RowIsValid(index)) {
			valid_count++;
		}
	}

	SelectionVector element_sel(valid_count * array_size);
	idx_t element_count = 0;
	for (idx_t i = 0; i < count; i++) {
		auto idx = sel.get_index(i);
		auto index = vdata.sel->get_index(idx);
		auto offset = index * array_size;
		if (vdata.validity.RowIsValid(index)) {
			for (idx_t elem_idx = 0; elem_idx < array_size; elem_idx++) {
				element_sel.set_index(element_count++, offset + elem_idx);
			}
		}
	}

	child_stats.Verify(child_entry, element_sel, element_count);
}

} // namespace duckdb
