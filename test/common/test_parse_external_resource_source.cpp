#include "catch.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/parsed_data/external_resource_options.hpp"
#include "duckdb/parser/statement/attach_statement.hpp"
#include "duckdb/parser/statement/connect_statement.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

TEST_CASE("Parse ATTACH TO NEW TEMPORARY EXTERNAL RESOURCE + ToString roundtrip", "[parse_external_resource]") {
	auto parser = Parser::GetBuiltinParser();
	parser.ParseQuery(
	    "ATTACH TO NEW TEMPORARY EXTERNAL RESOURCE 'quack@local' (INSTANCE 'r7i.16xlarge', REGION 'eu-west-1') "
	    "AS my_db (READ_ONLY)");
	REQUIRE(parser.statements.size() == 1);
	REQUIRE(parser.statements[0]->type == StatementType::ATTACH_STATEMENT);

	auto &attach = parser.statements[0]->Cast<AttachStatement>();
	REQUIRE(attach.info->external_resource != nullptr);
	auto &er = *attach.info->external_resource;
	// Create form: the type is separated from the create params; not a reference.
	REQUIRE(er.reference_name.empty());
	// The type is a string literal, so the parser resolves it directly — no bind step.
	REQUIRE(er.provider == "quack@local");
	REQUIRE(er.parsed_params.size() == 2);
	REQUIRE(er.parsed_params.find("INSTANCE") != er.parsed_params.end());
	REQUIRE(er.parsed_params.find("REGION") != er.parsed_params.end());
	// the attach alias is the database name.
	REQUIRE(attach.info->name.GetIdentifierName() == "my_db");

	// ToString renders back to the ATTACH TO NEW TEMPORARY EXTERNAL RESOURCE surface.
	auto str = attach.info->ToString();
	REQUIRE(StringUtil::Contains(str, "ATTACH TO NEW TEMPORARY EXTERNAL RESOURCE"));
	REQUIRE(StringUtil::Contains(str, "quack@local"));
	REQUIRE(StringUtil::Contains(str, "AS my_db"));

	// Roundtrip: re-parsing the rendered SQL yields the same statement + resource.
	auto reparser = Parser::GetBuiltinParser();
	reparser.ParseQuery(str);
	REQUIRE(reparser.statements.size() == 1);
	REQUIRE(reparser.statements[0]->type == StatementType::ATTACH_STATEMENT);
	auto &re = reparser.statements[0]->Cast<AttachStatement>();
	REQUIRE(re.info->external_resource != nullptr);
	REQUIRE(re.info->external_resource->parsed_params.size() == 2);
	REQUIRE(re.info->name.GetIdentifierName() == "my_db");
	REQUIRE(StringUtil::Contains(re.info->ToString(), "ATTACH TO NEW TEMPORARY EXTERNAL RESOURCE"));
}

TEST_CASE("Parse ATTACH TO EXTERNAL RESOURCE (reference) + ToString roundtrip", "[parse_external_resource]") {
	auto parser = Parser::GetBuiltinParser();
	parser.ParseQuery("ATTACH TO EXTERNAL RESOURCE beefy AS my_db");
	REQUIRE(parser.statements.size() == 1);
	REQUIRE(parser.statements[0]->type == StatementType::ATTACH_STATEMENT);

	auto &attach = parser.statements[0]->Cast<AttachStatement>();
	REQUIRE(attach.info->external_resource != nullptr);
	auto &er = *attach.info->external_resource;
	// Reference form: a bare identifier names a registered resource; no type/params.
	REQUIRE(er.reference_name == "beefy");
	REQUIRE(er.provider.empty());
	REQUIRE(er.parsed_params.empty());
	REQUIRE(attach.info->name.GetIdentifierName() == "my_db");

	auto str = attach.info->ToString();
	REQUIRE(StringUtil::Contains(str, "ATTACH TO EXTERNAL RESOURCE beefy"));
	REQUIRE(!StringUtil::Contains(str, "CREATE"));

	auto reparser = Parser::GetBuiltinParser();
	reparser.ParseQuery(str);
	REQUIRE(reparser.statements.size() == 1);
	auto &re = reparser.statements[0]->Cast<AttachStatement>();
	REQUIRE(re.info->external_resource->reference_name == "beefy");
}

TEST_CASE("Parse CONNECT TO NEW TEMPORARY EXTERNAL RESOURCE + ToString roundtrip", "[parse_external_resource]") {
	auto parser = Parser::GetBuiltinParser();
	parser.ParseQuery("CONNECT TO NEW TEMPORARY EXTERNAL RESOURCE 'quack@local' (region 'eu-west-1')");
	REQUIRE(parser.statements.size() == 1);
	REQUIRE(parser.statements[0]->type == StatementType::CONNECT_STATEMENT);

	auto &connect = parser.statements[0]->Cast<ConnectStatement>();
	REQUIRE(connect.info->external_resource != nullptr);
	REQUIRE(connect.info->external_resource->parsed_params.size() == 1);

	auto str = connect.info->ToString();
	REQUIRE(StringUtil::Contains(str, "CONNECT TO NEW TEMPORARY EXTERNAL RESOURCE"));
	REQUIRE(StringUtil::Contains(str, "quack@local"));

	auto reparser = Parser::GetBuiltinParser();
	reparser.ParseQuery(str);
	REQUIRE(reparser.statements.size() == 1);
	REQUIRE(reparser.statements[0]->type == StatementType::CONNECT_STATEMENT);
	REQUIRE(reparser.statements[0]->Cast<ConnectStatement>().info->external_resource != nullptr);
	REQUIRE(reparser.statements[0]->Cast<ConnectStatement>().info->ToString() == str);
}

TEST_CASE("ATTACH TO EXTERNAL RESOURCE ToString stays reparseable", "[parse_external_resource]") {
	auto parser = Parser::GetBuiltinParser();
	parser.ParseQuery("ATTACH TO NEW TEMPORARY EXTERNAL RESOURCE 'quack@local' (\"weird param\" 'v') "
	                  "AS my_db (\"weird option\" 'w')");
	REQUIRE(parser.statements.size() == 1);
	auto &attach = parser.statements[0]->Cast<AttachStatement>();

	// Option names that need quoting keep their quotes, so the rendering can be parsed back.
	auto str = attach.info->ToString();
	REQUIRE(StringUtil::Contains(str, "\"weird param\""));
	REQUIRE(StringUtil::Contains(str, "\"weird option\""));
	auto reparser = Parser::GetBuiltinParser();
	reparser.ParseQuery(str);
	REQUIRE(reparser.statements.size() == 1);
	REQUIRE(reparser.statements[0]->type == StatementType::ATTACH_STATEMENT);

	// The grammar has no IF NOT EXISTS / OR REPLACE slot for this form, so ToString must not render one
	// even if the field says otherwise - doing so would emit SQL that no longer parses.
	attach.info->on_conflict = OnCreateConflict::IGNORE_ON_CONFLICT;
	auto ignore_str = attach.info->ToString();
	REQUIRE(!StringUtil::Contains(ignore_str, "IF NOT EXISTS"));
	auto ignore_parser = Parser::GetBuiltinParser();
	ignore_parser.ParseQuery(ignore_str);
	REQUIRE(ignore_parser.statements.size() == 1);

	attach.info->on_conflict = OnCreateConflict::REPLACE_ON_CONFLICT;
	auto replace_str = attach.info->ToString();
	REQUIRE(!StringUtil::Contains(replace_str, "OR REPLACE"));
	auto replace_parser = Parser::GetBuiltinParser();
	replace_parser.ParseQuery(replace_str);
	REQUIRE(replace_parser.statements.size() == 1);
}

TEST_CASE("CONNECT TO EXTERNAL RESOURCE option lists", "[parse_external_resource]") {
	// Provisioning: the create clause claims the list, so it is the recipe's.
	auto provision = Parser::GetBuiltinParser();
	provision.ParseQuery("CONNECT TO NEW TEMPORARY EXTERNAL RESOURCE 'quack@local' (region 'eu-west-1')");
	REQUIRE(provision.statements.size() == 1);
	auto &connect = provision.statements[0]->Cast<ConnectStatement>();
	REQUIRE(connect.info->external_resource->parsed_params.size() == 1);
	REQUIRE(connect.info->parsed_options.empty());
	REQUIRE(connect.info->options.empty());

	// Borrowing: a registered resource takes no create params, so the list is unambiguously the
	// connection's and is accepted.
	auto borrow = Parser::GetBuiltinParser();
	borrow.ParseQuery("CONNECT TO EXTERNAL RESOURCE beefy (token 'abc')");
	REQUIRE(borrow.statements.size() == 1);
	auto &borrowed = borrow.statements[0]->Cast<ConnectStatement>();
	REQUIRE(borrowed.info->external_resource->reference_name == "beefy");
	REQUIRE(borrowed.info->parsed_options.size() + borrowed.info->options.size() == 1);
	// ...and ToString keeps them, so the rendering still round-trips.
	auto borrowed_str = borrowed.info->ToString();
	REQUIRE(StringUtil::Contains(borrowed_str, "token"));
	auto borrow_again = Parser::GetBuiltinParser();
	borrow_again.ParseQuery(borrowed_str);
	REQUIRE(borrow_again.statements.size() == 1);
	auto &again = borrow_again.statements[0]->Cast<ConnectStatement>();
	REQUIRE(again.info->external_resource->reference_name == "beefy");
	REQUIRE(again.info->parsed_options.size() + again.info->options.size() == 1);

	// Only the genuinely ambiguous spelling is refused: a second list while provisioning could be told
	// apart from the first by position alone.
	auto rejected = Parser::GetBuiltinParser();
	REQUIRE_THROWS(
	    rejected.ParseQuery("CONNECT TO NEW TEMPORARY EXTERNAL RESOURCE 'quack@local' (region 'x') (token 'abc')"));
}
