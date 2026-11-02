#include "duckdb/planner/operator/logical_empty_result.hpp"
#include "sql_export_test_helpers.hpp"
#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/window_function.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/statement/logical_plan_statement.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/bound_expression_sql_exporter.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/logical_plan_sql_exporter.hpp"
#include "duckdb/planner/sql_export_helpers.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_sample.hpp"
#include "duckdb/planner/planner.hpp"
#include <stdexcept>
#include <type_traits>
#include "logical_plan_sql_export_test_helpers.hpp"

using namespace duckdb;

namespace logical_plan_sql_export_test {

static const Value &GetPlanIssueFact(const LogicalPlanVerificationIssue &issue, const string &name) {
	for (auto &fact : issue.facts) {
		if (fact.first == name) {
			return fact.second;
		}
	}
	throw InternalException("Missing logical plan SQL export issue fact");
}

static void SQLExportCompressionNameProbe(DataChunk &input, ExpressionState &, Vector &result) {
	result.Reference(Value::BIGINT(99), count_t(input.size()));
}

TEST_CASE("Logical plan SQL export identifies rejected output types", "[sql_export][logical_plan_sql_export]") {
	DuckDB db(nullptr);
	Connection connection(db);

	SECTION("unrepresentable output") {
		LogicalType type = LogicalType::POINTER;
		auto plan = make_uniq<LogicalEmptyResult>(
		    vector<LogicalType> {type}, vector<ColumnBinding> {ColumnBinding(TableIndex(1), ProjectionIndex(0))});
		plan->ResolveOperatorTypes();
		auto result = LogicalPlanSQLExporter::Export(*connection.context, *plan);
		RequirePlanExportIssue(result, LogicalPlanVerificationIssueCode::UNSUPPORTED_EXPORT_FEATURE);
		auto &issue = result.GetIssues()[0];
		REQUIRE(issue.construct == LogicalPlanVerificationConstructIdentity::ExportFeature("output_type"));
		REQUIRE(GetPlanIssueFact(issue, "column_index") == Value::UBIGINT(0));
		REQUIRE(GetPlanIssueFact(issue, "logical_type") == Value(type.ToString()));
		REQUIRE(GetPlanIssueFact(issue, "varchar_collations") == Value(SQLExportHelpers::TypeCollationSignature(type)));
	}
}

TEST_CASE("Logical plan SQL export preserves qualified identities through binary copies",
          "[sql_export][logical_plan_sql_export][serialization]") {
	unique_ptr<QueryNode> owned_query;
	{
		DuckDB db;
		Connection connection(db);
		REQUIRE_NO_FAIL(connection.Query("SET debug_verify_serializer=false"));
		connection.BeginTransaction();
		auto plan = OptimizeLogicalPlanExportQuery(connection, "SELECT abs(i) FROM (VALUES (-7), (2)) t(i)");
		REQUIRE(plan->type == LogicalOperatorType::LOGICAL_PROJECTION);
		REQUIRE(plan->expressions[0]->GetExpressionClass() == ExpressionClass::BOUND_FUNCTION);
		auto expression_copy = plan->expressions[0]->Copy();
		plan->expressions[0] = std::move(expression_copy);
		REQUIRE_NO_FAIL(connection.Query("SET debug_verify_serializer=true"));
		auto live = LogicalPlanSQLExporter::Export(*connection.context, *plan);
		REQUIRE(live.IsSuccess());
		owned_query = live.GetValue().query->Copy();
		auto restored = plan->Copy(*connection.context);
		auto copied = LogicalPlanSQLExporter::Export(*connection.context, *restored);
		REQUIRE(copied.IsSuccess());
		REQUIRE_NO_FAIL(connection.Query(copied.GetValue().query->ToString()));
		Planner::VerifyPlan(*connection.context, plan);
		REQUIRE(LogicalPlanSQLExporter::Export(*connection.context, *plan).IsSuccess());
		connection.Rollback();
	}
	DuckDB receiving_db;
	Connection receiving_connection(receiving_db);
	auto result = receiving_connection.Query(owned_query->ToString());
	REQUIRE_FALSE(result->HasError());
	REQUIRE(result->GetTypes() == vector<LogicalType> {LogicalType::INTEGER});
	REQUIRE(result->Collection().GetValue(0, 0) == Value::INTEGER(7));
	REQUIRE(result->Collection().GetValue(0, 1) == Value::INTEGER(2));
}

TEST_CASE("Logical plan field types follow expression SQL type admission", "[sql_export][logical_plan_sql_export]") {
	DuckDB db;
	Connection connection(db);
	auto types = LogicalType::AllTypes();
	types.push_back(LogicalType::SQLNULL);
	types.push_back(LogicalType::POINTER);
	types.push_back(LogicalType::ANY);
	const ColumnBinding binding(TableIndex(74), ProjectionIndex(0));
	for (auto &type : types) {
		for (auto &candidate : vector<LogicalType> {type, LogicalType::LIST(type)}) {
			INFO(static_cast<uint32_t>(type.id()));
			BoundColumnRefExpression expression(candidate, binding);
			BoundExpressionSQLExportContext context;
			context.resolve_binding = [&](const ColumnBinding &) -> optional<ResolvedSQLColumnReference> {
				return ResolvedSQLColumnReference {{Identifier("c0")}, candidate};
			};
			auto expression_result = BoundExpressionSQLExporter::Export(expression, context);
			SQLExportExtensionOperator op("sql_type_admission", {binding}, {candidate}, {TableIndex(74)});
			op.export_sql = [](SQLExportExtensionOperator &input, LogicalPlanSQLExportContext &,
			                   const LogicalPlanVerificationPath &path) {
				auto query = make_uniq<SelectNode>();
				for (idx_t i = 0; i < input.types.size(); i++) {
					query->select_list.push_back(ConstantExpression::Null());
				}
				return input.ExportQuery(std::move(query), path);
			};
			auto result = LogicalPlanSQLExporter::Export(*connection.context, op);
			REQUIRE(result.IsValid());
			REQUIRE(result.IsSuccess() == expression_result.IsSuccess());
		}
	}
}

TEST_CASE("Logical plan SQL export applies requested output names", "[sql_export][logical_plan_sql_export]") {
	DuckDB db(nullptr);
	Connection connection(db);
	auto plan = OptimizeLogicalPlanExportQuery(connection, "SELECT 1 AS a, 2 AS b");
	LogicalPlanSQLExportOptions options;
	options.output_names = vector<Identifier> {"same", "same"};
	auto exported = LogicalPlanSQLExporter::Export(*connection.context, *plan, options);
	REQUIRE(exported.IsSuccess());
	auto result = connection.Query(exported.GetValue().query->ToString());
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->GetNames() == vector<Identifier> {"same", "same"});
	REQUIRE(result->GetTypes() == vector<LogicalType> {LogicalType::INTEGER, LogicalType::INTEGER});
	REQUIRE(CHECK_COLUMN(result, 0, {1}));
	REQUIRE(CHECK_COLUMN(result, 1, {2}));

	REQUIRE_NO_FAIL(connection.Query("CREATE TABLE explain_names(i INTEGER)"));
	auto prepared =
	    connection.Prepare("EXPLAIN (SQL) SELECT i AS \"same name\", i::BIGINT AS \"same name\" FROM explain_names");
	REQUIRE(!prepared->HasError());
	vector<Value> parameters;
	auto execute_explain = [&]() {
		return prepared->Execute(parameters);
	};
	auto first = execute_explain();
	REQUIRE_NO_FAIL(*first);
	REQUIRE(first->GetNames() == vector<Identifier> {"explain_key", "explain_value"});
	REQUIRE(first->GetTypes() == vector<LogicalType> {LogicalType::VARCHAR, LogicalType::VARCHAR});
	REQUIRE(first->RowCount() == 1);
	REQUIRE(first->Collection().GetValue(0, 0) == Value("sql"));
	auto sql = first->Collection().GetValue(1, 0).GetValue<string>();
	REQUIRE(StringUtil::Contains(sql, "\nFROM"));
	auto explained = connection.Query(sql);
	REQUIRE_NO_FAIL(*explained);
	REQUIRE(explained->GetNames() == vector<Identifier> {"same name", "same name"});
	REQUIRE(explained->GetTypes() == vector<LogicalType> {LogicalType::INTEGER, LogicalType::BIGINT});
	auto repeated = execute_explain();
	REQUIRE_NO_FAIL(*repeated);
	REQUIRE(repeated->Collection().GetValue(1, 0) == Value(sql));
	REQUIRE_NO_FAIL(connection.Query("DROP TABLE explain_names"));
	REQUIRE(prepared->Execute()->HasError());
	REQUIRE_NO_FAIL(connection.Query("CREATE TABLE explain_names(i VARCHAR)"));
	auto rebound = execute_explain();
	REQUIRE_NO_FAIL(*rebound);
	auto rebound_query = connection.Query(rebound->Collection().GetValue(1, 0).GetValue<string>());
	REQUIRE_NO_FAIL(*rebound_query);
	REQUIRE(rebound_query->GetTypes() == vector<LogicalType> {LogicalType::VARCHAR, LogicalType::BIGINT});
	Parser parser(*connection.context);
	parser.ParseQuery("EXPLAIN (SQL) SELECT 42");
	auto copied = parser.statements[0]->Copy();
	REQUIRE(copied->ToString() == "EXPLAIN (SQL) SELECT 42");
}

TEST_CASE("Logical plan SQL export preserves empty filter projection maps", "[sql_export][logical_plan_sql_export]") {
	DuckDB db(nullptr);
	Connection connection(db);
	REQUIRE_NO_FAIL(connection.Query("CREATE TABLE filter_input(x INTEGER,y VARCHAR,z INTEGER);"
	                                 "INSERT INTO filter_input VALUES(1,'a',10),(2,'b',20),(3,NULL,30)"));
	for (auto projection :
	     vector<vector<ProjectionIndex>> {{},
	                                      {ProjectionIndex(2), ProjectionIndex(0)},
	                                      {ProjectionIndex(1), ProjectionIndex(1), ProjectionIndex(0)}}) {
		connection.BeginTransaction();
		auto filter = make_uniq<LogicalFilter>();
		filter->children.push_back(OptimizeLogicalPlanExportQuery(connection, "SELECT * FROM filter_input WHERE x>1"));
		filter->projection_map = projection;
		filter->ResolveOperatorTypes();
		auto exported = LogicalPlanSQLExporter::Export(*connection.context, *filter);
		REQUIRE(exported.IsSuccess());
		auto copied = filter->Copy(*connection.context);
		auto copied_export = LogicalPlanSQLExporter::Export(*connection.context, *copied);
		REQUIRE(copied_export.IsSuccess());
		auto text = exported.GetValue().query->ToString();
		REQUIRE(text == copied_export.GetValue().query->ToString());
		REQUIRE_NO_FAIL(connection.Query("SET debug_disable_optimizer=true"));
		auto native = connection.Query(make_uniq<LogicalPlanStatement>(std::move(filter)));
		REQUIRE_NO_FAIL(*native);
		REQUIRE(native->RowCount() == 2);
		REQUIRE_NO_FAIL(connection.Query("SET debug_disable_optimizer=false"));
		auto generated = connection.Query(text);
		REQUIRE_NO_FAIL(*generated);
		REQUIRE(generated->GetTypes() == native->GetTypes());
		REQUIRE(SQLExportRows(*generated, true) == SQLExportRows(*native, true));
		auto statement = make_uniq<SelectStatement>();
		statement->node = std::move(exported.GetValue().query);
		auto ast = connection.Query(std::move(statement));
		REQUIRE_NO_FAIL(*ast);
		REQUIRE(ast->GetTypes() == native->GetTypes());
		REQUIRE(SQLExportRows(*ast, true) == SQLExportRows(*native, true));
		connection.Rollback();
	}
}

TEST_CASE("Window SQL range origins survive copies and optional serialization fields",
          "[sql_export][logical_plan_sql_export][window_sql_export][serialization]") {
	DuckDB db(nullptr);
	Connection connection(db);
	connection.BeginTransaction();
	const vector<string> queries {
	    "SELECT count(*) OVER (ORDER BY x RANGE BETWEEN 0 PRECEDING AND CURRENT ROW) FROM (VALUES (1),(2)) t(x)",
	    "SELECT count(*) OVER (ORDER BY x RANGE BETWEEN INTERVAL 1 DAY PRECEDING AND CURRENT ROW) "
	    "FROM (VALUES (DATE '2024-02-28'),(DATE '2024-02-29')) t(x)",
	    "SELECT count(*) OVER (ORDER BY CAST(x AS TIMESTAMP) RANGE BETWEEN INTERVAL 1 DAY PRECEDING AND CURRENT ROW) "
	    "FROM (VALUES (DATE '2024-02-28'),(DATE '2024-02-29')) t(x)",
	    "SELECT count(*) OVER (ORDER BY x RANGE BETWEEN 1 PRECEDING AND CURRENT ROW) FROM (VALUES (1),(2)) t(x)"};
	const vector<LogicalType> order_types {LogicalType::INTEGER, LogicalType::DATE, LogicalType::TIMESTAMP,
	                                       LogicalType::INTEGER};
	for (idx_t i = 0; i < queries.size(); i++) {
		for (idx_t route = 0; route < 3; route++) {
			CAPTURE(i, route);
			auto plan = OptimizeLogicalPlanExportQuery(connection, queries[i]);
			auto op = FindLogicalPlanExportOperator(*plan, LogicalOperatorType::LOGICAL_WINDOW);
			REQUIRE(op);
			if (route == 1) {
				op->expressions[0] = op->expressions[0]->Copy();
			} else if (route == 2) {
				plan = plan->Copy(*connection.context);
				op = FindLogicalPlanExportOperator(*plan, LogicalOperatorType::LOGICAL_WINDOW);
			}
			auto &window = op->expressions[0]->Cast<BoundWindowExpression>();
			REQUIRE(window.SQLRangeOrderType() == order_types[i]);
			auto exported = LogicalPlanSQLExporter::Export(*connection.context, *plan);
			REQUIRE(exported.IsSuccess());
			window.RetainSQLRange(nullptr, nullptr, LogicalType::INVALID);
			window.SQLRangeStartBoundaryMutable().reset();
			window.SQLRangeEndBoundaryMutable().reset();
			window.SQLRangeOrderCastsMutable().clear();
			auto legacy = plan->Copy(*connection.context);
			auto without_origin = LogicalPlanSQLExporter::Export(*connection.context, *legacy);
			REQUIRE(without_origin.HasError());
			REQUIRE(without_origin.GetIssues()[0].construct->identifier == "window_range_offset");
		}
	}
	connection.Rollback();
}

TEST_CASE("Window SQL export rejects unrelated RANGE endpoints",
          "[sql_export][logical_plan_sql_export][window_sql_export]") {
	DuckDB db(nullptr);
	Connection connection(db);
	connection.BeginTransaction();
	auto plan = OptimizeLogicalPlanExportQuery(
	    connection, "SELECT sum(x) OVER (ORDER BY x RANGE BETWEEN 1 PRECEDING AND CURRENT ROW) "
	                "FROM (VALUES (1),(2),(4)) t(x)");
	auto op = FindLogicalPlanExportOperator(*plan, LogicalOperatorType::LOGICAL_WINDOW);
	REQUIRE(op);
	auto &window = op->expressions[0]->Cast<BoundWindowExpression>();
	auto endpoint = window.StartExpr()->Copy();
	for (bool missing_offset : {false, true}) {
		window.StartExprMutable() = endpoint->Copy();
		auto &arithmetic = window.StartExprMutable()->Cast<BoundFunctionExpression>();
		if (missing_offset) {
			window.StartExprMutable() = window.OrderBy()[0].expression->Copy();
		} else {
			arithmetic.GetChildrenMutable()[0] = make_uniq<BoundConstantExpression>(Value::INTEGER(100));
		}
		auto exported = LogicalPlanSQLExporter::Export(*connection.context, *plan);
		REQUIRE(exported.HasError());
		REQUIRE(exported.GetIssues()[0].code == LogicalPlanVerificationIssueCode::UNSUPPORTED_EXPORT_FEATURE);
		REQUIRE(exported.GetIssues()[0].construct->identifier == "window_range_offset");
	}
	for (idx_t recovery = 0; recovery < 3; recovery++) {
		window.StartExprMutable() = endpoint->Copy();
		auto &function = window.StartExprMutable()->Cast<BoundFunctionExpression>().FunctionMutable();
		auto definition = make_shared_ptr<ScalarFunction>(*function.GetDefinition());
		if (recovery == 0) {
			definition->SetUnbindCallback(
			    [](FunctionUnbindInput &) -> unique_ptr<ParsedExpression> { return nullptr; });
		} else if (recovery == 1) {
			definition->GetProperties().SetCaptureArgumentAliases(true);
		} else {
			definition->GetProperties().SetRequiresExpressionNames(true);
		}
		function.SetDefinition(std::move(definition));
		REQUIRE(LogicalPlanSQLExporter::Export(*connection.context, *plan).HasError());
	}
	window.StartExprMutable() = endpoint->Copy();
	window.OrderByMutable()[0].type = OrderType::DESCENDING;
	REQUIRE(LogicalPlanSQLExporter::Export(*connection.context, *plan).HasError());
	window.OrderByMutable()[0].type = OrderType::ASCENDING;
	window.WindowStartMutable() = WindowBoundary::EXPR_FOLLOWING_RANGE;
	REQUIRE(LogicalPlanSQLExporter::Export(*connection.context, *plan).HasError());
	window.WindowStartMutable() = WindowBoundary::EXPR_PRECEDING_RANGE;
	window.StartExprMutable()->Cast<BoundFunctionExpression>().GetChildrenMutable()[1] =
	    make_uniq<BoundConstantExpression>(Value::INTEGER(2));
	auto changed = LogicalPlanSQLExporter::Export(*connection.context, *plan);
	REQUIRE(changed.IsSuccess());
	auto generated = connection.Query(changed.GetValue().query->ToString());
	auto original = connection.Query("SELECT sum(x) OVER (ORDER BY x RANGE BETWEEN 2 PRECEDING AND CURRENT ROW) "
	                                 "FROM (VALUES (1),(2),(4)) t(x)");
	REQUIRE_NO_FAIL(*generated);
	REQUIRE_NO_FAIL(*original);
	REQUIRE(SQLExportRows(*generated, false) == SQLExportRows(*original, false));
	connection.Rollback();
}

TEST_CASE("Window SQL export retains logical signatures and context boundaries",
          "[sql_export][logical_plan_sql_export][window_sql_export]") {
	DuckDB db(nullptr);
	Connection connection(db);
	connection.BeginTransaction();
	auto plan = OptimizeLogicalPlanExportQuery(connection, "SELECT lead(x) OVER (ORDER BY x) FROM (VALUES(1),(2))t(x)");
	auto op = FindLogicalPlanExportOperator(*plan, LogicalOperatorType::LOGICAL_WINDOW);
	REQUIRE(op);
	auto &window = op->expressions[0]->Cast<BoundWindowExpression>();
	auto &function = *window.WindowFunctionMutable();
	const vector<LogicalType> arguments {LogicalType::INTEGER, LogicalType::BIGINT, LogicalType::INTEGER};
	REQUIRE(function.GetLogicalArguments() == arguments);
	REQUIRE(function.GetLogicalReturnType() == LogicalType::INTEGER);
	auto copy = plan->Copy(*connection.context);
	auto copied_op = FindLogicalPlanExportOperator(*copy, LogicalOperatorType::LOGICAL_WINDOW);
	REQUIRE(copied_op);
	auto &copied = *copied_op->expressions[0]->Cast<BoundWindowExpression>().WindowFunction();
	REQUIRE(copied.GetLogicalArguments() == arguments);
	REQUIRE(copied.GetLogicalReturnType() == LogicalType::INTEGER);
	auto binding = window.GetChildren()[0]->Cast<BoundColumnRefExpression>().Binding();
	BoundExpressionSQLExportContext context;
	context.resolve_binding = [binding](const ColumnBinding &candidate) -> optional<ResolvedSQLColumnReference> {
		if (candidate == binding) {
			return ResolvedSQLColumnReference {{Identifier("x")}, LogicalType::INTEGER};
		}
		return {};
	};
	auto standalone = BoundExpressionSQLExporter::Export(window, context);
	REQUIRE(standalone.HasError());
	REQUIRE(standalone.GetIssues()[0].code == LogicalPlanVerificationIssueCode::UNSUPPORTED_EXPRESSION);
	LogicalPlanVerificationPath path;
	path.components.push_back({LogicalPlanVerificationPathComponentType::OPERATOR_EXPRESSION, 0});
	auto exported = BoundExpressionSQLExporter::ExportWindowAtPath(window, context, path);
	REQUIRE(exported.IsSuccess());
	auto text = exported.GetValue()->ToString();
	auto result = connection.Query("SELECT " + text + " FROM (VALUES(1),(2))t(x)");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(CHECK_COLUMN(result, 0, {2, Value()}));
	connection.Rollback();
}

TEST_CASE("Copied UNION SQL outlives its plan and original exported AST",
          "[sql_export][logical_plan_sql_export][set_operation_sql_export]") {
	DuckDB db(nullptr);
	Connection connection(db);
	REQUIRE_NO_FAIL(connection.Query("SET threads=1; CREATE SEQUENCE seq"));
	connection.BeginTransaction();
	auto plan = OptimizeLogicalPlanExportQuery(connection,
	                                           "SELECT x,nextval('seq') y FROM (SELECT nextval('seq') x FROM range(2) "
	                                           "UNION ALL SELECT 0 WHERE false LIMIT 1)t");
	auto exported = LogicalPlanSQLExporter::Export(*connection.context, *plan);
	REQUIRE(exported.IsSuccess());
	auto statement = make_uniq<SelectStatement>();
	statement->node = exported.GetValue().query->Copy();
	plan.reset();
	exported.GetValue().query.reset();
	auto result = connection.Query(std::move(statement));
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->RowCount() == 1);
	REQUIRE(result->Collection().GetValue(0, 0) == Value::BIGINT(1));
	REQUIRE(result->Collection().GetValue(1, 0) == Value::BIGINT(3));
	connection.Rollback();
}

TEST_CASE("Logical plan SQL export rejects nonrepeatable seeded sampling",
          "[sql_export][logical_plan_sql_export][sample_sql_export]") {
	DuckDB db(nullptr);
	Connection connection(db);
	connection.BeginTransaction();
	auto plan = OptimizeLogicalPlanExportQuery(connection, "SELECT * FROM range(10000) USING SAMPLE 31 (reservoir,42)");
	auto sample_op = FindLogicalPlanExportOperator(*plan, LogicalOperatorType::LOGICAL_SAMPLE);
	REQUIRE(sample_op);
	auto &sample = sample_op->Cast<LogicalSample>();
	sample.sample_options->repeatable = false;
	for (bool binary : {false, true}) {
		CAPTURE(binary);
		auto copy = binary ? sample.Copy(*connection.context) : nullptr;
		auto exported = LogicalPlanSQLExporter::Export(*connection.context, copy ? *copy : sample);
		REQUIRE(exported.HasError());
		REQUIRE(exported.IsValid());
		REQUIRE(exported.GetIssues()[0].phase == LogicalPlanVerificationPhase::PLAN_EXPORT);
		REQUIRE(exported.GetIssues()[0].code == LogicalPlanVerificationIssueCode::UNSUPPORTED_EXPORT_FEATURE);
		REQUIRE(exported.GetIssues()[0].construct ==
		        LogicalPlanVerificationConstructIdentity::ExportFeature("sample_repeatability"));
	}
	connection.Rollback();
}

TEST_CASE("Logical plan SQL export discards extracted scan order hints",
          "[sql_export][logical_plan_sql_export][sample_sql_export][table_source_sql]") {
	DuckDB db(nullptr);
	Connection connection(db);
	REQUIRE_NO_FAIL(connection.Query("CREATE TABLE sample_source AS SELECT i FROM range(10000) t(i)"));
	connection.BeginTransaction();

	auto ordered_plan =
	    OptimizeLogicalPlanExportQuery(connection, "SELECT i FROM sample_source ORDER BY i DESC LIMIT 5");
	auto &ordered_get =
	    FindLogicalPlanExportOperator(*ordered_plan, LogicalOperatorType::LOGICAL_GET)->Cast<LogicalGet>();
	REQUIRE(ordered_get.row_group_order_options);
	for (bool binary : {false, true}) {
		auto copy = binary ? ordered_get.Copy(*connection.context) : nullptr;
		auto ordered_export = LogicalPlanSQLExporter::Export(*connection.context, copy ? *copy : ordered_get);
		INFO((ordered_export.HasError() ? ordered_export.GetIssues()[0].message : string()));
		REQUIRE(ordered_export.IsSuccess());
		auto expected = connection.Query("SELECT i FROM sample_source");
		auto actual = connection.Query(ordered_export.GetValue().query->ToString());
		REQUIRE_NO_FAIL(*expected);
		REQUIRE_NO_FAIL(*actual);
		REQUIRE(SQLExportRows(*actual, true) == SQLExportRows(*expected, true));
	}
	connection.Rollback();
}

TEST_CASE("Logical plan SQL export retains sampling errors and partial consumption",
          "[sql_export][logical_plan_sql_export][sample_sql_export]") {
	for (const auto &clause : {"31 (reservoir,42)", "50% (bernoulli,42)", "50% (system,42)"}) {
		for (idx_t consumption = 0; consumption < 3; consumption++) {
			for (bool late_error : {false, true}) {
				auto sql = string("SELECT * FROM (SELECT i,nextval('seq') v,") +
				           (late_error ? "CASE WHEN i<8192 THEN i ELSE error('sample input reached') END" : "i") +
				           " e FROM range(10000)t(i)) USING SAMPLE " + clause + (consumption == 1 ? " LIMIT 1" : "");
				vector<string> expected_rows;
				Value expected_sequence;
				bool expected_error = false;
				for (auto route : {0, 3, 4}) {
					if (route == 4 && (consumption != 0 || late_error)) {
						continue;
					}
					CAPTURE(clause, consumption, late_error, route);
					DuckDB db(nullptr);
					Connection connection(db);
					REQUIRE_NO_FAIL(connection.Query("SET threads=1; "
					                                 "SET max_streaming_buffer_size='1b'; CREATE SEQUENCE seq"));
					connection.BeginTransaction();
					auto plan = OptimizeLogicalPlanExportQuery(connection, sql);
					auto exported = LogicalPlanSQLExporter::Export(*connection.context, *plan);
					REQUIRE(exported.IsSuccess());
					QueryParameters parameters;
					unique_ptr<QueryResult> result;
					if (route == 0) {
						result = SubmitSQLExportResult(*connection.context, sql, parameters);
					} else if (route == 3) {
						result = SubmitSQLExportResult(*connection.context, exported.GetValue().query->ToString(),
						                               parameters);
					} else {
						auto statement = make_uniq<SelectStatement>();
						statement->node = std::move(exported.GetValue().query);
						result = SubmitSQLExportResult(*connection.context, std::move(statement), parameters);
					}
					unique_ptr<QueryResultStream<>> stream;
					if (!result->HasError()) {
						stream = make_uniq<QueryResultStream<>>(std::move(result));
					}
					vector<string> rows;
					while (stream && !stream->HasError()) {
						auto chunk = stream->Fetch();
						if (!chunk) {
							break;
						}
						for (idx_t row = 0; row < chunk->size(); row++) {
							rows.push_back(chunk->GetValue(0, row).ToString() + ":" +
							               chunk->GetValue(1, row).ToString() + ":" +
							               chunk->GetValue(2, row).ToString());
						}
						if (consumption == 2) {
							break;
						}
					}
					auto has_error = (stream ? stream->HasError() : result->HasError());
					if (has_error) {
						REQUIRE(late_error);
						REQUIRE(StringUtil::Contains((stream ? stream->GetError() : result->GetError()),
						                             "sample input reached"));
					}
					stream.reset();
					result.reset();
					connection.Rollback();
					auto sequence = connection.Query("SELECT currval('seq')");
					REQUIRE_NO_FAIL(*sequence);
					if (route == 0) {
						expected_rows = std::move(rows);
						expected_sequence = sequence->Collection().GetValue(0, 0);
						expected_error = has_error;
					} else {
						REQUIRE(rows == expected_rows);
						REQUIRE(has_error == expected_error);
						REQUIRE(sequence->Collection().GetValue(0, 0) == expected_sequence);
					}
				}
			}
		}
	}
}

TEST_CASE("SQL export preserves ordinary functions with compression-like names",
          "[sql_export][logical_plan_sql_export][compressed_materialization]") {
	DuckDB db(nullptr);
	Connection connection(db);
	ExtensionLoader loader(*db.instance, "sql_export_compression_name_probe");
	loader.UseDefaultSchema();
	for (auto name : {"__internal_compress_integral_probe", "__internal_decompress_integral_probe"}) {
		loader.RegisterFunction(
		    ScalarFunction(name, {LogicalType::BIGINT}, LogicalType::BIGINT, SQLExportCompressionNameProbe));
	}
	connection.BeginTransaction();
	for (auto name : {"__internal_compress_integral_probe", "__internal_decompress_integral_probe"}) {
		for (bool binary_copy : {false, true}) {
			CAPTURE(name, binary_copy);
			auto sql = string("SELECT ") + name + "(i) FROM range(2)t(i)";
			auto plan = OptimizeLogicalPlanExportQuery(connection, sql);
			if (binary_copy) {
				plan = plan->Copy(*connection.context);
			}
			auto exported = LogicalPlanSQLExporter::Export(*connection.context, *plan);
			INFO((exported.HasError() ? exported.GetIssues()[0].message : string()));
			REQUIRE(exported.IsSuccess());
			auto generated_sql = exported.GetValue().query->ToString();
			REQUIRE(StringUtil::Contains(generated_sql, name));
			auto direct = connection.Query(sql);
			auto generated = connection.Query(generated_sql);
			REQUIRE_NO_FAIL(*direct);
			REQUIRE_NO_FAIL(*generated);
			REQUIRE(CHECK_COLUMN(direct, 0, {99, 99}));
			REQUIRE(generated->GetTypes() == direct->GetTypes());
			REQUIRE(SQLExportRows(*generated, true) == SQLExportRows(*direct, true));
		}
	}
	connection.Rollback();
}

} // namespace logical_plan_sql_export_test
