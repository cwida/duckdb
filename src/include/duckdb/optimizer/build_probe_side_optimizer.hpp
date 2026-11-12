//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/build_side_probe_side_optimizer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/unordered_set.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/planner/logical_operator.hpp"

namespace duckdb {

struct BuildSize {
	double left_side;
	double right_side;

	// Initialize with 1 so the build side is just the cardinality if types aren't
	// known.
	BuildSize() : left_side(1), right_side(1) {
	}
};

enum class RecursiveProbeSidePreference : uint8_t { NONE, KEEP, SWAP };

class BuildProbeSideOptimizer : LogicalOperatorVisitor {
private:
	static constexpr idx_t COLUMN_COUNT_PENALTY = 2;
	static constexpr double PREFER_RIGHT_DEEP_PENALTY = 0.15;

public:
	explicit BuildProbeSideOptimizer(ClientContext &context, LogicalOperator &op);
	void VisitOperator(LogicalOperator &op) override;
	void VisitExpression(unique_ptr<Expression> *expression) override {};

	static double GetBuildSize(vector<LogicalType> types, idx_t cardinality);

private:
	bool TryFlipJoinChildren(LogicalOperator &op) const;
	static idx_t ChildHasJoins(LogicalOperator &op);
	RecursiveProbeSidePreference GetRecursiveProbeSidePreference(const LogicalOperator &op) const;

	static BuildSize GetBuildSizes(const LogicalOperator &op, idx_t lhs_cardinality, idx_t rhs_cardinality);

private:
	ClientContext &context;
	vector<ColumnBinding> preferred_on_probe_side;
	unordered_set<TableIndex> active_recursive_cte_indexes;
};

} // namespace duckdb
