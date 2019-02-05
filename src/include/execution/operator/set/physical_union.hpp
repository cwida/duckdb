//===----------------------------------------------------------------------===//
//                         DuckDB
//
// execution/operator/set/physical_union.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "execution/physical_operator.hpp"

namespace duckdb {
class PhysicalUnion : public PhysicalOperator {
public:
	PhysicalUnion(LogicalOperator &op, unique_ptr<PhysicalOperator> top, unique_ptr<PhysicalOperator> bottom);

	void _GetChunk(ClientContext &context, DataChunk &chunk, PhysicalOperatorState *state) override;
	unique_ptr<PhysicalOperatorState> GetOperatorState(ExpressionExecutor *parent_executor) override;
	void AcceptExpressions(SQLNodeVisitor *v) override{};
};

class PhysicalUnionOperatorState : public PhysicalOperatorState {
public:
	PhysicalUnionOperatorState(ExpressionExecutor *parent_executor)
	    : PhysicalOperatorState(nullptr, parent_executor), top_done(false) {
	}
	unique_ptr<PhysicalOperatorState> top_state;
	unique_ptr<PhysicalOperatorState> bottom_state;
	bool top_done = false;
};

}; // namespace duckdb
