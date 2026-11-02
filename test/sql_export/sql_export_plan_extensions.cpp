#include "logical_plan_sql_export_test_helpers.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_default_expression.hpp"
#include "duckdb/planner/operator/logical_explain.hpp"

namespace logical_plan_sql_export_test {

static unique_ptr<SQLExportExtensionOperator> LeafExtension(const string &name, TableIndex index) {
	return make_uniq<SQLExportExtensionOperator>(name, vector<ColumnBinding> {{index, ProjectionIndex(0)}},
	                                             vector<LogicalType> {LogicalType::INTEGER},
	                                             vector<TableIndex> {index});
}

static PlanExportResult ExportExtensionProjection(SQLExportExtensionOperator &op, LogicalPlanSQLExportContext &context,
                                                  const LogicalPlanVerificationPath &path) {
	auto child = context.ExportChild(*op.children[0], LogicalPlanSQLExportHelpers::PlanChildPath(path, 0));
	if (child.HasError()) {
		return PlanExportResult::Failure(child.GetIssues());
	}
	auto bindings = LogicalPlanSQLExportHelpers::CreateBindingContext(context.GetClientContext(), {child.GetValue()});
	auto select = make_uniq<SelectNode>();
	select->from_table = LogicalPlanSQLExportHelpers::CreateSubquery(std::move(child.GetValue()));
	auto expressions = LogicalPlanSQLExportHelpers::CollectExpressions(op);
	for (idx_t i = 0; i < expressions.size(); i++) {
		auto expression = context.ExportExpression(op, expressions, i, bindings, path);
		if (expression.HasError()) {
			return PlanExportResult::Failure(expression.GetIssues());
		}
		select->select_list.push_back(std::move(expression.GetValue()));
	}
	return op.ExportQuery(std::move(select), path);
}

TEST_CASE("Extension operators without SQL reconstruction remain unsupported",
          "[sql_export][logical_plan_sql_export]") {
	DuckDB db;
	Connection connection(db);
	auto plan = LeafExtension("unhandled_extension", TableIndex(82));
	RequirePlanExportIssue(LogicalPlanSQLExporter::Export(*connection.context, *plan),
	                       LogicalPlanVerificationIssueCode::UNSUPPORTED_EXTENSION);
}

TEST_CASE("SQL verification fallback preserves extension reconstruction failures",
          "[sql_export][logical_plan_sql_export]") {
	DuckDB db;
	Connection connection(db);
	for (bool allow_unsupported : {false, true}) {
		auto plan = LeafExtension("failed_extension", TableIndex(82));
		plan->export_sql = [](SQLExportExtensionOperator &, LogicalPlanSQLExportContext &,
		                      const LogicalPlanVerificationPath &path) {
			auto unsupported = LogicalPlanSQLExportHelpers::PlanUnsupportedFeature(path, "extension_feature",
			                                                                       "Unsupported extension feature");
			auto defect = SQLExportHelpers::MakeIssue(LogicalPlanVerificationIssueCode::INTERNAL_INVARIANT,
			                                          LogicalPlanVerificationPhase::PLAN_EXPORT, path, {},
			                                          "Extension reconstruction defect");
			return PlanExportResult::Failure({std::move(unsupported), std::move(defect)});
		};
		auto exported = LogicalPlanSQLExporter::Export(*connection.context, *plan);
		REQUIRE(exported.HasError());
		REQUIRE(exported.GetIssues().size() == 2);
		REQUIRE(exported.GetIssues()[0].code == LogicalPlanVerificationIssueCode::UNSUPPORTED_EXPORT_FEATURE);
		REQUIRE(exported.GetIssues()[1].code == LogicalPlanVerificationIssueCode::INTERNAL_INVARIANT);
		LogicalExplain explain(std::move(plan), ExplainType::EXPLAIN_SQL, ProfilerPrintFormat::Default());
		explain.allow_unsupported_sql = allow_unsupported;
#ifndef DUCKDB_CRASH_ON_ASSERT
		REQUIRE_THROWS_AS(explain.CreateSQLResult(*connection.context, TableIndex(83)), InternalException);
#endif
	}
}

TEST_CASE("Extension SQL reconstruction owns its result and preserves positional bindings",
          "[sql_export][logical_plan_sql_export]") {
	optional<LogicalPlanSQLExportRelation> owned;
	{
		DuckDB db;
		Connection connection(db);
		vector<ColumnBinding> bindings;
		vector<unique_ptr<Expression>> expressions;
		for (idx_t i : {idx_t(1), idx_t(0)}) {
			bindings.emplace_back(TableIndex(85), ProjectionIndex(i));
			expressions.push_back(
			    make_uniq<BoundColumnRefExpression>(Identifier("same"), LogicalType::INTEGER, bindings.back()));
		}
		auto plan = make_uniq<SQLExportExtensionOperator>("positional_child", bindings,
		                                                  vector<LogicalType>(2, LogicalType::INTEGER),
		                                                  vector<TableIndex> {}, std::move(expressions));
		plan->children.push_back(IntegerValues(TableIndex(85), {{10, 20}, {10, 20}}));
		plan->export_sql = ExportExtensionProjection;
		auto result = LogicalPlanSQLExporter::Export(*connection.context, *plan);
		REQUIRE(result.IsSuccess());
		owned = std::move(result.GetValue());
		plan->children[0]->Cast<LogicalExpressionGet>().expressions[0][0] =
		    make_uniq<BoundDefaultExpression>(LogicalType::INTEGER);
		auto failure = LogicalPlanSQLExporter::Export(*connection.context, *plan);
		RequireLogicalPlanExportIssue(
		    failure, LogicalPlanVerificationIssueCode::UNSUPPORTED_EXPRESSION,
		    LogicalPlanVerificationPhase::EXPRESSION_EXPORT,
		    LogicalPlanVerificationPath {LogicalPlanVerificationPathRoot::LOGICAL_PLAN,
		                                 {{LogicalPlanVerificationPathComponentType::OPERATOR_CHILD, 0},
		                                  {LogicalPlanVerificationPathComponentType::OPERATOR_EXPRESSION, 0}}});
	}
	DuckDB db;
	Connection connection(db);
	auto rows = connection.Query(owned->query->ToString());
	REQUIRE_NO_FAIL(*rows);
	REQUIRE(rows->RowCount() == 2);
	auto row_values = rows->Collection().GetRows();
	for (idx_t row = 0; row < 2; row++) {
		REQUIRE(row_values.GetValue(0, row) == Value::INTEGER(20));
		REQUIRE(row_values.GetValue(1, row) == Value::INTEGER(10));
	}
}

TEST_CASE("Extension SQL reconstruction propagates exceptions", "[sql_export][logical_plan_sql_export]") {
	DuckDB db;
	Connection connection(db);
	auto plan = LeafExtension("throwing_extension", TableIndex(120));
	plan->export_sql = [](SQLExportExtensionOperator &, LogicalPlanSQLExportContext &,
	                      const LogicalPlanVerificationPath &) -> PlanExportResult {
		throw std::runtime_error("synthetic export failure");
	};
	REQUIRE_THROWS_AS(LogicalPlanSQLExporter::Export(*connection.context, *plan), std::runtime_error);
}

TEST_CASE("Logical plan SQL export isolates extension column names and scope modifiers",
          "[sql_export][logical_plan_sql_export]") {
	DuckDB db;
	Connection connection(db);
	for (const auto &sql :
	     {"SELECT 42, 43", "SELECT 42 AS same, 43 AS same", "SELECT 42 AS \"odd.name\", 43 AS \"a b\"",
	      "SELECT v.a, v.b FROM (VALUES (1,2),(1,2),(3,4)) v(a,b)",
	      "SELECT DISTINCT v.a, v.b FROM (VALUES (1,2),(1,2),(3,4)) v(a,b)",
	      "SELECT v.a, v.b FROM (VALUES (1,2),(3,4)) v(a,b) ORDER BY v.a DESC LIMIT 1",
	      "SELECT v.a, v.b FROM (VALUES (1,2),(3,4)) v(a,b) LIMIT 0",
	      "SELECT v.a, v.b FROM (VALUES (1,2),(3,4)) v(a,b) QUALIFY row_number() OVER ()=1",
	      "WITH v(a,b) AS (VALUES (11,12)) SELECT v.a,v.b FROM v"}) {
		INFO(sql);
		auto leaf = make_uniq<SQLExportExtensionOperator>(
		    "positional",
		    vector<ColumnBinding> {{TableIndex(800), ProjectionIndex(0)}, {TableIndex(800), ProjectionIndex(1)}},
		    vector<LogicalType> {LogicalType::INTEGER, LogicalType::INTEGER}, vector<TableIndex> {TableIndex(800)});
		vector<unique_ptr<Expression>> expressions;
		for (auto index : {1, 0, 1}) {
			expressions.push_back(make_uniq<BoundColumnRefExpression>(
			    LogicalType::INTEGER, ColumnBinding(TableIndex(800), ProjectionIndex(index))));
		}
		leaf->export_sql = [&](SQLExportExtensionOperator &op, LogicalPlanSQLExportContext &,
		                       const LogicalPlanVerificationPath &path) {
			Parser parser(*connection.context);
			parser.ParseQuery(sql);
			return op.ExportQuery(std::move(parser.statements[0]->Cast<SelectStatement>().node), path);
		};
		auto plan = PlanProjection(TableIndex(801), std::move(leaf), std::move(expressions));
		auto exported = LogicalPlanSQLExporter::Export(*connection.context, *plan);
		REQUIRE(exported.IsSuccess());
		REQUIRE(exported.GetValue().fields.size() == 3);
		auto direct = connection.Query("SELECT q.b,q.a,q.b FROM (" + string(sql) + ") q(a,b) ORDER BY 1,2,3");
		auto result = connection.Query(exported.GetValue().query->ToString() + " ORDER BY 1,2,3");
		REQUIRE_FALSE(direct->HasError());
		REQUIRE_FALSE(result->HasError());
		REQUIRE(result->GetTypes() == direct->GetTypes());
		REQUIRE(result->Equals(*direct, false));
		if (string(sql).find("WITH") != string::npos) {
			REQUIRE(exported.GetValue().query->Cast<SelectNode>().from_table->type == TableReferenceType::SUBQUERY);
		}
	}
}

} // namespace logical_plan_sql_export_test
