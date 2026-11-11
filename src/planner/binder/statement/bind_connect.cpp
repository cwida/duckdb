#include "duckdb/parser/statement/connect_statement.hpp"
#include "duckdb/parser/statement/disconnect_statement.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/operator/logical_connect.hpp"
#include "duckdb/planner/operator/logical_disconnect.hpp"
#include "duckdb/planner/expression_binder/table_function_binder.hpp"
#include "duckdb/execution/expression_executor.hpp"

namespace duckdb {

BoundStatement Binder::Bind(ConnectStatement &stmt) {
	BoundStatement result;
	result.types = {LogicalType::BOOLEAN};
	result.names = {"Success"};

	// Bind the connection-string options (if any) and fold them into the bound options map.
	// NOTE: mirrors the option binding in bind_attach; candidate for a shared Binder helper.
	if (!stmt.info->parsed_options.empty()) {
		TableFunctionBinder option_binder(*this, context, "Connect", "Connect parameter");
		for (auto &entry : stmt.info->parsed_options) {
			auto bound_expr = option_binder.Bind(entry.second);
			auto val = ExpressionExecutor::EvaluateScalar(context, *bound_expr, true);
			if (val.IsNull()) {
				throw BinderException("NULL is not supported as a valid option for CONNECT option \"" + entry.first +
				                      "\"");
			}
			stmt.info->options[entry.first] = std::move(val);
		}
		stmt.info->parsed_options.clear();
	}

	// Bind the external resource clause (ATTACH/CONNECT TO EXTERNAL RESOURCE ...): resolve the type + create params.
	if (stmt.info->external_resource) {
		BindExternalResource(*stmt.info->external_resource);
	}

	result.plan = make_uniq<LogicalConnect>(std::move(stmt.info));

	auto &properties = GetStatementProperties();
	properties.return_type = StatementReturnType::NOTHING;
	return result;
}

BoundStatement Binder::Bind(DisconnectStatement &stmt) {
	BoundStatement result;
	result.types = {LogicalType::BOOLEAN};
	result.names = {"Success"};

	result.plan = make_uniq<LogicalDisconnect>(std::move(stmt.info));

	auto &properties = GetStatementProperties();
	properties.return_type = StatementReturnType::NOTHING;
	// DISCONNECT returns to LOCAL also in an aborted transaction, so that it can be rolled back
	properties.requires_valid_transaction = false;
	return result;
}

} // namespace duckdb
