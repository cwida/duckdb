#include "duckdb/planner/logical_plan_verifier.hpp"
#include "duckdb/parser/common_table_expression_info.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "sql_export_test_helpers.hpp"
#include "duckdb/catalog/catalog_entry/table_function_catalog_entry.hpp"
#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/optimizer/remove_unused_columns.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/statement/logical_plan_statement.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/planner/logical_plan_sql_exporter.hpp"
#include "duckdb/planner/sql_export_helpers.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_secure_view.hpp"
#include "duckdb/planner/planner.hpp"
#include <stdexcept>
#include <type_traits>
#include "logical_plan_sql_export_test_helpers.hpp"

using namespace duckdb;

namespace logical_plan_sql_export_test {

static unique_ptr<LogicalOperator> OptimizeLogicalPlanExportQueryWithRepeatedPruning(Connection &connection,
                                                                                     const string &query) {
	Parser parser(*connection.context);
	parser.ParseQuery(query);
	REQUIRE(parser.statements.size() == 1);
	Planner planner(*connection.context);
	planner.CreatePlan(std::move(parser.statements[0]));
	Optimizer optimizer(*planner.binder, *connection.context);
	auto plan = optimizer.Optimize(std::move(planner.plan));
	RemoveUnusedColumns remove_unused_columns(optimizer);
	remove_unused_columns.VisitOperator(plan);
	plan->ResolveOperatorTypes();
	return plan;
}

TEST_CASE("Logical plan SQL export rejects opaque table sources",
          "[sql_export][logical_plan_sql_export][table_source_sql]") {
	DuckDB db(nullptr);
	Connection connection(db);
	connection.BeginTransaction();

	SECTION("opaque source") {
		auto plan = OptimizeLogicalPlanExportQuery(connection, "SELECT * FROM range(1)");
		auto get = FindLogicalPlanExportOperator(*plan, LogicalOperatorType::LOGICAL_GET);
		REQUIRE(get);
		get->Cast<LogicalGet>().function.to_sql = [](ClientContext &, const LogicalGet &) -> TableFunctionToSQLResult {
			return {nullptr, "test_opaque"};
		};
		auto result = LogicalPlanSQLExporter::Export(*connection.context, *get);
		RequirePlanExportIssue(result, LogicalPlanVerificationIssueCode::UNSUPPORTED_SOURCE);
		auto &issue = result.GetIssues()[0];
		REQUIRE(issue.construct->function->name == "range");
		REQUIRE(issue.facts.size() == 1);
		REQUIRE((issue.facts[0] == pair<string, Value> {"guard", Value("test_opaque")}));
	}
	SECTION("process-local binding input") {
		auto plan = OptimizeLogicalPlanExportQuery(connection, "SELECT * FROM range(1)");
		auto get = FindLogicalPlanExportOperator(*plan, LogicalOperatorType::LOGICAL_GET);
		REQUIRE(get);
		get->Cast<LogicalGet>().bind_info = make_shared_ptr<TableFunctionInfo>();
		auto result = LogicalPlanSQLExporter::Export(*connection.context, *get);
		RequirePlanExportIssue(result, LogicalPlanVerificationIssueCode::UNSUPPORTED_SOURCE);
		REQUIRE((result.GetIssues()[0].facts[0] == pair<string, Value> {"guard", Value("process_local_input")}));
	}
	SECTION("retained invocation and absent callback") {
		auto plan = OptimizeLogicalPlanExportQuery(connection, "SELECT sum(x) FROM range(10) t(x)");
		auto get = FindLogicalPlanExportOperator(*plan, LogicalOperatorType::LOGICAL_GET);
		REQUIRE(get);
		auto parameters = get->Cast<LogicalGet>().parameters;
		auto copy = plan->Copy(*connection.context);
		auto copied_get = FindLogicalPlanExportOperator(*copy, LogicalOperatorType::LOGICAL_GET);
		REQUIRE(copied_get);
		REQUIRE(copied_get->Cast<LogicalGet>().parameters == parameters);
		REQUIRE(LogicalPlanSQLExporter::Export(*connection.context, *copy).IsSuccess());
		copied_get->Cast<LogicalGet>().function.to_sql = nullptr;
		REQUIRE(LogicalPlanSQLExporter::Export(*connection.context, *copy).IsSuccess());
	}
	connection.Rollback();
}

TEST_CASE("Source SQL callbacks preserve centrally applied scan modifiers",
          "[sql_export][logical_plan_sql_export][table_source_sql]") {
	DuckDB db(nullptr);
	Connection connection(db);
	ExtensionLoader loader(*db.instance, "sql_export_wrapper");
	auto register_wrapper = [&](const Identifier &original, const Identifier &name, const LogicalType &argument) {
		auto &entry = loader.GetTableFunction(original);
		auto function = *entry.functions.GetFunctionByArguments(*connection.context, {argument});
		function.name = name;
		function.to_sql = [](ClientContext &, const LogicalGet &get) -> TableFunctionToSQLResult {
			vector<unique_ptr<ParsedExpression>> arguments;
			for (auto &parameter : get.parameters) {
				arguments.push_back(ConstantExpression::FromValue(parameter));
			}
			for (auto &parameter : get.named_parameters) {
				arguments.push_back(make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL,
				                                                    make_uniq<ColumnRefExpression>(parameter.first),
				                                                    ConstantExpression::FromValue(parameter.second)));
			}
			auto source = make_uniq<TableFunctionRef>();
			source->function = make_uniq<FunctionExpression>(get.function.GetQualifiedName(), std::move(arguments));
			return {std::move(source), {}};
		};
		loader.RegisterFunction(std::move(function));
	};
	register_wrapper("range", "export_wrapped_range", LogicalType::BIGINT);
	string sql;
	bool ordinal = false;
	bool file_filter = false;
	SECTION("ordinary invocation") {
		sql = "SELECT * FROM export_wrapped_range(3)";
	}
	SECTION("ordinality window fusion") {
		sql = "SELECT * FROM export_wrapped_range(3) WITH ORDINALITY";
		ordinal = true;
	}
	if (db.ExtensionIsLoaded("parquet")) {
		SECTION("file pruning residual") {
			register_wrapper("read_parquet", "export_wrapped_parquet", LogicalType::VARCHAR);
			auto directory = TestJoinPath(TestDirectoryPath(), "sql_export_wrapper");
			TestCreateDirectory(directory);
			REQUIRE_NO_FAIL(connection.Query("COPY (SELECT 1 id) TO '" + directory + "/a.parquet' (FORMAT PARQUET)"));
			REQUIRE_NO_FAIL(connection.Query("COPY (SELECT 2 id) TO '" + directory + "/b.parquet' (FORMAT PARQUET)"));
			sql = "SELECT id FROM export_wrapped_parquet('" + directory +
			      "/*.parquet', filename=true) WHERE filename LIKE '%/a.parquet'";
			file_filter = true;
		}
	}
	auto native = connection.Query(sql);
	REQUIRE_NO_FAIL(*native);
	connection.BeginTransaction();
	auto plan = OptimizeLogicalPlanExportQuery(connection, sql);
	auto get_op = FindLogicalPlanExportOperator(*plan, LogicalOperatorType::LOGICAL_GET);
	REQUIRE(get_op);
	auto &get = get_op->Cast<LogicalGet>();
	if (ordinal) {
		REQUIRE(FindLogicalPlanExportOperator(*plan, LogicalOperatorType::LOGICAL_WINDOW));
		REQUIRE(get.source_ordinality == OrdinalityType::WITH_ORDINALITY);
		REQUIRE_FALSE(get.ordinality_idx.IsValid());
	}
	if (file_filter) {
		REQUIRE(get.extra_info.file_filter_expressions);
		REQUIRE_FALSE(get.extra_info.file_filter_expressions->empty());
		REQUIRE_FALSE(get.extra_info.file_filters.empty());
	}
	for (bool copy : {false, true}) {
		CAPTURE(copy);
		if (copy) {
			plan = plan->Copy(*connection.context);
			plan->ResolveOperatorTypes();
		}
		auto exported = LogicalPlanSQLExporter::Export(*connection.context, *plan);
		REQUIRE(exported.IsSuccess());
		auto generated_sql = exported.GetValue().query->ToString();
		if (ordinal) {
			REQUIRE(StringUtil::Contains(generated_sql, "WITH ORDINALITY"));
		}
		auto generated = connection.Query(generated_sql);
		REQUIRE_NO_FAIL(*generated);
		REQUIRE(generated->GetTypes() == native->GetTypes());
		REQUIRE(generated->RowCount() == native->RowCount());
		auto generated_rows = generated->Collection().GetRows();
		auto native_rows = native->Collection().GetRows();
		for (idx_t row = 0; row < native_rows.size(); row++) {
			for (idx_t column = 0; column < native->ColumnCount(); column++) {
				REQUIRE(
				    Value::NotDistinctFrom(generated_rows.GetValue(column, row), native_rows.GetValue(column, row)));
			}
		}
	}
	connection.Rollback();
}

TEST_CASE("Logical plan SQL export retains bound source arguments and reopens files",
          "[sql_export][logical_plan_sql_export][table_source_sql]") {
	DuckDB db(nullptr);
	Connection connection(db);
	REQUIRE_NO_FAIL(connection.Query("SET VARIABLE sql_export_count=4"));
	connection.BeginTransaction();
	const string variable_sql = "SELECT * FROM range(getvariable('sql_export_count')::BIGINT) ORDER BY ALL";
	auto plan = OptimizeLogicalPlanExportQuery(connection, variable_sql);
	auto exported = LogicalPlanSQLExporter::Export(*connection.context, *plan);
	REQUIRE(exported.IsSuccess());
	auto generated_sql = exported.GetValue().query->ToString();
	REQUIRE_FALSE(StringUtil::Contains(generated_sql, "getvariable"));
	REQUIRE_NO_FAIL(connection.Query("SET VARIABLE sql_export_count=2"));
	auto original = connection.Query(variable_sql);
	auto generated = connection.Query(generated_sql);
	REQUIRE_NO_FAIL(*original);
	REQUIRE_NO_FAIL(*generated);
	REQUIRE(original->RowCount() == 2);
	REQUIRE(generated->RowCount() == 4);

	auto csv_path = TestCreatePath("sql_export_generic_source.csv");
	REQUIRE_NO_FAIL(connection.Query("CREATE TABLE csv_source(i INTEGER, s VARCHAR);"
	                                 "INSERT INTO csv_source VALUES (1, 'a'), (2, NULL);"
	                                 "COPY csv_source TO " +
	                                 Value(csv_path).ToSQLString() + " (HEADER, DELIMITER '|')"));
	const auto count_sql = "SELECT count(*) FROM read_csv(" + Value(csv_path).ToSQLString() +
	                       ", header := true, delim := '|', auto_detect := true)";
	auto count_plan = OptimizeLogicalPlanExportQueryWithRepeatedPruning(connection, count_sql);
	auto &count_get = FindLogicalPlanExportOperator(*count_plan, LogicalOperatorType::LOGICAL_GET)->Cast<LogicalGet>();
	REQUIRE(count_get.GetColumnIds().size() == 1);
	REQUIRE(count_get.GetColumnIds()[0].IsEmptyColumn());
	auto count_export = LogicalPlanSQLExporter::Export(*connection.context, *count_plan);
	REQUIRE(count_export.IsSuccess());
	auto count_direct = connection.Query(count_sql);
	auto count_rebound = connection.Query(count_export.GetValue().query->ToString());
	REQUIRE_NO_FAIL(*count_direct);
	REQUIRE_NO_FAIL(*count_rebound);
	REQUIRE(SQLExportRows(*count_rebound, true) == SQLExportRows(*count_direct, true));
	connection.Rollback();
}

TEST_CASE("Logical plan SQL export retains consumed file predicates",
          "[sql_export][logical_plan_sql_export][table_source_sql]") {
	DuckDB db(nullptr);
	Connection connection(db);
	auto directory = TestCreatePath("sql_export_file_predicates");
	TestDeleteDirectory(directory);
	REQUIRE_NO_FAIL(
	    connection.Query("COPY (SELECT * FROM (VALUES (10,1), (10,1), (NULL,1), (20,2), (30,NULL)) t(i,p)) TO " +
	                     Value(directory).ToSQLString() + " (FORMAT parquet, PARTITION_BY(p))"));
	connection.BeginTransaction();
	auto source = "read_parquet(" + Value(directory + "/*/*.parquet").ToSQLString() + ", hive_partitioning := true)";
	auto sql = "SELECT i FROM " + source + " WHERE p = 1 ORDER BY i";
	auto plan = OptimizeLogicalPlanExportQuery(connection, sql);
	auto get = FindLogicalPlanExportOperator(*plan, LogicalOperatorType::LOGICAL_GET);
	REQUIRE(get);
	auto &info = get->Cast<LogicalGet>().extra_info;
	REQUIRE(info.file_filter_expressions);
	REQUIRE_FALSE(info.file_filter_expressions->empty());
	auto copy = plan->Copy(*connection.context);
	auto copied_get = FindLogicalPlanExportOperator(*copy, LogicalOperatorType::LOGICAL_GET);
	REQUIRE(copied_get);
	auto &copied_info = copied_get->Cast<LogicalGet>().extra_info;
	REQUIRE(copied_info.file_filter_expressions);
	REQUIRE(copied_info.file_filter_expressions->size() == info.file_filter_expressions->size());
	for (idx_t index = 0; index < info.file_filter_expressions->size(); index++) {
		REQUIRE((*copied_info.file_filter_expressions)[index]->Equals(*(*info.file_filter_expressions)[index]));
	}
	REQUIRE(LogicalPlanSQLExporter::Export(*connection.context, *copy).IsSuccess());
	info.file_filter_expressions.reset();
	auto missing = LogicalPlanSQLExporter::Export(*connection.context, *plan);
	REQUIRE(missing.HasError());
	REQUIRE(missing.GetIssues()[0].construct ==
	        LogicalPlanVerificationConstructIdentity::ExportFeature("file_filter_residual"));
	connection.Rollback();
	TestDeleteDirectory(directory);
}

TEST_CASE("Logical plan SQL export renders the retained table AT clause",
          "[sql_export][logical_plan_sql_export][table_source_sql]") {
	DuckDB db(nullptr);
	Connection connection(db);
	REQUIRE_NO_FAIL(connection.Query("CREATE TABLE versioned(i INTEGER)"));
	connection.BeginTransaction();
	auto plan = OptimizeLogicalPlanExportQuery(connection, "SELECT i FROM versioned");
	auto get = FindLogicalPlanExportOperator(*plan, LogicalOperatorType::LOGICAL_GET);
	REQUIRE(get);
	// Native tables reject time travel, so the clause an external catalog would bind is attached directly
	get->Cast<LogicalGet>().at_clause = make_uniq<BoundAtClause>(Identifier("VERSION"), Value::BIGINT(3));
	auto copy = plan->Copy(*connection.context);
	auto copied_get = FindLogicalPlanExportOperator(*copy, LogicalOperatorType::LOGICAL_GET);
	REQUIRE(copied_get);
	REQUIRE(copied_get->Cast<LogicalGet>().at_clause);
	REQUIRE(copied_get->Cast<LogicalGet>().at_clause->GetValue() == Value::BIGINT(3));
	for (auto exported_plan : {plan.get(), copy.get()}) {
		auto exported = LogicalPlanSQLExporter::Export(*connection.context, *exported_plan);
		REQUIRE(exported.IsSuccess());
		auto text = exported.GetValue().query->ToString();
		REQUIRE(text.find("AT (VERSION => CAST(3 AS BIGINT))") != string::npos);
	}
	connection.Rollback();
}

TEST_CASE("Table row number SQL export retains stream effects",
          "[sql_export][logical_plan_sql_export][table_row_number_sql_export]") {
	for (auto consumption : {0, 1, 2}) {
		for (bool late_error : {false, true}) {
			vector<string> expected_rows;
			Value expected_sequence;
			bool expected_error = false;
			for (auto route : {0, 3, 4, 5}) {
				if (route >= 4 && (consumption != 0 || late_error)) {
					continue;
				}
				CAPTURE(consumption, late_error, route);
				DuckDB db(nullptr);
				Connection connection(db);
				REQUIRE_NO_FAIL(connection.Query("SET threads=1; "
				                                 "SET max_streaming_buffer_size='1b'; CREATE SEQUENCE seq; "
				                                 "CREATE TABLE stream_numbers AS SELECT i FROM range(10000)t(i); "
				                                 "DELETE FROM stream_numbers WHERE i%5=1"));
				connection.BeginTransaction();
				auto sql = string("SELECT i,n,nextval('seq'),") +
				           (late_error ? "CASE WHEN n>4096 THEN error('row number stream') ELSE 'ok' END" : "'ok'") +
				           " FROM (SELECT i,row_number() OVER () n FROM stream_numbers)" +
				           (consumption == 1 ? " LIMIT 1" : "");
				auto plan = OptimizeLogicalPlanExportQuery(connection, sql);
				if (route == 4) {
					plan = plan->Copy(*connection.context);
					plan->ResolveOperatorTypes();
				}
				auto exported = LogicalPlanSQLExporter::Export(*connection.context, *plan);
				REQUIRE(exported.IsSuccess());
				QueryParameters parameters;
				unique_ptr<QueryResult> result;
				plan.reset();
				if (route == 0) {
					result = SubmitSQLExportResult(*connection.context, sql, parameters);
				} else if (route == 3 || route == 4) {
					result =
					    SubmitSQLExportResult(*connection.context, exported.GetValue().query->ToString(), parameters);
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
						string value;
						for (idx_t col = 0; col < chunk->ColumnCount(); col++) {
							value += chunk->GetValue(col, row).ToSQLString() + "|";
						}
						rows.push_back(std::move(value));
					}
					if (consumption == 2) {
						break;
					}
				}
				auto has_error = (stream ? stream->HasError() : result->HasError());
				if (has_error) {
					REQUIRE(
					    StringUtil::Contains((stream ? stream->GetError() : result->GetError()), "row number stream"));
				}
				stream.reset();
				result.reset();
				connection.Rollback();
				auto sequence = connection.Query("SELECT last_value FROM duckdb_sequences() WHERE sequence_name='seq'");
				REQUIRE_NO_FAIL(*sequence);
				if (route == 0) {
					expected_rows = std::move(rows);
					expected_sequence = sequence->Collection().GetValue(0, 0);
					expected_error = has_error;
				} else {
					REQUIRE(rows == expected_rows);
					REQUIRE(has_error == expected_error);
					REQUIRE(Value::NotDistinctFrom(sequence->Collection().GetValue(0, 0), expected_sequence));
				}
			}
		}
	}
}

TEST_CASE("Table row number SQL export guards filtered numbering",
          "[sql_export][logical_plan_sql_export][table_row_number_sql_export]") {
	DuckDB db(nullptr);
	Connection connection(db);
	REQUIRE_NO_FAIL(connection.Query("CREATE TABLE filtered_numbers AS SELECT i FROM range(100)t(i)"));
	connection.BeginTransaction();
	for (bool copy : {false, true}) {
		auto plan = OptimizeLogicalPlanExportQuery(connection, "SELECT i,row_number() OVER () FROM filtered_numbers");
		auto &get = FindLogicalPlanExportOperator(*plan, LogicalOperatorType::LOGICAL_GET)->Cast<LogicalGet>();
		auto filtered = OptimizeLogicalPlanExportQuery(connection, "SELECT i FROM filtered_numbers WHERE i>50");
		auto &filtered_get =
		    FindLogicalPlanExportOperator(*filtered, LogicalOperatorType::LOGICAL_GET)->Cast<LogicalGet>();
		REQUIRE(filtered_get.table_filters.HasFilters());
		get.table_filters = std::move(filtered_get.table_filters);
		if (copy) {
			plan = plan->Copy(*connection.context);
			plan->ResolveOperatorTypes();
		}
		auto exported = LogicalPlanSQLExporter::Export(*connection.context, *plan);
		REQUIRE(exported.HasError());
		REQUIRE(exported.GetIssues()[0].code == LogicalPlanVerificationIssueCode::UNSUPPORTED_SOURCE);
		REQUIRE(exported.GetIssues()[0].construct->function->name == "seq_scan");
		REQUIRE(exported.GetIssues()[0].facts.size() == 1);
		REQUIRE((exported.GetIssues()[0].facts[0] == pair<string, Value> {"guard", Value("row_number_with_filters")}));
		REQUIRE_NO_FAIL(connection.Query("SET debug_disable_optimizer=true"));
		auto native = connection.Query(make_uniq<LogicalPlanStatement>(std::move(plan)));
		REQUIRE_NO_FAIL(*native);
		REQUIRE(native->RowCount() == 49);
		REQUIRE(native->Collection().GetValue(0, 0) == Value::BIGINT(51));
		REQUIRE(native->Collection().GetValue(1, 0) == Value::BIGINT(1));
		REQUIRE_NO_FAIL(connection.Query("SET debug_disable_optimizer=false"));
	}
	auto plan = OptimizeLogicalPlanExportQuery(connection, "SELECT i,row_number() OVER () FROM filtered_numbers");
	auto &get = FindLogicalPlanExportOperator(*plan, LogicalOperatorType::LOGICAL_GET)->Cast<LogicalGet>();
	get.dynamic_filters = make_shared_ptr<DynamicTableFilterSet>();
	REQUIRE_FALSE(get.function.to_sql(*connection.context, get).source);
	connection.Rollback();
}

TEST_CASE("Table SQL export distinguishes pruning hints from row filters",
          "[sql_export][logical_plan_sql_export][table_row_number_sql_export]") {
	DuckDB db(nullptr);
	Connection connection(db);
	REQUIRE_NO_FAIL(connection.Query("CREATE TABLE hinted_scan AS SELECT i,i%7 v FROM range(100)t(i)"));
	connection.BeginTransaction();
	for (bool numbered : {false, true}) {
		for (bool binary : {false, true}) {
			auto sql = string("SELECT i,v") + (numbered ? ",row_number() OVER ()" : "") + " FROM hinted_scan";
			auto plan = OptimizeLogicalPlanExportQuery(connection, sql);
			auto &get = FindLogicalPlanExportOperator(*plan, LogicalOperatorType::LOGICAL_GET)->Cast<LogicalGet>();
			auto donor = OptimizeLogicalPlanExportQuery(connection, "SELECT i,v FROM hinted_scan WHERE i>v");
			auto &donor_get =
			    FindLogicalPlanExportOperator(*donor, LogicalOperatorType::LOGICAL_GET)->Cast<LogicalGet>();
			REQUIRE(donor_get.table_filters.HasMultiColumnFilters());
			REQUIRE_FALSE(donor_get.table_filters.HasFilters());
			get.table_filters = std::move(donor_get.table_filters);
			if (binary) {
				plan = plan->Copy(*connection.context);
				plan->ResolveOperatorTypes();
			}
			auto exported = LogicalPlanSQLExporter::Export(*connection.context, *plan);
			if (!numbered) {
				REQUIRE(exported.IsSuccess());
			} else {
				REQUIRE(exported.HasError());
				REQUIRE(exported.GetIssues()[0].code == LogicalPlanVerificationIssueCode::UNSUPPORTED_SOURCE);
				REQUIRE((exported.GetIssues()[0].facts[0] ==
				         pair<string, Value> {"guard", Value("row_number_with_filters")}));
			}
			REQUIRE_NO_FAIL(connection.Query("SET debug_disable_optimizer=true"));
			auto native = connection.Query(make_uniq<LogicalPlanStatement>(std::move(plan)));
			REQUIRE_NO_FAIL(*native);
			REQUIRE(native->RowCount() == 100);
			REQUIRE(native->Collection().GetValue(0, 0) == Value::BIGINT(0));
			if (numbered) {
				REQUIRE(native->Collection().GetValue(2, 0) == Value::BIGINT(1));
			}
			REQUIRE_NO_FAIL(connection.Query("SET debug_disable_optimizer=false"));
			if (!numbered) {
				auto generated = connection.Query(exported.GetValue().query->ToString());
				REQUIRE_NO_FAIL(*generated);
				REQUIRE(generated->GetTypes() == native->GetTypes());
				REQUIRE(SQLExportRows(*generated, false) == SQLExportRows(*native, false));
			}
		}
	}
	auto file_filtered = OptimizeLogicalPlanExportQuery(connection, "SELECT i,v FROM hinted_scan");
	auto &file_filtered_get =
	    FindLogicalPlanExportOperator(*file_filtered, LogicalOperatorType::LOGICAL_GET)->Cast<LogicalGet>();
	file_filtered_get.extra_info.file_filters = "i > 50";
	auto file_filtered_export = LogicalPlanSQLExporter::Export(*connection.context, *file_filtered);
	REQUIRE(file_filtered_export.HasError());
	REQUIRE(file_filtered_export.GetIssues()[0].construct ==
	        LogicalPlanVerificationConstructIdentity::ExportFeature("file_filter_residual"));
	REQUIRE_NO_FAIL(connection.Query("CREATE SEQUENCE hint_calls"));
	for (const auto &sql : {"SELECT i,v FROM hinted_scan WHERE i>v AND nextval('hint_calls')>0",
	                        "SELECT i,v FROM hinted_scan WHERE i<v"}) {
		auto plan = OptimizeLogicalPlanExportQuery(connection, sql);
		auto &get = FindLogicalPlanExportOperator(*plan, LogicalOperatorType::LOGICAL_GET)->Cast<LogicalGet>();
		auto donor = OptimizeLogicalPlanExportQuery(connection, "SELECT i,v FROM hinted_scan WHERE i>v");
		auto &donor_get = FindLogicalPlanExportOperator(*donor, LogicalOperatorType::LOGICAL_GET)->Cast<LogicalGet>();
		get.table_filters = std::move(donor_get.table_filters);
		auto exported = LogicalPlanSQLExporter::Export(*connection.context, *plan);
		REQUIRE(exported.IsSuccess());
		auto generated = connection.Query(exported.GetValue().query->ToString());
		auto fresh = connection.Query(sql);
		REQUIRE_NO_FAIL(*generated);
		REQUIRE_NO_FAIL(*fresh);
		REQUIRE(generated->GetTypes() == fresh->GetTypes());
		REQUIRE(SQLExportRows(*generated, false) == SQLExportRows(*fresh, false));
	}
	connection.Rollback();
}

TEST_CASE("Logical plan SQL export preserves secure view casts through repeated pruning",
          "[sql_export][logical_plan_sql_export][secure_view_sql_export]") {
	DuckDB db(nullptr);
	Connection connection(db);
	REQUIRE_NO_FAIL(connection.Query("CREATE SCHEMA secret"));
	REQUIRE_NO_FAIL(connection.Query(
	    "CREATE TABLE secret.path_data(s STRUCT(a INTEGER, b STRUCT(c VARCHAR, d VARCHAR)), t INTEGER)"));
	REQUIRE_NO_FAIL(connection.Query("INSERT INTO secret.path_data VALUES ({'a':1,'b':{'c':'bad','d':'2'}},10), "
	                                 "({'a':NULL,'b':{'c':'3','d':NULL}},20), (NULL,30)"));
	REQUIRE_NO_FAIL(
	    connection.Query("CREATE SECURE VIEW secret.secure_paths(first, second) AS SELECT s, t FROM secret.path_data"));
	REQUIRE_NO_FAIL(
	    connection.Query("CREATE SECURE VIEW secret.secure_paths_twice AS SELECT * FROM secret.secure_paths"));
	connection.BeginTransaction();

	const string sql = "SELECT (first.b::STRUCT(c BIGINT,d BIGINT)).d FROM secret.secure_paths_twice ORDER BY second";
	auto plan = OptimizeLogicalPlanExportQueryWithRepeatedPruning(connection, sql);
	REQUIRE(FindLogicalPlanExportOperator(*plan, LogicalOperatorType::LOGICAL_SECURE_VIEW));
	auto exported = LogicalPlanSQLExporter::Export(*connection.context, *plan);
	REQUIRE(exported.IsSuccess());
	auto text = exported.GetValue().query->ToString();
	plan.reset();
	connection.Rollback();
	auto direct = connection.Query(sql);
	auto generated = connection.Query(text);
	INFO(direct->GetError());
	INFO(generated->GetError());
	REQUIRE(direct->HasError());
	REQUIRE(generated->HasError());
	REQUIRE(StringUtil::Contains(direct->GetError(), "bad"));
	REQUIRE(StringUtil::Contains(generated->GetError(), "bad"));
	auto statement = make_uniq<SelectStatement>();
	statement->node = exported.GetValue().query->Copy();
	auto ast = connection.Query(std::move(statement));
	REQUIRE(ast->HasError());
	REQUIRE(StringUtil::Contains(ast->GetError(), "bad"));
}

TEST_CASE("Logical plan SQL export rejects incomplete secure view metadata",
          "[sql_export][logical_plan_sql_export][secure_view_sql_export]") {
	DuckDB db(nullptr);
	Connection connection(db);

	SECTION("legacy display-only node") {
		auto view = make_uniq<LogicalSecureView>("legacy", IntegerValues(TableIndex(8400), {{1}}));
		view->ResolveOperatorTypes();
		auto result = LogicalPlanSQLExporter::Export(*connection.context, *view);
		RequirePlanExportIssue(result, LogicalPlanVerificationIssueCode::UNSUPPORTED_EXPORT_FEATURE);
	}
	SECTION("incomplete caller predicates") {
		REQUIRE_NO_FAIL(connection.Query("CREATE TABLE source_data(a INTEGER, b INTEGER)"));
		REQUIRE_NO_FAIL(connection.Query("INSERT INTO source_data VALUES (1,2),(2,3),(NULL,4)"));
		REQUIRE_NO_FAIL(connection.Query("CREATE SECURE VIEW secure_data AS SELECT a,b FROM source_data"));
		connection.BeginTransaction();
		auto plan = OptimizeLogicalPlanExportQuery(connection, "SELECT b FROM secure_data WHERE a=1");
		auto &view =
		    FindLogicalPlanExportOperator(*plan, LogicalOperatorType::LOGICAL_SECURE_VIEW)->Cast<LogicalSecureView>();
		REQUIRE(view.source_filters.size() == 1);
		REQUIRE(view.pushed_filters.size() == 1);
		REQUIRE(LogicalPlanSQLExporter::Export(*connection.context, *plan).IsSuccess());
		view.source_filters[0].reset();
		auto missing_mapping = LogicalPlanSQLExporter::Export(*connection.context, *plan);
		REQUIRE(missing_mapping.HasError());
		REQUIRE(*missing_mapping.GetIssues()[0].construct->identifier == "secure_view_filter");
		view.source_filters.clear();
		auto missing_predicate = LogicalPlanSQLExporter::Export(*connection.context, *plan);
		REQUIRE(missing_predicate.HasError());
		REQUIRE(*missing_predicate.GetIssues()[0].construct->identifier == "secure_view_filter");
		connection.Rollback();
	}

	SECTION("incomplete current mapping") {
		REQUIRE_NO_FAIL(connection.Query("CREATE TABLE source_data(a INTEGER, b INTEGER)"));
		REQUIRE_NO_FAIL(connection.Query("CREATE SECURE VIEW secure_data AS SELECT a,b FROM source_data"));
		connection.BeginTransaction();
		auto plan = OptimizeLogicalPlanExportQuery(connection, "SELECT b FROM secure_data");
		auto &view =
		    FindLogicalPlanExportOperator(*plan, LogicalOperatorType::LOGICAL_SECURE_VIEW)->Cast<LogicalSecureView>();
		REQUIRE_FALSE(view.output_expressions.empty());
		view.output_expressions.pop_back();
		auto result = LogicalPlanSQLExporter::Export(*connection.context, *plan);
		RequirePlanExportIssue(result, LogicalPlanVerificationIssueCode::UNSUPPORTED_EXPORT_FEATURE,
		                       {LogicalPlanVerificationPathRoot::LOGICAL_PLAN,
		                        {{LogicalPlanVerificationPathComponentType::OPERATOR_CHILD, 0}}});
		connection.Rollback();
	}
}

// SQL cannot supply a typed replacement for an existing operator, forge an invalid
// boundary, or observe whether verification repaired the caller's type vector.
TEST_CASE("Resolved subtree export preserves its original plan", "[sql_export][logical_plan_sql_export][plan_views]") {
	DuckDB db(nullptr);
	Connection connection(db);
	connection.BeginTransaction();
	auto plan = OptimizeLogicalPlanExportQuery(
	    connection, "WITH q AS MATERIALIZED (SELECT i FROM range(3) t(i)) SELECT i+1 FROM q");
	REQUIRE(LogicalPlanVerifier::VerifyAlways(*plan).IsSuccess());
	auto source = FindLogicalPlanExportOperator(*plan, LogicalOperatorType::LOGICAL_GET);
	REQUIRE(source);
	auto &get = source->Cast<LogicalGet>();
	auto original_owner = plan.get();
	auto original_bind_data = get.bind_data.get();
	auto original_types = plan->types;
	auto original_bindings = plan->GetColumnBindings();
	auto source_bindings = get.GetColumnBindings();
	get.function.to_sql = [](ClientContext &, const LogicalGet &) -> TableFunctionToSQLResult {
		return {nullptr, "opaque_original_source"};
	};
	auto relation = [&]() {
		Parser parser(*connection.context);
		parser.ParseQuery("SELECT 42::BIGINT AS i");
		vector<LogicalPlanSQLExportField> fields;
		for (idx_t i = 0; i < get.types.size(); i++) {
			fields.push_back({source_bindings[i], get.types[i]});
		}
		return LogicalPlanSQLExportRelation {std::move(parser.statements[0]->Cast<SelectStatement>().node),
		                                     std::move(fields)};
	};

	SECTION("sources precede materialized CTE consumers") {
		for (idx_t repeat = 0; repeat < 2; repeat++) {
			vector<LogicalPlanSQLExportReplacement> replacements;
			replacements.push_back({get, relation()});
			auto exported = LogicalPlanSQLExporter::ExportWithSources(*connection.context, *plan, replacements);
			REQUIRE(exported.IsSuccess());
			auto result = connection.Query(exported.GetValue().query->ToString());
			REQUIRE_NO_FAIL(*result);
			REQUIRE(result->RowCount() == 1);
			REQUIRE(result->Collection().GetValue(0, 0) == Value::BIGINT(43));
		}
	}
	SECTION("unreplaced sources remain opaque") {
		RequirePlanExportIssue(LogicalPlanSQLExporter::ExportWithSources(*connection.context, get, {}),
		                       LogicalPlanVerificationIssueCode::UNSUPPORTED_SOURCE);
	}
	SECTION("incomplete fields") {
		auto replacement = relation();
		replacement.fields.clear();
		vector<LogicalPlanSQLExportReplacement> replacements;
		replacements.push_back({get, std::move(replacement)});
		RequirePlanExportIssue(LogicalPlanSQLExporter::ExportWithSources(*connection.context, *plan, replacements),
		                       LogicalPlanVerificationIssueCode::INTERNAL_INVARIANT);
	}
	SECTION("wrong type") {
		auto replacement = relation();
		replacement.fields[0].type = LogicalType::INTEGER;
		vector<LogicalPlanSQLExportReplacement> replacements;
		replacements.push_back({get, std::move(replacement)});
		RequirePlanExportIssue(LogicalPlanSQLExporter::ExportWithSources(*connection.context, *plan, replacements),
		                       LogicalPlanVerificationIssueCode::INTERNAL_INVARIANT);
	}
	SECTION("duplicate boundary") {
		vector<LogicalPlanSQLExportReplacement> replacements;
		replacements.push_back({get, relation()});
		replacements.push_back({get, relation()});
		RequirePlanExportIssue(LogicalPlanSQLExporter::ExportWithSources(*connection.context, *plan, replacements),
		                       LogicalPlanVerificationIssueCode::INTERNAL_INVARIANT);
	}
	SECTION("overlapping boundaries") {
		auto root_relation = relation();
		root_relation.fields[0].source_binding = original_bindings[0];
		vector<LogicalPlanSQLExportReplacement> replacements;
		replacements.push_back({*plan, std::move(root_relation)});
		replacements.push_back({get, relation()});
		RequirePlanExportIssue(LogicalPlanSQLExporter::ExportWithSources(*connection.context, *plan, replacements),
		                       LogicalPlanVerificationIssueCode::INTERNAL_INVARIANT);
	}
	SECTION("foreign boundary") {
		auto other = OptimizeLogicalPlanExportQuery(connection, "SELECT i FROM range(4) t(i)");
		REQUIRE(LogicalPlanVerifier::VerifyAlways(*other).IsSuccess());
		auto other_get = FindLogicalPlanExportOperator(*other, LogicalOperatorType::LOGICAL_GET);
		REQUIRE(other_get);
		auto replacement = relation();
		replacement.fields[0].source_binding = other_get->GetColumnBindings()[0];
		vector<LogicalPlanSQLExportReplacement> replacements;
		replacements.push_back({*other_get, std::move(replacement)});
		RequirePlanExportIssue(LogicalPlanSQLExporter::ExportWithSources(*connection.context, *plan, replacements),
		                       LogicalPlanVerificationIssueCode::INTERNAL_INVARIANT);
	}
	SECTION("read-only verification never repairs unresolved types") {
		plan->types.clear();
		REQUIRE(LogicalPlanVerifier::VerifyResolvedBindings(*plan).HasError());
		REQUIRE(plan->types.empty());
		plan->types = original_types;
		REQUIRE(LogicalPlanVerifier::VerifyResolvedBindings(*plan).IsSuccess());
	}
	REQUIRE(plan.get() == original_owner);
	REQUIRE(get.bind_data.get() == original_bind_data);
	REQUIRE(plan->types == original_types);
	REQUIRE(plan->GetColumnBindings() == original_bindings);
	REQUIRE(get.GetColumnBindings() == source_bindings);
	connection.Rollback();
}

static unique_ptr<LogicalOperator> BindResolvedReplacementPlan(Connection &connection, const string &sql) {
	Parser parser(*connection.context);
	parser.ParseQuery(sql);
	Planner planner(*connection.context);
	planner.CreatePlan(std::move(parser.statements[0]));
	REQUIRE(LogicalPlanVerifier::VerifyAlways(*planner.plan).IsSuccess());
	return std::move(planner.plan);
}

static LogicalPlanSQLExportRelation ReplacementRelation(Connection &connection, LogicalOperator &op,
                                                        const string &sql) {
	Parser parser(*connection.context);
	parser.ParseQuery(sql);
	auto bindings = op.GetColumnBindings();
	vector<LogicalPlanSQLExportField> fields;
	for (idx_t i = 0; i < bindings.size(); i++) {
		fields.push_back({bindings[i], op.types[i]});
	}
	return {std::move(parser.statements[0]->Cast<SelectStatement>().node), std::move(fields)};
}

static void RequireReplacementResult(Connection &connection, LogicalOperator &plan,
                                     const vector<LogicalPlanSQLExportReplacement> &replacements,
                                     const string &expected_sql) {
	auto exported = LogicalPlanSQLExporter::ExportWithSources(*connection.context, plan, replacements);
	REQUIRE(exported.IsSuccess());
	auto sql = exported.GetValue().query->ToString();
	INFO(sql);
	auto actual = connection.Query(sql);
	auto expected = connection.Query(expected_sql);
	REQUIRE_NO_FAIL(*actual);
	REQUIRE_NO_FAIL(*expected);
	REQUIRE(actual->GetTypes() == expected->GetTypes());
	REQUIRE(actual->RowCount() == expected->RowCount());
	for (idx_t row = 0; row < expected->RowCount(); row++) {
		for (idx_t column = 0; column < expected->GetTypes().size(); column++) {
			REQUIRE(Value::NotDistinctFrom(actual->Collection().GetValue(column, row),
			                               expected->Collection().GetValue(column, row)));
		}
	}
}

// SQL cannot select a replacement boundary or set its C++ materialization policy.
// Exercise the API here and use the resulting SQL to check evaluation and optimization.
TEST_CASE("Source replacement materialization is selected per boundary",
          "[sql_export][logical_plan_sql_export][source_materialization]") {
	DuckDB db(nullptr);
	Connection connection(db);
	REQUIRE_NO_FAIL(connection.Query("CREATE SEQUENCE source_calls"));
	connection.BeginTransaction();
	auto plan =
	    BindResolvedReplacementPlan(connection, "SELECT a.i,a.i,b.i FROM range(3) a(i),range(1) b(i) ORDER BY a.i");
	auto cross = FindLogicalPlanExportOperator(*plan, LogicalOperatorType::LOGICAL_CROSS_PRODUCT);
	REQUIRE(cross);
	vector<LogicalPlanSQLExportReplacement> replacements;
	replacements.push_back(
	    {*cross->children[0],
	     ReplacementRelation(connection, *cross->children[0], "SELECT nextval('source_calls') FROM range(3)"),
	     CTEMaterialize::CTE_MATERIALIZE_ALWAYS});
	replacements.push_back(
	    {*cross->children[1], ReplacementRelation(connection, *cross->children[1], "SELECT 42::BIGINT")});
	idx_t expected_materialized = 1;
	SECTION("explicit single evaluation") {
	}
	SECTION("normal CTE rules preserve volatile evaluation") {
		replacements[0].materialized = CTEMaterialize::CTE_MATERIALIZE_DEFAULT;
		expected_materialized = 0;
	}
	auto exported = LogicalPlanSQLExporter::ExportWithSources(*connection.context, *plan, replacements);
	REQUIRE(exported.IsSuccess());
	auto &definitions = exported.GetValue().query->cte_map.map;
	REQUIRE(definitions.size() == 2);
	idx_t materialized = 0, default_policy = 0;
	for (auto &entry : definitions) {
		materialized += entry.second->materialized == CTEMaterialize::CTE_MATERIALIZE_ALWAYS;
		default_policy += entry.second->materialized == CTEMaterialize::CTE_MATERIALIZE_DEFAULT;
	}
	REQUIRE(materialized == expected_materialized);
	REQUIRE(default_policy == 2 - expected_materialized);
	RequireReplacementResult(connection, *plan, replacements,
	                         "VALUES (1::BIGINT,1::BIGINT,42::BIGINT),(2,2,42),(3,3,42)");
	auto calls = connection.Query("SELECT currval('source_calls')");
	REQUIRE_NO_FAIL(*calls);
	REQUIRE(calls->Collection().GetValue(0, 0) == Value::BIGINT(3));
	connection.Rollback();
}

TEST_CASE("Source replacement policy preserves optimizer and user CTE choices",
          "[sql_export][logical_plan_sql_export][source_materialization]") {
	DuckDB db(nullptr);
	Connection connection(db);
	REQUIRE_NO_FAIL(connection.Query("CREATE TABLE source_values AS SELECT i FROM range(10) t(i)"));
	connection.BeginTransaction();
	string query = "SELECT i FROM range(10) t(i) WHERE i=3";
	bool user_cte = false, empty = false, nested_cte = false;
	SECTION("filter pushdown") {
	}
	SECTION("empty input pruning") {
		query = "SELECT i FROM range(10) t(i) WHERE false";
		empty = true;
	}
	SECTION("user materialized CTE") {
		query = "WITH q AS MATERIALIZED (SELECT i FROM range(10) t(i)) SELECT i FROM q WHERE i=3";
		user_cte = true;
	}
	SECTION("materialized CTE inside replacement") {
		nested_cte = true;
	}
	auto plan = BindResolvedReplacementPlan(connection, query);
	auto boundary = FindLogicalPlanExportOperator(*plan, LogicalOperatorType::LOGICAL_GET);
	REQUIRE(boundary);
	for (auto policy : {CTEMaterialize::CTE_MATERIALIZE_ALWAYS, CTEMaterialize::CTE_MATERIALIZE_DEFAULT,
	                    CTEMaterialize::CTE_MATERIALIZE_NEVER}) {
		vector<LogicalPlanSQLExportReplacement> replacements;
		auto source_sql = nested_cte ? "WITH r AS MATERIALIZED (SELECT i FROM source_values) SELECT i FROM r"
		                             : "SELECT i FROM source_values";
		replacements.push_back({*boundary, ReplacementRelation(connection, *boundary, source_sql), policy});
		auto exported = LogicalPlanSQLExporter::ExportWithSources(*connection.context, *plan, replacements);
		REQUIRE(exported.IsSuccess());
		auto optimized = OptimizeLogicalPlanExportQuery(connection, exported.GetValue().query->ToString());
		if (!empty) {
			bool expect_materialized = policy == CTEMaterialize::CTE_MATERIALIZE_ALWAYS || user_cte || nested_cte;
			REQUIRE(bool(FindLogicalPlanExportOperator(*optimized, LogicalOperatorType::LOGICAL_MATERIALIZED_CTE)) ==
			        expect_materialized);
			if (!expect_materialized) {
				auto get = FindLogicalPlanExportOperator(*optimized, LogicalOperatorType::LOGICAL_GET);
				REQUIRE(get);
				REQUIRE(get->Cast<LogicalGet>().table_filters.HasFilters());
			}
		} else if (policy != CTEMaterialize::CTE_MATERIALIZE_ALWAYS) {
			REQUIRE(FindLogicalPlanExportOperator(*optimized, LogicalOperatorType::LOGICAL_EMPTY_RESULT));
			REQUIRE_FALSE(FindLogicalPlanExportOperator(*optimized, LogicalOperatorType::LOGICAL_GET));
		}
		RequireReplacementResult(connection, *plan, replacements, query);
	}
	connection.Rollback();
}

// Only the API can select an existing operator as a replacement boundary. The
// assertions below execute SQL to distinguish replacement from original input.
TEST_CASE("Resolved replacements take precedence over CTE producer shortcuts",
          "[sql_export][logical_plan_sql_export][plan_view_regression]") {
	DuckDB db(nullptr);
	Connection connection(db);
	connection.BeginTransaction();
	string query, replacement_sql, expected_sql;
	bool recursive = false;
	SECTION("identity projection producer") {
		query = "WITH q AS MATERIALIZED (SELECT i FROM range(3) t(i)) SELECT i+1 FROM q";
		replacement_sql = "SELECT 42::BIGINT AS i";
		expected_sql = "SELECT 43::BIGINT";
	}
	SECTION("recursive producer") {
		query = "WITH RECURSIVE q(i) AS MATERIALIZED "
		        "(SELECT 1 UNION ALL SELECT i+1 FROM q WHERE i<3) SELECT i+1 FROM q";
		replacement_sql = "SELECT 42::INTEGER AS i";
		expected_sql = "SELECT 43::INTEGER";
		recursive = true;
	}
	auto plan = BindResolvedReplacementPlan(connection, query);
	auto cte = FindLogicalPlanExportOperator(*plan, LogicalOperatorType::LOGICAL_MATERIALIZED_CTE);
	REQUIRE(cte);
	auto boundary = recursive ? FindLogicalPlanExportOperator(*cte, LogicalOperatorType::LOGICAL_RECURSIVE_CTE)
	                          : optional_ptr<LogicalOperator>(cte->children[0].get());
	REQUIRE(boundary);
	if (!recursive) {
		REQUIRE(boundary->type == LogicalOperatorType::LOGICAL_PROJECTION);
		auto fields = LogicalPlanSQLExportHelpers::CreateFields(*boundary->children[0], {});
		REQUIRE(fields.IsSuccess());
		REQUIRE(
		    LogicalPlanSQLExportHelpers::IsIdentityProjection(boundary->Cast<LogicalProjection>(), fields.GetValue()));
	}
	vector<LogicalPlanSQLExportReplacement> replacements;
	replacements.push_back({*boundary, ReplacementRelation(connection, *boundary, replacement_sql)});
	auto original_sql = replacements[0].relation.query->ToString();
	for (idx_t repeat = 0; repeat < 2; repeat++) {
		RequireReplacementResult(connection, *plan, replacements, expected_sql);
		REQUIRE(replacements[0].relation.query->ToString() == original_sql);
	}
	connection.Rollback();
}

TEST_CASE("Aggregate recovery does not inspect a replaced projection",
          "[sql_export][logical_plan_sql_export][plan_view_regression]") {
	DuckDB db(nullptr);
	Connection connection(db);
	connection.BeginTransaction();
	auto plan = BindResolvedReplacementPlan(connection, "SELECT sum(x) FROM (SELECT 1 AS x FROM range(3)) t");
	auto aggregate = FindLogicalPlanExportOperator(*plan, LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY);
	REQUIRE(aggregate);
	auto &boundary = *aggregate->children[0];
	REQUIRE(boundary.type == LogicalOperatorType::LOGICAL_PROJECTION);
	auto ordinary = LogicalPlanSQLExporter::Export(*connection.context, *plan);
	auto unchanged = LogicalPlanSQLExporter::ExportWithSources(*connection.context, *plan, {});
	REQUIRE(ordinary.IsSuccess());
	REQUIRE(unchanged.IsSuccess());
	REQUIRE(ordinary.GetValue().query->ToString() == unchanged.GetValue().query->ToString());
	RequireReplacementResult(connection, *plan, {}, "SELECT sum(x) FROM (SELECT 1 AS x FROM range(3)) t");
	vector<LogicalPlanSQLExportReplacement> replacements;
	replacements.push_back({boundary, ReplacementRelation(connection, boundary, "SELECT 42::INTEGER AS x")});
	RequireReplacementResult(connection, *plan, replacements, "SELECT sum(42::INTEGER)");
	connection.Rollback();
}

TEST_CASE("Generated source names do not capture replacement catalog references",
          "[sql_export][logical_plan_sql_export][plan_view_regression]") {
	DuckDB db(nullptr);
	Connection connection(db);
	REQUIRE_NO_FAIL(connection.Query("CREATE TABLE __export_source(c0 BIGINT); "
	                                 "INSERT INTO __export_source VALUES (42),(42),(NULL); "
	                                 "CREATE TABLE r0 AS FROM __export_source"));
	connection.BeginTransaction();
	auto plan = BindResolvedReplacementPlan(connection,
	                                        "SELECT a.i,b.i FROM range(1) a(i), range(1) b(i) ORDER BY b.i NULLS LAST");
	auto cross = FindLogicalPlanExportOperator(*plan, LogicalOperatorType::LOGICAL_CROSS_PRODUCT);
	REQUIRE(cross);
	for (auto sql : {"SELECT c0 FROM __export_source", "SELECT c0 FROM __EXPORT_SOURCE",
	                 "SELECT (SELECT max(c0) FROM __export_source) AS c0 "
	                 "UNION ALL SELECT c0 FROM __export_source WHERE c0 IS NULL "
	                 "UNION ALL SELECT max(c0) FROM __export_source",
	                 "WITH local_q AS MATERIALIZED (SELECT c0 FROM __export_source) SELECT c0 FROM local_q",
	                 "SELECT max::BIGINT AS c0 FROM (SUMMARIZE SELECT c0 FROM __export_source), range(2) "
	                 "UNION ALL SELECT NULL::BIGINT",
	                 "SELECT c0 FROM __export_source UNION ALL SELECT c0 FROM r0 WHERE false"}) {
		CAPTURE(sql);
		vector<LogicalPlanSQLExportReplacement> replacements;
		replacements.push_back(
		    {*cross->children[0], ReplacementRelation(connection, *cross->children[0], "SELECT 1::BIGINT")});
		replacements.push_back({*cross->children[1], ReplacementRelation(connection, *cross->children[1], sql)});
		RequireReplacementResult(connection, *plan, replacements,
		                         "SELECT 1::BIGINT,c0 FROM __export_source ORDER BY c0 NULLS LAST");
	}
	vector<LogicalPlanSQLExportReplacement> root_replacement;
	root_replacement.push_back({*plan, ReplacementRelation(connection, *plan, "SELECT 7::BIGINT,8::BIGINT")});
	RequireReplacementResult(connection, *plan, root_replacement, "SELECT 7::BIGINT,8::BIGINT");
	connection.Rollback();
}

TEST_CASE("Generated source names do not capture exported plan references",
          "[sql_export][logical_plan_sql_export][plan_view_regression]") {
	DuckDB db(nullptr);
	Connection connection(db);
	REQUIRE_NO_FAIL(connection.Query("CREATE TABLE capture_target(c0 BIGINT); INSERT INTO capture_target VALUES (7)"));
	connection.BeginTransaction();
	auto plan = BindResolvedReplacementPlan(connection, "SELECT a.i, c0 FROM range(1) a(i), capture_target");
	auto cross = FindLogicalPlanExportOperator(*plan, LogicalOperatorType::LOGICAL_CROSS_PRODUCT);
	REQUIRE(cross);
	optional_ptr<LogicalGet> table_get;
	optional_ptr<LogicalOperator> replaced;
	for (auto &child : cross->children) {
		auto get = FindLogicalPlanExportOperator(*child, LogicalOperatorType::LOGICAL_GET);
		REQUIRE(get);
		if (get->Cast<LogicalGet>().GetTable()) {
			table_get = get->Cast<LogicalGet>();
		} else {
			replaced = child.get();
		}
	}
	REQUIRE(table_get);
	REQUIRE(replaced);
	vector<LogicalPlanSQLExportReplacement> replacements;
	replacements.push_back({*replaced, ReplacementRelation(connection, *replaced, "SELECT 1::BIGINT AS i")});
	SECTION("an unqualified callback reference matching a generated name is refused") {
		table_get->function.to_sql = [](ClientContext &, const LogicalGet &) -> TableFunctionToSQLResult {
			auto reference = make_uniq<BaseTableRef>();
			reference->SetTable(Identifier("__export_source"));
			return {std::move(reference), {}};
		};
		auto exported = LogicalPlanSQLExporter::ExportWithSources(*connection.context, *plan, replacements);
		RequirePlanExportIssue(exported, LogicalPlanVerificationIssueCode::INTERNAL_INVARIANT);
	}
	SECTION("a non-colliding callback reference binds past the generated names") {
		table_get->function.to_sql = [](ClientContext &, const LogicalGet &) -> TableFunctionToSQLResult {
			auto reference = make_uniq<BaseTableRef>();
			reference->SetTable(Identifier("capture_target"));
			return {std::move(reference), {}};
		};
		RequireReplacementResult(connection, *plan, replacements, "SELECT 1::BIGINT AS i, 7::BIGINT AS c0");
	}
	connection.Rollback();
}

TEST_CASE("Resolved subtree export rejects a plan with a detached child",
          "[sql_export][logical_plan_sql_export][plan_views]") {
	DuckDB db(nullptr);
	Connection connection(db);
	connection.BeginTransaction();
	auto plan = BindResolvedReplacementPlan(connection, "SELECT i + 1 FROM range(3) t(i)");
	REQUIRE_FALSE(plan->children.empty());
	auto detached = std::move(plan->children[0]);
	auto exported = LogicalPlanSQLExporter::ExportWithSources(*connection.context, *plan, {});
	REQUIRE(exported.HasError());
	REQUIRE(exported.GetIssues()[0].code == LogicalPlanVerificationIssueCode::INTERNAL_INVARIANT);
	REQUIRE(StringUtil::Contains(exported.GetIssues()[0].message, "is null"));
	plan->children[0] = std::move(detached);
	REQUIRE(LogicalPlanVerifier::VerifyResolvedBindings(*plan).IsSuccess());
	connection.Rollback();
}

TEST_CASE("Window ordinality fusion respects replacement boundaries",
          "[sql_export][logical_plan_sql_export][plan_view_regression]") {
	DuckDB db(nullptr);
	Connection connection(db);
	connection.BeginTransaction();
	auto plan = OptimizeLogicalPlanExportQuery(connection, "SELECT * FROM range(3) WITH ORDINALITY");
	auto window = FindLogicalPlanExportOperator(*plan, LogicalOperatorType::LOGICAL_WINDOW);
	REQUIRE(window);
	auto &boundary = *window->children[0];
	REQUIRE(boundary.type == LogicalOperatorType::LOGICAL_GET);
	auto &get = boundary.Cast<LogicalGet>();
	REQUIRE(get.source_ordinality == OrdinalityType::WITH_ORDINALITY);
	REQUIRE_FALSE(get.ordinality_idx.IsValid());
	vector<LogicalPlanSQLExportReplacement> replacements;
	replacements.push_back(
	    {boundary, ReplacementRelation(connection, boundary, "SELECT 10::BIGINT UNION ALL SELECT 20::BIGINT")});
	auto exported = LogicalPlanSQLExporter::ExportWithSources(*connection.context, *plan, replacements);
	REQUIRE(exported.IsSuccess());
	auto sql = exported.GetValue().query->ToString();
	INFO(sql);
	REQUIRE_FALSE(StringUtil::Contains(StringUtil::Lower(sql), "range("));
	auto result = connection.Query("SELECT * FROM (" + sql + ") t ORDER BY 1");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->RowCount() == 2);
	REQUIRE(result->Collection().GetValue(0, 0) == Value::BIGINT(10));
	REQUIRE(result->Collection().GetValue(0, 1) == Value::BIGINT(20));
	vector<Value> ordinals = {result->Collection().GetValue(1, 0), result->Collection().GetValue(1, 1)};
	std::sort(ordinals.begin(), ordinals.end(), [](const Value &a, const Value &b) { return a < b; });
	REQUIRE(ordinals[0] == Value::BIGINT(1));
	REQUIRE(ordinals[1] == Value::BIGINT(2));
	connection.Rollback();
}

// Unlike parser-produced SELECT nodes, an API-built SELECT may have no from_table.
TEST_CASE("Replacement name reservation accepts a SELECT without a FROM node",
          "[sql_export][logical_plan_sql_export][plan_view_null_from]") {
	DuckDB db(nullptr);
	Connection connection(db);
	connection.BeginTransaction();
	auto plan = BindResolvedReplacementPlan(connection, "SELECT 1::BIGINT");
	auto relation = ReplacementRelation(connection, *plan, "SELECT 42::BIGINT");
	relation.query->Cast<SelectNode>().from_table.reset();
	vector<LogicalPlanSQLExportReplacement> replacements;
	replacements.push_back({*plan, std::move(relation)});
	RequireReplacementResult(connection, *plan, replacements, "SELECT 42::BIGINT");
	REQUIRE_FALSE(replacements[0].relation.query->Cast<SelectNode>().from_table);
	connection.Rollback();
}

TEST_CASE("LIMIT reconstruction respects replacement boundaries",
          "[sql_export][logical_plan_sql_export][plan_view_limit]") {
	DuckDB db(nullptr);
	Connection connection(db);
	connection.BeginTransaction();
	auto plan = BindResolvedReplacementPlan(connection, "SELECT i FROM range(3) t(i) LIMIT (SELECT 2::BIGINT)");
	auto limit = FindLogicalPlanExportOperator(*plan, LogicalOperatorType::LOGICAL_LIMIT);
	REQUIRE(limit);
	auto &input = *limit->children[0];
	REQUIRE(input.type == LogicalOperatorType::LOGICAL_PROJECTION);
	SECTION("row-stream projection cannot supply an independent SQL limit") {
		REQUIRE(input.types.size() == 2);
		vector<LogicalPlanSQLExportReplacement> replacements;
		replacements.push_back(
		    {input, ReplacementRelation(connection, input, "SELECT i,1::BIGINT AS n FROM range(3) t(i)")});
		auto exported = LogicalPlanSQLExporter::ExportWithSources(*connection.context, *plan, replacements);
		if (exported.IsSuccess()) {
			auto sql = exported.GetValue().query->ToString();
			auto actual = connection.Query(sql);
			REQUIRE_NO_FAIL(*actual);
			INFO(sql);
			CAPTURE(actual->RowCount());
			REQUIRE(exported.HasError());
		}
		REQUIRE(plan->children[0].get() == limit.get());
		RequirePlanExportIssue(exported, LogicalPlanVerificationIssueCode::UNSUPPORTED_EXPORT_FEATURE,
		                       {LogicalPlanVerificationPathRoot::LOGICAL_PLAN,
		                        {{LogicalPlanVerificationPathComponentType::OPERATOR_CHILD, 0},
		                         {LogicalPlanVerificationPathComponentType::OPERATOR_CHILD, 0}}});
		REQUIRE(exported.GetIssues()[0].construct ==
		        LogicalPlanVerificationConstructIdentity::ExportFeature("limit_binding"));
	}
	SECTION("independent scalar source remains replaceable") {
		auto cross = FindLogicalPlanExportOperator(input, LogicalOperatorType::LOGICAL_CROSS_PRODUCT);
		REQUIRE(cross);
		auto &scalar = *cross->children[1];
		REQUIRE(scalar.types == vector<LogicalType> {LogicalType::BIGINT});
		vector<LogicalPlanSQLExportReplacement> replacements;
		replacements.push_back({scalar, ReplacementRelation(connection, scalar, "SELECT 1::BIGINT")});
		RequireReplacementResult(connection, *plan, replacements, "SELECT 0::BIGINT");
	}
	connection.Rollback();
}

// SQL cannot replace a bound dummy scan. This API check establishes that its replacement
// supplies the row stream even when the parent only projects scalar expressions.
TEST_CASE("Scalar projections preserve replacement dummy cardinality",
          "[sql_export][logical_plan_sql_export][plan_views]") {
	DuckDB db(nullptr);
	Connection connection(db);
	connection.BeginTransaction();
	auto plan = BindResolvedReplacementPlan(connection, "SELECT 42::INTEGER, NULL::VARCHAR");
	auto dummy = FindLogicalPlanExportOperator(*plan, LogicalOperatorType::LOGICAL_DUMMY_SCAN);
	REQUIRE(dummy);
	RequireReplacementResult(connection, *plan, {}, "SELECT 42::INTEGER, NULL::VARCHAR");
	for (auto count : {0, 1, 3}) {
		auto rows = std::to_string(count);
		vector<LogicalPlanSQLExportReplacement> replacements;
		replacements.push_back(
		    {*dummy, ReplacementRelation(connection, *dummy, "SELECT NULL::INTEGER FROM range(" + rows + ")")});
		RequireReplacementResult(connection, *plan, replacements,
		                         "SELECT 42::INTEGER, NULL::VARCHAR FROM range(" + rows + ")");
	}
	connection.Rollback();
}

// Replacing the singleton input of a bound VALUES node also requires the source API.
TEST_CASE("VALUES preserves replacement dummy cardinality", "[sql_export][logical_plan_sql_export][plan_views]") {
	DuckDB db(nullptr);
	Connection connection(db);
	connection.BeginTransaction();
	auto plan = BindResolvedReplacementPlan(connection, "VALUES (42::INTEGER), (42::INTEGER)");
	auto dummy = FindLogicalPlanExportOperator(*plan, LogicalOperatorType::LOGICAL_DUMMY_SCAN);
	REQUIRE(dummy);
	RequireReplacementResult(connection, *plan, {}, "VALUES (42::INTEGER), (42::INTEGER)");
	for (auto count : {0, 1, 3}) {
		auto rows = std::to_string(count);
		vector<LogicalPlanSQLExportReplacement> replacements;
		replacements.push_back(
		    {*dummy, ReplacementRelation(connection, *dummy, "SELECT NULL::INTEGER FROM range(" + rows + ")")});
		RequireReplacementResult(connection, *plan, replacements, "SELECT 42::INTEGER FROM range(" + rows + " * 2)");
	}
	connection.Rollback();
}

} // namespace logical_plan_sql_export_test
