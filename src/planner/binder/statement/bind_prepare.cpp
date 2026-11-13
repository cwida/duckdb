#include "duckdb/planner/binder.hpp"
#include "duckdb/parser/statement/prepare_statement.hpp"
#include "duckdb/planner/planner.hpp"
#include "duckdb/planner/operator/logical_prepare.hpp"

namespace duckdb {

BoundStatement Binder::Bind(PrepareStatement &stmt) {
	Planner prepared_planner(context);
	auto prepared_data = prepared_planner.PrepareSQLStatement(std::move(stmt.statement));
	global_binder_state->bound_tables = prepared_planner.binder->global_binder_state->bound_tables;

	if (prepared_planner.properties.always_require_rebind) {
		// we always need to rebind - don't keep the plan around
		prepared_planner.plan.reset();
	}

	auto prepare = make_uniq<LogicalPrepare>(stmt.name, std::move(prepared_data), std::move(prepared_planner.plan));
	// we can always prepare, even if the transaction has been invalidated
	// this is required because most clients ALWAYS invoke prepared statements
	auto &properties = GetStatementProperties();
	properties.requires_valid_transaction = false;
	properties.bound_all_parameters = true;
	properties.parameter_count = 0;
	properties.return_type = StatementReturnType::NOTHING;

	BoundStatement result;
	result.names = {"Success"};
	result.types = {LogicalType::BOOLEAN};
	result.plan = std::move(prepare);
	return result;
}

} // namespace duckdb
