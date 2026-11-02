#include "catch.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/statement/load_statement.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

static duckdb::unique_ptr<LoadInfo> ParseLoad(const string &query) {
	auto parser = Parser::GetBuiltinParser();
	parser.ParseQuery(query);
	REQUIRE(parser.statements.size() == 1);
	REQUIRE(parser.statements[0]->type == StatementType::LOAD_STATEMENT);
	return parser.statements[0]->Cast<LoadStatement>().info->Copy();
}

TEST_CASE("Parse INSTALL / FORCE INSTALL statements", "[parse_load]") {
	REQUIRE(ParseLoad("INSTALL x")->load_type == LoadType::INSTALL);
	REQUIRE(ParseLoad("FORCE INSTALL x")->load_type == LoadType::FORCE_INSTALL);

	auto from_repo = ParseLoad("FORCE INSTALL x FROM 'some_repo'");
	REQUIRE(from_repo->load_type == LoadType::FORCE_INSTALL);
	REQUIRE(from_repo->repository == "some_repo");

	REQUIRE(ParseLoad("LOAD x")->load_type == LoadType::LOAD);
}
