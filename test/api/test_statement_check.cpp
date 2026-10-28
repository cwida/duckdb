#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/function/pragma_function.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/statement/prepare_statement.hpp"
#include "duckdb/planner/extension_callback.hpp"
#include "duckdb/planner/logical_operator.hpp"

using namespace duckdb;
using namespace Catch::Matchers;

namespace {

idx_t expansion_calls;
idx_t bind_calls;

struct StatementCheck : ExtensionCallback {
	StatementType denied_type = StatementType::INVALID_STATEMENT;
	StatementCheckType last_type = StatementCheckType::SUBMISSION;
	idx_t calls = 0;
	bool fatal = false;

	void OnStatementCheck(ClientContext &context, const SQLStatement &statement, StatementCheckType type) override {
		calls++;
		last_type = type;
		if (fatal) {
			throw FatalException("statement check failed");
		}
		if (statement.type == StatementType::PREPARE_STATEMENT) {
			OnStatementCheck(context, *statement.Cast<PrepareStatement>().statement, type);
		}
		Value deny_select;
		if (statement.type == denied_type ||
		    (statement.type == StatementType::SELECT_STATEMENT &&
		     context.TryGetCurrentSetting("statement_check_deny_select", deny_select) &&
		     deny_select.GetValue<bool>())) {
			throw PermissionException("statement check denied");
		}
	}
};

struct StatementCheckFixture {
	DuckDB db {nullptr};
	Connection con {db};
	shared_ptr<StatementCheck> check = make_shared_ptr<StatementCheck>();

	StatementCheckFixture() {
		auto &config = DBConfig::GetConfig(*db.instance);
		config.AddExtensionOption("statement_check_deny_select", "Test statement checks", LogicalType::BOOLEAN,
		                          Value::BOOLEAN(false));
		// Registration also covers the connection that was opened above.
		ExtensionCallback::Register(config, check);
		ExtensionLoader loader(*db.instance, "statement_check_test");
		loader.RegisterFunction(TableFunction(
		    "statement_check_provider", {}, [](ClientContext &, TableFunctionInput &, DataChunk &) {},
		    [](ClientContext &, TableFunctionBindInput &, vector<LogicalType> &types,
		       vector<Identifier> &names) -> unique_ptr<FunctionData> {
			    bind_calls++;
			    types.push_back(LogicalType::INTEGER);
			    names.emplace_back("i");
			    return nullptr;
		    }));
		loader.RegisterFunction(
		    PragmaFunction::PragmaCall("statement_check_expand",
		                               [](ClientContext &, const FunctionParameters &parameters) -> string {
			                               expansion_calls++;
			                               return parameters.values[0].GetValue<string>();
		                               },
		                               {LogicalType::VARCHAR}));
		expansion_calls = 0;
		bind_calls = 0;
	}

	unique_ptr<SQLStatement> Parse(const string &sql) {
		auto statements = con.ExtractStatements(sql);
		return std::move(statements[0]);
	}
};

template <class RESULT>
void RequireDenied(const unique_ptr<RESULT> &result) {
	REQUIRE(result->HasError());
	REQUIRE_THAT(result->GetError(), Contains("statement check denied"));
}

} // namespace

TEST_CASE("Statement checks precede expansion and distinguish client operations", "[api][statement_check]") {
	StatementCheckFixture fixture;
	auto &con = fixture.con;
	auto &check = *fixture.check;
	const string sql = "PRAGMA statement_check_expand('SELECT 42')";
	check.denied_type = StatementType::PRAGMA_STATEMENT;

	RequireDenied(con.Query(sql));
	REQUIRE(check.last_type == StatementCheckType::SUBMISSION);
	RequireDenied(con.Submit(sql));
	RequireDenied(con.context->Query(fixture.Parse(sql), QueryParameters()));
	RequireDenied(con.Prepare(sql));
	REQUIRE(check.last_type == StatementCheckType::PREPARATION);
	RequireDenied(con.Prepare(fixture.Parse(sql)));
	REQUIRE_THROWS_WITH(con.context->BindStatement(fixture.Parse(sql)), Contains("statement check denied"));
	REQUIRE(check.last_type == StatementCheckType::INSPECTION);
	REQUIRE_THROWS_WITH(con.GetTableNames(sql), Contains("statement check denied"));
	REQUIRE_THROWS_WITH(con.ExtractPlan(sql), Contains("statement check denied"));
	REQUIRE(expansion_calls == 0);

	const auto calls = check.calls;
	REQUIRE(con.ExtractStatements(sql).size() == 1);
	REQUIRE(check.calls == calls);
	Connection later(fixture.db);
	RequireDenied(later.Query(sql));
	REQUIRE_NO_FAIL(con.Query("SELECT 42"));

	// The original PRAGMA is now allowed, but its generated SELECT must still be checked.
	check.denied_type = StatementType::INVALID_STATEMENT;
	REQUIRE_NO_FAIL(con.Query("SET statement_check_deny_select=true"));
	RequireDenied(con.Query(sql));
	RequireDenied(con.Prepare(sql));
	REQUIRE(check.last_type == StatementCheckType::SUBMISSION); // The generated PREPARE wrapper.
	REQUIRE_THROWS_WITH(con.context->BindStatement(fixture.Parse(sql)), Contains("statement check denied"));
	REQUIRE_THROWS_WITH(con.GetTableNames(sql), Contains("statement check denied"));
	REQUIRE_THROWS_WITH(con.ExtractPlan(sql), Contains("statement check denied"));
	REQUIRE(expansion_calls > 0);
	REQUIRE_NO_FAIL(later.Query("SELECT 42")); // The policy setting is connection-local.
}

TEST_CASE("Expanded statement checks observe preceding changes and preserve rollback", "[api][statement_check]") {
	StatementCheckFixture fixture;
	auto &con = fixture.con;
	const auto verification =
	    GENERATE("copy_statement", "reparse_statement", "prepared_statement", "explain_statement");
	REQUIRE_NO_FAIL(con.Query(string("SET debug_verify_statement='") + verification + "'"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE effects(i INTEGER)"));

	fixture.check->denied_type = StatementType::PRAGMA_STATEMENT;
	RequireDenied(con.Query("PRAGMA statement_check_expand('SELECT * FROM statement_check_provider()')"));
	REQUIRE(expansion_calls == 0);
	fixture.check->denied_type = StatementType::INVALID_STATEMENT;
	RequireDenied(con.Query("PRAGMA statement_check_expand('SET statement_check_deny_select=true; SELECT 42')"));
	RequireDenied(con.Query("PRAGMA statement_check_expand('SELECT * FROM statement_check_provider()')"));
	REQUIRE(bind_calls == 0); // Verification must not bind the generated statement before the check.
	// Checking all children up front would incorrectly reject this batch before the SET takes effect.
	REQUIRE_NO_FAIL(con.Query("PRAGMA statement_check_expand('SET statement_check_deny_select=false; SELECT 42')"));
	RequireDenied(con.Query("SET statement_check_deny_select=true; SELECT 42"));
	RequireDenied(con.Query("PRAGMA statement_check_expand('INSERT INTO effects VALUES (1); SELECT 42')"));
	REQUIRE_NO_FAIL(con.Query("SET statement_check_deny_select=false"));
	auto result = con.Query("SELECT count(*) FROM effects");
	REQUIRE(CHECK_COLUMN(result, 0, {0}));
	const auto calls = fixture.check->calls;
	REQUIRE_NO_FAIL(con.Query("PRAGMA statement_check_expand('')"));
	REQUIRE(fixture.check->calls > calls);
}

TEST_CASE("Statement rejection precedes EXECUTE argument evaluation", "[api][statement_check]") {
	StatementCheckFixture fixture;
	auto &con = fixture.con;
	REQUIRE_NO_FAIL(con.Query("CREATE SEQUENCE effects"));
	REQUIRE_NO_FAIL(con.Query("PREPARE p AS SELECT $1::BIGINT"));
	fixture.check->denied_type = StatementType::EXECUTE_STATEMENT;
	RequireDenied(con.Query("EXECUTE p(nextval('effects'))"));
	auto result = con.Query("SELECT nextval('effects')");
	REQUIRE(CHECK_COLUMN(result, 0, {1}));
}

TEST_CASE("Fatal statement check errors invalidate the database during inspection", "[api][statement_check]") {
	StatementCheckFixture fixture;
	fixture.check->fatal = true;
	SECTION("Table names") {
		REQUIRE_THROWS_WITH(fixture.con.GetTableNames("SELECT 42"), Contains("statement check failed"));
	}
	SECTION("Plan") {
		REQUIRE_THROWS_WITH(fixture.con.ExtractPlan("SELECT 42"), Contains("statement check failed"));
	}
	fixture.check->fatal = false;
	auto result = fixture.con.Query("SELECT 42");
	REQUIRE_FAIL(result);
	REQUIRE_THAT(result->GetError(), Contains("database has been invalidated"));
}
