#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/common/multi_file/multi_file_states.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/planner.hpp"
#include "duckdb/storage/statistics/node_statistics.hpp"

using namespace duckdb;

namespace {

LogicalGet &BoundSource(LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_GET) {
		return op.Cast<LogicalGet>();
	}
	REQUIRE(op.children.size() == 1);
	return BoundSource(*op.children[0]);
}

} // namespace

TEST_CASE("Parquet row bounds survive consuming the initial reader", "[optimizer][source_cardinality_bounds]") {
	DuckDB db;
	if (ExtensionHelper::LoadExtension(db, "parquet") != ExtensionLoadResult::LOADED_EXTENSION) {
		SKIP_TEST("require parquet");
	}
	Connection con(db);
	auto path = TestCreatePath("source_row_bounds.parquet");
	REQUIRE_NO_FAIL(con.Query("COPY (SELECT i FROM range(5) t(i)) TO '" + path + "' (FORMAT PARQUET)"));
	con.BeginTransaction();
	Planner planner(*con.context);
	Parser parser(*con.context);
	parser.ParseQuery("SELECT * FROM read_parquet('" + path + "')");
	planner.CreatePlan(std::move(parser.statements[0]));
	auto &get = BoundSource(*planner.plan);
	auto &bind_data = get.bind_data->Cast<MultiFileBindData>();
	REQUIRE(bind_data.initial_reader);
	bind_data.initial_reader.reset();
	auto stats = get.function.cardinality(*con.context, get.bind_data.get());
	REQUIRE(stats);
	REQUIRE(stats->has_max_cardinality);
	REQUIRE(stats->max_cardinality == 5);

	// An execution copy has no retained row groups from which to prove a bound.
	auto execution_copy = get.bind_data->Copy();
	auto copied_stats = get.function.cardinality(*con.context, execution_copy.get());
	REQUIRE(copied_stats);
	REQUIRE_FALSE(copied_stats->has_max_cardinality);
	con.Rollback();
}
