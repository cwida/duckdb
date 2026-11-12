#include "duckdb/optimizer/recursive_join_planning.hpp"

#include "duckdb/planner/operator/logical_cteref.hpp"

namespace duckdb {

static bool ContainsActiveRecursiveReference(const LogicalOperator &op, const unordered_set<TableIndex> &active_ctes) {
	if (op.type == LogicalOperatorType::LOGICAL_CTE_REF && active_ctes.count(op.Cast<LogicalCTERef>().cte_index) != 0) {
		return true;
	}
	for (auto &child : op.children) {
		if (ContainsActiveRecursiveReference(*child, active_ctes)) {
			return true;
		}
	}
	return false;
}

RecursiveJoinProperties RecursiveJoinProperties::Extract(LogicalOperator &op,
                                                         const unordered_set<TableIndex> &active_ctes) {
	RecursiveJoinProperties result;
	result.build = JoinBuildProperties::Extract(op);
	result.depends_on_recursive_input = !active_ctes.empty() && ContainsActiveRecursiveReference(op, active_ctes);
	return result;
}

bool CanPlaceRecursiveProbe(JoinType type, const RecursiveJoinProperties &probe_side,
                            const RecursiveJoinProperties &build_side) {
	return probe_side.depends_on_recursive_input && !build_side.depends_on_recursive_input &&
	       !build_side.build.correlation_sensitive && !PropagatesBuildSide(type);
}

} // namespace duckdb
