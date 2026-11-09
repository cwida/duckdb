// SQL cannot reach the ExportWithSources replacement API, so statement-root fragment export
// is exercised here. The SQL-visible statement export is covered by test/sql/sql_export.

#include "catch.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/planner/logical_plan_sql_exporter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/planner.hpp"
#include "logical_plan_sql_export_test_helpers.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

namespace logical_plan_sql_export_test {

static unique_ptr<LogicalOperator> OptimizeStatementPlan(Connection &connection, const string &query) {
	Parser parser(*connection.context);
	parser.ParseQuery(query);
	REQUIRE(parser.statements.size() == 1);
	Planner planner(*connection.context);
	planner.CreatePlan(std::move(parser.statements[0]));
	Optimizer optimizer(*planner.binder, *connection.context);
	auto plan = optimizer.Optimize(std::move(planner.plan));
	plan->ResolveOperatorTypes();
	return plan;
}

static optional_ptr<LogicalGet> FindRangeGet(LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_GET &&
	    op.Cast<LogicalGet>().function.GetName().GetIdentifierName() == "range") {
		return &op.Cast<LogicalGet>();
	}
	for (auto &child : op.children) {
		if (auto found = FindRangeGet(*child)) {
			return found;
		}
	}
	return nullptr;
}

static LogicalPlanSQLExportRelation StatementReplacement(Connection &connection, LogicalOperator &op,
                                                         const string &sql) {
	Parser parser(*connection.context);
	parser.ParseQuery(sql);
	REQUIRE(parser.statements.size() == 1);
	auto bindings = op.GetColumnBindings();
	REQUIRE(bindings.size() == op.types.size());
	vector<LogicalPlanSQLExportField> fields;
	for (idx_t i = 0; i < bindings.size(); i++) {
		fields.push_back({bindings[i], op.types[i]});
	}
	return {std::move(parser.statements[0]->Cast<SelectStatement>().node), std::move(fields)};
}

static string ExportStatementFragment(Connection &connection, const string &statement, const string &source_sql,
                                      const LogicalPlanSQLExportOptions &options = {}) {
	auto plan = OptimizeStatementPlan(connection, statement);
	auto range_get = FindRangeGet(*plan);
	REQUIRE(range_get);
	vector<LogicalPlanSQLExportReplacement> replacements;
	replacements.push_back({*range_get, StatementReplacement(connection, *range_get, source_sql)});
	auto exported = LogicalPlanSQLExporter::ExportWithSources(*connection.context, *plan, replacements, options);
	REQUIRE(exported.IsSuccess());
	return exported.GetValue().query->ToString();
}

TEST_CASE("Data-modifying fragment roots accept replaced source subtrees",
          "[sql_export][logical_plan_sql_export][statement_fragments]") {
	DuckDB db(nullptr);
	Connection connection(db);
	REQUIRE_NO_FAIL(connection.Query("CREATE TABLE frag_target(a BIGINT, b VARCHAR)"));
	REQUIRE_NO_FAIL(connection.Query("INSERT INTO frag_target VALUES (1, 'one'), (2, 'two'), (3, 'three')"));
	connection.BeginTransaction();

	SECTION("insert root") {
		auto sql = ExportStatementFragment(connection, "INSERT INTO frag_target SELECT i, 'x' || i FROM range(3) t(i)",
		                                   "SELECT 7::BIGINT AS i");
		REQUIRE(sql.find("INSERT INTO") != string::npos);
		REQUIRE(sql.find("__export_source") != string::npos);
		REQUIRE_NO_FAIL(connection.Query(sql));
		auto result = connection.Query("SELECT b FROM frag_target WHERE a = 7");
		REQUIRE_NO_FAIL(*result);
		REQUIRE(result->RowCount() == 1);
		REQUIRE(result->Collection().GetValue(0, 0) == Value("x7"));
	}
	SECTION("update root") {
		auto sql = ExportStatementFragment(connection,
		                                   "UPDATE frag_target SET b = 'u' WHERE a IN (SELECT i FROM range(10) t(i))",
		                                   "SELECT 1::BIGINT AS i");
		REQUIRE(sql.find("UPDATE") != string::npos);
		REQUIRE(sql.find("__export_source") != string::npos);
		REQUIRE_NO_FAIL(connection.Query(sql));
		auto result = connection.Query("SELECT a FROM frag_target WHERE b = 'u'");
		REQUIRE_NO_FAIL(*result);
		REQUIRE(result->RowCount() == 1);
		REQUIRE(result->Collection().GetValue(0, 0) == Value::BIGINT(1));
	}
	SECTION("delete root") {
		auto sql = ExportStatementFragment(
		    connection, "DELETE FROM frag_target WHERE a IN (SELECT i FROM range(10) t(i))", "SELECT 2::BIGINT AS i");
		REQUIRE(sql.find("DELETE FROM") != string::npos);
		REQUIRE_NO_FAIL(connection.Query(sql));
		auto result = connection.Query("SELECT a FROM frag_target ORDER BY a");
		REQUIRE_NO_FAIL(*result);
		REQUIRE(result->RowCount() == 2);
		REQUIRE(result->Collection().GetValue(0, 0) == Value::BIGINT(1));
		REQUIRE(result->Collection().GetValue(0, 1) == Value::BIGINT(3));
	}
	SECTION("merge root") {
		auto sql = ExportStatementFragment(connection,
		                                   "MERGE INTO frag_target USING (SELECT i AS a, 'm' || i AS b FROM range(5) "
		                                   "t(i)) s ON frag_target.a = s.a "
		                                   "WHEN MATCHED THEN UPDATE SET b = s.b "
		                                   "WHEN NOT MATCHED THEN INSERT VALUES (s.a, s.b)",
		                                   "SELECT 2::BIGINT AS i UNION ALL SELECT 9::BIGINT AS i");
		// the source definitions render as the top-level WITH of the merge statement
		REQUIRE(sql.rfind("WITH", 0) == 0);
		REQUIRE(sql.find("__export_source") != string::npos);
		REQUIRE(sql.find("MERGE INTO") != string::npos);
		REQUIRE_NO_FAIL(connection.Query(sql));
		auto result = connection.Query("SELECT b FROM frag_target WHERE a IN (2, 9) ORDER BY a");
		REQUIRE_NO_FAIL(*result);
		REQUIRE(result->RowCount() == 2);
		REQUIRE(result->Collection().GetValue(0, 0) == Value("m2"));
		REQUIRE(result->Collection().GetValue(0, 1) == Value("m9"));
	}
	SECTION("merge consuming a materialized CTE") {
		auto sql = ExportStatementFragment(connection,
		                                   "WITH src AS MATERIALIZED (SELECT i FROM range(3) t(i)) "
		                                   "MERGE INTO frag_target USING (SELECT i AS a, 'c' AS b FROM src) s "
		                                   "ON frag_target.a = s.a WHEN MATCHED THEN UPDATE SET b = s.b",
		                                   "SELECT 3::BIGINT AS i");
		// the replaced source precedes the consumer definition in the top-level WITH
		REQUIRE(sql.find("__export_source") != string::npos);
		REQUIRE(sql.find("__export_source") < sql.find("MERGE INTO"));
		REQUIRE_NO_FAIL(connection.Query(sql));
		auto result = connection.Query("SELECT b FROM frag_target WHERE a = 3");
		REQUIRE_NO_FAIL(*result);
		REQUIRE(result->Collection().GetValue(0, 0) == Value("c"));
	}
	SECTION("returning aliases positionally") {
		auto sql = ExportStatementFragment(connection,
		                                   "INSERT INTO frag_target SELECT i, 'r' FROM range(2) t(i) RETURNING b, a",
		                                   "SELECT 11::BIGINT AS i UNION ALL SELECT 12::BIGINT AS i");
		REQUIRE(sql.find("RETURNING") != string::npos);
		auto result = connection.Query(sql);
		REQUIRE_NO_FAIL(*result);
		REQUIRE(result->ColumnCount() == 2);
		REQUIRE(result->ColumnName(0) == "c0");
		REQUIRE(result->ColumnName(1) == "c1");
		REQUIRE(result->RowCount() == 2);
		REQUIRE(result->Collection().GetValue(0, 0) == Value("r"));
	}
	SECTION("returning honors explicit output names") {
		LogicalPlanSQLExportOptions options;
		options.output_names = vector<Identifier> {Identifier("written_b"), Identifier("written_a")};
		auto sql = ExportStatementFragment(connection,
		                                   "INSERT INTO frag_target SELECT i, 'n' FROM range(1) t(i) RETURNING b, a",
		                                   "SELECT 21::BIGINT AS i", options);
		auto result = connection.Query(sql);
		REQUIRE_NO_FAIL(*result);
		REQUIRE(result->ColumnName(0) == "written_b");
		REQUIRE(result->ColumnName(1) == "written_a");
	}
	connection.Rollback();
}

TEST_CASE("Write statement fragment roots carry definitions in their source query",
          "[sql_export][logical_plan_sql_export][statement_fragments]") {
	DuckDB db(nullptr);
	Connection connection(db);
	connection.BeginTransaction();

	SECTION("create table as") {
		auto sql = ExportStatementFragment(connection, "CREATE TABLE frag_made AS SELECT i AS x FROM range(3) t(i)",
		                                   "SELECT 7::BIGINT AS i");
		// the statement head cannot carry a WITH clause, the definitions live in the source query
		REQUIRE(sql.rfind("CREATE TABLE", 0) == 0);
		REQUIRE(sql.find("__export_source") != string::npos);
		REQUIRE_NO_FAIL(connection.Query(sql));
		auto result = connection.Query("SELECT x FROM frag_made");
		REQUIRE_NO_FAIL(*result);
		REQUIRE(result->RowCount() == 1);
		REQUIRE(result->Collection().GetValue(0, 0) == Value::BIGINT(7));
	}
	SECTION("create table as with a consumer CTE") {
		auto sql = ExportStatementFragment(
		    connection,
		    "CREATE TABLE frag_made2 AS WITH q AS MATERIALIZED (SELECT i FROM range(3) t(i)) SELECT i + 1 AS x FROM q",
		    "SELECT 7::BIGINT AS i");
		REQUIRE(sql.rfind("CREATE TABLE", 0) == 0);
		// the replaced source precedes the consumer definition it feeds
		auto source_position = sql.find("__export_source");
		REQUIRE(source_position != string::npos);
		REQUIRE_NO_FAIL(connection.Query(sql));
		auto result = connection.Query("SELECT x FROM frag_made2");
		REQUIRE_NO_FAIL(*result);
		REQUIRE(result->RowCount() == 1);
		REQUIRE(result->Collection().GetValue(0, 0) == Value::BIGINT(8));
	}
	SECTION("copy to") {
		auto path = TestCreatePath("fragment_copy_out.csv");
		auto sql = ExportStatementFragment(
		    connection, "COPY (SELECT i FROM range(3) t(i)) TO '" + path + "' (FORMAT csv, HEADER false)",
		    "SELECT 7::BIGINT AS i");
		REQUIRE(sql.rfind("COPY", 0) == 0);
		REQUIRE(sql.find("__export_source") != string::npos);
		REQUIRE_NO_FAIL(connection.Query(sql));
		auto result = connection.Query("SELECT * FROM read_csv('" + path + "', header=false, columns={'i':'BIGINT'})");
		REQUIRE_NO_FAIL(*result);
		REQUIRE(result->RowCount() == 1);
		REQUIRE(result->Collection().GetValue(0, 0) == Value::BIGINT(7));
	}
	connection.Rollback();
}

TEST_CASE("Statement fragment roots keep the replacement invariants",
          "[sql_export][logical_plan_sql_export][statement_fragments]") {
	DuckDB db(nullptr);
	Connection connection(db);
	REQUIRE_NO_FAIL(connection.Query("CREATE TABLE frag_guard(a BIGINT, b VARCHAR)"));
	connection.BeginTransaction();

	SECTION("overlapping statement boundaries") {
		auto plan = OptimizeStatementPlan(connection, "INSERT INTO frag_guard SELECT i, 'x' FROM range(3) t(i)");
		auto range_get = FindRangeGet(*plan);
		REQUIRE(range_get);
		auto root_bindings = plan->GetColumnBindings();
		Parser parser(*connection.context);
		parser.ParseQuery("SELECT 1::BIGINT AS n");
		vector<LogicalPlanSQLExportField> root_fields;
		for (idx_t i = 0; i < root_bindings.size(); i++) {
			root_fields.push_back({root_bindings[i], plan->types[i]});
		}
		vector<LogicalPlanSQLExportReplacement> replacements;
		replacements.push_back(
		    {*plan, {std::move(parser.statements[0]->Cast<SelectStatement>().node), std::move(root_fields)}});
		replacements.push_back({*range_get, StatementReplacement(connection, *range_get, "SELECT 7::BIGINT AS i")});
		RequirePlanExportIssue(LogicalPlanSQLExporter::ExportWithSources(*connection.context, *plan, replacements),
		                       LogicalPlanVerificationIssueCode::INTERNAL_INVARIANT);
	}
	SECTION("a replaced RETURNING root reads the replacement") {
		REQUIRE_NO_FAIL(connection.Query("INSERT INTO frag_guard VALUES (2, 'kept')"));
		auto plan = OptimizeStatementPlan(connection, "INSERT INTO frag_guard SELECT 1::BIGINT, 'orig' RETURNING a");
		vector<LogicalPlanSQLExportReplacement> replacements;
		replacements.push_back({*plan, StatementReplacement(connection, *plan, "SELECT 99::BIGINT AS a")});
		auto exported = LogicalPlanSQLExporter::ExportWithSources(*connection.context, *plan, replacements);
		REQUIRE(exported.IsSuccess());
		auto result = connection.Query(exported.GetValue().query->ToString());
		REQUIRE_NO_FAIL(*result);
		REQUIRE(result->RowCount() == 1);
		REQUIRE(result->Collection().GetValue(0, 0) == Value::BIGINT(99));
		// the original write must not run
		auto count = connection.Query("SELECT count(*) FROM frag_guard");
		REQUIRE(count->Collection().GetValue(0, 0) == Value::BIGINT(1));
	}
	SECTION("a replaced statement below a RETURNING root reads the replacement") {
		REQUIRE_NO_FAIL(connection.Query("INSERT INTO frag_guard VALUES (2, 'kept')"));
		auto plan = OptimizeStatementPlan(connection, "INSERT INTO frag_guard SELECT 1::BIGINT, 'orig' RETURNING a");
		REQUIRE(plan->children.size() == 1);
		auto &boundary = *plan->children[0];
		vector<LogicalPlanSQLExportReplacement> replacements;
		replacements.push_back(
		    {boundary, StatementReplacement(connection, boundary, "SELECT 99::BIGINT AS a, 'repl' AS b")});
		auto exported = LogicalPlanSQLExporter::ExportWithSources(*connection.context, *plan, replacements);
		REQUIRE(exported.IsSuccess());
		auto result = connection.Query(exported.GetValue().query->ToString());
		REQUIRE_NO_FAIL(*result);
		REQUIRE(result->RowCount() == 1);
		REQUIRE(result->Collection().GetValue(0, 0) == Value::BIGINT(99));
		auto count = connection.Query("SELECT count(*) FROM frag_guard");
		REQUIRE(count->Collection().GetValue(0, 0) == Value::BIGINT(1));
	}
	SECTION("select roots behave as before") {
		auto plan = OptimizeStatementPlan(connection, "SELECT i + 1 FROM range(3) t(i)");
		auto range_get = FindRangeGet(*plan);
		REQUIRE(range_get);
		vector<LogicalPlanSQLExportReplacement> replacements;
		replacements.push_back({*range_get, StatementReplacement(connection, *range_get, "SELECT 41::BIGINT AS i")});
		auto exported = LogicalPlanSQLExporter::ExportWithSources(*connection.context, *plan, replacements);
		REQUIRE(exported.IsSuccess());
		auto result = connection.Query(exported.GetValue().query->ToString());
		REQUIRE_NO_FAIL(*result);
		REQUIRE(result->RowCount() == 1);
		REQUIRE(result->Collection().GetValue(0, 0) == Value::BIGINT(42));
	}
	connection.Rollback();
}

} // namespace logical_plan_sql_export_test
