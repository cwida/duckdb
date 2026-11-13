#include "catch.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/set.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/expression_barrier.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

namespace {

mutex observed_lock;
set<int64_t> observed;
//! The operator where the test extension bars the filters that call observe()
LogicalOperatorType bar_observe_at = LogicalOperatorType::LOGICAL_INVALID;

//! Records the values it is called on and returns them unchanged
void ObserveFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	UnifiedVectorFormat format;
	args.data[0].ToUnifiedFormat(format);
	auto values = UnifiedVectorFormat::GetData<int64_t>(format);
	lock_guard<mutex> guard(observed_lock);
	for (idx_t i = 0; i < args.size(); i++) {
		auto idx = format.sel->get_index(i);
		if (format.validity.RowIsValid(idx)) {
			observed.insert(values[idx]);
		}
	}
	result.Reference(args.data[0]);
}

bool CallsObserve(const Expression &expr) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION &&
	    expr.Cast<BoundFunctionExpression>().Function().GetName() == "observe") {
		return true;
	}
	bool calls_observe = false;
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) { calls_observe |= CallsObserve(child); });
	return calls_observe;
}

void BarObserve(OptimizerExtensionInput &input, const LogicalOperator &op, unique_ptr<Expression> &filter) {
	if (op.type == bar_observe_at && !ExpressionBarrier::Contains(*filter) && CallsObserve(*filter)) {
		filter = ExpressionBarrier::Wrap(std::move(filter));
	}
}

//! Whether observe() was called on an odd value, i.e. on a row the join with allowed removes
bool ObservesOddValue(Connection &con, const string &query) {
	{
		lock_guard<mutex> guard(observed_lock);
		observed.clear();
	}
	auto result = con.Query(query);
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Collection().GetValue(0, 0) == Value::BIGINT(5));
	lock_guard<mutex> guard(observed_lock);
	for (auto value : observed) {
		if (value % 2 != 0) {
			return true;
		}
	}
	return false;
}

} // namespace

TEST_CASE("An optimizer extension can replace the filters pushed into an operator", "[optimizer][filter_pushdown]") {
	DuckDB db(nullptr);
	OptimizerExtension extension;
	extension.pushdown_filter_function = BarObserve;
	OptimizerExtension::Register(DBConfig::GetConfig(*db.instance), extension);
	Connection con(db);

	ScalarFunction observe("observe", {LogicalType::BIGINT}, LogicalType::BIGINT, ObserveFunction);
	CreateScalarFunctionInfo info(observe);
	con.context->RunFunctionInTransaction(
	    [&]() { Catalog::GetSystemCatalog(*con.context).CreateFunction(*con.context, info); });
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t AS SELECT range AS i FROM range(10)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE allowed AS SELECT range * 2 AS i FROM range(5)"));
	REQUIRE_NO_FAIL(con.Query("CREATE SECURE VIEW sv AS SELECT t.i FROM t JOIN allowed USING (i)"));
	const string through_view = "SELECT count(*) FROM sv WHERE observe(i) >= 0";
	const string through_join = "SELECT count(*) FROM t JOIN allowed USING (i) WHERE observe(t.i) >= 0";

	// left as is, the filter is pushed below the join onto every row of t
	bar_observe_at = LogicalOperatorType::LOGICAL_INVALID;
	REQUIRE(ObservesOddValue(con, through_view));
	REQUIRE(ObservesOddValue(con, through_join));

	// barred where it enters the secure view, it stays above the join inside the view
	bar_observe_at = LogicalOperatorType::LOGICAL_SECURE_VIEW;
	REQUIRE(!ObservesOddValue(con, through_view));
	REQUIRE(ObservesOddValue(con, through_join));

	// barred where it enters the join, it stays above the join
	bar_observe_at = LogicalOperatorType::LOGICAL_COMPARISON_JOIN;
	REQUIRE(!ObservesOddValue(con, through_join));
}
