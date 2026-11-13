#include "sql_export_test_helpers.hpp"
#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/catalog/catalog_entry/aggregate_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/sequence_catalog_entry.hpp"
#include "duckdb/parser/parsed_data/create_aggregate_function_info.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/statement/logical_plan_statement.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/logical_plan_sql_exporter.hpp"
#include "duckdb/planner/sql_export_helpers.hpp"
#include "duckdb/planner/operator/logical_pivot.hpp"
#include <stdexcept>
#include <type_traits>
#include "logical_plan_sql_export_test_helpers.hpp"

using namespace duckdb;

namespace logical_plan_sql_export_test {

static void RequirePivotStreamingEffects(Connection &connection) {
	for (idx_t route = 0; route < 4; route++) {
		CAPTURE(route);
		auto sequence = "pivot_stream_" + to_string(route);
		REQUIRE_NO_FAIL(connection.Query("CREATE SEQUENCE " + sequence));
		auto &sequence_entry =
		    Catalog::GetEntry<SequenceCatalogEntry>(*connection.context, QualifiedName::Parse(sequence));
		auto sql = "SELECT g,a_s FROM (SELECT i g, CASE WHEN i%2=0 THEN 'a' ELSE 'b' END k, i v, "
		           "nextval('" +
		           sequence +
		           "') e FROM range(5000)t(i)) t PIVOT (sum(v) AS s, max(e) AS effect FOR k IN ('a','b','z'))";
		QueryParameters parameters;
		unique_ptr<QueryResult> result;
		if (route == 0) {
			result = SubmitSQLExportResult(*connection.context, sql, parameters);
		} else {
			auto plan = OptimizeLogicalPlanExportQuery(connection, sql);
			if (route == 3) {
				plan = plan->Copy(*connection.context);
			}
			auto exported = LogicalPlanSQLExporter::Export(*connection.context, *plan);
			if (exported.HasError()) {
				INFO(exported.GetIssues()[0].message);
				REQUIRE(exported.IsSuccess());
			}
			if (route == 2) {
				auto statement = make_uniq<SelectStatement>();
				statement->node = std::move(exported.GetValue().query);
				result = SubmitSQLExportResult(*connection.context, std::move(statement), parameters);
			} else {
				result = SubmitSQLExportResult(*connection.context, exported.GetValue().query->ToString(), parameters);
			}
		}
		REQUIRE_FALSE(result->HasError());
		REQUIRE(result->IsOpen());
		QueryResultStream stream(std::move(result));
		auto first = stream.Fetch();
		REQUIRE(first);
		REQUIRE(first->size() > 0);
		// The barrier evaluated every nextval before the first row reached the consumer
		REQUIRE(sequence_entry.CurrentValue() == 5000);
		// Draining ends the statement: closing it early would abort it, and invalidate the transaction
		while (stream.Fetch()) {
		}
		REQUIRE_FALSE(stream.HasError());
		auto effect = connection.Query("SELECT currval('" + sequence + "')");
		REQUIRE_NO_FAIL(*effect);
		REQUIRE(effect->Collection().GetValue(0, 0) == Value::BIGINT(5000));
	}
}

static BoundAggregateExpression &PivotAggregate(LogicalOperator &plan, idx_t aggregate_idx = 0) {
	auto pivot = FindLogicalPlanExportOperator(plan, LogicalOperatorType::LOGICAL_PIVOT);
	REQUIRE(pivot);
	auto &aggregates = pivot->Cast<LogicalPivot>().bound_pivot.aggregates;
	REQUIRE(aggregate_idx < aggregates.size());
	REQUIRE(aggregates[aggregate_idx]->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE);
	return aggregates[aggregate_idx]->Cast<BoundAggregateExpression>();
}

static void RequirePivotSQLExportFailure(Connection &connection, unique_ptr<LogicalOperator> plan,
                                         const string &message) {
	auto exported = LogicalPlanSQLExporter::Export(*connection.context, *plan);
	REQUIRE(exported.HasError());
	REQUIRE(exported.GetIssues().size() == 1);
	INFO(exported.GetIssues()[0].message);
	REQUIRE(exported.GetIssues()[0].code == LogicalPlanVerificationIssueCode::UNSUPPORTED_EXPORT_FEATURE);
	REQUIRE(StringUtil::Contains(exported.GetIssues()[0].message, message));
}

struct PivotNonstandardEmptySum {
	static bool IgnoreNull() {
		return true;
	}

	static void Initialize(int64_t &state) {
		state = 0;
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void Operation(STATE &state, const INPUT_TYPE &input, AggregateUnaryInput &) {
		state += input.GetSize();
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void ConstantOperation(STATE &state, const INPUT_TYPE &input, AggregateUnaryInput &, idx_t count) {
		state += input.GetSize() * count;
	}

	template <class STATE, class OP>
	static void Combine(const STATE &source, STATE &target, AggregateInputData &) {
		target += source;
	}

	template <class RESULT_TYPE, class STATE>
	static void Finalize(STATE &state, RESULT_TYPE &result, AggregateFinalizeData &) {
		result = state + 99;
	}
};

struct PivotNonstandardEmptyIntegerSum {
	static bool IgnoreNull() {
		return true;
	}

	static void Initialize(int64_t &state) {
		state = 0;
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void Operation(STATE &state, const INPUT_TYPE &input, AggregateUnaryInput &) {
		state += input;
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void ConstantOperation(STATE &state, const INPUT_TYPE &input, AggregateUnaryInput &, idx_t count) {
		state += input * count;
	}

	template <class STATE, class OP>
	static void Combine(const STATE &source, STATE &target, AggregateInputData &) {
		target += source;
	}

	template <class RESULT_TYPE, class STATE>
	static void Finalize(STATE &state, RESULT_TYPE &result, AggregateFinalizeData &) {
		result = hugeint_t(state + 99);
	}
};

TEST_CASE("Logical plan SQL export reconstructs list PIVOT barriers",
          "[sql_export][logical_plan_sql_export][pivot_sql_export]") {
	DuckDB db(nullptr);
	Connection connection(db);
	connection.BeginTransaction();
	REQUIRE_NO_FAIL(
	    connection.Query("SET pivot_filter_threshold=0; SET threads=1; SET max_streaming_buffer_size='1b'"));

	auto optimized_sum = OptimizeLogicalPlanExportQuery(
	    connection, "FROM (VALUES ('a',1),('b',2)) t(k,v) PIVOT (sum(v) FOR k IN ('a','b','z'))");
	auto &sum = PivotAggregate(*optimized_sum);
	REQUIRE(sum.Function().GetDefinition());
	REQUIRE(sum.Function().GetDefinition()->GetName() == "sum");
	REQUIRE(sum.Function().GetName() == "sum");
	auto &catalog = Catalog::GetSystemCatalog(*connection.context);
	auto &sum_no_overflow_entry = catalog.GetEntry<AggregateFunctionCatalogEntry>(
	    *connection.context,
	    QualifiedName(catalog.GetName(), Identifier::DefaultSchema(), Identifier("sum_no_overflow")));
	auto sum_no_overflow = sum_no_overflow_entry.functions.GetFunctionByArguments(*connection.context,
	                                                                              sum.Function().GetLogicalArguments());
	sum.FunctionMutable().ReplaceImplementation(*sum_no_overflow);
	REQUIRE(sum.Function().GetName() == "sum_no_overflow");
	REQUIRE(LogicalPlanSQLExporter::Export(*connection.context, *optimized_sum).IsSuccess());
	RequirePivotStreamingEffects(connection);
	connection.Rollback();
}

TEST_CASE("Logical plan SQL export validates noncanonical PIVOT defaults and list sources",
          "[sql_export][logical_plan_sql_export][pivot_sql_export]") {
	auto require_default = [](Connection &connection, unique_ptr<LogicalOperator> plan, const string &sql) {
		auto exported = LogicalPlanSQLExporter::Export(*connection.context, *plan);
		INFO((exported.HasError() ? exported.GetIssues()[0].message : string()));
		REQUIRE(exported.IsSuccess());
		auto original = connection.Query(sql);
		REQUIRE_NO_FAIL(*original);
		auto generated = connection.Query(exported.GetValue().query->ToString());
		REQUIRE_NO_FAIL(*generated);
		REQUIRE(generated->GetTypes() == original->GetTypes());
		REQUIRE(SQLExportRows(*generated, false) == SQLExportRows(*original, false));
		auto statement = make_uniq<SelectStatement>();
		statement->node = std::move(exported.GetValue().query);
		auto ast = connection.Query(std::move(statement));
		REQUIRE_NO_FAIL(*ast);
		REQUIRE(ast->GetTypes() == original->GetTypes());
		REQUIRE(SQLExportRows(*ast, false) == SQLExportRows(*original, false));
	};

	SECTION("replaced aggregate empty result") {
		DuckDB db(nullptr);
		Connection connection(db);
		ExtensionLoader loader(*db.instance, "sql_export_pivot_default");
		auto function = AggregateFunction::UnaryAggregate<int64_t, string_t, int64_t, PivotNonstandardEmptySum>(
		    LogicalType::VARCHAR, LogicalType::BIGINT);
		function.SetName(Identifier("sum"));
		CreateAggregateFunctionInfo info(std::move(function));
		info.on_conflict = OnCreateConflict::REPLACE_ON_CONFLICT;
		loader.RegisterFunction(std::move(info));
		REQUIRE_NO_FAIL(connection.Query("SET pivot_filter_threshold=0"));
		connection.BeginTransaction();
		const string sql = "FROM (VALUES ('a','x')) t(k,v) PIVOT(sum(v) FOR k IN ('a','z'))";
		for (bool binary : {false, true}) {
			CAPTURE(binary);
			auto plan = OptimizeLogicalPlanExportQuery(connection, sql);
			if (binary) {
				plan = plan->Copy(*connection.context);
				plan->ResolveOperatorTypes();
			}
			require_default(connection, std::move(plan), sql);
		}
		connection.Rollback();
	}

	SECTION("modified registered aggregate empty result") {
		DuckDB db(nullptr);
		Connection connection(db);
		REQUIRE_NO_FAIL(connection.Query("SET pivot_filter_threshold=0"));
		connection.BeginTransaction();
		auto &entry = Catalog::GetEntry<AggregateFunctionCatalogEntry>(*connection.context,
		                                                               QualifiedName("system", "main", "sum"));
		idx_t modified_overloads = 0;
		entry.functions.ApplyToFunctions([&](AggregateFunction &function) {
			auto &signature = function.GetSignature();
			if (signature.GetParameterCount() != 1 || signature.GetParameter(0).GetType() != LogicalType::INTEGER) {
				return;
			}
			auto qualified_name = function.GetQualifiedName();
			function = AggregateFunction::UnaryAggregate<int64_t, int32_t, hugeint_t, PivotNonstandardEmptyIntegerSum>(
			    LogicalType::INTEGER, LogicalType::HUGEINT);
			function.SetQualifiedName(qualified_name);
			modified_overloads++;
		});
		REQUIRE(modified_overloads == 1);
		struct PivotDefaultCase {
			string sql;
			vector<Value> expected;
		};
		for (const auto &test :
		     {PivotDefaultCase {"FROM (VALUES ('a',1)) t(k,v) PIVOT(sum(v) FOR k IN ('a','z'))",
		                        {Value::HUGEINT(hugeint_t(100)), Value::HUGEINT(hugeint_t(99))}},
		      PivotDefaultCase {
		          "FROM (VALUES ('a_b','c',1)) t(k1,k2,v) "
		          "PIVOT(sum(v) FOR (k1,k2) IN (('a_b','c'),('a','b_c'),('z','z')))",
		          {Value::HUGEINT(hugeint_t(100)), Value::HUGEINT(hugeint_t(99)), Value::HUGEINT(hugeint_t(99))}},
		      PivotDefaultCase {"FROM (SELECT 'a' k, 1 v WHERE false) t PIVOT(sum(v) FOR k IN ('a','z'))",
		                        {Value::HUGEINT(hugeint_t(99)), Value::HUGEINT(hugeint_t(99))}},
		      PivotDefaultCase {"FROM (VALUES ('g','a',1)) t(g,k,v) PIVOT(sum(v) FOR k IN ('a','z'))",
		                        {Value("g"), Value::HUGEINT(hugeint_t(100)), Value::HUGEINT(hugeint_t(99))}}}) {
			CAPTURE(test.sql);
			auto direct = connection.Query(test.sql);
			REQUIRE_NO_FAIL(*direct);
			REQUIRE(direct->RowCount() == 1);
			REQUIRE(direct->ColumnCount() == test.expected.size());
			auto direct_rows = direct->Collection().GetRows();
			for (idx_t column_idx = 0; column_idx < test.expected.size(); column_idx++) {
				REQUIRE(Value::NotDistinctFrom(direct_rows.GetValue(column_idx, 0), test.expected[column_idx]));
			}
			for (bool binary : {false, true}) {
				CAPTURE(binary);
				auto plan = OptimizeLogicalPlanExportQuery(connection, test.sql);
				if (binary) {
					plan = plan->Copy(*connection.context);
					plan->ResolveOperatorTypes();
				}
				require_default(connection, std::move(plan), test.sql);
			}
		}
		connection.Rollback();
	}

	SECTION("volatile and fallible empty defaults cannot move into execution") {
		for (bool fallible : {false, true}) {
			CAPTURE(fallible);
			DuckDB db(nullptr);
			Connection connection(db);
			ExtensionLoader loader(*db.instance, "sql_export_pivot_volatile_default");
			auto function = AggregateFunction::UnaryAggregate<int64_t, string_t, int64_t, PivotNonstandardEmptySum>(
			    LogicalType::VARCHAR, LogicalType::BIGINT);
			function.SetName(Identifier("volatile_default"));
			if (fallible) {
				function.SetFallible();
			} else {
				function.SetVolatile();
			}
			loader.RegisterFunction(std::move(function));
			REQUIRE_NO_FAIL(connection.Query("SET pivot_filter_threshold=0"));
			connection.BeginTransaction();
			const string sql = "FROM (VALUES ('a','x')) t(k,v) PIVOT(volatile_default(v) FOR k IN ('a','z'))";
			for (bool binary : {false, true}) {
				auto plan = OptimizeLogicalPlanExportQuery(connection, sql);
				if (binary) {
					plan = plan->Copy(*connection.context);
					plan->ResolveOperatorTypes();
				}
				RequirePivotSQLExportFailure(connection, std::move(plan), "before query execution");
			}
			connection.Rollback();
		}
	}

	SECTION("supplied aligned lists") {
		DuckDB db(nullptr);
		Connection connection(db);
		REQUIRE_NO_FAIL(connection.Query("SET pivot_filter_threshold=0"));
		connection.BeginTransaction();
		const string pivot_sql = "FROM (VALUES ('g','a',1)) t(g,k,v) PIVOT(sum(v) FOR k IN ('a','b'))";
		for (const auto &child_sql : {
		         "SELECT 'g' g,[10::HUGEINT] v,['a'] k UNION ALL SELECT 'g',[20::HUGEINT],['b']",
		         "SELECT 'g' g,[]::HUGEINT[] v,[]::VARCHAR[] k",
		         "SELECT 'g' g,[10::HUGEINT,NULL] v,['a','a'] k",
		         "SELECT 'g1' g,[10::HUGEINT] v,['a'] k UNION ALL SELECT 'g2',[20::HUGEINT],['b']",
		         "SELECT 'g' g,[10::HUGEINT,20::HUGEINT] v,['a','a'] k",
		         "SELECT i::VARCHAR g,[i::HUGEINT,NULL,i::HUGEINT+10] v,['a','x','a'] k FROM range(3) t(i)",
		     }) {
			for (bool binary : {false, true}) {
				CAPTURE(child_sql, binary);
				auto original = OptimizeLogicalPlanExportQuery(connection, pivot_sql);
				auto pivot = FindLogicalPlanExportOperator(*original, LogicalOperatorType::LOGICAL_PIVOT);
				REQUIRE(pivot);
				auto plan = pivot->Copy(*connection.context);
				plan->Cast<LogicalPivot>().pivot_index = TableIndex(100000);
				plan->children[0] = OptimizeLogicalPlanExportQuery(connection, child_sql);
				plan->ResolveOperatorTypes();
				if (binary) {
					plan = plan->Copy(*connection.context);
					plan->ResolveOperatorTypes();
				}
				auto native_plan = plan->Copy(*connection.context);
				native_plan->ResolveOperatorTypes();
				auto exported = LogicalPlanSQLExporter::Export(*connection.context, *plan);
				INFO((exported.HasError() ? exported.GetIssues()[0].message : string()));
				REQUIRE(exported.IsSuccess());
				auto text = exported.GetValue().query->ToString();
				auto native = connection.Query(make_uniq<LogicalPlanStatement>(std::move(native_plan)));
				REQUIRE_NO_FAIL(*native);
				auto generated = connection.Query(text);
				REQUIRE_NO_FAIL(*generated);
				REQUIRE(generated->GetTypes() == native->GetTypes());
				REQUIRE(SQLExportRows(*generated, false) == SQLExportRows(*native, false));
				auto statement = make_uniq<SelectStatement>();
				statement->node = std::move(exported.GetValue().query);
				auto ast = connection.Query(std::move(statement));
				REQUIRE_NO_FAIL(*ast);
				REQUIRE(ast->GetTypes() == native->GetTypes());
				REQUIRE(SQLExportRows(*ast, false) == SQLExportRows(*native, false));
			}
		}
		connection.Rollback();
	}
}

} // namespace logical_plan_sql_export_test
