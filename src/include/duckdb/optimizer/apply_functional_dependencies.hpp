//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/apply_functional_dependencies.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/logical_operator_visitor.hpp"

#include "duckdb/planner/column_binding_map.hpp"

namespace duckdb {

struct BoundOrderByNode;

class ApplyFunctionalDependencies : public LogicalOperatorVisitor {
public:
	explicit ApplyFunctionalDependencies(bool read_only = false) : read_only(read_only) {
	}

	static vector<column_binding_set_t> GetUniqueColumnCombinations(LogicalOperator &op) {
		ApplyFunctionalDependencies visitor(true);
		visitor.VisitOperator(op);
		return visitor.result;
	}

	void VisitOperator(LogicalOperator &op) override;
	void VisitExpression(unique_ptr<Expression> *expression) override;

private:
	void VisitLogicalGet(LogicalGet &get);
	void VisitLogicalAggregate(LogicalAggregate &aggr);
	void VisitLogicalDistinct(LogicalDistinct &distinct);
	void VisitLogicalProjection(LogicalProjection &projection);
	void VisitLogicalOrder(LogicalOrder &order);

	void VisitWindowExpression(BoundWindowExpression &wexpr) const;

	void VisitPartitioning(vector<unique_ptr<Expression>> &partitions) const;
	void VisitOrderBys(vector<BoundOrderByNode> &orders) const;

	const bool read_only;
	vector<column_binding_set_t> result;
};

} // namespace duckdb
