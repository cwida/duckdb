#include "duckdb/storage/table/table_statistics.hpp"

#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/execution/reservoir_sample.hpp"
#include "duckdb/storage/table/persistent_table_data.hpp"
#include "duckdb/parser/column_list.hpp"

namespace duckdb {

void TableStatistics::Initialize(const vector<LogicalType> &types, PersistentTableData &data) {
	D_ASSERT(Empty());
	D_ASSERT(!table_sample);

	stats_lock = make_shared_ptr<mutex>();
	column_stats = std::move(data.table_stats.column_stats);
	if (data.table_stats.table_sample) {
		table_sample = std::move(data.table_stats.table_sample);
	} else {
		table_sample = make_uniq<ReservoirSample>(static_cast<idx_t>(FIXED_SAMPLE_SIZE));
	}
	if (column_stats.size() != types.size()) { // LCOV_EXCL_START
		throw DataCorruptionException(
		    "Table statistics column count is not aligned with table column count. Corrupt file?");
	} // LCOV_EXCL_STOP
}

void TableStatistics::InitializeEmpty(const TableStatistics &other) {
	D_ASSERT(Empty());
	D_ASSERT(!table_sample);
	D_ASSERT(other.stats_lock);

	// hold other's lock while reading its stats - a concurrent ANALYZE/VACUUM can replace them (e.g. SetDistinct)
	lock_guard<mutex> l(*other.stats_lock);
	stats_lock = make_shared_ptr<mutex>();
	if (other.table_sample) {
		D_ASSERT(other.table_sample->type == SampleType::RESERVOIR_SAMPLE);
		auto &res = other.table_sample->Cast<ReservoirSample>();
		table_sample = res.Copy();
	} else {
		table_sample = make_uniq<ReservoirSample>(static_cast<idx_t>(FIXED_SAMPLE_SIZE));
	}

	for (auto &stats : other.column_stats) {
		auto new_column_stats = ColumnStatistics::CreateEmptyStats(stats->Statistics().GetType());
		if (stats->HasDistinctStats()) {
			new_column_stats->SetDistinct(stats->DistinctStats().Copy());
		}

		auto &base_stats = new_column_stats->Statistics();
		if (new_column_stats->HasDistinctStats()) {
			base_stats.SetDistinctCount(new_column_stats->DistinctStats().GetCount());
		}
		column_stats.push_back(new_column_stats);
	}
}

void TableStatistics::InitializeEmpty(const vector<LogicalType> &types) {
	D_ASSERT(Empty());
	D_ASSERT(!table_sample);

	stats_lock = make_shared_ptr<mutex>();
	table_sample = make_uniq<ReservoirSample>(static_cast<idx_t>(FIXED_SAMPLE_SIZE));
	for (auto &type : types) {
		column_stats.push_back(ColumnStatistics::CreateEmptyStats(type));
	}
}

void TableStatistics::InitializeAddColumn(TableStatistics &parent, const LogicalType &new_column_type) {
	D_ASSERT(Empty());
	D_ASSERT(parent.stats_lock);

	stats_lock = parent.stats_lock;
	lock_guard<mutex> lock(*stats_lock);
	for (idx_t i = 0; i < parent.column_stats.size(); i++) {
		column_stats.push_back(parent.column_stats[i]);
	}
	column_stats.push_back(ColumnStatistics::CreateEmptyStats(new_column_type));
	if (parent.table_sample) {
		table_sample = std::move(parent.table_sample);
	}
	if (table_sample) {
		table_sample->Destroy();
	}
}

void TableStatistics::InitializeRemoveColumn(TableStatistics &parent, idx_t removed_column) {
	D_ASSERT(Empty());
	D_ASSERT(parent.stats_lock);

	stats_lock = parent.stats_lock;
	lock_guard<mutex> lock(*stats_lock);
	for (idx_t i = 0; i < parent.column_stats.size(); i++) {
		if (i != removed_column) {
			column_stats.push_back(parent.column_stats[i]);
		}
	}
	if (parent.table_sample) {
		table_sample = std::move(parent.table_sample);
	}
	if (table_sample) {
		table_sample->Destroy();
	}
}

void TableStatistics::InitializeAlterType(TableStatistics &parent, idx_t changed_idx, const LogicalType &new_type) {
	D_ASSERT(Empty());
	D_ASSERT(parent.stats_lock);

	stats_lock = parent.stats_lock;
	lock_guard<mutex> lock(*stats_lock);
	for (idx_t i = 0; i < parent.column_stats.size(); i++) {
		if (i == changed_idx) {
			column_stats.push_back(ColumnStatistics::CreateEmptyStats(new_type));
		} else {
			column_stats.push_back(parent.column_stats[i]);
		}
	}
	if (parent.table_sample) {
		table_sample = std::move(parent.table_sample);
	}
	if (table_sample) {
		table_sample->Destroy();
	}
}

void TableStatistics::InitializeAddConstraint(TableStatistics &parent) {
	D_ASSERT(Empty());
	D_ASSERT(parent.stats_lock);

	stats_lock = parent.stats_lock;
	lock_guard<mutex> lock(*stats_lock);
	for (idx_t i = 0; i < parent.column_stats.size(); i++) {
		column_stats.push_back(parent.column_stats[i]);
	}
	if (parent.table_sample) {
		table_sample = std::move(parent.table_sample);
	}
}

void TableStatistics::MergeStats(TableStatistics &other) {
	auto l = GetLock();
	D_ASSERT(column_stats.size() == other.column_stats.size());
	if (table_sample) {
		if (other.table_sample) {
			D_ASSERT(table_sample->type == SampleType::RESERVOIR_SAMPLE);
			auto &this_reservoir = table_sample->Cast<ReservoirSample>();
			D_ASSERT(other.table_sample->type == SampleType::RESERVOIR_SAMPLE);
			this_reservoir.Merge(std::move(other.table_sample));
		}
		// if no other.table sample, do nothing
	} else {
		if (other.table_sample) {
			auto &other_reservoir = other.table_sample->Cast<ReservoirSample>();
			auto other_table_sample_copy = other_reservoir.Copy();
			table_sample = std::move(other_table_sample_copy);
		}
	}
	for (idx_t i = 0; i < column_stats.size(); i++) {
		if (column_stats[i]) {
			D_ASSERT(other.column_stats[i]);
			column_stats[i]->Merge(*other.column_stats[i]);
		}
	}
}

void TableStatistics::MergeStats(idx_t i, BaseStatistics &stats) {
	auto l = GetLock();
	MergeStats(*l, i, stats);
}

void TableStatistics::MergeStats(TableStatisticsLock &lock, idx_t i, BaseStatistics &stats, StatsMergeType merge_type) {
	column_stats[i]->Statistics().Merge(stats, merge_type);
}

ColumnStatistics &TableStatistics::GetStats(TableStatisticsLock &lock, idx_t i) {
	return *column_stats[i];
}

// BlockingSample &TableStatistics::GetTableSampleRef(TableStatisticsLock &lock) {
//	D_ASSERT(table_sample);
//	return *table_sample;
//}

unique_ptr<BlockingSample> TableStatistics::GetTableSample(TableStatisticsLock &lock) {
	return std::move(table_sample);
}

void TableStatistics::SetTableSample(TableStatisticsLock &lock, unique_ptr<BlockingSample> sample) {
	table_sample = std::move(sample);
}

void TableStatistics::DestroyTableSample(TableStatisticsLock &lock) const {
	if (table_sample) {
		table_sample->Destroy();
	}
}

unique_ptr<BaseStatistics> TableStatistics::CopyStats(const StorageIndex &index) {
	lock_guard<mutex> l(*stats_lock);

	auto column_index = index.GetPrimaryIndex();
	auto &stats = *column_stats[column_index];
	auto result = stats.Statistics().Copy();
	if (stats.HasDistinctStats()) {
		result.SetDistinctCount(stats.DistinctStats().GetCount());
	}
	if (index.IsPushdownExtract()) {
		return result.PushdownExtract(index.GetChildIndexes()[0]);
	}
	return result.ToUnique();
}

void TableStatistics::CopyStats(TableStatistics &other) {
	TableStatisticsLock lock(*stats_lock);
	CopyStats(lock, other);
}

void TableStatistics::CopyStats(TableStatisticsLock &lock, TableStatistics &other) {
	D_ASSERT(other.Empty());
	other.stats_lock = make_shared_ptr<mutex>();
	for (auto &stats : column_stats) {
		other.column_stats.push_back(stats->Copy());
	}

	if (table_sample) {
		D_ASSERT(table_sample->type == SampleType::RESERVOIR_SAMPLE);
		auto &res = table_sample->Cast<ReservoirSample>();
		other.table_sample = res.Copy();
	}
}

void TableStatistics::Serialize(Serializer &serializer) const {
	serializer.WriteProperty(100, "column_stats", column_stats);
	unique_ptr<BlockingSample> to_serialize = nullptr;
	if (table_sample) {
		D_ASSERT(table_sample->type == SampleType::RESERVOIR_SAMPLE);
		auto &reservoir_sample = table_sample->Cast<ReservoirSample>();
		to_serialize = unique_ptr_cast<BlockingSample, ReservoirSample>(reservoir_sample.Copy());
		auto &res_serialize = to_serialize->Cast<ReservoirSample>();
		res_serialize.EvictOverBudgetSamples();
	}
	serializer.WritePropertyWithDefault<unique_ptr<BlockingSample>>(101, "table_sample", to_serialize, nullptr);
}

void TableStatistics::Deserialize(Deserializer &deserializer, ColumnList &columns) {
	auto physical_columns = columns.Physical();

	auto iter = physical_columns.begin();
	deserializer.ReadList(100, "column_stats", [&](Deserializer::List &list, idx_t i) {
		auto &col = *iter;
		iter.operator++();

		auto type = col.GetType();
		deserializer.Set<LogicalType &>(type);

		column_stats.push_back(list.ReadElement<shared_ptr<ColumnStatistics>>());

		deserializer.Unset<LogicalType>();
	});
	table_sample = deserializer.ReadPropertyWithDefault<unique_ptr<BlockingSample>>(101, "table_sample");
	if (table_sample) {
		D_ASSERT(table_sample->type == SampleType::RESERVOIR_SAMPLE);
#ifdef DEBUG
		if (table_sample) {
			auto &reservoir_sample = table_sample->Cast<ReservoirSample>();
			reservoir_sample.Verify();
		}
#endif
	} else {
		table_sample = make_uniq<ReservoirSample>(static_cast<idx_t>(FIXED_SAMPLE_SIZE));
		table_sample->Destroy();
	}
}

unique_ptr<TableStatisticsLock> TableStatistics::GetLock() {
	D_ASSERT(stats_lock);
	return make_uniq<TableStatisticsLock>(*stats_lock);
}

bool TableStatistics::Empty() const {
	D_ASSERT(column_stats.empty() == (stats_lock.get() == nullptr));
	return column_stats.empty();
}

} // namespace duckdb
