//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/peg/dialect_extension.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/optional.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/enums/dialect_compatibility_mode.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {
struct DBConfig;
class MatcherFactory;
class Tokenizer;
class PEGKeywordHelper;
class MatcherAllocator;
class ParsedGrammar;
struct CompiledGrammar;
struct CompiledGrammarRule;
class Matcher;

using compiled_rules_map_t = case_insensitive_map_t<unique_ptr<CompiledGrammarRule>>;

using terminal_rule_overrides_t = case_insensitive_map_t<unique_ptr<Matcher>>;

struct CreateTokenizerInput {
	const PEGKeywordHelper &keyword_helper;
};

struct CreateMatcherFactoryInput {
	MatcherAllocator &allocator;
	const ParsedGrammar &parsed_grammar;
	const compiled_rules_map_t &rules;
	const PEGKeywordHelper &keyword_helper;
	terminal_rule_overrides_t terminal_rules;
};

struct CreateKeywordHelperInput {
	const ParsedGrammar &grammar;
};

struct GrammarChangesInput {
	ParsedGrammar &parsed_grammar;
	const ClientContext &context;
};

//! A named SQL dialect that can customize the PEG parser.
class DialectExtension {
public:
	explicit DialectExtension(string name_p, optional<DialectCompatibilityMode> compatibility_mode = std::nullopt)
	    : name(std::move(name_p)), compatibility_mode(compatibility_mode) {
	}
	virtual ~DialectExtension() = default;

public:
	static void Register(DBConfig &config, shared_ptr<DialectExtension> extension);

public:
	shared_ptr<CompiledGrammar> GetCompiledGrammar(const ClientContext &context);
	const string &Name() const;
	const optional<DialectCompatibilityMode> &GetCompatibilityMode() const;

public:
	virtual void ApplyGrammarChanges(GrammarChangesInput &input);
	virtual unique_ptr<MatcherFactory> CreateMatcherFactory(CreateMatcherFactoryInput &input);
	virtual unique_ptr<Tokenizer> CreateTokenizer(CreateTokenizerInput &input);
	virtual unique_ptr<PEGKeywordHelper> CreateKeywordHelper(CreateKeywordHelperInput &input);

private:
	//! The name to reference this dialect with the 'current_dialect' setting.
	string name;
	//! If set, the name of the 'dialect_compatibility_mode' setting to use upon selection.
	optional<DialectCompatibilityMode> compatibility_mode;
	mutex lock;
	//! Cache the compiled grammar, since it never needs to be invalidated
	shared_ptr<CompiledGrammar> cache;
};

} // namespace duckdb
