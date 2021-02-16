//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/physical_operator.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/enums/physical_operator_type.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/execution/execution_context.hpp"
#include <functional>

namespace duckdb {
class PhysicalOperator;
//! The current state/context of the operator. The PhysicalOperatorState is
//! updated using the GetChunk function, and allows the caller to repeatedly
//! call the GetChunk function and get new batches of data everytime until the
//! data source is exhausted.
class PhysicalOperatorState {
public:
	PhysicalOperatorState(ExecutionContext &execution_context, PhysicalOperator &op, PhysicalOperator *child);
	virtual ~PhysicalOperatorState() = default;

	//! Flag indicating whether or not the operator is finished [note: not all
	//! operators use this flag]
	bool finished;
	//! DataChunk that stores data from the child of this operator
	DataChunk child_chunk;
	//! State of the child of this operator
	unique_ptr<PhysicalOperatorState> child_state;
	//! The initial chunk
	DataChunk initial_chunk;
};

//! PhysicalOperator is the base class of the physical operators present in the
//! execution plan
/*!
    The execution model is a pull-based execution model. GetChunk is called on
   the root node, which causes the root node to be executed, and presumably call
   GetChunk again on its child nodes. Every node in the operator chain has a
   state that is updated as GetChunk is called: PhysicalOperatorState (different
   operators subclass this state and add different properties).
*/
class PhysicalOperator {
public:
	PhysicalOperator(PhysicalOperatorType type, vector<LogicalType> types) : type(type), types(types) {
	}
	virtual ~PhysicalOperator() {
	}

	//! The physical operator type
	PhysicalOperatorType type;
	//! The set of children of the operator
	vector<unique_ptr<PhysicalOperator>> children;
	//! The types returned by this physical operator
	vector<LogicalType> types;

public:
	virtual string GetName() const;
	virtual string ParamsToString() const {
		return "";
	}
	virtual string ToString() const;
	void Print();

	//! Return a vector of the types that will be returned by this operator
	vector<LogicalType> &GetTypes() {
		return types;
	}
	//! Initialize a given chunk to the types that will be returned by this
	//! operator, this will prepare chunk for a call to GetChunk. This method
	//! only has to be called once for any amount of calls to GetChunk.
	virtual void InitializeChunk(DataChunk &chunk) {
		auto &types = GetTypes();
		chunk.Initialize(types);
	}
	virtual void InitializeChunkEmpty(DataChunk &chunk) {
		auto &types = GetTypes();
		chunk.InitializeEmpty(types);
	}
	//! Retrieves a chunk from this operator and stores it in the chunk
	//! variable.
	virtual void GetChunkInternal(ExecutionContext &context, DataChunk &chunk, PhysicalOperatorState *state) = 0;

	void GetChunk(ExecutionContext &context, DataChunk &chunk, PhysicalOperatorState *state);

	//! Create a new empty instance of the operator state
	virtual unique_ptr<PhysicalOperatorState> GetOperatorState(ExecutionContext &execution_context) {
		return make_unique<PhysicalOperatorState>(execution_context, *this,
		                                          children.size() == 0 ? nullptr : children[0].get());
	}

	virtual bool IsSink() const {
		return false;
	}
};

} // namespace duckdb
