//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/join/physical_hash_join.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/operator/join/physical_comparison_join.hpp"
#include "duckdb/execution/physical_operator.hpp"

namespace duckdb {
class JoinHashTable;

//! Residual predicate information for hash joins
struct ResidualPredicateInfo {
	unordered_map<idx_t, idx_t> build_input_to_layout_map;
	unordered_map<idx_t, idx_t> probe_input_to_probe_map;
	vector<LogicalType> probe_types;

	ResidualPredicateInfo() = default;
	unique_ptr<ResidualPredicateInfo> Copy() const {
		auto result = make_uniq<ResidualPredicateInfo>();
		result->build_input_to_layout_map = build_input_to_layout_map;
		result->probe_input_to_probe_map = probe_input_to_probe_map;
		result->probe_types = probe_types;
		return result;
	}
};

//! PhysicalHashJoin represents a hash loop join between two tables
class PhysicalHashJoin : public PhysicalComparisonJoin {
public:
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::HASH_JOIN;

	struct JoinProjectionColumns {
		vector<idx_t> col_idxs;
		vector<LogicalType> col_types;
	};

public:
	PhysicalHashJoin(PhysicalPlan &physical_plan, LogicalOperator &op, PhysicalOperator &left, PhysicalOperator &right,
	                 vector<JoinCondition> conds, JoinType join_type,
	                 const vector<ProjectionIndex> &left_projection_map,
	                 const vector<ProjectionIndex> &right_projection_map, vector<LogicalType> delim_types,
	                 idx_t estimated_cardinality, unique_ptr<JoinFilterPushdownInfo> pushdown_info);
	PhysicalHashJoin(PhysicalPlan &physical_plan, LogicalOperator &op, PhysicalOperator &left, PhysicalOperator &right,
	                 vector<JoinCondition> cond, JoinType join_type, idx_t estimated_cardinality);

	//! Initialize HT for this operator
	unique_ptr<JoinHashTable> InitializeHashTable(ClientContext &context, idx_t initial_radix_bits) const;

	//! The types of the join keys
	vector<LogicalType> condition_types;

	//! The indices/types of the payload columns
	JoinProjectionColumns payload_columns;
	//! The indices/types of the lhs columns that need to be output
	JoinProjectionColumns lhs_output_columns;
	//! The indices/types of the rhs columns that need to be output
	JoinProjectionColumns rhs_output_columns;

	//! Duplicate eliminated types; only used for delim_joins (i.e. correlated subqueries)
	vector<LogicalType> delim_types;

	unique_ptr<ResidualPredicateInfo> residual_info;
	//! For probe phase (includes predicate columns)
	JoinProjectionColumns lhs_probe_columns;
	//! Mapping from lhs_output_columns positions to lhs_probe_columns positions
	vector<idx_t> lhs_output_in_probe;

public:
	InsertionOrderPreservingMap<string> ParamsToString() const override;

public:
	// Operator Interface
	unique_ptr<OperatorState> GetOperatorState(ExecutionContext &context) const override;

	bool ParallelOperator() const override {
		return true;
	}
	PipelineExternalInputSupport GetExternalInputSupport() const override {
		return PipelineExternalInputSupport::SUPPORTED;
	}
	PipelineExternalInputCost GetExternalInputCost() const override {
		return PipelineExternalInputCost::SERIALIZED_FANOUT;
	}
	void InitializeOutputChunk(Allocator &allocator, DataChunk &chunk) const override;

protected:
	// CachingOperator Interface
	OperatorResultType ExecuteInternal(ExecutionContext &context, DataChunk &input, DataChunk &chunk,
	                                   GlobalOperatorState &gstate, OperatorState &state) const override;

	// Source interface
	unique_ptr<GlobalSourceState> GetGlobalSourceState(ClientContext &context) const override;
	unique_ptr<LocalSourceState> GetLocalSourceState(ExecutionContext &context,
	                                                 GlobalSourceState &gstate) const override;
	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                 OperatorSourceInput &input) const override;

	ProgressData GetProgress(ClientContext &context, GlobalSourceState &gstate) const override;

	//! Becomes a source when it is an external join
	bool IsSource() const override {
		return true;
	}

	bool ParallelSource() const override {
		return true;
	}

public:
	// Sink Interface
	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override;

	unique_ptr<LocalSinkState> GetLocalSinkState(ExecutionContext &context) const override;
	void SetPreserveBuildForRecursiveReuse(bool preserve) const;
	bool CanPreserveBuildForRecursiveReuse() const;
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override;
	SinkCombineResultType Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const override;
	void PrepareFinalize(ClientContext &context, GlobalSinkState &global_state) const override;
	SinkFinalizeType Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
	                          OperatorSinkFinalizeInput &input) const override;

	bool IsSink() const override {
		return true;
	}
	bool ParallelSink() const override {
		return true;
	}

private:
	static void ExtractResidualPredicateColumns(unique_ptr<Expression> &predicate, idx_t probe_column_count,
	                                            vector<idx_t> &probe_column_ids, vector<idx_t> &build_column_ids);

	void InitializeResidualPredicate(const vector<LogicalType> &lhs_input_types, const vector<idx_t> &probe_cols);

	void InitializeBuildSide(const vector<LogicalType> &lhs_input_types, const vector<LogicalType> &rhs_input_types,
	                         const vector<ProjectionIndex> &right_projection_map, const vector<idx_t> &build_cols);
	void MapResidualBuildColumns(const vector<LogicalType> &lhs_input_types, const vector<LogicalType> &rhs_input_types,
	                             const vector<idx_t> &build_cols,
	                             const unordered_map<idx_t, idx_t> &build_columns_in_conditions,
	                             unordered_map<idx_t, idx_t> &build_input_to_layout);
};

} // namespace duckdb
