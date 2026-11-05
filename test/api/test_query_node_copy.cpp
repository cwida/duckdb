#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/parser/common_table_expression_info.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/query_node/select_node.hpp"

using namespace duckdb;

static unique_ptr<CommonTableExpressionInfo> TestCTE() {
	auto cte = make_uniq<CommonTableExpressionInfo>();
	cte->aliases.emplace_back("cte_alias");
	cte->key_targets.push_back(make_uniq<ColumnRefExpression>(Identifier("key_column")));
	cte->payload_aggregates.push_back(ConstantExpression::Integer(42));
	auto query_node = make_uniq<SelectNode>();
	query_node->select_list.push_back(make_uniq<ColumnRefExpression>(Identifier("result_column")));
	cte->query_node = std::move(query_node);
	cte->materialized = CTEMaterialize::CTE_MATERIALIZE_ALWAYS;
	cte->is_trigger_generated = true;
	return cte;
}

static void RequireEquivalentCTE(const CommonTableExpressionInfo &original, const CommonTableExpressionInfo &copy) {
	REQUIRE(copy.aliases == original.aliases);
	REQUIRE(ParsedExpression::ListEquals(copy.key_targets, original.key_targets));
	REQUIRE(ParsedExpression::ListEquals(copy.payload_aggregates, original.payload_aggregates));
	REQUIRE(copy.query_node->Equals(original.query_node.get()));
	REQUIRE(copy.materialized == original.materialized);
	REQUIRE(copy.is_trigger_generated == original.is_trigger_generated);
}

TEST_CASE("CTE copy preserves all properties", "[api][parser]") {
	auto original = TestCTE();
	auto copy = original->Copy();

	RequireEquivalentCTE(*original, *copy);
}

TEST_CASE("CTE copy owns its nested state", "[api][parser]") {
	auto original = TestCTE();
	auto copy = original->Copy();

	REQUIRE(copy->key_targets[0].get() != original->key_targets[0].get());
	REQUIRE(copy->payload_aggregates[0].get() != original->payload_aggregates[0].get());
	REQUIRE(copy->query_node.get() != original->query_node.get());
	auto &original_query = original->query_node->Cast<SelectNode>();
	auto &copied_query = copy->query_node->Cast<SelectNode>();
	REQUIRE(copied_query.select_list[0].get() != original_query.select_list[0].get());

	copy->aliases[0] = "copied_alias";
	copy->key_targets[0]->SetAlias("copied_key");
	copy->payload_aggregates[0]->SetAlias("copied_payload");
	copied_query.select_list[0]->SetAlias("copied_result");
	REQUIRE(original->aliases[0] == "cte_alias");
	REQUIRE_FALSE(original->key_targets[0]->HasAlias());
	REQUIRE_FALSE(original->payload_aggregates[0]->HasAlias());
	REQUIRE_FALSE(original_query.select_list[0]->HasAlias());
}

TEST_CASE("CTE containers preserve complete entries when copied", "[api][parser]") {
	CommonTableExpressionMap cte_map;
	cte_map.map["generated_cte"] = TestCTE();

	auto copied_map = cte_map.Copy();
	REQUIRE(copied_map.map.size() == 1);
	auto copied_map_entry = copied_map.map.find("generated_cte");
	REQUIRE(copied_map_entry != copied_map.map.end());
	RequireEquivalentCTE(*cte_map.map["generated_cte"], *copied_map_entry->second);

	SelectNode select;
	select.cte_map.map["generated_cte"] = TestCTE();
	auto copied_select = select.Copy();
	auto &copied_cte_map = copied_select->Cast<SelectNode>().cte_map.map;
	REQUIRE(copied_cte_map.size() == 1);
	auto copied_select_entry = copied_cte_map.find("generated_cte");
	REQUIRE(copied_select_entry != copied_cte_map.end());
	RequireEquivalentCTE(*select.cte_map.map["generated_cte"], *copied_select_entry->second);
}
