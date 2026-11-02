#include "duckdb/optimizer/remove_redundant_order_keys.hpp"

#include "duckdb/optimizer/apply_functional_dependencies.hpp"

namespace duckdb {

void RemoveRedundantOrderKeys::Optimize(LogicalOperator &op) {
	ApplyFunctionalDependencies visitor;
	visitor.VisitOperator(op);
}

} // namespace duckdb
