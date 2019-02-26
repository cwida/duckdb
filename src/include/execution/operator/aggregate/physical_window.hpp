//===----------------------------------------------------------------------===//
//                         DuckDB
//
// execution/operator/aggregate/physical_window.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "common/types/chunk_collection.hpp"
#include "common/types/tuple.hpp"
#include "execution/physical_operator.hpp"

namespace duckdb {

//! PhysicalWindow implements window functions
class PhysicalWindow : public PhysicalOperator {
public:
	PhysicalWindow(LogicalOperator &op, vector<unique_ptr<Expression>> select_list,
	               PhysicalOperatorType type = PhysicalOperatorType::WINDOW);

	void _GetChunk(ClientContext &context, DataChunk &chunk, PhysicalOperatorState *state) override;

	unique_ptr<PhysicalOperatorState> GetOperatorState() override;

	void AcceptExpressions(SQLNodeVisitor *v) override {
		for (auto &e : select_list) {
			v->VisitExpression(&e);
		}
	}

	//! The projection list of the SELECT statement (that contains aggregates)
	vector<unique_ptr<Expression>> select_list;
};

//! The operator state of the window
class PhysicalWindowOperatorState : public PhysicalOperatorState {
public:
	PhysicalWindowOperatorState(PhysicalOperator *child) : PhysicalOperatorState(child), position(0) {
	}

	size_t position;
	ChunkCollection tuples;
	ChunkCollection window_results;
};

} // namespace duckdb
