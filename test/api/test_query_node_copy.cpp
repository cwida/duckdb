#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/parser/common_table_expression_info.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/view_catalog_entry.hpp"
#include "duckdb/main/external_dependencies.hpp"
#include "duckdb/parser/parsed_data/create_view_info.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/tableref/delimgetref.hpp"
#include "duckdb/parser/tableref/emptytableref.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/parser/tableref/pivotref.hpp"

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

class TestDependencyItem : public DependencyItem {};

static shared_ptr<ExternalDependency> TestDependency(const shared_ptr<DependencyItem> &item) {
	auto dependency = make_shared_ptr<ExternalDependency>();
	dependency->AddDependency("client_object", item);
	return dependency;
}

static void SetBaseProperties(TableRef &ref, const shared_ptr<ExternalDependency> &dependency) {
	ref.alias = Identifier("ref_alias");
	ref.query_location = QueryLocation(7, 3);
	ref.sample = make_uniq<SampleOptions>(42);
	ref.sample->sample_size = Value::BIGINT(10);
	ref.sample->is_percentage = false;
	ref.sample->method = SampleMethod::RESERVOIR_SAMPLE;
	ref.external_dependency = dependency;
}

static void RequireCopiedBaseProperties(const TableRef &original, const TableRef &copy) {
	REQUIRE(copy.type == original.type);
	REQUIRE(copy.alias == original.alias);
	REQUIRE(copy.query_location.IsValid());
	REQUIRE(copy.query_location.offset == original.query_location.offset);
	REQUIRE(copy.query_location.length == original.query_location.length);
	REQUIRE(copy.sample);
	REQUIRE(copy.sample.get() != original.sample.get());
	REQUIRE(SampleOptions::Equals(copy.sample.get(), original.sample.get()));
	REQUIRE(copy.external_dependency);
	REQUIRE(copy.external_dependency == original.external_dependency);
}

static unique_ptr<TableRef> TestBaseTable(const string &name) {
	auto ref = make_uniq<BaseTableRef>();
	ref->SetQualifiedName(QualifiedName(Identifier(name)));
	return std::move(ref);
}

static unique_ptr<TableRef> TestJoin(unique_ptr<TableRef> left) {
	auto join = make_uniq<JoinRef>(JoinRefType::NATURAL);
	join->left = std::move(left);
	join->right = TestBaseTable("join_right");
	return std::move(join);
}

static unique_ptr<TableRef> TestPivot() {
	auto pivot = make_uniq<PivotRef>();
	pivot->source = TestBaseTable("pivot_source");
	pivot->aggregates.push_back(make_uniq<ColumnRefExpression>(Identifier("pivot_value")));
	PivotColumn column;
	column.pivot_expressions.push_back(make_uniq<ColumnRefExpression>(Identifier("pivot_key")));
	PivotColumnEntry entry;
	entry.values.emplace_back(Value::INTEGER(1));
	column.entries.push_back(std::move(entry));
	pivot->pivots.push_back(std::move(column));
	return std::move(pivot);
}

TEST_CASE("Table reference copies keep the base table reference properties", "[api][parser]") {
	auto item = make_shared_ptr<TestDependencyItem>();
	auto dependency = TestDependency(item);

	vector<unique_ptr<TableRef>> refs;
	refs.push_back(TestBaseTable("reference_kind"));
	refs.push_back(TestJoin(TestBaseTable("join_left")));
	refs.push_back(TestPivot());
	refs.push_back(make_uniq<EmptyTableRef>());
	refs.push_back(make_uniq<DelimGetRef>(vector<LogicalType> {LogicalType::INTEGER}));
	for (auto &ref : refs) {
		INFO(EnumUtil::ToString(ref->type));
		SetBaseProperties(*ref, dependency);
		auto copy = ref->Copy();
		RequireCopiedBaseProperties(*ref, *copy);
	}
}

static void RequireViewJoinDependency(Connection &con, const string &step, const string &view_name,
                                      const shared_ptr<DependencyItem> &item) {
	INFO(step);
	con.BeginTransaction();
	auto &view = Catalog::GetEntry<ViewCatalogEntry>(*con.context, QualifiedName(Identifier(view_name)));
	auto &from_table = view.GetQuery().node->Cast<SelectNode>().from_table;
	REQUIRE(from_table->type == TableReferenceType::JOIN);
	REQUIRE(from_table->external_dependency);
	REQUIRE(from_table->external_dependency->GetDependency("client_object") == item);
	con.Rollback();
}

TEST_CASE("A view over a join keeps the external dependency of the join through catalog copies", "[api][parser]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE l (k INTEGER, a INTEGER)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE r (k INTEGER, b INTEGER)"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO l VALUES (1, 10)"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO r VALUES (1, 20)"));

	Parser parser(*con.context);
	parser.ParseQuery("CREATE VIEW v AS SELECT * FROM l JOIN r USING (k)");
	REQUIRE(parser.statements.size() == 1);
	auto &view_info = parser.statements[0]->Cast<CreateStatement>().info->Cast<CreateViewInfo>();
	auto &from_table = view_info.query->node->Cast<SelectNode>().from_table;
	REQUIRE(from_table->type == TableReferenceType::JOIN);
	auto item = make_shared_ptr<TestDependencyItem>();
	from_table->external_dependency = TestDependency(item);
	REQUIRE_NO_FAIL(con.Query(std::move(parser.statements[0])));
	RequireViewJoinDependency(con, "CREATE VIEW", "v", item);

	REQUIRE_NO_FAIL(con.Query("ALTER VIEW v RENAME TO w"));
	RequireViewJoinDependency(con, "ALTER VIEW RENAME", "w", item);

	REQUIRE_NO_FAIL(con.Query("COMMENT ON VIEW w IS 'a view over a join'"));
	RequireViewJoinDependency(con, "COMMENT ON VIEW", "w", item);

	auto result = con.Query("SELECT k, a, b FROM w");
	REQUIRE(CHECK_COLUMN(result, 0, {1}));
	REQUIRE(CHECK_COLUMN(result, 1, {10}));
	REQUIRE(CHECK_COLUMN(result, 2, {20}));
}
