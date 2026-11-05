#pragma once
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/string_map_set.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/windows_undefs.hpp"

namespace duckdb {

struct PEGExpression {
	enum class Type : uint8_t {
		LITERAL,         // keyword/string literal
		REFERENCE,       // reference to a parameter or another rule
		FUNCTION_CALL,   // function-call marker wrapping a child
		SEQUENCE,        // ordered "AND" of children
		CHOICE,          // "OR" between children ('/')
		OPTIONAL,        // child?
		REPEAT,          // child+  (one or more)
		OPTIONAL_REPEAT, // child*  (zero or more)
		REGEX            // regex
	};

public:
	explicit PEGExpression(Type type_p) : type(type_p), text(string_t("")) {
	}
	PEGExpression(Type type_p, string_t text_p) : type(type_p), text(text_p) {
	}

public:
	Type type;
	//! literal text / reference name / function name
	string_t text;
	//! used by SEQUENCE, CHOICE, OPTIONAL, REPEAT*, FUNCTION_CALL
	vector<PEGExpression> children;
};

struct PEGRule {
public:
	PEGRule(string_map_t<idx_t> &&parameters, PEGExpression &&expression)
	    : parameters(std::move(parameters)), expression(std::move(expression)) {
	}

public:
	string_map_t<idx_t> parameters;
	PEGExpression expression;
};

struct PEGParser {
public:
	void ParseRules(const char *grammar);
	void AddRule(string_t rule_name, PEGRule &&rule);

	case_insensitive_map_t<PEGRule> rules;
};

inline bool IsPEGOperator(char c) {
	switch (c) {
	case '/':
	case '?':
	case '(':
	case ')':
	case '*':
	case '+':
	case '!':
		return true;
	default:
		return false;
	}
}

} // namespace duckdb
