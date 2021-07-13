//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/join/physical_hash_join.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/chunk_collection.hpp"
#include "duckdb/common/value_operations/value_operations.hpp"
#include "duckdb/execution/join_hashtable.hpp"
#include "duckdb/execution/operator/join/perfect_hash_join_executor.hpp"
#include "duckdb/execution/operator/join/physical_comparison_join.hpp"
#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/planner/operator/logical_join.hpp"

namespace duckdb {

class PhysicalHashJoinState : public PhysicalOperatorState {
public:
	PhysicalHashJoinState(PhysicalOperator &op, PhysicalOperator *left, PhysicalOperator *right,
	                      vector<JoinCondition> &conditions)
	    : PhysicalOperatorState(op, left) {
	}

	DataChunk cached_chunk;
	DataChunk join_keys;
	ExpressionExecutor probe_executor;
	unique_ptr<JoinHashTable::ScanStructure> scan_structure;
	SelectionVector build_sel_vec;
	SelectionVector probe_sel_vec;
	SelectionVector seq_sel_vec;
};
//! PhysicalHashJoin represents a hash loop join between two tables

class HashJoinGlobalState : public GlobalOperatorState {
public:
	HashJoinGlobalState() {
	}

	//! The HT used by the join
	unique_ptr<JoinHashTable> hash_table;
	//! Only used for FULL OUTER JOIN: scan state of the final scan to find unmatched tuples in the build-side
	JoinHTScanState ht_scan_state;
	vector<Vector> build_columns; // for build_data storage
	LogicalType key_type {LogicalType::INVALID};
};

class HashJoinLocalState : public LocalSinkState {
public:
	DataChunk build_chunk;
	DataChunk join_keys;
	ExpressionExecutor build_executor;
};
class PhysicalHashJoin : public PhysicalComparisonJoin {
public:
	PhysicalHashJoin(LogicalOperator &op, unique_ptr<PhysicalOperator> left, unique_ptr<PhysicalOperator> right,
	                 vector<JoinCondition> cond, JoinType join_type, const vector<idx_t> &left_projection_map,
	                 const vector<idx_t> &right_projection_map, vector<LogicalType> delim_types,
	                 idx_t estimated_cardinality, PerfectHashJoinStats perfect_join_stats);
	PhysicalHashJoin(LogicalOperator &op, unique_ptr<PhysicalOperator> left, unique_ptr<PhysicalOperator> right,
	                 vector<JoinCondition> cond, JoinType join_type, idx_t estimated_cardinality,
	                 PerfectHashJoinStats join_state);
	void Combine(ExecutionContext &context, GlobalOperatorState &gstate, LocalSinkState &lstate) override;

	vector<idx_t> right_projection_map;
	//! The types of the keys
	vector<LogicalType> condition_types;
	//! The types of all conditions
	vector<LogicalType> build_types;
	//! Duplicate eliminated types; only used for delim_joins (i.e. correlated subqueries)
	vector<LogicalType> delim_types;
	//! Checks and execute perfect hash join optimization
	unique_ptr<PerfectHashJoinExecutor> pjoin_executor {nullptr};
	bool use_perfect_hash {false};

public:
	unique_ptr<GlobalOperatorState> GetGlobalState(ClientContext &context) override;

	unique_ptr<LocalSinkState> GetLocalSinkState(ExecutionContext &context) override;
	void Sink(ExecutionContext &context, GlobalOperatorState &state, LocalSinkState &lstate,
	          DataChunk &input) const override;
	bool Finalize(Pipeline &pipeline, ClientContext &context, unique_ptr<GlobalOperatorState> gstate) override;

	void GetChunkInternal(ExecutionContext &context, DataChunk &chunk, PhysicalOperatorState *state) const override;
	unique_ptr<PhysicalOperatorState> GetOperatorState() override;
	void FinalizeOperatorState(PhysicalOperatorState &state, ExecutionContext &context) override;

private:
	void ProbeHashTable(ExecutionContext &context, DataChunk &chunk, PhysicalOperatorState *state_p) const;
};

} // namespace duckdb
