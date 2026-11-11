#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/parser/parser.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/planner.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"
#include "duckdb/storage/statistics/node_statistics.hpp"

using namespace duckdb;

TEST_CASE("Aggregate overflow proofs use guaranteed bounds and can be withdrawn", "[optimizer][aggregate_statistics]") {
	DuckDB db;
	Connection con(db);
	for (const auto &type_name : {"HUGEINT", "DECIMAL(38,2)"}) {
		REQUIRE_NO_FAIL(con.Query("CREATE OR REPLACE TABLE bounds_input(v " + string(type_name) + ")"));
		con.BeginTransaction();
		for (const auto &function_name : {"sum", "avg"}) {
			INFO(type_name << " " << function_name);
			Parser parser(*con.context);
			parser.ParseQuery("SELECT " + string(function_name) + "(v) FROM bounds_input");
			Planner planner(*con.context);
			planner.CreatePlan(std::move(parser.statements[0]));
			optional_ptr<LogicalOperator> node = *planner.plan;
			while (node->type != LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
				REQUIRE(node->children.size() == 1);
				node = *node->children[0];
			}
			auto &aggregate = node->Cast<LogicalAggregate>().expressions[0]->Cast<BoundAggregateExpression>();
			REQUIRE(aggregate.Function().GetCallbacks().HasStatisticsCallback());
			auto callback = aggregate.Function().GetCallbacks().GetStatisticsCallback();
			vector<BaseStatistics> child_stats;
			child_stats.push_back(BaseStatistics::CreateUnknown(aggregate.GetChildren()[0]->GetReturnType()));
			NodeStatistics rows(2U, 2U);
			AggregateStatisticsInput input(aggregate.BindInfo(), child_stats, rows);
			auto set_range = [&](hugeint_t minimum, hugeint_t maximum) {
				NumericStats::SetMin(child_stats[0], Value::HUGEINT(minimum));
				NumericStats::SetMax(child_stats[0], Value::HUGEINT(maximum));
				callback(*con.context, aggregate, input);
			};

			REQUIRE(aggregate.CanThrow());
			set_range(-10, 20);
			REQUIRE_FALSE(aggregate.CanThrow());

			rows.has_max_cardinality = false;
			callback(*con.context, aggregate, input);
			REQUIRE(aggregate.CanThrow());
			rows.has_max_cardinality = true;

			// Decimal SUM has a narrower final result than AVG's HUGEINT accumulator.
			const auto large_value = Hugeint::POWERS_OF_TEN[37] * hugeint_t(6);
			for (const auto &value : {large_value, -large_value}) {
				set_range(value, value);
				const bool decimal_sum = string(type_name) == "DECIMAL(38,2)" && string(function_name) == "sum";
				REQUIRE(aggregate.CanThrow() == decimal_sum);
			}

			const auto overflowing_value = Hugeint::POWERS_OF_TEN[37] * hugeint_t(9);
			set_range(overflowing_value, overflowing_value);
			REQUIRE(aggregate.CanThrow());
			set_range(-overflowing_value, -overflowing_value);
			REQUIRE(aggregate.CanThrow());

			set_range(1, 2);
			REQUIRE_FALSE(aggregate.CanThrow());
			child_stats[0] = BaseStatistics::CreateUnknown(child_stats[0].GetType());
			callback(*con.context, aggregate, input);
			REQUIRE(aggregate.CanThrow());
		}
		con.Rollback();
	}
}
