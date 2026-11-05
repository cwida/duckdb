#include "catch.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

namespace {

struct BindData : public TableFunctionData {
	mutable string alias;
	unique_ptr<FunctionData> Copy() const override {
		auto out = make_uniq<BindData>();
		out->alias = alias;
		return std::move(out);
	}
};

} // namespace

TEST_CASE("Filter pushdown rewrites BoundColumnRef alias to the scan column name", "[optimizer][filter_pushdown]") {
	DuckDB db(nullptr);
	Connection con(db);

	TableFunction tf(
	    "alias_inspect", {}, [](auto &, auto &, auto &out) { out.SetChildCardinality(0); },
	    [](auto &, auto &, auto &types, auto &names) -> unique_ptr<FunctionData> {
		    types.emplace_back(LogicalType::INTEGER);
		    names.emplace_back("col");
		    return make_uniq<BindData>();
	    },
	    [](auto &, auto &) -> unique_ptr<GlobalTableFunctionState> { return {}; });

	tf.pushdown_complex_filter = [](auto &, auto &get, auto *bind_data_p, auto &filters) {
		auto &data = bind_data_p->template Cast<BindData>();
		for (auto &expr : filters) {
			ExpressionIterator::VisitExpressionMutable<BoundColumnRefExpression>(expr, [&](auto &ref, auto &) {
				if (ref.Binding().table_index == get.table_index && data.alias.empty()) {
					data.alias = ref.GetAlias().GetIdentifierName();
				}
			});
		}
	};
	tf.to_string = [](auto &input) {
		InsertionOrderPreservingMap<string> result;
		result["FilterAlias"] = input.bind_data->template Cast<BindData>().alias;
		return result;
	};

	ExtensionInfo ext_info {};
	ExtensionActiveLoad load_info {*db.instance, ext_info, "test_extension", ""};
	ExtensionLoader loader {load_info};
	loader.RegisterFunction(tf);

	REQUIRE_NO_FAIL(con.Query("PRAGMA explain_output='optimized_only'"));
	auto result =
	    con.Query("EXPLAIN SELECT * FROM (SELECT col AS other FROM alias_inspect()) sub WHERE other IS NOT NULL");
	REQUIRE_NO_FAIL(*result);

	string explain;
	for (idx_t row = 0; row < result->RowCount(); row++) {
		for (idx_t col = 0; col < result->ColumnCount(); col++) {
			explain += result->Collection().GetValue(col, row).ToString();
		}
	}
	REQUIRE(StringUtil::Contains(explain, "FilterAlias: col"));
}
