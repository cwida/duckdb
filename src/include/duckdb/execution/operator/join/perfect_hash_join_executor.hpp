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

class PhysicalHashJoinState;
class HashJoinGlobalState;

struct PerfectHashJoinStats {
	Value build_min;
	Value build_max;
	Value probe_min;
	Value probe_max;
	bool is_build_small {false};
	bool is_build_dense {false};
	bool is_probe_in_domain {false};
	idx_t build_range {0};
	idx_t estimated_cardinality {0};
};

//! PhysicalHashJoin represents a hash loop join between two tables
class PerfectHashJoinExecutor {
public:
	PerfectHashJoinExecutor(PerfectHashJoinStats pjoin_stats);
	using PerfectHashTable = std::vector<Vector>;
	bool ProbePerfectHashTable(ExecutionContext &context, DataChunk &chunk, PhysicalHashJoinState *state,
	                           JoinHashTable *ht_ptr, PhysicalOperator *operator_child);
	bool CanDoPerfectHashJoin();
	void BuildPerfectHashTable(JoinHashTable *ht_ptr, JoinHTScanState &join_ht_state, LogicalType type);
	void FillSelectionVectorSwitchProbe(Vector &source, SelectionVector &build_sel_vec, SelectionVector &probe_sel_vec,
	                                    idx_t count);
	template <typename T>
	void TemplatedFillSelectionVectorProbe(Vector &source, SelectionVector &build_sel_vec,
	                                       SelectionVector &probe_sel_vec, idx_t count);
	void FillSelectionVectorSwitchBuild(Vector &source, SelectionVector &sel_vec, SelectionVector &seq_sel_vec,
	                                    idx_t count);
	template <typename T>
	void TemplatedFillSelectionVectorBuild(Vector &source, SelectionVector &sel_vec, SelectionVector &seq_sel_vec,
	                                       idx_t count);
	void FullScanHashTable(JoinHTScanState &state, LogicalType key_type, JoinHashTable *hash_table);
	bool has_duplicates {false};

private:
	PerfectHashTable perfect_hash_table;           // columnar perfect hash table
	PerfectHashJoinStats pjoin_stats;              // build and probe statistics
	unique_ptr<bool[]> bitmap_build_idx {nullptr}; // stores the occurences of each value in the build side
	size_t unique_keys {0};                        // stores the number of unique keys in the build side
	size_t probe_sel_count {0};                    // keeps track of how many probe keys have a match
};

} // namespace duckdb
