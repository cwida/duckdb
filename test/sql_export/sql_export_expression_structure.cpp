#include "duckdb/main/config.hpp"
#include "duckdb/function/cast/cast_function_set.hpp"
#include "catch.hpp"
#include "duckdb/planner/filter/table_filter_functions.hpp"
#include "test_helpers.hpp"
#include "duckdb/planner/sql_export_helpers.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/cast/default_casts.hpp"
#include "duckdb/function/cast/vector_cast_helpers.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/planner/bound_expression_sql_exporter.hpp"
#include "duckdb/planner/expression/bound_between_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_default_expression.hpp"
#include "duckdb/planner/expression/bound_expanded_expression.hpp"
#include "duckdb/planner/expression/bound_lambda_expression.hpp"
#include "duckdb/planner/expression/bound_lambdaref_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_parameter_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression/bound_subquery_expression.hpp"
#include "duckdb/planner/expression/bound_unnest_expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/expression/legacy_bound_between_expression.hpp"
#include "duckdb/planner/expression/legacy_bound_cast_expression.hpp"
#include "duckdb/planner/expression/legacy_bound_comparison_expression.hpp"
#include <cmath>
#include <cstring>
#include "bound_expression_sql_export_test_helpers.hpp"

using namespace duckdb;

namespace bound_expression_sql_export_test {

template <int32_t OFFSET>
struct OffsetCastOperation {
	template <class SOURCE_TYPE, class TARGET_TYPE>
	static TARGET_TYPE Operation(SOURCE_TYPE input) {
		return static_cast<TARGET_TYPE>(input + OFFSET);
	}
};

static bool IntegerToBigintPlusOne(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	return VectorCastHelpers::TemplatedCastLoop<int32_t, int64_t, OffsetCastOperation<1>>(source, result, count,
	                                                                                      parameters);
}

static bool StringToIntegerEight(Vector &, Vector &result, idx_t count, CastParameters &) {
	result.Reference(Value::INTEGER(8), count_t(count));
	return true;
}

static idx_t cast_bind_calls = 0;

static BoundCastInfo CountCastBinding(BindCastInput &, const LogicalType &, const LogicalType &) {
	cast_bind_calls++;
	return BoundCastInfo(IntegerToBigintPlusOne);
}

TEST_CASE("Bound expression SQL export resolves columns only by binding", "[sql_export][bound_expression_sql_export]") {
	auto left_binding = ColumnBinding(TableIndex(10), ProjectionIndex(0));
	auto right_binding = ColumnBinding(TableIndex(11), ProjectionIndex(0));
	auto left = make_uniq<BoundColumnRefExpression>(Identifier("duplicate_alias"), LogicalType::INTEGER, left_binding);
	auto right =
	    make_uniq<BoundColumnRefExpression>(Identifier("duplicate_alias"), LogicalType::INTEGER, right_binding);
	auto comparison =
	    BoundComparisonExpression::Create(ExpressionType::COMPARE_LESSTHAN, std::move(left), std::move(right));
	BoundExpressionSQLExportContext context;
	context.resolve_binding = [=](const ColumnBinding &binding) -> optional<ResolvedSQLColumnReference> {
		if (binding == left_binding) {
			return ResolvedSQLColumnReference {{Identifier("src"), Identifier("select")}, LogicalType::INTEGER};
		}
		if (binding == right_binding) {
			return ResolvedSQLColumnReference {{Identifier("src"), Identifier("Mixed Case")}, LogicalType::INTEGER};
		}
		return {};
	};
	auto result = BoundExpressionSQLExporter::Export(*comparison, context);
	REQUIRE(result.IsSuccess());
	REQUIRE(result.GetValue()->GetExpressionClass() == ExpressionClass::COMPARISON);
	auto &parsed = result.GetValue()->Cast<ComparisonExpression>();
	REQUIRE(parsed.Left().Cast<ColumnRefExpression>().ColumnNames() ==
	        vector<Identifier> {Identifier("src"), Identifier("select")});
	REQUIRE(parsed.Right().Cast<ColumnRefExpression>().ColumnNames() ==
	        vector<Identifier> {Identifier("src"), Identifier("Mixed Case")});

	DuckDB db;
	Connection connection(db);
	RequireRoundTrip(connection, *comparison, context, " FROM (SELECT 11 AS \"select\", 22 AS \"Mixed Case\") AS src",
	                 "src.\"select\" < src.\"Mixed Case\"");

	BoundColumnRefExpression missing(Identifier("usable_alias"), LogicalType::INTEGER, left_binding);
	BoundExpressionSQLExportContext missing_context;
	auto missing_result = BoundExpressionSQLExporter::Export(missing, missing_context);
	LogicalPlanVerificationPath standalone_path;
	standalone_path.root = LogicalPlanVerificationPathRoot::STANDALONE_EXPRESSION;
	RequireIssue(missing_result, LogicalPlanVerificationIssueCode::INVALID_BINDING, standalone_path);
	REQUIRE(missing_result.GetIssues()[0].facts.size() == 2);

	auto wrong_type = ResolveBinding(left_binding, {Identifier("src"), Identifier("select")}, LogicalType::BIGINT);
	auto mismatch = BoundExpressionSQLExporter::Export(missing, wrong_type);
	RequireIssue(mismatch, LogicalPlanVerificationIssueCode::TYPE_MISMATCH, standalone_path);
	auto &type_mismatch = *mismatch.GetIssues()[0].construct->type_mismatch;
	REQUIRE(type_mismatch.expected_type == LogicalType::BIGINT);
	REQUIRE(type_mismatch.actual_type == LogicalType::INTEGER);

	BoundColumnRefExpression correlated(LogicalType::INTEGER, left_binding, 1);
	auto correlated_result = BoundExpressionSQLExporter::Export(correlated, context);
	RequireIssue(correlated_result, LogicalPlanVerificationIssueCode::UNSUPPORTED_EXPORT_FEATURE, standalone_path);
	REQUIRE(*correlated_result.GetIssues()[0].construct->identifier == "correlated_column_reference");
}

TEST_CASE("Bound expression SQL export composes deterministic expression paths",
          "[sql_export][bound_expression_sql_export]") {
	auto binding = ColumnBinding(TableIndex(20), ProjectionIndex(3));
	auto expression = BoundComparisonExpression::Create(
	    ExpressionType::COMPARE_EQUAL, make_uniq<BoundColumnRefExpression>(LogicalType::INTEGER, binding),
	    make_uniq<BoundColumnRefExpression>(LogicalType::INTEGER, binding));
	BoundExpressionSQLExportContext context;
	LogicalPlanVerificationPath root;
	root.root = LogicalPlanVerificationPathRoot::LOGICAL_PLAN;
	root.components.push_back(
	    LogicalPlanVerificationPathComponent {LogicalPlanVerificationPathComponentType::OPERATOR_CHILD, 2});
	root.components.push_back(
	    LogicalPlanVerificationPathComponent {LogicalPlanVerificationPathComponentType::OPERATOR_EXPRESSION, 4});
	auto result = BoundExpressionSQLExporter::ExportAtPath(*expression, context, root);
	REQUIRE(result.IsValid());
	REQUIRE(result.HasError());
	REQUIRE(result.GetIssues().size() == 2);
	for (idx_t child_index = 0; child_index < 2; child_index++) {
		auto expected = root;
		expected.components.push_back(LogicalPlanVerificationPathComponent {
		    LogicalPlanVerificationPathComponentType::EXPRESSION_CHILD, child_index});
		REQUIRE(result.GetIssues()[child_index].path == optional<LogicalPlanVerificationPath>(expected));
	}
}

TEST_CASE("Bound expression SQL export reconstructs registered casts", "[sql_export][bound_expression_sql_export]") {
	BoundExpressionSQLExportContext context;
	DuckDB registered_db;
	Connection registered_connection(registered_db);
	ExtensionLoader loader(*registered_db.instance, "synthetic_cast_extension");
	loader.RegisterCastFunction(LogicalType::INTEGER, LogicalType::BIGINT, BoundCastInfo(IntegerToBigintPlusOne), 0);
	auto registered = BoundCastExpression::AddCastToType(*registered_connection.context, Constant(Value::INTEGER(7)),
	                                                     LogicalType::BIGINT);
	REQUIRE(ExpressionExecutor::EvaluateScalar(*registered_connection.context, *registered) == Value::BIGINT(8));
	auto exported = BoundExpressionSQLExporter::Export(*registered, context);
	REQUIRE(exported.IsSuccess());
	auto rebound = registered_connection.Query("SELECT " + exported.GetValue()->ToString());
	REQUIRE_FALSE(rebound->HasError());
	REQUIRE(rebound->GetTypes() == vector<LogicalType> {LogicalType::BIGINT});
	REQUIRE(rebound->Collection().GetValue(0, 0) == Value::BIGINT(8));

	context.client_context = registered_connection.context.get();
	auto default_direct = BoundCastExpression::AddDefaultCastToType(Constant(Value::INTEGER(7)), LogicalType::BIGINT);
	auto default_direct_result = BoundExpressionSQLExporter::Export(*default_direct, context);
	REQUIRE(default_direct_result.HasError());
	REQUIRE(*default_direct_result.GetIssues()[0].construct->identifier == "default_cast_binding");

	DuckDB unrelated_db;
	Connection unrelated_connection(unrelated_db);
	ExtensionLoader unrelated_loader(*unrelated_db.instance, "synthetic_unrelated_cast_extension");
	unrelated_loader.RegisterCastFunction(LogicalType::INTEGER, LogicalType::BIGINT,
	                                      BoundCastInfo(IntegerToBigintPlusOne), 0);
	BoundExpressionSQLExportContext unrelated_context;
	unrelated_context.client_context = unrelated_connection.context.get();
	auto default_unrelated = BoundCastExpression::AddDefaultCastToType(Constant(Value("7")), LogicalType::INTEGER);
	auto default_unrelated_result = BoundExpressionSQLExporter::Export(*default_unrelated, unrelated_context);
	REQUIRE(default_unrelated_result.IsSuccess());
	auto unrelated_rebound = unrelated_connection.Query("SELECT " + default_unrelated_result.GetValue()->ToString());
	REQUIRE_NO_FAIL(*unrelated_rebound);
	REQUIRE(unrelated_rebound->Collection().GetValue(0, 0) == Value::INTEGER(7));

	for (auto nested_types : vector<pair<LogicalType, LogicalType>> {
	         {LogicalType::LIST(LogicalType::INTEGER), LogicalType::LIST(LogicalType::BIGINT)},
	         {LogicalType::ARRAY(LogicalType::INTEGER, 2), LogicalType::ARRAY(LogicalType::BIGINT, 2)},
	         {LogicalType::STRUCT({{"v", LogicalType::INTEGER}}), LogicalType::STRUCT({{"v", LogicalType::BIGINT}})},
	         {LogicalType::MAP(LogicalType::VARCHAR, LogicalType::INTEGER),
	          LogicalType::MAP(LogicalType::VARCHAR, LogicalType::BIGINT)},
	         {LogicalType::UNION({{"v", LogicalType::INTEGER}}), LogicalType::UNION({{"v", LogicalType::BIGINT}})}}) {
		INFO(nested_types.first.ToString());
		auto nested =
		    BoundCastExpression::AddDefaultCastToType(Constant(Value(nested_types.first)), nested_types.second);
		auto rejected = BoundExpressionSQLExporter::Export(*nested, context);
		REQUIRE(rejected.HasError());
		REQUIRE(*rejected.GetIssues()[0].construct->identifier == "default_cast_binding");
	}

	auto list = Value::LIST(LogicalType::VARCHAR, {Value("7"), Value(LogicalType::VARCHAR)});
	auto unrelated_list =
	    BoundCastExpression::AddDefaultCastToType(Constant(list), LogicalType::LIST(LogicalType::INTEGER));
	auto list_export = BoundExpressionSQLExporter::Export(*unrelated_list, context);
	REQUIRE(list_export.IsSuccess());
	auto list_result = registered_connection.Query("SELECT " + list_export.GetValue()->ToString());
	REQUIRE_NO_FAIL(*list_result);
	REQUIRE(list_result->Collection().GetValue(0, 0) ==
	        ExpressionExecutor::EvaluateScalar(*registered_connection.context, *unrelated_list));

	auto union_type = LogicalType::UNION({{"small", LogicalType::INTEGER}, {"big", LogicalType::BIGINT}});
	auto union_cast = BoundCastExpression::AddDefaultCastToType(Constant(Value::INTEGER(7)), union_type);
	REQUIRE(ExpressionExecutor::EvaluateScalar(*registered_connection.context, *union_cast).type() == union_type);
	auto union_export = BoundExpressionSQLExporter::Export(*union_cast, context);
	REQUIRE(union_export.HasError());
	REQUIRE(*union_export.GetIssues()[0].construct->identifier == "default_cast_binding");
	auto ambiguous_union = registered_connection.Query("SELECT CAST(7 AS UNION(small INTEGER, big BIGINT))");
	REQUIRE(ambiguous_union->HasError());
}

TEST_CASE("Default cast checks do not invoke custom bind callbacks", "[sql_export][bound_expression_sql_export]") {
	DuckDB db;
	Connection connection(db);
	ExtensionLoader loader(*db.instance, "cast_binding_probe");
	loader.RegisterCastFunction(LogicalType::INTEGER, LogicalType::BIGINT, CountCastBinding, 0);
	const auto &casts = CastFunctionSet::Get(*connection.context);
	cast_bind_calls = 0;
	REQUIRE(casts.CanOverrideDefaultCast(LogicalType::INTEGER, LogicalType::BIGINT));
	REQUIRE(
	    casts.CanOverrideDefaultCast(LogicalType::LIST(LogicalType::INTEGER), LogicalType::LIST(LogicalType::BIGINT)));
	REQUIRE_FALSE(casts.CanOverrideDefaultCast(LogicalType::INTEGER, LogicalType::INTEGER));
	REQUIRE_FALSE(casts.CanOverrideDefaultCast(LogicalType::VARCHAR, LogicalType::INTEGER));
	REQUIRE(cast_bind_calls == 0);
	auto bound =
	    BoundCastExpression::AddCastToType(*connection.context, Constant(Value::INTEGER(7)), LogicalType::BIGINT);
	REQUIRE(cast_bind_calls == 1);
	REQUIRE(ExpressionExecutor::EvaluateScalar(*connection.context, *bound) == Value::BIGINT(8));

	loader.RegisterCastFunction(LogicalType::VARCHAR, LogicalType::INTEGER, CountCastBinding, 0);
	auto enum_result = connection.Query("SELECT '7'::ENUM('7')");
	REQUIRE_NO_FAIL(*enum_result);
	cast_bind_calls = 0;
	REQUIRE(casts.CanOverrideDefaultCast(enum_result->GetTypes()[0], LogicalType::INTEGER));
	REQUIRE(cast_bind_calls == 0);

	loader.RegisterCastFunction(LogicalType::LIST(LogicalType::ANY), LogicalType::VARCHAR, CountCastBinding, 0);
	REQUIRE(casts.CanOverrideDefaultCast(LogicalType::LIST(LogicalType::DOUBLE), LogicalType::VARCHAR));
	REQUIRE(cast_bind_calls == 0);
}

TEST_CASE("Default VARIANT casts retain runtime binding protection", "[sql_export][bound_expression_sql_export]") {
	DuckDB db;
	Connection connection(db);
	ExtensionLoader loader(*db.instance, "variant_cast_probe");
	loader.RegisterCastFunction(LogicalType::VARCHAR, LogicalType::INTEGER, BoundCastInfo(StringToIntegerEight), 0);
	auto input = connection.Query("SELECT '7'::VARIANT");
	REQUIRE_NO_FAIL(*input);
	auto value = input->Collection().GetValue(0, 0);
	auto original = BoundCastExpression::AddDefaultCastToType(Constant(value), LogicalType::INTEGER);
	auto rebound = BoundCastExpression::AddCastToType(*connection.context, Constant(value), LogicalType::INTEGER);
	REQUIRE(ExpressionExecutor::EvaluateScalar(*connection.context, *original) == Value::INTEGER(7));
	REQUIRE(ExpressionExecutor::EvaluateScalar(*connection.context, *rebound) == Value::INTEGER(8));
	BoundExpressionSQLExportContext context;
	context.client_context = connection.context.get();
	auto exported = BoundExpressionSQLExporter::Export(*original, context);
	REQUIRE(exported.HasError());
	REQUIRE(*exported.GetIssues()[0].construct->identifier == "default_cast_binding");
	REQUIRE(CastFunctionSet::Get(*connection.context)
	            .CanOverrideDefaultCast(LogicalType::LIST(LogicalType::VARIANT()),
	                                    LogicalType::LIST(LogicalType::INTEGER)));

	DBConfig default_config;
	default_config.options.load_extensions = false;
	DuckDB default_db(nullptr, &default_config);
	Connection default_connection(default_db);
	context.client_context = default_connection.context.get();
	auto safe_export = BoundExpressionSQLExporter::Export(*original, context);
	INFO((safe_export.HasError() ? safe_export.GetIssues()[0].message : string()));
	REQUIRE(safe_export.IsSuccess());
	auto result = default_connection.Query("SELECT " + safe_export.GetValue()->ToString());
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Collection().GetValue(0, 0) == Value::INTEGER(7));
}

TEST_CASE("Bound expression SQL export rejects TRY around volatile children",
          "[sql_export][bound_expression_sql_export]") {
	DuckDB db;
	Connection connection(db);
	connection.BeginTransaction();
	BoundExpressionSQLExportContext context;
	LogicalPlanVerificationPath path;
	path.root = LogicalPlanVerificationPathRoot::STANDALONE_EXPRESSION;

	auto random_plan = BindExportQuery(connection, "SELECT random()");
	auto random = FindExpression(*random_plan, [](const Expression &expression) {
		return expression.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION && expression.IsVolatile() &&
		       expression.Cast<BoundFunctionExpression>().Function().GetName() == "random";
	});
	REQUIRE(random);
	BoundOperatorExpression volatile_try(ExpressionType::OPERATOR_TRY, LogicalType::DOUBLE);
	volatile_try.GetChildrenMutable().push_back(random->Copy());
	auto volatile_result = BoundExpressionSQLExporter::Export(volatile_try, context);
	RequireIssue(volatile_result, LogicalPlanVerificationIssueCode::UNSUPPORTED_EXPORT_FEATURE, path);
	REQUIRE(*volatile_result.GetIssues()[0].construct->identifier == "try_volatile_child");
	connection.Rollback();
}

TEST_CASE("Bound expression SQL export drops plan-local table filter hints",
          "[sql_export][bound_expression_sql_export]") {
	auto filter = CreateOptionalFilterExpression(
	    BoundComparisonExpression::Create(ExpressionType::COMPARE_LESSTHAN,
	                                      make_uniq<BoundReferenceExpression>(LogicalType::INTEGER, 0),
	                                      make_uniq<BoundConstantExpression>(Value::INTEGER(10))),
	    LogicalType::INTEGER);
	BoundExpressionSQLExportContext standalone;
	REQUIRE(BoundExpressionSQLExporter::Export(*filter, standalone).HasError());
	BoundExpressionSQLExportContext plan_context;
	plan_context.discard_optimizer_metadata = true;
	auto exported = BoundExpressionSQLExporter::Export(*filter, plan_context);
	REQUIRE(exported.IsSuccess());
	REQUIRE(exported.GetValue()->GetExpressionClass() == ExpressionClass::CONSTANT);
	REQUIRE(exported.GetValue()->Equals(*ConstantExpression::Boolean(true)));
}

TEST_CASE("Bound expression SQL export rejects repeated volatile exclusive BETWEEN input",
          "[sql_export][bound_expression_sql_export]") {
	DuckDB db;
	Connection connection(db);
	connection.BeginTransaction();

	auto random_plan = OptimizeExportQuery(connection, "SELECT random()");
	auto random_expression = FindExpression(*random_plan, [](const Expression &expression) {
		return expression.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION &&
		       expression.Cast<BoundFunctionExpression>().Function().GetName() == "random";
	});
	REQUIRE(random_expression);
	auto volatile_between = BoundBetweenExpression::Create(random_expression->Copy(), Constant(Value::DOUBLE(0)),
	                                                       Constant(Value::DOUBLE(1)), false, false);
	BoundExpressionSQLExportContext empty_context;
	auto volatile_result = BoundExpressionSQLExporter::Export(*volatile_between, empty_context);
	REQUIRE(volatile_result.HasError());
	REQUIRE(volatile_result.GetIssues()[0].code == LogicalPlanVerificationIssueCode::UNSUPPORTED_EXPORT_FEATURE);
	REQUIRE(*volatile_result.GetIssues()[0].construct->identifier == "exclusive_between_input_evaluation");
	connection.Rollback();
}

TEST_CASE("Bound expression SQL export rejects deferred expression kinds",
          "[sql_export][bound_expression_sql_export]") {
	BoundExpressionSQLExportContext context;
	LogicalPlanVerificationPath path;
	path.root = LogicalPlanVerificationPathRoot::STANDALONE_EXPRESSION;
	vector<unique_ptr<Expression>> deferred;
	deferred.push_back(make_uniq<BoundDefaultExpression>(LogicalType::INTEGER));
	deferred.push_back(make_uniq<BoundParameterExpression>(Identifier("1")));
	deferred.push_back(make_uniq<BoundReferenceExpression>(LogicalType::INTEGER, storage_t(0)));
	deferred.push_back(make_uniq<BoundSubqueryExpression>(LogicalType::INTEGER));
	deferred.push_back(make_uniq<BoundWindowExpression>(LogicalType::INTEGER, nullptr, nullptr, nullptr));
	deferred.push_back(make_uniq<BoundUnnestExpression>(LogicalType::INTEGER));
	deferred.push_back(
	    make_uniq<BoundLambdaExpression>(ExpressionType::LAMBDA, LogicalType::INTEGER, Constant(Value::INTEGER(1)), 1));
	deferred.push_back(
	    make_uniq<BoundLambdaRefExpression>(LogicalType::INTEGER, ColumnBinding(TableIndex(1), ProjectionIndex(0)), 0));
	deferred.push_back(make_uniq<LegacyBoundCastExpression>(Constant(Value::INTEGER(1)), LogicalType::BIGINT, false));
	deferred.push_back(make_uniq<LegacyBoundComparisonExpression>(
	    ExpressionType::COMPARE_EQUAL, Constant(Value::INTEGER(1)), Constant(Value::INTEGER(1))));
	deferred.push_back(make_uniq<LegacyBoundBetweenExpression>(Constant(Value::INTEGER(1)), Constant(Value::INTEGER(0)),
	                                                           Constant(Value::INTEGER(2)), true, true));
	for (auto &expression : deferred) {
		RequireIssue(BoundExpressionSQLExporter::Export(*expression, context),
		             LogicalPlanVerificationIssueCode::UNSUPPORTED_EXPRESSION, path);
	}

	vector<unique_ptr<Expression>> expanded_children;
	expanded_children.push_back(Constant(Value::INTEGER(1)));
	BoundExpandedExpression expanded(std::move(expanded_children));
	RequireIssue(BoundExpressionSQLExporter::Export(expanded, context),
	             LogicalPlanVerificationIssueCode::INTERNAL_INVARIANT, path);
}

TEST_CASE("Bound expression SQL export owns outputs and propagates resolver exceptions",
          "[sql_export][bound_expression_sql_export]") {
	auto success = []() {
		BoundConstantExpression expression(Value::SMALLINT(9));
		BoundExpressionSQLExportContext context;
		return BoundExpressionSQLExporter::Export(expression, context);
	}();
	REQUIRE(success.IsSuccess());
	REQUIRE(success.GetValue()->Cast<CastExpression>().GetTargetType()->Equals(
	    *TypeExpression::FromLogicalType(LogicalType::SMALLINT)));

	auto failure = []() {
		BoundColumnRefExpression expression(LogicalType::INTEGER, ColumnBinding(TableIndex(88), ProjectionIndex(7)));
		BoundExpressionSQLExportContext context;
		return BoundExpressionSQLExporter::Export(expression, context);
	}();
	REQUIRE(failure.HasError());
	REQUIRE(failure.GetIssues()[0].facts.size() == 2);

	BoundColumnRefExpression expression(LogicalType::INTEGER, ColumnBinding(TableIndex(1), ProjectionIndex(0)));
	BoundExpressionSQLExportContext throwing_context;
	throwing_context.resolve_binding = [](const ColumnBinding &) -> optional<ResolvedSQLColumnReference> {
		throw InvalidInputException("synthetic resolver failure");
	};
	REQUIRE_THROWS_AS(BoundExpressionSQLExporter::Export(expression, throwing_context), InvalidInputException);
}

} // namespace bound_expression_sql_export_test
