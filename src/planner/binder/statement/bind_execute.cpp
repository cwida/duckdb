#include "duckdb/planner/binder.hpp"
#include "duckdb/main/prepared_statement.hpp"
#include "duckdb/main/prepared_statement_data.hpp"
#include "duckdb/parser/statement/execute_statement.hpp"
#include "duckdb/planner/planner.hpp"
#include "duckdb/planner/expression_binder/constant_binder.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_context_state.hpp"

namespace duckdb {

PreparedExecution Binder::ResolveExecute(ExecuteStatement &stmt) {
	auto parameter_count = stmt.named_param_map.size();

	// bind the prepared statement
	auto &client_data = ClientData::Get(context);

	auto entry = client_data.prepared_statements.find(stmt.name);
	if (entry == client_data.prepared_statements.end()) {
		throw BinderException("Prepared statement %s does not exist", stmt.name);
	}

	// check if we need to rebind the prepared statement
	// this happens if the catalog changes, since in this case e.g. tables we relied on may have been deleted
	auto prepared = entry->second;
	auto &named_param_map = prepared->unbound_statement->named_param_map;

	// values that were supplied pre-typed do not go through the literal binding below
	identifier_map_t<BoundParameterData> bind_values = stmt.bound_values;
	if (stmt.bound_values.empty()) {
		PreparedStatement::VerifyParameters(stmt.named_values, named_param_map, &context);
	} else {
		PreparedStatement::VerifyParameters(stmt.bound_values, named_param_map, &context);
	}

	auto &mapped_named_values = stmt.named_values;
	// bind any supplied parameters
	auto constant_binder = Binder::CreateBinder(context);
	constant_binder->SetCanContainNulls(true);
	for (auto &pair : mapped_named_values) {
		bool is_literal = pair.second->GetExpressionType() == ExpressionType::VALUE_CONSTANT;

		ConstantBinder cbinder(*constant_binder, context, "EXECUTE statement");
		auto bound_expr = cbinder.Bind(pair.second);
		BoundParameterData parameter_data;
		if (is_literal) {
			auto &constant = bound_expr->Cast<BoundConstantExpression>();
			LogicalType return_type;
			if (constant.GetReturnType() == LogicalTypeId::VARCHAR &&
			    StringType::GetCollation(constant.GetReturnType()).empty()) {
				return_type = LogicalTypeId::STRING_LITERAL;
			} else if (constant.GetReturnType().IsIntegral()) {
				return_type = LogicalType::INTEGER_LITERAL(constant.GetValue());
			} else {
				return_type = constant.GetValue().type();
			}
			parameter_data = BoundParameterData(constant.GetValue(), std::move(return_type));
		} else {
			auto value = ExpressionExecutor::EvaluateScalar(context, *bound_expr, true);
			auto value_type = value.type();
			parameter_data = BoundParameterData(std::move(value), std::move(value_type));
		}
		bind_values[pair.first] = std::move(parameter_data);
	}
	prepared->PopulateMissingParameterValues(context, bind_values);
	RebindQueryInfo rebind = RebindQueryInfo::DO_NOT_REBIND;
	if (prepared->RequireRebind(context, bind_values)) {
		rebind = RebindQueryInfo::ATTEMPT_TO_REBIND;
	}
	for (auto &state : context.GetContext().GetRegisteredState().States()) {
		BindPreparedStatementCallbackInfo info {*prepared, bind_values};
		auto new_rebind = state->OnRebindPreparedStatement(context, info, rebind);
		if (new_rebind == RebindQueryInfo::ATTEMPT_TO_REBIND) {
			rebind = RebindQueryInfo::ATTEMPT_TO_REBIND;
		}
	}
	PreparedExecution result;
	result.prepared = std::move(prepared);
	result.bind_values = std::move(bind_values);
	result.rebind = rebind == RebindQueryInfo::ATTEMPT_TO_REBIND;
	result.parameter_count = parameter_count;
	return result;
}

BoundStatement Binder::BindExecute(PreparedExecution &execution) {
	// plan the prepared statement again with the supplied parameter values
	Planner prepared_planner(context);
	prepared_planner.parameter_data = execution.bind_values;
	auto prepared = prepared_planner.PrepareSQLStatement(execution.prepared->unbound_statement->Copy());
	D_ASSERT(prepared->properties.bound_all_parameters);
	global_binder_state->bound_tables = prepared_planner.binder->global_binder_state->bound_tables;

	// copy the properties of the prepared statement into the planner
	auto &properties = GetStatementProperties();
	properties = prepared->properties;
	properties.parameter_count = execution.parameter_count;

	prepared->Bind(context, execution.bind_values);

	BoundStatement result;
	result.names = prepared->names;
	result.types = prepared->types;
	result.plan = std::move(prepared_planner.plan);
	return result;
}

BoundStatement Binder::Bind(ExecuteStatement &stmt) {
	auto execution = ResolveExecute(stmt);
	return BindExecute(execution);
}

} // namespace duckdb
