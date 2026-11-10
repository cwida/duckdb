#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/main/config.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_get.hpp"

using namespace duckdb;

struct JoinFilterHookTest : OptimizerExtensionInfo {
	idx_t join_filters = 0;
	idx_t cte_alternatives = 0;

	static void Inspect(LogicalOperator &op, JoinFilterHookTest &info) {
		if (op.type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
			auto &join = op.Cast<LogicalComparisonJoin>();
			if (join.filter_pushdown) {
				info.join_filters += join.filter_pushdown->probe_info.size();
			}
		} else if (op.type == LogicalOperatorType::LOGICAL_GET) {
			auto &get = op.Cast<LogicalGet>();
			if (get.dynamic_filters) {
				info.cte_alternatives += get.dynamic_filters->GetDisjunctionFilters().size();
			}
		}
		for (auto &child : op.children) {
			Inspect(*child, info);
		}
	}

	static void Optimize(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan) {
		auto &info = static_cast<JoinFilterHookTest &>(*input.info);
		info.join_filters = 0;
		info.cte_alternatives = 0;
		Inspect(*plan, info);
	}
};

TEST_CASE("Ordinary and CTE join filters are generated before optimizer hooks", "[optimizer][dynamic_filter_groups]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET disabled_optimizers='cte_filter_pusher,statistics_propagation,join_order,"
	                          "build_side_probe_side,compressed_materialization'"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE probe AS SELECT i AS k FROM range(1000) t(i)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE build AS SELECT i AS k FROM range(3) t(i)"));
	auto info = make_shared_ptr<JoinFilterHookTest>();
	OptimizerExtension extension;
	extension.optimizer_info = info;
	extension.optimize_function = JoinFilterHookTest::Optimize;
	OptimizerExtension::Register(DBConfig::GetConfig(*con.context), std::move(extension));

	REQUIRE_NO_FAIL(con.Query("SELECT count(*) FROM probe JOIN build USING (k)"));
	REQUIRE(info->join_filters > 0);
	REQUIRE(info->cte_alternatives == 0);
	auto result = con.Query("WITH p AS MATERIALIZED (SELECT k FROM probe) "
	                        "SELECT count(*) FROM p a JOIN build b ON a.k=b.k "
	                        "CROSS JOIN (p c JOIN build d ON c.k=d.k)");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(info->join_filters > 0);
	REQUIRE(info->cte_alternatives == 2);
	REQUIRE(CHECK_COLUMN(result, 0, {9}));
}
