#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/statement/external_resource_statement.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression_binder/table_function_binder.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/planner/operator/logical_external_resource.hpp"

namespace duckdb {

Value Binder::BindExternalResourceValue(unique_ptr<ParsedExpression> &expr) {
	TableFunctionBinder binder(*this, context, "External resource", "External resource parameter");
	auto bound = binder.Bind(expr);
	return ExpressionExecutor::EvaluateScalar(context, *bound, true);
}

void Binder::BindExternalResourceParams(case_insensitive_map_t<unique_ptr<ParsedExpression>> &parsed_params,
                                        unordered_map<string, Value> &params) {
	for (auto &entry : parsed_params) {
		params[entry.first] = BindExternalResourceValue(entry.second);
	}
}

BoundStatement Binder::Bind(ExternalResourceStatement &stmt) {
	BoundStatement result;
	result.types = {LogicalType::BOOLEAN};
	result.names = {"Success"};

	// Every verb runs the resource type's callbacks (create/status, resolve, destroy), which reach outside the
	// database - refused up front, before the statement is looked at any further.
	if (!Settings::Get<EnableExternalAccessSetting>(context)) {
		throw PermissionException("External resources are disabled through configuration");
	}

	BoundExternalResource data;
	data.operation = stmt.operation;
	data.name = stmt.name.GetIdentifierName();

	// A durable resource is registered under its name, so every verb needs one.
	if (data.name.empty()) {
		throw BinderException("EXTERNAL RESOURCE requires a name (AS <name>)");
	}

	if (stmt.operation != ExternalResourceOperation::DESTROY) {
		// CREATE / REGISTER: the resource type is a string literal; only the create params need resolving.
		data.type = stmt.type;
		BindExternalResourceParams(stmt.options, data.params);
		if (stmt.operation == ExternalResourceOperation::REGISTER) {
			data.handle = BindExternalResourceValue(stmt.handle);
			if (data.handle.IsNull()) {
				throw BinderException("REGISTER EXTERNAL RESOURCE: the handle must not be NULL");
			}
		}
	}

	result.plan = make_uniq<LogicalExternalResource>(std::move(data));

	auto &properties = GetStatementProperties();
	properties.return_type = StatementReturnType::NOTHING;
	return result;
}

} // namespace duckdb
