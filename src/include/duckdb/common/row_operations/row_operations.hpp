//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/row_operations/row_operations.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/vector.hpp"

namespace duckdb {

struct AggregateObject;
class RowLayout;
struct SelectionVector;
class StringHeap;
class Vector;

// RowOperations contains a set of operations that operate on data using a RowLayout
struct RowOperations {
	//===--------------------------------------------------------------------===//
	// Aggregation Operators
	//===--------------------------------------------------------------------===//
	//! initialize - unaligned addresses
	static void InitializeStates(RowLayout &layout, Vector &addresses, const SelectionVector &sel, idx_t count);
	//! destructor - unaligned addresses, updated
	static void DestroyStates(RowLayout &layout, Vector &addresses, idx_t count);
	//! update - aligned addresses
	static void UpdateStates(AggregateObject &aggr, Vector &addresses, DataChunk &payload, idx_t arg_idx, idx_t count);
	//! filtered update - aligned addresses
	static void UpdateFilteredStates(AggregateObject &aggr, Vector &addresses, DataChunk &payload, idx_t arg_idx);
	//! combine - unaligned addresses, updated
	static void CombineStates(RowLayout &layout, Vector &sources, Vector &targets, idx_t count);
	//! finalize - unaligned addresses, updated
	static void FinalizeStates(RowLayout &layout, Vector &addresses, DataChunk &result, idx_t aggr_idx);

	//===--------------------------------------------------------------------===//
	// Read/Write Operators
	//===--------------------------------------------------------------------===//
	//! Scatter group data to the rows. Initialises the ValidityMask.
	static void Scatter(VectorData source_data[], const RowLayout &layout, Vector &addresses, StringHeap &string_heap,
	                    const SelectionVector &sel, idx_t count);
	//! Gather a single column
	static void Gather(const RowLayout &layout, Vector &addresses, Vector &dest, idx_t count, idx_t col_idx);
};

} // namespace duckdb