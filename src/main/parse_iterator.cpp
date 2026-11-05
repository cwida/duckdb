#include "duckdb/main/parse_iterator.hpp"
#include "duckdb/main/database.hpp"

#include "duckdb/main/client_context.hpp"
#include "duckdb/main/extension_callback_manager.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/parser_extension.hpp"
#include "duckdb/parser/token_iterator.hpp"
#include "duckdb/parser/peg/matcher.hpp"
#include "duckdb/parser/peg/compiled_grammar.hpp"
#include "duckdb/parser/peg/tokenizer/parser_tokenizer.hpp"
#include "duckdb/parser/sql_statement.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/parser/parsed_data/create_info.hpp"

namespace duckdb {

ParseIterator::ParseIterator(ClientContext &context_p, const string &sql_p, bool apply_invalidation_policy_p,
                             optional_ptr<ClientContextLock> held_lock_p)
    : context(context_p), apply_invalidation_policy(apply_invalidation_policy_p), held_lock(held_lock_p) {
	try {
		sql = Parser::NormalizeSQLString(sql_p);
	} catch (std::exception &ex) {
		ApplyInvalidationPolicy(ErrorData(ex));
		throw;
	}
}

void ParseIterator::ApplyInvalidationPolicy(const ErrorData &error) {
	if (!apply_invalidation_policy) {
		return;
	}
	if (held_lock) {
		context.ApplyInvalidationPolicy(error);
		return;
	}
	// Parsing itself can run without the context lock, but the transaction state must not change underneath us
	auto lock = context.LockContext();
	context.ApplyInvalidationPolicy(error);
}

ParseIterator::~ParseIterator() = default;

ParseIterator::ParseIterator(ParseIterator &&) noexcept = default;

ClientContext &ParseIterator::GetClientContext() {
	return context;
}

bool ParseIterator::Peek() {
	// Already buffered from a prior Peek — just report it.
	if (current_statement) {
		return true;
	}
	if (exhausted) {
		return false;
	}
	try {
		return PeekInternal();
	} catch (std::exception &ex) {
		// Retrying would parse the failing statement again, and apply the policy again to whatever transaction is
		// active by then
		exhausted = true;
		// Rendering the error for display is left to the caller, since some render errors themselves
		ApplyInvalidationPolicy(ErrorData(ex));
		throw;
	}
}

bool ParseIterator::PeekInternal() {
	auto &client_context = context;
	auto options = client_context.GetParserOptions();
	// On the very first Peek, give `parser_override` extensions a chance to claim the whole
	// query. If one does, we yield its statements one at a time and skip the PEG path entirely.
	if (!override_resolved) {
		override_resolved = true;
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
				auto result = ext.parser_override(ext.parser_info.get(), sql, options);
				if (result.type == ParserExtensionResultType::PARSE_SUCCESSFUL) {
					overridden_statements = make_uniq<vector<unique_ptr<SQLStatement>>>(std::move(result.statements));
					break;
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
			if (!overridden_statements && options.parser_override_setting == AllowParserOverride::STRICT_OVERRIDE &&
			    has_strict_extension_error) {
				last_strict_extension_error.Throw();
			}
		}
	}
	if (overridden_statements) {
		if (override_cursor >= overridden_statements->size()) {
			exhausted = true;
			return false;
		}
		BufferStatement(std::move((*overridden_statements)[override_cursor++]));
		return true;
	}
	// parser options can change between statements of the same input (SET statements such as
	// integer_division or active_grammar_extensions), so each statement gets a parser with fresh options
	Parser parser(options);
	EnsureTokenized();
	// Walk the token cursor through the cached `tokens`, calling Parser::ParseTopLevelStatement
	// repeatedly. A nullptr return with cursor advanced means a separator-only TopLevelStatement
	// (e.g. between statements or trailing ';'s); we loop past it. A nullptr return with cursor
	// at end means the input is exhausted.
	while (true) {
		if (token_iterator->AtEnd()) {
			exhausted = true;
			return false;
		}
		auto stmt = parser.ParseTopLevelStatement(*token_iterator, sql);
		if (stmt) {
			// ParseTopLevelStatement doesn't populate stmt->query (it operates on tokens, not the
			// source string). Mirror Parser::ParseQuery's per-statement post-processing: extend from
			// the statement's start to the next statement's start (or end of input) so the trailing
			// `;` and inter-statement whitespace end up inside stmt->query — downstream consumers
			// (logging, error reporting, EXPLAIN) rely on that shape.
			idx_t stmt_loc = stmt->stmt_location.offset;
			idx_t end_loc = sql.size();
			if (auto current = token_iterator->Current()) {
				if (current->type != TokenType::END_OF_INPUT) {
					end_loc = current->offset;
				}
			}
			stmt->query = sql.substr(stmt_loc, end_loc - stmt_loc);
			stmt->stmt_location = QueryLocation(0, stmt->query.size());
			if (stmt->type == StatementType::CREATE_STATEMENT) {
				auto &create = stmt->Cast<CreateStatement>();
				create.info->sql = stmt->query;
			}
			BufferStatement(std::move(stmt));
			return true;
		}
		if (token_iterator->AtEnd()) {
			exhausted = true;
			return false;
		}
		// separator-only TLS in the middle of the input — loop and try the next.
	}
}

void ParseIterator::BufferStatement(unique_ptr<SQLStatement> statement) {
	try {
		PostParseExtension::CallPostParseFunctions(context, *statement);
	} catch (...) {
		// A rejected statement ends the input: nothing after it is parsed or yielded.
		exhausted = true;
		throw;
	}
	current_statement = std::move(statement);
}

void ParseIterator::EnsureTokenized() {
	if (!token_iterator) {
		// Tokenize the full input once. Subsequent Peek/HasMore calls walk through the iterator;
		// we never re-tokenize. Tokenization is grammar-free.
		auto owned_tokens = make_uniq<vector<MatcherToken>>();
		ParserTokenizerBehavior behavior(sql, *owned_tokens);
		auto compiled_grammar = CompiledGrammar::Get(context);
		auto &tokenizer = compiled_grammar->GetTokenizer();
		tokenizer.TokenizeInput(behavior);
		token_iterator = make_uniq<TokenIterator>(std::move(owned_tokens));
	}
}

bool ParseIterator::HasMore() {
	// A statement is already parsed and buffered by a prior Peek.
	if (current_statement) {
		return true;
	}
	if (exhausted) {
		return false;
	}
	// parser_override path: yield remaining overridden statements.
	if (overridden_statements) {
		return override_cursor < overridden_statements->size();
	}
	// PEG path: inspect the remaining tokens without parsing or advancing the committed position.
	EnsureTokenized();
	TokenIterator lookahead(*token_iterator);
	return lookahead.HasMoreStatements();
}

unique_ptr<SQLStatement> ParseIterator::GetStatement() {
	if (!current_statement) {
		return nullptr;
	}
	return std::move(current_statement);
}

} // namespace duckdb
