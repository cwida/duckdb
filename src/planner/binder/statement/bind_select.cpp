#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/bound_query_node.hpp"
#include "duckdb/planner/operator/logical_get.hpp"

namespace duckdb {

BoundStatement Binder::Bind(SelectStatement &stmt) {
	auto &properties = GetStatementProperties();
	properties.return_type = StatementReturnType::QUERY_RESULT;
	auto result = Bind(*stmt.node);
	// A bare table-function passthrough honors the function's call_return_type (e.g. a CONNECT-routed
	// DDL/SET reports as NOTHING), but only when the function opts in via a non-default modifier.
	if (result.plan) {
		auto get = GetPassthroughTableFunctionGet(*result.plan);
		if (get && get->function.call_return_type != StatementReturnType::QUERY_RESULT) {
			properties.return_type = get->function.call_return_type;
		}
	}
	return result;
}

} // namespace duckdb
