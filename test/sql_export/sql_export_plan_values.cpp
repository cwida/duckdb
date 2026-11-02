#include "sql_export_test_helpers.hpp"
#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/parser/statement/logical_plan_statement.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/logical_plan_sql_exporter.hpp"
#include "duckdb/planner/sql_export_helpers.hpp"
#include "duckdb/planner/operator/logical_empty_result.hpp"
#include "duckdb/planner/operator/logical_expression_get.hpp"
#include "duckdb/planner/operator/logical_limit.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include <stdexcept>
#include <type_traits>
#include "logical_plan_sql_export_test_helpers.hpp"

using namespace duckdb;

namespace logical_plan_sql_export_test {

static void
RequireValuesEvaluationRejection(const LogicalPlanVerificationResult<LogicalPlanSQLExportRelation> &result) {
	REQUIRE(result.HasError());
	REQUIRE(result.GetIssues()[0].code == LogicalPlanVerificationIssueCode::UNSUPPORTED_EXPORT_FEATURE);
	REQUIRE(result.GetIssues()[0].construct ==
	        LogicalPlanVerificationConstructIdentity::ExportFeature("values_expression_evaluation"));
}

static unique_ptr<LogicalExpressionGet> ValuesFromProjection(Connection &connection, const string &sql,
                                                             idx_t row_count = 1) {
	auto plan = OptimizeLogicalPlanExportQuery(connection, sql);
	auto &projection = plan->Cast<LogicalProjection>();
	vector<vector<unique_ptr<Expression>>> rows;
	for (idx_t i = 0; i < row_count; i++) {
		vector<unique_ptr<Expression>> row;
		for (auto &expression : projection.expressions) {
			row.push_back(expression->Copy());
		}
		rows.push_back(std::move(row));
	}
	auto native = make_uniq<LogicalExpressionGet>(TableIndex(1000), projection.types, std::move(rows));
	native->children.push_back(std::move(projection.children[0]));
	native->ResolveOperatorTypes();
	return native;
}

static void CheckValuesRoundTrip(Connection &connection, unique_ptr<LogicalOperator> native) {
	native->ResolveOperatorTypes();
	auto exported = LogicalPlanSQLExporter::Export(*connection.context, *native);
	INFO((exported.HasError() ? exported.GetIssues()[0].message : string()));
	REQUIRE(exported.IsSuccess());
	auto sql = exported.GetValue().query->ToString();
	INFO(sql);
	auto direct = connection.Query(make_uniq<LogicalPlanStatement>(std::move(native)));
	REQUIRE_NO_FAIL(*direct);
	connection.Rollback();
	auto generated = connection.Query(sql);
	auto statement = make_uniq<SelectStatement>();
	statement->node = std::move(exported.GetValue().query);
	auto ast = connection.Query(std::move(statement));
	for (auto &result_ref : vector<reference<QueryResult>> {*generated, *ast}) {
		auto &result = result_ref.get();
		REQUIRE_NO_FAIL(result);
		REQUIRE(result.GetTypes() == direct->GetTypes());
		REQUIRE(SQLExportRows(result, false) == SQLExportRows(*direct, false));
	}
}

static bool AddValuesRows(unique_ptr<LogicalOperator> &op, idx_t row_count) {
	if (op->type != LogicalOperatorType::LOGICAL_EXPRESSION_GET) {
		for (auto &child : op->children) {
			if (AddValuesRows(child, row_count)) {
				return true;
			}
		}
		return false;
	}
	auto &input = op->Cast<LogicalExpressionGet>();
	if (input.expr_types.size() != 2) {
		return false;
	}
	auto table_index = input.table_index;
	input.table_index = TableIndex(1000000);
	vector<vector<unique_ptr<Expression>>> rows;
	for (idx_t i = 0; i < row_count; i++) {
		vector<unique_ptr<Expression>> row;
		for (idx_t column = 0; column < input.expr_types.size(); column++) {
			row.push_back(make_uniq<BoundColumnRefExpression>(
			    input.expr_types[column], ColumnBinding(input.table_index, ProjectionIndex(column))));
		}
		REQUIRE(row.size() == 2);
		row[1] = PlanIntegerConstant(NumericCast<int32_t>(i));
		rows.push_back(std::move(row));
	}
	auto get = make_uniq<LogicalExpressionGet>(table_index, input.expr_types, std::move(rows));
	get->children.push_back(std::move(op));
	op = std::move(get);
	return true;
}

TEST_CASE("Logical plan SQL export rejects effectful VALUES even with empty input",
          "[sql_export][logical_plan_sql_export]") {
	DuckDB db(nullptr);
	Connection connection(db);
	REQUIRE_NO_FAIL(connection.Query("CREATE SEQUENCE empty_values_guard"));
	connection.BeginTransaction();
	auto values = ValuesFromProjection(connection, "SELECT x,nextval('empty_values_guard') FROM (VALUES(1),(2))t(x)");
	values->children[0] = make_uniq<LogicalEmptyResult>(std::move(values->children[0]));
	values->ResolveOperatorTypes();
	RequireValuesEvaluationRejection(LogicalPlanSQLExporter::Export(*connection.context, *values));
	auto effect = connection.Query("SELECT currval('empty_values_guard')");
	REQUIRE(effect->HasError());
	REQUIRE(StringUtil::Contains(effect->GetError(), "sequence is not yet defined"));
	connection.Rollback();
}

TEST_CASE("Logical plan SQL export checks consumers of multirow VALUES", "[sql_export][logical_plan_sql_export]") {
	DuckDB db(nullptr);
	Connection connection(db);
	REQUIRE_NO_FAIL(connection.Query("SET threads=1"));
	const string input = " FROM (VALUES (2,0),(NULL,1),(2,2),(1,3))t(x,r)";
	vector<string> queries {
	    "SELECT x,r" + input,
	    "SELECT x,r" + input + " WHERE x=0",
	    "SELECT x,r" + input + " ORDER BY x,r LIMIT 2",
	    "SELECT list_sort(list(x))" + input,
	    "SELECT array_sort(list(x))" + input,
	    "SELECT list_sort(list(x), 'DESC', 'NULLS FIRST')" + input,
	    "SELECT length(list_sort(list(x)))" + input,
	    "SELECT list(x ORDER BY x)" + input,
	    "SELECT min(x),count(x)" + input,
	    "SELECT r,min(x)" + input + " GROUP BY r",
	    "SELECT r,list_sort(list(x))" + input + " GROUP BY r",
	    "SELECT DISTINCT x,r" + input,
	    "(SELECT x,r" + input + ") UNION ALL (SELECT 2,3)",
	    "SELECT sum(x),sum(r)" + input + " JOIN (VALUES (1),(2))u(y) ON x=y",
	    "SELECT sum(x),sum(r)" + input + " LEFT JOIN (VALUES (1),(2))u(y) ON x=y",
	};
	for (auto &query : queries) {
		CAPTURE(query);
		auto disabled = StringUtil::Contains(query, "list(x ORDER BY") ? "aggregate_function_rewriter" : "";
		REQUIRE_NO_FAIL(connection.Query("SET disabled_optimizers='" + string(disabled) + "'"));
		connection.BeginTransaction();
		auto native = OptimizeLogicalPlanExportQuery(connection, query);
		REQUIRE(AddValuesRows(native, 3));
		native->ResolveOperatorTypes();
		CheckValuesRoundTrip(connection, std::move(native));
	}
	REQUIRE_NO_FAIL(connection.Query("SET disabled_optimizers=''"));
	connection.BeginTransaction();
	auto rewritten = OptimizeLogicalPlanExportQuery(connection, "SELECT list(x ORDER BY x)" + input);
	REQUIRE(AddValuesRows(rewritten, 3));
	CheckValuesRoundTrip(connection, std::move(rewritten));
}

TEST_CASE("Logical plan SQL export verifies unordered VALUES list canonicalizers",
          "[sql_export][logical_plan_sql_export]") {
	SECTION("list_sort aliases") {
		DuckDB db(nullptr);
		Connection connection(db);
		REQUIRE_NO_FAIL(connection.Query("SET threads=1"));
		for (const auto &name : {"list_sort", "array_sort"}) {
			for (bool binary : {false, true}) {
				CAPTURE(name, binary);
				connection.BeginTransaction();
				auto plan = OptimizeLogicalPlanExportQuery(
				    connection, "SELECT " + string(name) + "(list(x)) FROM (VALUES ('b',0),('B',1))t(x,r)");
				REQUIRE(AddValuesRows(plan, 3));
				plan->ResolveOperatorTypes();
				if (binary) {
					plan = plan->Copy(*connection.context);
					plan->ResolveOperatorTypes();
				}
				CheckValuesRoundTrip(connection, std::move(plan));
			}
		}
	}

	SECTION("collation-equivalent payloads") {
		DuckDB db(nullptr);
		Connection connection(db);
		REQUIRE_NO_FAIL(connection.Query("SET threads=1"));
		REQUIRE_NO_FAIL(connection.Query("SET default_collation='nocase'"));
		for (bool binary : {false, true}) {
			CAPTURE(binary);
			connection.BeginTransaction();
			auto plan = OptimizeLogicalPlanExportQuery(connection,
			                                           "SELECT list_sort(list(x)) FROM (VALUES ('b',0),('B',1))t(x,r)");
			REQUIRE(AddValuesRows(plan, 3));
			plan->ResolveOperatorTypes();
			if (binary) {
				plan = plan->Copy(*connection.context);
				plan->ResolveOperatorTypes();
			}
			auto native_plan = plan->Copy(*connection.context);
			native_plan->ResolveOperatorTypes();
			auto exported = LogicalPlanSQLExporter::Export(*connection.context, *plan);
			auto direct = connection.Query(make_uniq<LogicalPlanStatement>(std::move(native_plan)));
			REQUIRE_NO_FAIL(*direct);
			vector<Value> expected;
			for (idx_t i = 0; i < 3; i++) {
				expected.push_back(Value("b"));
				expected.push_back(Value("B"));
			}
			REQUIRE(Value::NotDistinctFrom(direct->Collection().GetValue(0, 0),
			                               Value::LIST(LogicalType::VARCHAR, std::move(expected))));
			connection.Rollback();
			INFO((exported.HasError() ? exported.GetIssues()[0].message : string()));
			REQUIRE(exported.IsSuccess());
			auto text = exported.GetValue().query->ToString();
			auto generated = connection.Query(text);
			REQUIRE_NO_FAIL(*generated);
			REQUIRE(generated->GetTypes() == direct->GetTypes());
			auto statement = make_uniq<SelectStatement>();
			statement->node = std::move(exported.GetValue().query);
			auto ast = connection.Query(std::move(statement));
			REQUIRE_NO_FAIL(*ast);
			REQUIRE(ast->GetTypes() == direct->GetTypes());
			for (auto result : {generated.get(), ast.get()}) {
				auto value = result->Collection().GetValue(0, 0);
				auto &values = ListValue::GetChildren(value);
				REQUIRE(values.size() == 6);
				REQUIRE(std::count(values.begin(), values.end(), Value("b")) == 3);
				REQUIRE(std::count(values.begin(), values.end(), Value("B")) == 3);
			}
		}
	}

	SECTION("collation setting changes after binding") {
		const string sql = "SELECT list_sort(list(x)) FROM (VALUES ('a',0),('B',1))t(x,r)";
		auto expected_value = [](bool bind_nocase, bool multirow_values) {
			vector<Value> expected;
			idx_t row_count = multirow_values ? 3 : 1;
			for (idx_t i = 0; i < row_count; i++) {
				if (bind_nocase) {
					expected.insert(expected.begin(), Value("a"));
					expected.push_back(Value("B"));
				} else {
					expected.insert(expected.begin(), Value("B"));
					expected.push_back(Value("a"));
				}
			}
			return Value::LIST(LogicalType::VARCHAR, std::move(expected));
		};

		SECTION("bind data copy retains the sort key") {
			for (bool bind_nocase : {false, true}) {
				for (bool multirow_values : {false, true}) {
					CAPTURE(bind_nocase, multirow_values);
					DuckDB db(nullptr);
					Connection connection(db);
					REQUIRE_NO_FAIL(connection.Query(bind_nocase ? "SET threads=1; SET default_collation='nocase'"
					                                             : "SET threads=1; SET default_collation=''"));
					connection.BeginTransaction();
					auto plan = OptimizeLogicalPlanExportQuery(connection, sql);
					if (multirow_values) {
						REQUIRE(AddValuesRows(plan, 3));
					}
					plan->ResolveOperatorTypes();
					REQUIRE_NO_FAIL(
					    connection.Query(bind_nocase ? "SET default_collation=''" : "SET default_collation='nocase'"));
					plan = plan->Copy(*connection.context);
					plan->ResolveOperatorTypes();
					REQUIRE_NO_FAIL(connection.Query("SET debug_disable_optimizer=true"));
					auto direct = connection.Query(make_uniq<LogicalPlanStatement>(std::move(plan)));
					REQUIRE_NO_FAIL(connection.Query("SET debug_disable_optimizer=false"));
					REQUIRE_NO_FAIL(*direct);
					REQUIRE(Value::NotDistinctFrom(direct->Collection().GetValue(0, 0),
					                               expected_value(bind_nocase, multirow_values)));
					connection.Rollback();
				}
			}
		}

		SECTION("export retains the bound sort key") {
			for (bool bind_nocase : {false, true}) {
				for (bool binary : {false, true}) {
					CAPTURE(bind_nocase, binary);
					DuckDB db(nullptr);
					Connection connection(db);
					REQUIRE_NO_FAIL(connection.Query(bind_nocase ? "SET threads=1; SET default_collation='nocase'"
					                                             : "SET threads=1; SET default_collation=''"));
					connection.BeginTransaction();
					auto plan = OptimizeLogicalPlanExportQuery(connection, sql);
					REQUIRE(AddValuesRows(plan, 3));
					plan->ResolveOperatorTypes();
					if (binary) {
						plan = plan->Copy(*connection.context);
						plan->ResolveOperatorTypes();
					}
					REQUIRE_NO_FAIL(
					    connection.Query(bind_nocase ? "SET default_collation=''" : "SET default_collation='nocase'"));
					auto exported = LogicalPlanSQLExporter::Export(*connection.context, *plan);
					INFO((exported.HasError() ? exported.GetIssues()[0].message : string()));
					REQUIRE(exported.IsSuccess());

					auto generated = connection.Query(exported.GetValue().query->ToString());
					REQUIRE_NO_FAIL(*generated);
					REQUIRE(Value::NotDistinctFrom(generated->Collection().GetValue(0, 0),
					                               expected_value(bind_nocase, true)));
					connection.Rollback();
				}
			}
		}
	}
}

TEST_CASE("Logical plan SQL export retains unused VALUES field evaluation", "[sql_export][logical_plan_sql_export]") {
	DuckDB db(nullptr);
	Connection connection(db);
	connection.BeginTransaction();
	auto values = ValuesFromProjection(connection, "SELECT x,CAST(x AS INTEGER) FROM (VALUES('1'),('bad'))t(x)");
	vector<unique_ptr<Expression>> expressions;
	expressions.push_back(
	    make_uniq<BoundColumnRefExpression>(LogicalType::VARCHAR, ColumnBinding(TableIndex(1000), ProjectionIndex(0))));
	auto projection = PlanProjection(TableIndex(1001), std::move(values), std::move(expressions));
	RequireValuesEvaluationRejection(LogicalPlanSQLExporter::Export(*connection.context, *projection));
	auto direct = connection.Query(make_uniq<LogicalPlanStatement>(std::move(projection)));
	REQUIRE(direct->HasError());
	connection.Rollback();
}

TEST_CASE("Logical plan SQL export rejects a throwing later VALUES row below LIMIT",
          "[sql_export][logical_plan_sql_export]") {
	DuckDB db(nullptr);
	Connection connection(db);
	REQUIRE_NO_FAIL(connection.Query("SET threads=1"));
	connection.BeginTransaction();
	auto input = StringUtil::Repeat("(1),", STANDARD_VECTOR_SIZE / 2) + "(1)";
	auto values = ValuesFromProjection(
	    connection, "SELECT x,error('VALUES row reached')::INTEGER FROM (VALUES " + input + ")t(x)", 2);
	values->expressions[0][1] = values->expressions[0][0]->Copy();
	auto limit = make_uniq<LogicalLimit>(BoundLimitNode::ConstantValue(1), BoundLimitNode());
	limit->children.push_back(std::move(values));
	limit->ResolveOperatorTypes();
	auto exported = LogicalPlanSQLExporter::Export(*connection.context, *limit);
	RequirePlanExportIssue(exported, LogicalPlanVerificationIssueCode::UNSUPPORTED_EXPORT_FEATURE,
	                       {LogicalPlanVerificationPathRoot::LOGICAL_PLAN,
	                        {{LogicalPlanVerificationPathComponentType::OPERATOR_CHILD, 0}}});
	REQUIRE(exported.GetIssues()[0].construct ==
	        LogicalPlanVerificationConstructIdentity::ExportFeature("values_expression_evaluation"));
	auto direct = connection.Query(make_uniq<LogicalPlanStatement>(std::move(limit)));
	REQUIRE_NO_FAIL(*direct);
	REQUIRE(direct->RowCount() == 1);
	connection.Rollback();
}

} // namespace logical_plan_sql_export_test
