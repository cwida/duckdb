#pragma once

#include "duckdb/common/string_map_set.hpp"
#include "duckdb/common/optional.hpp"
#include "duckdb/common/queue.hpp"
#include "duckdb/parser/peg/matcher/list.hpp"

namespace duckdb {
struct CompiledGrammar;
struct PEGExpression;
class MatcherFactory;
struct CompiledGrammarRule;

using compiled_rules_map_t = case_insensitive_map_t<unique_ptr<CompiledGrammarRule>>;

//! Class for building matchers
class MatcherFactory {
private:
	struct MatcherConstructionState {
		void Register(string_t rule_name);
		void Schedule(string_t rule_name);
		bool Begin(string_t rule_name);
		bool HasScheduled() const;
		string_t TakeNext();

	private:
		string_set_t unconstructed;
		string_set_t scheduled;
		queue<string_t> pending;
	};

public:
	MatcherFactory(MatcherAllocator &allocator, const ParsedGrammar &grammar_p, const compiled_rules_map_t &rules,
	               const PEGKeywordHelper &keyword_helper, terminal_rule_overrides_t terminal_rule_overrides_p);
	virtual ~MatcherFactory() = default;

public:
	Matcher &CreateRootMatcher(const string &root_rule);
	//! Look up a matcher for a rule that was built by CreateRootMatcher. Throws if the rule has not been built.
	Matcher &GetMatcher(const string &rule_name);

protected:
	// Base primitives
	KeywordMatcher &Keyword(const string &keyword) const;
	ListMatcher &List() const;
	ListMatcher &List(vector<reference<Matcher>> matchers) const;
	ChoiceMatcher &Choice(vector<reference<Matcher>> &&matchers) const;
	OptionalMatcher &Optional(Matcher &matcher) const;
	RepeatMatcher &Repeat(Matcher &matcher) const;

	virtual unique_ptr<KeywordMatcher> CreateKeyword(const string &keyword, const KeywordInfo &info) const;
	virtual unique_ptr<ListMatcher> CreateList() const;
	virtual unique_ptr<ChoiceMatcher> CreateChoice(vector<reference<Matcher>> &&matchers) const;
	virtual unique_ptr<OptionalMatcher> CreateOptional(Matcher &matcher) const;
	virtual unique_ptr<RepeatMatcher> CreateRepeat(Matcher &matcher) const;

	void AddKeywordOverride(const char *name, KeywordInfo keyword_info);
	void AddRuleOverride(const char *name, unique_ptr<Matcher> &&matcher_p);
	void AddPackratMemoizedRule(const char *name);
	void SuppressSuggestions(const char *name);
	void LabelRule(const char *name, const char *label);
	Matcher &CreateMatcher(string_t rule_name);
	Matcher &CreateMatcher(string_t rule_name, vector<reference<Matcher>> &parameters);
	Matcher &CreateMatcher(const PEGExpression &expression, const string_map_t<idx_t> &parameter_map,
	                       vector<reference<Matcher>> &parameters);

private:
	optional_ptr<const CompiledGrammarRule> GetRule(const string &name) const;

private:
	MatcherAllocator &allocator;
	const ParsedGrammar &grammar;
	const compiled_rules_map_t &rules;
	const PEGKeywordHelper &keyword_helper;
	//! Keeps terminal rule names alive while the matcher graph is constructed.
	terminal_rule_overrides_t terminal_rule_overrides;
	string_map_t<reference<Matcher>> matchers;
	MatcherConstructionState construction_state;
	mutable case_insensitive_map_t<reference<KeywordMatcher>> keywords;
	case_insensitive_map_t<KeywordInfo> keyword_overrides;
	string_set_t no_suggestion_rules;
	string_set_t packrat_memoized_rules;
	string_map_t<string> rule_labels;
};

} // namespace duckdb
