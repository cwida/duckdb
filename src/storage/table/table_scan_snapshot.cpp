#include "duckdb/storage/table/table_scan_snapshot.hpp"

#include "duckdb/common/helper.hpp"
#include "duckdb/storage/table/row_group_collection.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/transaction/transaction_data.hpp"

namespace duckdb {

TableScanSnapshot::TableScanSnapshot(unique_ptr<const RowGroupCollection> table_p,
                                     unique_ptr<const RowGroupCollection> local_p)
    : table(std::move(table_p)), local(std::move(local_p)) {
	D_ASSERT(table);
}

TableScanSnapshot::~TableScanSnapshot() = default;

idx_t TableScanSnapshot::GetTotalRows() const {
	return table->GetTotalRows() + (local ? local->GetTotalRows() : 0);
}

vector<PartitionStatistics> TableScanSnapshot::GetPartitionStats(TransactionData transaction) const {
	auto result = table->GetPartitionStats(transaction);
	if (local) {
		auto local_stats = local->GetPartitionStats(transaction);
		result.insert(result.end(), local_stats.begin(), local_stats.end());
	}
	return result;
}

void TableScanSnapshot::InitializeParallelScan(ParallelTableScanState &state) const {
	table->InitializeParallelScan(state.scan_state);
	if (local) {
		local->InitializeParallelScan(state.local_state);
	} else {
		state.local_state.collection = nullptr;
		state.local_state.row_groups.reset();
		state.local_state.AssignRowGroup(nullptr);
		state.local_state.vector_index = 0;
		state.local_state.max_row = 0;
		state.local_state.batch_index = 0;
		state.local_state.processed_rows = 0;
	}
}

optional_idx TableScanSnapshot::NextParallelScan(ClientContext &context, ParallelTableScanState &state,
                                                 TableScanState &scan_state, bool initialize_columns) const {
	const auto rows = table->NextParallelScan(context, state.scan_state, scan_state.table_state, initialize_columns);
	if (rows.IsValid() || !local) {
		return rows;
	}
	if (state.scan_state.row_number_base.IsValid()) {
		scan_state.local_state.row_number_base = state.scan_state.row_number_base.GetIndex();
	}
	return local->NextParallelScan(context, state.local_state, scan_state.local_state, initialize_columns);
}

} // namespace duckdb
