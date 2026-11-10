#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
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

//! The SQL of every statement routed to the connected database
vector<string> routed_queries;

//! A catalog that can be CONNECT-ed to, and answers every routed statement with its SQL and the catalog name
class RoutingCatalog : public DuckCatalog {
public:
	explicit RoutingCatalog(AttachedDatabase &db) : DuckCatalog(db) {
	}

public:
	using DuckCatalog::RemoteExecute;

	bool Supports(RemoteCapability capability) const override {
		return capability == RemoteCapability::CONNECT;
	}

	unique_ptr<TableRef> RemoteExecute(ClientContext &context, const string &sql) override {
		if (sql == "UNREACHABLE") {
			throw IOException("the connected database is unreachable");
		}
		routed_queries.push_back(sql);
		auto node = make_uniq<SelectNode>();
		node->select_list.push_back(ConstantExpression::String(sql));
		node->select_list.push_back(ConstantExpression::String(GetName().GetIdentifierName()));
		node->from_table = make_uniq<EmptyTableRef>();
		auto select = make_uniq<SelectStatement>();
		select->node = std::move(node);
		return make_uniq<SubqueryRef>(std::move(select));
	}
};

struct RoutingStorageExtension : StorageExtension {
	RoutingStorageExtension() {
		attach = [](optional_ptr<StorageExtensionInfo>, ClientContext &, AttachedDatabase &db, const string &,
		            AttachInfo &info, AttachOptions &) -> unique_ptr<Catalog> {
			info.path = IN_MEMORY_PATH;
			return make_uniq_base<Catalog, RoutingCatalog>(db);
		};
		create_transaction_manager = [](optional_ptr<StorageExtensionInfo>, AttachedDatabase &db,
		                                Catalog &) -> unique_ptr<TransactionManager> {
			return make_uniq<DuckTransactionManager>(db);
		};
	}
};

} // namespace

TEST_CASE("Prepare statements while CONNECT-ed", "[api][connect]") {
	DBConfig config;
	StorageExtension::Register(config, "connect_routing_test", make_shared_ptr<RoutingStorageExtension>());
	DuckDB db(nullptr, &config);
	Connection con(db);

	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t (i INTEGER)"));
	REQUIRE_NO_FAIL(con.Query("ATTACH 'routed' AS remote (TYPE connect_routing_test)"));
	// parsed before CONNECT, with the local grammar
	auto parsed_insert = con.ExtractStatements("INSERT INTO t VALUES (2)");
	auto parsed_connect = con.ExtractStatements("CONNECT remote");
	auto parameterized = con.ExtractStatements("SELECT $1::INTEGER");
	REQUIRE_NO_FAIL(con.Query("CONNECT remote"));
	routed_queries.clear();

	// preparing routes nothing
	auto select = con.Prepare("SELECT 42");
	REQUIRE(!select->HasError());
	auto insert = con.Prepare("INSERT INTO t VALUES (1)");
	REQUIRE(!insert->HasError());
	REQUIRE(routed_queries.empty());

	// each execution routes the statement as written
	auto result = select->Execute();
	REQUIRE(CHECK_COLUMN(result, 0, {"SELECT 42"}));
	result = insert->Execute();
	REQUIRE(CHECK_COLUMN(result, 0, {"INSERT INTO t VALUES (1)"}));
	result = insert->Execute();
	REQUIRE(CHECK_COLUMN(result, 0, {"INSERT INTO t VALUES (1)"}));
	REQUIRE(routed_queries == vector<string> {"SELECT 42", "INSERT INTO t VALUES (1)", "INSERT INTO t VALUES (1)"});
	auto parsed = con.Prepare(std::move(parsed_insert[0]));
	REQUIRE(!parsed->HasError());
	REQUIRE(routed_queries.size() == 3);
	result = parsed->Execute();
	REQUIRE(CHECK_COLUMN(result, 0, {"INSERT INTO t VALUES (2)"}));
	REQUIRE(routed_queries.size() == 4);
	auto handle = select->Submit();
	REQUIRE_NO_FAIL(*handle);
	handle->Complete();
	REQUIRE(CHECK_COLUMN(handle, 0, {"SELECT 42"}));
	REQUIRE(CHECK_COLUMN(handle, 1, {"remote"}));
	REQUIRE(routed_queries.size() == 5);
	// the result is known only at execution
	REQUIRE(!select->MetadataAvailable());
	REQUIRE(select->GetStatementType() == StatementType::PASSTHROUGH_STATEMENT);

	// parameter values cannot be routed
	REQUIRE(insert->GetParameterCount() == 0);
	vector<Value> values {Value::INTEGER(1)};
	result = insert->Execute(values);
	REQUIRE_FAIL(result);
	REQUIRE(StringUtil::Contains(result->GetError(), "cannot be executed while CONNECT-ed"));
	auto prepared = con.Prepare(std::move(parameterized[0]));
	REQUIRE(prepared->HasError());
	REQUIRE(StringUtil::Contains(prepared->GetError(), "cannot be executed while CONNECT-ed"));
	REQUIRE_FAIL(insert->Submit(values));
	REQUIRE(routed_queries.size() == 5);

	// CONNECT is routed too, however it was parsed
	auto connect = con.Prepare("CONNECT remote");
	REQUIRE(!connect->HasError());
	result = connect->Execute();
	REQUIRE(CHECK_COLUMN(result, 0, {"CONNECT remote"}));
	auto parsed_connect_prepared = con.Prepare(std::move(parsed_connect[0]));
	REQUIRE(!parsed_connect_prepared->HasError());
	result = parsed_connect_prepared->Execute();
	REQUIRE(CHECK_COLUMN(result, 0, {"CONNECT remote"}));
	REQUIRE(routed_queries.size() == 7);

	// DISCONNECT prepared while CONNECT-ed runs locally
	auto disconnect = con.Prepare("DISCONNECT");
	REQUIRE(!disconnect->HasError());
	REQUIRE_NO_FAIL(disconnect->Execute());
	REQUIRE(!con.context->IsConnected());
	REQUIRE(routed_queries.size() == 7);

	// statements prepared for the connected database do not run locally
	REQUIRE_FAIL(select->Execute());
	REQUIRE_FAIL(insert->Execute());
	REQUIRE_FAIL(parsed->Execute());
	REQUIRE_FAIL(connect->Execute());
	REQUIRE_FAIL(parsed_connect_prepared->Execute());
	REQUIRE(!con.context->IsConnected());
	result = con.Query("SELECT count(*) FROM t");
	REQUIRE(CHECK_COLUMN(result, 0, {0}));
	REQUIRE(routed_queries.size() == 7);

	// they run on the database that is connected at execution
	REQUIRE_NO_FAIL(con.Query("ATTACH 'routed' AS other (TYPE connect_routing_test)"));
	REQUIRE_NO_FAIL(con.Query("CONNECT other"));
	result = select->Execute();
	REQUIRE(CHECK_COLUMN(result, 0, {"SELECT 42"}));
	REQUIRE(CHECK_COLUMN(result, 1, {"other"}));
	REQUIRE(routed_queries.size() == 8);

	// a statement that cannot be routed fails at execution, and the connection stays usable
	auto unreachable = con.Prepare("UNREACHABLE");
	REQUIRE(!unreachable->HasError());
	result = unreachable->Execute();
	REQUIRE_FAIL(result);
	REQUIRE(StringUtil::Contains(result->GetError(), "unreachable"));
	REQUIRE_FAIL(unreachable->Submit());
	result = select->Execute();
	REQUIRE(CHECK_COLUMN(result, 1, {"other"}));
	REQUIRE(routed_queries.size() == 9);

	// they fail once another connection detached the connected database
	Connection detacher(db);
	REQUIRE_NO_FAIL(detacher.Query("DETACH other"));
	result = select->Execute();
	REQUIRE_FAIL(result);
	REQUIRE(StringUtil::Contains(result->GetError(), "has been detached"));
	REQUIRE(routed_queries.size() == 9);
	REQUIRE_NO_FAIL(con.Query("DISCONNECT"));
}
