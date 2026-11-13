//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/table/table_scan_snapshot.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/function/partition_stats.hpp"

namespace duckdb {
class ClientContext;
class RowGroupCollection;
class TableScanState;
struct ParallelTableScanState;
struct TransactionData;

class TableScanSnapshot {
public:
	TableScanSnapshot(unique_ptr<const RowGroupCollection> table, unique_ptr<const RowGroupCollection> local);
	~TableScanSnapshot();

public:
	idx_t GetTotalRows() const;
	vector<PartitionStatistics> GetPartitionStats(TransactionData transaction) const;
	void InitializeParallelScan(ParallelTableScanState &state) const;
	optional_idx NextParallelScan(ClientContext &context, ParallelTableScanState &state, TableScanState &scan_state,
	                              bool initialize_columns = true) const;

private:
	unique_ptr<const RowGroupCollection> table;
	unique_ptr<const RowGroupCollection> local;
};

} // namespace duckdb
