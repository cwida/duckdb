#include "catch.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

// ToString() of a subscript or slice must parse again to the same expression. A negative constant base cannot be
// reached through SQL, since no subscript binds on a number, so the round trip is checked on the parsed statement.
static void RequireSubscriptRoundTrip(const string &query, const string &expected) {
	auto parser = Parser::GetBuiltinParser();
	parser.ParseQuery(query);
	REQUIRE(parser.statements.size() == 1);
	auto &statement = parser.statements[0]->Cast<SelectStatement>();
	REQUIRE(statement.ToString() == expected);

	auto reparser = Parser::GetBuiltinParser();
	reparser.ParseQuery(statement.ToString());
	REQUIRE(reparser.statements.size() == 1);
	REQUIRE(reparser.statements[0]->Cast<SelectStatement>().Equals(statement));
}

TEST_CASE("ToString keeps the parentheses around the base of a subscript", "[api][parser]") {
	RequireSubscriptRoundTrip("SELECT ('abc' COLLATE nocase)[1]", "SELECT ('abc' COLLATE nocase)[1]");
	RequireSubscriptRoundTrip("SELECT ('abcd' COLLATE nocase)[2:3]", "SELECT ('abcd' COLLATE nocase)[2:3]");
	RequireSubscriptRoundTrip("SELECT ('abcd' COLLATE nocase)[2:-1:2]", "SELECT ('abcd' COLLATE nocase)[2:-1:2]");
	RequireSubscriptRoundTrip("SELECT (-1)[1]", "SELECT (-1)[1]");
	RequireSubscriptRoundTrip("SELECT (-1.5)[1:2]", "SELECT (-1.5)[1:2]");
	// bases that already print in a form that parses again are left alone
	RequireSubscriptRoundTrip("SELECT 'abc'[1]", "SELECT 'abc'[1]");
	RequireSubscriptRoundTrip("SELECT [1, 2][1]", "SELECT list_value(1, 2)[1]");
	RequireSubscriptRoundTrip("SELECT (-x)[1] FROM t", "SELECT (-(x))[1] FROM t");
}
