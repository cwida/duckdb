#include "duckdb/parser/peg/keyword_helper/parsed_grammar_keyword_helper.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/peg/parsed_grammar.hpp"

namespace duckdb {

static void PopulateKeywordMap(const ParsedGrammar &grammar, const string &root_rule_name, const string &rule_name,
                               case_insensitive_set_t &keyword_map, case_insensitive_set_t &active_rules);

static void ExpressionToKeyword(const ParsedGrammar &grammar, const string &root_rule_name, const string &rule_name,
                                const PEGExpression &expression, case_insensitive_set_t &keyword_map,
                                case_insensitive_set_t &active_rules) {
	if (expression.type == PEGExpression::Type::LITERAL) {
		keyword_map.insert(StringUtil::Lower(expression.text.GetString()));
	} else if (expression.type == PEGExpression::Type::REFERENCE) {
		PopulateKeywordMap(grammar, root_rule_name, expression.text.GetString(), keyword_map, active_rules);
	} else if (expression.type == PEGExpression::Type::CHOICE) {
		for (auto &child : expression.children) {
			ExpressionToKeyword(grammar, root_rule_name, rule_name, child, keyword_map, active_rules);
		}
	} else {
		throw InvalidInputException("Keyword grammar rule '%s' contains unsupported token '%s' in rule '%s'",
		                            root_rule_name, expression.text.GetString(), rule_name);
	}
}

static void PopulateKeywordMap(const ParsedGrammar &grammar, const string &root_rule_name, const string &rule_name,
                               case_insensitive_set_t &keyword_map, case_insensitive_set_t &active_rules) {
	if (!active_rules.insert(rule_name).second) {
		throw InvalidInputException("Keyword grammar rule '%s' contains a recursive reference to rule '%s'",
		                            root_rule_name, rule_name);
	}
	auto rule_p = grammar.GetRule(rule_name);
	if (!rule_p) {
		throw InvalidInputException("No registered data exists for keyword rule '%s'", rule_name);
	}
	auto &rule = *rule_p;
	if (!rule.recipe.parameters.empty()) {
		throw InvalidInputException("Keyword grammar rule '%s' references parameterized rule '%s'", root_rule_name,
		                            rule_name);
	}

	ExpressionToKeyword(grammar, root_rule_name, rule_name, rule.recipe.expression, keyword_map, active_rules);
	active_rules.erase(rule_name);
}

static DefaultKeywordMaps BuildKeywordMaps(const ParsedGrammar &grammar) {
	DefaultKeywordMaps keyword_maps;
	struct KeywordRuleMapping {
		const char *rule_name;
		reference<case_insensitive_set_t> keyword_map;
		bool optional;
	};
	// grammars without a bare-label rule only accept non-keywords as bare labels
	const KeywordRuleMapping mapping[] = {
	    {"ReservedKeyword", keyword_maps.reserved_keyword_map, false},
	    {"UnreservedKeyword", keyword_maps.unreserved_keyword_map, false},
	    {"ColumnNameKeyword", keyword_maps.colname_keyword_map, false},
	    {"TypeFuncKeyword", keyword_maps.typefunc_keyword_map, false},
	    {"TypeNameKeyword", keyword_maps.typename_keyword_map, false},
	    {"BareLabelKeyword", keyword_maps.bare_label_keyword_map, true},
	};
	for (auto &entry : mapping) {
		if (entry.optional && !grammar.GetRule(entry.rule_name)) {
			continue;
		}
		case_insensitive_set_t active_rules;
		PopulateKeywordMap(grammar, entry.rule_name, entry.rule_name, entry.keyword_map.get(), active_rules);
	}
	// the bare-label classification is orthogonal: every entry must also be an unreserved or column-name keyword
	for (auto &keyword : keyword_maps.bare_label_keyword_map) {
		if (!keyword_maps.unreserved_keyword_map.count(keyword) && !keyword_maps.colname_keyword_map.count(keyword)) {
			throw InvalidInputException("Bare label keyword '%s' is not an unreserved or column-name keyword", keyword);
		}
	}
	return keyword_maps;
}

ParsedGrammarKeywordHelper::ParsedGrammarKeywordHelper(const ParsedGrammar &grammar)
    : keyword_maps(BuildKeywordMaps(grammar)), literal_table(grammar, keyword_maps.ToLiteralMap()) {
}

keyword_categories_t ParsedGrammarKeywordHelper::GetIdentifierMask(SuggestionState type) const {
	return DefaultKeywordMaps::GetIdentifierMask(type);
}

vector<ParserKeyword> ParsedGrammarKeywordHelper::KeywordList() const {
	return keyword_maps.ToList();
}

} // namespace duckdb
