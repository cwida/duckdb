#include "duckdb/parser/peg/parsed_grammar.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/peg/matcher.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#ifdef PEG_PARSER_SOURCE_FILE
#include <fstream>
#else
#include "duckdb/parser/peg/inlined_grammar.hpp"
#endif

namespace duckdb {

ParsedGrammar::ParsedGrammar(ParsedGrammar &&other) noexcept
    : rules(std::move(other.rules)),
      terminal_rule_override_callbacks(std::move(other.terminal_rule_override_callbacks)) {
	string_heap.Move(other.string_heap);
}

ParsedGrammar &ParsedGrammar::operator=(ParsedGrammar &&other) noexcept {
	if (this != &other) {
		rules.clear();
		terminal_rule_override_callbacks.clear();
		string_heap.Destroy();
		string_heap.Move(other.string_heap);
		rules = std::move(other.rules);
		terminal_rule_override_callbacks = std::move(other.terminal_rule_override_callbacks);
	}
	return *this;
}

ParsedGrammar ParsedGrammar::Parse(const string &grammar) {
	PEGParser parser;
	parser.ParseRules(grammar.c_str());
	ParsedGrammar result;
	for (auto &entry : parser.rules) {
		result.AddParsedRule(ParsedGrammarRule(entry.first, std::move(entry.second)));
	}
	return result;
}

ParsedGrammar ParsedGrammar::CreateDefault() {
#ifdef PEG_PARSER_SOURCE_FILE
	std::ifstream t(PEG_PARSER_SOURCE_FILE);
	std::stringstream buffer;
	buffer << t.rdbuf();
	auto grammar_string = buffer.str();

	const char *grammar = grammar_string.c_str();
#else
	const char *grammar = const_char_ptr_cast(INLINED_PEG_GRAMMAR);
#endif
	auto result = Parse(grammar);
	PEGTransformerFactory::RegisterDefaultTransforms(result);
	return result;
}

optional_ptr<const ParsedGrammarRule> ParsedGrammar::GetRule(const string &rule_name) const {
	auto entry = rules.find(rule_name);
	if (entry == rules.end()) {
		return nullptr;
	}
	return *entry->second;
}
ParsedGrammarRule &ParsedGrammar::GetMutableRule(const string &rule_name) {
	auto entry = rules.find(rule_name);
	if (entry == rules.end()) {
		throw InvalidInputException("Grammar rule '%s' does not exist", rule_name);
	}
	return *entry->second;
}

ParsedGrammarRule ParsedGrammar::ParseSingleRule(const string &rule_definition) {
	PEGParser parser;
	parser.ParseRules(rule_definition.c_str());
	if (parser.rules.size() != 1) {
		throw InvalidInputException("Expected exactly one PEG rule definition");
	}
	auto &entry = *parser.rules.begin();
	return ParsedGrammarRule(entry.first, std::move(entry.second));
}

static void RegisterText(StringHeap &string_heap, PEGExpression &expression) {
	expression.text = string_heap.AddString(expression.text);
	for (auto &child : expression.children) {
		RegisterText(string_heap, child);
	}
}

void ParsedGrammar::RegisterStrings(PEGRule &rule) {
	string_map_t<idx_t> parameters;
	for (auto &entry : rule.parameters) {
		parameters.emplace(string_heap.AddString(entry.first), entry.second);
	}
	rule.parameters = std::move(parameters);
	RegisterText(string_heap, rule.expression);
}

void ParsedGrammar::AddParsedRule(ParsedGrammarRule rule) {
	if (GetRule(rule.name)) {
		throw InvalidInputException("Grammar rule '%s' already exists", rule.name);
	}
	RegisterStrings(rule.recipe);
	auto name = rule.name;
	rules.emplace(std::move(name), make_uniq<ParsedGrammarRule>(std::move(rule)));
}

void ParsedGrammar::AddRule(const string &rule_definition, grammar_transform_process_function_t transform_process,
                            bool suppress_suggestions) {
	auto rule = ParseSingleRule(rule_definition);
	rule.transform_process = std::move(transform_process);
	rule.suppress_suggestions = suppress_suggestions;
	AddParsedRule(std::move(rule));
}

static idx_t FindChoice(const ParsedGrammarRule &rule, const grammar_cursor_function_t &find_cursor) {
	for (idx_t child_idx = 0; child_idx < rule.recipe.expression.children.size(); child_idx++) {
		auto &expression = rule.recipe.expression.children[child_idx];
		if (!find_cursor(expression)) {
			continue;
		}
		return child_idx;
	}
	throw InvalidInputException("Could not find a choice cursor in grammar rule '%s'", rule.name);
}

static idx_t FindChoiceCursor(const ParsedGrammarRule &rule, const grammar_cursor_function_t &find_cursor,
                              bool prepend) {
	if (!find_cursor) {
		return prepend ? 0 : rule.recipe.expression.children.size();
	}
	return FindChoice(rule, find_cursor) + (prepend ? 0 : 1);
}

void ParsedGrammar::InsertChoice(const string &rule_name, const string &choice,
                                 const grammar_cursor_function_t &find_cursor, bool prepend) {
	auto choice_definition = StringUtil::Format("Choice <- %s", choice);
	auto choice_rule = ParseSingleRule(choice_definition);
	auto &rule = GetMutableRule(rule_name);
	RegisterStrings(choice_rule.recipe);
	if (rule.recipe.expression.type != PEGExpression::Type::CHOICE) {
		//! Wrap in CHOICE beforehand
		PEGExpression choice_expression(PEGExpression::Type::CHOICE, "/");
		choice_expression.children.push_back(rule.recipe.expression);
		rule.recipe.expression = std::move(choice_expression);
	}
	auto cursor = FindChoiceCursor(rule, find_cursor, prepend);

	vector<PEGExpression> children;
	children.reserve(rule.recipe.expression.children.size() + choice_rule.recipe.expression.children.size() + 1);
	for (idx_t child_idx = 0; child_idx < cursor; child_idx++) {
		children.push_back(rule.recipe.expression.children[child_idx]);
	}
	children.push_back(std::move(choice_rule.recipe.expression));
	for (idx_t child_idx = cursor; child_idx < rule.recipe.expression.children.size(); child_idx++) {
		children.push_back(rule.recipe.expression.children[child_idx]);
	}
	rule.recipe.expression.children = std::move(children);
}

void ParsedGrammar::AddChoice(const string &rule_name, const string &choice,
                              const grammar_cursor_function_t &find_cursor) {
	InsertChoice(rule_name, choice, find_cursor, false);
}

void ParsedGrammar::PrependChoice(const string &rule_name, const string &choice,
                                  const grammar_cursor_function_t &find_cursor) {
	InsertChoice(rule_name, choice, find_cursor, true);
}

void ParsedGrammar::ReplaceChoice(const string &rule_name, const string &choice,
                                  const grammar_cursor_function_t &find_cursor) {
	if (!find_cursor) {
		throw InvalidInputException("ReplaceChoice requires a choice cursor");
	}

	auto &rule = GetMutableRule(rule_name);
	if (rule.recipe.expression.type != PEGExpression::Type::CHOICE) {
		throw InvalidInputException("Grammar rule '%s' does not contain a choice", rule.name);
	}

	auto choice_definition = StringUtil::Format("Choice <- %s", choice);
	auto choice_rule = ParseSingleRule(choice_definition);
	RegisterStrings(choice_rule.recipe);

	auto cursor = FindChoiceCursor(rule, find_cursor, true);
	rule.recipe.expression.children[cursor] = std::move(choice_rule.recipe.expression);
}

void ParsedGrammar::RemoveChoice(const string &rule_name, const grammar_cursor_function_t &find_cursor) {
	if (!find_cursor) {
		throw InvalidInputException("RemoveChoice requires a choice cursor");
	}
	auto &rule = GetMutableRule(rule_name);
	if (rule.recipe.expression.type != PEGExpression::Type::CHOICE) {
		throw InvalidInputException("Grammar rule '%s' does not contain a choice", rule.name);
	}
	auto &children = rule.recipe.expression.children;
	if (children.size() <= 1) {
		throw InternalException(
		    "Choice rule '%s' has %d children, this shouldn't happen, minimum children for CHOICE is 2", rule.name,
		    children.size());
	}
	auto cursor = FindChoice(rule, find_cursor);
	children.erase_at(cursor);
	if (children.size() == 1) {
		auto remaining_choice = std::move(children[0]);
		rule.recipe.expression = std::move(remaining_choice);
	}
}

void ParsedGrammar::ReplaceRule(const string &rule_definition, grammar_transform_process_function_t transform_process) {
	auto rule = ParseSingleRule(rule_definition);
	auto entry = rules.find(rule.name);
	if (entry == rules.end()) {
		throw InvalidInputException("Grammar rule '%s' does not exist", rule.name);
	}
	RegisterStrings(rule.recipe);
	rule.transform_process = std::move(transform_process);
	entry->second = make_uniq<ParsedGrammarRule>(std::move(rule));
}

void ParsedGrammar::SetTransformProcess(const string &rule_name, grammar_transform_process_function_t transform_process,
                                        bool collapsible, bool suppress_suggestions) {
	auto &rule = GetMutableRule(rule_name);
	rule.transform_process = std::move(transform_process);
	rule.collapsible = collapsible;
	rule.suppress_suggestions = suppress_suggestions;
}

void ParsedGrammar::SetExpressionDepth(const string &rule_name, ExpressionDepthKind kind) {
	GetMutableRule(rule_name).expression_depth = kind;
}

void ParsedGrammar::AddTerminalRuleOverride(const string &rule_name, terminal_rule_matcher_factory_t matcher_factory) {
	if (!matcher_factory) {
		throw InvalidInputException("Cannot add an empty terminal rule matcher factory for '%s'", rule_name);
	}
	terminal_rule_override_callbacks.emplace_back(
	    [rule_name, matcher_factory = std::move(matcher_factory)](const PEGKeywordHelper &keyword_helper,
	                                                              terminal_rule_overrides_t &overrides) {
		    AddTerminalRuleOverride(overrides, rule_name, matcher_factory(keyword_helper));
	    });
}

void ParsedGrammar::AddTerminalRuleOverride(terminal_rule_overrides_t &overrides, const string &rule_name,
                                            unique_ptr<Matcher> matcher) {
	if (!matcher) {
		throw InvalidInputException("Cannot add an empty terminal rule override for '%s'", rule_name);
	}
	if (overrides.count(rule_name)) {
		throw InvalidInputException("Terminal rule override for '%s' already exists", rule_name);
	}
	overrides.emplace(rule_name, std::move(matcher));
}

} // namespace duckdb
