//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/join/outer_join_marker.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/atomic.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/execution/operator/join/physical_comparison_join.hpp"
#include "duckdb/execution/physical_operator.hpp"

namespace duckdb {

struct OuterJoinGlobalScanState {
	mutex lock;
	ColumnDataCollection *data = nullptr;
	ColumnDataParallelScanState global_scan;
	atomic<idx_t> rows_scanned {0};

	ProgressData GetProgress() const;
};

struct OuterJoinLocalScanState {
	void Reset();

	DataChunk scan_chunk;
	SelectionVector match_sel;
	ColumnDataLocalScanState local_scan;
};

class OuterJoinMarker {
public:
	explicit OuterJoinMarker(bool enabled);

	bool Enabled() const {
		return enabled;
	}
	//! Initializes the outer join counter
	void Initialize(idx_t count);
	//! Resets the outer join counter
	void Reset();

	//! Sets an indiivdual match
	void SetMatch(idx_t position);

	//! Sets multiple matches
	void SetMatches(const SelectionVector &sel, idx_t count, idx_t base_idx = 0);

	//! Constructs a left-join result based on which tuples have not found matches
	void ConstructLeftJoinResult(DataChunk &left, DataChunk &result);

	//! Returns the maximum number of threads that can be associated with an right-outer join scan
	idx_t MaxThreads() const;

	//! Initialize a scan
	void InitializeScan(ColumnDataCollection &data, OuterJoinGlobalScanState &gstate);

	//! Initialize a local scan
	void InitializeScan(OuterJoinGlobalScanState &gstate, OuterJoinLocalScanState &lstate);

	//! Perform the scan
	void Scan(OuterJoinGlobalScanState &gstate, OuterJoinLocalScanState &lstate, DataChunk &result);

	//! Read-only matches vector
	const bool *GetMatches() const {
		return found_match.get();
	}

private:
	bool enabled;
	unsafe_unique_array<bool> found_match;
	idx_t count;
};

} // namespace duckdb
