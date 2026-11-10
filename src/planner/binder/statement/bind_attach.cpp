#include "duckdb/planner/binder.hpp"
#include "duckdb/parser/statement/attach_statement.hpp"
#include "duckdb/parser/parsed_data/external_resource_options.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/planner/operator/logical_attach.hpp"
#include "duckdb/planner/expression_binder/table_function_binder.hpp"
#include "duckdb/execution/expression_executor.hpp"

namespace duckdb {

void Binder::BindExternalResource(ExternalResourceOptions &external_resource) {
	// Reference form (`EXTERNAL RESOURCE <name>`): the resource already exists — the physical operator
	// looks it up in the manager at execution. Nothing to resolve here.
	if (!external_resource.reference_name.empty()) {
		return;
	}

	// Create form (`NEW TEMPORARY EXTERNAL RESOURCE '<type>' (opts)`): the type is a string literal set by the
	// parser, so only the create params need resolving.
	BindExternalResourceParams(external_resource.parsed_params, external_resource.params);
	external_resource.parsed_params.clear();
}

BoundStatement Binder::Bind(AttachStatement &stmt) {
	BoundStatement result;
	result.types = {LogicalType::BOOLEAN};
	result.names = {"Success"};

	// resolve the path expression if set by the parser
	if (stmt.info->parsed_path) {
		TableFunctionBinder path_binder(*this, context, "Attach", "Attach path");
		auto bound_path = path_binder.Bind(stmt.info->parsed_path);
		auto path_val = ExpressionExecutor::EvaluateScalar(context, *bound_path, true);
		if (path_val.IsNull()) {
			throw BinderException("ATTACH path expression must not evaluate to NULL");
		}
		stmt.info->path = path_val.ToString();
		stmt.info->parsed_path.reset();
	}

	// bind the options
	TableFunctionBinder option_binder(*this, context, "Attach", "Attach parameter");
	unordered_map<string, Value> kv_options;
	for (auto &entry : stmt.info->parsed_options) {
		auto bound_expr = option_binder.Bind(entry.second);
		auto val = ExpressionExecutor::EvaluateScalar(context, *bound_expr, true);
		if (val.IsNull()) {
			throw BinderException("NULL is not supported as a valid option for ATTACH option \"" + entry.first + "\"");
		}
		stmt.info->options[entry.first] = std::move(val);
	}
	stmt.info->parsed_options.clear();

	// Bind the external resource clause (ATTACH/CONNECT TO EXTERNAL RESOURCE ...): resolve the type + create params.
	if (stmt.info->external_resource) {
		BindExternalResource(*stmt.info->external_resource);
	}

	result.plan = make_uniq<LogicalAttach>(std::move(stmt.info));

	auto &properties = GetStatementProperties();
	properties.return_type = StatementReturnType::NOTHING;
	return result;
}

} // namespace duckdb
