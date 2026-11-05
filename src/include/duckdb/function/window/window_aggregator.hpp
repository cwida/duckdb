//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/window/window_aggregator.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/operator/aggregate/aggregate_object.hpp"
#include "duckdb/function/window/window_boundaries_state.hpp"

namespace duckdb {

class WindowCollection;
class WindowCursor;
struct WindowSharedExpressions;

class WindowAggregator {
public:
	using CollectionPtr = optional_ptr<WindowCollection>;

	template <typename OP>
	static void EvaluateSubFrames(const DataChunk &bounds, const WindowExcludeMode exclude_mode, idx_t count,
	                              idx_t row_idx, SubFrames &frames, OP operation) {
		auto begins = FlatVector::GetData<const idx_t>(bounds.data[FRAME_BEGIN]);
		auto ends = FlatVector::GetData<const idx_t>(bounds.data[FRAME_END]);
		auto peer_begin = FlatVector::GetData<const idx_t>(bounds.data[PEER_BEGIN]);
		auto peer_end = FlatVector::GetData<const idx_t>(bounds.data[PEER_END]);

		for (idx_t i = 0, cur_row = row_idx; i < count; ++i, ++cur_row) {
			idx_t nframes = 0;
			if (exclude_mode == WindowExcludeMode::NO_OTHER) {
				auto begin = begins[i];
				auto end = MaxValue(ends[i], begin);
				frames[nframes++] = FrameBounds(begin, end);
			} else {
				//	The frame_exclusion option allows rows around the current row to be excluded from the frame,
				//	even if they would be included according to the frame start and frame end options.
				//	EXCLUDE CURRENT ROW excludes the current row from the frame.
				//	EXCLUDE GROUP excludes the current row and its ordering peers from the frame.
				//	EXCLUDE TIES excludes any peers of the current row from the frame, but not the current row itself.
				//	EXCLUDE NO OTHERS simply specifies explicitly the default behavior
				//	of not excluding the current row or its peers.
				//	https://www.postgresql.org/docs/current/sql-expressions.html#SYNTAX-WINDOW-FUNCTIONS
				//
				//	For the sake of the client, we make some guarantees about the subframes:
				//	* They are in order left-to-right
				//	* They do not intersect
				//	* start <= end
				//	* The number is always the same
				//
				//	Since we always have peer_begin <= cur_row < cur_row + 1 <= peer_end
				//	this is not too hard to arrange, but it may be that some subframes are contiguous,
				//	and some are empty.

				//	WindowExcludePart::LEFT
				const auto frame_begin = begins[i];
				const auto frame_end = MaxValue(ends[i], frame_begin);
				auto begin = frame_begin;
				auto end = (exclude_mode == WindowExcludeMode::CURRENT_ROW) ? cur_row : peer_begin[i];
				end = MinValue(end, frame_end);
				end = MaxValue(end, frame_begin);
				frames[nframes++] = FrameBounds(begin, end);

				// with EXCLUDE TIES, in addition to the frame part right of the peer group's end,
				// we also need to consider the current row
				if (exclude_mode == WindowExcludeMode::TIES) {
					begin = MinValue(MaxValue(cur_row, frame_begin), frame_end);
					end = MaxValue(MinValue(cur_row + 1, frame_end), frame_begin);
					frames[nframes++] = FrameBounds(begin, end);
				}

				//	WindowExcludePart::RIGHT
				end = frame_end;
				begin = (exclude_mode == WindowExcludeMode::CURRENT_ROW) ? (cur_row + 1) : peer_end[i];
				begin = MaxValue(begin, frame_begin);
				begin = MinValue(begin, frame_end);
				frames[nframes++] = FrameBounds(begin, end);
			}

			operation(i);
		}
	}

	explicit WindowAggregator(const BoundWindowExpression &wexpr);
	WindowAggregator(const BoundWindowExpression &wexpr, WindowSharedExpressions &shared);
	virtual ~WindowAggregator();

	//	Threading states
	virtual unique_ptr<GlobalSinkState> GetGlobalState(ClientContext &client, idx_t group_count,
	                                                   const ValidityMask &partition_mask) const;
	virtual unique_ptr<LocalSinkState> GetLocalState(ExecutionContext &context,
	                                                 const GlobalSinkState &gstate) const = 0;

	//	Build
	virtual void Sink(ExecutionContext &context, DataChunk &sink_chunk, DataChunk &coll_chunk, idx_t input_idx,
	                  optional_ptr<SelectionVector> filter_sel, idx_t filtered, OperatorSinkInput &sink);
	virtual void Finalize(ExecutionContext &context, CollectionPtr collection, const FrameStats &stats,
	                      OperatorSinkInput &sink);

	//	Probe
	virtual void Evaluate(ExecutionContext &context, const DataChunk &bounds, Vector &result, idx_t count,
	                      idx_t row_idx, OperatorSinkInput &sink) const = 0;

	//! The window function
	const BoundWindowExpression &wexpr;
	//! A description of the aggregator
	const AggregateObject aggr;
	//! The argument types for the function
	vector<LogicalType> arg_types;
	//! The result type of the window function
	const LogicalType result_type;
	//! The size of a single aggregate state
	const idx_t state_size;
	//! The window exclusion clause
	const WindowExcludeMode exclude_mode;
	//! Partition collection column indices
	vector<column_t> child_idx;
};

class WindowAggregatorGlobalState : public GlobalSinkState {
public:
	WindowAggregatorGlobalState(ClientContext &client, const WindowAggregator &aggregator_p, idx_t group_count);

	//! The client we are in
	ClientContext &client;

	//! Global allocator
	ArenaAllocator allocator;

	//! The aggregator data
	const WindowAggregator &aggregator;

	//! Partition starts
	vector<idx_t> partition_offsets;

	//! The aggregate function
	const AggregateObject aggr;

	//! The filtered rows in inputs.
	ValidityArray filter_mask;
	//! Storage of the filter mask (one byte per row), from the buffer allocator so that it counts against the limit
	AllocatedData filter_mask_data;

	//! Lock for single threading
	mutable mutex lock;

	//! Count of local tasks
	mutable std::atomic<idx_t> locals;

	//! Number of finalised states
	std::atomic<idx_t> finalized;

protected:
	void BuildPartitionOffsets(const idx_t group_count, const ValidityMask &partition_mask);
};

class WindowAggregatorLocalState : public LocalSinkState {
public:
	using CollectionPtr = optional_ptr<WindowCollection>;

	static void InitSubFrames(SubFrames &frames, const WindowExcludeMode exclude_mode);

	explicit WindowAggregatorLocalState(ExecutionContext &context);

	void Sink(ExecutionContext &context, WindowAggregatorGlobalState &gastate, DataChunk &sink_chunk,
	          DataChunk &coll_chunk, idx_t row_idx);
	virtual void Finalize(ExecutionContext &context, WindowAggregatorGlobalState &gastate, CollectionPtr collection);

	//! Global allocator
	ArenaAllocator allocator;

	//! The state used for reading the collection
	unique_ptr<WindowCursor> cursor;
};

} // namespace duckdb
