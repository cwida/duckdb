#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/common/chrono.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/emptytableref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"

using namespace duckdb;

namespace {

//! The query nodes pushed into the remote catalog
vector<string> pushed_nodes;
//! The local expressions the remote catalog was asked to accept
vector<string> checked_expressions;

//! A local catalog that accepts query node pushdown, rejects MAP values, and answers every pushed node with
//! a single "pushed" row
class TestPushdownCatalog : public DuckCatalog {
public:
	explicit TestPushdownCatalog(AttachedDatabase &db) : DuckCatalog(db) {
	}

public:
	using DuckCatalog::RemoteExecute;
	using DuckCatalog::SupportsPushdown;

	bool Supports(RemoteCapability capability) const override {
		return capability == RemoteCapability::IS_REMOTE || capability == RemoteCapability::EXECUTE_QUERY_NODE;
	}

	bool SupportsPushdown(const ParsedExpression &expression) override {
		auto sql = expression.ToString();
		checked_expressions.push_back(sql);
		return !StringUtil::Contains(StringUtil::Lower(sql), "map");
	}

	unique_ptr<TableRef> RemoteExecute(ClientContext &context, unique_ptr<QueryNode> node) override {
		pushed_nodes.push_back(node->ToString());
		auto result = make_uniq<SelectNode>();
		result->select_list.push_back(ConstantExpression::String("pushed"));
		result->from_table = make_uniq<EmptyTableRef>();
		auto select = make_uniq<SelectStatement>();
		select->node = std::move(result);
		return make_uniq<SubqueryRef>(std::move(select));
	}
};

struct TestPushdownStorageExtension : StorageExtension {
	TestPushdownStorageExtension() {
		attach = [](optional_ptr<StorageExtensionInfo>, ClientContext &, AttachedDatabase &db, const string &,
		            AttachInfo &info, AttachOptions &) -> unique_ptr<Catalog> {
			info.path = IN_MEMORY_PATH;
			return make_uniq_base<Catalog, TestPushdownCatalog>(db);
		};
		create_transaction_manager = [](optional_ptr<StorageExtensionInfo>, AttachedDatabase &db,
		                                Catalog &) -> unique_ptr<TransactionManager> {
			return make_uniq<DuckTransactionManager>(db);
		};
	}
};

string GenerateValuesQuery(idx_t rows) {
	string sql = "SELECT count(*) FROM (VALUES ";
	for (idx_t i = 0; i < rows; i++) {
		if (i > 0) {
			sql += ", ";
		}
		auto row = to_string(i);
		sql += "(" + row + ", NULL, 'v-" + row + "')";
	}
	sql += ") v(a, b, c)";
	return sql;
}

double PrepareSeconds(Connection &con, const string &sql) {
	auto start = steady_clock::now();
	auto prepared = con.Prepare(sql);
	auto end = steady_clock::now();
	REQUIRE(!prepared->HasError());
	return duration_cast<duration<double>>(end - start).count();
}

} // namespace

TEST_CASE("Every VALUES cell and select list entry is checked by the remote catalog", "[api]") {
	DBConfig config;
	StorageExtension::Register(config, "test_pushdown", make_shared_ptr<TestPushdownStorageExtension>());
	DuckDB db(nullptr, &config);
	Connection con(db);

	REQUIRE_NO_FAIL(con.Query("ATTACH 'test' AS s (TYPE test_pushdown)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE s.tbl AS SELECT range AS i FROM range(5)"));

	// a MAP constant in any row of a VALUES list blocks pushdown
	pushed_nodes.clear();
	auto result =
	    con.Query("SELECT count(*) FROM s.tbl, (VALUES (1, NULL), (2, NULL), (3, NULL), (4, MAP {1: 2})) t(x, y)");
	REQUIRE(CHECK_COLUMN(result, 0, {20}));
	result = con.Query("SELECT count(*) FROM s.tbl, (VALUES (1, MAP {1: 2}), (2, NULL), (3, NULL), (4, NULL)) t(x, y)");
	REQUIRE(CHECK_COLUMN(result, 0, {20}));
	REQUIRE(pushed_nodes.empty());

	// without it the query is pushed, and every cell was offered to the remote catalog
	checked_expressions.clear();
	result = con.Query("SELECT count(*) FROM s.tbl, (VALUES (1, NULL), (2, NULL), (3, NULL), (4, 5)) t(x, y)");
	REQUIRE(CHECK_COLUMN(result, 0, {"pushed"}));
	REQUIRE(pushed_nodes.size() == 1);
	for (auto &cell : {"1", "2", "3", "4", "5"}) {
		REQUIRE(std::count(checked_expressions.begin(), checked_expressions.end(), cell) == 1);
	}
	REQUIRE(std::count(checked_expressions.begin(), checked_expressions.end(), "NULL") == 3);

	// the same holds for constants in a select list followed by a remote subquery
	pushed_nodes.clear();
	result = con.Query("SELECT 1, 2, MAP {1: 2}, (SELECT count(*) FROM s.tbl)");
	REQUIRE(CHECK_COLUMN(result, 3, {5}));
	REQUIRE(pushed_nodes.empty());

	pushed_nodes.clear();
	result = con.Query("SELECT 1, 2, 3, (SELECT count(*) FROM s.tbl)");
	REQUIRE(CHECK_COLUMN(result, 0, {"pushed"}));
	REQUIRE(pushed_nodes.size() == 1);

	// multi-row INSERT ... VALUES into a remote table
	pushed_nodes.clear();
	checked_expressions.clear();
	result = con.Query("INSERT INTO s.tbl VALUES (1), (2), (3), (4)");
	REQUIRE(CHECK_COLUMN(result, 0, {"pushed"}));
	REQUIRE(pushed_nodes.size() == 1);
	for (auto &cell : {"1", "2", "3", "4"}) {
		REQUIRE(std::count(checked_expressions.begin(), checked_expressions.end(), cell) == 1);
	}

	pushed_nodes.clear();
	REQUIRE_NO_FAIL(con.Query("DETACH s"));
	result = con.Query("SELECT count(*) FROM (VALUES (1), (2)) t(x)");
	REQUIRE(CHECK_COLUMN(result, 0, {2}));
	REQUIRE(pushed_nodes.empty());
}

TEST_CASE("Planning a long VALUES list with a remote catalog attached is linear", "[api][.]") {
	DBConfig config;
	StorageExtension::Register(config, "test_pushdown", make_shared_ptr<TestPushdownStorageExtension>());
	DuckDB db(nullptr, &config);
	Connection con(db);

	auto sql = GenerateValuesQuery(50000);
	auto local_time = PrepareSeconds(con, sql);
	REQUIRE_NO_FAIL(con.Query("ATTACH 'test' AS s (TYPE test_pushdown)"));
	auto remote_time = PrepareSeconds(con, sql);
	// the pushdown analysis must not dominate planning
	REQUIRE(remote_time < local_time * 3 + 1);
}
