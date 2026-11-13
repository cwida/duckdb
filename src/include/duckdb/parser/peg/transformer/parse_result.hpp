#pragma once
#include "utf8proc_wrapper.hpp"
#include "duckdb/common/arena_linked_list.hpp"
#include "duckdb/common/array_ptr.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/types/blob.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/parsed_expression.hpp"
#include "duckdb/parser/peg/special_string_utils.hpp"
#include "duckdb/parser/peg/token_type.hpp"
#include "duckdb/common/windows_undefs.hpp"

namespace duckdb {

inline string TokenTypeToString(TokenType type) {
	switch (type) {
	case TokenType::KEYWORD:
		return "KEYWORD";
	case TokenType::STRING_LITERAL:
		return "STRING_LITERAL";
	case TokenType::NUMBER_LITERAL:
		return "NUMBER_LITERAL";
	case TokenType::OPERATOR:
		return "OPERATOR";
	case TokenType::IDENTIFIER:
		return "IDENTIFIER";
	case TokenType::COMMENT:
		return "COMMENT";
	case TokenType::TERMINATOR:
		return "TERMINATOR";
	case TokenType::TOKEN_ERROR:
		return "ERROR";
	case TokenType::CATALOG_NAME:
		return "CATALOG_NAME";
	case TokenType::SCHEMA_NAME:
		return "SCHEMA_NAME";
	case TokenType::TABLE_NAME:
		return "TABLE_NAME";
	case TokenType::TYPE_NAME:
		return "TYPE_NAME";
	case TokenType::COLUMN_NAME:
		return "COLUMN_NAME";
	case TokenType::SCALAR_FUNCTION:
		return "SCALAR_FUNCTION";
	case TokenType::TABLE_FUNCTION:
		return "TABLE_FUNCTION";
	case TokenType::PRAGMA_FUNCTION:
		return "PRAGMA_FUNCTION";
	case TokenType::SETTING_NAME:
		return "SETTING_NAME";
	case TokenType::END_OF_INPUT:
		return "END_OF_INPUT";
	case TokenType::END_OF_INPUT_AUTOCOMPLETE:
		return "END_OF_INPUT_AUTOCOMPLETE";
	default:
		return "UNKNOWN";
	}
}

class PEGTransformer; // Forward declaration
struct CompiledGrammarRule;
class Matcher;

enum class ParseResultType : uint8_t {
	LIST,
	OPTIONAL,
	REPEAT,
	CHOICE,
	EXPRESSION,
	IDENTIFIER,
	KEYWORD,
	OPERATOR,
	STATEMENT,
	EXTENSION,
	NUMBER,
	STRING,
	END_OF_INPUT,
	TOKEN,
	INVALID
};

inline const char *ParseResultToString(ParseResultType type) {
	switch (type) {
	case ParseResultType::LIST:
		return "LIST";
	case ParseResultType::OPTIONAL:
		return "OPTIONAL";
	case ParseResultType::REPEAT:
		return "REPEAT";
	case ParseResultType::CHOICE:
		return "CHOICE";
	case ParseResultType::EXPRESSION:
		return "EXPRESSION";
	case ParseResultType::IDENTIFIER:
		return "IDENTIFIER";
	case ParseResultType::KEYWORD:
		return "KEYWORD";
	case ParseResultType::OPERATOR:
		return "OPERATOR";
	case ParseResultType::STATEMENT:
		return "STATEMENT";
	case ParseResultType::EXTENSION:
		return "EXTENSION";
	case ParseResultType::NUMBER:
		return "NUMBER";
	case ParseResultType::STRING:
		return "STRING";
	case ParseResultType::END_OF_INPUT:
		return "END_OF_INPUT";
	case ParseResultType::TOKEN:
		return "TOKEN";
	case ParseResultType::INVALID:
		return "INVALID";
	}
	return "INVALID";
}

class ParseResult {
public:
	explicit ParseResult(ParseResultType type, optional_idx offset, optional_idx length = optional_idx())
	    : type(type), has_offset(offset.IsValid()), has_length(length.IsValid()),
	      offset(offset.IsValid() ? offset.GetIndex() : 0), length(length.IsValid() ? length.GetIndex() : 0) {
	}
	virtual ~ParseResult() = default;

	ParseResult(const ParseResult &) = delete;
	ParseResult &operator=(const ParseResult &) = delete;

	template <class TARGET>
	TARGET &Cast() {
		if (TARGET::TYPE != ParseResultType::INVALID && type != TARGET::TYPE) {
			throw InternalException("Failed to cast parse result of type %s to type %s for rule %s",
			                        ParseResultToString(TARGET::TYPE), ParseResultToString(type), Name());
		}
		return reinterpret_cast<TARGET &>(*this);
	}

	optional_ptr<const CompiledGrammarRule> rule;
	ParseResultType type;
	//! Set when a collapsible rule handed this result out in place of its own, so the transformer runs this
	//! result's rule rather than the one the parent asked for
	bool collapsed = false;

	void SetRule(const CompiledGrammarRule &rule_p) {
		rule = rule_p;
	}
	//! The name of the rule (or matcher) that produced this result. Rule names live in the compiled grammar and
	//! outlive every parse, so a result points at one instead of owning a copy of it.
	const string &Name() const {
		static const string EMPTY;
		return name ? *name : EMPTY;
	}
	//! Only a pointer to the name is kept, so these only take a grammar object that owns it, or a result that already
	//! points at one.
	inline void SetNameFrom(const CompiledGrammarRule &rule_p);
	inline void SetNameFrom(const Matcher &matcher_p);
	void SetNameFrom(const ParseResult &other) {
		name = other.name;
	}
	optional_ptr<const CompiledGrammarRule> GetRule() const {
		return rule;
	}

	optional_idx GetOffset() const {
		return has_offset ? optional_idx(offset) : optional_idx();
	}

	optional_idx GetLength() const {
		return has_length ? optional_idx(length) : optional_idx();
	}

	//! Returns the source location [offset, offset+length) of this parse result (length 0 when unknown)
	QueryLocation GetLocation() const {
		if (!has_offset) {
			return QueryLocation();
		}
		return QueryLocation(offset, has_length ? length : 0);
	}

	//! Grow this result's location so it encloses the given child's location (used to build composite
	//! locations bottom-up)
	void EncloseChild(const ParseResult &child) {
		auto child_location = child.GetLocation();
		if (!has_offset || !child_location.IsValid()) {
			return;
		}
		auto enclosing = GetLocation().Merge(child_location);
		length = enclosing.length;
		has_length = true;
	}

	virtual void ToStringInternal(std::stringstream &ss, std::unordered_set<const ParseResult *> &visited,
	                              const std::string &indent, bool is_last) const {
		ss << indent << (is_last ? "└─" : "├─") << " " << ParseResultToString(type);
		if (!Name().empty()) {
			ss << " (" << Name() << ")";
		}
	}

	// The public entry point
	std::string ToString() const {
		std::stringstream ss;
		std::unordered_set<const ParseResult *> visited;
		// The root is always the "last" element at its level
		ToStringInternal(ss, visited, "", true);
		return ss.str();
	}

private:
	//! Keep validity flags with the small metadata fields, without narrowing source positions.
	bool has_offset;
	bool has_length;
	idx_t offset;
	//! For leaf tokens the token length; for composite results the enclosing extent of children.
	idx_t length;
	const string *name = nullptr;
};

struct IdentifierParseResult : ParseResult {
	static constexpr ParseResultType TYPE = ParseResultType::IDENTIFIER;
	Identifier identifier;

	explicit IdentifierParseResult(string identifier_p, optional_idx offset, optional_idx length = optional_idx())
	    : ParseResult(TYPE, offset, length), identifier(std::move(identifier_p)) {
	}

	void ToStringInternal(std::stringstream &ss, std::unordered_set<const ParseResult *> &visited,
	                      const std::string &indent, bool is_last) const override {
		ParseResult::ToStringInternal(ss, visited, indent, is_last);
		ss << ": " << identifier.GetIdentifierName() << "\n";
	}
};

//! A single token consumed without interpreting it; carries its text for debugging only
struct TokenParseResult : ParseResult {
	static constexpr ParseResultType TYPE = ParseResultType::TOKEN;
	string text;

	TokenParseResult(string text_p, optional_idx offset, optional_idx length)
	    : ParseResult(TYPE, offset, length), text(std::move(text_p)) {
	}

	void ToStringInternal(std::stringstream &ss, std::unordered_set<const ParseResult *> &visited,
	                      const std::string &indent, bool is_last) const override {
		ParseResult::ToStringInternal(ss, visited, indent, is_last);
		ss << ": " << text << "\n";
	}
};

struct EndOfInputParseResult : ParseResult {
	static constexpr ParseResultType TYPE = ParseResultType::END_OF_INPUT;

	EndOfInputParseResult() : ParseResult(TYPE, optional_idx()) {
	}

	void ToStringInternal(std::stringstream &ss, std::unordered_set<const ParseResult *> &visited,
	                      const std::string &indent, bool is_last) const override {
		ParseResult::ToStringInternal(ss, visited, indent, is_last);
		ss << "\n";
	}
};

struct KeywordParseResult : ParseResult {
	static constexpr ParseResultType TYPE = ParseResultType::KEYWORD;
	string keyword;

	explicit KeywordParseResult(string keyword_p, optional_idx offset, optional_idx length = optional_idx())
	    : ParseResult(TYPE, offset, length), keyword(std::move(keyword_p)) {
	}

	void ToStringInternal(std::stringstream &ss, std::unordered_set<const ParseResult *> &visited,
	                      const std::string &indent, bool is_last) const override {
		ParseResult::ToStringInternal(ss, visited, indent, is_last);
		ss << ": \"" << keyword << "\"\n";
	}
};

struct ListParseResult : ParseResult {
	static constexpr ParseResultType TYPE = ParseResultType::LIST;

public:
	explicit ListParseResult(unsafe_array_ptr<reference<ParseResult>> results_p,
	                         optional_ptr<const Matcher> named_matcher, optional_idx offset)
	    : ParseResult(TYPE, offset), children(results_p) {
		if (named_matcher) {
			SetNameFrom(*named_matcher);
		}
		for (auto &child : children) {
			EncloseChild(child.get());
		}
	}

	unsafe_array_ptr<reference<ParseResult>> GetChildren() const {
		return children;
	}

	ParseResult &GetChild(idx_t index) {
		if (index >= children.size()) {
			throw InternalException("Child index out of bounds");
		}
		return children[index].get();
	}

	template <class T>
	T &Child(idx_t index) {
		return GetChild(index).Cast<T>();
	}

	void ToStringInternal(std::stringstream &ss, std::unordered_set<const ParseResult *> &visited,
	                      const std::string &indent, bool is_last) const override {
		ss << indent << (is_last ? "└─" : "├─");

		if (visited.count(this)) {
			ss << " List (" << Name() << ") [... already printed ...]\n";
			return;
		}
		visited.insert(this);

		ss << " " << ParseResultToString(type);
		if (!Name().empty()) {
			ss << " (" << Name() << ")";
		}
		ss << " [" << children.size() << " children]\n";

		std::string child_indent = indent + (is_last ? "   " : "│  ");
		for (size_t i = 0; i < children.size(); ++i) {
			children[i].get().ToStringInternal(ss, visited, child_indent, i == children.size() - 1);
		}
	}

private:
	unsafe_array_ptr<reference<ParseResult>> children;
};

struct RepeatParseResult : ParseResult {
	static constexpr ParseResultType TYPE = ParseResultType::REPEAT;

	explicit RepeatParseResult(unsafe_array_ptr<reference<ParseResult>> results_p, optional_idx offset)
	    : ParseResult(TYPE, offset), children(results_p) {
		for (auto &child : children) {
			EncloseChild(child.get());
		}
	}

	unsafe_array_ptr<reference<ParseResult>> GetChildren() const {
		return children;
	}

	template <class T>
	T &Child(idx_t index) {
		if (index >= children.size()) {
			throw InternalException("Child index out of bounds");
		}
		return children[index].get().Cast<T>();
	}

	void ToStringInternal(std::stringstream &ss, std::unordered_set<const ParseResult *> &visited,
	                      const std::string &indent, bool is_last) const override {
		ss << indent << (is_last ? "└─" : "├─");

		if (visited.count(this)) {
			ss << " Repeat (" << Name() << ") [... already printed ...]\n";
			return;
		}
		visited.insert(this);

		ss << " " << ParseResultToString(type);
		if (!Name().empty()) {
			ss << " (" << Name() << ")";
		}
		ss << " [" << children.size() << " children]\n";

		std::string child_indent = indent + (is_last ? "   " : "│  ");
		for (size_t i = 0; i < children.size(); ++i) {
			children[i].get().ToStringInternal(ss, visited, child_indent, i == children.size() - 1);
		}
	}

private:
	unsafe_array_ptr<reference<ParseResult>> children;
};

struct OptionalParseResult : ParseResult {
	static constexpr ParseResultType TYPE = ParseResultType::OPTIONAL;

	explicit OptionalParseResult() : ParseResult(TYPE, optional_idx()), optional_result(nullptr) {
	}
	explicit OptionalParseResult(optional_ptr<ParseResult> result_p, optional_idx offset)
	    : ParseResult(TYPE, offset), optional_result(result_p) {
		SetNameFrom(*result_p);
		EncloseChild(*result_p);
	}

	bool HasResult() const {
		return optional_result != nullptr;
	}

	ParseResult &GetResultUnsafe() {
		D_ASSERT(optional_result);
		return *optional_result;
	}

	ParseResult &GetResult() {
		if (!optional_result) {
			throw InternalException("OptionalParseResult is null");
		}
		return *optional_result;
	}

	void ToStringInternal(std::stringstream &ss, std::unordered_set<const ParseResult *> &visited,
	                      const std::string &indent, bool is_last) const override {
		if (HasResult()) {
			// The optional node has a value, so we "collapse" it by just printing its child.
			// We pass the same indentation and is_last status, so it takes the place of the Optional node.
			optional_result->ToStringInternal(ss, visited, indent, is_last);
		} else {
			// The optional node is empty, which is useful information, so we print it.
			ss << indent << (is_last ? "└─" : "├─") << " " << ParseResultToString(type) << " [empty]\n";
		}
	}

private:
	optional_ptr<ParseResult> optional_result;
};

class ChoiceParseResult : public ParseResult {
public:
	static constexpr ParseResultType TYPE = ParseResultType::CHOICE;

	explicit ChoiceParseResult(ParseResult &parse_result_p, idx_t selected_idx_p, optional_idx offset)
	    : ParseResult(TYPE, offset), result(parse_result_p), selected_idx(selected_idx_p) {
		SetNameFrom(parse_result_p);
		EncloseChild(parse_result_p);
	}

	ParseResult &GetResult() {
		return result;
	}

	void ToStringInternal(std::stringstream &ss, std::unordered_set<const ParseResult *> &visited,
	                      const std::string &indent, bool is_last) const override {
		// The choice was resolved. We print a marker and then print the child below it.
		ss << indent << (is_last ? "└─" : "├─") << " [" << ParseResultToString(type) << " (idx: " << selected_idx
		   << ")] ->\n";

		// The child is now on a new indentation level and is the only child of our marker.
		std::string child_indent = indent + (is_last ? "   " : "│  ");
		result.ToStringInternal(ss, visited, child_indent, true);
	}

private:
	ParseResult &result;
	idx_t selected_idx;
};

//! Whether the parse result allocator has to keep a pointer to a node of exactly this type, to destroy it before the
//! arena is dropped. Most node types only hold spans and pointers into the arena and can be left alone. This is a
//! trait on the exact type rather than a member, so a subclass that adds an owning member does not inherit the opt-out.
template <class RESULT>
struct ParseResultNeedsDestructor : std::true_type {};
template <>
struct ParseResultNeedsDestructor<EndOfInputParseResult> : std::false_type {};
template <>
struct ParseResultNeedsDestructor<ListParseResult> : std::false_type {};
template <>
struct ParseResultNeedsDestructor<RepeatParseResult> : std::false_type {};
template <>
struct ParseResultNeedsDestructor<OptionalParseResult> : std::false_type {};
template <>
struct ParseResultNeedsDestructor<ChoiceParseResult> : std::false_type {};

class NumberParseResult : public ParseResult {
public:
	static constexpr ParseResultType TYPE = ParseResultType::NUMBER;

	explicit NumberParseResult(string number_p, optional_idx offset, optional_idx length = optional_idx())
	    : ParseResult(TYPE, offset, length), number(std::move(number_p)) {
	}
	string number;

	void ToStringInternal(std::stringstream &ss, std::unordered_set<const ParseResult *> &visited,
	                      const std::string &indent, bool is_last) const override {
		ParseResult::ToStringInternal(ss, visited, indent, is_last);
		ss << ": " << number << "\n";
	}
};

class StringLiteralParseResult : public ParseResult {
public:
	static constexpr ParseResultType TYPE = ParseResultType::STRING;

	explicit StringLiteralParseResult(string string_p, SpecialStringCharacter string_type_p, optional_idx offset,
	                                  optional_idx length = optional_idx())
	    : ParseResult(TYPE, offset, length), result(std::move(string_p)), string_type(string_type_p) {
	}

	string GetRawString() const {
		return result;
	}

	virtual unique_ptr<ParsedExpression> ToExpression() {
		switch (string_type) {
		case SpecialStringCharacter::STANDARD:
			return ConstantExpression::String(result);
		case SpecialStringCharacter::NATIONAL_STRING:
			return make_uniq<CastExpression>(LogicalType::VARCHAR, ConstantExpression::String(result));
		case SpecialStringCharacter::HEXADECIMAL_STRING:
			// result contains raw hex digits (e.g. "FF" for X'FF')
			return ConstantExpression::Hex(result);
		case SpecialStringCharacter::BIT_STRING:
			return ConstantExpression::Bit(result);
		case SpecialStringCharacter::ESCAPE_STRING:
			string escaped_result;
			escaped_result.reserve(result.size());

			for (size_t i = 0; i < result.size(); ++i) {
				if (result[i] == '\\' && i + 1 < result.size()) {
					i++;
					switch (result[i]) {
					case 'b':
						escaped_result += '\b';
						break;
					case 'f':
						escaped_result += '\f';
						break;
					case '0':
					case '1':
					case '2':
					case '3':
					case '4':
					case '5':
					case '6':
					case '7': {
						size_t oct_start = i;
						size_t oct_end = oct_start + 1;
						while (oct_end < result.size() && oct_end - oct_start < 3 && result[oct_end] >= '0' &&
						       result[oct_end] <= '7') {
							oct_end++;
						}
						string oct_str = result.substr(oct_start, oct_end - oct_start);
						escaped_result += static_cast<char>(strtoul(oct_str.c_str(), nullptr, 8));
						i = oct_end - 1;
						break;
					}
					case 'x': {
						size_t hex_start = i + 1;
						size_t hex_end = hex_start;
						while (hex_end < result.size() && hex_end - hex_start < 2 &&
						       StringUtil::CharacterIsHex(result[hex_end])) {
							hex_end++;
						}
						if (hex_end > hex_start) {
							string hex_str = result.substr(hex_start, hex_end - hex_start);
							escaped_result += static_cast<char>(strtoul(hex_str.c_str(), nullptr, 16));
							i = hex_end - 1;
						} else {
							escaped_result += 'x';
						}
						break;
					}
					case 'n':
						escaped_result += '\n';
						break;
					case 't':
						escaped_result += '\t';
						break;
					case 'r':
						escaped_result += '\r';
						break;
					case '\\':
						escaped_result += '\\';
						break;
					case '\'':
						escaped_result += '\'';
						break;
					default:
						escaped_result += result[i];
						break;
					}
				} else {
					escaped_result += result[i];
				}
			}
			if (escaped_result.find('\0') != string::npos) {
				throw ParserException("Null character not permitted in escape string literal");
			}
			UnicodeInvalidReason reason;
			size_t pos;
			auto utf_validity = Utf8Proc::Analyze(escaped_result.c_str(), escaped_result.size(), &reason, &pos);
			if (utf_validity == UnicodeType::INVALID) {
				const char *reason_str =
				    reason == UnicodeInvalidReason::BYTE_MISMATCH ? "byte mismatch" : "invalid unicode codepoint";
				throw ParserException("Invalid UTF-8 in escape string literal at byte offset %d: %s", pos, reason_str);
			}
			return ConstantExpression::String(escaped_result);
		}
		return ConstantExpression::String(result);
	}

	string result;

	SpecialStringCharacter string_type;

	void ToStringInternal(std::stringstream &ss, std::unordered_set<const ParseResult *> &visited,
	                      const std::string &indent, bool is_last) const override {
		ParseResult::ToStringInternal(ss, visited, indent, is_last);
		string special_string;
		if (string_type == SpecialStringCharacter::ESCAPE_STRING) {
			special_string = "E";
		} else if (string_type == SpecialStringCharacter::NATIONAL_STRING) {
			special_string = "N";
		} else if (string_type == SpecialStringCharacter::HEXADECIMAL_STRING) {
			special_string = "X";
		}
		ss << ": " << special_string << "\"" << result << "\"n";
	}
};

class OperatorParseResult : public ParseResult {
public:
	static constexpr ParseResultType TYPE = ParseResultType::OPERATOR;

	explicit OperatorParseResult(string operator_p, optional_idx offset, optional_idx length = optional_idx())
	    : ParseResult(TYPE, offset, length), operator_token(std::move(operator_p)) {
	}
	string operator_token;

	void ToStringInternal(std::stringstream &ss, std::unordered_set<const ParseResult *> &visited,
	                      const std::string &indent, bool is_last) const override {
		ParseResult::ToStringInternal(ss, visited, indent, is_last);
		ss << ": " << operator_token << "\n";
	}
};

} // namespace duckdb
