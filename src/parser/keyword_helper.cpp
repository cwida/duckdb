#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/parser/peg/keyword_helper/duckdb_keyword_helper.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/identifier.hpp"

namespace duckdb {

bool KeywordHelper::IsKeyword(const string &text, KeywordCategory category) {
	return DuckDBKeywordHelper::Instance().GetKeywordCategory(text) != category;
}

KeywordCategory KeywordHelper::KeywordCategoryType(const string &text) {
	return DuckDBKeywordHelper::Instance().GetKeywordCategory(text);
}

bool KeywordHelper::RequiresQuotes(const string &text, bool allow_caps) {
	for (size_t i = 0; i < text.size(); i++) {
		if (i > 0 && (text[i] >= '0' && text[i] <= '9')) {
			continue;
		}
		if (text[i] >= 'a' && text[i] <= 'z') {
			continue;
		}
		if (allow_caps) {
			if (text[i] >= 'A' && text[i] <= 'Z') {
				continue;
			}
		}
		if (text[i] == '_') {
			continue;
		}
		return true;
	}
	return IsKeyword(text);
}

string KeywordHelper::EscapeQuotes(const string &text, char quote) {
	return StringUtil::Replace(text, string(1, quote), string(2, quote));
}

string KeywordHelper::WriteQuotedAndEscaped(const string &text, char quote) {
	string result;
	result.reserve(text.size() + 2);
	result += quote;
	for (auto c : text) {
		if (c == quote) {
			// character matches quote - escape by adding the quote again
			result += quote;
		}
		result += c;
	}
	result += quote;
	return result;
}

string KeywordHelper::WriteQuoted(const string &text, char quote) {
	// 1. Escapes all occurrences of 'quote' by doubling them (escape in SQL)
	// 2. Adds quotes around the string
	return WriteQuotedAndEscaped(text, quote);
}

static bool RequiresQuotesForPrinting(const string &text, bool allow_caps) {
	// After a qualification dot, an underscore followed by a digit can begin a decimal literal.
	const bool ambiguous_after_dot = text.size() > 1 && text[0] == '_' && StringUtil::CharacterIsDigit(text[1]);
	return text.empty() || ambiguous_after_dot || KeywordHelper::RequiresQuotes(text, allow_caps);
}

string KeywordHelper::WriteOptionallyQuoted(const string &text, char quote, bool allow_caps) {
	if (!RequiresQuotesForPrinting(text, allow_caps)) {
		return text;
	}
	return WriteQuotedAndEscaped(text, quote);
}

SQLIdentifier::SQLIdentifier(const Identifier &id) : raw_string(id.GetIdentifierName()) {
}

string SQLIdentifier::ToString(const string &identifier) {
	if (!RequiresQuotesForPrinting(identifier, true)) {
		return identifier;
	}
	return SQLQuotedIdentifier::ToString(identifier);
}

string SQLIdentifier::ToString(const Identifier &identifier) {
	return ToString(identifier.GetIdentifierName());
}

string SQLIdentifier::ToString(const char *identifier) {
	return ToString(string(identifier));
}

SQLQuotedIdentifier::SQLQuotedIdentifier(const Identifier &id) : raw_string(id.GetIdentifierName()) {
}

string SQLQuotedIdentifier::ToString(const string &identifier) {
	return KeywordHelper::WriteQuotedAndEscaped(identifier, '"');
}

string SQLQuotedIdentifier::ToString(const Identifier &identifier) {
	return ToString(identifier.GetIdentifierName());
}

string SQLQuotedIdentifier::ToString(const char *identifier) {
	return ToString(string(identifier));
}

string SQLString::ToString(const string &literal) {
	return KeywordHelper::WriteQuotedAndEscaped(literal, '\'');
}

} // namespace duckdb
