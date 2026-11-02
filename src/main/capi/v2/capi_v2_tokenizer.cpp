#include "duckdb/main/capi_v2/capi_v2_internal.hpp"

#include "duckdb/parser/peg/compiled_grammar.hpp"
#include "duckdb/parser/peg/tokenizer/highlight_tokenizer.hpp"

namespace duckdb::capiv2 {
namespace {

struct TokenV2 {
	DUCKDB_V2_TOKEN_TYPE type;
	idx_t start;
	idx_t length;
};

struct TokenIteratorWrapperV2 {
	vector<TokenV2> tokens;
	idx_t input_length = 0;
	idx_t position = 0;
	bool ends_unterminated = false;
};

// With this terminator, TokenizeInput returns true exactly on a clean exit; both sentinels are dropped anyway.
class CleanExitTokenizerBehavior : public HighlightTokenizerBehavior {
public:
	using HighlightTokenizerBehavior::HighlightTokenizerBehavior;

	TokenType GetTerminator() const override {
		return TokenType::END_OF_INPUT_AUTOCOMPLETE;
	}
};

DUCKDB_V2_TOKEN_TYPE ConvertTokenType(TokenType type) {
	switch (type) {
	case TokenType::KEYWORD:
		return DUCKDB_V2_TOKEN_TYPE_KEYWORD;
	case TokenType::IDENTIFIER:
		return DUCKDB_V2_TOKEN_TYPE_IDENTIFIER;
	case TokenType::STRING_LITERAL:
		return DUCKDB_V2_TOKEN_TYPE_STRING_LITERAL;
	case TokenType::NUMBER_LITERAL:
		return DUCKDB_V2_TOKEN_TYPE_NUMBER_LITERAL;
	case TokenType::OPERATOR:
		return DUCKDB_V2_TOKEN_TYPE_OPERATOR;
	case TokenType::COMMENT:
		return DUCKDB_V2_TOKEN_TYPE_COMMENT;
	case TokenType::TERMINATOR:
		return DUCKDB_V2_TOKEN_TYPE_TERMINATOR;
	default:
		// The raw tokenizer only assigns lexical classes; the semantic ones need the matcher pass.
		throw InternalException("tokenizer emitted token type %d, which is not a lexical class",
		                        static_cast<int>(type));
	}
}

} // namespace

auto Convert(duckdb_v2_token_iterator_handle ptr) -> TokenIteratorWrapperV2 * {
	return reinterpret_cast<TokenIteratorWrapperV2 *>(ptr);
}
auto Convert(TokenIteratorWrapperV2 *ptr) -> duckdb_v2_token_iterator_handle {
	return reinterpret_cast<duckdb_v2_token_iterator_handle>(ptr);
}

} // namespace duckdb::capiv2

//----------------------------------------------------------------------------------------------------------------------
// Public API
//----------------------------------------------------------------------------------------------------------------------

using namespace duckdb::capiv2;

DUCKDB_V2_ERROR duckdb_v2_tokenize_sql(duckdb_v2_connection_handle conn, const duckdb_v2_str *sql,
                                       duckdb_v2_token_iterator_handle *out_iterator,
                                       duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(out_iterator);
	*out_iterator = nullptr;
	DUCKDB_CHECK_ARG(conn);
	DUCKDB_CHECK_ARG(sql);
	return WithErrorHandler(err, [&]() {
		auto *connection = Convert(conn);
		duckdb::string input(Convert(sql));
		duckdb::vector<duckdb::MatcherToken> raw_tokens;
		CleanExitTokenizerBehavior behavior(input, raw_tokens);
		auto grammar = duckdb::CompiledGrammar::Get(*connection->context);
		const bool clean_exit = grammar->GetTokenizer().TokenizeInput(behavior);

		auto wrapper = duckdb::make_uniq<TokenIteratorWrapperV2>();
		wrapper->input_length = input.size();
		wrapper->tokens.reserve(raw_tokens.size());
		bool last_unterminated = false;
		for (auto &token : raw_tokens) {
			if (token.type == duckdb::TokenType::END_OF_INPUT ||
			    token.type == duckdb::TokenType::END_OF_INPUT_AUTOCOMPLETE) {
				continue;
			}
			wrapper->tokens.push_back({ConvertTokenType(token.type), token.offset, token.length});
			last_unterminated = token.unterminated;
		}
		// A trailing line comment is a dirty exit without the flag; an open string or quoted identifier is a clean
		// exit with it.
		wrapper->ends_unterminated = !clean_exit || last_unterminated;
		*out_iterator = Convert(wrapper.release());
	});
}

DUCKDB_V2_ERROR duckdb_v2_token_iterator_next(duckdb_v2_token_iterator_handle iterator, DUCKDB_V2_TOKEN_TYPE *out_type,
                                              idx_t *out_start, idx_t *out_length, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(out_type);
	DUCKDB_CHECK_ARG(out_start);
	DUCKDB_CHECK_ARG(out_length);
	*out_type = DUCKDB_V2_TOKEN_TYPE_INVALID;
	*out_start = 0;
	*out_length = 0;
	DUCKDB_CHECK_ARG(iterator);
	return WithErrorHandler(err, [&]() {
		auto &wrapper = *Convert(iterator);
		if (wrapper.position >= wrapper.tokens.size()) {
			*out_type = DUCKDB_V2_TOKEN_TYPE_END_OF_INPUT;
			*out_start = wrapper.input_length;
			return;
		}
		auto &token = wrapper.tokens[wrapper.position++];
		*out_type = token.type;
		*out_start = token.start;
		*out_length = token.length;
	});
}

DUCKDB_V2_ERROR duckdb_v2_token_iterator_ends_unterminated(duckdb_v2_token_iterator_handle iterator,
                                                           bool *out_ends_unterminated,
                                                           duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(out_ends_unterminated);
	*out_ends_unterminated = false;
	DUCKDB_CHECK_ARG(iterator);
	return WithErrorHandler(err, [&]() { *out_ends_unterminated = Convert(iterator)->ends_unterminated; });
}

DUCKDB_V2_ERROR duckdb_v2_token_iterator_destroy(duckdb_v2_token_iterator_handle *iterator) {
	return WithErrorHandler(nullptr, [&]() {
		if (!iterator) {
			return;
		}
		if (*iterator) {
			delete Convert(*iterator);
			*iterator = nullptr;
		}
	});
}
