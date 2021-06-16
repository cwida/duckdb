//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/join/perfect_hash_join_executor.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/row_operations/row_operations.hpp"
#include "duckdb/execution/join_hashtable.hpp"

namespace duckdb {
constexpr size_t BUILD_THRESHOLD = 1 << 14; // 16384
constexpr size_t MIN_THRESHOLD = 1 << 7;    // 128

class PhysicalHashJoinState;
class HashJoinGlobalState;

struct PerfectHashJoinStats {
	Value build_min;
	Value build_max;
	Value probe_min;
	Value probe_max;
	bool is_build_small {false};
	bool is_probe_in_range {false};
	bool is_build_min_small {false};
	bool is_build_dense {false};
	idx_t range {0};
	idx_t estimated_cardinality {0};
};

//! PhysicalHashJoin represents a hash loop join between two tables
class PerfectHashJoinExecutor {
public:
	PerfectHashJoinExecutor(PerfectHashJoinStats pjoin_stats);
	using PerfectHashTable = std::vector<Vector>;
	bool ProbePerfectHashTable(ExecutionContext &context, DataChunk &chunk, PhysicalHashJoinState *state,
	                           JoinHashTable *ht_ptr, PhysicalOperator *operator_child);
	bool CheckForPerfectHashJoin(JoinHashTable *ht_ptr);
	void BuildPerfectHashTable(JoinHashTable *ht_ptr, JoinHTScanState &join_ht_state, LogicalType type);
	void FillSelectionVectorSwitch(Vector &source, SelectionVector &sel_vec, idx_t count);
	template <typename T>
	void TemplatedFillSelectionVector(Vector &source, SelectionVector &sel_vec, idx_t count);
	void FullScanHashTable(JoinHTScanState &state, LogicalType key_type, JoinHashTable *hash_table);

private:
	bool hasInvisibleJoin {false};
	PerfectHashTable perfect_hash_table;
	PerfectHashJoinStats pjoin_stats;
};

} // namespace duckdb