#include "duckdb/parser/parser.hpp"

#include "duckdb/parser/transformer.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/statement/update_statement.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/tableref/expressionlistref.hpp"
#include "postgres_parser.hpp"
#include "duckdb/parser/query_error_context.hpp"

#include "parser/parser.hpp"

namespace duckdb {

Parser::Parser() {
}

void Parser::ParseQuery(const string &query) {
	Transformer transformer;
	{
		PostgresParser parser;
		parser.Parse(query);

		if (!parser.success) {
			throw ParserException(QueryErrorContext::Format(query, parser.error_message, parser.error_location - 1));
		}

		if (!parser.parse_tree) {
			// empty statement
			return;
		}

		// if it succeeded, we transform the Postgres parse tree into a list of
		// SQLStatements
		transformer.TransformParseTree(parser.parse_tree, statements);
	}
	if (!statements.empty()) {
		auto &last_statement = statements.back();
		last_statement->stmt_length = query.size() - last_statement->stmt_location;
		for (auto &statement : statements) {
			statement->query = query;
			if (statement->type == StatementType::CREATE_STATEMENT) {
				auto &create = (CreateStatement &)*statement;
				create.info->sql = query.substr(statement->stmt_location, statement->stmt_length);
			}
		}
	}
}

vector<SimplifiedToken> Parser::Tokenize(const string &query) {
	auto pg_tokens = PostgresParser::Tokenize(query);
	vector<SimplifiedToken> result;
	result.reserve(pg_tokens.size());
	for (auto &pg_token : pg_tokens) {
		SimplifiedToken token;
		switch (pg_token.type) {
		case duckdb_libpgquery::PGSimplifiedTokenType::PG_SIMPLIFIED_TOKEN_IDENTIFIER:
			token.type = SimplifiedTokenType::SIMPLIFIED_TOKEN_IDENTIFIER;
			break;
		case duckdb_libpgquery::PGSimplifiedTokenType::PG_SIMPLIFIED_TOKEN_NUMERIC_CONSTANT:
			token.type = SimplifiedTokenType::SIMPLIFIED_TOKEN_NUMERIC_CONSTANT;
			break;
		case duckdb_libpgquery::PGSimplifiedTokenType::PG_SIMPLIFIED_TOKEN_STRING_CONSTANT:
			token.type = SimplifiedTokenType::SIMPLIFIED_TOKEN_STRING_CONSTANT;
			break;
		case duckdb_libpgquery::PGSimplifiedTokenType::PG_SIMPLIFIED_TOKEN_OPERATOR:
			token.type = SimplifiedTokenType::SIMPLIFIED_TOKEN_OPERATOR;
			break;
		case duckdb_libpgquery::PGSimplifiedTokenType::PG_SIMPLIFIED_TOKEN_KEYWORD:
			token.type = SimplifiedTokenType::SIMPLIFIED_TOKEN_KEYWORD;
			break;
		// comments are not supported by our tokenizer right now
		case duckdb_libpgquery::PGSimplifiedTokenType::PG_SIMPLIFIED_TOKEN_COMMENT: // LCOV_EXCL_START
			token.type = SimplifiedTokenType::SIMPLIFIED_TOKEN_COMMENT;
			break;
		} // LCOV_EXCL_STOP
		token.start = pg_token.start;
		result.push_back(token);
	}
	return result;
}

bool Parser::IsKeyword(const string &text) {
	return PostgresParser::IsKeyword(text);
}

vector<unique_ptr<ParsedExpression>> Parser::ParseExpressionList(const string &select_list) {
	// construct a mock query prefixed with SELECT
	string mock_query = "SELECT " + select_list;
	// parse the query
	Parser parser;
	parser.ParseQuery(mock_query);
	// check the statements
	if (parser.statements.size() != 1 || parser.statements[0]->type != StatementType::SELECT_STATEMENT) {
		throw ParserException("Expected a single SELECT statement");
	}
	auto &select = (SelectStatement &)*parser.statements[0];
	if (select.node->type != QueryNodeType::SELECT_NODE) {
		throw ParserException("Expected a single SELECT node");
	}
	auto &select_node = (SelectNode &)*select.node;
	return move(select_node.select_list);
}

vector<OrderByNode> Parser::ParseOrderList(const string &select_list) {
	// construct a mock query
	string mock_query = "SELECT * FROM tbl ORDER BY " + select_list;
	// parse the query
	Parser parser;
	parser.ParseQuery(mock_query);
	// check the statements
	if (parser.statements.size() != 1 || parser.statements[0]->type != StatementType::SELECT_STATEMENT) {
		throw ParserException("Expected a single SELECT statement");
	}
	auto &select = (SelectStatement &)*parser.statements[0];
	if (select.node->type != QueryNodeType::SELECT_NODE) {
		throw InternalException("Expected a single SELECT node");
	}
	auto &select_node = (SelectNode &)*select.node;
	if (select_node.modifiers.empty() || select_node.modifiers[0]->type != ResultModifierType::ORDER_MODIFIER ||
	    select_node.modifiers.size() != 1) {
		throw InternalException("Expected a single ORDER clause");
	}
	auto &order = (OrderModifier &)*select_node.modifiers[0];
	return move(order.orders);
}

void Parser::ParseUpdateList(const string &update_list, vector<string> &update_columns,
                             vector<unique_ptr<ParsedExpression>> &expressions) {
	// construct a mock query
	string mock_query = "UPDATE tbl SET " + update_list;
	// parse the query
	Parser parser;
	parser.ParseQuery(mock_query);
	// check the statements
	if (parser.statements.size() != 1 || parser.statements[0]->type != StatementType::UPDATE_STATEMENT) {
		throw ParserException("Expected a single UPDATE statement");
	}
	auto &update = (UpdateStatement &)*parser.statements[0];
	update_columns = move(update.columns);
	expressions = move(update.expressions);
}

vector<vector<unique_ptr<ParsedExpression>>> Parser::ParseValuesList(const string &value_list) {
	// construct a mock query
	string mock_query = "VALUES " + value_list;
	// parse the query
	Parser parser;
	parser.ParseQuery(mock_query);
	// check the statements
	if (parser.statements.size() != 1 || parser.statements[0]->type != StatementType::SELECT_STATEMENT) {
		throw ParserException("Expected a single SELECT statement");
	}
	auto &select = (SelectStatement &)*parser.statements[0];
	if (select.node->type != QueryNodeType::SELECT_NODE) {
		throw ParserException("Expected a single SELECT node");
	}
	auto &select_node = (SelectNode &)*select.node;
	if (!select_node.from_table || select_node.from_table->type != TableReferenceType::EXPRESSION_LIST) {
		throw InternalException("Expected a single VALUES statement");
	}
	auto &values_list = (ExpressionListRef &)*select_node.from_table;
	return move(values_list.values);
}

vector<ColumnDefinition> Parser::ParseColumnList(const string &column_list) {
	string mock_query = "CREATE TABLE blabla (" + column_list + ")";
	Parser parser;
	parser.ParseQuery(mock_query);
	if (parser.statements.size() != 1 || parser.statements[0]->type != StatementType::CREATE_STATEMENT) {
		throw ParserException("Expected a single CREATE statement");
	}
	auto &create = (CreateStatement &)*parser.statements[0];
	if (create.info->type != CatalogType::TABLE_ENTRY) {
		throw InternalException("Expected a single CREATE TABLE statement");
	}
	auto &info = ((CreateTableInfo &)*create.info);
	return move(info.columns);
}

} // namespace duckdb
