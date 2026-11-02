#include "duckdb/parser/peg/dialect_extension.hpp"
#include "duckdb/parser/peg/keyword_helper/parsed_grammar_keyword_helper.hpp"
#include "duckdb/parser/peg/compiled_grammar.hpp"
#include "duckdb/parser/peg/tokenizer/tokenizer.hpp"
#include "duckdb/parser/peg/matcher_factory.hpp"

namespace duckdb {

const string &DialectExtension::Name() const {
	return name;
}

const optional<DialectCompatibilityMode> &DialectExtension::GetCompatibilityMode() const {
	return compatibility_mode;
}

shared_ptr<CompiledGrammar> DialectExtension::GetCompiledGrammar(const ClientContext &context) {
	{
		lock_guard<mutex> guard(lock);
		if (cache) {
			return cache;
		}
	}

	//! Grammar changes
	auto parsed_grammar = ParsedGrammar::CreateDefault();
	GrammarChangesInput grammar_change_input {parsed_grammar, context};
	ApplyGrammarChanges(grammar_change_input);

	//! Keyword Helper
	CreateKeywordHelperInput keyword_helper_input {parsed_grammar};
	auto keyword_helper = CreateKeywordHelper(keyword_helper_input);

	//! Tokenizer
	CreateTokenizerInput tokenizer_input {*keyword_helper};
	auto tokenizer = CreateTokenizer(tokenizer_input);

	//! MatcherFactory
	MatcherAllocator allocator;
	compiled_rules_map_t rules;
	for (auto &entry : parsed_grammar.rules) {
		auto &rule = *entry.second;
		rules.emplace(rule.name, make_uniq<CompiledGrammarRule>(rule.name, rule.transform_process, rule.collapsible,
		                                                        rule.expression_depth, rule.suppress_suggestions));
	}
	auto terminal_rule_overrides = parsed_grammar.BuildTerminalRuleOverrides(*keyword_helper);
	CreateMatcherFactoryInput matcher_factory_input {allocator, parsed_grammar, rules, *keyword_helper,
	                                                 std::move(terminal_rule_overrides)};
	auto matcher_factory = CreateMatcherFactory(matcher_factory_input);

	auto &program_matcher = matcher_factory->CreateRootMatcher("Program");
	auto &top_level_statement_matcher = matcher_factory->GetMatcher("TopLevelStatement");

	auto result = shared_ptr<CompiledGrammar>(new CompiledGrammar(std::move(allocator), std::move(keyword_helper),
	                                                              std::move(tokenizer), std::move(rules),
	                                                              program_matcher, top_level_statement_matcher));

	lock_guard<mutex> guard(lock);
	if (!cache) {
		cache = result;
	}
	return cache;
}

void DialectExtension::ApplyGrammarChanges(GrammarChangesInput &) {
	return;
}

unique_ptr<MatcherFactory> DialectExtension::CreateMatcherFactory(CreateMatcherFactoryInput &input) {
	return make_uniq<MatcherFactory>(input.allocator, input.parsed_grammar, input.rules, input.keyword_helper,
	                                 std::move(input.terminal_rules));
}

unique_ptr<Tokenizer> DialectExtension::CreateTokenizer(CreateTokenizerInput &input) {
	auto &keyword_helper = input.keyword_helper;
	return make_uniq<Tokenizer>(keyword_helper);
}

unique_ptr<PEGKeywordHelper> DialectExtension::CreateKeywordHelper(CreateKeywordHelperInput &input) {
	auto &parsed_grammar = input.grammar;
	return make_uniq<ParsedGrammarKeywordHelper>(parsed_grammar);
}

} // namespace duckdb
