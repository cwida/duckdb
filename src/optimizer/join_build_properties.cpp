#include "duckdb/optimizer/join_build_properties.hpp"

#include "duckdb/planner/logical_operator_repeatability.hpp"

namespace duckdb {

JoinBuildProperties JoinBuildProperties::Extract(LogicalOperator &op) {
	JoinBuildProperties result;
	result.repeatable = ClassifyLogicalOperatorRepeatability(op) == LogicalOperatorRepeatability::REPEATABLE;
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_DELIM_GET:
	case LogicalOperatorType::LOGICAL_DELIM_JOIN:
	case LogicalOperatorType::LOGICAL_DEPENDENT_JOIN:
		result.correlation_sensitive = true;
		break;
	default:
		break;
	}
	for (auto &child : op.children) {
		auto properties = Extract(*child);
		result.correlation_sensitive |= properties.correlation_sensitive;
		result.repeatable &= properties.repeatable;
	}
	return result;
}

} // namespace duckdb
