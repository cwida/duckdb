//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/join/physical_range_join.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/row/block_iterator.hpp"
#include "duckdb/execution/operator/join/physical_comparison_join.hpp"
#include "duckdb/common/sorting/sort.hpp"
#include "duckdb/common/sorting/sorted_run.hpp"

namespace duckdb {

//! PhysicalRangeJoin represents one or more inequality range join predicates between
//! two tables
class PhysicalRangeJoin : public PhysicalComparisonJoin {
public:
	class GlobalSortedTable;

	class LocalSortedTable {
	public:
		LocalSortedTable(ExecutionContext &context, GlobalSortedTable &global_table, const idx_t child);

		void Sink(ExecutionContext &context, DataChunk &input);
		void ResetForReuse(ExecutionContext &context);

		//! The global table we are connected to
		GlobalSortedTable &global_table;
		//! The local sort state
		unique_ptr<LocalSinkState> local_sink;
		//! Local copy of the sorting expression executor
		ExpressionExecutor executor;
		//! Holds a vector of incoming sorting columns
		DataChunk keys;
		//! The sort data
		DataChunk sort_chunk;
		//! The number of NULL values
		idx_t has_null;
		//! The total number of rows
		idx_t count;

	private:
		// Merge the NULLs of all non-DISTINCT predicates into the primary so they sort to the end.
		idx_t MergeNulls(Vector &primary, const vector<JoinCondition> &conditions);
	};

	class GlobalSortedTable {
	public:
		GlobalSortedTable(ClientContext &client, const vector<BoundOrderByNode> &orders,
		                  const vector<LogicalType> &payload_layout, const PhysicalComparisonJoin &op);

		inline idx_t Count() const {
			return count;
		}

		inline idx_t BlockCount() const {
			return sorted->key_data->ChunkCount();
		}

		inline idx_t BlockStart(idx_t i) const {
			return MinValue<idx_t>(i * STANDARD_VECTOR_SIZE, count);
		}

		inline idx_t BlockEnd(idx_t i) const {
			return BlockStart(i + 1) - 1;
		}

		inline idx_t BlockSize(idx_t i) const {
			return i < BlockCount() ? MinValue<idx_t>(STANDARD_VECTOR_SIZE, count - BlockStart(i)) : 0;
		}

		inline SortKeyType GetSortKeyType() const {
			return sorted->key_data->GetLayout().GetSortKeyType();
		}

		void InitializeMatches();

		//! Combine local states
		void Combine(ExecutionContext &context, LocalSortedTable &ltable);
		void ResetForReuse(ClientContext &client);
		//! Prepare for sorting.
		void Finalize(ClientContext &client, InterruptState &interrupt);
		//! Schedules the materialisation process.
		void Materialize(Pipeline &pipeline, Event &event);
		//! Single-threaded materialisation.
		void Materialize(ExecutionContext &context, InterruptState &interrupt);
		//! Parallel materialisation.
		void MaterializeSortedRun(ExecutionContext &context, InterruptState &interrupt);
		//! Parallel materialisation finish.
		void GetSortedRun(ClientContext &client);
		//! Materialize an empty sorted run.
		void MaterializeEmpty(ClientContext &client);
		//! Print the table to the console
		void Print();

		//! Create an iteration state
		unique_ptr<ExternalBlockIteratorState> CreateIteratorState() {
			auto state = make_uniq<ExternalBlockIteratorState>(*sorted->key_data, sorted->payload_data.get());

			// Unless we do this, we will only get values from the first chunk
			Repin(*state);

			return state;
		}
		//! Reset the pins for an iterator so we release memory in a timely manner
		static void Repin(ExternalBlockIteratorState &iter) {
			iter.SetKeepPinned(true);
			iter.SetPinPayload(true);
		}
		//! Create an iteration state
		unique_ptr<SortedRunScanState> CreateScanState(ClientContext &client) {
			return make_uniq<SortedRunScanState>(client, *sort);
		}
		//! Initialize a payload scanning state
		void InitializePayloadState(TupleDataChunkState &state) {
			sorted->payload_data->InitializeChunkState(state);
		}

		//! The hosting operator
		const PhysicalComparisonJoin &op;
		//! The sort description
		unique_ptr<Sort> sort;
		//! The shared sort state
		unique_ptr<GlobalSinkState> global_sink;
		//! Whether or not the RHS has NULL values
		atomic<idx_t> has_null;
		//! The total number of rows in the RHS
		atomic<idx_t> count;
		//! The number of materialisation tasks completed in parallel
		atomic<idx_t> tasks_completed;
		//! The shared materialisation state
		unique_ptr<GlobalSourceState> global_source;
		//! The materialized data
		unique_ptr<SortedRun> sorted;
		//! A bool indicating for each tuple in the RHS if they found a match (only used in FULL OUTER JOIN)
		unsafe_unique_array<bool> found_match;
	};

public:
	PhysicalRangeJoin(PhysicalPlan &physical_plan, LogicalComparisonJoin &op, PhysicalOperatorType type,
	                  PhysicalOperator &left, PhysicalOperator &right, vector<JoinCondition> cond, JoinType join_type,
	                  idx_t estimated_cardinality, unique_ptr<JoinFilterPushdownInfo> pushdown_info);

	// Projection mappings
	using ProjectionMapping = vector<idx_t>;
	ProjectionMapping left_projection_map;
	ProjectionMapping right_projection_map;

	//!	The full set of types (left + right child)
	vector<LogicalType> unprojected_types;

public:
	// Compare conditions for priority
	static bool LessThan(const JoinCondition &a, const JoinCondition &b);

	// Gather the result values and slice the payload columns to those values.
	static void SliceSortedPayload(DataChunk &chunk, GlobalSortedTable &table, ExternalBlockIteratorState &state,
	                               TupleDataChunkState &chunk_state, const idx_t chunk_idx,
	                               const unsafe_vector<idx_t> &result, SortedRunScanState &scan_state);
	// Apply a tail condition to the current selection
	static idx_t SelectJoinTail(const ExpressionType &condition, const Vector &left, const Vector &right,
	                            const SelectionVector *sel, idx_t count, SelectionVector *true_sel);

	//!	Utility to project full width internal chunks to projected results
	void ProjectResult(DataChunk &chunk, DataChunk &result) const;
};

} // namespace duckdb
