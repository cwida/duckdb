#include "duckdb/planner/binder.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/parser/statement/explain_statement.hpp"
#include "duckdb/planner/operator/logical_explain.hpp"
#include "duckdb/common/tree_renderer.hpp"

namespace duckdb {

BoundStatement Binder::Bind(ExplainStatement &stmt) {
	BoundStatement result;
	if (stmt.explain_type == ExplainType::EXPLAIN_SQL) {
		switch (stmt.stmt->type) {
		case StatementType::SELECT_STATEMENT:
		case StatementType::INSERT_STATEMENT:
		case StatementType::UPDATE_STATEMENT:
		case StatementType::DELETE_STATEMENT:
		case StatementType::MERGE_INTO_STATEMENT:
		case StatementType::COPY_STATEMENT:
			break;
		case StatementType::CREATE_STATEMENT: {
			auto &info = *stmt.stmt->Cast<CreateStatement>().info;
			if (info.type == CatalogType::TABLE_ENTRY && info.Cast<CreateTableInfo>().query) {
				break;
			}
			throw NotImplementedException("EXPLAIN (SQL) supports SELECT, VALUES, WITH, INSERT, UPDATE, DELETE, "
			                              "MERGE INTO, CREATE TABLE ... AS and COPY ... TO statements only");
		}
		default:
			throw NotImplementedException("EXPLAIN (SQL) supports SELECT, VALUES, WITH, INSERT, UPDATE, DELETE, "
			                              "MERGE INTO, CREATE TABLE ... AS and COPY ... TO statements only");
		}
	}

	// bind the underlying statement
	auto plan = Bind(*stmt.stmt);
	// render the unoptimized logical plan, but only when it will be shown: a plain EXPLAIN in a multi-plan format.
	// (it is unused for EXPLAIN ANALYZE, and single-plan formats like FORMAT WEB render only the final plan)
	string logical_plan_unopt;
	if (stmt.explain_type == ExplainType::EXPLAIN_STANDARD) {
		auto renderer = TreeRenderer::CreateRenderer(context, stmt.format);
		if (!renderer || !renderer->RendersSinglePlan()) {
			logical_plan_unopt = plan.plan->ToString(context, stmt.format);
		}
	}
	auto explain = make_uniq<LogicalExplain>(std::move(plan.plan), stmt.explain_type, stmt.format);
	explain->logical_plan_unopt = logical_plan_unopt;
	if (stmt.explain_type == ExplainType::EXPLAIN_SQL) {
		explain->sql_output_names = std::move(plan.names);
		explain->allow_unsupported_sql = stmt.allow_unsupported_sql;
	}

	result.plan = std::move(explain);
	result.names = {"explain_key", "explain_value"};
	result.types = {LogicalType::VARCHAR, LogicalType::VARCHAR};

	auto &properties = GetStatementProperties();
	properties.return_type = StatementReturnType::QUERY_RESULT;
	return result;
}

} // namespace duckdb
