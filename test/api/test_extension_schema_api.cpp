#include "catch.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/extension_manager.hpp"
#include "test_helpers.hpp"
#include "duckdb/catalog/catalog_search_path.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/catalog/catalog_entry/table_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/common/enums/database_modification_type.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/statement/logical_plan_statement.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/planner.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/transaction/meta_transaction.hpp"

using namespace duckdb;

static void ReturnCustomSchema(DataChunk &args, ExpressionState &, Vector &result) {
	result.Reference(Value("custom_schema"), count_t(args.size()));
}

namespace {

static optional_ptr<LogicalGet> FindQualifiedTableFunction(LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_GET) {
		return op.Cast<LogicalGet>();
	}
	for (auto &child : op.children) {
		auto get = FindQualifiedTableFunction(*child);
		if (get) {
			return get;
		}
	}
	return nullptr;
}

static void CheckTableFunctionQualification(Connection &connection, const string &sql, const QualifiedName &name,
                                            int64_t first_value) {
	Parser parser(*connection.context);
	parser.ParseQuery(sql);
	Planner planner(*connection.context);
	planner.CreatePlan(std::move(parser.statements[0]));
	planner.plan->ResolveOperatorTypes();
	Optimizer optimizer(*planner.binder, *connection.context);
	planner.plan = optimizer.Optimize(std::move(planner.plan));
	planner.plan->ResolveOperatorTypes();
	auto get = FindQualifiedTableFunction(*planner.plan);
	REQUIRE(get);
	REQUIRE(get->function.GetQualifiedName() == name);
	REQUIRE(get->function.GetCatalogName() == name.Catalog());
	REQUIRE(get->function.GetSchemaName() == name.Schema());
	auto definition = get->function;
	REQUIRE(definition.GetQualifiedName() == name);
	definition.SetName(Identifier("renamed_function"));
	REQUIRE(definition.GetQualifiedName() == name.WithName("renamed_function"));
	auto copy = planner.plan->Copy(*connection.context);
	copy->ResolveOperatorTypes();
	auto copied_get = FindQualifiedTableFunction(*copy);
	REQUIRE(copied_get);
	REQUIRE(copied_get->function.GetQualifiedName() == name);
	auto direct = connection.Query(make_uniq<LogicalPlanStatement>(std::move(planner.plan)));
	auto copied = connection.Query(make_uniq<LogicalPlanStatement>(std::move(copy)));
	for (auto &result_ref : vector<reference<QueryResult>> {*direct, *copied}) {
		auto &result = result_ref.get();
		REQUIRE_NO_FAIL(result);
		REQUIRE(result.GetTypes() == vector<LogicalType> {LogicalType::BIGINT});
		REQUIRE(result.RowCount() == 2);
		REQUIRE(result.Collection().GetValue(0, 0) == Value::BIGINT(first_value));
		REQUIRE(result.Collection().GetValue(0, 1) == Value::BIGINT(first_value + 1));
	}
}

} // namespace

TEST_CASE("Table function registration retains canonical qualification", "[api][table_function_qualification]") {
	bool incremental = true;
	SECTION("Incremental overload registration") {
	}
	SECTION("Catalog overload merge") {
		incremental = false;
	}
	DuckDB db(nullptr);
	Connection connection(db);
	ExtensionLoader loader(*db.instance, "function_qualification");
	REQUIRE_NO_FAIL(
	    connection.Query("CREATE SCHEMA shadow; CREATE MACRO shadow.qualified_range(x) AS TABLE SELECT 99"));
	REQUIRE_NO_FAIL(connection.Query("SET search_path='shadow,main'"));
	auto shadow = connection.Query("SELECT * FROM qualified_range(2)");
	REQUIRE_NO_FAIL(*shadow);
	REQUIRE(shadow->Collection().GetValue(0, 0) == Value::INTEGER(99));
	connection.BeginTransaction();
	auto &range =
	    Catalog::GetEntry<TableFunctionCatalogEntry>(*connection.context, QualifiedName("system", "main", "range"));
	auto one = *range.functions.GetFunctionByArguments(*connection.context, {LogicalType::BIGINT});
	auto two = *range.functions.GetFunctionByArguments(*connection.context, {LogicalType::BIGINT, LogicalType::BIGINT});
	for (auto &function : vector<reference<TableFunction>> {one, two}) {
		function.get().SetQualifiedName(QualifiedName("qualified_range"));
	}
	loader.RegisterFunction(one);
	if (incremental) {
		loader.AddFunctionOverload(TableFunctionSet(two));
	} else {
		loader.RegisterFunction(two);
	}
	const QualifiedName name("system", "main", "qualified_range");
	REQUIRE(Catalog::GetEntry<TableFunctionCatalogEntry>(*connection.context, name).internal);
	CheckTableFunctionQualification(connection, "SELECT * FROM system.main.qualified_range(2)", name, 0);
	CheckTableFunctionQualification(connection, "SELECT * FROM system.main.qualified_range(2,4)", name, 2);
	connection.Rollback();
}

TEST_CASE("Aliased table function overloads retain catalog identity", "[api][table_function_qualification]") {
	bool nested = true;
	SECTION("Nested schema") {
	}
	SECTION("Flat schema") {
		nested = false;
	}
	DuckDB db(nullptr);
	Connection connection(db);
	REQUIRE_NO_FAIL(connection.Query("ATTACH ':memory:' AS attached"));
	for (const auto &catalog_name : {"memory", "attached"}) {
		CAPTURE(catalog_name);
		REQUIRE_NO_FAIL(connection.Query("CREATE SCHEMA " + string(catalog_name) + ".outer_schema"));
		REQUIRE_NO_FAIL(connection.Query("CREATE SCHEMA " + string(catalog_name) + ".outer_schema.inner_schema"));
		connection.BeginTransaction();
		auto &range =
		    Catalog::GetEntry<TableFunctionCatalogEntry>(*connection.context, QualifiedName("system", "main", "range"));
		auto one = *range.functions.GetFunctionByArguments(*connection.context, {LogicalType::BIGINT});
		auto two =
		    *range.functions.GetFunctionByArguments(*connection.context, {LogicalType::BIGINT, LogicalType::BIGINT});
		vector<Identifier> path {catalog_name, "outer_schema"};
		if (nested) {
			path.emplace_back("inner_schema");
		}
		QualifiedName name(std::move(path), "qualified_range");
		auto &catalog = Catalog::GetCatalog(*connection.context, Identifier(catalog_name));
		MetaTransaction::Get(*connection.context)
		    .ModifyDatabase(catalog.GetAttached(), DatabaseModificationType::CREATE_CATALOG_ENTRY);
		for (auto &function : vector<reference<TableFunction>> {one, two}) {
			CreateTableFunctionInfo info(function.get());
			info.internal = false;
			info.SetQualifiedName(name);
			info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
			catalog.CreateFunction(*connection.context, info);
			auto &entry = Catalog::GetEntry<TableFunctionCatalogEntry>(*connection.context, name);
			REQUIRE_FALSE(entry.internal);
			REQUIRE(entry.name == name.Name());
			REQUIRE(entry.functions.name == name.Name());
			CheckTableFunctionQualification(connection, "SELECT * FROM " + name.ToString() + "(2)", name, 0);
		}
		CheckTableFunctionQualification(connection, "SELECT * FROM " + name.ToString() + "(2,4)", name, 2);
		connection.Rollback();
	}
}

TEST_CASE("Function qualification setters retain existing component semantics", "[api][table_function_qualification]") {
	for (bool schema_first : {false, true}) {
		TableFunction function;
		function.name = Identifier("function_name");
		if (schema_first) {
			function.SetSchemaName("schema_name");
			function.SetCatalogName("catalog_name");
		} else {
			function.SetCatalogName("catalog_name");
			function.SetSchemaName("schema_name");
		}
		REQUIRE(function.GetQualifiedName() == QualifiedName("catalog_name", "schema_name", "function_name"));
		function.SetQualifiedName(
		    QualifiedName(vector<Identifier> {"catalog_name", "outer_schema", "inner_schema"}, "function_name"));
		function.SetCatalogName(Identifier());
		REQUIRE(function.GetCatalogName().empty());
		REQUIRE(function.GetSchemaName() == Identifier("inner_schema"));
		function.SetCatalogName("other_catalog");
		REQUIRE(function.GetQualifiedName() ==
		        QualifiedName(vector<Identifier> {"other_catalog", "outer_schema", "inner_schema"}, "function_name"));
		function.SetSchemaName("other_schema");
		REQUIRE(function.GetQualifiedName() == QualifiedName("other_catalog", "other_schema", "function_name"));
		function.SetName("other_name");
		REQUIRE(function.GetQualifiedName() == QualifiedName("other_catalog", "other_schema", "other_name"));
	}
}

static void ReturnMainSchema(DataChunk &args, ExpressionState &, Vector &result) {
	result.Reference(Value("main_schema"), count_t(args.size()));
}

static ExtensionLoader CreateExtensionLoader(DuckDB &db, const string &name) {
	ExtensionInfo extension_info {};
	ExtensionActiveLoad load_info {*db.instance, extension_info, duckdb::Identifier(name), duckdb::Identifier()};
	return ExtensionLoader {load_info};
}

TEST_CASE("Test ExtensionLoader schema API", "[api]") {
	DuckDB db(nullptr);
	Connection conn(db);

	SECTION("CreateExtensionSchema creates a schema visible in the catalog") {
		auto loader = CreateExtensionLoader(db, "test_ext");
		loader.CreateSchema("test_schema");

		auto result =
		    conn.Query("SELECT schema_name FROM information_schema.schemata WHERE schema_name = 'test_schema'");
		REQUIRE_NO_FAIL(*result);
		REQUIRE(result->RowCount() == 1);
	}

	SECTION("UseDefaultSchema routes RegisterFunction into the custom schema") {
		auto loader = CreateExtensionLoader(db, "test_ext");
		loader.CreateSchema("custom_schema");
		loader.UseDefaultSchema("custom_schema");
		loader.RegisterFunction(ScalarFunction("fn_in_custom", {}, LogicalType::VARCHAR, ReturnCustomSchema));

		// Both qualified and unqualified call fail because schema is not on the search path yet
		REQUIRE_FAIL(conn.Query("SELECT custom_schema.fn_in_custom()"));
		REQUIRE_FAIL(conn.Query("SELECT fn_in_custom()"));
	}

	SECTION("Test AddSchemaToSearchPath") {
		auto loader = CreateExtensionLoader(db, "test_ext");
		loader.CreateSchema("search_schema");
		loader.UseDefaultSchema("search_schema");
		loader.AddSchemaToSearchPath("search_schema");
		loader.RegisterFunction(ScalarFunction("fn_in_search", {}, LogicalType::VARCHAR, ReturnCustomSchema));

		// we need to manually call this to refresh the catalog search path for this connection
		// normally this is done in physical_load.cpp, after the extension is successfully loaded
		loader.RefreshSearchPath(*conn.context);

		REQUIRE_NO_FAIL(conn.Query("SELECT search_schema.fn_in_search()"));
		REQUIRE_NO_FAIL(conn.Query("SELECT fn_in_search()"));
	}

	SECTION("UseDefaultSchema() registers subsequent functions in the DEFAULT_SCHEMA") {
		auto loader = CreateExtensionLoader(db, "test_ext");
		loader.CreateSchema("reset_schema");

		loader.UseDefaultSchema("reset_schema");
		loader.RegisterFunction(ScalarFunction("fn_before_reset", {}, LogicalType::VARCHAR, ReturnCustomSchema));

		// reset_schema has not been added to the search path yet
		REQUIRE_FAIL(conn.Query("SELECT reset_schema.fn_before_reset()"));
		loader.AddSchemaToSearchPath("reset_schema");
		loader.RefreshSearchPath(*conn.context);

		loader.UseDefaultSchema();
		// register another function in the main schema
		loader.RegisterFunction(ScalarFunction("fn_after_reset", {}, LogicalType::VARCHAR, ReturnMainSchema));

		// function registered in reset_schema
		REQUIRE_NO_FAIL(conn.Query("SELECT reset_schema.fn_before_reset()"));
		// check a function that is registered in the main schema
		REQUIRE_NO_FAIL(conn.Query("SELECT main.fn_after_reset()"));
		// fn_before_reset is not in the default schema
		REQUIRE_FAIL(conn.Query("SELECT main.fn_before_reset()"));
		// fn_after_reset is not in reset_schema
		REQUIRE_FAIL(conn.Query("SELECT reset_schema.fn_after_reset()"));
	}

	SECTION("UseDefaultSchema rejects invalid schema names") {
		auto loader = CreateExtensionLoader(db, "test_ext");
		REQUIRE_THROWS(loader.UseDefaultSchema("pg_catalog"));
	}

	SECTION("AddSchemaToSearchPath requires UseDefaultSchema and CreateExtensionSchema first") {
		auto loader = CreateExtensionLoader(db, "test_ext");
		// schema does not exist
		REQUIRE_THROWS(loader.AddSchemaToSearchPath("orphan_schema"));
		// create schema
		loader.CreateSchema("orphan_schema");
		// still throws because schema is not set
		REQUIRE_THROWS(loader.AddSchemaToSearchPath("orphan_schema"));
	}

	SECTION("UseDedicatedSchemaForExtension combines create, set, and add-to-path") {
		auto loader = CreateExtensionLoader(db, "test_ext");
		loader.UseDedicatedSchemaForExtension("combined_schema");
		loader.RegisterFunction(ScalarFunction("fn_combined", {}, LogicalType::VARCHAR, ReturnCustomSchema));
		loader.RefreshSearchPath(*conn.context);

		REQUIRE_NO_FAIL(conn.Query("SELECT combined_schema.fn_combined()"));
		REQUIRE_NO_FAIL(conn.Query("SELECT fn_combined()"));
	}
}

TEST_CASE("Test that Catalog::CreateSchema rejects unsupported options", "[api]") {
	DuckDB db(nullptr);
	Connection conn(db);
	conn.BeginTransaction();

	auto &catalog = Catalog::GetCatalog(*conn.context, "memory");
	MetaTransaction::Get(*conn.context)
	    .ModifyDatabase(catalog.GetAttached(), DatabaseModificationType::CREATE_CATALOG_ENTRY);

	CreateSchemaInfo info;
	info.SetQualifiedName(QualifiedName(vector<Identifier> {"memory", "s1"}, Identifier()));
	info.options.emplace("location", ConstantExpression::String("s3://bucket/folder"));

	// the options are not supported by the DuckDB catalog - they must be rejected instead of discarded
	REQUIRE_THROWS(catalog.CreateSchema(*conn.context, info));

	info.options.clear();
	REQUIRE_NOTHROW(catalog.CreateSchema(*conn.context, info));
	conn.Commit();

	auto result = conn.Query("SELECT schema_name FROM information_schema.schemata WHERE schema_name = 's1'");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->RowCount() == 1);
}
