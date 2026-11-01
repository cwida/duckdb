#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/peg/compiled_grammar.hpp"
#include "duckdb/parser/peg/keyword_helper/duckdb_keyword_helper.hpp"

#include "duckdb/main/extension_callback_manager.hpp"
#include "duckdb/parser/group_by_node.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/parser/parser_extension.hpp"
#include "duckdb/parser/token_iterator.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/parser/statement/extension_statement.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/statement/update_statement.hpp"
#include "duckdb/parser/query_node/update_query_node.hpp"
#include "duckdb/parser/tableref/expressionlistref.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/peg/syntax_error.hpp"
#include "duckdb/parser/peg/tokenizer/parser_tokenizer.hpp"
#include "duckdb/parser/peg/tokenizer/highlight_tokenizer.hpp"
#include "utf8proc_wrapper.hpp"

namespace duckdb {

Parser::Parser(const ParserOptions &options_p) : options(options_p) {
}

Parser::~Parser() = default;
Parser::Parser(Parser &&other) noexcept = default;

ParserOptions ParserOptions::Builtin() {
	ParserOptions options;
	options.compiled_grammar = CompiledGrammar::DefaultGrammar();
	return options;
}

Parser Parser::GetBuiltinParser() {
	return Parser(ParserOptions::Builtin());
}

CompiledGrammar &Parser::GetGrammar() {
	if (!compiled_grammar) {
		if (options.compiled_grammar) {
			compiled_grammar = options.compiled_grammar;
		} else {
			throw InternalException("ParserOptions requires a compiled grammar");
		}
	}
	return *compiled_grammar;
}

static bool ReplaceUnicodeSpaces(const string &query, string &new_query, vector<UnicodeSpace> &unicode_spaces) {
	if (unicode_spaces.empty()) {
		// no unicode spaces found
		return false;
	}
	idx_t prev = 0;
	for (auto &usp : unicode_spaces) {
		new_query += query.substr(prev, usp.pos - prev);
		// Keep byte offsets aligned with the original query for diagnostics.
		new_query.append(usp.bytes, ' ');
		prev = usp.pos + usp.bytes;
	}
	new_query += query.substr(prev, query.size() - prev);
	return true;
}

static bool IsValidDollarQuotedStringTagFirstChar(const unsigned char &c) {
	// the first character can be between A-Z, a-z, underscore, or \200 - \377
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c >= 0x80;
}

static bool IsValidDollarQuotedStringTagSubsequentChar(const unsigned char &c) {
	// subsequent characters can also be between 0-9
	return IsValidDollarQuotedStringTagFirstChar(c) || (c >= '0' && c <= '9');
}

//! Throw a ParserException if `query` contains invalid UTF-8, so the tokenizer never reads past
//! bad bytes (a bad-byte query can otherwise recurse the tokenizer — see ossfuzz clusterfuzz-test-24).
static UnicodeType ValidateUTF8Query(const string &query) {
	UnicodeInvalidReason reason = UnicodeInvalidReason::INVALID_UNICODE;
	size_t invalid_pos = 0;
	auto unicode_type = Utf8Proc::Analyze(query.c_str(), query.size(), &reason, &invalid_pos);
	if (unicode_type != UnicodeType::INVALID) {
		return unicode_type;
	}
	const char *reason_str =
	    reason == UnicodeInvalidReason::BYTE_MISMATCH ? "byte sequence mismatch" : "invalid unicode";
	throw ParserException::SyntaxError(query, StringUtil::Format("Invalid UTF-8 in query (%s)", reason_str),
	                                   optional_idx(NumericCast<idx_t>(invalid_pos)));
}

// This function strips unicode space characters from the query and replaces them with regular spaces
// It returns true if any unicode space characters were found and stripped
// See here for a list of unicode space characters - https://jkorpela.fi/chars/spaces.html
bool Parser::StripUnicodeSpaces(const string &query_str, string &new_query) {
	const idx_t NBSP_LEN = 2;
	const idx_t USP_LEN = 3;
	idx_t pos = 0;
	unsigned char quote;
	string_t dollar_quote_tag;
	vector<UnicodeSpace> unicode_spaces;
	auto query = const_uchar_ptr_cast(query_str.c_str());
	auto qsize = query_str.size();

regular:
	for (; pos + 2 < qsize; pos++) {
		if (query[pos] == 0xC2) {
			if (query[pos + 1] == 0xA0) {
				// U+00A0 - C2A0
				unicode_spaces.emplace_back(pos, NBSP_LEN);
			}
		}
		if (query[pos] == 0xE2) {
			if (query[pos + 1] == 0x80) {
				if (query[pos + 2] >= 0x80 && query[pos + 2] <= 0x8B) {
					// U+2000 to U+200B
					// E28080 - E2808B
					unicode_spaces.emplace_back(pos, USP_LEN);
				} else if (query[pos + 2] == 0xAF) {
					// U+202F - E280AF
					unicode_spaces.emplace_back(pos, USP_LEN);
				}
			} else if (query[pos + 1] == 0x81) {
				if (query[pos + 2] == 0x9F) {
					// U+205F - E2819f
					unicode_spaces.emplace_back(pos, USP_LEN);
				} else if (query[pos + 2] == 0xA0) {
					// U+2060 - E281A0
					unicode_spaces.emplace_back(pos, USP_LEN);
				}
			}
		} else if (query[pos] == 0xE3) {
			if (query[pos + 1] == 0x80 && query[pos + 2] == 0x80) {
				// U+3000 - E38080
				unicode_spaces.emplace_back(pos, USP_LEN);
			}
		} else if (query[pos] == 0xEF) {
			if (query[pos + 1] == 0xBB && query[pos + 2] == 0xBF) {
				// U+FEFF - EFBBBF
				unicode_spaces.emplace_back(pos, USP_LEN);
			}
		} else if (query[pos] == '"' || query[pos] == '\'') {
			quote = query[pos];
			pos++;
			goto in_quotes;
		} else if (query[pos] == '$' &&
		           (query[pos + 1] == '$' || IsValidDollarQuotedStringTagFirstChar(query[pos + 1]))) {
			// (optionally tagged) dollar-quoted string
			auto start = &query[++pos];
			for (; pos + 2 < qsize; pos++) {
				if (query[pos] == '$') {
					// end of tag
					dollar_quote_tag =
					    string_t(const_char_ptr_cast(start), NumericCast<uint32_t, int64_t>(&query[pos] - start));
					goto in_dollar_quotes;
				}

				if (!IsValidDollarQuotedStringTagSubsequentChar(query[pos])) {
					// invalid char in dollar-quoted string, continue as normal
					goto regular;
				}
			}
			goto end;
		} else if (query[pos] == '-' && query[pos + 1] == '-') {
			goto in_comment;
		}
	}
	goto end;
in_quotes:
	for (; pos + 1 < qsize; pos++) {
		if (query[pos] == quote) {
			if (query[pos + 1] == quote) {
				// escaped quote
				pos++;
				continue;
			}
			pos++;
			goto regular;
		}
	}
	goto end;
in_dollar_quotes:
	for (; pos + 2 < qsize; pos++) {
		if (query[pos] == '$' &&
		    qsize - (pos + 1) >= dollar_quote_tag.GetSize() + 1 && // found '$' and enough space left
		    query[pos + dollar_quote_tag.GetSize() + 1] == '$' &&  // ending '$' at the right spot
		    memcmp(&query[pos + 1], dollar_quote_tag.GetData(), dollar_quote_tag.GetSize()) == 0) { // tags match
			pos += dollar_quote_tag.GetSize() + 1;
			goto regular;
		}
	}
	goto end;
in_comment:
	for (; pos < qsize; pos++) {
		if (query[pos] == '\n' || query[pos] == '\r') {
			goto regular;
		}
	}
	goto end;
end:
	return ReplaceUnicodeSpaces(query_str, new_query, unicode_spaces);
}

// vector<string> SplitQueries(const string &input_query) {
// 	vector<string> queries;
// 	auto tokenized_input = Parser::Tokenize(input_query);
// 	size_t last_split = 0;
//
// 	for (const auto &token : tokenized_input) {
// 		if (token.type == SimplifiedTokenType::SIMPLIFIED_TOKEN_OPERATOR && input_query[token.start] == ';') {
// 			string segment = input_query.substr(last_split, token.start - last_split);
// 			StringUtil::Trim(segment);
// 			if (!segment.empty()) {
// 				segment.append(";");
// 				queries.push_back(std::move(segment));
// 			}
// 			last_split = token.start + 1;
// 		}
// 	}
// 	string final_segment = input_query.substr(last_split);
// 	StringUtil::Trim(final_segment);
// 	if (!final_segment.empty()) {
// 		queries.push_back(std::move(final_segment));
// 	}
// 	return queries;
// }

void Parser::ThrowParserOverrideError(ParserOverrideResult &result) {
	if (result.type == ParserExtensionResultType::DISPLAY_ORIGINAL_ERROR) {
		throw ParserException("Parser override failed to return a valid statement: %s\n\nConsider restarting the "
		                      "database and "
		                      "using the setting \"set allow_parser_override_extension=fallback\" to fallback to the "
		                      "default parser.",
		                      result.error.RawMessage());
	}
	if (result.type == ParserExtensionResultType::DISPLAY_EXTENSION_ERROR) {
		result.error.Throw();
	}
}

string Parser::NormalizeSQLString(const string &query) {
	// Validate before strip: StripUnicodeSpaces walks multi-byte sequences and assumes valid UTF-8.
	if (ValidateUTF8Query(query) == UnicodeType::ASCII) {
		return query;
	}
	string normalized;
	if (StripUnicodeSpaces(query, normalized)) {
		return normalized;
	}
	return query;
}

void Parser::ParseQuery(const string &query_p) {
	const string query = NormalizeSQLString(query_p);
	if (options.extensions) {
		bool has_strict_extension_error = false;
		ErrorData last_strict_extension_error;
		for (auto &ext : options.extensions->ParserExtensions()) {
			if (!ext.parser_override) {
				continue;
			}
			if (options.parser_override_setting == AllowParserOverride::DEFAULT_OVERRIDE) {
				continue;
			}
			auto result = ext.parser_override(ext.parser_info.get(), query, options);
			if (result.type == ParserExtensionResultType::PARSE_SUCCESSFUL) {
				statements = std::move(result.statements);
				return;
			}
			if (options.parser_override_setting == AllowParserOverride::STRICT_OVERRIDE) {
				if (result.type == ParserExtensionResultType::DISPLAY_EXTENSION_ERROR) {
					has_strict_extension_error = true;
					last_strict_extension_error = std::move(result.error);
				} else {
					has_strict_extension_error = false;
				}
				continue;
			}
		}
		if (options.parser_override_setting == AllowParserOverride::STRICT_OVERRIDE && has_strict_extension_error) {
			last_strict_extension_error.Throw();
		}
	}
	// PEG parser: tokenize, then peel one TopLevelStatement at a time. On per-statement PEG
	// failure, hand the rest of the query to parse_function extensions; the extension reports
	// how many bytes it consumed and we advance the token cursor past them.
	auto owned_tokens = make_uniq<vector<MatcherToken>>();
	ParserTokenizerBehavior behavior(query, *owned_tokens);
	auto &tokenizer = GetGrammar().GetTokenizer();
	tokenizer.TokenizeInput(behavior);
	TokenIterator token_iterator(std::move(owned_tokens));
	while (token_iterator.Current()) {
		auto stmt = ParseTopLevelStatement(token_iterator, query);
		if (stmt) {
			statements.push_back(std::move(stmt));
		}
	}

	if (!statements.empty()) {
		for (idx_t i = 0; i + 1 < statements.size(); i++) {
			auto start = statements[i]->stmt_location.offset;
			statements[i]->stmt_location = QueryLocation(start, statements[i + 1]->stmt_location.offset - start);
		}
		statements.back()->stmt_location = QueryLocation(statements.back()->stmt_location.offset,
		                                                 query.size() - statements.back()->stmt_location.offset);
		for (auto &statement : statements) {
			statement->query = query.substr(statement->stmt_location.offset, statement->stmt_location.length);
			statement->stmt_location = QueryLocation(0, statement->query.size());
			if (statement->type == StatementType::CREATE_STATEMENT) {
				auto &create = statement->Cast<CreateStatement>();
				create.info->sql = statement->query;
			}
		}
	}
}

unique_ptr<SQLStatement> Parser::TryParseExtensionStatement(TokenIterator &token_iterator, const string &query) {
	if (!options.extensions || !options.extensions->HasParserExtensions()) {
		return nullptr;
	}
	auto current = token_iterator.Current();
	idx_t failure_byte = current ? current->offset : query.size();
	// SimpleToken view of the tail: text + classified type, in source order, so extensions can
	// dispatch on the token stream without re-tokenizing. The extension reports how many of these
	// tokens it consumed.
	auto simple_tokens = token_iterator.RemainingTokens();
	for (auto &ext : options.extensions->ParserExtensions()) {
		if (!ext.parse_function) {
			continue;
		}
		auto result = ext.parse_function(ext.parser_info.get(), simple_tokens);
		if (result.consumed_tokens < 0) {
			// The extension wants to surface an error.
			throw ParserException::SyntaxError(query, result.error, result.error_location);
		}
		if (result.consumed_tokens == 0) {
			// The extension ran but did not claim this input — let the next one try.
			continue;
		}
		// consumed_tokens > 0: the extension accepted that many leading tokens.
		auto consumed = NumericCast<idx_t>(result.consumed_tokens);
		if (consumed > simple_tokens.size()) {
			throw ParserException("Extension returned consumed_tokens=%llu — only %llu tokens are available",
			                      (uint64_t)consumed, (uint64_t)simple_tokens.size());
		}
		// The claimed region runs from the failure point to the end of the last consumed token;
		// advancing the cursor by consumed_tokens lands on a token boundary.
		TokenIterator consumed_iterator(token_iterator);
		consumed_iterator.Advance(consumed);
		auto &last_token = consumed_iterator.Previous();
		const idx_t end_byte = last_token.offset + last_token.length;
		auto estmt = make_uniq<ExtensionStatement>(ext, std::move(result.parse_data));
		estmt->stmt_location = QueryLocation(failure_byte, end_byte - failure_byte);
		token_iterator.SetPosition(consumed_iterator);
		return std::move(estmt);
	}
	return nullptr;
}

unique_ptr<SQLStatement> Parser::ParseTopLevelStatement(TokenIterator &token_iterator, const string &query) {
	if (!token_iterator.Current()) {
		return nullptr;
	}
	auto &compiled_grammar = GetGrammar();
	idx_t statement_start = token_iterator.Position();
	optional_idx failure_index;
	unique_ptr<SQLStatement> stmt;
	try {
		stmt =
		    PEGTransformerFactory::TransformTopLevelStatement(token_iterator, options, compiled_grammar, failure_index);
	} catch (ParserException &) {
		// the statement matched but could not be transformed - an extension may still claim it
		token_iterator.SetPosition(statement_start);
		auto ext_stmt = TryParseExtensionStatement(token_iterator, query);
		if (!ext_stmt) {
			throw;
		}
		return ext_stmt;
	}
	if (!failure_index.IsValid()) {
		return stmt;
	}
	auto ext_stmt = TryParseExtensionStatement(token_iterator, query);
	if (ext_stmt) {
		return ext_stmt;
	}
	// composing the detailed error re-parses the statement, so only do it once the error is final
	throw PEGSyntaxError::Create(token_iterator, statement_start, failure_index.GetIndex(), compiled_grammar, options);
}

vector<SimplifiedToken> Parser::Tokenize(const string &query) {
	auto &keyword_helper = DuckDBKeywordHelper::Instance();
	vector<MatcherToken> tokens;
	HighlightTokenizerBehavior behavior(query, tokens);
	Tokenizer tokenizer(keyword_helper);
	tokenizer.TokenizeInput(behavior);

	vector<SimplifiedToken> result;
	result.reserve(tokens.size());
	for (auto &token : tokens) {
		if (token.type == TokenType::END_OF_INPUT || token.type == TokenType::END_OF_INPUT_AUTOCOMPLETE) {
			continue;
		}
		SimplifiedToken simplified;
		simplified.start = token.offset;
		switch (token.type) {
		case TokenType::KEYWORD:
			simplified.type = SimplifiedTokenType::SIMPLIFIED_TOKEN_KEYWORD;
			break;
		case TokenType::IDENTIFIER:
			simplified.type = keyword_helper.IsGrammarWord(token.text)
			                      ? SimplifiedTokenType::SIMPLIFIED_TOKEN_KEYWORD
			                      : SimplifiedTokenType::SIMPLIFIED_TOKEN_IDENTIFIER;
			break;
		case TokenType::STRING_LITERAL:
			simplified.type = SimplifiedTokenType::SIMPLIFIED_TOKEN_STRING_CONSTANT;
			break;
		case TokenType::NUMBER_LITERAL:
			simplified.type = SimplifiedTokenType::SIMPLIFIED_TOKEN_NUMERIC_CONSTANT;
			break;
		case TokenType::OPERATOR:
		case TokenType::TERMINATOR:
			simplified.type = SimplifiedTokenType::SIMPLIFIED_TOKEN_OPERATOR;
			break;
		case TokenType::COMMENT:
			simplified.type = SimplifiedTokenType::SIMPLIFIED_TOKEN_COMMENT;
			break;
		default:
			simplified.type = SimplifiedTokenType::SIMPLIFIED_TOKEN_IDENTIFIER;
			break;
		}
		result.push_back(simplified);
	}
	return result;
}

vector<SimplifiedToken> Parser::TokenizeError(const string &error_msg) {
	idx_t error_start = 0;
	idx_t error_end = error_msg.size();

	vector<SimplifiedToken> tokens;
	// find "XXX Error:" - this marks the start of the error message
	auto error = StringUtil::Find(error_msg, "Error:");
	if (error.IsValid()) {
		SimplifiedToken token;
		token.type = SimplifiedTokenType::SIMPLIFIED_TOKEN_ERROR_EMPHASIS;
		token.start = 0;
		tokens.push_back(token);

		token.type = SimplifiedTokenType::SIMPLIFIED_TOKEN_ERROR;
		token.start = error.GetIndex() + 6;
		tokens.push_back(token);

		error_start = error.GetIndex() + 6;
	} else {
		SimplifiedToken token;
		token.type = SimplifiedTokenType::SIMPLIFIED_TOKEN_ERROR;
		token.start = 0;
		tokens.push_back(token);
	}

	// find "LINE (number)" - this marks the end of the message
	auto line_pos = StringUtil::Find(error_msg, "\nLINE ");
	if (line_pos.IsValid()) {
		// tokenize between
		error_end = line_pos.GetIndex();
	}

	// now iterate over the
	bool in_quotes = false;
	char quote_char = '\0';
	for (idx_t i = error_start; i < error_end; i++) {
		if (in_quotes) {
			// in a quote - look for the quote character
			if (error_msg[i] == quote_char) {
				SimplifiedToken token;
				token.start = i;
				token.type = SimplifiedTokenType::SIMPLIFIED_TOKEN_ERROR;
				tokens.push_back(token);
				in_quotes = false;
			}
			if (StringUtil::CharacterIsNewline(error_msg[i])) {
				// found a newline in a quote, abort the quoted state entirely
				tokens.pop_back();
				in_quotes = false;
			}
		} else if (error_msg[i] == '"' || error_msg[i] == '\'') {
			// not quoted and found a quote - enter the quoted state
			SimplifiedToken token;
			token.start = i;
			token.type = SimplifiedTokenType::SIMPLIFIED_TOKEN_ERROR_SUGGESTION;
			token.start++;
			tokens.push_back(token);
			quote_char = error_msg[i];
			in_quotes = true;
		}
	}
	if (in_quotes) {
		// unterminated quotes at the end of the error - pop back the quoted state
		tokens.pop_back();
	}
	if (line_pos.IsValid()) {
		SimplifiedToken token;
		token.start = line_pos.GetIndex() + 1;
		token.type = SimplifiedTokenType::SIMPLIFIED_TOKEN_ERROR_EMPHASIS;
		tokens.push_back(token);

		// tokenize the LINE part
		idx_t query_start;
		for (query_start = line_pos.GetIndex() + 6; query_start < error_msg.size(); query_start++) {
			if (error_msg[query_start] != ':' && !StringUtil::CharacterIsDigit(error_msg[query_start])) {
				break;
			}
		}
		if (query_start < error_msg.size()) {
			token.start = query_start;
			token.type = SimplifiedTokenType::SIMPLIFIED_TOKEN_ERROR;
			tokens.push_back(token);

			idx_t query_end;
			for (query_end = query_start; query_end < error_msg.size(); query_end++) {
				if (error_msg[query_end] == '\n') {
					break;
				}
			}
			// after LINE XXX: comes a caret - look for it
			idx_t caret_position = error_msg.size();
			bool place_caret = false;
			idx_t caret_start = query_end + 1;
			if (caret_start < error_msg.size()) {
				for (idx_t i = caret_start; i < error_msg.size(); i++) {
					if (error_msg[i] == '^') {
						// found the caret
						// to get the caret position in the query we need to
						caret_position = i - caret_start - ((query_start - line_pos.GetIndex()) - 1);
						place_caret = true;
						break;
					}
				}
			}

			// tokenize the actual query
			string query = error_msg.substr(query_start, query_end - query_start);
			auto query_tokens = Tokenize(query);
			for (auto &query_token : query_tokens) {
				if (place_caret) {
					// find the caret position and highlight the identifier it points to
					if (query_token.start >= caret_position) {
						// we need to place the caret here
						query_token.start = query_start + caret_position;
						query_token.type = SimplifiedTokenType::SIMPLIFIED_TOKEN_ERROR_EMPHASIS;
						tokens.push_back(query_token);

						place_caret = false;
						continue;
					}
				}
				switch (query_token.type) {
				case SimplifiedTokenType::SIMPLIFIED_TOKEN_KEYWORD:
					query_token.type = SimplifiedTokenType::SIMPLIFIED_TOKEN_ERROR_EMPHASIS;
					break;
				default:
					query_token.type = SimplifiedTokenType::SIMPLIFIED_TOKEN_ERROR;
					break;
				}
				query_token.start += query_start;
				tokens.push_back(query_token);
			}
			if (query_end < error_msg.size()) {
				token.start = query_end;
				token.type = SimplifiedTokenType::SIMPLIFIED_TOKEN_ERROR;
				tokens.push_back(token);
			}
		}
	}
	return tokens;
}

KeywordCategory Parser::ToKeywordCategory(const string &text) {
	return DuckDBKeywordHelper::Instance().GetKeywordCategory(text);
}

KeywordCategory Parser::IsKeyword(const string &text) {
	return ToKeywordCategory(text);
}

vector<ParserKeyword> Parser::KeywordList() {
	auto &keyword_helper = DuckDBKeywordHelper::Instance();
	return keyword_helper.KeywordList();
}

unique_ptr<QueryNode> Parser::ParseSelectNode(const string &query) {
	Parser parser(options);
	parser.ParseQuery(query);
	if (parser.statements.size() != 1 || parser.statements[0]->type != StatementType::SELECT_STATEMENT) {
		throw InternalException("Expected a single select statement");
	}
	return std::move(parser.statements[0]->Cast<SelectStatement>().node);
}

vector<unique_ptr<ParsedExpression>> Parser::ParseExpressionList(const string &select_list) {
	// construct a mock query prefixed with SELECT
	string mock_query = "SELECT " + select_list;
	// parse the query
	Parser parser(options);
	parser.ParseQuery(mock_query);
	// check the statements
	if (parser.statements.size() != 1 || parser.statements[0]->type != StatementType::SELECT_STATEMENT) {
		throw ParserException("Expected a single SELECT statement");
	}
	auto &select = parser.statements[0]->Cast<SelectStatement>();
	if (select.node->type != QueryNodeType::SELECT_NODE) {
		throw ParserException("Expected a single SELECT node");
	}
	auto &select_node = select.node->Cast<SelectNode>();
	if (!select_node.modifiers.empty()) {
		throw ParserException("Cannot have any modifiers in the expression list");
	}
	if (select_node.where_clause) {
		throw ParserException("Cannot have a WHERE clause in the expression list");
	}
	if (!select_node.groups.group_expressions.empty()) {
		throw ParserException("Cannot have a GROUP BY clause in the expression list");
	}
	if (select_node.having) {
		throw ParserException("Cannot have a HAVING clause in the expression list");
	}
	if (select_node.qualify) {
		throw ParserException("Cannot have a QUALIFY clause in the expression list");
	}
	if (select_node.sample) {
		throw ParserException("Cannot have a SAMPLE clause in the expression list");
	}
	return std::move(select_node.select_list);
}

unique_ptr<ParsedExpression> Parser::ParseSingleExpression(const string &expression) {
	auto expressions = ParseExpressionList(expression);
	if (expressions.size() != 1) {
		throw InternalException("Expected a single expression");
	}
	return std::move(expressions[0]);
}

GroupByNode Parser::ParseGroupByList(const string &group_by) {
	// construct a mock SELECT query with our group_by expressions
	string mock_query = StringUtil::Format("SELECT 42 GROUP BY %s", group_by);
	// parse the query
	Parser parser(options);
	parser.ParseQuery(mock_query);
	// check the result
	if (parser.statements.size() != 1 || parser.statements[0]->type != StatementType::SELECT_STATEMENT) {
		throw ParserException("Expected a single SELECT statement");
	}
	auto &select = parser.statements[0]->Cast<SelectStatement>();
	D_ASSERT(select.node->type == QueryNodeType::SELECT_NODE);
	auto &select_node = select.node->Cast<SelectNode>();
	return std::move(select_node.groups);
}

vector<OrderByNode> Parser::ParseOrderList(const string &select_list) {
	// construct a mock query
	string mock_query = "SELECT * FROM tbl ORDER BY " + select_list;
	// parse the query
	Parser parser(options);
	parser.ParseQuery(mock_query);
	// check the statements
	if (parser.statements.size() != 1 || parser.statements[0]->type != StatementType::SELECT_STATEMENT) {
		throw ParserException("Expected a single SELECT statement");
	}
	auto &select = parser.statements[0]->Cast<SelectStatement>();
	D_ASSERT(select.node->type == QueryNodeType::SELECT_NODE);
	auto &select_node = select.node->Cast<SelectNode>();
	if (select_node.modifiers.empty() || select_node.modifiers[0]->type != ResultModifierType::ORDER_MODIFIER ||
	    select_node.modifiers.size() != 1) {
		throw ParserException("Expected a single ORDER clause");
	}
	auto &order = select_node.modifiers[0]->Cast<OrderModifier>();
	return std::move(order.orders);
}

void Parser::ParseUpdateList(const string &update_list, vector<Identifier> &update_columns,
                             vector<unique_ptr<ParsedExpression>> &expressions) {
	// construct a mock query
	string mock_query = "UPDATE tbl SET " + update_list;
	// parse the query
	Parser parser(options);
	parser.ParseQuery(mock_query);
	// check the statements
	if (parser.statements.size() != 1 || parser.statements[0]->type != StatementType::UPDATE_STATEMENT) {
		throw ParserException("Expected a single UPDATE statement");
	}
	auto &update = parser.statements[0]->Cast<UpdateStatement>();
	update_columns = update.node->set_info->columns;
	expressions = std::move(update.node->set_info->expressions);
}

vector<vector<unique_ptr<ParsedExpression>>> Parser::ParseValuesList(const string &value_list) {
	// construct a mock query
	string mock_query = "VALUES " + value_list;
	// parse the query
	Parser parser(options);
	parser.ParseQuery(mock_query);
	// check the statements
	if (parser.statements.size() != 1 || parser.statements[0]->type != StatementType::SELECT_STATEMENT) {
		throw ParserException("Expected a single SELECT statement");
	}
	auto &select = parser.statements[0]->Cast<SelectStatement>();
	if (select.node->type != QueryNodeType::SELECT_NODE) {
		throw ParserException("Expected a single SELECT node");
	}
	auto &select_node = select.node->Cast<SelectNode>();
	if (!select_node.from_table || select_node.from_table->type != TableReferenceType::EXPRESSION_LIST) {
		throw ParserException("Expected a single VALUES statement");
	}
	auto &values_list = select_node.from_table->Cast<ExpressionListRef>();
	return std::move(values_list.values);
}

ColumnList Parser::ParseColumnList(const string &column_list) {
	string mock_query = "CREATE TABLE tbl (" + column_list + ")";
	Parser parser(options);
	parser.ParseQuery(mock_query);
	if (parser.statements.size() != 1 || parser.statements[0]->type != StatementType::CREATE_STATEMENT) {
		throw ParserException("Expected a single CREATE statement");
	}
	auto &create = parser.statements[0]->Cast<CreateStatement>();
	if (create.info->type != CatalogType::TABLE_ENTRY) {
		throw InternalException("Expected a single CREATE TABLE statement");
	}
	auto &info = create.info->Cast<CreateTableInfo>();
	return std::move(info.columns);
}

ColumnDefinition Parser::ParseColumnDefinition(const string &column_definition) {
	auto column_list = ParseColumnList(column_definition);
	return column_list.GetColumn(LogicalIndex(0)).Copy();
}

} // namespace duckdb
