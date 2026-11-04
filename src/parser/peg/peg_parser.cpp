#include "duckdb/parser/peg/peg_parser.hpp"
#include "duckdb/parser/peg/parsed_grammar.hpp"
#include "duckdb/parser/peg/inlined_grammar.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/numeric_utils.hpp"

namespace duckdb {

namespace {

enum class PEGTokenType {
	LITERAL,       // literal token ('Keyword')
	REFERENCE,     // reference token (Rule)
	OPERATOR,      // operator token (/ or )
	FUNCTION_CALL, // start of function call (i.e. Function(...))
	REGEX          // regular expression ([ \t\n\r] or <[a-z_]i[a-z0-9_]i>)
};

struct PEGToken {
	PEGTokenType type;
	string_t text;
};

enum class PEGParseState {
	RULE_NAME,      // Rule name
	RULE_SEPARATOR, // look for <-
	RULE_DEFINITION // part of rule definition
};

class PEGExpressionParser {
public:
	explicit PEGExpressionParser(const vector<PEGToken> &tokens_p) : tokens(tokens_p), pos(0) {
	}

	PEGExpression Parse() {
		auto root = ParseChoice();
		if (pos != tokens.size()) {
			throw InternalException("Unexpected trailing tokens in PEG rule");
		}
		return root;
	}

private:
	const vector<PEGToken> &tokens;
	idx_t pos;

	bool HasNext() const {
		return pos < tokens.size();
	}
	const PEGToken &Peek() const {
		return tokens[pos];
	}
	const PEGToken &Advance() {
		return tokens[pos++];
	}
	bool IsOp(char op) const {
		return HasNext() && Peek().type == PEGTokenType::OPERATOR && Peek().text.GetData()[0] == op;
	}

	// choice := sequence ('/' sequence)*
	PEGExpression ParseChoice() {
		auto first = ParseSequence();
		if (!IsOp('/')) {
			return first;
		}
		PEGExpression choice(PEGExpression::Type::CHOICE);
		choice.children.push_back(std::move(first));
		while (IsOp('/')) {
			Advance();
			choice.children.push_back(ParseSequence());
		}
		return choice;
	}

	// sequence := postfix*
	PEGExpression ParseSequence() {
		PEGExpression seq(PEGExpression::Type::SEQUENCE);
		while (HasNext() && !IsOp('/') && !IsOp(')')) {
			seq.children.push_back(ParsePostfix());
		}
		if (seq.children.size() == 1) {
			return std::move(seq.children[0]); // collapse trivial sequences
		}
		return seq;
	}

	// postfix := primary ('?' | '*' | '+')?
	PEGExpression ParsePostfix() {
		auto node = ParsePrimary();
		if (IsOp('?')) {
			Advance();
			PEGExpression n(PEGExpression::Type::OPTIONAL);
			n.children.push_back(std::move(node));
			return n;
		}
		if (IsOp('*')) {
			Advance();
			PEGExpression n(PEGExpression::Type::OPTIONAL_REPEAT);
			n.children.push_back(std::move(node));
			return n;
		}
		if (IsOp('+')) {
			Advance();
			PEGExpression n(PEGExpression::Type::REPEAT);
			n.children.push_back(std::move(node));
			return n;
		}
		return node;
	}

	// primary := '(' choice ')' | literal | reference | function_call
	PEGExpression ParsePrimary() {
		if (IsOp('(')) {
			Advance();
			auto inner = ParseChoice();
			if (!IsOp(')')) {
				throw InternalException("Expected closing ')' in PEG rule");
			}
			Advance();
			if (inner.type == PEGExpression::Type::SEQUENCE) {
				return inner;
			}
			PEGExpression group(PEGExpression::Type::SEQUENCE);
			group.children.push_back(std::move(inner));
			return group;
		}
		if (IsOp('!')) {
			// FIXME: NOT ignored when parsing
			Advance();
			return ParsePrimary();
		}
		if (!HasNext()) {
			throw InternalException("Unexpected end of PEG rule tokens");
		}
		auto &token = Advance();
		switch (token.type) {
		case PEGTokenType::LITERAL:
			return PEGExpression(PEGExpression::Type::LITERAL, token.text);
		case PEGTokenType::REFERENCE:
			return PEGExpression(PEGExpression::Type::REFERENCE, token.text);
		case PEGTokenType::FUNCTION_CALL: {
			PEGExpression fn(PEGExpression::Type::FUNCTION_CALL, token.text);
			auto body = ParseChoice(); // same "consume until close" as '(' does
			if (!IsOp(')')) {
				throw InternalException("Expected closing ')' after function call '%s'", token.text.GetString());
			}
			Advance();
			fn.children.push_back(std::move(body));
			return fn;
		}
		case PEGTokenType::REGEX:
			return PEGExpression(PEGExpression::Type::REGEX, token.text);
		default:
			throw InternalException("unrecognized peg token type");
		}
	}
};

static PEGExpression BuildExpression(vector<PEGToken> &tokens) {
	PEGExpressionParser parser(tokens);
	return parser.Parse();
	tokens.clear();
}

} // namespace

void PEGParser::AddRule(string_t rule_name, PEGRule &&rule) {
	auto entry = rules.find(rule_name.GetString());
	if (entry != rules.end()) {
		throw InternalException("Failed to parse grammar - duplicate rule name %s", rule_name.GetString());
	}
	rules.insert(make_pair(rule_name, std::move(rule)));
}

void PEGParser::ParseRules(const char *grammar) {
	string_t rule_name;
	PEGParseState parse_state = PEGParseState::RULE_NAME;
	idx_t bracket_count = 0;
	vector<PEGToken> tokens;
	string_map_t<idx_t> parameters;
	auto clear_state = [&tokens, &parameters]() {
		tokens.clear();
		parameters.clear();
	};
	bool in_or_clause = false;
	// look for the rules
	idx_t c = 0;
	while (grammar[c]) {
		if (grammar[c] == '#') {
			// comment - ignore until EOL
			while (grammar[c] && !StringUtil::CharacterIsNewline(grammar[c])) {
				c++;
			}
			continue;
		}
		if (parse_state == PEGParseState::RULE_DEFINITION && StringUtil::CharacterIsNewline(grammar[c]) &&
		    bracket_count == 0 && !in_or_clause && !tokens.empty()) {
			// if we see a newline while we are parsing a rule definition we can complete the rule
			auto new_rule = PEGRule(std::move(parameters), BuildExpression(tokens));
			AddRule(rule_name, std::move(new_rule));
			parameters = string_map_t<idx_t>();
			rule_name = string_t();
			// look for the subsequent rule
			parse_state = PEGParseState::RULE_NAME;
			c++;
			continue;
		}
		if (StringUtil::CharacterIsSpace(grammar[c])) {
			// skip whitespace
			c++;
			continue;
		}
		switch (parse_state) {
		case PEGParseState::RULE_NAME: {
			// look for alpha-numerics
			idx_t start_pos = c;
			if (grammar[c] == '%') {
				// rules can start with % (%whitespace)
				c++;
			}
			while (grammar[c] && StringUtil::CharacterIsAlphaNumeric(grammar[c])) {
				c++;
			}
			if (c == start_pos) {
				throw InternalException("Failed to parse grammar - expected an alpha-numeric rule name (pos %d)", c);
			}
			rule_name = string_t(grammar + start_pos, UnsafeNumericCast<uint32_t>(c - start_pos));
			clear_state();
			parse_state = PEGParseState::RULE_SEPARATOR;
			break;
		}
		case PEGParseState::RULE_SEPARATOR: {
			if (grammar[c] == '(') {
				if (!parameters.empty()) {
					throw InternalException("Failed to parse grammar - multiple parameters at position %d", c);
				}
				// parameter
				c++;
				idx_t parameter_start = c;
				while (grammar[c] && StringUtil::CharacterIsAlphaNumeric(grammar[c])) {
					c++;
				}
				if (parameter_start == c) {
					throw InternalException("Failed to parse grammar - expected a parameter at position %d", c);
				}
				parameters.insert(
				    make_pair(string_t(grammar + parameter_start, UnsafeNumericCast<uint32_t>(c - parameter_start)),
				              parameters.size()));
				if (grammar[c] != ')') {
					throw InternalException("Failed to parse grammar - expected closing bracket at position %d", c);
				}
				c++;
			} else {
				if (grammar[c] != '<' || grammar[c + 1] != '-') {
					throw InternalException("Failed to parse grammar - expected a rule definition (<-) (pos %d)", c);
				}
				c += 2;
				parse_state = PEGParseState::RULE_DEFINITION;
			}
			break;
		}
		case PEGParseState::RULE_DEFINITION: {
			// we parse either:
			// (1) a literal ('Keyword'i)
			// (2) a rule reference (Rule)
			// (3) an operator ( '(' '/' '?' '*' ')' '+')
			in_or_clause = false;
			if (grammar[c] == '\'') {
				// parse literal
				c++;
				idx_t literal_start = c;
				while (grammar[c] && grammar[c] != '\'') {
					if (grammar[c] == '\\') {
						// escape
						c++;
					}
					c++;
				}
				if (!grammar[c]) {
					throw InternalException("Failed to parse grammar - did not find closing ' (pos %d)", c);
				}
				PEGToken token;
				token.text = string_t(grammar + literal_start, UnsafeNumericCast<uint32_t>(c - literal_start));
				token.type = PEGTokenType::LITERAL;
				tokens.push_back(token);
				c++;
				if (grammar[c] == 'i') {
					throw InternalException("Failed to parse grammar - unexpected \"i\" found in grammar near rule %s",
					                        rule_name.GetString());
				}
			} else if (StringUtil::CharacterIsAlphaNumeric(grammar[c])) {
				// alphanumeric character - this is a rule reference
				idx_t rule_start = c;
				while (grammar[c] && StringUtil::CharacterIsAlphaNumeric(grammar[c])) {
					c++;
				}
				PEGToken token;
				token.text = string_t(grammar + rule_start, UnsafeNumericCast<uint32_t>(c - rule_start));
				if (grammar[c] == '(') {
					// this is a function call
					c++;
					bracket_count++;
					token.type = PEGTokenType::FUNCTION_CALL;
				} else {
					token.type = PEGTokenType::REFERENCE;
				}
				tokens.push_back(token);
			} else if (grammar[c] == '[' || grammar[c] == '<') {
				// regular expression- [^"] or <...>
				idx_t rule_start = c;
				char final_char = grammar[c] == '[' ? ']' : '>';
				while (grammar[c] && grammar[c] != final_char) {
					if (grammar[c] == '\\') {
						// handle escapes
						c++;
					}
					if (grammar[c]) {
						c++;
					}
				}
				c++;
				PEGToken token;
				token.text = string_t(grammar + rule_start, UnsafeNumericCast<uint32_t>(c - rule_start));
				token.type = PEGTokenType::REGEX;
				tokens.push_back(token);
			} else if (IsPEGOperator(grammar[c])) {
				if (grammar[c] == '(') {
					bracket_count++;
				} else if (grammar[c] == ')') {
					if (bracket_count == 0) {
						throw InternalException("Failed to parse grammar - unclosed bracket at position %d in rule %s",
						                        c, rule_name.GetString());
					}
					bracket_count--;
				} else if (grammar[c] == '/') {
					in_or_clause = true;
				}
				// operator - operators are always length 1
				PEGToken token;
				token.text = string_t(grammar + c, 1);
				token.type = PEGTokenType::OPERATOR;
				tokens.push_back(token);
				c++;
			} else {
				throw InternalException("Unrecognized rule contents in rule %s (character %s)", rule_name.GetString(),
				                        string(1, grammar[c]));
			}
			break;
		}
		default:
			break;
		}
		if (!grammar[c]) {
			break;
		}
	}
	if (parse_state == PEGParseState::RULE_SEPARATOR) {
		throw InternalException("Failed to parse grammar - rule %s does not have a definition", rule_name.GetString());
	}
	if (parse_state == PEGParseState::RULE_DEFINITION) {
		if (tokens.empty()) {
			throw InternalException("Failed to parse grammar - rule %s is empty", rule_name.GetString());
		}
		auto new_rule = PEGRule(std::move(parameters), BuildExpression(tokens));
		AddRule(rule_name, std::move(new_rule));
	}
}

} // namespace duckdb
