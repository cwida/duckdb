//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/peg/parsed_grammar.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/arena_containers/arena_ptr.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/types/string_heap.hpp"
#include "duckdb/parser/peg/peg_parser.hpp"
#include "duckdb/parser/peg/peg_rule_id.hpp"
#include "duckdb/parser/peg/transformer/transform_result.hpp"

#include <functional>

namespace duckdb {

class ParseResult;
class PEGTransformer;
class PEGKeywordHelper;
class DialectExtension;
class Matcher;
class TransformProcess;
class GrammarChange;

using grammar_transform_process_function_t =
    std::function<arena_ptr<TransformProcess>(PEGTransformer &, ParseResult &)>;
using grammar_cursor_function_t = std::function<bool(const PEGExpression &)>;
using terminal_rule_overrides_t = case_insensitive_map_t<unique_ptr<Matcher>>;
using terminal_rule_matcher_factory_t = std::function<unique_ptr<Matcher>(const PEGKeywordHelper &)>;
using terminal_rule_override_callback_t = std::function<void(const PEGKeywordHelper &, terminal_rule_overrides_t &)>;

//! How a rule contributes to the expression depth while it is transformed (see ParsedGrammar::SetExpressionDepth)
enum class ExpressionDepthKind : uint8_t {
	NONE,
	//! one level: the rule nests an expression or type
	NESTING,
	//! one level per repeated element: the rule's repetitions nest its result, like operator chains
	CHAIN
};

struct ParsedGrammarRule {
	ParsedGrammarRule(string name_p, PEGRule recipe_p) : name(std::move(name_p)), recipe(std::move(recipe_p)) {
	}

	string name;
	PEGRule recipe;
	grammar_transform_process_function_t transform_process;
	//! See ParsedGrammar::SetTransformProcess
	bool collapsible = false;
	//! See ParsedGrammar::AddRule
	bool suppress_suggestions = false;
	//! See ParsedGrammar::SetExpressionDepth
	ExpressionDepthKind expression_depth = ExpressionDepthKind::NONE;
};

//! Mutable, owning representation of a PEG grammar before matcher compilation.
class ParsedGrammar {
public:
	ParsedGrammar() = default;
	DUCKDB_API ParsedGrammar(ParsedGrammar &&other) noexcept;
	DUCKDB_API ParsedGrammar &operator=(ParsedGrammar &&other) noexcept;
	ParsedGrammar(const ParsedGrammar &) = delete;
	ParsedGrammar &operator=(const ParsedGrammar &) = delete;

	DUCKDB_API static ParsedGrammar Parse(const string &grammar);
	DUCKDB_API static ParsedGrammar CreateDefault();

	DUCKDB_API optional_ptr<const ParsedGrammarRule> GetRule(const string &rule_name) const;
	//! A rule with suppress_suggestions set contributes no autocomplete suggestions. This keeps catch-all rules, such
	//! as a bare expression accepted as a statement, from polluting completion at the position where they apply.
	DUCKDB_API void AddRule(const string &rule_definition,
	                        grammar_transform_process_function_t transform_process = nullptr,
	                        bool suppress_suggestions = false);
	DUCKDB_API void AddChoice(const string &rule_name, const string &choice,
	                          const grammar_cursor_function_t &find_cursor = nullptr);
	DUCKDB_API void PrependChoice(const string &rule_name, const string &choice,
	                              const grammar_cursor_function_t &find_cursor = nullptr);
	DUCKDB_API void RemoveChoice(const string &rule_name, const grammar_cursor_function_t &find_cursor);
	DUCKDB_API void ReplaceChoice(const string &rule_name, const string &choice,
	                              const grammar_cursor_function_t &find_cursor);
	DUCKDB_API void ReplaceRule(const string &rule_definition,
	                            grammar_transform_process_function_t transform_process = nullptr);
	//! A collapsible transform promises to return its only child's result unchanged whenever the rule matched no
	//! other child with a result, such as a level of the operator precedence hierarchy that matched no tail. The
	//! matcher then skips building a result for the rule and the child is transformed as itself. Setting a new
	//! transform clears that promise unless the caller repeats it, since it is a property of the transform.
	DUCKDB_API void SetTransformProcess(const string &rule_name, grammar_transform_process_function_t transform_process,
	                                    bool collapsible = false, bool suppress_suggestions = false);
	//! Declares how transforming the rule nests the resulting expression, so that max_expression_depth can be enforced
	//! while transforming: the depth of the expression trees is bounded by the levels of the rules on the stack
	DUCKDB_API void SetExpressionDepth(const string &rule_name, ExpressionDepthKind kind);
	DUCKDB_API void AddTerminalRuleOverride(const string &rule_name, terminal_rule_matcher_factory_t matcher_factory);

private:
	friend class DialectExtension;
	friend class MatcherFactory;
	friend class GrammarLiteralTable;
	friend struct CompiledGrammar;
	friend class PEGTransformerFactory;
	friend class GrammarChange;

	void AddParsedRule(ParsedGrammarRule rule);
	void InsertChoice(const string &rule_name, const string &choice, const grammar_cursor_function_t &find_cursor,
	                  bool prepend);
	void RegisterStrings(PEGRule &rule);
	ParsedGrammarRule &GetMutableRule(const string &rule_name);
	static ParsedGrammarRule ParseSingleRule(const string &rule_definition);
	static void AddTerminalRuleOverride(terminal_rule_overrides_t &overrides, const string &rule_name,
	                                    unique_ptr<Matcher> matcher);
	terminal_rule_overrides_t BuildTerminalRuleOverrides(const PEGKeywordHelper &keyword_helper) const;

	StringHeap string_heap;
	case_insensitive_map_t<unique_ptr<ParsedGrammarRule>> rules;
	vector<terminal_rule_override_callback_t> terminal_rule_override_callbacks;
};

//! Immutable semantic data referenced directly by matchers and parse results.
struct CompiledGrammarRule {
	CompiledGrammarRule(string name_p, grammar_transform_process_function_t transform_process_p,
	                    bool collapsible_p = false, ExpressionDepthKind expression_depth_p = ExpressionDepthKind::NONE,
	                    bool suppress_suggestions_p = false)
	    : name(std::move(name_p)), transform_process(std::move(transform_process_p)), collapsible(collapsible_p),
	      suppress_suggestions(suppress_suggestions_p), expression_depth(expression_depth_p) {
	}

	arena_ptr<TransformProcess> StartTransform(PEGTransformer &transformer, ParseResult &parse_result) const;

	string name;
	//! Built-in rules get their generated constant, rules added at runtime get an ID past BUILTIN_COUNT
	PEGRuleId rule_id = PEGRuleId::INVALID;
	grammar_transform_process_function_t transform_process;
	//! See ParsedGrammar::SetTransformProcess
	bool collapsible;
	//! See ParsedGrammar::AddRule
	bool suppress_suggestions;
	//! See ParsedGrammar::SetExpressionDepth
	ExpressionDepthKind expression_depth;
};

} // namespace duckdb
