//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/row_operations/row_operations.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/function/function.hpp"

namespace duckdb {

struct ClusteredAggr;

class ArenaAllocator;
struct AggregateObject;
struct AggregateFilterData;
struct AggregateFilterDataSet;
class DataChunk;
class TupleDataLayout;
struct SelectionVector;
class StringHeap;
struct UnifiedVectorFormat;

struct RowOperationsState {
	explicit RowOperationsState(ArenaAllocator &allocator) : allocator(allocator) {
	}

	ArenaAllocator &allocator;
	unique_ptr<Vector> addresses; // Re-usable vector for row_aggregate.cpp
	//! Per-aggregate slots in which the aggregates can cache state across finalize calls
	vector<unique_ptr<FunctionLocalState>> local_states;
};

// RowOperations contains a set of operations that operate on data using a TupleDataLayout
struct RowOperations {
	//===--------------------------------------------------------------------===//
	// Aggregation Operators
	//===--------------------------------------------------------------------===//
	//! initialize - unaligned addresses
	static void InitializeStates(TupleDataLayout &layout, Vector &addresses, const SelectionVector &sel, idx_t count);
	//! destructor - unaligned addresses, updated
	static void DestroyStates(RowOperationsState &state, TupleDataLayout &layout, Vector &addresses);
	//! update - aligned addresses
	static void UpdateStates(RowOperationsState &state, AggregateObject &aggr, Vector &addresses, DataChunk &payload,
	                         idx_t arg_idx, optional_ptr<const ClusteredAggr> clustered = nullptr);
	//! filtered update - aligned addresses
	static void UpdateFilteredStates(RowOperationsState &state, AggregateFilterData &filter_data, AggregateObject &aggr,
	                                 Vector &addresses, idx_t arg_idx);
	//! clustered update loop shared by grouped and perfect aggregate hash tables
	static void UpdateStatesClustered(RowOperationsState &state, vector<AggregateObject> &aggregates,
	                                  AggregateFilterDataSet *filter_set, const unsafe_vector<idx_t> *filter,
	                                  Vector &addresses, DataChunk &payload, ClusteredAggr &clustered,
	                                  bool skip_addresses);
	//! combine - unaligned addresses, updated
	static void CombineStates(RowOperationsState &state, TupleDataLayout &layout, Vector &sources, Vector &targets);
	//! finalize - unaligned addresses, updated
	static void FinalizeStates(RowOperationsState &state, TupleDataLayout &layout, Vector &addresses, DataChunk &result,
	                           idx_t aggr_idx);

	//===--------------------------------------------------------------------===//
	// Deprecated overloads (count parameter removed - use count-free versions)
	//===--------------------------------------------------------------------===//
	[[deprecated("count parameter is deprecated; call DestroyStates without count instead")]] static void
	DestroyStates(RowOperationsState &state, TupleDataLayout &layout, Vector &addresses, idx_t count) {
		if (count != addresses.size()) {
			throw InternalException("DestroyStates: count (%llu) does not match vector size (%llu)", count,
			                        addresses.size());
		}
		DestroyStates(state, layout, addresses);
	}
	[[deprecated("count parameter is deprecated; call UpdateStates without count instead")]] static void
	UpdateStates(RowOperationsState &state, AggregateObject &aggr, Vector &addresses, DataChunk &payload, idx_t arg_idx,
	             idx_t count) {
		if (count != addresses.size()) {
			throw InternalException("UpdateStates: count (%llu) does not match vector size (%llu)", count,
			                        addresses.size());
		}
		UpdateStates(state, aggr, addresses, payload, arg_idx);
	}
	[[deprecated("count parameter is deprecated; call CombineStates without count instead")]] static void
	CombineStates(RowOperationsState &state, TupleDataLayout &layout, Vector &sources, Vector &targets, idx_t count) {
		if (count != sources.size()) {
			throw InternalException("CombineStates: count (%llu) does not match vector size (%llu)", count,
			                        sources.size());
		}
		CombineStates(state, layout, sources, targets);
	}
};

} // namespace duckdb
