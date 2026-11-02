#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/common/smaller_binary.hpp"
#include "duckdb/function/cast/cast_function_set.hpp"
#include "duckdb/function/cast/vector_cast_helpers.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/logical_operator_visitor.hpp"
#include "duckdb/planner/planner.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"

using namespace duckdb;

namespace {

struct CastCounts {
	idx_t checked = 0;
	idx_t specialized = 0;
};

static BoundCastInfo GetDefaultCast(const LogicalType &source, const LogicalType &target) {
	CastFunctionSet function_set;
	GetCastFunctionInput input;
	return function_set.GetCastFunction(source, target, input);
}

static BaseStatistics CreateNumericStats(const LogicalType &type, const Value &min, const Value &max) {
	auto result = NumericStats::CreateEmpty(type);
	NumericStats::SetMin(result, min);
	NumericStats::SetMax(result, max);
	result.Set(StatsInfo::CAN_HAVE_VALID_VALUES);
	return result;
}

static unique_ptr<LogicalOperator> OptimizeQuery(Connection &connection, const string &query) {
	Parser parser(*connection.context);
	parser.ParseQuery(query);
	REQUIRE(parser.statements.size() == 1);
	Planner planner(*connection.context);
	planner.CreatePlan(std::move(parser.statements[0]));
	Optimizer optimizer(*planner.binder, *connection.context);
	return optimizer.Optimize(std::move(planner.plan));
}

static void CountExpressionCasts(const Expression &expr, CastCounts &counts) {
	if (BoundCastExpression::IsCast(expr)) {
		auto &cast = expr.Cast<BoundFunctionExpression>();
		auto checked_cast =
		    GetDefaultCast(BoundCastExpression::SourceType(cast), BoundCastExpression::TargetType(cast));
		if (BoundCastExpression::GetBoundCast(cast).Equals(checked_cast)) {
			counts.checked++;
		} else {
			counts.specialized++;
		}
	}
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) { CountExpressionCasts(child, counts); });
}

static CastCounts CountPlanCasts(const LogicalOperator &op) {
	CastCounts result;
	LogicalOperatorVisitor::EnumerateExpressions(
	    op, [&](const unique_ptr<Expression> *expr) { CountExpressionCasts(**expr, result); });
	for (auto &child : op.children) {
		auto child_counts = CountPlanCasts(*child);
		result.checked += child_counts.checked;
		result.specialized += child_counts.specialized;
	}
	return result;
}

static void RequireSingleCast(const LogicalOperator &op, bool specialized) {
	auto counts = CountPlanCasts(op);
	REQUIRE(counts.checked == (specialized ? 0 : 1));
	REQUIRE(counts.specialized == (specialized ? 1 : 0));
}

static optional_ptr<const BoundFunctionExpression> FindCast(const LogicalOperator &op) {
	optional_ptr<const BoundFunctionExpression> result;
	LogicalOperatorVisitor::EnumerateExpressions(op, [&](const unique_ptr<Expression> *expr) {
		ExpressionIterator::VisitExpression<BoundFunctionExpression>(
		    **expr, [&](const BoundFunctionExpression &function) {
			    if (!result && BoundCastExpression::IsCast(function)) {
				    result = function;
			    }
		    });
	});
	if (result) {
		return result;
	}
	for (auto &child : op.children) {
		result = FindCast(*child);
		if (result) {
			return result;
		}
	}
	return nullptr;
}

struct OffsetCast {
	template <class SRC, class DST>
	static DST Operation(SRC input) {
		return static_cast<DST>(input + 1);
	}
};

static bool BigintToTinyintOffsetCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	return VectorCastHelpers::TemplatedCastLoop<int64_t, int8_t, OffsetCast>(source, result, count, parameters);
}

static bool TinyintToBigintOffsetCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	return VectorCastHelpers::TemplatedCastLoop<int8_t, int64_t, OffsetCast>(source, result, count, parameters);
}

static unique_ptr<BaseStatistics> BigintToTinyintOffsetStatistics(CastStatisticsInput &input) {
	if (input.child_stats.GetStatsType() != StatisticsType::NUMERIC_STATS ||
	    !NumericStats::HasMinMax(input.child_stats)) {
		return nullptr;
	}
	auto min = BigIntValue::Get(NumericStats::Min(input.child_stats));
	auto max = BigIntValue::Get(NumericStats::Max(input.child_stats));
	if (min < -129 || max > 126) {
		return nullptr;
	}
	auto result = NumericStats::CreateEmpty(input.target_type);
	result.CopyBase(input.child_stats);
	NumericStats::SetMin(result, Value::TINYINT(static_cast<int8_t>(min + 1)));
	NumericStats::SetMax(result, Value::TINYINT(static_cast<int8_t>(max + 1)));
	return result.ToUnique();
}

#if DUCKDB_SMALLER_BINARY(unchecked_numeric_casts)
static constexpr bool UNCHECKED_CASTS_ENABLED = false;
#else
static constexpr bool UNCHECKED_CASTS_ENABLED = true;
#endif

} // namespace

TEST_CASE("Cast statistics only specialize proven native integer ranges", "[optimizer][cast]") {
	auto checked_cast = GetDefaultCast(LogicalType::BIGINT, LogicalType::TINYINT);
	REQUIRE(checked_cast.HasStatisticsCallback());

	auto safe_stats = CreateNumericStats(LogicalType::BIGINT, Value::BIGINT(-128), Value::BIGINT(127));
	auto safe_cast = checked_cast.Copy();
	auto propagated = safe_cast.PropagateStatistics(LogicalType::BIGINT, LogicalType::TINYINT, safe_stats);
	REQUIRE(propagated);
	REQUIRE(NumericStats::Min(*propagated) == Value::TINYINT(-128));
	REQUIRE(NumericStats::Max(*propagated) == Value::TINYINT(127));
	REQUIRE(safe_cast.Equals(checked_cast) != UNCHECKED_CASTS_ENABLED);

	auto all_null_stats = NumericStats::CreateEmpty(LogicalType::BIGINT);
	all_null_stats.Set(StatsInfo::CAN_HAVE_NULL_VALUES);
	auto all_null_cast = checked_cast.Copy();
	auto all_null_result = all_null_cast.PropagateStatistics(LogicalType::BIGINT, LogicalType::TINYINT, all_null_stats);
	REQUIRE(all_null_result);
	REQUIRE(all_null_result->CanHaveNull());
	REQUIRE(!all_null_result->CanHaveNoNull());
	REQUIRE(all_null_cast.Equals(checked_cast) != UNCHECKED_CASTS_ENABLED);

	// Re-running propagation with unsafe statistics must restore the checked implementation.
	auto unsafe_stats = CreateNumericStats(LogicalType::BIGINT, Value::BIGINT(-129), Value::BIGINT(127));
	REQUIRE(!safe_cast.PropagateStatistics(LogicalType::BIGINT, LogicalType::TINYINT, unsafe_stats));
	REQUIRE(safe_cast.Equals(checked_cast));

	auto boolean_cast = GetDefaultCast(LogicalType::INTEGER, LogicalType::BOOLEAN);
	auto boolean_stats = CreateNumericStats(LogicalType::INTEGER, Value::INTEGER(0), Value::INTEGER(1));
	auto checked_boolean_cast = boolean_cast.Copy();
	REQUIRE(!boolean_cast.PropagateStatistics(LogicalType::INTEGER, LogicalType::BOOLEAN, boolean_stats));
	REQUIRE(boolean_cast.Equals(checked_boolean_cast));

	auto hugeint_cast = GetDefaultCast(LogicalType::HUGEINT, LogicalType::TINYINT);
	REQUIRE(hugeint_cast.HasStatisticsCallback());
	auto floating_cast = GetDefaultCast(LogicalType::DOUBLE, LogicalType::TINYINT);
	REQUIRE(floating_cast.HasStatisticsCallback());

	auto copy = checked_cast.Copy();
	REQUIRE(copy.Equals(checked_cast));
	copy.SetFunction(DefaultCasts::NopCast);
	REQUIRE(!copy.HasStatisticsCallback());
	REQUIRE(!copy.Equals(checked_cast));
}

TEST_CASE("Specialized casts preserve vector encodings and validity", "[optimizer][cast]") {
	auto checked_cast = GetDefaultCast(LogicalType::BIGINT, LogicalType::TINYINT);
	auto cast = checked_cast.Copy();
	auto stats = CreateNumericStats(LogicalType::BIGINT, Value::BIGINT(-128), Value::BIGINT(127));
	REQUIRE(cast.PropagateStatistics(LogicalType::BIGINT, LogicalType::TINYINT, stats));
	REQUIRE(cast.Equals(checked_cast) != UNCHECKED_CASTS_ENABLED);

	Vector flat(LogicalType::BIGINT, 4);
	auto flat_data = FlatVector::GetDataMutable<int64_t>(flat);
	flat_data[0] = -128;
	flat_data[1] = 0;
	flat_data[2] = 42;
	flat_data[3] = 127;
	FlatVector::ValidityMutable(flat).SetInvalid(2);
	Vector flat_result(LogicalType::TINYINT, 4);
	CastParameters parameters;
	REQUIRE(cast.Cast(flat, flat_result, 4, parameters));
	REQUIRE(flat_result.GetValue(0) == Value::TINYINT(-128));
	REQUIRE(flat_result.GetValue(1) == Value::TINYINT(0));
	REQUIRE(flat_result.GetValue(2).IsNull());
	REQUIRE(flat_result.GetValue(3) == Value::TINYINT(127));

	SelectionVector selection(4);
	selection.set_index(0, 3);
	selection.set_index(1, 2);
	selection.set_index(2, 1);
	selection.set_index(3, 0);
	Vector dictionary(LogicalType::BIGINT, 4);
	dictionary.Slice(flat, selection, 4);
	Vector dictionary_result(LogicalType::TINYINT, 4);
	REQUIRE(cast.Cast(dictionary, dictionary_result, 4, parameters));
	REQUIRE(dictionary_result.GetValue(0) == Value::TINYINT(127));
	REQUIRE(dictionary_result.GetValue(1).IsNull());
	REQUIRE(dictionary_result.GetValue(2) == Value::TINYINT(0));
	REQUIRE(dictionary_result.GetValue(3) == Value::TINYINT(-128));

	Vector constant(Value::BIGINT(127), count_t(4));
	Vector constant_result(LogicalType::TINYINT, 4);
	REQUIRE(cast.Cast(constant, constant_result, 4, parameters));
	REQUIRE(constant_result.GetVectorType() == VectorType::CONSTANT_VECTOR);
	REQUIRE(constant_result.GetValue(0) == Value::TINYINT(127));
}

TEST_CASE("Optimizer specialization respects exact integer cast boundaries", "[optimizer][cast]") {
	DuckDB db;
	Connection connection(db);
	REQUIRE_NO_FAIL(connection.Query("CREATE TABLE signed_safe AS SELECT * FROM (VALUES (-128::BIGINT), (127)) t(i)"));
	REQUIRE_NO_FAIL(connection.Query("CREATE TABLE signed_low AS SELECT * FROM (VALUES (-129::BIGINT), (127)) t(i)"));
	REQUIRE_NO_FAIL(connection.Query("CREATE TABLE signed_high AS SELECT * FROM (VALUES (-128::BIGINT), (128)) t(i)"));
	REQUIRE_NO_FAIL(connection.Query("CREATE TABLE unsigned_safe AS SELECT * FROM (VALUES (0::BIGINT), (255)) t(i)"));
	REQUIRE_NO_FAIL(connection.Query("CREATE TABLE unsigned_low AS SELECT * FROM (VALUES (-1::BIGINT), (255)) t(i)"));
	REQUIRE_NO_FAIL(connection.Query("CREATE TABLE unsigned_high AS SELECT * FROM (VALUES (0::BIGINT), (256)) t(i)"));
	REQUIRE_NO_FAIL(connection.Query("CREATE TABLE boolean_values AS SELECT * FROM (VALUES (0::INTEGER), (1)) t(i)"));

	connection.BeginTransaction();
	RequireSingleCast(*OptimizeQuery(connection, "SELECT i::TINYINT FROM signed_safe"), UNCHECKED_CASTS_ENABLED);
	RequireSingleCast(*OptimizeQuery(connection, "SELECT TRY_CAST(i AS TINYINT) FROM signed_safe"),
	                  UNCHECKED_CASTS_ENABLED);
	RequireSingleCast(*OptimizeQuery(connection, "SELECT i::TINYINT FROM signed_low"), false);
	RequireSingleCast(*OptimizeQuery(connection, "SELECT i::TINYINT FROM signed_high"), false);
	RequireSingleCast(*OptimizeQuery(connection, "SELECT i::UTINYINT FROM unsigned_safe"), UNCHECKED_CASTS_ENABLED);
	RequireSingleCast(*OptimizeQuery(connection, "SELECT i::UTINYINT FROM unsigned_low"), false);
	RequireSingleCast(*OptimizeQuery(connection, "SELECT i::UTINYINT FROM unsigned_high"), false);
	RequireSingleCast(*OptimizeQuery(connection, "SELECT i::BOOLEAN FROM boolean_values"), false);
	connection.Rollback();
}

TEST_CASE("Statistics specialize casts only after a successful proof", "[optimizer][cast]") {
	DuckDB db;
	Connection connection(db);
	REQUIRE_NO_FAIL(connection.Query("CREATE TABLE safe_values AS SELECT i::BIGINT i FROM range(128) t(i)"));
	REQUIRE_NO_FAIL(connection.Query("CREATE TABLE unsafe_values AS SELECT i::BIGINT i FROM range(256) t(i)"));
	REQUIRE_NO_FAIL(connection.Query("CREATE TABLE huge_values AS SELECT i::HUGEINT i FROM range(128) t(i)"));

	connection.BeginTransaction();
	auto safe_plan = OptimizeQuery(connection, "SELECT i::TINYINT FROM safe_values");
	RequireSingleCast(*safe_plan, UNCHECKED_CASTS_ENABLED);
	auto unsafe_plan = OptimizeQuery(connection, "SELECT i::TINYINT FROM unsafe_values");
	RequireSingleCast(*unsafe_plan, false);
	RequireSingleCast(*OptimizeQuery(connection, "SELECT i::TINYINT FROM huge_values"), false);

	auto safe_cast = FindCast(*safe_plan);
	auto unsafe_cast = FindCast(*unsafe_plan);
	REQUIRE(safe_cast);
	REQUIRE(unsafe_cast);
	REQUIRE(safe_cast->Equals(*unsafe_cast) != UNCHECKED_CASTS_ENABLED);
	auto safe_copy = safe_cast->Copy();
	REQUIRE(safe_copy->Equals(*safe_cast));
	connection.Rollback();

	auto safe_result = connection.Query("SELECT min(i::TINYINT), max(i::TINYINT) FROM safe_values");
	REQUIRE_NO_FAIL(*safe_result);
	REQUIRE(CHECK_COLUMN(safe_result, 0, {0}));
	REQUIRE(CHECK_COLUMN(safe_result, 1, {127}));
	auto unsafe_result = connection.Query("SELECT count(*) FILTER (WHERE TRY_CAST(i AS TINYINT) IS NULL) "
	                                      "FROM unsafe_values");
	REQUIRE_NO_FAIL(*unsafe_result);
	REQUIRE(CHECK_COLUMN(unsafe_result, 0, {128}));
	REQUIRE_FAIL(connection.Query("SELECT sum(i::TINYINT) FROM unsafe_values"));
}

TEST_CASE("Compressed materialization uses cast-owned proofs", "[optimizer][cast]") {
	DuckDB db;
	Connection connection(db);
	REQUIRE_NO_FAIL(connection.Query("CREATE TABLE safe_values AS SELECT i::BIGINT i FROM range(128) t(i)"));
	REQUIRE_NO_FAIL(connection.Query("CREATE TABLE huge_values AS SELECT i::HUGEINT i FROM range(128) t(i)"));

	connection.BeginTransaction();
	auto order_counts = CountPlanCasts(*OptimizeQuery(connection, "SELECT i FROM safe_values ORDER BY i"));
	REQUIRE(order_counts.specialized == (UNCHECKED_CASTS_ENABLED ? 2 : 0));
	REQUIRE(order_counts.checked == (UNCHECKED_CASTS_ENABLED ? 0 : 2));

	auto cte_counts = CountPlanCasts(*OptimizeQuery(
	    connection, "WITH cte AS MATERIALIZED (SELECT i FROM safe_values) SELECT i FROM cte ORDER BY i"));
	REQUIRE(cte_counts.specialized == (UNCHECKED_CASTS_ENABLED ? 2 : 0));
	REQUIRE(cte_counts.checked == (UNCHECKED_CASTS_ENABLED ? 0 : 2));

	auto huge_counts = CountPlanCasts(*OptimizeQuery(connection, "SELECT i FROM huge_values ORDER BY i"));
	REQUIRE(huge_counts.specialized == 0);
	REQUIRE(huge_counts.checked == 2);
	connection.Rollback();

	auto result = connection.Query("SELECT sum(i) FROM (SELECT i FROM safe_values ORDER BY i)");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(CHECK_COLUMN(result, 0, {8128}));
}

TEST_CASE("Custom casts without statistics callbacks are not specialized", "[optimizer][cast]") {
	DuckDB db;
	Connection connection(db);
	auto &casts = DBConfig::GetConfig(*connection.context).GetCastFunctions();
	casts.RegisterCastFunction(LogicalType::BIGINT, LogicalType::TINYINT, BoundCastInfo(BigintToTinyintOffsetCast));

	GetCastFunctionInput input(*connection.context);
	auto custom_cast = casts.GetCastFunction(LogicalType::BIGINT, LogicalType::TINYINT, input);
	REQUIRE(!custom_cast.HasStatisticsCallback());
	REQUIRE_NO_FAIL(connection.Query("CREATE TABLE custom_values AS SELECT i::BIGINT i FROM range(3) t(i)"));
	auto result = connection.Query("SELECT i::TINYINT FROM custom_values ORDER BY i");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(CHECK_COLUMN(result, 0, {1, 2, 3}));

	DuckDB reverse_db;
	Connection reverse_connection(reverse_db);
	auto &reverse_casts = DBConfig::GetConfig(*reverse_connection.context).GetCastFunctions();
	reverse_casts.RegisterCastFunction(LogicalType::TINYINT, LogicalType::BIGINT,
	                                   BoundCastInfo(TinyintToBigintOffsetCast));
	REQUIRE_NO_FAIL(reverse_connection.Query("CREATE TABLE reverse_values AS SELECT i::BIGINT i FROM range(128) t(i)"));
	auto reverse_result = reverse_connection.Query("SELECT sum(i) FROM (SELECT i FROM reverse_values ORDER BY i)");
	REQUIRE_NO_FAIL(*reverse_result);
	REQUIRE(CHECK_COLUMN(reverse_result, 0, {8128}));
}

TEST_CASE("Join filters preserve registered integer casts", "[optimizer][cast]") {
	DuckDB db;
	Connection connection(db);
	auto &casts = CastFunctionSet::Get(*connection.context);
	casts.RegisterCastFunction(LogicalType::BIGINT, LogicalType::TINYINT, BoundCastInfo(BigintToTinyintOffsetCast));
	REQUIRE_NO_FAIL(
	    connection.Query("SET disabled_optimizers="
	                     "'statistics_propagation,join_order,build_side_probe_side,compressed_materialization'"));
	REQUIRE_NO_FAIL(connection.Query("CREATE TABLE probe AS SELECT 100::BIGINT k FROM range(10000)"));
	unique_ptr<QueryResult> result;
	SECTION("An intermediate cast changes the value") {
		REQUIRE_NO_FAIL(connection.Query("CREATE TABLE build AS SELECT 101::BIGINT k"));
		result = connection.Query("SELECT count(*) FROM probe p JOIN build b ON p.k::TINYINT::BIGINT=b.k");
	}
	SECTION("A direct cast would introduce an override") {
		REQUIRE_NO_FAIL(connection.Query("CREATE TABLE build AS SELECT 100::TINYINT k"));
		result = connection.Query("SELECT count(*) FROM probe p JOIN build b ON p.k::SMALLINT::TINYINT=b.k");
	}
	REQUIRE_NO_FAIL(*result);
	REQUIRE(CHECK_COLUMN(result, 0, {10000}));
}

TEST_CASE("Compressed materialization uses value-preserving default casts", "[optimizer][cast]") {
	DuckDB db;
	Connection connection(db);
	auto &casts = DBConfig::GetConfig(*connection.context).GetCastFunctions();
	BoundCastInfo custom_cast(BigintToTinyintOffsetCast);
	custom_cast.SetStatisticsCallback(BigintToTinyintOffsetStatistics);
	casts.RegisterCastFunction(LogicalType::BIGINT, LogicalType::TINYINT, std::move(custom_cast));

	REQUIRE_NO_FAIL(connection.Query("CREATE TABLE custom_values AS SELECT i::BIGINT i FROM range(101) t(i)"));
	REQUIRE_NO_FAIL(connection.Query("SET debug_verify_serializer=true"));
	auto explicit_cast = connection.Query("SELECT sum(i::TINYINT) FROM custom_values");
	REQUIRE_NO_FAIL(*explicit_cast);
	REQUIRE(CHECK_COLUMN(explicit_cast, 0, {5151}));

	auto materialized = connection.Query("SELECT sum(i) FROM (SELECT i FROM custom_values ORDER BY i)");
	REQUIRE_NO_FAIL(*materialized);
	REQUIRE(CHECK_COLUMN(materialized, 0, {5050}));

	REQUIRE_NO_FAIL(connection.Query("SET storage_compatibility_version='v1.5.2'"));
	auto legacy_explicit_cast = connection.Query("SELECT sum(i::TINYINT) FROM custom_values");
	REQUIRE_NO_FAIL(*legacy_explicit_cast);
	REQUIRE(CHECK_COLUMN(legacy_explicit_cast, 0, {5151}));
	auto legacy_materialized = connection.Query("SELECT sum(i) FROM (SELECT i FROM custom_values ORDER BY i)");
	REQUIRE_NO_FAIL(*legacy_materialized);
	REQUIRE(CHECK_COLUMN(legacy_materialized, 0, {5050}));
}
