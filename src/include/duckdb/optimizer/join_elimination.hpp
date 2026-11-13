//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/join_elimination.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/reference_map.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/planner/column_binding.hpp"
#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/planner/logical_operator_visitor.hpp"

namespace duckdb {
class JoinElimination;
class PipelineInfo;

struct DistinctGroupRef {
	column_binding_set_t distinct_group;
	unordered_set<ProjectionIndex> ref_column_ids;
};

struct SavedProjectionMap {
	reference<LogicalOperator> op;
	idx_t child_index;
	vector<ColumnBinding> selected;
};

class PipelineInfo {
public:
	unique_ptr<LogicalOperator> root = nullptr;
	unordered_set<TableIndex> ref_table_ids;
	unordered_map<TableIndex, column_binding_set_t> distinct_groups;

	// pushdown filter condition(ex in table scan operator),
	// if have outer table columns then cannot elimination
	bool has_filter = false;

	optional_ptr<LogicalOperator> join_parent = nullptr;
	idx_t join_index = 0;

public:
	PipelineInfo CreateChild() {
		auto result = PipelineInfo();
		result.ref_table_ids = ref_table_ids;
		result.distinct_groups = distinct_groups;
		return result;
	}
};

class JoinElimination : public LogicalOperatorVisitor {
public:
	explicit JoinElimination() {
	}

	void OptimizeChildren(LogicalOperator &op, optional_ptr<LogicalOperator> parent, idx_t idx);
	// with specific condition we can eliminate a (left/right, semi, inner) join.
	// exemplify left/right join eliminaion condition:
	// 1. output can only have outer table columns
	// 2. join result cannot filter by inner table columns(ex. in where clause/ having clause ...)
	// 3. must ensure each outer row can match at most one inner table row, such as:
	//  1) inner table join condition is unique(ex. 1. join conditions have inner table's primary key 2. inner table
	//  join condition columns contains a whole distinct group) 2) join result columns contains a whole distinct group
	unique_ptr<LogicalOperator> Optimize(unique_ptr<LogicalOperator> op);
	void OptimizeInternal(unique_ptr<LogicalOperator> op);
	unique_ptr<Expression> VisitReplace(BoundColumnRefExpression &expr, unique_ptr<Expression> *expr_ptr) override;

	unique_ptr<JoinElimination> CreateChildren() {
		auto result = make_uniq<JoinElimination>();
		result->pipe_info = pipe_info.CreateChild();
		return result;
	}

private:
	unique_ptr<LogicalOperator> TryEliminateJoin();
	// void ExtractDistinctReferences(vector<Expression> &expressions, idx_t target_table_index);
	bool ContainDistinctGroup(vector<ColumnBinding> &exprs);

	PipelineInfo pipe_info;
	// Projection maps resolved before this optimizer detaches the children it rewrites.
	vector<SavedProjectionMap> saved_projection_maps;

	optional_ptr<LogicalOperator> children_root;
	vector<unique_ptr<JoinElimination>> children;

	unique_ptr<JoinElimination> left_child;
	unique_ptr<JoinElimination> right_child;
};
} // namespace duckdb
