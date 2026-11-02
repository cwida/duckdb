//===----------------------------------------------------------------------===//
//                         DuckDB
//
// matcher.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/arena_containers/arena_ptr.hpp"
#include "duckdb/common/arena_containers/arena_vector.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/identifier.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/reference_map.hpp"
#include "duckdb/common/enums/identifier_case_mode.hpp"
#include "duckdb/parser/parser_extension.hpp"
#include "duckdb/parser/peg/keyword_helper.hpp"
#include "duckdb/parser/token_iterator.hpp"
#include "duckdb/parser/peg/parser_packrat.hpp"
#include "duckdb/parser/peg/tokenizer/tokenizer.hpp"
#include "duckdb/parser/peg/parsed_grammar.hpp"
#include "duckdb/parser/peg/transformer/parse_result.hpp"
#include "duckdb/storage/arena_allocator.hpp"

#include <map>

namespace duckdb {
class ClientContext;
class PEGTransformerFactory;
class ParseResultAllocator;
class Matcher;
class MatcherAllocator;
struct ListMatchChain;
class MatchProcess;

enum class SuggestionState : uint8_t {
	SUGGEST_KEYWORD,
	SUGGEST_CATALOG_NAME,
	SUGGEST_SCHEMA_NAME,
	SUGGEST_TABLE_NAME,
	SUGGEST_TYPE_NAME,
	SUGGEST_COLUMN_NAME,
	SUGGEST_FILE_NAME,
	SUGGEST_DIRECTORY,
	SUGGEST_VARIABLE,
	SUGGEST_SCALAR_FUNCTION_NAME,
	SUGGEST_TABLE_FUNCTION_NAME,
	SUGGEST_PRAGMA_NAME,
	SUGGEST_SETTING_NAME,
	SUGGEST_RESERVED_VARIABLE,
	SUGGEST_BARE_LABEL
};

enum class CandidateType { KEYWORD, IDENTIFIER, LITERAL };

struct AutoCompleteCandidate {
	// NOLINTNEXTLINE: allow implicit conversion from string
	AutoCompleteCandidate(string candidate_p, SuggestionState suggestion_type, int32_t score_bonus = 0,
	                      CandidateType candidate_type = CandidateType::IDENTIFIER)
	    : candidate(std::move(candidate_p)), suggestion_type(suggestion_type), score_bonus(score_bonus),
	      candidate_type(candidate_type) {
	}
	// NOLINTNEXTLINE: allow implicit conversion from const char*
	AutoCompleteCandidate(const char *candidate_p, SuggestionState suggestion_type, int32_t score_bonus = 0,
	                      CandidateType candidate_type = CandidateType::IDENTIFIER)
	    : AutoCompleteCandidate(string(candidate_p), suggestion_type, score_bonus, candidate_type) {
	}
	// NOLINTNEXTLINE: allow implicit conversion from Identifier
	AutoCompleteCandidate(const Identifier &candidate_p, SuggestionState suggestion_type, int32_t score_bonus = 0,
	                      CandidateType candidate_type = CandidateType::IDENTIFIER)
	    : AutoCompleteCandidate(candidate_p.GetIdentifierName(), suggestion_type, score_bonus, candidate_type) {
	}

	string candidate;
	//! Type being suggested
	SuggestionState suggestion_type;
	//! The higher the score bonus, the more likely this candidate will be chosen
	int32_t score_bonus;
	//! The type of candidate we are suggesting - this modifies how we handle quoting/case sensitivity
	CandidateType candidate_type;
	//! Extra char to push at the back
	char extra_char = '\0';
	//! Suggestion position
	idx_t suggestion_pos = 0;
	//! The final score
	optional_idx score;
};

struct AutoCompleteSuggestion {
	AutoCompleteSuggestion(string text_p, idx_t pos, string type_p, idx_t score, char extra_char_p)
	    : text(std::move(text_p)), pos(pos), type(std::move(type_p)), score(score), extra_char(extra_char_p) {
	}

	string text;
	idx_t pos;
	string type;
	idx_t score;
	char extra_char;
};

enum class SuggestionType { OPTIONAL, MANDATORY };

enum class MatchMode : uint8_t { BUILD_PARSE_RESULT, RECOGNIZE_ONLY };

class MatcherResult {
public:
	static MatcherResult Success(optional_ptr<ParseResult> parse_result = nullptr) {
		return MatcherResult(true, parse_result);
	}

	static MatcherResult Failure() {
		return MatcherResult(false, nullptr);
	}

	bool IsSuccess() const {
		return success;
	}

	bool HasParseResult() const {
		return parse_result != nullptr;
	}

	optional_ptr<ParseResult> GetParseResult() const {
		return parse_result;
	}

private:
	MatcherResult(bool success_p, optional_ptr<ParseResult> parse_result_p)
	    : success(success_p), parse_result(parse_result_p) {
	}

private:
	bool success;
	optional_ptr<ParseResult> parse_result;
};

struct MatcherSuggestion {
	// NOLINTNEXTLINE: allow implicit conversion from auto-complete candidate
	MatcherSuggestion(AutoCompleteCandidate keyword_p) : keyword(std::move(keyword_p)), type(keyword.suggestion_type) {
	}
	// NOLINTNEXTLINE: allow implicit conversion from suggestion state
	MatcherSuggestion(SuggestionState type, char extra_char = '\0')
	    : keyword("", type), type(type), extra_char(extra_char) {
	}

	//! Literal suggestion
	AutoCompleteCandidate keyword;
	SuggestionState type;
	char extra_char = '\0';
	//! Label of the enclosing labelled rule (e.g. "an expression"), if the suggestion was added inside one
	optional_ptr<const string> label;
};

struct MatchContext {
	MatchContext(vector<MatcherSuggestion> &suggestions_p, ParseResultAllocator &allocator_p,
	             ArenaAllocator &process_allocator_p, idx_t &max_token_index_p,
	             MatchMode mode_p = MatchMode::BUILD_PARSE_RESULT,
	             IdentifierCaseMode identifier_case_mode_p = IdentifierCaseMode::PRESERVE_CASE,
	             ParserPackratCache *packrat_cache_p = nullptr)
	    : suggestions(suggestions_p), allocator(allocator_p), process_allocator(process_allocator_p),
	      max_token_index(max_token_index_p), identifier_case_mode(identifier_case_mode_p),
	      packrat_cache(packrat_cache_p), mode(mode_p) {
	}

	vector<MatcherSuggestion> &suggestions;
	ParseResultAllocator &allocator;
	ArenaAllocator &process_allocator;
	idx_t &max_token_index;
	IdentifierCaseMode identifier_case_mode;
	ParserPackratCache *packrat_cache;
	MatchMode mode;
	//! Label of the labelled rule currently being walked for suggestions, if any
	optional_ptr<const string> active_label;
	//! Ignore rule-level suggestion suppression, so the suggestions a rule adds depend only on its
	//! position and the packrat cache stays valid while collecting them
	bool memoizable_suggestions = false;
	//! With memoizable_suggestions: suggestion walks already done, keyed by matcher and active label
	std::map<std::pair<const Matcher *, const string *>, SuggestionType> walked_suggestions;
};

struct MatchState {
	MatchState(TokenIterator &token_iterator_p, MatchContext &context_p)
	    : token_iterator(token_iterator_p), context(context_p) {
	}
	MatchState(const MatchState &state)
	    : token_iterator(state.token_iterator), context(state.context), rule(state.rule) {
	}

	TokenIterator token_iterator;
	MatchContext &context;
	unique_ptr<reference_set_t<const Matcher>> added_suggestions;
	optional_ptr<const CompiledGrammarRule> rule;

	bool BuildParseResult() const {
		return context.mode == MatchMode::BUILD_PARSE_RESULT;
	}

	template <class RESULT, class... ARGS>
	MatcherResult AllocateParseResult(ARGS &&... args);

	template <class PROCESS, class... ARGS>
	arena_ptr<MatchProcess> Make(ARGS &&... args);

	void UpdateMaxTokenIndex() {
		if (token_iterator.Position() > context.max_token_index) {
			context.max_token_index = token_iterator.Position();
		}
	}

	idx_t GetMaxTokenIndex() const {
		return context.max_token_index;
	}

	//! Fold a non-quoted identifier in-place according to the configured case mode
	void FoldIdentifier(string &text) const {
		switch (context.identifier_case_mode) {
		case IdentifierCaseMode::LOWERCASE:
			text = StringUtil::Lower(text);
			break;
		case IdentifierCaseMode::UPPERCASE:
			text = StringUtil::Upper(text);
			break;
		default:
			break;
		}
	}

	void AddSuggestion(MatcherSuggestion suggestion);
};

//! Input to start a Matcher execution
struct MatchInput {
	const Matcher &matcher;
	MatchState &state;
};

//! Essentially a std::variant<MatchInput, MatcherResult>
//! Produced by a MatchProcess::Resume call, controlling the next step in the execution
class MatchStep {
public:
	static MatchStep Child(MatchInput input);
	static MatchStep Complete(MatcherResult result);

	optional<MatchInput> GetChild();
	MatcherResult GetResult() const;

private:
	MatchStep(optional<MatchInput> child_p, optional<MatcherResult> result_p)
	    : child(std::move(child_p)), result(result_p) {
	}

private:
	optional<MatchInput> child;
	optional<MatcherResult> result;
};

class MatchProcess {
public:
	virtual ~MatchProcess() = default;

	//! Resume matching, optionally with the result of the previously requested child.
	virtual MatchStep Resume(optional<MatcherResult> child_result) = 0;
};

enum class MatcherType {
	KEYWORD,
	LIST,
	OPTIONAL,
	CHOICE,
	REPEAT,
	VARIABLE,
	STRING_LITERAL,
	NUMBER_LITERAL,
	OPERATOR,
	END_OF_INPUT,
	CUSTOM
};

//! Over-approximation of the tokens a matcher can start with (its FIRST set), computed once per grammar. Lets the
//! matcher skip sub-matchers that certainly cannot match at the current token.
struct MatcherFirstSet {
	bool computed = false;
	//! the matcher can succeed without consuming a token
	bool nullable = false;
	//! no information: never skip
	bool any = false;
	//! MatcherTokenClass bits
	uint8_t class_mask = 0;
	//! bitset over grammar literal ids (keywords and symbols)
	vector<uint64_t> literals;
	optional_ptr<const GrammarLiteralTable> table;

	bool HasLiteral(idx_t literal_id) const {
		auto word = literal_id / 64;
		return word < literals.size() && (literals[word] >> (literal_id % 64)) & 1;
	}
	DUCKDB_API void AddLiteral(idx_t literal_id);
	//! Merge other into this set, returns whether anything changed
	DUCKDB_API bool Merge(const MatcherFirstSet &other);
	//! False only if the matcher certainly cannot match at the current token
	DUCKDB_API bool MightMatch(MatchState &state) const;
};

class Matcher {
public:
	explicit Matcher(MatcherType type = MatcherType::CUSTOM) : type(type) {
	}
	virtual ~Matcher() = default;

	//! Match and construct the parse result
	MatcherResult MatchParseResult(MatchState &state) const;
	//! Create matcher-local state with state.Make<PROCESS>() for incremental execution.
	virtual arena_ptr<MatchProcess> StartMatch(MatchState &state) const = 0;
	virtual bool IsAtomic() const {
		return false;
	}
	virtual SuggestionType AddSuggestion(MatchState &state) const;
	virtual SuggestionType AddSuggestionInternal(MatchState &state) const = 0;
	//! AddSuggestionInternal, with the matcher's label (if any) applied to the suggestions it adds
	SuggestionType AddLabelledSuggestion(MatchState &state) const;
	virtual string ToString() const = 0;
	void Print() const;

	MatcherType Type() const {
		return type;
	}
	void SetName(string name_p) {
		name = std::move(name_p);
	}
	void SetRule(const CompiledGrammarRule &rule_p) {
		rule = rule_p;
		name = rule_p.name;
	}
	//! A short description of what the matcher accepts, used to summarise it in syntax errors
	void SetLabel(string label_p) {
		label = std::move(label_p);
	}
	const string &GetLabel() const {
		return label;
	}
	optional_ptr<const CompiledGrammarRule> GetRule() const {
		return rule;
	}
	bool HasName() const {
		return !name.empty();
	}
	//! The stored name, which lives as long as the grammar and can therefore be referenced by a parse result
	const string &GetName() const {
		return name;
	}
	//! The name to print for this matcher, which for an unnamed one renders the grammar it matches
	string GetPrintName() const;
	optional_idx GetPackratId() const {
		return packrat_id;
	}
	void SetPackratMemoized() {
		packrat_memoized = true;
	}
	bool IsPackratMemoized() const {
		return packrat_memoized;
	}
	//! See ParsedGrammar::SetTransformProcess
	void SetCollapsible() {
		collapsible = true;
	}
	bool IsCollapsible() const {
		return collapsible;
	}
	//! The matcher runs the built-in implementation of its type (set by MatcherFactory, never for derived matchers)
	void SetStructural() {
		structural = true;
	}
	bool IsStructural() const {
		return structural;
	}

public:
	template <class TARGET>
	TARGET &Cast() {
		if (type != TARGET::TYPE) {
			InternalException::Throw("Failed to cast matcher to type - matcher type mismatch");
		}
		return reinterpret_cast<TARGET &>(*this);
	}

	template <class TARGET>
	const TARGET &Cast() const {
		if (type != TARGET::TYPE) {
			InternalException::Throw("Failed to cast matcher to type - matcher type mismatch");
		}
		return reinterpret_cast<const TARGET &>(*this);
	}

public:
	MatcherFirstSet first_set;

protected:
	friend class MatcherAllocator;
	MatcherType type;
	string name;
	string label;
	optional_idx packrat_id;
	bool packrat_memoized = false;
	bool collapsible = false;
	bool structural = false;
	optional_ptr<const CompiledGrammarRule> rule;
};

//! Compute the FIRST sets of all matchers reachable from root
DUCKDB_API void ComputeFirstSets(Matcher &root, const GrammarLiteralTable &table);

class AtomicMatcher : public Matcher {
public:
	explicit AtomicMatcher(MatcherType type) : Matcher(type) {
	}

	bool IsAtomic() const final {
		return true;
	}
	DUCKDB_API arena_ptr<MatchProcess> StartMatch(MatchState &state) const final;
	virtual MatcherResult MatchAtomic(MatchState &state) const = 0;
	//! Describes the tokens this matcher can start with; the default (no information) never skips the matcher
	virtual void InitializeFirstSet(MatcherFirstSet &first_set, const GrammarLiteralTable &table) const {
		first_set.any = true;
	}
};

class KeywordInfo {
public:
	KeywordInfo() {
	}
	explicit KeywordInfo(int32_t score_bonus, char extra_char = ' ')
	    : score_bonus(score_bonus), extra_char(extra_char) {
	}

public:
	int32_t score_bonus = 0;
	char extra_char = '\0';
};

class MatcherAllocator {
public:
	DUCKDB_API MatcherAllocator();
	DUCKDB_API MatcherAllocator(MatcherAllocator &&) noexcept;
	DUCKDB_API MatcherAllocator &operator=(MatcherAllocator &&) noexcept;
	DUCKDB_API ~MatcherAllocator();

	Matcher &Allocate(unique_ptr<Matcher> matcher);
	void InitializeListChains();

private:
	vector<unique_ptr<Matcher>> matchers;
	vector<unique_ptr<ListMatchChain>> list_chains;
};

class ParseResultAllocator {
public:
	ParseResultAllocator();
	~ParseResultAllocator();

	template <class RESULT, class... ARGS>
	optional_ptr<ParseResult> Make(ARGS &&... args) {
		static_assert(std::is_base_of<ParseResult, RESULT>::value, "Expected a parse result");
		auto result = arena.Make<RESULT>(std::forward<ARGS>(args)...);
		if (ParseResultNeedsDestructor<RESULT>::value) {
			// held by an owner before growing the list, so the result is still destroyed if the growth throws
			arena_ptr<ParseResult> owned(result);
			pending_destructors.push_back(std::move(owned));
		}
		return optional_ptr<ParseResult>(result);
	}

	//! Uninitialized child storage owned by the result arena.
	unsafe_array_ptr<reference<ParseResult>> AllocateChildren(idx_t capacity) {
		arena.AlignNext();
		return unsafe_array_ptr<reference<ParseResult>>(
		    reinterpret_cast<reference<ParseResult> *>(arena.Allocate(capacity * sizeof(reference<ParseResult>))),
		    capacity);
	}

	//! Copy a collected set of children into the arena, where it lives as long as the results it belongs to
	unsafe_array_ptr<reference<ParseResult>> MakeChildren(const arena_vector<reference<ParseResult>> &children) {
		auto count = children.size();
		arena.AlignNext();
		// an empty set still takes an address from the arena, which costs nothing and keeps the span non-null
		auto target =
		    reinterpret_cast<reference<ParseResult> *>(arena.Allocate(count * sizeof(reference<ParseResult>)));
		if (count > 0) {
			memcpy(static_cast<void *>(target), static_cast<const void *>(children.data()),
			       count * sizeof(reference<ParseResult>));
		}
		return unsafe_array_ptr<reference<ParseResult>>(target, count);
	}

private:
	ArenaAllocator arena;
	//! Dropping the arena reclaims the memory of every result at once but calls no destructors, so a result that
	//! owns something has to be destroyed before that happens. An `arena_ptr` destroys what it points at without
	//! freeing it, which is all these are here for. Only the node types that `ParseResultNeedsDestructor` selects end
	//! up in the list, and nothing ever reads it.
	arena_vector<arena_ptr<ParseResult>> pending_destructors;
};

template <class PROCESS, class... ARGS>
arena_ptr<MatchProcess> MatchState::Make(ARGS &&... args) {
	static_assert(std::is_base_of<MatchProcess, PROCESS>::value, "Expected a matcher process");
	return arena_ptr<MatchProcess>(context.process_allocator.Make<PROCESS>(std::forward<ARGS>(args)...));
}

void ParseResult::SetNameFrom(const CompiledGrammarRule &rule_p) {
	name = &rule_p.name;
}

void ParseResult::SetNameFrom(const Matcher &matcher_p) {
	name = &matcher_p.GetName();
}

template <class RESULT, class... ARGS>
MatcherResult MatchState::AllocateParseResult(ARGS &&... args) {
	if (!BuildParseResult()) {
		return MatcherResult::Success();
	}
	auto result = context.allocator.Make<RESULT>(std::forward<ARGS>(args)...);
	if (rule) {
		result->SetRule(*rule);
		result->SetNameFrom(*rule);
	}
	return MatcherResult::Success(result);
}

} // namespace duckdb
