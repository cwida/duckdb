#pragma once

#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/optimizer/column_binding_replacer.hpp"
#include "duckdb/planner/column_binding_map.hpp"

namespace duckdb {

class Optimizer;
class LogicalOperator;

class ProjectionPullup : public LogicalOperatorVisitor {
public:
	explicit ProjectionPullup(Optimizer &optimizer_p, unique_ptr<LogicalOperator> &root)
	    : optimizer(optimizer_p), root(root) {
	}

	void Optimize(unique_ptr<LogicalOperator> &op);
	void VisitOperator(unique_ptr<LogicalOperator> &op) override;

private:
	Optimizer &optimizer;
	unique_ptr<LogicalOperator> &root;
	vector<reference<LogicalOperator>> parents;
	optional_ptr<LogicalOperator> FindParent(LogicalOperator &target, LogicalOperator &current);
	void PopParents(const LogicalOperator &op);
	void InsertProjectionBelowOp(unique_ptr<LogicalOperator> &op, unique_ptr<LogicalOperator> &child, bool stop_at_op);
	void CanPullThrough(column_binding_map_t<unique_ptr<Expression>> &projection_map, bool keep_throwing_filters_above,
	                    bool &can_pull_through);
	void PullUpColrefProjection(unique_ptr<LogicalOperator> &op, LogicalProjection &proj,
	                            vector<ColumnBinding> &proj_bindings);
	void PullUpNonColrefProjection(unique_ptr<LogicalOperator> &op, LogicalProjection &proj,
	                               vector<ColumnBinding> &proj_bindings, idx_t pull_up_to_here);
};

} // namespace duckdb
