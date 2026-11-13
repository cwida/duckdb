#include "sql_export_test_helpers.hpp"
#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/parser/statement/logical_plan_statement.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/planner/logical_plan_sql_exporter.hpp"
#include "duckdb/planner/sql_export_helpers.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_limit.hpp"
#include "duckdb/planner/operator/logical_top_n.hpp"
#include <stdexcept>
#include <type_traits>
#include "logical_plan_sql_export_test_helpers.hpp"

using namespace duckdb;

namespace logical_plan_sql_export_test {

TEST_CASE("Logical plan SQL export preserves partial CTE streams and errors",
          "[sql_export][logical_plan_sql_export][cte_sql_export]") {
	DuckDB db(nullptr);
	Connection connection(db);
	REQUIRE_NO_FAIL(connection.Query("SET threads=1; SET max_streaming_buffer_size='1b'"));
	REQUIRE_NO_FAIL(connection.Query(
	    "CREATE TABLE cte_stream AS SELECT CASE WHEN i=4096 THEN 'bad' ELSE '1' END s FROM range(4097)t(i)"));
	for (auto scenario : {"producer error", "consumer error", "close consumer"}) {
		CAPTURE(scenario);
		const bool producer = string(scenario) == "producer error";
		const bool finish = string(scenario) == "consumer error";
		connection.BeginTransaction();
		auto plan = OptimizeLogicalPlanExportQuery(
		    connection, producer
		                    ? "WITH c AS MATERIALIZED (SELECT CAST(s AS INTEGER) x FROM cte_stream) SELECT x FROM c"
		                    : "WITH c AS MATERIALIZED (SELECT s FROM cte_stream) SELECT CAST(s AS INTEGER) FROM c");
		REQUIRE(FindLogicalPlanExportOperator(*plan, LogicalOperatorType::LOGICAL_MATERIALIZED_CTE));
		auto exported = LogicalPlanSQLExporter::Export(*connection.context, *plan);
		REQUIRE(exported.IsSuccess());
		QueryParameters parameters;
		auto check = [&](unique_ptr<QueryResult> result) {
			if (result->HasError()) {
				REQUIRE(producer);
				REQUIRE(result->GetErrorType() == ExceptionType::CONVERSION);
				return idx_t(0);
			}
			REQUIRE_FALSE(result->HasError());
			REQUIRE(result->IsOpen());
			QueryResultStream stream(std::move(result));
			idx_t count = 0;
			while (auto chunk = stream.Fetch()) {
				REQUIRE(chunk->size() > 0);
				REQUIRE(chunk->GetValue(0, 0) == Value::INTEGER(1));
				count += chunk->size();
				if (!producer && !finish) {
					stream.Close();
					break;
				}
			}
			REQUIRE(stream.HasError() == (producer || finish));
			if (producer || finish) {
				REQUIRE(stream.GetErrorType() == ExceptionType::CONVERSION);
			}
			REQUIRE(count > 0);
			return count;
		};
		REQUIRE_NO_FAIL(connection.Query("SET debug_disable_optimizer=true"));
		auto native_count = check(
		    SubmitSQLExportResult(*connection.context, make_uniq<LogicalPlanStatement>(std::move(plan)), parameters));
		connection.Rollback();
		REQUIRE_NO_FAIL(connection.Query("SET debug_disable_optimizer=false"));
		REQUIRE(check(SubmitSQLExportResult(*connection.context, exported.GetValue().query->ToString(), parameters)) ==
		        native_count);
		auto statement = make_uniq<SelectStatement>();
		statement->node = std::move(exported.GetValue().query);
		REQUIRE(check(SubmitSQLExportResult(*connection.context, std::move(statement), parameters)) == native_count);
		REQUIRE_NO_FAIL(connection.Query("SELECT 42"));
	}
}

TEST_CASE("Logical plan SQL export rejects recursive wrappers when inlining is unavailable",
          "[sql_export][logical_plan_sql_export][recursive_cte_sql_export]") {
	DuckDB db(nullptr);
	Connection connection(db);
	REQUIRE_NO_FAIL(connection.Query("SET threads=1"));
	connection.BeginTransaction();
	auto plan = OptimizeLogicalPlanExportQuery(
	    connection, "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c WHERE x<3) SELECT x FROM c LIMIT 1");
	REQUIRE(FindLogicalPlanExportOperator(*plan, LogicalOperatorType::LOGICAL_RECURSIVE_CTE));
	REQUIRE_FALSE(FindLogicalPlanExportOperator(*plan, LogicalOperatorType::LOGICAL_MATERIALIZED_CTE));
	REQUIRE_NO_FAIL(connection.Query("SET disabled_optimizers='cte_inlining'"));
	auto exported = LogicalPlanSQLExporter::Export(*connection.context, *plan);
	REQUIRE(exported.HasError());
	REQUIRE(*exported.GetIssues()[0].construct ==
	        LogicalPlanVerificationConstructIdentity::ExportFeature("recursive_cte_materialization"));
	REQUIRE_NO_FAIL(connection.Query("SET debug_disable_optimizer=true"));
	auto native = connection.Query(make_uniq<LogicalPlanStatement>(std::move(plan)));
	REQUIRE_NO_FAIL(*native);
	REQUIRE(CHECK_COLUMN(native, 0, {1}));
	connection.Rollback();
}

TEST_CASE("SQL export retains unpruned offsets through LIMIT and TopN copies",
          "[sql_export][logical_plan_sql_export][table_source_sql]") {
	DuckDB db(nullptr);
	Connection connection(db);
	REQUIRE_NO_FAIL(connection.Query("CREATE TABLE offset_rows AS SELECT i::INTEGER AS i FROM range(245760) t(i)"));
	REQUIRE_NO_FAIL(connection.Query("CHECKPOINT"));
	REQUIRE_NO_FAIL(connection.Query("INSERT INTO offset_rows VALUES (-1)"));
	REQUIRE_NO_FAIL(connection.Query("SET disabled_optimizers='statistics_propagation'"));
	connection.BeginTransaction();
	for (idx_t offset : {idx_t(1), idx_t(122880), idx_t(200000)}) {
		auto sql = string("SELECT i FROM offset_rows ORDER BY i ") + (offset == 200000 ? "" : "LIMIT 1 ") + "OFFSET " +
		           to_string(offset);
		CAPTURE(sql);
		auto original = OptimizeLogicalPlanExportQuery(connection, sql);
		for (bool binary : {false, true}) {
			auto copy = binary ? original->Copy(*connection.context) : nullptr;
			auto &plan = copy ? *copy : *original;
			auto &get = FindLogicalPlanExportOperator(plan, LogicalOperatorType::LOGICAL_GET)->Cast<LogicalGet>();
			REQUIRE(get.row_group_order_options);
			REQUIRE(get.row_group_order_options->row_group_offset > 0);
			auto top_n = FindLogicalPlanExportOperator(plan, LogicalOperatorType::LOGICAL_TOP_N);
			auto limit = FindLogicalPlanExportOperator(plan, LogicalOperatorType::LOGICAL_LIMIT);
			REQUIRE((top_n || limit));
			REQUIRE(bool(top_n) == (offset == 1));
			auto &retained =
			    top_n ? top_n->Cast<LogicalTopN>().unpruned_offset : limit->Cast<LogicalLimit>().unpruned_offset;
			REQUIRE(retained.IsValid());
			REQUIRE(retained.GetIndex() == offset);
			auto reduced =
			    top_n ? top_n->Cast<LogicalTopN>().offset : limit->Cast<LogicalLimit>().offset_val.GetConstantValue();
			REQUIRE(reduced < offset);
			auto exported = LogicalPlanSQLExporter::Export(*connection.context, plan);
			REQUIRE(exported.IsSuccess());
			if (binary) {
				retained.SetInvalid();
				auto missing = LogicalPlanSQLExporter::Export(*connection.context, plan);
				REQUIRE(missing.HasError());
				REQUIRE(missing.GetIssues()[0].code == LogicalPlanVerificationIssueCode::UNSUPPORTED_EXPORT_FEATURE);
				REQUIRE(missing.GetIssues()[0].construct->identifier == "pruned_offset");
			}
		}
	}
	connection.Rollback();
}

} // namespace logical_plan_sql_export_test
