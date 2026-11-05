#include "duckdb/parser/statement/detach_statement.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/operator/logical_detach.hpp"
#include "duckdb/main/config.hpp"

namespace duckdb {

BoundStatement Binder::Bind(DetachStatement &stmt) {
	BoundStatement result;

	result.plan = make_uniq<LogicalDetach>(std::move(stmt.info));
	result.names = {"Success"};
	result.types = {LogicalType::BOOLEAN};

	auto &properties = GetStatementProperties();
	properties.return_type = StatementReturnType::NOTHING;
	return result;
}

} // namespace duckdb
