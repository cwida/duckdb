#include "duckdb/execution/operator/helper/physical_vacuum.hpp"

#include "duckdb/storage/storage_lock.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/statistics/distinct_statistics.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"

namespace duckdb {

PhysicalVacuum::PhysicalVacuum(PhysicalPlan &physical_plan, unique_ptr<VacuumInfo> info_p,
                               optional_ptr<TableCatalogEntry> table, unordered_map<idx_t, idx_t> column_id_map,
                               idx_t estimated_cardinality)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::VACUUM, {LogicalType::BOOLEAN}, estimated_cardinality),
      info(std::move(info_p)), table(table), column_id_map(std::move(column_id_map)) {
}

namespace {

class VacuumLocalSinkState : public LocalSinkState {
public:
	explicit VacuumLocalSinkState(VacuumInfo &info, optional_ptr<TableCatalogEntry> table) : hashes(LogicalType::HASH) {
		for (const auto &column_name : info.columns) {
			auto &column = table->GetColumn(column_name);
			if (DistinctStatistics::TypeIsSupported(column.GetType())) {
				column_distinct_stats.push_back(make_uniq<DistinctStatistics>());
			} else {
				column_distinct_stats.push_back(nullptr);
			}
		}
	};

	vector<unique_ptr<DistinctStatistics>> column_distinct_stats;
	Vector hashes;
};

class VacuumGlobalSinkState : public GlobalSinkState {
public:
	explicit VacuumGlobalSinkState(VacuumInfo &info, optional_ptr<TableCatalogEntry> table) {
		for (const auto &column_name : info.columns) {
			auto &column = table->GetColumn(column_name);
			if (DistinctStatistics::TypeIsSupported(column.GetType())) {
				column_distinct_stats.push_back(make_uniq<DistinctStatistics>());
			} else {
				column_distinct_stats.push_back(nullptr);
			}
		}
	};

	mutex stats_lock;
	vector<unique_ptr<DistinctStatistics>> column_distinct_stats;
};

} // namespace

unique_ptr<LocalSinkState> PhysicalVacuum::GetLocalSinkState(ExecutionContext &context) const {
	return make_uniq<VacuumLocalSinkState>(*info, table);
}

unique_ptr<GlobalSinkState> PhysicalVacuum::GetGlobalSinkState(ClientContext &context) const {
	return make_uniq<VacuumGlobalSinkState>(*info, table);
}

SinkResultType PhysicalVacuum::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
	auto &lstate = input.local_state.Cast<VacuumLocalSinkState>();
	D_ASSERT(lstate.column_distinct_stats.size() == column_id_map.size());

	for (idx_t col_idx = 0; col_idx < chunk.data.size(); col_idx++) {
		if (!DistinctStatistics::TypeIsSupported(chunk.data[col_idx].GetType())) {
			continue;
		}
		lstate.column_distinct_stats[col_idx]->Update(chunk.data[col_idx], chunk.size(), lstate.hashes);
	}

	return SinkResultType::NEED_MORE_INPUT;
}

SinkCombineResultType PhysicalVacuum::Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const {
	auto &g_state = input.global_state.Cast<VacuumGlobalSinkState>();
	auto &l_state = input.local_state.Cast<VacuumLocalSinkState>();

	lock_guard<mutex> lock(g_state.stats_lock);
	D_ASSERT(g_state.column_distinct_stats.size() == l_state.column_distinct_stats.size());

	for (idx_t col_idx = 0; col_idx < g_state.column_distinct_stats.size(); col_idx++) {
		if (g_state.column_distinct_stats[col_idx]) {
			D_ASSERT(l_state.column_distinct_stats[col_idx]);
			g_state.column_distinct_stats[col_idx]->Merge(*l_state.column_distinct_stats[col_idx]);
		}
	}

	return SinkCombineResultType::FINISHED;
}

SinkFinalizeType PhysicalVacuum::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                                          OperatorSinkFinalizeInput &input) const {
	auto &sink = input.global_state.Cast<VacuumGlobalSinkState>();
	if (!table->IsDuckTable()) {
		throw NotImplementedException("Vacuum is only implemented for DuckDB tables");
	}

	auto tbl = table;
	// Scan tasks have released their local state; only the live mutations need checkpoint exclusion.
	auto checkpoint_lock = DuckTransaction::Get(context, tbl->ParentCatalog()).GetCheckpointLockForStatement();
	for (idx_t col_idx = 0; col_idx < sink.column_distinct_stats.size(); col_idx++) {
		tbl->GetStorage().SetDistinct(column_id_map.at(col_idx), std::move(sink.column_distinct_stats[col_idx]));
	}
	if (tbl) {
		tbl->GetStorage().VacuumIndexes();
	}

	return SinkFinalizeType::READY;
}

SourceResultType PhysicalVacuum::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                 OperatorSourceInput &input) const {
	// NOP
	return SourceResultType::FINISHED;
}

} // namespace duckdb
