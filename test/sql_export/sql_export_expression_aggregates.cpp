#include "catch.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "test_helpers.hpp"
#include "duckdb/planner/sql_export_helpers.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/aggregate_function_catalog_entry.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/planner/bound_expression_sql_exporter.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include <cmath>
#include <cstring>
#include "bound_expression_sql_export_test_helpers.hpp"

using namespace duckdb;

namespace bound_expression_sql_export_test {

TEST_CASE("SQL export retains logical SUM identity across specialization and serialization",
          "[sql_export][bound_expression_sql_export][optimizer]") {
	for (bool decimal : {false, true}) {
		CAPTURE(decimal);
		DuckDB db;
		Connection connection(db);
		auto type = decimal ? LogicalType::DECIMAL(9, 2) : LogicalType::INTEGER;
		auto result_type = decimal ? LogicalType::DECIMAL(38, 2) : LogicalType::HUGEINT;
		REQUIRE_NO_FAIL(connection.Query("CREATE TABLE aggregate_values(i " + type.ToString() + ")"));
		REQUIRE_NO_FAIL(connection.Query(decimal ? "INSERT INTO aggregate_values VALUES (1.25), (2.50)"
		                                         : "INSERT INTO aggregate_values VALUES (1), (2), (2), (NULL)"));
		REQUIRE_NO_FAIL(connection.Query("SET disabled_optimizers='compressed_materialization'"));
		connection.BeginTransaction();
		auto plan = OptimizeExportQuery(connection, "SELECT sum(i) FROM aggregate_values");
		auto expression = FindExpression(*plan, [](const Expression &candidate) {
			return candidate.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE;
		});
		REQUIRE(expression);
		auto &aggregate = expression->Cast<BoundAggregateExpression>();
		if (!decimal) {
			auto &entry = Catalog::GetEntry<AggregateFunctionCatalogEntry>(
			    *connection.context, QualifiedName("system", "main", "sum_no_overflow"));
			auto implementation = entry.functions.GetFunctionByArguments(*connection.context, {type});
			REQUIRE(aggregate.Function().GetCallbacks() == implementation->GetCallbacks());
			REQUIRE(aggregate.Function().GetCallbacks() != aggregate.Function().GetDefinition()->GetCallbacks());
		}
		auto &column = aggregate.GetChildren()[0]->Cast<BoundColumnRefExpression>();
		auto context = ResolveBinding(column.Binding(), {Identifier("i")}, column.GetReturnType());
		auto restored = BinaryRoundTrip(*connection.context, aggregate);
		for (auto candidate : vector<reference<const Expression>> {*expression, *restored}) {
			auto &bound = candidate.get().Cast<BoundAggregateExpression>();
			REQUIRE(bound.Function().GetName() == "sum_no_overflow");
			REQUIRE(bound.Function().GetDefinition());
			REQUIRE(bound.Function().GetDefinition()->GetName() == "sum");
			REQUIRE(bound.Function().GetLogicalArguments() == vector<LogicalType> {type});
			REQUIRE(bound.Function().GetLogicalReturnType() == result_type);
			REQUIRE(bound.GetReturnType() == result_type);
			auto exported = BoundExpressionSQLExporter::Export(bound, context);
			REQUIRE(exported.IsSuccess());
			if (decimal) {
				auto &cast = exported.GetValue()->Cast<CastExpression>();
				REQUIRE(cast.GetTargetType()->Equals(*TypeExpression::FromLogicalType(result_type)));
				REQUIRE(cast.Child().Cast<FunctionExpression>().FunctionName() == "sum");
			} else {
				REQUIRE(exported.GetValue()->Cast<FunctionExpression>().FunctionName() == "sum");
			}
		}
		connection.Rollback();
	}
}

TEST_CASE("Aggregate SQL clauses retain logical result annotations",
          "[sql_export][bound_expression_sql_export][aggregate_call_sql_export]") {
	DuckDB db;
	Connection connection(db);
	REQUIRE_NO_FAIL(
	    connection.Query("CREATE TABLE clause_values(v VARCHAR); INSERT INTO clause_values VALUES ('A'),('a')"));
	connection.BeginTransaction();
	LogicalPlanVerificationPath path;
	path.root = LogicalPlanVerificationPathRoot::STANDALONE_EXPRESSION;
	auto plan = BindExportQuery(connection, "SELECT last(v) FROM clause_values");
	auto expression = FindExpression(*plan, [](const Expression &candidate) {
		return candidate.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE;
	});
	REQUIRE(expression);
	for (bool binary : {false, true}) {
		auto copy = binary ? BinaryRoundTrip(*connection.context, *expression) : expression->Copy();
		auto &aggregate = copy->Cast<BoundAggregateExpression>();
		auto &column = aggregate.GetChildren()[0]->Cast<BoundColumnRefExpression>();
		auto context = ResolveBinding(column.Binding(), {Identifier("v")}, column.GetReturnType());
		auto call = BoundExpressionSQLExporter::ExportAggregateCallAtPath(aggregate, context, path);
		REQUIRE(call.IsSuccess());
		REQUIRE(call.GetValue()->GetExpressionClass() == ExpressionClass::FUNCTION);
		REQUIRE(call.GetValue()->GetQualifiedName() == QualifiedName("system", "main", "last"));
		auto direct = connection.Query("SELECT (" + call.GetValue()->ToString() + ")='A' FROM clause_values");
		REQUIRE_NO_FAIL(*direct);
		REQUIRE(direct->Collection().GetValue(0, 0) == Value::BOOLEAN(false));
		aggregate.SetReturnType(LogicalType::VARCHAR_COLLATION("nocase"));
		auto rejected = BoundExpressionSQLExporter::ExportAggregateCallAtPath(aggregate, context, path);
		REQUIRE(rejected.HasError());
		REQUIRE(rejected.GetIssues()[0].construct ==
		        LogicalPlanVerificationConstructIdentity::ExportFeature("aggregate_call_result_type"));
		auto ordinary = BoundExpressionSQLExporter::Export(aggregate, context);
		REQUIRE(ordinary.IsSuccess());
		auto annotated = connection.Query("SELECT (" + ordinary.GetValue()->ToString() + ")='A' FROM clause_values");
		REQUIRE_NO_FAIL(*annotated);
		REQUIRE(annotated->Collection().GetValue(0, 0) == Value::BOOLEAN(true));
	}
	const ColumnBinding binding(TableIndex(1), ProjectionIndex(0));
	for (const auto &type :
	     {LogicalType::VARCHAR_COLLATION("nocase"), LogicalType::LIST(LogicalType::VARCHAR_COLLATION("nocase")),
	      LogicalType::STRUCT({{Identifier("v"), LogicalType::VARCHAR_COLLATION("nocase")}})}) {
		auto &entry = Catalog::GetEntry<AggregateFunctionCatalogEntry>(*connection.context,
		                                                               QualifiedName("system", "main", "last"));
		auto definition = entry.functions.GetFunctionByArguments(*connection.context, {type});
		vector<unique_ptr<Expression>> arguments;
		arguments.push_back(make_uniq<BoundColumnRefExpression>(type, binding));
		FunctionBinder binder(*connection.context);
		auto aggregate = binder.BindAggregateFunction(definition, std::move(arguments));
		auto context = ResolveBinding(binding, {Identifier("v")}, type);
		for (idx_t generation = 0; generation < 3; generation++) {
			auto call = BoundExpressionSQLExporter::ExportAggregateCallAtPath(*aggregate, context, path);
			REQUIRE(call.IsSuccess());
			REQUIRE(call.GetValue()->GetExpressionClass() == ExpressionClass::FUNCTION);
			REQUIRE(aggregate->GetReturnType().EqualsIncludingCollation(type));
			REQUIRE(aggregate->Function().GetLogicalReturnType().EqualsIncludingCollation(type));
			aggregate =
			    unique_ptr_cast<Expression, BoundAggregateExpression>(BinaryRoundTrip(*connection.context, *aggregate));
		}
	}
	connection.Rollback();
}

} // namespace bound_expression_sql_export_test
