//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/aggregate_state_serialization.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/function/aggregate_state_layout.hpp"

namespace duckdb {

class Vector;
class ArenaAllocator;
class BoundAggregateFunction;
struct FunctionData;

//! Who owns the variable size data of deserialized aggregate states.
enum class StateMemoryOwnership : uint8_t {
	//! The states borrow memory from the input vector: cheap, but they must be consumed
	//! (combined or finalized) before the input is destroyed
	BORROWED,
	//! All variable size data is copied to the supplied allocator: the states may outlive the input
	OWNED
};

//! Converts aggregate states to and from their exported logical form, through the aggregate
//! state export layouts. Used by the aggregate state export functions and by state spilling.
struct AggregateStateSerialization {
	//! Serialize `count` states into `result` (of the layout's logical type) at [offset, offset + count)
	static void SerializeStates(const BoundAggregateFunction &aggr, optional_ptr<const FunctionData> bind_data,
	                            const AggregateStateLayout &layout, Vector &states, idx_t count, Vector &result,
	                            ArenaAllocator &allocator, idx_t offset);
	//! Serialize `count` states at the given addresses, without an aggregate-specific export callback
	static void SerializeStates(const AggregateStateLayout &layout, Vector &result, idx_t count,
	                            const data_ptr_t *addresses, idx_t offset);
	//! Deserialize `count` rows of `input` into consecutive packed states in `dest_buffer`.
	//! With OWNED ownership, all variable size state data is allocated from `allocator`;
	//! with BORROWED ownership, primitive string values may point into `input`.
	static void DeserializeStates(const BoundAggregateFunction &aggr, const AggregateStateLayout &layout,
	                              const Vector &input, idx_t count, data_ptr_t dest_buffer, ArenaAllocator &allocator,
	                              StateMemoryOwnership ownership);
};

} // namespace duckdb
