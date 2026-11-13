#include "logical_plan_sql_export_test_helpers.hpp"
#include "duckdb/planner/operator/logical_dummy_scan.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/planner.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"

namespace logical_plan_sql_export_test {

unique_ptr<LogicalOperator> OptimizeLogicalPlanExportQuery(Connection &connection, const string &query) {
	Parser parser(*connection.context);
	parser.ParseQuery(query);
	REQUIRE(parser.statements.size() == 1);
	Planner planner(*connection.context);
	planner.CreatePlan(std::move(parser.statements[0]));
	Optimizer optimizer(*planner.binder, *connection.context);
	return optimizer.Optimize(std::move(planner.plan));
}

optional_ptr<LogicalOperator> FindLogicalPlanExportOperator(LogicalOperator &op, LogicalOperatorType type) {
	if (op.type == type) {
		return op;
	}
	for (auto &child : op.children) {
		if (!child) {
			continue;
		}
		auto result = FindLogicalPlanExportOperator(*child, type);
		if (result) {
			return result;
		}
	}
	return nullptr;
}

void RequireLogicalPlanExportIssue(const PlanExportResult &result, LogicalPlanVerificationIssueCode code,
                                   LogicalPlanVerificationPhase phase, const LogicalPlanVerificationPath &path) {
	REQUIRE(result.IsValid());
	REQUIRE(result.HasError());
	REQUIRE_FALSE(result.IsSuccess());
	REQUIRE(result.GetIssues().size() == 1);
	INFO(result.GetIssues()[0].message);
	REQUIRE(result.GetIssues()[0].code == code);
	REQUIRE(result.GetIssues()[0].phase == phase);
	REQUIRE(result.GetIssues()[0].path == optional<LogicalPlanVerificationPath>(path));
}

void RequirePlanExportIssue(const PlanExportResult &result, LogicalPlanVerificationIssueCode code,
                            const LogicalPlanVerificationPath &path) {
	RequireLogicalPlanExportIssue(result, code, LogicalPlanVerificationPhase::PLAN_EXPORT, path);
}

unique_ptr<Expression> PlanIntegerConstant(int32_t value) {
	return make_uniq<BoundConstantExpression>(Value::INTEGER(value));
}

unique_ptr<LogicalExpressionGet> IntegerValues(TableIndex table_index,
                                               std::initializer_list<std::initializer_list<int32_t>> input_rows) {
	vector<vector<unique_ptr<Expression>>> rows;
	idx_t column_count = 0;
	for (auto &input_row : input_rows) {
		vector<unique_ptr<Expression>> row;
		for (auto value : input_row) {
			row.push_back(PlanIntegerConstant(value));
		}
		column_count = row.size();
		rows.push_back(std::move(row));
	}
	vector<LogicalType> types(column_count, LogicalType::INTEGER);
	auto result = make_uniq<LogicalExpressionGet>(table_index, std::move(types), std::move(rows));
	result->children.push_back(make_uniq<LogicalDummyScan>(TableIndex(table_index.index + 1000)));
	return result;
}

unique_ptr<LogicalProjection> PlanProjection(TableIndex table_index, unique_ptr<LogicalOperator> child,
                                             vector<unique_ptr<Expression>> expressions) {
	auto result = make_uniq<LogicalProjection>(table_index, std::move(expressions));
	result->children.push_back(std::move(child));
	return result;
}

vector<string> SQLExportRows(QueryResult &result, bool ordered) {
	vector<string> rows;
	auto result_rows = result.Collection().GetRows();
	for (idx_t row = 0; row < result_rows.size(); row++) {
		string text;
		for (idx_t col = 0; col < result.ColumnCount(); col++) {
			auto value = result_rows.GetValue(col, row).ToSQLString();
			text += to_string(value.size()) + ":" + value;
		}
		rows.push_back(std::move(text));
	}
	if (!ordered) {
		std::sort(rows.begin(), rows.end());
	}
	return rows;
}

} // namespace logical_plan_sql_export_test
