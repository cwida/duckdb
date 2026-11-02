#include "catch.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

namespace {

//! Commits rows through another connection while the verifier is between planning and executing the generated SQL
class ConcurrentInsertOnQueryEnd : public ClientContextState {
public:
	explicit ConcurrentInsertOnQueryEnd(Connection &writer) : writer(writer) {
	}

	void QueryEnd(ClientContext &, optional_ptr<ErrorData>) override {
		if (!armed) {
			return;
		}
		armed = false;
		auto result = writer.Query("INSERT INTO versioned_keys VALUES (11), (13)");
		insert_succeeded = !result->HasError();
	}

	Connection &writer;
	bool armed = false;
	bool insert_succeeded = false;
};

} // namespace

TEST_CASE("SQL export verification executes the generated statement under the planning snapshot",
          "[sql_export][statement_verification]") {
	DuckDB db(nullptr);
	Connection writer(db);
	Connection reader(db);
	REQUIRE_NO_FAIL(writer.Query("CREATE TABLE versioned_keys(key INTEGER PRIMARY KEY)"));
	REQUIRE_NO_FAIL(writer.Query("INSERT INTO versioned_keys VALUES (12)"));
	REQUIRE_NO_FAIL(reader.Query("SET debug_verify_statement='explain_sql_strict'"));

	// With a single committed key the optimizer proves the filter always true and the generated SQL drops it
	auto state = make_shared_ptr<ConcurrentInsertOnQueryEnd>(writer);
	reader.context->registered_state->Insert("concurrent_insert", state);
	state->armed = true;
	auto result = reader.Query("SELECT key FROM versioned_keys WHERE key >= 12 ORDER BY key");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(!state->armed);
	REQUIRE(state->insert_succeeded);
	REQUIRE(CHECK_COLUMN(result, 0, {12}));

	// The verification transaction is complete: the new rows are visible and no transaction is left open
	result = reader.Query("SELECT key FROM versioned_keys WHERE key >= 12 ORDER BY key");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(CHECK_COLUMN(result, 0, {12, 13}));
	REQUIRE_NO_FAIL(reader.Query("BEGIN"));
	REQUIRE_NO_FAIL(reader.Query("ROLLBACK"));

	// A failing verification EXPLAIN leaves no transaction behind either
	result = reader.Query("SELECT key FROM missing_keys");
	REQUIRE(result->HasError());
	REQUIRE_NO_FAIL(reader.Query("BEGIN"));
	REQUIRE_NO_FAIL(reader.Query("ROLLBACK"));
}
