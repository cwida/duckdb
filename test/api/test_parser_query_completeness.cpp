#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/grammar_extension.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/peg/parsed_grammar.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/emptytableref.hpp"

using namespace duckdb;

static bool QueryIsComplete(Connection &con, const string &query) {
	Parser parser(*con.context);
	return parser.IsQueryComplete(query);
}

TEST_CASE("Parser::IsQueryComplete follows the default grammar", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);

	// complete, semicolon-terminated statement sequences
	REQUIRE(QueryIsComplete(con, "SELECT 1;"));
	REQUIRE(QueryIsComplete(con, "SELECT 1; SELECT 2;"));
	REQUIRE(QueryIsComplete(con, ";"));
	REQUIRE(QueryIsComplete(con, "SELECT 1;;"));
	REQUIRE(QueryIsComplete(con, "SELECT 1; -- trailing comment"));
	REQUIRE(QueryIsComplete(con, "SELECT 1; /* trailing comment */"));
	REQUIRE(QueryIsComplete(con, "SELECT 'a;b';"));
	REQUIRE(QueryIsComplete(con, "SELECT $$a;b$$;"));
	REQUIRE(QueryIsComplete(con, "SELECT \"a;b\" FROM tbl;"));
	REQUIRE(QueryIsComplete(con, "BEGIN;"));
	REQUIRE(QueryIsComplete(con, "BEGIN TRANSACTION;"));
	REQUIRE(QueryIsComplete(con, "END;"));

	// input the grammar rejects mid-way counts as complete, so the error surfaces on execution
	REQUIRE(QueryIsComplete(con, "SELEC 1;"));
	REQUIRE(QueryIsComplete(con, "SELECT FROM WHERE;"));
	REQUIRE(QueryIsComplete(con, "SELECT (1;"));
	REQUIRE(QueryIsComplete(con, "CREATE FUNCTION f(n INTEGER) RETURNS INTEGER AS BEGIN SELECT 1;"));
	// invalid UTF-8 is submitted as well
	REQUIRE(QueryIsComplete(con, "SELECT '\xff\xff';"));
	// a zero-length delimited identifier is a fatal lexical error - no continuation can repair it
	REQUIRE(QueryIsComplete(con, "SELECT \"\";"));
	REQUIRE(QueryIsComplete(con, "SELECT \"\""));
	// a malformed numeric literal makes the matcher throw - contained and classified as complete
	REQUIRE(QueryIsComplete(con, "SELECT 1e2e3;"));

	// incomplete inputs that more input can extend
	REQUIRE_FALSE(QueryIsComplete(con, ""));
	REQUIRE_FALSE(QueryIsComplete(con, "   "));
	REQUIRE_FALSE(QueryIsComplete(con, "SELECT 1"));
	REQUIRE_FALSE(QueryIsComplete(con, "SELECT 1; SELECT 2"));
	REQUIRE_FALSE(QueryIsComplete(con, "SELECT 'a;"));
	REQUIRE_FALSE(QueryIsComplete(con, "SELECT \"a;"));
	REQUIRE_FALSE(QueryIsComplete(con, "SELECT $$a;"));
	REQUIRE_FALSE(QueryIsComplete(con, "SELECT $tag$a;$other$;"));
	REQUIRE_FALSE(QueryIsComplete(con, "SELECT 1; /* unterminated"));
	REQUIRE_FALSE(QueryIsComplete(con, "SELECT 1 -- comment"));
}

static idx_t completeness_block_transform_count = 0;

static arena_ptr<TransformResultValue> FinalizeCompletenessBlock(PEGTransformer &transformer,
                                                                 ParseResult &parse_result) {
	completeness_block_transform_count++;
	auto select_node = make_uniq<SelectNode>();
	select_node->select_list.push_back(ConstantExpression::String("block"));
	select_node->from_table = make_uniq<EmptyTableRef>();
	auto statement = make_uniq<SelectStatement>();
	statement->node = std::move(select_node);
	return transformer.MakeResult<unique_ptr<SQLStatement>>(std::move(statement));
}

static arena_ptr<TransformProcess> StartCompletenessBlockTransform(PEGTransformer &transformer,
                                                                   ParseResult &parse_result) {
	return transformer.MakeProcess<FinalizeTransformProcess>(transformer, parse_result, FinalizeCompletenessBlock);
}

class CompletenessBlockGrammar final : public GrammarExtension {
public:
	CompletenessBlockGrammar()
	    : GrammarExtension("completeness_block_syntax", "BEGIN_TEST_BLOCK ... END_TEST_BLOCK block statement") {
	}

	vector<GrammarChange> GetChanges() const override {
		vector<GrammarChange> changes;
		changes.push_back(
		    GrammarChange::AddRule("CompletenessBlockStatement <- 'BEGIN_TEST_BLOCK' (Statement ';')* 'END_TEST_BLOCK'",
		                           StartCompletenessBlockTransform));
		changes.push_back(GrammarChange::PrependChoice("Statement", "CompletenessBlockStatement"));
		return changes;
	}
};

TEST_CASE("Parser::IsQueryComplete follows grammar extensions", "[api]") {
	DuckDB db(nullptr);
	GrammarExtension::Register(*db.instance, make_shared_ptr<CompletenessBlockGrammar>());
	Connection con(db);

	// the extension is not active - the input is rejected mid-way and counts as complete
	REQUIRE(QueryIsComplete(con, "BEGIN_TEST_BLOCK SELECT 1;"));

	REQUIRE_NO_FAIL(*con.Query("SET active_grammar_extensions = ['completeness_block_syntax']"));

	// an open block asks for more input until it is closed and terminated
	REQUIRE_FALSE(QueryIsComplete(con, "BEGIN_TEST_BLOCK SELECT 1;"));
	REQUIRE_FALSE(QueryIsComplete(con, "BEGIN_TEST_BLOCK SELECT 1; SELECT 2;"));
	REQUIRE_FALSE(QueryIsComplete(con, "BEGIN_TEST_BLOCK SELECT 1; END_TEST_BLOCK"));
	REQUIRE(QueryIsComplete(con, "BEGIN_TEST_BLOCK SELECT 1; END_TEST_BLOCK;"));
	REQUIRE(QueryIsComplete(con, "BEGIN_TEST_BLOCK END_TEST_BLOCK;"));

	// a fatal lexical error inside an open block still submits - closing the block cannot repair it
	REQUIRE(QueryIsComplete(con, "BEGIN_TEST_BLOCK SELECT \"\";"));

	// the completeness check matches without transforming
	REQUIRE(completeness_block_transform_count == 0);

	// the block parses and executes as one statement, now invoking the transformer
	auto result = con.Query("BEGIN_TEST_BLOCK SELECT 1; SELECT 2; END_TEST_BLOCK;");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Collection().GetValue(0, 0) == Value("block"));
	REQUIRE(completeness_block_transform_count > 0);
}
