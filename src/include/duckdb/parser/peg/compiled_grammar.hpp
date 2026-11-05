#pragma once

#include "duckdb/common/pair.hpp"
#include "duckdb/parser/peg/matcher.hpp"
#include "duckdb/parser/peg/parsed_grammar.hpp"

namespace duckdb {

class ClientContext;
class DialectExtension;
class GrammarExtension;
class PassthroughDialect;

using compiled_rules_map_t = case_insensitive_map_t<unique_ptr<CompiledGrammarRule>>;

struct CompiledGrammar {
public:
	CompiledGrammar(MatcherAllocator &&allocator, unique_ptr<PEGKeywordHelper> &&keyword_helper,
	                unique_ptr<Tokenizer> &&tokenizer, compiled_rules_map_t &&rules, const Matcher &program_matcher,
	                const Matcher &top_level_statement_matcher);
	static shared_ptr<CompiledGrammar> Create(const vector<reference<GrammarExtension>> &grammar_extensions);

public:
	const Matcher &ProgramMatcher() const {
		return program_matcher;
	}
	const Matcher &TopLevelStatementMatcher() const {
		return top_level_statement_matcher;
	}
	const PEGKeywordHelper &GetKeywordHelper() const {
		return *keyword_helper;
	}
	const Tokenizer &GetTokenizer() const {
		return *tokenizer;
	}
	optional_ptr<const CompiledGrammarRule> GetRule(const string &rule_name) const;
	optional_ptr<const CompiledGrammarRule> GetRule(PEGRuleId rule_id) const;

public:
	static shared_ptr<CompiledGrammar> Get(ClientContext &context);
	//! Get the shared, lazily compiled base DuckDB grammar.
	static shared_ptr<CompiledGrammar> DefaultGrammar();
	//! Compile the base DuckDB grammar.
	static shared_ptr<CompiledGrammar> Create();
	//! Get the (cached) grammar for the selected extensions without changing the client configuration.
	static shared_ptr<CompiledGrammar> Create(const ClientContext &context, const vector<string> &active_extensions);

private:
	MatcherAllocator allocator;
	unique_ptr<PEGKeywordHelper> keyword_helper;
	unique_ptr<Tokenizer> tokenizer;
	case_insensitive_map_t<unique_ptr<CompiledGrammarRule>> rules;
	//! Rules indexed by PEGRuleId; built-in slots stay empty when the grammar does not define the rule
	vector<optional_ptr<const CompiledGrammarRule>> rules_by_id;
	const Matcher &program_matcher;
	const Matcher &top_level_statement_matcher;
};

//! Per-database holder for the compiled base grammar and the grammars compiled for active grammar extensions.
struct ParserCache {
public:
	ParserCache();
	~ParserCache();

	//! The maximum number of grammars compiled for grammar extensions that are kept
	static constexpr idx_t MAX_CACHED_EXTENSION_MATCHERS = 16;

	shared_ptr<CompiledGrammar> GetMatcher();
	//! The grammar compiled for the given grammar extensions, in order - shared by all connections that use them
	shared_ptr<CompiledGrammar> GetMatcher(const ClientContext &context, const vector<string> &active_extensions);
	//! The grammar that forwards statements instead of interpreting them, used while CONNECT-ed
	shared_ptr<CompiledGrammar> GetPassthroughMatcher(const ClientContext &context);

private:
	std::mutex mutex;
	shared_ptr<CompiledGrammar> matcher;
	std::mutex extension_mutex;
	//! The compiled grammars with the names of their grammar extensions in order, from least to most recently used
	vector<pair<vector<string>, shared_ptr<CompiledGrammar>>> extension_matchers;
	std::mutex passthrough_mutex;
	unique_ptr<PassthroughDialect> passthrough_dialect;
};

} // namespace duckdb
