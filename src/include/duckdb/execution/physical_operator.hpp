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

namespace duckdb {
class ExpressionExecutor;
class PhysicalOperator;

//! The current state/context of the operator. The PhysicalOperatorState is
//! updated using the GetChunk function, and allows the caller to repeatedly
//! call the GetChunk function and get new batches of data everytime until the
//! data source is exhausted.
class PhysicalOperatorState {
public:
	PhysicalOperatorState(PhysicalOperator *child);
	virtual ~PhysicalOperatorState() = default;

	//! Flag indicating whether or not the operator is finished [note: not all
	//! operators use this flag]
	bool finished;
	//! DataChunk that stores data from the child of this operator
	DataChunk child_chunk;
	//! State of the child of this operator
	unique_ptr<PhysicalOperatorState> child_state;
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
	PhysicalOperator(PhysicalOperatorType type, vector<TypeId> types) : type(type), types(types) {
	}
	virtual ~PhysicalOperator() {
	}

	//! The physical operator type
	PhysicalOperatorType type;
	//! The set of children of the operator
	vector<unique_ptr<PhysicalOperator>> children;
	//! The types returned by this physical operator
	vector<TypeId> types;

public:
	string ToString(idx_t depth = 0) const;
	void Print();

	//! Return a vector of the types that will be returned by this operator
	vector<TypeId> &GetTypes() {
		return types;
	}
	//! Initialize a given chunk to the types that will be returned by this
	//! operator, this will prepare chunk for a call to GetChunk. This method
	//! only has to be called once for any amount of calls to GetChunk.
	virtual void InitializeChunk(DataChunk &chunk) {
		auto &types = GetTypes();
		chunk.Initialize(types);
	}
	//! Retrieves a chunk from this operator and stores it in the chunk
	//! variable.
	virtual void GetChunkInternal(ExecutionContext &context, DataChunk &chunk, PhysicalOperatorState *state) = 0;

	void GetChunk(ExecutionContext &context, DataChunk &chunk, PhysicalOperatorState *state);

	//! Create a new empty instance of the operator state
	virtual unique_ptr<PhysicalOperatorState> GetOperatorState() {
		return make_unique<PhysicalOperatorState>(children.size() == 0 ? nullptr : children[0].get());
	}

	virtual string ExtraRenderInformation() const {
		return "";
	}

	virtual bool IsSink() const {
		return false;
	}
};

class GlobalOperatorState {
public:
	virtual ~GlobalOperatorState() {
	}
};

class LocalSinkState {
public:
	virtual ~LocalSinkState() {
	}
};

class PhysicalSink : public PhysicalOperator {
public:
	PhysicalSink(PhysicalOperatorType type, vector<TypeId> types) : PhysicalOperator(type, move(types)) {
	}

	unique_ptr<GlobalOperatorState> sink_state;

public:
	//! The sink method is called constantly with new input, as long as new input is available. Note that this method
	//! CAN be called in parallel, proper locking is needed when accessing data inside the GlobalOperatorState.
	virtual void Sink(ExecutionContext &context, GlobalOperatorState &gstate, LocalSinkState &lstate,
	                  DataChunk &input) = 0;
	// The combine is called when a single thread has completed execution of its part of the pipeline, it is the final
	// time that a specific LocalSinkState is accessible. This method can be called in parallel while other Sink() or
	// Combine() calls are active on the same GlobalOperatorState.
	virtual void Combine(ExecutionContext &context, GlobalOperatorState &gstate, LocalSinkState &lstate) {
	}
	//! The finalize is called when ALL threads are finished execution. It is called only once per pipeline, and is
	//! entirely single threaded.
	virtual void Finalize(ExecutionContext &context, unique_ptr<GlobalOperatorState> gstate) {
		this->sink_state = move(gstate);
	}

	virtual unique_ptr<LocalSinkState> GetLocalSinkState(ExecutionContext &context) {
		return make_unique<LocalSinkState>();
	}
	virtual unique_ptr<GlobalOperatorState> GetGlobalState(ClientContext &context) {
		return make_unique<GlobalOperatorState>();
	}

	bool IsSink() const override {
		return true;
	}
};

} // namespace duckdb
