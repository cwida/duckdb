#include "catch.hpp"
#include <algorithm>
#include <atomic>
#include <thread>
#include <type_traits>
#include "test_helpers.hpp"

#include "duckdb/common/limits.hpp"
#include "duckdb/main/parse_iterator.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/grammar_extension.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/peg/compiled_grammar.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/peg/keyword_helper/default_keyword_maps.hpp"
#include "duckdb/parser/peg/keyword_helper/parsed_grammar_keyword_helper.hpp"
#include "duckdb/parser/peg/matcher/identifier_matcher.hpp"
#include "duckdb/parser/peg/matcher/choice_matcher.hpp"
#include "duckdb/parser/peg/matcher/keyword_matcher.hpp"
#include "duckdb/parser/peg/matcher/list.hpp"
#include "duckdb/parser/peg/matcher/list_matcher.hpp"
#include "duckdb/parser/peg/matcher_stack.hpp"
#include "duckdb/parser/peg/matcher_factory.hpp"
#include "duckdb/parser/peg/parsed_grammar.hpp"
#include "duckdb/parser/peg/tokenizer/parser_tokenizer.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/emptytableref.hpp"

using namespace duckdb;

struct LiteralChoiceTestResult {
	bool success;
	idx_t position;
	idx_t max_position;
	string tree;
};

static compiled_rules_map_t CompileTestProgramRule(const ParsedGrammar &grammar) {
	compiled_rules_map_t rules;
	auto rule = grammar.GetRule("Program");
	if (!rule) {
		throw InternalException("Test grammar is missing the Program rule");
	}
	rules.emplace(rule->name, make_uniq<CompiledGrammarRule>(rule->name, rule->transform_process, rule->collapsible));
	return rules;
}

static vector<MatcherSuggestion> GetLiteralChoiceSuggestions(const Matcher &matcher) {
	vector<MatcherToken> tokens;
	TokenIterator iterator(tokens);
	vector<MatcherSuggestion> suggestions;
	ParseResultAllocator allocator;
	idx_t max_position = 0;
	ArenaAllocator process_allocator(Allocator::DefaultAllocator());
	MatchContext context(suggestions, allocator, process_allocator, max_position);
	MatchState state(iterator, context);
	matcher.AddSuggestion(state);
	return suggestions;
}

TEST_CASE("Literal choice dispatch retains autocomplete metadata", "[api][grammar_extension]") {
	auto compiled = CompiledGrammar::Create();
	auto grammar = ParsedGrammar::Parse("Program <- 'TABLE' / '(' / '.' / 'table'");
	auto rules = CompileTestProgramRule(grammar);
	MatcherAllocator allocator;
	MatcherFactory factory(allocator, grammar, rules, compiled->GetKeywordHelper(), {});
	auto &root = factory.CreateRootMatcher("Program").Cast<ListMatcher>();
	auto &choice = root.matchers[0].get().Cast<ChoiceMatcher>();
	vector<reference<Matcher>> children = choice.matchers;
	ChoiceMatcher sequential(std::move(children));
	auto actual = GetLiteralChoiceSuggestions(choice);
	auto expected = GetLiteralChoiceSuggestions(sequential);
	REQUIRE(actual.size() == 3);
	REQUIRE(actual.size() == expected.size());
	for (idx_t i = 0; i < actual.size(); i++) {
		REQUIRE(actual[i].type == expected[i].type);
		REQUIRE(actual[i].keyword.candidate == expected[i].keyword.candidate);
		REQUIRE(actual[i].keyword.score_bonus == expected[i].keyword.score_bonus);
		REQUIRE(actual[i].keyword.extra_char == expected[i].keyword.extra_char);
		REQUIRE(actual[i].keyword.candidate_type == expected[i].keyword.candidate_type);
	}
}

static LiteralChoiceTestResult MatchLiteralChoiceTest(const Matcher &matcher, const string &text, MatchMode mode) {
	vector<MatcherToken> tokens;
	if (!text.empty()) {
		tokens.emplace_back(text, 0, TokenType::KEYWORD);
	}
	TokenIterator iterator(tokens);
	vector<MatcherSuggestion> suggestions;
	ParseResultAllocator allocator;
	ArenaAllocator packrat_allocator(Allocator::DefaultAllocator());
	ParserPackratCache packrat(packrat_allocator);
	idx_t max_position = 0;
	ArenaAllocator process_allocator(Allocator::DefaultAllocator());
	MatchContext context(suggestions, allocator, process_allocator, max_position, mode,
	                     IdentifierCaseMode::PRESERVE_CASE, &packrat);
	MatchState state(iterator, context);
	auto result = matcher.MatchParseResult(state);
	return {result.IsSuccess(), state.token_iterator.Position(), max_position,
	        result.HasParseResult() ? result.GetParseResult()->ToString() : string()};
}

TEST_CASE("Literal choice dispatch preserves ordered choice results", "[api][grammar_extension]") {
	auto compiled = CompiledGrammar::Create();
	auto grammar = ParsedGrammar::Parse("Program <- 'SELECT' / 'FROM' / 'select' / 'WHERE' / '('");
	auto rules = CompileTestProgramRule(grammar);
	MatcherAllocator allocator;
	MatcherFactory factory(allocator, grammar, rules, compiled->GetKeywordHelper(), {});
	auto &root = factory.CreateRootMatcher("Program").Cast<ListMatcher>();
	auto &choice = root.matchers[0].get().Cast<ChoiceMatcher>();
	vector<reference<Matcher>> children = choice.matchers;
	ChoiceMatcher sequential(std::move(children));
	auto &table = compiled->GetKeywordHelper().GetLiteralTable();
	REQUIRE(choice.matchers[0].get().Cast<KeywordMatcher>().GetDispatchLiteral(table).IsValid());
	for (auto &text : vector<string> {"WHERE", "unknown_literal"}) {
		vector<MatcherToken> tokens {MatcherToken(text, 0, TokenType::KEYWORD)};
		TokenIterator iterator(tokens);
		vector<MatcherSuggestion> suggestions;
		ParseResultAllocator parse_results;
		idx_t max_position = 0;
		ArenaAllocator process_allocator(Allocator::DefaultAllocator());
		MatchContext context(suggestions, parse_results, process_allocator, max_position);
		MatchState state(iterator, context);
		auto process = choice.StartMatch(state);
		auto step = process->Resume(nullopt);
		if (text == "WHERE") {
			REQUIRE(step.GetChild());
			REQUIRE(&step.GetChild()->matcher == &choice.matchers[3].get());
		} else {
			REQUIRE_FALSE(step.GetChild());
			REQUIRE_FALSE(step.GetResult().IsSuccess());
		}
	}
	for (auto mode : {MatchMode::BUILD_PARSE_RESULT, MatchMode::RECOGNIZE_ONLY}) {
		for (auto &text : vector<string> {"select", "FROM", "where", "(", "unknown_literal", ""}) {
			auto actual = MatchLiteralChoiceTest(choice, text, mode);
			auto expected = MatchLiteralChoiceTest(sequential, text, mode);
			REQUIRE(actual.success == expected.success);
			REQUIRE(actual.position == expected.position);
			REQUIRE(actual.max_position == expected.max_position);
			REQUIRE(actual.tree == expected.tree);
		}
	}
}

class DispatchOverrideKeywordMatcher final : public KeywordMatcher {
public:
	DispatchOverrideKeywordMatcher(const string &keyword, const KeywordInfo &info, const PEGKeywordHelper &helper,
	                               idx_t &calls_p)
	    : KeywordMatcher(keyword, info, helper), calls(calls_p), accepts_from(keyword == "SELECT") {
	}

	MatcherResult MatchAtomic(MatchState &state) const override {
		calls++;
		auto token = state.token_iterator.Current();
		if (accepts_from && token && StringUtil::CIEquals(token->text, "FROM")) {
			state.token_iterator.Advance();
			return MatcherResult::Success();
		}
		return KeywordMatcher::MatchAtomic(state);
	}

private:
	idx_t &calls;
	bool accepts_from;
};

class DispatchOverrideMatcherFactory final : public MatcherFactory {
public:
	DispatchOverrideMatcherFactory(MatcherAllocator &allocator, const ParsedGrammar &grammar,
	                               const compiled_rules_map_t &rules, const PEGKeywordHelper &helper_p, idx_t &calls_p)
	    : MatcherFactory(allocator, grammar, rules, helper_p, {}), helper(helper_p), calls(calls_p) {
	}

private:
	unique_ptr<KeywordMatcher> CreateKeyword(const string &keyword, const KeywordInfo &info) const override {
		return make_uniq<DispatchOverrideKeywordMatcher>(keyword, info, helper, calls);
	}

	const PEGKeywordHelper &helper;
	idx_t &calls;
};

TEST_CASE("Literal dispatch does not assume custom keyword matcher semantics", "[api][grammar_extension]") {
	auto compiled = CompiledGrammar::Create();
	auto grammar = ParsedGrammar::Parse("Program <- 'SELECT' / 'FROM'");
	auto rules = CompileTestProgramRule(grammar);
	MatcherAllocator allocator;
	idx_t calls = 0;
	DispatchOverrideMatcherFactory factory(allocator, grammar, rules, compiled->GetKeywordHelper(), calls);
	auto &root = factory.CreateRootMatcher("Program");
	auto result = MatchLiteralChoiceTest(root, "FROM", MatchMode::RECOGNIZE_ONLY);
	REQUIRE(result.success);
	REQUIRE(result.position == 1);
	REQUIRE(calls == 1);
}

TEST_CASE("Literal dispatch leaves mixed and unregistered alternatives unchanged", "[api][grammar_extension]") {
	auto compiled = CompiledGrammar::Create();
	for (auto &definition : vector<string> {"Program <- 'SELECT' / ('FROM' 'WHERE')",
	                                        "Program <- 'SELECT' / 'unregistered_dispatch_word'"}) {
		auto grammar = ParsedGrammar::Parse(definition);
		auto rules = CompileTestProgramRule(grammar);
		MatcherAllocator allocator;
		MatcherFactory factory(allocator, grammar, rules, compiled->GetKeywordHelper(), {});
		auto &root = factory.CreateRootMatcher("Program").Cast<ListMatcher>();
		auto &choice = root.matchers[0].get().Cast<ChoiceMatcher>();
		vector<reference<Matcher>> children = choice.matchers;
		ChoiceMatcher sequential(std::move(children));
		for (auto &text : vector<string> {"SELECT", "FROM", "unregistered_dispatch_word", "missing"}) {
			auto actual = MatchLiteralChoiceTest(choice, text, MatchMode::BUILD_PARSE_RESULT);
			auto expected = MatchLiteralChoiceTest(sequential, text, MatchMode::BUILD_PARSE_RESULT);
			REQUIRE(actual.success == expected.success);
			REQUIRE(actual.position == expected.position);
			REQUIRE(actual.max_position == expected.max_position);
			REQUIRE(actual.tree == expected.tree);
		}
	}
}

TEST_CASE("Keyword category masks require explicit conversions and valid IDs", "[api][grammar_extension]") {
	static_assert(!std::is_convertible<uint8_t, keyword_categories_t>::value, "Category masks must be explicit");
	static_assert(!std::is_convertible<keyword_categories_t, uint8_t>::value, "Category masks must stay opaque");
	static_assert(!std::is_assignable<keyword_categories_t &, uint8_t>::value, "Raw masks must be explicit");
	constexpr auto first = keyword_categories_t::CreateCategory(0);
	constexpr auto last = keyword_categories_t::CreateCategory(keyword_categories_t::MAX_CATEGORY_ID - 1);
	static_assert(sizeof(keyword_categories_t) == sizeof(uint8_t), "Category masks must remain compact");
	REQUIRE(static_cast<uint8_t>(first) == 1);
	REQUIRE(static_cast<uint8_t>(last) == 128);
	REQUIRE(keyword_categories_t(static_cast<uint8_t>(first)) == first);
	REQUIRE((first & last) == keyword_categories_t());
	REQUIRE(((first | last) & first) == first);
	REQUIRE(first != keyword_categories_t());
	REQUIRE_THROWS_AS(keyword_categories_t::CreateCategory(keyword_categories_t::MAX_CATEGORY_ID),
	                  InvalidInputException);
	REQUIRE_THROWS_AS(keyword_categories_t::CreateCategory(256), InvalidInputException);
}

TEST_CASE("Literal IDs and opaque flags are independent", "[api][grammar_extension]") {
	REQUIRE(sizeof(LiteralInfo) == sizeof(uint32_t));
	REQUIRE(sizeof(LiteralInfo().LiteralId()) == sizeof(uint16_t));
	LiteralInfo missing;
	REQUIRE(missing.LiteralId() == 0);
	REQUIRE_FALSE(missing.IsKeyword());
	REQUIRE_FALSE(missing.HasAnyFlags(~keyword_categories_t()));
	LiteralInfo original(LiteralInfo::MAX_LITERAL_ID);
	REQUIRE(original.LiteralId() == 65535);
	REQUIRE_FALSE(original.IsKeyword());
	LiteralInfo accumulated(LiteralInfo::MAX_LITERAL_ID);
	keyword_categories_t expected_flags;
	for (idx_t id = 0; id < keyword_categories_t::MAX_CATEGORY_ID; id++) {
		auto flag = keyword_categories_t::CreateCategory(id);
		LiteralInfo literal(LiteralInfo::MAX_LITERAL_ID, flag);
		REQUIRE(literal.HasAnyFlags(flag));
		REQUIRE_FALSE(literal.HasAnyFlags(~flag));
		REQUIRE_FALSE(literal.HasAnyFlags(keyword_categories_t()));
		REQUIRE(literal.IsKeyword());
		REQUIRE(literal.LiteralId() == original.LiteralId());
		REQUIRE_FALSE(literal == original);
		LiteralInfo copy(literal);
		REQUIRE(copy == literal);
		LiteralInfo unassigned;
		unassigned.AddCategories(flag);
		REQUIRE(unassigned.LiteralId() == 0);
		REQUIRE(unassigned.IsKeyword());
		REQUIRE(unassigned.CategoryFlags() == flag);
		accumulated.AddCategories(flag);
		expected_flags |= flag;
		REQUIRE(accumulated.LiteralId() == LiteralInfo::MAX_LITERAL_ID);
		REQUIRE(accumulated.CategoryFlags() == expected_flags);
		LiteralInfo before(accumulated);
		accumulated.AddCategories(flag);
		accumulated.AddCategories(keyword_categories_t());
		REQUIRE(accumulated == before);
	}
}

TEST_CASE("Default keyword categories decode independently of literal IDs", "[api][grammar_extension]") {
	DefaultKeywordMaps maps;
	vector<KeywordCategory> expected;
	REQUIRE(DefaultKeywordMaps::GetKeywordCategories(LiteralInfo()).empty());
	REQUIRE(DefaultKeywordMaps::GetKeywordCategories(LiteralInfo(LiteralInfo::MAX_LITERAL_ID)).empty());
	vector<pair<KeywordCategory, reference<case_insensitive_set_t>>> categories {
	    {KeywordCategory::KEYWORD_RESERVED, maps.reserved_keyword_map},
	    {KeywordCategory::KEYWORD_UNRESERVED, maps.unreserved_keyword_map},
	    {KeywordCategory::KEYWORD_TYPE_FUNC, maps.typefunc_keyword_map},
	    {KeywordCategory::KEYWORD_COL_NAME, maps.colname_keyword_map},
	    {KeywordCategory::KEYWORD_TYPE_NAME, maps.typename_keyword_map}};
	for (auto &category : categories) {
		category.second.get().insert("overlapping");
		expected.push_back(category.first);
		auto single_word = "category_" + to_string(expected.size());
		category.second.get().insert(single_word);
		REQUIRE(DefaultKeywordMaps::GetKeywordCategories(maps.LookupKeyword(single_word)) ==
		        vector<KeywordCategory> {category.first});
		for (auto id : {uint16_t(0), uint16_t(1), LiteralInfo::MAX_LITERAL_ID}) {
			auto info = maps.LookupKeyword("OVERLAPPING", id);
			REQUIRE(info.LiteralId() == id);
			REQUIRE(DefaultKeywordMaps::GetKeywordCategories(info) == expected);
		}
	}
}

TEST_CASE("Grammar literal IDs reject overflow", "[api][grammar_extension]") {
	auto grammar = ParsedGrammar::Parse("LiteralTest <- '('");
	case_insensitive_map_t<LiteralInfo> keywords;
	const auto flags = keyword_categories_t::CreateCategory(6) | keyword_categories_t::CreateCategory(7);
	for (idx_t i = 1; i < LiteralInfo::MAX_LITERAL_ID; i++) {
		keywords.emplace("literal_limit_" + to_string(i), LiteralInfo(0, flags));
	}
	GrammarLiteralTable table(grammar, keywords);
	idx_t max_id = table.Lookup("(").LiteralId();
	for (auto &entry : keywords) {
		auto info = table.Lookup(entry.first);
		REQUIRE(info.LiteralId() != 0);
		REQUIRE(info.HasAnyFlags(keyword_categories_t::CreateCategory(6)));
		REQUIRE(info.HasAnyFlags(keyword_categories_t::CreateCategory(7)));
		max_id = MaxValue<idx_t>(max_id, info.LiteralId());
	}
	REQUIRE(max_id == LiteralInfo::MAX_LITERAL_ID);
	keywords.emplace("one_literal_too_many", LiteralInfo());
	REQUIRE_THROWS_WITH(GrammarLiteralTable(grammar, keywords),
	                    Catch::Matchers::Contains("Grammar has too many distinct literals"));
}

TEST_CASE("Grammar literal IDs include category-only words and overlapping categories", "[api][grammar_extension]") {
	auto grammar = ParsedGrammar::Parse("LiteralTest <- 'SELECT' / 'select' / '('");
	DefaultKeywordMaps categories;
	categories.reserved_keyword_map.insert("SELECT");
	categories.typefunc_keyword_map.insert("category_only");
	categories.typename_keyword_map.insert("CATEGORY_ONLY");
	GrammarLiteralTable table(grammar, categories.ToLiteralMap());
	REQUIRE(sizeof(LiteralInfo) == sizeof(uint32_t));
	REQUIRE(table.Lookup("select") == table.Lookup("SELECT"));
	REQUIRE(table.Lookup("SELECT").LiteralId() != 0);
	REQUIRE(table.Lookup("SELECT").IsKeyword());
	REQUIRE(table.Lookup("(").LiteralId() != 0);
	REQUIRE_FALSE(table.Lookup("(").IsKeyword());
	REQUIRE(table.Lookup("category_only").LiteralId() != 0);
	vector<KeywordCategory> expected {KeywordCategory::KEYWORD_TYPE_FUNC, KeywordCategory::KEYWORD_TYPE_NAME};
	REQUIRE(DefaultKeywordMaps::GetKeywordCategories(table.Lookup("category_only")) == expected);
	REQUIRE(DefaultKeywordMaps::GetKeywordCategories(table.Lookup("SELECT")) ==
	        vector<KeywordCategory> {KeywordCategory::KEYWORD_RESERVED});
	REQUIRE(DefaultKeywordMaps::GetKeywordCategories(table.Lookup("(")).empty());
	REQUIRE(DefaultKeywordMaps::GetKeywordCategories(table.Lookup("missing")).empty());
	REQUIRE(table.Lookup("missing").LiteralId() == 0);
	REQUIRE_FALSE(table.Lookup("missing").IsKeyword());
}

TEST_CASE("Literal lookup preserves long grammar literals and category-only words", "[api][grammar_extension]") {
	auto literal = string(256, 'a');
	auto keyword = string(512, 'b');
	auto grammar = ParsedGrammar::Parse("LiteralTest <- '" + literal + "'");
	const auto flag = keyword_categories_t::CreateCategory(6);
	case_insensitive_map_t<LiteralInfo> keywords;
	keywords.emplace(keyword, LiteralInfo(0, flag));
	GrammarLiteralTable table(grammar, keywords);
	REQUIRE(table.Lookup(literal).LiteralId() != 0);
	REQUIRE(table.Lookup(StringUtil::Upper(literal)) == table.Lookup(literal));
	REQUIRE(table.Lookup(keyword).HasAnyFlags(flag));
	REQUIRE(table.Lookup(StringUtil::Upper(keyword)) == table.Lookup(keyword));
	REQUIRE(table.Lookup(keyword + "b") == LiteralInfo());
	REQUIRE(table.Lookup(string(512, 'c')) == LiteralInfo());
	REQUIRE(table.Lookup(string(65536, 'a')) == LiteralInfo());
}

TEST_CASE("Grammar literal tables preserve dialect-defined flags", "[api][grammar_extension]") {
	auto grammar = ParsedGrammar::Parse("LiteralTest <- 'SHARED' / 'shared' / 'plain'");
	const keyword_categories_t first_flag = keyword_categories_t::CreateCategory(6);
	const keyword_categories_t second_flag = keyword_categories_t::CreateCategory(7);
	case_insensitive_map_t<LiteralInfo> keywords;
	keywords.emplace("shared", LiteralInfo(0, first_flag | second_flag));
	keywords.emplace("category_only", LiteralInfo(0, second_flag));
	GrammarLiteralTable table(grammar, keywords);

	auto shared = table.Lookup("SHARED");
	auto category_only = table.Lookup("CATEGORY_ONLY");
	auto plain = table.Lookup("plain");
	REQUIRE(shared == table.Lookup("shared"));
	REQUIRE(shared.LiteralId() != 0);
	REQUIRE(category_only.LiteralId() != 0);
	REQUIRE(plain.LiteralId() != 0);
	REQUIRE(shared.LiteralId() != category_only.LiteralId());
	REQUIRE(shared.LiteralId() != plain.LiteralId());
	REQUIRE(category_only.LiteralId() != plain.LiteralId());
	REQUIRE(shared.HasAnyFlags(first_flag));
	REQUIRE(shared.HasAnyFlags(second_flag));
	REQUIRE(category_only.HasAnyFlags(second_flag));
	REQUIRE_FALSE(category_only.HasAnyFlags(first_flag));
	REQUIRE_FALSE(plain.IsKeyword());
	REQUIRE_FALSE(plain.HasAnyFlags(first_flag | second_flag));
	REQUIRE(table.Lookup("missing") == LiteralInfo());
}

TEST_CASE("Token literal caches follow grammar identity and token edits", "[api][grammar_extension]") {
	auto grammar = ParsedGrammar::Parse("LiteralTest <- 'SELECT'");
	DefaultKeywordMaps first_categories;
	first_categories.reserved_keyword_map.insert("SELECT");
	DefaultKeywordMaps second_categories;
	second_categories.unreserved_keyword_map.insert("SELECT");
	second_categories.typename_keyword_map.insert("extension_word");
	GrammarLiteralTable first(grammar, first_categories.ToLiteralMap());
	optional<GrammarLiteralTable> second;
	second.emplace(grammar, second_categories.ToLiteralMap());
	vector<MatcherToken> tokens {MatcherToken("select", 0, TokenType::KEYWORD),
	                             MatcherToken("extension_word", 7, TokenType::IDENTIFIER)};
	TokenIterator iterator(tokens);
	REQUIRE(iterator.CurrentLiteralInfo(first) == first.Lookup("SELECT"));
	TokenIterator branch(iterator);
	branch.Advance();
	branch.SetPreviousTokenType(TokenType::COLUMN_NAME);
	REQUIRE(iterator.CurrentLiteralInfo(first) == first.Lookup("SELECT"));
	REQUIRE(iterator.CurrentLiteralInfo(*second) == second->Lookup("SELECT"));
	REQUIRE_FALSE(iterator.CurrentLiteralInfo(*second) == first.Lookup("SELECT"));
	REQUIRE(branch.CurrentLiteralInfo(first).LiteralId() == 0);
	REQUIRE(branch.CurrentLiteralInfo(*second) == second->Lookup("extension_word"));
	REQUIRE(branch.CurrentLiteralInfo(first).LiteralId() == 0);
	iterator.CurrentLiteralInfo(*second);
	auto old_cache_id = second->CacheId();
	second.reset();
	second.emplace(grammar, first_categories.ToLiteralMap());
	REQUIRE(second->CacheId() != old_cache_id);
	REQUIRE(iterator.CurrentLiteralInfo(*second) == first.Lookup("SELECT"));
	tokens[0] = MatcherToken("extension_word", 0, TokenType::IDENTIFIER);
	TokenIterator edited(tokens);
	REQUIRE(edited.CurrentLiteralInfo(*second).LiteralId() == 0);
	branch.Advance();
	REQUIRE(branch.CurrentLiteralInfo(first).LiteralId() == 0);
}

class LiteralTestKeywordHelper final : public PEGKeywordHelper {
public:
	explicit LiteralTestKeywordHelper(bool allow_identifier = false)
	    : literal_table(ParsedGrammar::Parse("LiteralTest <- 'custom_word'"), BuildKeywords(allow_identifier)) {
	}
	const GrammarLiteralTable &GetLiteralTable() const override {
		return literal_table;
	}
	keyword_categories_t GetIdentifierMask(SuggestionState type) const override {
		return DefaultKeywordMaps::GetIdentifierMask(type);
	}
	vector<ParserKeyword> KeywordList() const override {
		return {};
	}

private:
	static case_insensitive_map_t<LiteralInfo> BuildKeywords(bool allow_identifier) {
		DefaultKeywordMaps maps;
		if (allow_identifier) {
			maps.unreserved_keyword_map.insert("custom_word");
		} else {
			maps.reserved_keyword_map.insert("custom_word");
		}
		return maps.ToLiteralMap();
	}

	GrammarLiteralTable literal_table;
};

static bool MatchLiteralTestToken(const Matcher &matcher, const string &text) {
	vector<MatcherToken> tokens {MatcherToken(text, 0, TokenType::IDENTIFIER)};
	TokenIterator iterator(tokens);
	vector<MatcherSuggestion> suggestions;
	ParseResultAllocator allocator;
	idx_t max_token_index = 0;
	ArenaAllocator process_allocator(Allocator::DefaultAllocator());
	MatchContext context(suggestions, allocator, process_allocator, max_token_index);
	MatchState state(iterator, context);
	return matcher.MatchParseResult(state).IsSuccess();
}

TEST_CASE("Custom keyword helpers and standalone literal matchers keep their semantics", "[api][grammar_extension]") {
	LiteralTestKeywordHelper helper;
	REQUIRE(helper.GetLiteralTable().Lookup("custom_word").LiteralId() != 0);
	IdentifierMatcher identifier(SuggestionState::SUGGEST_COLUMN_NAME, helper);
	REQUIRE_FALSE(MatchLiteralTestToken(identifier, "CUSTOM_WORD"));
	LiteralTestKeywordHelper permissive_helper(true);
	IdentifierMatcher permissive_identifier(SuggestionState::SUGGEST_COLUMN_NAME, permissive_helper);
	REQUIRE(MatchLiteralTestToken(permissive_identifier, "CUSTOM_WORD"));
	KeywordMatcher standalone("custom_word", KeywordInfo());
	KeywordMatcher custom_helper("custom_word", KeywordInfo(), helper);
	REQUIRE(MatchLiteralTestToken(standalone, "CUSTOM_WORD"));
	REQUIRE(MatchLiteralTestToken(custom_helper, "CUSTOM_WORD"));
	REQUIRE_FALSE(MatchLiteralTestToken(custom_helper, "another_word"));
	DuckDB db(nullptr);
	Connection con(db);
	auto grammar = CompiledGrammar::Get(*con.context);
	KeywordMatcher unregistered("unregistered_literal_test_word", KeywordInfo(), grammar->GetKeywordHelper());
	REQUIRE(MatchLiteralTestToken(unregistered, "UNREGISTERED_LITERAL_TEST_WORD"));
	REQUIRE_FALSE(MatchLiteralTestToken(unregistered, "another_word"));
}

struct RegisteredTransformResult {
	idx_t value;
};

namespace duckdb {
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.test.RegisteredTransformResult", ::RegisteredTransformResult);
} // namespace duckdb

TEST_CASE("Transform result types use stable registered names", "[api][grammar_extension]") {
	TypedTransformResult<RegisteredTransformResult> result({42});
	auto copied_type_name = string(TransformResultTypeName<RegisteredTransformResult>());

	// a copied name (different address, as across an extension boundary) still matches
	REQUIRE(result.GetValuePointer(copied_type_name.c_str()) == &result.value);
	REQUIRE(result.type_hash == TransformResultTypeNameHash(copied_type_name.c_str()));
	REQUIRE(result.type_hash != TransformResultTypeHash<bool>());
	REQUIRE(TryGetTransformResult<RegisteredTransformResult>(result) == &result.value);
	REQUIRE(TryGetTransformResult<bool>(result) == nullptr);
}

struct TransformLifetimeState {
	idx_t active = 0;
	idx_t started = 0;
	idx_t depth = 257;
	bool throw_at_leaf = false;
	bool throw_in_factory = false;
	vector<idx_t> destroyed;
};

class NestedTestTransformProcess final : public TransformProcess {
public:
	NestedTestTransformProcess(PEGTransformer &transformer, ParseResult &input, TransformLifetimeState &lifetime)
	    : transformer(transformer), input(input.Cast<OptionalParseResult>()), lifetime(lifetime),
	      depth(++lifetime.active) {
		lifetime.started++;
	}

	~NestedTestTransformProcess() override {
		lifetime.destroyed.push_back(depth);
		lifetime.active--;
	}

	TransformStep Resume(arena_ptr<TransformResultValue> child_result) override {
		if (child_result) {
			if (depth == 1 && ++completed_children < 2) {
				return TransformStep::Child({input.GetResult()});
			}
			return TransformStep::Complete(std::move(child_result));
		}
		if (input.HasResult()) {
			return TransformStep::Child({input.GetResult()});
		}
		if (lifetime.throw_at_leaf) {
			throw InvalidInputException("Nested transform test failure");
		}
		return TransformStep::Complete(transformer.MakeResult<bool>(true));
	}

private:
	PEGTransformer &transformer;
	OptionalParseResult &input;
	TransformLifetimeState &lifetime;
	idx_t depth;
	idx_t completed_children = 0;
};

TEST_CASE("Transform stack growth preserves processes and unwinds expression depth", "[api][grammar_extension]") {
	TransformLifetimeState lifetime;
	CompiledGrammarRule rule(
	    "NestedTransformTest",
	    [&lifetime](PEGTransformer &transformer, ParseResult &input) {
		    if (lifetime.throw_in_factory && lifetime.active + 1 == lifetime.depth) {
			    throw InvalidInputException("Nested transform factory failure");
		    }
		    return transformer.MakeProcess<NestedTestTransformProcess>(transformer, input, lifetime);
	    },
	    false, ExpressionDepthKind::NESTING);
	vector<unique_ptr<OptionalParseResult>> nodes;
	for (idx_t i = 0; i < lifetime.depth; i++) {
		auto node = nodes.empty() ? make_uniq<OptionalParseResult>()
		                          : make_uniq<OptionalParseResult>(nodes.back().get(), optional_idx());
		node->SetRule(rule);
		nodes.push_back(std::move(node));
	}
	auto grammar = CompiledGrammar::DefaultGrammar();
	auto options = ParserOptions::Builtin();
	options.max_expression_depth = lifetime.depth;
	vector<MatcherToken> tokens;
	TokenIterator iterator(tokens);
	ArenaAllocator arena(Allocator::DefaultAllocator());
	PEGTransformer transformer(arena, iterator, options, *grammar);
	SECTION("Parents resume after two deep children") {
	}
	SECTION("A child throws while its parents wait") {
		lifetime.throw_at_leaf = true;
	}
	SECTION("A new frame's process factory throws") {
		lifetime.throw_in_factory = true;
	}
	{
		TransformStack stack(transformer);
		if (lifetime.throw_at_leaf || lifetime.throw_in_factory) {
			REQUIRE_THROWS_AS(stack.Execute<bool>({*nodes.back()}), InvalidInputException);
		} else {
			REQUIRE(stack.Execute<bool>({*nodes.back()}));
			REQUIRE(lifetime.started == 1 + 2 * (lifetime.depth - 1));
		}
	}
	REQUIRE(lifetime.active == 0);
	REQUIRE(lifetime.destroyed.size() == lifetime.started);
	REQUIRE(lifetime.destroyed.back() == 1);
	for (idx_t i = 1; i < lifetime.destroyed.size(); i++) {
		REQUIRE((lifetime.destroyed[i] == lifetime.depth || lifetime.destroyed[i] + 1 == lifetime.destroyed[i - 1]));
	}
	// Reusing the transformer at its exact depth limit also checks exceptional depth accounting.
	lifetime.throw_at_leaf = false;
	lifetime.throw_in_factory = false;
	TransformStack stack(transformer);
	REQUIRE(stack.Execute<bool>({*nodes.back()}));
	REQUIRE(lifetime.active == 0);
}

struct TransformAllocationData : PrivateAllocatorData {
	idx_t current = 0;
	idx_t peak = 0;
};

static data_ptr_t TrackedAllocate(PrivateAllocatorData *data, idx_t size) {
	auto result = Allocator::DefaultAllocate(nullptr, size);
	auto &stats = data->Cast<TransformAllocationData>();
	stats.current += size;
	stats.peak = MaxValue(stats.peak, stats.current);
	return result;
}

static void TrackedFree(PrivateAllocatorData *data, data_ptr_t pointer, idx_t size) {
	data->Cast<TransformAllocationData>().current -= size;
	Allocator::DefaultFree(nullptr, pointer, size);
}

static data_ptr_t TrackedReallocate(PrivateAllocatorData *data, data_ptr_t pointer, idx_t old_size, idx_t size) {
	auto result = Allocator::DefaultReallocate(nullptr, pointer, old_size, size);
	auto &stats = data->Cast<TransformAllocationData>();
	stats.current = stats.current - old_size + size;
	stats.peak = MaxValue(stats.peak, stats.current);
	return result;
}

struct TransformMemoryState {
	idx_t active = 0;
	idx_t started = 0;
	idx_t destroyed = 0;
	idx_t leaves = 0;
	idx_t throw_after = 0;
	bool intact = true;
	bool reentrant = false;
};

class ScratchTransformProcess final : public TransformProcess {
public:
	ScratchTransformProcess(PEGTransformer &transformer, ParseResult &input, TransformMemoryState &state)
	    : transformer(transformer), input(input.Cast<OptionalParseResult>()), state(state) {
		state.active++;
		state.started++;
		std::fill_n(scratch, sizeof(scratch), this->input.HasResult() ? 0x35 : 0x77);
	}

	~ScratchTransformProcess() override {
		auto expected = input.HasResult() ? 0x35 : 0x77;
		for (auto value : scratch) {
			state.intact = state.intact && value == expected;
		}
		state.active--;
		state.destroyed++;
	}

	TransformStep Resume(arena_ptr<TransformResultValue> child_result) override {
		if (state.reentrant && input.HasResult()) {
			for (idx_t i = 0; i < 4096; i++) {
				TransformStack nested(transformer);
				if (!nested.Execute<bool>({input.GetResult()})) {
					throw InternalException("Reentrant scratch transform returned false");
				}
			}
			return TransformStep::Complete(transformer.MakeResult<bool>(true));
		}
		if (child_result) {
			auto value = TryGetTransformResult<bool>(*child_result);
			if (!value || !*value) {
				throw InternalException("Scratch transform child result did not survive");
			}
			if (++completed_children == 4096) {
				return TransformStep::Complete(std::move(child_result));
			}
		}
		if (input.HasResult()) {
			return TransformStep::Child({input.GetResult()});
		}
		state.leaves++;
		if (state.throw_after && state.leaves == state.throw_after) {
			throw InvalidInputException("Scratch transform test failure");
		}
		return TransformStep::Complete(transformer.MakeResult<bool>(true));
	}

private:
	PEGTransformer &transformer;
	OptionalParseResult &input;
	TransformMemoryState &state;
	idx_t completed_children = 0;
	uint8_t scratch[4096];
};

static idx_t MeasureTransformScratchMemory(bool throws, bool reentrant) {
	TransformMemoryState state;
	state.reentrant = reentrant;
	CompiledGrammarRule rule("ScratchTransformTest", [&state](PEGTransformer &transformer, ParseResult &input) {
		return transformer.MakeProcess<ScratchTransformProcess>(transformer, input, state);
	});
	OptionalParseResult leaf;
	leaf.SetRule(rule);
	OptionalParseResult root(&leaf, optional_idx());
	root.SetRule(rule);
	auto grammar = CompiledGrammar::DefaultGrammar();
	auto options = ParserOptions::Builtin();
	vector<MatcherToken> tokens;
	TokenIterator iterator(tokens);
	auto data = make_uniq<TransformAllocationData>();
	auto &stats = *data;
	Allocator underlying(TrackedAllocate, TrackedFree, TrackedReallocate, std::move(data));
	{
		ArenaAllocator results(underlying);
		PEGTransformer transformer(results, iterator, options, *grammar);
		for (idx_t attempt = 0; attempt < 2; attempt++) {
			state.leaves = 0;
			state.throw_after = throws && attempt == 0 ? 2048 : 0;
			bool caught = false;
			try {
				TransformStack stack(transformer);
				if (!stack.Execute<bool>({root})) {
					throw InternalException("Scratch transform returned false");
				}
			} catch (InvalidInputException &) {
				caught = true;
			}
			if (caught != bool(state.throw_after) || state.active || !state.intact ||
			    state.started != state.destroyed) {
				throw InternalException("Scratch transform lifetime contract failed");
			}
		}
	}
	if (stats.current) {
		throw InternalException("Scratch transform leaked tracked arena storage");
	}
	return stats.peak;
}

TEST_CASE("Transform process storage follows live frames across siblings and reentry", "[api][grammar_extension]") {
	bool throws = false;
	bool reentrant = false;
	SECTION("Success and reuse") {
	}
	SECTION("Exception and reuse") {
		throws = true;
	}
	SECTION("Reentrant success and reuse") {
		reentrant = true;
	}
	SECTION("Reentrant exception and reuse") {
		throws = true;
		reentrant = true;
	}
	REQUIRE(MeasureTransformScratchMemory(throws, reentrant) < 4 * 1024 * 1024);
}

TEST_CASE("Failed generated process initialization releases child buffers", "[api][grammar_extension]") {
	static const TransformFrameOps ops {
	    "ThrowInInitializer",
	    [](PEGTransformer &, GeneratedTransformProcess &process) {
		    process.ReserveChildSlots(4096);
		    throw InvalidInputException("Generated initializer test failure");
	    },
	    [](PEGTransformer &, GeneratedTransformProcess &) -> arena_ptr<TransformResultValue> {
		    throw InternalException("Failed initializer must not reach finalization");
	    }};
	CompiledGrammarRule rule("ThrowInInitializer", [](PEGTransformer &transformer, ParseResult &input) {
		return transformer.MakeProcess<GeneratedTransformProcess>(transformer, TransformInput {input}, ops);
	});
	OptionalParseResult input;
	input.SetRule(rule);
	auto grammar = CompiledGrammar::DefaultGrammar();
	auto options = ParserOptions::Builtin();
	vector<MatcherToken> tokens;
	TokenIterator iterator(tokens);
	auto data = make_uniq<TransformAllocationData>();
	auto &stats = *data;
	Allocator underlying(TrackedAllocate, TrackedFree, TrackedReallocate, std::move(data));
	{
		ArenaAllocator results(underlying);
		PEGTransformer transformer(results, iterator, options, *grammar);
		for (idx_t i = 0; i < 512; i++) {
			REQUIRE_THROWS_AS(transformer.Transform<bool>(input), InvalidInputException);
		}
		REQUIRE(stats.peak < 4 * 1024 * 1024);
	}
	REQUIRE(stats.current == 0);
}

class GrammarExtensionTestValueTransformProcess final : public TransformProcess {
public:
	explicit GrammarExtensionTestValueTransformProcess(PEGTransformer &transformer_p) : transformer(transformer_p) {
	}

	TransformStep Resume(arena_ptr<TransformResultValue> child_result) override {
		D_ASSERT(!child_result);
		return TransformStep::Complete(transformer.MakeResult<bool>(true));
	}

private:
	PEGTransformer &transformer;
};

class GrammarExtensionTestTransformProcess final : public TransformProcess {
public:
	GrammarExtensionTestTransformProcess(PEGTransformer &transformer_p, ParseResult &parse_result_p)
	    : transformer(transformer_p), parse_result(parse_result_p) {
	}

	TransformStep Resume(arena_ptr<TransformResultValue> child_result) override {
		if (!child_result) {
			auto &list = parse_result.Cast<ListParseResult>();
			return TransformStep::Child({transformer.GetRule("GrammarExtensionTestValue"), list.GetChild(0)});
		}
		D_ASSERT(TryGetTransformResult<bool>(*child_result));
		auto statement = make_uniq<SelectStatement>();
		auto select_node = make_uniq<SelectNode>();
		select_node->select_list.push_back(ConstantExpression::Integer(42));
		select_node->from_table = make_uniq<EmptyTableRef>();
		statement->node = std::move(select_node);
		return TransformStep::Complete(transformer.MakeResult<unique_ptr<SelectStatement>>(std::move(statement)));
	}

private:
	PEGTransformer &transformer;
	ParseResult &parse_result;
};

static arena_ptr<TransformProcess> StartGrammarExtensionTestValueTransform(PEGTransformer &transformer, ParseResult &) {
	return transformer.MakeProcess<GrammarExtensionTestValueTransformProcess>(transformer);
}

static arena_ptr<TransformProcess> StartGrammarExtensionTestTransform(PEGTransformer &transformer,
                                                                      ParseResult &parse_result) {
	return transformer.MakeProcess<GrammarExtensionTestTransformProcess>(transformer, parse_result);
}

class GrammarExtensionTestMatchProcess final : public MatchProcess {
public:
	GrammarExtensionTestMatchProcess(const Matcher &child_p, MatchState &state_p)
	    : child(child_p), state(state_p), child_state(state_p) {
	}

	MatchStep Resume(optional<MatcherResult> child_result) override {
		D_ASSERT(awaiting_child == child_result.has_value());
		if (!child_result) {
			awaiting_child = true;
			return MatchStep::Child({child, child_state});
		}
		awaiting_child = false;
		if (child_result->IsSuccess()) {
			state.token_iterator.SetPosition(child_state.token_iterator);
		}
		return MatchStep::Complete(*child_result);
	}

private:
	const Matcher &child;
	MatchState &state;
	MatchState child_state;
	bool awaiting_child = false;
};

class GrammarExtensionTestMatcher final : public Matcher {
public:
	GrammarExtensionTestMatcher() : child("ANSWER", KeywordInfo()) {
	}

	arena_ptr<MatchProcess> StartMatch(MatchState &state) const override {
		return state.Make<GrammarExtensionTestMatchProcess>(child, state);
	}

	SuggestionType AddSuggestionInternal(MatchState &state) const override {
		return child.AddSuggestion(state);
	}

	string ToString() const override {
		return "GrammarExtensionTestMatcher";
	}

private:
	KeywordMatcher child;
};

class AddGrammarExtensionTestValue final : public GrammarExtension {
public:
	AddGrammarExtensionTestValue() : GrammarExtension("extension_test_value", "GrammarExtensionTestValue") {
	}

	vector<GrammarChange> GetChanges() const override {
		vector<GrammarChange> changes;
		changes.push_back(
		    GrammarChange::AddRule("GrammarExtensionTestValue <- 'WRONG'", StartGrammarExtensionTestValueTransform));
		changes.push_back(GrammarChange::AddChoice("UnreservedKeyword", "'ANSWER'"));
		changes.push_back(GrammarChange::AddTerminalRuleOverride(
		    "GrammarExtensionTestValue", [](const PEGKeywordHelper &keyword_helper) {
			    if (DefaultKeywordMaps::GetKeywordCategory(keyword_helper.LookupKeyword("ANSWER")) !=
			        KeywordCategory::KEYWORD_UNRESERVED) {
				    throw InternalException("Parser change keyword is missing from the compiled keyword helper");
			    }
			    return make_uniq<GrammarExtensionTestMatcher>();
		    }));
		return changes;
	}
};

class AddGrammarExtensionTestAtom final : public GrammarExtension {
public:
	AddGrammarExtensionTestAtom() : GrammarExtension("extension_test_atom", "GrammarExtensionTestAtom") {
	}

	vector<GrammarChange> GetChanges() const override {
		vector<GrammarChange> changes;
		changes.push_back(GrammarChange::AddRule("GrammarExtensionTestAtom <- GrammarExtensionTestValue",
		                                         StartGrammarExtensionTestTransform));
		changes.push_back(
		    GrammarChange::PrependChoice("SelectAtom", "GrammarExtensionTestAtom", [](const PEGExpression &expression) {
			    return expression.type == PEGExpression::Type::REFERENCE &&
			           expression.text.GetString() == "SelectParens";
		    }));
		return changes;
	}
};

static void RegisterGrammarExtensionTestSyntax(DatabaseInstance &db) {
	GrammarExtension::Register(db, make_shared_ptr<AddGrammarExtensionTestValue>());
	GrammarExtension::Register(db, make_shared_ptr<AddGrammarExtensionTestAtom>());
}

static void ActivateGrammarExtensionTestSyntax(Connection &con) {
	REQUIRE_NO_FAIL(*con.Query("SET active_grammar_extensions = ['extension_test_value', 'extension_test_atom']"));
}

TEST_CASE("Literal caches respect active grammar extensions", "[api][grammar_extension]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto base = CompiledGrammar::Get(*con.context);
	auto &base_table = base->GetKeywordHelper().GetLiteralTable();
	vector<MatcherToken> tokens {MatcherToken("answer", 0, TokenType::IDENTIFIER)};
	TokenIterator iterator(tokens);
	REQUIRE(iterator.CurrentLiteralInfo(base_table).LiteralId() == 0);
	RegisterGrammarExtensionTestSyntax(*db.instance);
	ActivateGrammarExtensionTestSyntax(con);
	auto extended = CompiledGrammar::Get(*con.context);
	auto &extended_table = extended->GetKeywordHelper().GetLiteralTable();
	REQUIRE(extended_table.CacheId() != base_table.CacheId());
	REQUIRE(iterator.CurrentLiteralInfo(extended_table)
	            .HasAnyFlags(DefaultKeywordMaps::GetIdentifierMask(SuggestionState::SUGGEST_COLUMN_NAME)));
	REQUIRE(iterator.CurrentLiteralInfo(base_table).LiteralId() == 0);
	KeywordMatcher keyword("ANSWER", KeywordInfo(), extended->GetKeywordHelper());
	REQUIRE(MatchLiteralTestToken(keyword, "answer"));
	REQUIRE_FALSE(MatchLiteralTestToken(keyword, "missing"));
	REQUIRE_NO_FAIL(*con.Query("SET active_grammar_extensions = []"));
	REQUIRE(CompiledGrammar::Get(*con.context) == base);
}

static void CheckGrammarExtensionTestSyntax(Connection &con) {
	auto result = con.Query("ANSWER");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Collection().GetValue(0, 0) == Value::INTEGER(42));
}

TEST_CASE("Grammar extensions apply in registration order", "[api][grammar_extension]") {
	DuckDB db(nullptr);
	RegisterGrammarExtensionTestSyntax(*db.instance);
	Connection con(db);
	ActivateGrammarExtensionTestSyntax(con);
	CheckGrammarExtensionTestSyntax(con);
}

TEST_CASE("add_grammar_extension appends to the active grammar extensions", "[api][grammar_extension]") {
	DuckDB db(nullptr);
	RegisterGrammarExtensionTestSyntax(*db.instance);
	Connection set_con(db);
	ActivateGrammarExtensionTestSyntax(set_con);

	// adding the extensions one by one compiles the same grammar as setting them at once
	Connection con(db);
	auto base = CompiledGrammar::Get(*con.context);
	REQUIRE_NO_FAIL(*con.Query("CALL add_grammar_extension('extension_test_value')"));
	REQUIRE(CompiledGrammar::Get(*con.context) != base);
	REQUIRE_NO_FAIL(*con.Query("CALL add_grammar_extension('extension_test_atom')"));
	REQUIRE(CompiledGrammar::Get(*con.context) == CompiledGrammar::Get(*set_con.context));
	CheckGrammarExtensionTestSyntax(con);

	// the active extensions are reported by the setting and by duckdb_grammar_extensions
	auto result = con.Query("SELECT current_setting('active_grammar_extensions')");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Collection().GetValue(0, 0).ToString() == "[extension_test_value, extension_test_atom]");
	result = con.Query("SELECT name FROM duckdb_grammar_extensions() WHERE active ORDER BY name");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->RowCount() == 2);

	// removing one keeps the other active
	REQUIRE_NO_FAIL(*con.Query("CALL remove_grammar_extension('extension_test_atom')"));
	Connection value_con(db);
	REQUIRE_NO_FAIL(*value_con.Query("SET active_grammar_extensions = ['extension_test_value']"));
	REQUIRE(CompiledGrammar::Get(*con.context) == CompiledGrammar::Get(*value_con.context));
	REQUIRE_FAIL(con.Query("ANSWER"));

	// removing the last one falls back to the default grammar
	REQUIRE_NO_FAIL(*con.Query("CALL remove_grammar_extension('extension_test_value')"));
	REQUIRE(CompiledGrammar::Get(*con.context) == base);
	result = con.Query("SELECT count(*) FROM duckdb_grammar_extensions() WHERE active");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Collection().GetValue(0, 0) == Value::BIGINT(0));
}

class ConstantSevenTransformProcess final : public TransformProcess {
public:
	explicit ConstantSevenTransformProcess(PEGTransformer &transformer_p) : transformer(transformer_p) {
	}

	TransformStep Resume(arena_ptr<TransformResultValue> child_result) override {
		D_ASSERT(!child_result);
		return TransformStep::Complete(
		    transformer.MakeResult<unique_ptr<ParsedExpression>>(ConstantExpression::Integer(7)));
	}

private:
	PEGTransformer &transformer;
};

static arena_ptr<TransformProcess> StartConstantSevenTransform(PEGTransformer &transformer, ParseResult &) {
	return transformer.MakeProcess<ConstantSevenTransformProcess>(transformer);
}

//! Replaces the transform of a rule the default grammar marks as collapsible with one that does not forward its
//! operand
class OverrideAdditiveExpressionTransform final : public GrammarExtension {
public:
	OverrideAdditiveExpressionTransform(const string &name, bool collapsible_p)
	    : GrammarExtension(name, "OverrideAdditiveExpressionTransform"), collapsible(collapsible_p) {
	}

	vector<GrammarChange> GetChanges() const override {
		vector<GrammarChange> changes;
		changes.push_back(
		    GrammarChange::SetTransformProcess("AdditiveExpression", StartConstantSevenTransform, collapsible));
		return changes;
	}

private:
	bool collapsible;
};

TEST_CASE("Generated transform rule references belong to the active compiled grammar", "[api][grammar_extension]") {
	auto original = CompiledGrammar::DefaultGrammar();
	OverrideAdditiveExpressionTransform extension("seven", false);
	auto modified = CompiledGrammar::Create({extension});
	for (idx_t i = 1; i < static_cast<idx_t>(PEGRuleId::BUILTIN_COUNT); i++) {
		auto id = static_cast<PEGRuleId>(i);
		auto name = PEGRuleIdToString(id);
		REQUIRE(original->GetRule(id).get() == original->GetRule(name).get());
		REQUIRE(modified->GetRule(id).get() == modified->GetRule(name).get());
	}
	auto id = PEGRuleId::AdditiveExpression;
	REQUIRE(original->GetRule(id));
	REQUIRE(modified->GetRule(id));
	REQUIRE(original->GetRule(id).get() != modified->GetRule(id).get());
	REQUIRE_FALSE(modified->GetRule(id)->collapsible);
	auto options = ParserOptions::Builtin();
	vector<MatcherToken> tokens;
	TokenIterator iterator(tokens);
	ArenaAllocator arena(Allocator::DefaultAllocator());
	PEGTransformer first(arena, iterator, options, *original);
	PEGTransformer second(arena, iterator, options, *modified);
	REQUIRE(&first.GetRule(id) == original->GetRule(id).get());
	REQUIRE(&second.GetRule(id) == modified->GetRule(id).get());
}

TEST_CASE("Small grammars resolve missing generated references only when used", "[api][grammar_extension]") {
	auto parsed = ParsedGrammar::Parse("Program <- TopLevelStatement\nTopLevelStatement <- 'SELECT'");
	compiled_rules_map_t rules;
	for (auto name : {"Program", "TopLevelStatement"}) {
		auto rule = parsed.GetRule(name);
		REQUIRE(rule);
		rules.emplace(name, make_uniq<CompiledGrammarRule>(name, rule->transform_process));
	}
	auto keyword_grammar = ParsedGrammar::CreateDefault();
	auto keywords = make_uniq<ParsedGrammarKeywordHelper>(keyword_grammar);
	auto tokenizer = make_uniq<Tokenizer>(*keywords);
	MatcherAllocator allocator;
	MatcherFactory factory(allocator, parsed, rules, *keywords, {});
	auto &program = factory.CreateRootMatcher("Program");
	auto &top_level = factory.GetMatcher("TopLevelStatement");
	CompiledGrammar grammar(std::move(allocator), std::move(keywords), std::move(tokenizer), std::move(rules), program,
	                        top_level);
	auto id = PEGRuleId::AdditiveExpression;
	REQUIRE_FALSE(grammar.GetRule(id));
	REQUIRE_FALSE(grammar.GetRule("AdditiveExpression"));
	auto options = ParserOptions::Builtin();
	vector<MatcherToken> tokens;
	TokenIterator iterator(tokens);
	ArenaAllocator arena(Allocator::DefaultAllocator());
	PEGTransformer transformer(arena, iterator, options, grammar);
#ifndef DUCKDB_CRASH_ON_ASSERT
	// InternalException aborts instead of throwing in crash-on-assert builds.
	REQUIRE_THROWS_AS(transformer.GetRule(id), InternalException);
	REQUIRE_THROWS_AS(transformer.GetRule("AdditiveExpression"), InternalException);
#endif
}

TEST_CASE("Overriding a transform opts its rule out of collapsing", "[api][grammar_extension]") {
	DuckDB db(nullptr);
	GrammarExtension::Register(*db.instance, make_shared_ptr<OverrideAdditiveExpressionTransform>("seven", false));
	GrammarExtension::Register(*db.instance,
	                           make_shared_ptr<OverrideAdditiveExpressionTransform>("collapsible_seven", true));
	// separate connections, because once an extension is active it also rewrites the literals of a later SET
	Connection con(db);
	REQUIRE_NO_FAIL(*con.Query("SET active_grammar_extensions = ['seven']"));
	auto result = con.Query("SELECT 1");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Collection().GetValue(0, 0) == Value::INTEGER(7));

	// the extension promised the collapse is safe, so an operand without a tail skips its transform
	Connection collapsible_con(db);
	REQUIRE_NO_FAIL(*collapsible_con.Query("SET active_grammar_extensions = ['collapsible_seven']"));
	result = collapsible_con.Query("SELECT 1");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Collection().GetValue(0, 0) == Value::INTEGER(1));
	result = collapsible_con.Query("SELECT 1 + 1");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Collection().GetValue(0, 0) == Value::INTEGER(7));
}

TEST_CASE("Connections share the grammar compiled for the same active grammar extensions", "[api][grammar_extension]") {
	DuckDB db(nullptr);
	GrammarExtension::Register(*db.instance, make_shared_ptr<OverrideAdditiveExpressionTransform>("seven", false));
	GrammarExtension::Register(*db.instance,
	                           make_shared_ptr<OverrideAdditiveExpressionTransform>("collapsible_seven", true));
	Connection con(db);
	REQUIRE_NO_FAIL(*con.Query("SET active_grammar_extensions = ['seven', 'collapsible_seven']"));
	auto grammar = CompiledGrammar::Get(*con.context);

	// the names of grammar extensions are case insensitive
	Connection same_con(db);
	REQUIRE_NO_FAIL(*same_con.Query("SET active_grammar_extensions = ['SEVEN', 'collapsible_seven']"));
	REQUIRE(CompiledGrammar::Get(*same_con.context) == grammar);

	// the order of the grammar extensions determines the grammar: the last transform override wins
	Connection reversed_con(db);
	REQUIRE_NO_FAIL(*reversed_con.Query("SET active_grammar_extensions = ['collapsible_seven', 'seven']"));
	REQUIRE(CompiledGrammar::Get(*reversed_con.context) != grammar);
	auto result = con.Query("SELECT 1");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Collection().GetValue(0, 0) == Value::INTEGER(1));
	result = reversed_con.Query("SELECT 1");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Collection().GetValue(0, 0) == Value::INTEGER(7));

	Connection subset_con(db);
	REQUIRE_NO_FAIL(*subset_con.Query("SET active_grammar_extensions = ['seven']"));
	REQUIRE(CompiledGrammar::Get(*subset_con.context) != grammar);
	REQUIRE(CompiledGrammar::Get(*subset_con.context) != CompiledGrammar::Get(*reversed_con.context));
}

//! Replaces the transform of AdditiveExpression like OverrideAdditiveExpressionTransform, and counts how often a
//! grammar is compiled with it
class CountingSevenGrammarExtension final : public GrammarExtension {
public:
	CountingSevenGrammarExtension(const string &name, std::atomic<idx_t> &compilations_p)
	    : GrammarExtension(name, "CountingSevenGrammarExtension"), compilations(compilations_p) {
	}

	vector<GrammarChange> GetChanges() const override {
		compilations++;
		vector<GrammarChange> changes;
		changes.push_back(GrammarChange::SetTransformProcess("AdditiveExpression", StartConstantSevenTransform, false));
		return changes;
	}

private:
	std::atomic<idx_t> &compilations;
};

static void RequireSeven(Connection &con) {
	auto result = con.Query("SELECT 1");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Collection().GetValue(0, 0) == Value::INTEGER(7));
}

TEST_CASE("Grammars for active grammar extensions are compiled once and outlive the connections that set them",
          "[api][grammar_extension]") {
	DuckDB db(nullptr);
	std::atomic<idx_t> compilations(0);
	GrammarExtension::Register(*db.instance, make_shared_ptr<CountingSevenGrammarExtension>("counted", compilations));

	// connections that set the same extensions concurrently compile the grammar once
	const idx_t thread_count = 8;
	vector<shared_ptr<CompiledGrammar>> grammars(thread_count);
	vector<uint8_t> succeeded(thread_count, 0);
	vector<std::thread> threads;
	for (idx_t i = 0; i < thread_count; i++) {
		threads.emplace_back([&, i]() {
			Connection con(db);
			auto set_result = con.Query("SET active_grammar_extensions = ['counted']");
			auto result = con.Query("SELECT 1");
			succeeded[i] = !set_result->HasError() && !result->HasError() &&
			               result->Collection().GetValue(0, 0) == Value::INTEGER(7);
			grammars[i] = CompiledGrammar::Get(*con.context);
		});
	}
	for (auto &thread : threads) {
		thread.join();
	}
	REQUIRE(compilations == 1);
	for (idx_t i = 0; i < thread_count; i++) {
		REQUIRE(succeeded[i]);
		REQUIRE(grammars[i] == grammars[0]);
	}

	// setting the extensions again after a RESET uses the cached grammar
	Connection con(db);
	REQUIRE_NO_FAIL(*con.Query("SET active_grammar_extensions = ['counted']"));
	REQUIRE(CompiledGrammar::Get(*con.context) == grammars[0]);
	REQUIRE_NO_FAIL(*con.Query("RESET active_grammar_extensions"));
	REQUIRE(CompiledGrammar::Get(*con.context) != grammars[0]);
	REQUIRE_NO_FAIL(*con.Query("SET active_grammar_extensions = ['counted']"));
	REQUIRE(CompiledGrammar::Get(*con.context) == grammars[0]);
	REQUIRE(compilations == 1);

	// the grammar stays usable for a connection after the connection that compiled it is gone
	auto other_con = make_uniq<Connection>(db);
	Connection shared_con(db);
	REQUIRE_NO_FAIL(*other_con->Query("SET active_grammar_extensions = ['counted']"));
	REQUIRE_NO_FAIL(*shared_con.Query("SET active_grammar_extensions = ['counted']"));
	other_con.reset();
	REQUIRE_NO_FAIL(*con.Query("RESET active_grammar_extensions"));
	RequireSeven(shared_con);
	REQUIRE(compilations == 1);
}

//! Rewrites every additive expression to the constant seven
class SevenGrammarExtension final : public GrammarExtension {
public:
	explicit SevenGrammarExtension(const string &name) : GrammarExtension(name, "SevenGrammarExtension") {
	}

	vector<GrammarChange> GetChanges() const override {
		vector<GrammarChange> changes;
		changes.push_back(GrammarChange::SetTransformProcess("AdditiveExpression", StartConstantSevenTransform, false));
		return changes;
	}
};

//! The rendered constant of the single select-list expression of a parsed SELECT
static string ParsedSelectConstant(const SQLStatement &statement) {
	auto &select = statement.Cast<SelectStatement>().node->Cast<SelectNode>();
	REQUIRE(select.select_list[0]->GetExpressionClass() == ExpressionClass::CONSTANT);
	return select.select_list[0]->ToString();
}

//! SET through the C++ setting entry: the seven/eight transforms rewrite every parsed expression,
//! so a SQL SET statement's own list value could not survive parsing
static void SetActiveGrammarExtensions(Connection &con, const vector<Value> &names) {
	ActiveGrammarExtensionsSetting::SetLocal(*con.context, Value::LIST(LogicalType::VARCHAR, names));
}

//! SET GLOBAL through the C++ setting entry, for the same reason as SetActiveGrammarExtensions
static void SetDefaultGrammarExtensions(DuckDB &db, const vector<Value> &names) {
	DefaultGrammarExtensionsSetting::SetGlobal(db.instance.get(), db.instance->config,
	                                           Value::LIST(LogicalType::VARCHAR, names));
}

TEST_CASE("The database default applies to sessions without an explicit selection", "[api][grammar_extension]") {
	DuckDB db(nullptr);
	Connection con(db);
	Connection empty_con(db);
	auto result = empty_con.Query("SELECT 1");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Collection().GetValue(0, 0) == Value::INTEGER(1));
	auto base_grammar = CompiledGrammar::Get(*con.context);
	// registration alone activates nothing
	GrammarExtension::Register(*db.instance, make_shared_ptr<SevenGrammarExtension>("seven"));
	result = con.Query("SELECT 1");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Collection().GetValue(0, 0) == Value::INTEGER(1));

	// one iterator across the global SET: the second statement must parse with the new default,
	// the snapshot identity (not only the registry version) invalidates the session cache
	ParseIterator iterator(*empty_con.context, "SELECT 1; SELECT 1", false, nullptr);
	REQUIRE(iterator.Peek());
	auto first = iterator.GetStatement();
	REQUIRE(ParsedSelectConstant(*first) == "1");
	SetDefaultGrammarExtensions(db, {Value("seven")});
	REQUIRE(iterator.Peek());
	auto second = iterator.GetStatement();
	REQUIRE(ParsedSelectConstant(*second) == "7");

	// every session without an explicit selection parses with the default, existing or new
	RequireSeven(con);
	RequireSeven(empty_con);
	Connection later(db);
	RequireSeven(later);
	REQUIRE(CompiledGrammar::Get(*con.context) != base_grammar);

	// an explicitly empty session selection opts out, RESET returns to the database default
	SetActiveGrammarExtensions(con, {});
	result = con.Query("SELECT 1");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Collection().GetValue(0, 0) == Value::INTEGER(1));
	RequireSeven(later);
	REQUIRE_NO_FAIL(*con.Query("RESET active_grammar_extensions"));
	RequireSeven(con);

	// resetting the global deactivates it for every session without an explicit selection
	DefaultGrammarExtensionsSetting::ResetGlobal(db.instance.get(), db.instance->config);
	result = con.Query("SELECT 1");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Collection().GetValue(0, 0) == Value::INTEGER(1));
}

class ConstantEightTransformProcess final : public TransformProcess {
public:
	explicit ConstantEightTransformProcess(PEGTransformer &transformer_p) : transformer(transformer_p) {
	}

	TransformStep Resume(arena_ptr<TransformResultValue> child_result) override {
		D_ASSERT(!child_result);
		return TransformStep::Complete(
		    transformer.MakeResult<unique_ptr<ParsedExpression>>(ConstantExpression::Integer(8)));
	}

private:
	PEGTransformer &transformer;
};

static arena_ptr<TransformProcess> StartConstantEightTransform(PEGTransformer &transformer, ParseResult &) {
	return transformer.MakeProcess<ConstantEightTransformProcess>(transformer);
}

//! Overrides the same rule as SevenGrammarExtension and yields 8
class OverrideAdditiveExpressionEight final : public GrammarExtension {
public:
	explicit OverrideAdditiveExpressionEight(const string &name)
	    : GrammarExtension(name, "OverrideAdditiveExpressionEight") {
	}

	vector<GrammarChange> GetChanges() const override {
		vector<GrammarChange> changes;
		changes.push_back(GrammarChange::SetTransformProcess("AdditiveExpression", StartConstantEightTransform, false));
		return changes;
	}
};

static void RequireEight(Connection &con) {
	auto result = con.Query("SELECT 1");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Collection().GetValue(0, 0) == Value::INTEGER(8));
}

TEST_CASE("The session selection replaces the database default", "[api][grammar_extension]") {
	DuckDB db(nullptr);
	GrammarExtension::Register(*db.instance, make_shared_ptr<SevenGrammarExtension>("seven"));
	GrammarExtension::Register(*db.instance, make_shared_ptr<OverrideAdditiveExpressionEight>("explicit_eight"));
	SetDefaultGrammarExtensions(db, {Value("seven")});
	Connection con(db);
	RequireSeven(con);
	// a session selection does not compose with the default, it replaces it
	SetActiveGrammarExtensions(con, {Value("explicit_eight")});
	RequireEight(con);
	// both listed: the later grammar wins, names are case insensitive
	SetActiveGrammarExtensions(con, {Value("explicit_eight"), Value("SEVEN")});
	RequireSeven(con);
	SetActiveGrammarExtensions(con, {Value("seven"), Value("explicit_eight")});
	RequireEight(con);
	// the settings surface the session list and the database default independently
	auto session_value = ActiveGrammarExtensionsSetting::GetSetting(*con.context);
	REQUIRE(ListValue::GetChildren(session_value).size() == 2);
	auto default_value = DefaultGrammarExtensionsSetting::GetSetting(*con.context);
	REQUIRE(ListValue::GetChildren(default_value).size() == 1);
	REQUIRE(ListValue::GetChildren(default_value)[0] == Value("seven"));
	// RESET returns the session to the database default
	REQUIRE_NO_FAIL(*con.Query("RESET active_grammar_extensions"));
	RequireSeven(con);
	session_value = ActiveGrammarExtensionsSetting::GetSetting(*con.context);
	REQUIRE(ListValue::GetChildren(session_value).empty());
	// a local NULL also returns to the database default
	SetActiveGrammarExtensions(con, {Value("explicit_eight")});
	RequireEight(con);
	ActiveGrammarExtensionsSetting::SetLocal(*con.context, Value(LogicalType::LIST(LogicalType::VARCHAR)));
	RequireSeven(con);
}

//! Grammar adding a rule another grammar can reference
class ProvidingGrammarExtension final : public GrammarExtension {
public:
	ProvidingGrammarExtension() : GrammarExtension("providing_grammar", "ProvidingGrammarExtension") {
	}

	vector<GrammarChange> GetChanges() const override {
		vector<GrammarChange> changes;
		changes.push_back(GrammarChange::AddRule("ProvidedTestRule <- 'PROVIDEDTESTTOKEN'"));
		return changes;
	}
};

//! Grammar that only compiles together with the providing grammar
class DependentGrammarExtension final : public GrammarExtension {
public:
	DependentGrammarExtension() : GrammarExtension("dependent_grammar", "DependentGrammarExtension") {
	}

	vector<GrammarChange> GetChanges() const override {
		vector<GrammarChange> changes;
		changes.push_back(GrammarChange::AddRule("DependentTestRule <- ProvidedTestRule"));
		return changes;
	}
};

TEST_CASE("The session selection does not compose with the database default", "[api][grammar_extension]") {
	DuckDB db(nullptr);
	GrammarExtension::Register(*db.instance, make_shared_ptr<ProvidingGrammarExtension>());
	GrammarExtension::Register(*db.instance, make_shared_ptr<DependentGrammarExtension>());
	SetDefaultGrammarExtensions(db, {Value("providing_grammar")});
	Connection con(db);
	REQUIRE_NO_FAIL(*con.Query("SELECT 1"));
	// the dependent grammar compiles only together with the provider: a session selection that
	// composed WITH the default would succeed here, replacement must fail
	REQUIRE_THROWS(SetActiveGrammarExtensions(con, {Value("dependent_grammar")}));
	REQUIRE_NO_FAIL(*con.Query("SELECT 1"));
	// listing both in the session selection composes
	SetActiveGrammarExtensions(con, {Value("providing_grammar"), Value("dependent_grammar")});
	REQUIRE_NO_FAIL(*con.Query("SELECT 1"));
	// clearing a nonempty database default with an empty list and with NULL deactivates it
	GrammarExtension::Register(*db.instance, make_shared_ptr<SevenGrammarExtension>("seven_b"));
	SetDefaultGrammarExtensions(db, {Value("seven_b")});
	REQUIRE_NO_FAIL(*con.Query("RESET active_grammar_extensions"));
	RequireSeven(con);
	SetDefaultGrammarExtensions(db, {});
	auto result = con.Query("SELECT 1");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Collection().GetValue(0, 0) == Value::INTEGER(1));
	SetDefaultGrammarExtensions(db, {Value("seven_b")});
	RequireSeven(con);
	DefaultGrammarExtensionsSetting::SetGlobal(db.instance.get(), db.instance->config,
	                                           Value(LogicalType::LIST(LogicalType::VARCHAR)));
	result = con.Query("SELECT 1");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Collection().GetValue(0, 0) == Value::INTEGER(1));
}

TEST_CASE("Add and remove grammar extensions act on the effective selection", "[api][grammar_extension]") {
	DuckDB db(nullptr);
	GrammarExtension::Register(*db.instance, make_shared_ptr<ProvidingGrammarExtension>());
	GrammarExtension::Register(*db.instance, make_shared_ptr<SevenGrammarExtension>("seven"));
	SetDefaultGrammarExtensions(db, {Value("providing_grammar")});

	// a no-op add on a default-driven session must not install a session selection
	Connection noop_con(db);
	REQUIRE_NO_FAIL(*noop_con.Query("CALL add_grammar_extension('providing_grammar')"));
	REQUIRE(ListValue::GetChildren(ActiveGrammarExtensionsSetting::GetSetting(*noop_con.context)).empty());

	// adding snapshots the default into the session selection, keeping its entries in order
	Connection con(db);
	REQUIRE_NO_FAIL(*con.Query("CALL add_grammar_extension('seven')"));
	auto children = ListValue::GetChildren(ActiveGrammarExtensionsSetting::GetSetting(*con.context));
	REQUIRE(children.size() == 2);
	REQUIRE(children[0] == Value("providing_grammar"));
	REQUIRE(children[1] == Value("seven"));
	RequireSeven(con);

	// the snapshot decouples that session from later default changes, the no-op add did not
	SetDefaultGrammarExtensions(db, {});
	RequireSeven(con);
	// the inherited entry stays in the stored session selection after the default change
	children = ListValue::GetChildren(ActiveGrammarExtensionsSetting::GetSetting(*con.context));
	REQUIRE(children.size() == 2);
	REQUIRE(children[0] == Value("providing_grammar"));
	auto noop_active =
	    noop_con.Query("SELECT active FROM duckdb_grammar_extensions() WHERE name = 'providing_grammar'");
	REQUIRE_NO_FAIL(*noop_active);
	REQUIRE(noop_active->Collection().GetValue(0, 0) == Value::BOOLEAN(false));
	// restoring the default re-applies to the no-op session, so the no-op installed no opt-out
	SetDefaultGrammarExtensions(db, {Value("providing_grammar")});
	auto noop_follows =
	    noop_con.Query("SELECT active FROM duckdb_grammar_extensions() WHERE name = 'providing_grammar'");
	REQUIRE_NO_FAIL(*noop_follows);
	REQUIRE(noop_follows->Collection().GetValue(0, 0) == Value::BOOLEAN(true));
	SetDefaultGrammarExtensions(db, {});

	// removing a default-provided grammar opts the session out without touching the default
	SetDefaultGrammarExtensions(db, {Value("providing_grammar")});
	Connection removing(db);
	auto before = removing.Query("SELECT active FROM duckdb_grammar_extensions() WHERE name = 'providing_grammar'");
	REQUIRE_NO_FAIL(*before);
	REQUIRE(before->Collection().GetValue(0, 0) == Value::BOOLEAN(true));
	REQUIRE_NO_FAIL(*removing.Query("CALL remove_grammar_extension('providing_grammar')"));
	auto after = removing.Query("SELECT active FROM duckdb_grammar_extensions() WHERE name = 'providing_grammar'");
	REQUIRE_NO_FAIL(*after);
	REQUIRE(after->Collection().GetValue(0, 0) == Value::BOOLEAN(false));
	Connection still_default(db);
	auto fresh_active =
	    still_default.Query("SELECT active FROM duckdb_grammar_extensions() WHERE name = 'providing_grammar'");
	REQUIRE_NO_FAIL(*fresh_active);
	REQUIRE(fresh_active->Collection().GetValue(0, 0) == Value::BOOLEAN(true));

	// a no-op remove of an inactive name leaves the selection untouched: the session still
	// follows the default rather than holding an installed empty opt-out
	REQUIRE_NO_FAIL(*still_default.Query("CALL remove_grammar_extension('seven')"));
	REQUIRE(ListValue::GetChildren(ActiveGrammarExtensionsSetting::GetSetting(*still_default.context)).empty());
	auto still_follows =
	    still_default.Query("SELECT active FROM duckdb_grammar_extensions() WHERE name = 'providing_grammar'");
	REQUIRE_NO_FAIL(*still_follows);
	REQUIRE(still_follows->Collection().GetValue(0, 0) == Value::BOOLEAN(true));
}

TEST_CASE("The database default rejects values it cannot validate", "[api][grammar_extension]") {
	DuckDB db(nullptr);
	Connection con(db);
	// unknown names fail at SET and leave the default unchanged
	REQUIRE_FAIL(con.Query("SET GLOBAL default_grammar_extensions = ['no_such_grammar']"));
	auto default_value = DefaultGrammarExtensionsSetting::GetSetting(*con.context);
	REQUIRE(ListValue::GetChildren(default_value).empty());
	REQUIRE_NO_FAIL(*con.Query("SELECT 1"));
	// without a running database the named extensions cannot be validated, so the value is rejected:
	// a broken default would make every parse fail, including the SET that fixes it
	DBConfig config;
	REQUIRE_THROWS(config.SetOptionByName(Identifier("default_grammar_extensions"),
	                                      Value::LIST(LogicalType::VARCHAR, {Value("not_loaded")})));
	// a registered selection that does not compile: the eager composition fails the SET and the
	// previous default stays, so sessions never meet a default they cannot parse with
	GrammarExtension::Register(*db.instance, make_shared_ptr<ProvidingGrammarExtension>());
	GrammarExtension::Register(*db.instance, make_shared_ptr<DependentGrammarExtension>());
	REQUIRE_NO_FAIL(*con.Query("SET GLOBAL default_grammar_extensions = ['providing_grammar', 'dependent_grammar']"));
	REQUIRE_NO_FAIL(*con.Query("SELECT 1"));
	REQUIRE_FAIL(con.Query("SET GLOBAL default_grammar_extensions = ['dependent_grammar']"));
	default_value = DefaultGrammarExtensionsSetting::GetSetting(*con.context);
	REQUIRE(ListValue::GetChildren(default_value).size() == 2);
	REQUIRE_NO_FAIL(*con.Query("SELECT 1"));
	// RESET deactivates the default for sessions without an explicit selection
	REQUIRE_NO_FAIL(*con.Query("RESET GLOBAL default_grammar_extensions"));
	default_value = DefaultGrammarExtensionsSetting::GetSetting(*con.context);
	REQUIRE(ListValue::GetChildren(default_value).empty());
}

TEST_CASE("The least recently used grammar for active grammar extensions is evicted", "[api][grammar_extension]") {
	DuckDB db(nullptr);
	std::atomic<idx_t> compilations(0);
	const idx_t extension_count = ParserCache::MAX_CACHED_EXTENSION_MATCHERS + 1;
	for (idx_t i = 0; i < extension_count; i++) {
		GrammarExtension::Register(
		    *db.instance, make_shared_ptr<CountingSevenGrammarExtension>("counted_" + to_string(i), compilations));
	}
	auto set_extension = [&](idx_t i) {
		Connection con(db);
		REQUIRE_NO_FAIL(*con.Query("SET active_grammar_extensions = ['counted_" + to_string(i) + "']"));
		RequireSeven(con);
		return CompiledGrammar::Get(*con.context);
	};
	auto first = set_extension(0);
	for (idx_t i = 1; i < ParserCache::MAX_CACHED_EXTENSION_MATCHERS; i++) {
		set_extension(i);
	}
	REQUIRE(compilations == ParserCache::MAX_CACHED_EXTENSION_MATCHERS);
	// using the first grammar again makes it the most recently used one, so adding another evicts the second
	REQUIRE(set_extension(0) == first);
	set_extension(ParserCache::MAX_CACHED_EXTENSION_MATCHERS);
	REQUIRE(compilations == ParserCache::MAX_CACHED_EXTENSION_MATCHERS + 1);
	REQUIRE(set_extension(0) == first);
	REQUIRE(compilations == ParserCache::MAX_CACHED_EXTENSION_MATCHERS + 1);
	set_extension(1);
	REQUIRE(compilations == ParserCache::MAX_CACHED_EXTENSION_MATCHERS + 2);
}

struct MatchProcessLifetimeState {
	idx_t active = 0;
	idx_t started = 0;
	idx_t depth = 0;
	bool throw_at_leaf = false;
	bool throw_in_constructor = false;
	bool storage_valid = true;
	bool create_result = false;
	bool fail_at_leaf = false;
	bool state_valid = true;
	idx_t root_children = 1;
	idx_t peak_arena_bytes = 0;
	vector<idx_t> destroyed;
};

class NestedTestMatchProcess : public MatchProcess {
public:
	NestedTestMatchProcess(const Matcher &matcher_p, MatchState &state, MatchProcessLifetimeState &lifetime_p)
	    : matcher(matcher_p), input_state(state), child_state(state), lifetime(lifetime_p), depth(++lifetime.active) {
		lifetime.started++;
	}

	~NestedTestMatchProcess() override {
		lifetime.state_valid &= &input_state.context == &child_state.context;
		lifetime.destroyed.push_back(depth);
		lifetime.active--;
	}

	MatchStep Resume(optional<MatcherResult> child_result) override {
		if (child_result) {
			if (depth == 1 && ++completed_children < lifetime.root_children) {
				return MatchStep::Child({matcher, child_state});
			}
			return MatchStep::Complete(*child_result);
		}
		if (depth < lifetime.depth) {
			return MatchStep::Child({matcher, child_state});
		}
		if (lifetime.throw_at_leaf) {
			throw InvalidInputException("Nested matcher test failure");
		}
		if (lifetime.fail_at_leaf) {
			return MatchStep::Complete(MatcherResult::Failure());
		}
		if (lifetime.create_result) {
			arena_vector<reference<ParseResult>> no_children(child_state.context.process_allocator);
			return MatchStep::Complete(child_state.AllocateParseResult<ListParseResult>(
			    child_state.context.allocator.MakeChildren(no_children), &matcher, optional_idx()));
		}
		return MatchStep::Complete(MatcherResult::Success());
	}

private:
	const Matcher &matcher;
	MatchState &input_state;
	MatchState child_state;
	MatchProcessLifetimeState &lifetime;
	idx_t depth;
	idx_t completed_children = 0;
};

class NestedTestMatcher final : public Matcher {
public:
	explicit NestedTestMatcher(MatchProcessLifetimeState &lifetime_p)
	    : Matcher(MatcherType::LIST), lifetime(lifetime_p) {
		SetName("nested result");
	}

	arena_ptr<MatchProcess> StartMatch(MatchState &state) const override {
		return state.Make<NestedTestMatchProcess>(*this, state, lifetime);
	}

	SuggestionType AddSuggestionInternal(MatchState &) const override {
		return SuggestionType::MANDATORY;
	}

	string ToString() const override {
		return "NestedTestMatcher";
	}

private:
	MatchProcessLifetimeState &lifetime;
};

TEST_CASE("Matcher stack vector growth preserves custom process lifetimes", "[api][grammar_extension]") {
	vector<MatcherToken> tokens;
	TokenIterator iterator(tokens);
	vector<MatcherSuggestion> suggestions;
	ParseResultAllocator allocator;
	idx_t max_token_index = 0;
	ArenaAllocator process_allocator(Allocator::DefaultAllocator());
	MatchContext context(suggestions, allocator, process_allocator, max_token_index);
	MatchState state(iterator, context);
	MatchProcessLifetimeState lifetime;
	lifetime.destroyed.reserve(2049);
	NestedTestMatcher matcher(lifetime);

	SECTION("Completed frames are reused across vector growth boundaries") {
		MatchStack stack;
		lifetime.create_result = true;
		for (idx_t depth : {idx_t(1), idx_t(64), idx_t(65), idx_t(128), idx_t(129), idx_t(256), idx_t(257), idx_t(1025),
		                    idx_t(33), idx_t(130)}) {
			process_allocator.Reset();
			lifetime.depth = depth;
			lifetime.started = 0;
			lifetime.destroyed.clear();
			auto result = stack.Execute({matcher, state});
			REQUIRE(result.IsSuccess());
			REQUIRE(result.HasParseResult());
			REQUIRE(result.GetParseResult()->Name() == "nested result");
			REQUIRE(lifetime.active == 0);
			REQUIRE(lifetime.state_valid);
			REQUIRE(lifetime.started == depth);
			REQUIRE(lifetime.destroyed.size() == depth);
			for (idx_t i = 0; i < depth; i++) {
				REQUIRE(lifetime.destroyed[i] == depth - i);
			}
		}
	}

	SECTION("Exceptions destroy child processes before their parents") {
		lifetime.depth = 1025;
		lifetime.throw_at_leaf = true;
		{
			MatchStack stack;
			REQUIRE_THROWS_AS(stack.Execute({matcher, state}), InvalidInputException);
		}
		REQUIRE(lifetime.active == 0);
		REQUIRE(lifetime.state_valid);
		REQUIRE(lifetime.started == lifetime.depth);
		REQUIRE(lifetime.destroyed.size() == lifetime.depth);
		for (idx_t i = 0; i < lifetime.depth; i++) {
			REQUIRE(lifetime.destroyed[i] == lifetime.depth - i);
		}
	}

	SECTION("A waiting parent resumes after multiple deep children") {
		lifetime.depth = 1025;
		lifetime.root_children = 2;
		MatchStack stack;
		REQUIRE(stack.Execute({matcher, state}).IsSuccess());
		REQUIRE(lifetime.active == 0);
		REQUIRE(lifetime.state_valid);
		REQUIRE(lifetime.started == 1 + 2 * (lifetime.depth - 1));
		REQUIRE(lifetime.destroyed.size() == lifetime.started);
		for (idx_t sibling = 0; sibling < 2; sibling++) {
			for (idx_t i = 0; i < lifetime.depth - 1; i++) {
				REQUIRE(lifetime.destroyed[sibling * (lifetime.depth - 1) + i] == lifetime.depth - i);
			}
		}
		REQUIRE(lifetime.destroyed.back() == 1);
	}

	SECTION("Failed matches leave the vector ready for another execution") {
		lifetime.depth = 1025;
		lifetime.fail_at_leaf = true;
		MatchStack stack;
		REQUIRE_FALSE(stack.Execute({matcher, state}).IsSuccess());
		REQUIRE(lifetime.active == 0);
		REQUIRE(lifetime.state_valid);
		REQUIRE(lifetime.started == lifetime.depth);
		REQUIRE(lifetime.destroyed.size() == lifetime.depth);
		lifetime.fail_at_leaf = false;
		lifetime.started = 0;
		lifetime.destroyed.clear();
		REQUIRE(stack.Execute({matcher, state}).IsSuccess());
		REQUIRE(lifetime.active == 0);
		REQUIRE(lifetime.state_valid);
		REQUIRE(lifetime.started == lifetime.depth);
	}
}

class DerivedListTestMatcher final : public ListMatcher {
public:
	explicit DerivedListTestMatcher(MatchProcessLifetimeState &lifetime_p) : lifetime(lifetime_p) {
	}

	arena_ptr<MatchProcess> StartMatch(MatchState &state) const override {
		return state.Make<NestedTestMatchProcess>(*this, state, lifetime);
	}

private:
	MatchProcessLifetimeState &lifetime;
};

template <idx_t SIZE>
class ArenaNestedTestMatchProcess final : public NestedTestMatchProcess {
public:
	ArenaNestedTestMatchProcess(const Matcher &matcher, MatchState &state, MatchProcessLifetimeState &lifetime_p)
	    : NestedTestMatchProcess(matcher, state, lifetime_p), lifetime(lifetime_p) {
		lifetime.storage_valid &= reinterpret_cast<uintptr_t>(this) % alignof(ArenaNestedTestMatchProcess) == 0;
		lifetime.peak_arena_bytes = MaxValue(lifetime.peak_arena_bytes, state.context.process_allocator.SizeInBytes());
		if (lifetime.throw_in_constructor && lifetime.active == lifetime.depth) {
			throw InvalidInputException("Nested process constructor failure");
		}
		payload.fill(0xa5);
	}

	~ArenaNestedTestMatchProcess() override {
		for (auto byte : payload) {
			lifetime.storage_valid &= byte == 0xa5;
		}
	}

private:
	MatchProcessLifetimeState &lifetime;
	array<uint8_t, SIZE> payload;
};

class ArenaNestedTestMatcher final : public ListMatcher {
public:
	explicit ArenaNestedTestMatcher(MatchProcessLifetimeState &lifetime_p) : lifetime(lifetime_p) {
		SetName("nested result");
	}

	arena_ptr<MatchProcess> StartMatch(MatchState &state) const override {
		if (lifetime.active % 2) {
			return state.Make<ArenaNestedTestMatchProcess<9000>>(*this, state, lifetime);
		}
		return state.Make<ArenaNestedTestMatchProcess<32>>(*this, state, lifetime);
	}

private:
	MatchProcessLifetimeState &lifetime;
};

TEST_CASE("MatchState allocates processes through its shared context", "[api][grammar_extension]") {
	vector<MatcherToken> tokens;
	TokenIterator iterator(tokens);
	vector<MatcherSuggestion> suggestions;
	ParseResultAllocator parse_results;
	ArenaAllocator process_allocator(Allocator::DefaultAllocator());
	idx_t max_token_index = 0;
	MatchContext context(suggestions, parse_results, process_allocator, max_token_index);
	MatchState state(iterator, context);
	MatchState child_state(state);
	MatchProcessLifetimeState lifetime;
	lifetime.depth = 3;
	ArenaNestedTestMatcher matcher(lifetime);
	auto parent = state.Make<ArenaNestedTestMatchProcess<9000>>(matcher, state, lifetime);
	auto parent_bytes = process_allocator.SizeInBytes();
	REQUIRE(parent_bytes >= sizeof(ArenaNestedTestMatchProcess<9000>));
	REQUIRE(&child_state.context.process_allocator == &process_allocator);
	REQUIRE(matcher.MatchParseResult(child_state).IsSuccess());
	// the children were allocated from the shared arena while they were alive
	REQUIRE(lifetime.peak_arena_bytes >= parent_bytes + sizeof(ArenaNestedTestMatchProcess<9000>));
	// and released again when their frames were popped
	REQUIRE(process_allocator.SizeInBytes() == parent_bytes);
	REQUIRE(lifetime.active == 1);
	parent.reset();
	REQUIRE(lifetime.active == 0);
	REQUIRE(lifetime.storage_valid);
	REQUIRE(lifetime.state_valid);
	REQUIRE(lifetime.destroyed == vector<idx_t> {3, 2, 1});
}

TEST_CASE("Matcher driver preserves overrides on derived built-in matchers", "[api][grammar_extension]") {
	vector<MatcherToken> tokens;
	TokenIterator iterator(tokens);
	vector<MatcherSuggestion> suggestions;
	ParseResultAllocator allocator;
	idx_t max_token_index = 0;
	ArenaAllocator process_allocator(Allocator::DefaultAllocator());
	MatchContext context(suggestions, allocator, process_allocator, max_token_index);
	MatchState state(iterator, context);
	MatchProcessLifetimeState lifetime;
	lifetime.depth = 130;
	DerivedListTestMatcher matcher(lifetime);
	REQUIRE(matcher.MatchParseResult(state).IsSuccess());
	REQUIRE(lifetime.started == lifetime.depth);
	REQUIRE(lifetime.active == 0);
	REQUIRE(lifetime.destroyed.size() == lifetime.depth);
}

class DerivedChoiceTestMatcher final : public ChoiceMatcher {
public:
	DerivedChoiceTestMatcher() : child("ANSWER", KeywordInfo()) {
	}

	arena_ptr<MatchProcess> StartMatch(MatchState &state) const override {
		return state.Make<GrammarExtensionTestMatchProcess>(child, state);
	}

private:
	KeywordMatcher child;
};

TEST_CASE("FIRST sets do not assume built-in semantics for derived matchers", "[api][grammar_extension]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto grammar = CompiledGrammar::Get(*con.context);
	DerivedChoiceTestMatcher derived;
	ListMatcher list;
	list.SetStructural();
	list.matchers.push_back(derived);
	ComputeFirstSets(list, grammar->GetKeywordHelper().GetLiteralTable());

	vector<MatcherToken> tokens {MatcherToken("answer", 0, TokenType::IDENTIFIER)};
	TokenIterator iterator(tokens);
	vector<MatcherSuggestion> suggestions;
	ParseResultAllocator allocator;
	idx_t max_token_index = 0;
	ArenaAllocator process_allocator(Allocator::DefaultAllocator());
	MatchContext context(suggestions, allocator, process_allocator, max_token_index);
	MatchState state(iterator, context);
	REQUIRE(list.MatchParseResult(state).IsSuccess());
}

enum class ListChainTestVariant {
	OPTIONAL_TAILS,
	INNER_REQUIRED,
	OUTER_REQUIRED,
	EMPTY_INNER,
	UNNAMED_ATOM,
	NONCOLLAPSIBLE_INNER,
	NONCOLLAPSIBLE_MIDDLE,
	NONCOLLAPSIBLE_OUTER,
	SUPPRESSED_MIDDLE,
	MEMOIZED_MIDDLE
};

static vector<string> MatchListChainTest(bool structural, ListChainTestVariant variant, MatchMode mode,
                                         const string &text, bool cursor) {
	CompiledGrammarRule atom_rule("Atom", nullptr), inner_rule("Inner", nullptr), middle_rule("Middle", nullptr),
	    outer_rule("Outer", nullptr);
	KeywordMatcher a("A", KeywordInfo()), b("B", KeywordInfo()), c("C", KeywordInfo()), d("D", KeywordInfo());
	if (variant != ListChainTestVariant::UNNAMED_ATOM) {
		a.SetRule(atom_rule);
	}
	OptionalMatcher optional_b(b), optional_c(c), optional_d(d);
	MatcherAllocator matchers;
	auto &inner = matchers.Allocate(make_uniq<ListMatcher>()).Cast<ListMatcher>();
	auto &middle = matchers.Allocate(make_uniq<ListMatcher>()).Cast<ListMatcher>();
	auto &outer = matchers.Allocate(make_uniq<ListMatcher>()).Cast<ListMatcher>();
	inner.SetRule(inner_rule);
	middle.SetRule(middle_rule);
	outer.SetRule(outer_rule);
	auto set_flags = [&](ListMatcher &matcher, ListChainTestVariant noncollapsible) {
		if (variant != noncollapsible) {
			matcher.SetCollapsible();
		}
		if (structural) {
			matcher.SetStructural();
		}
	};
	set_flags(inner, ListChainTestVariant::NONCOLLAPSIBLE_INNER);
	set_flags(middle, ListChainTestVariant::NONCOLLAPSIBLE_MIDDLE);
	set_flags(outer, ListChainTestVariant::NONCOLLAPSIBLE_OUTER);
	if (variant != ListChainTestVariant::EMPTY_INNER) {
		inner.matchers.push_back(a);
		inner.matchers.push_back(variant == ListChainTestVariant::INNER_REQUIRED ? reference<Matcher>(b)
		                                                                         : reference<Matcher>(optional_b));
	}
	middle.matchers = {inner, optional_c};
	outer.matchers = {middle, variant == ListChainTestVariant::OUTER_REQUIRED ? reference<Matcher>(d)
	                                                                          : reference<Matcher>(optional_d)};
	middle.suppress_suggestions = variant == ListChainTestVariant::SUPPRESSED_MIDDLE;
	if (variant == ListChainTestVariant::MEMOIZED_MIDDLE) {
		middle.SetPackratMemoized();
	}
	auto grammar = CompiledGrammar::DefaultGrammar();
	ComputeFirstSets(outer, grammar->GetKeywordHelper().GetLiteralTable());
	matchers.InitializeListChains();
	vector<MatcherToken> tokens;
	idx_t offset = 0;
	for (auto &word : StringUtil::Split(text, ' ')) {
		if (!word.empty()) {
			tokens.emplace_back(word, offset, TokenType::KEYWORD);
		}
		offset += word.size() + 1;
	}
	tokens.emplace_back("", text.size(), cursor ? TokenType::END_OF_INPUT_AUTOCOMPLETE : TokenType::END_OF_INPUT);
	TokenIterator iterator(tokens);
	vector<MatcherSuggestion> suggestions;
	ParseResultAllocator allocator;
	ArenaAllocator processes(Allocator::DefaultAllocator()), cache_allocator(Allocator::DefaultAllocator());
	ParserPackratCache cache(cache_allocator);
	idx_t max_position = 0;
	MatchContext context(suggestions, allocator, processes, max_position, mode, IdentifierCaseMode::PRESERVE_CASE,
	                     &cache);
	vector<string> snapshot;
	for (idx_t attempt = 0; attempt < 2; attempt++) {
		suggestions.clear();
		MatchState state(iterator, context);
		auto result = outer.MatchParseResult(state);
		REQUIRE(processes.SizeInBytes() == 0);
		snapshot.push_back(
		    StringUtil::Format("%d:%llu:%llu", result.IsSuccess(), state.token_iterator.Position(), max_position));
		for (auto &suggestion : suggestions) {
			snapshot.push_back(suggestion.keyword.candidate);
		}
		vector<reference<ParseResult>> pending;
		if (result.HasParseResult()) {
			pending.push_back(*result.GetParseResult());
		}
		while (!pending.empty()) {
			auto &node = pending.back().get();
			pending.pop_back();
			auto location = node.GetLocation();
			snapshot.push_back(StringUtil::Format("%s:%s:%s:%d:%llu:%llu", ParseResultToString(node.type), node.Name(),
			                                      node.GetRule() ? node.GetRule()->name : string(), node.collapsed,
			                                      location.offset, location.length));
			if (node.type == ParseResultType::LIST) {
				for (auto &child : node.Cast<ListParseResult>().GetChildren()) {
					pending.push_back(child);
				}
			} else if (node.type == ParseResultType::OPTIONAL) {
				auto &optional = node.Cast<OptionalParseResult>();
				if (optional.HasResult()) {
					pending.push_back(optional.GetResult());
				}
			}
		}
	}
	return snapshot;
}

TEST_CASE("Leading list chains preserve results, failure state and suggestions", "[api][grammar_extension]") {
	auto variant = ListChainTestVariant::OPTIONAL_TAILS;
	SECTION("Optional tails") {
	}
	SECTION("A required inner tail can fail") {
		variant = ListChainTestVariant::INNER_REQUIRED;
	}
	SECTION("A required outer tail can fail") {
		variant = ListChainTestVariant::OUTER_REQUIRED;
	}
	SECTION("An empty inner list retains its own result") {
		variant = ListChainTestVariant::EMPTY_INNER;
	}
	SECTION("An atom without a rule cannot replace its list") {
		variant = ListChainTestVariant::UNNAMED_ATOM;
	}
	SECTION("A noncollapsible inner list retains its result") {
		variant = ListChainTestVariant::NONCOLLAPSIBLE_INNER;
	}
	SECTION("A noncollapsible middle list retains its result") {
		variant = ListChainTestVariant::NONCOLLAPSIBLE_MIDDLE;
	}
	SECTION("A noncollapsible outer list retains its result") {
		variant = ListChainTestVariant::NONCOLLAPSIBLE_OUTER;
	}
	SECTION("Suggestion suppression remains a boundary") {
		variant = ListChainTestVariant::SUPPRESSED_MIDDLE;
	}
	SECTION("Memoized children retain their cache boundary") {
		variant = ListChainTestVariant::MEMOIZED_MIDDLE;
	}
	for (auto mode : {MatchMode::BUILD_PARSE_RESULT, MatchMode::RECOGNIZE_ONLY}) {
		for (auto cursor : {false, true}) {
			for (auto &text : vector<string> {"", "A", "A B", "A C", "A D", "A B C D", "A X", "X"}) {
				CAPTURE(text);
				CAPTURE(cursor);
				auto expected = MatchListChainTest(false, variant, mode, text, cursor);
				auto actual = MatchListChainTest(true, variant, mode, text, cursor);
				REQUIRE(actual == expected);
			}
		}
	}
}

TEST_CASE("List chain preparation follows the finalized graph", "[api][grammar_extension]") {
	ListMatcher parent, child;
	parent.SetStructural();
	child.SetStructural();
	parent.matchers.push_back(child);
	REQUIRE_FALSE(parent.GetChainChild());
	parent.InitializeListChain();
	REQUIRE(parent.GetChainChild().get() == &child);

	SECTION("Changing a link replaces the prepared edge") {
		ListMatcher replacement;
		replacement.SetStructural();
		parent.matchers[0] = replacement;
		parent.InitializeListChain();
		REQUIRE(parent.GetChainChild().get() == &replacement);
	}
	SECTION("Removing the child clears the prepared edge") {
		parent.matchers.clear();
		parent.InitializeListChain();
		REQUIRE_FALSE(parent.GetChainChild());
	}
	SECTION("Parent suppression clears the prepared edge") {
		parent.suppress_suggestions = true;
		parent.InitializeListChain();
		REQUIRE_FALSE(parent.GetChainChild());
	}
	SECTION("Child suppression clears the prepared edge") {
		child.suppress_suggestions = true;
		parent.InitializeListChain();
		REQUIRE_FALSE(parent.GetChainChild());
	}
	SECTION("Child memoization clears the prepared edge") {
		child.SetPackratMemoized();
		parent.InitializeListChain();
		REQUIRE_FALSE(parent.GetChainChild());
	}
	SECTION("Custom implementations do not form a prepared edge") {
		MatchProcessLifetimeState lifetime;
		DerivedListTestMatcher custom(lifetime);
		parent.matchers[0] = custom;
		parent.InitializeListChain();
		REQUIRE_FALSE(parent.GetChainChild());
	}
}

TEST_CASE("Deep leading list chains retain heap-owned state through exceptions", "[api][grammar_extension]") {
	CompiledGrammarRule rule("Wrapper", nullptr);
	MatchProcessLifetimeState lifetime;
	lifetime.depth = 3;
	lifetime.create_result = true;
	NestedTestMatcher leaf(lifetime);
	vector<unique_ptr<ListMatcher>> lists;
	for (idx_t i = 0; i < 4096; i++) {
		auto list = make_uniq<ListMatcher>();
		list->SetStructural();
		list->SetCollapsible();
		list->SetRule(rule);
		list->matchers.push_back(lists.empty() ? reference<Matcher>(leaf) : reference<Matcher>(*lists.back()));
		lists.push_back(std::move(list));
	}
	for (auto &list : lists) {
		list->InitializeListChain();
	}
	SECTION("Success") {
	}
	SECTION("Exception in a custom descendant") {
		lifetime.throw_at_leaf = true;
	}
	vector<MatcherToken> tokens;
	TokenIterator iterator(tokens);
	vector<MatcherSuggestion> suggestions;
	ParseResultAllocator allocator;
	ArenaAllocator processes(Allocator::DefaultAllocator());
	idx_t max_position = 0;
	MatchContext context(suggestions, allocator, processes, max_position);
	MatchState state(iterator, context);
	if (lifetime.throw_at_leaf) {
		REQUIRE_THROWS_AS(lists.back()->MatchParseResult(state), InvalidInputException);
	} else {
		REQUIRE(lists.back()->MatchParseResult(state).IsSuccess());
	}
	REQUIRE(processes.SizeInBytes() == 0);
	REQUIRE(lifetime.active == 0);
	REQUIRE(lifetime.state_valid);
	REQUIRE(lifetime.destroyed == vector<idx_t> {3, 2, 1});
}

static vector<MatcherToken> TokenizeFirstSetTestInput(const CompiledGrammar &grammar, const string &sql) {
	vector<MatcherToken> tokens;
	ParserTokenizerBehavior behavior(sql, tokens);
	grammar.GetTokenizer().TokenizeInput(behavior);
	return tokens;
}

TEST_CASE("Tokenizer assigns FIRST set token classes", "[api][grammar_extension]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto grammar = CompiledGrammar::Get(*con.context);
	auto token_class = [&](const string &sql) {
		auto tokens = TokenizeFirstSetTestInput(*grammar, sql);
		REQUIRE(tokens.size() == 2);
		return tokens[0].token_class;
	};
	const uint8_t generic_operator = MatcherTokenClass::OPERATOR | MatcherTokenClass::GENERIC_OPERATOR;
	const uint8_t string_literal = MatcherTokenClass::STRING | MatcherTokenClass::WORD;

	REQUIRE(token_class("select") == MatcherTokenClass::WORD);
	REQUIRE(token_class("some_column") == MatcherTokenClass::WORD);
	REQUIRE(token_class("\"quoted name\"") == MatcherTokenClass::WORD);
	// string literals are accepted in identifier positions
	REQUIRE(token_class("'str'") == string_literal);
	REQUIRE(token_class("E'str'") == string_literal);
	REQUIRE(token_class("$$str$$") == string_literal);
	REQUIRE(token_class("42") == MatcherTokenClass::NUMBER);
	REQUIRE(token_class(".5") == MatcherTokenClass::NUMBER);
	REQUIRE(token_class("1e10") == MatcherTokenClass::NUMBER);
	REQUIRE(token_class("+") == generic_operator);
	REQUIRE(token_class("||") == generic_operator);
	for (auto punctuation : {"(", ")", "[", "]", "{", "}", ",", ":", "::", ":=", "?", "$", "#"}) {
		INFO(punctuation);
		REQUIRE((token_class(punctuation) & generic_operator) == 0);
	}
	// operators with their own precedence level are not generic operators
	REQUIRE(token_class("<=") == MatcherTokenClass::OPERATOR);
	REQUIRE(token_class("=") == MatcherTokenClass::OPERATOR);
	// terminators and the end of input are only matched as literals
	auto tokens = TokenizeFirstSetTestInput(*grammar, ";");
	REQUIRE(tokens.size() == 2);
	REQUIRE(tokens[0].type == TokenType::TERMINATOR);
	REQUIRE(tokens[0].token_class == 0);
	REQUIRE(tokens[1].type == TokenType::END_OF_INPUT);
	REQUIRE(tokens[1].token_class == 0);
	// re-typing a matched token does not change its class
	tokens = TokenizeFirstSetTestInput(*grammar, "'path'");
	TokenIterator iterator(tokens);
	iterator.Advance();
	iterator.SetPreviousTokenType(TokenType::TABLE_NAME);
	REQUIRE(tokens[0].token_class == string_literal);
}

//! Checks MightMatch against the expectation, and that a rejected matcher indeed fails to match
static void CheckFirstSet(const CompiledGrammar &grammar, const Matcher &matcher, const string &sql, bool expected) {
	INFO("Matcher " << matcher.ToString() << " on input \"" << sql << "\"");
	auto tokens = TokenizeFirstSetTestInput(grammar, sql);
	TokenIterator iterator(tokens);
	vector<MatcherSuggestion> suggestions;
	ParseResultAllocator allocator;
	idx_t max_token_index = 0;
	ArenaAllocator process_allocator(Allocator::DefaultAllocator());
	MatchContext context(suggestions, allocator, process_allocator, max_token_index);
	MatchState state(iterator, context);
	auto might_match = matcher.first_set.MightMatch(state);
	REQUIRE(might_match == expected);
	if (!might_match) {
		REQUIRE_FALSE(matcher.MatchParseResult(state).IsSuccess());
	}
}

TEST_CASE("FIRST sets only reject tokens a matcher cannot start with", "[api][grammar_extension]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto grammar = CompiledGrammar::Get(*con.context);
	auto &helper = grammar->GetKeywordHelper();

	// keyword matchers created by the factory are matched by literal id
	auto keyword_grammar = ParsedGrammar::Parse("Program <- 'SELECT'");
	auto keyword_rules = CompileTestProgramRule(keyword_grammar);
	MatcherAllocator matcher_allocator;
	MatcherFactory factory(matcher_allocator, keyword_grammar, keyword_rules, helper, {});
	auto &select = factory.CreateRootMatcher("Program").Cast<ListMatcher>().matchers[0].get();
	REQUIRE(select.Type() == MatcherType::KEYWORD);
	// other keyword matchers can have their own semantics
	KeywordMatcher custom_keyword("SELECT", KeywordInfo(), helper);
	IdentifierMatcher identifier(SuggestionState::SUGGEST_COLUMN_NAME, helper);
	NumberLiteralMatcher number;
	StringLiteralMatcher string_literal;
	OperatorMatcher generic_operator;
	OperatorMatcher any_operator(OperatorMatcherMode::ALL_OPERATORS);
	ArithmeticOperatorMatcher arithmetic_operator;
	GrammarExtensionTestMatcher custom;

	// SELECT / NUMBER
	ChoiceMatcher choice({select, number});
	// NUMBER? IDENTIFIER
	OptionalMatcher optional_number(number);
	ListMatcher optional_prefix({optional_number, identifier});
	// NUMBER?
	ListMatcher nullable_list({optional_number});
	// STRING*
	RepeatMatcher repeat(string_literal);
	// (SELECT / NUMBER) IDENTIFIER: the identifier is not part of the FIRST set
	ListMatcher sequence({choice, identifier});
	// custom matchers provide no information
	ListMatcher custom_list({custom, number});
	// a list that is not known to run the built-in list semantics
	ListMatcher unmarked_list({number});
	ListMatcher uncomputed_list({number});

	ChoiceMatcher root({choice, optional_prefix, nullable_list, repeat, sequence, custom_list, unmarked_list,
	                    identifier, string_literal, generic_operator, any_operator, arithmetic_operator,
	                    custom_keyword});
	for (auto matcher : vector<reference<Matcher>> {choice, optional_number, optional_prefix, nullable_list, repeat,
	                                                sequence, custom_list, root}) {
		matcher.get().SetStructural();
	}
	ComputeFirstSets(root, helper.GetLiteralTable());

	SECTION("Atomic matchers") {
		CheckFirstSet(*grammar, identifier, "some_column", true);
		CheckFirstSet(*grammar, identifier, "\"quoted name\"", true);
		CheckFirstSet(*grammar, identifier, "'path'", true);
		CheckFirstSet(*grammar, identifier, "42", false);
		CheckFirstSet(*grammar, identifier, "+", false);
		CheckFirstSet(*grammar, identifier, ";", false);
		CheckFirstSet(*grammar, identifier, "", false);

		CheckFirstSet(*grammar, string_literal, "'str'", true);
		CheckFirstSet(*grammar, string_literal, "E'str'", true);
		CheckFirstSet(*grammar, string_literal, "$$str$$", true);
		CheckFirstSet(*grammar, string_literal, "some_column", false);
		CheckFirstSet(*grammar, string_literal, "42", false);

		CheckFirstSet(*grammar, generic_operator, "+", true);
		CheckFirstSet(*grammar, generic_operator, "<=", false);
		CheckFirstSet(*grammar, generic_operator, "some_column", false);
		CheckFirstSet(*grammar, any_operator, "+", true);
		CheckFirstSet(*grammar, any_operator, "<=", true);
		CheckFirstSet(*grammar, any_operator, "42", false);
		for (auto punctuation : {"(", ")", "[", "]", "{", "}", ",", ":", "::", ":=", "?", "$", "#"}) {
			CheckFirstSet(*grammar, generic_operator, punctuation, false);
			CheckFirstSet(*grammar, any_operator, punctuation, false);
		}
		CheckFirstSet(*grammar, arithmetic_operator, "*", true);
		CheckFirstSet(*grammar, arithmetic_operator, "'str'", false);
	}
	SECTION("Choices merge the FIRST sets of their alternatives") {
		CheckFirstSet(*grammar, choice, "select", true);
		CheckFirstSet(*grammar, choice, "SELECT", true);
		CheckFirstSet(*grammar, choice, "42", true);
		CheckFirstSet(*grammar, choice, "from", false);
		CheckFirstSet(*grammar, choice, "some_column", false);
		CheckFirstSet(*grammar, choice, "'str'", false);
		CheckFirstSet(*grammar, choice, "+", false);
		CheckFirstSet(*grammar, choice, "", false);
	}
	SECTION("Lists look past nullable elements only") {
		CheckFirstSet(*grammar, optional_prefix, "42 some_column", true);
		CheckFirstSet(*grammar, optional_prefix, "some_column", true);
		CheckFirstSet(*grammar, optional_prefix, "+", false);
		CheckFirstSet(*grammar, sequence, "select some_column", true);
		CheckFirstSet(*grammar, sequence, "42 some_column", true);
		CheckFirstSet(*grammar, sequence, "some_column", false);
	}
	SECTION("Nullable matchers are never rejected") {
		REQUIRE(optional_number.first_set.nullable);
		REQUIRE(nullable_list.first_set.nullable);
		REQUIRE_FALSE(optional_prefix.first_set.nullable);
		CheckFirstSet(*grammar, optional_number, "+", true);
		CheckFirstSet(*grammar, nullable_list, "+", true);
		CheckFirstSet(*grammar, nullable_list, "", true);
	}
	SECTION("Repeats start like their element") {
		REQUIRE_FALSE(repeat.first_set.nullable);
		CheckFirstSet(*grammar, repeat, "'a' 'b'", true);
		CheckFirstSet(*grammar, repeat, "some_column", false);
	}
	SECTION("Matchers without FIRST set information are never rejected") {
		REQUIRE(custom_keyword.first_set.any);
		CheckFirstSet(*grammar, custom_keyword, "+", true);
		REQUIRE(custom_list.first_set.any);
		CheckFirstSet(*grammar, custom_list, "+", true);
		REQUIRE(unmarked_list.first_set.any);
		CheckFirstSet(*grammar, unmarked_list, "+", true);
		REQUIRE(root.first_set.any);
		CheckFirstSet(*grammar, root, ";", true);
		REQUIRE_FALSE(uncomputed_list.first_set.computed);
		CheckFirstSet(*grammar, uncomputed_list, "+", true);
	}
	SECTION("Nothing can start past the last token") {
		vector<MatcherToken> tokens;
		TokenIterator iterator(tokens);
		vector<MatcherSuggestion> suggestions;
		ParseResultAllocator allocator;
		idx_t max_token_index = 0;
		ArenaAllocator process_allocator(Allocator::DefaultAllocator());
		MatchContext context(suggestions, allocator, process_allocator, max_token_index);
		MatchState state(iterator, context);
		REQUIRE_FALSE(choice.first_set.MightMatch(state));
		REQUIRE(nullable_list.first_set.MightMatch(state));
	}
}

TEST_CASE("Matcher stack supports variable-sized aligned arena processes", "[api][grammar_extension]") {
	vector<MatcherToken> tokens;
	TokenIterator iterator(tokens);
	vector<MatcherSuggestion> suggestions;
	ParseResultAllocator allocator;
	idx_t max_token_index = 0;
	ArenaAllocator process_allocator(Allocator::DefaultAllocator());
	MatchContext context(suggestions, allocator, process_allocator, max_token_index);
	MatchState state(iterator, context);
	MatchProcessLifetimeState lifetime;
	ArenaNestedTestMatcher matcher(lifetime);

	SECTION("Repeated executions preserve process storage and destruction order") {
		MatchStack stack;
		for (idx_t depth :
		     {idx_t(1), idx_t(64), idx_t(65), idx_t(128), idx_t(129), idx_t(130), idx_t(33), idx_t(130)}) {
			process_allocator.Reset();
			lifetime.depth = depth;
			lifetime.started = 0;
			lifetime.destroyed.clear();
			REQUIRE(stack.Execute({matcher, state}).IsSuccess());
			REQUIRE(lifetime.started == depth);
			REQUIRE(lifetime.active == 0);
			REQUIRE(lifetime.storage_valid);
			REQUIRE(lifetime.state_valid);
			REQUIRE(lifetime.destroyed.size() == depth);
			for (idx_t i = 0; i < depth; i++) {
				REQUIRE(lifetime.destroyed[i] == depth - i);
			}
		}
	}

	SECTION("Resume exceptions destroy arena children before their parents") {
		lifetime.throw_at_leaf = true;
	}
	SECTION("Constructor exceptions destroy constructed members and parent processes") {
		lifetime.throw_in_constructor = true;
	}
	SECTION("Siblings preserve process storage while their parent stays alive") {
		lifetime.depth = 65;
		lifetime.root_children = 2;
		MatchStack stack;
		REQUIRE(stack.Execute({matcher, state}).IsSuccess());
		REQUIRE(lifetime.active == 0);
		REQUIRE(lifetime.storage_valid);
		REQUIRE(lifetime.state_valid);
		REQUIRE(lifetime.started == 1 + 2 * (lifetime.depth - 1));
	}
	if (lifetime.throw_at_leaf || lifetime.throw_in_constructor) {
		lifetime.depth = 130;
		{
			MatchStack stack;
			REQUIRE_THROWS_AS(stack.Execute({matcher, state}), InvalidInputException);
		}
		REQUIRE(lifetime.active == 0);
		REQUIRE(lifetime.started == lifetime.depth);
		REQUIRE(lifetime.storage_valid);
		REQUIRE(lifetime.state_valid);
		REQUIRE(lifetime.destroyed.size() == lifetime.depth);
		for (idx_t i = 0; i < lifetime.depth; i++) {
			REQUIRE(lifetime.destroyed[i] == lifetime.depth - i);
		}
	}
}

TEST_CASE("Packrat results outlive reset process arenas", "[api][grammar_extension]") {
	vector<MatcherToken> tokens;
	TokenIterator iterator(tokens);
	vector<MatcherSuggestion> suggestions;
	ParseResultAllocator parse_results;
	ArenaAllocator packrat_allocator(Allocator::DefaultAllocator());
	ParserPackratCache cache(packrat_allocator);
	idx_t max_token_index = 0;
	ArenaAllocator process_allocator(Allocator::DefaultAllocator());
	MatchContext context(suggestions, parse_results, process_allocator, max_token_index);
	context.packrat_cache = &cache;
	MatchState state(iterator, context);
	MatchProcessLifetimeState lifetime;
	lifetime.depth = 3;
	lifetime.create_result = true;
	MatcherAllocator matchers;
	auto &matcher = matchers.Allocate(make_uniq<ArenaNestedTestMatcher>(lifetime));
	matcher.SetPackratMemoized();
	MatchStack stack;

	SECTION("Cached successes retain separately allocated parse results") {
	}
	SECTION("Cached failures need no new process") {
		lifetime.fail_at_leaf = true;
	}
	auto first = stack.Execute({matcher, state});
	REQUIRE(first.IsSuccess() == !lifetime.fail_at_leaf);
	REQUIRE(lifetime.started == lifetime.depth);
	REQUIRE(lifetime.active == 0);

	ArenaNestedTestMatcher overwriter(lifetime);
	lifetime.depth = 130;
	lifetime.create_result = false;
	process_allocator.Reset();
	stack.Execute({overwriter, state});
	process_allocator.Reset();
	lifetime.started = 0;
	auto cached = stack.Execute({matcher, state});
	REQUIRE(cached.IsSuccess() == first.IsSuccess());
	REQUIRE(cached.GetParseResult().get() == first.GetParseResult().get());
	REQUIRE(lifetime.started == 0);
	REQUIRE(lifetime.active == 0);
	REQUIRE(lifetime.storage_valid);
	REQUIRE(lifetime.state_valid);
	if (cached.IsSuccess()) {
		REQUIRE(cached.HasParseResult());
		REQUIRE(cached.GetParseResult()->Name() == "nested result");
	}
}

class ObservedNumberLiteralMatcher : public NumberLiteralMatcher {
public:
	explicit ObservedNumberLiteralMatcher(idx_t &attempts) : attempts(attempts) {
	}

	MatcherResult MatchAtomic(MatchState &state) const override {
		attempts++;
		return NumberLiteralMatcher::MatchAtomic(state);
	}

private:
	idx_t &attempts;
};

TEST_CASE("Choice rejection preserves atomic callbacks and alternative indices", "[api][grammar_extension]") {
	idx_t attempts = 0;
	ObservedNumberLiteralMatcher number(attempts);
	ListMatcher rejected({number});
	rejected.SetStructural();
	KeywordMatcher keyword("SELECT", KeywordInfo());
	ChoiceMatcher choice({number, rejected, keyword});
	choice.SetStructural();
	auto grammar = CompiledGrammar::DefaultGrammar();
	ComputeFirstSets(choice, grammar->GetKeywordHelper().GetLiteralTable());
	vector<MatcherToken> tokens {MatcherToken("select", 0, TokenType::KEYWORD)};
	TokenIterator iterator(tokens);
	vector<MatcherSuggestion> suggestions;
	ParseResultAllocator results;
	ArenaAllocator processes(Allocator::DefaultAllocator());
	idx_t max_position = 0;
	MatchMode mode = MatchMode::BUILD_PARSE_RESULT;
	SECTION("Build results") {
	}
	SECTION("Recognition retains all atomic attempts") {
		mode = MatchMode::RECOGNIZE_ONLY;
	}
	MatchContext context(suggestions, results, processes, max_position, mode);
	MatchState state(iterator, context);
	REQUIRE_FALSE(number.first_set.MightMatch(state));
	REQUIRE_FALSE(rejected.first_set.MightMatch(state));
	auto result = choice.MatchParseResult(state);
	REQUIRE(result.IsSuccess());
	REQUIRE(state.token_iterator.Position() == 1);
	REQUIRE(max_position == 1);
	auto build_results = mode == MatchMode::BUILD_PARSE_RESULT;
	REQUIRE(attempts == (build_results ? 1 : 2));
	REQUIRE(result.HasParseResult() == build_results);
	if (build_results) {
		auto &parsed = result.GetParseResult()->Cast<ChoiceParseResult>();
		REQUIRE(parsed.GetResult().type == ParseResultType::KEYWORD);
		REQUIRE(parsed.ToString().find("CHOICE (idx: 2)") != string::npos);
	}
}

TEST_CASE("Compiled grammar processes use arena ownership", "[api][grammar_extension]") {
	vector<MatcherToken> tokens;
	TokenIterator iterator(tokens);
	vector<MatcherSuggestion> suggestions;
	ParseResultAllocator parse_results;
	idx_t max_token_index = 0;
	ArenaAllocator process_allocator(Allocator::DefaultAllocator());
	MatchContext context(suggestions, parse_results, process_allocator, max_token_index);
	MatchState state(iterator, context);
	auto grammar = CompiledGrammar::Create();
	auto process = grammar->TopLevelStatementMatcher().StartMatch(state);
	REQUIRE(process);
	REQUIRE(process_allocator.SizeInBytes() > 0);
	process.reset();
	process_allocator.Reset();
	REQUIRE(process_allocator.SizeInBytes() == 0);
}

TEST_CASE("Grammar changes expose structured metadata", "[api][grammar_extension]") {
	auto add_rule = GrammarChange::AddRule("TrackedRule <- 'tracked'");
	REQUIRE(add_rule.Type() == GrammarChangeType::ADD_RULE);
	REQUIRE(add_rule.RuleName() == "TrackedRule");
	REQUIRE(add_rule.Definition() == "TrackedRule <- 'tracked'");

	auto add_choice = GrammarChange::AddChoice("TrackedRule", "'choice'");
	REQUIRE(add_choice.Type() == GrammarChangeType::ADD_CHOICE);
	REQUIRE(add_choice.RuleName() == "TrackedRule");
	REQUIRE(add_choice.Definition() == "'choice'");
}

TEST_CASE("Grammar choices support cursor placement", "[api][grammar_extension]") {
	auto grammar = ParsedGrammar::Parse("CursorRule <- 'first' / 'last'");
	grammar.AddChoice("CursorRule", "'second'", [](const PEGExpression &expression) {
		return expression.type == PEGExpression::Type::LITERAL && expression.text.GetString() == "first";
	});
	grammar.PrependChoice("CursorRule", "'third'", [](const PEGExpression &expression) {
		return expression.type == PEGExpression::Type::LITERAL && expression.text.GetString() == "last";
	});

	vector<string> choices;
	auto rule = grammar.GetRule("CursorRule");
	REQUIRE(rule);
	for (auto &expression : rule->recipe.expression.children) {
		if (expression.type == PEGExpression::Type::LITERAL) {
			choices.push_back(expression.text.GetString());
		}
	}
	REQUIRE(choices == vector<string> {"first", "second", "third", "last"});
}

TEST_CASE("Grammar choices can be replaced", "[api][grammar_extension]") {
	auto grammar = ParsedGrammar::Parse("CursorRule <- 'first' / 'second' / 'last'");
	grammar.ReplaceChoice("CursorRule", "'replacement'", [](const PEGExpression &expression) {
		return expression.type == PEGExpression::Type::LITERAL && expression.text.GetString() == "second";
	});

	auto rule = grammar.GetRule("CursorRule");
	REQUIRE(rule);
	REQUIRE(rule->recipe.expression.type == PEGExpression::Type::CHOICE);
	REQUIRE(rule->recipe.expression.children.size() == 3);
	REQUIRE(rule->recipe.expression.children[0].text.GetString() == "first");
	REQUIRE(rule->recipe.expression.children[1].text.GetString() == "replacement");
	REQUIRE(rule->recipe.expression.children[2].text.GetString() == "last");

	REQUIRE_THROWS(grammar.ReplaceChoice("CursorRule", "'replacement'", [](const PEGExpression &expression) {
		return expression.type == PEGExpression::Type::LITERAL && expression.text.GetString() == "missing";
	}));

	auto non_choice_grammar = ParsedGrammar::Parse("NonChoiceRule <- 'only'");
	REQUIRE_THROWS(
	    non_choice_grammar.ReplaceChoice("NonChoiceRule", "'replacement'", [](const PEGExpression &expression) {
		    return expression.type == PEGExpression::Type::LITERAL;
	    }));
}

TEST_CASE("Grammar choices can be removed", "[api][grammar_extension]") {
	auto grammar = ParsedGrammar::Parse("CursorRule <- 'first' / 'second' / 'last'");
	grammar.RemoveChoice("CursorRule", [](const PEGExpression &expression) {
		return expression.type == PEGExpression::Type::LITERAL && expression.text.GetString() == "second";
	});

	auto rule = grammar.GetRule("CursorRule");
	REQUIRE(rule);
	REQUIRE(rule->recipe.expression.type == PEGExpression::Type::CHOICE);
	REQUIRE(rule->recipe.expression.children.size() == 2);
	REQUIRE(rule->recipe.expression.children[0].text.GetString() == "first");
	REQUIRE(rule->recipe.expression.children[1].text.GetString() == "last");

	REQUIRE_THROWS(grammar.RemoveChoice("CursorRule", [](const PEGExpression &expression) {
		return expression.type == PEGExpression::Type::LITERAL && expression.text.GetString() == "missing";
	}));

	grammar.RemoveChoice("CursorRule", [](const PEGExpression &expression) {
		return expression.type == PEGExpression::Type::LITERAL && expression.text.GetString() == "first";
	});
	REQUIRE(rule->recipe.expression.type == PEGExpression::Type::LITERAL);
	REQUIRE(rule->recipe.expression.text.GetString() == "last");
}

class OverrideDefaultTerminalRule final : public GrammarExtension {
public:
	OverrideDefaultTerminalRule()
	    : GrammarExtension("default_terminal_rule", "Add a terminal rule override for identifier") {
	}

	vector<GrammarChange> GetChanges() const override {
		vector<GrammarChange> changes;
		changes.push_back(GrammarChange::AddTerminalRuleOverride("identifier", [](const PEGKeywordHelper &) {
			return make_uniq<KeywordMatcher>("replacement", KeywordInfo(0, ' '));
		}));
		return changes;
	}
};

TEST_CASE("Default terminal rule overrides are registered before additions", "[api][grammar_extension]") {
	DuckDB db(nullptr);
	Connection con(db);
	GrammarExtension::Register(*db.instance, make_shared_ptr<OverrideDefaultTerminalRule>());
	REQUIRE_FAIL(con.Query("SET active_grammar_extensions = ['default_terminal_rule']"));
}

TEST_CASE("The parser cache only holds the base grammar", "[api][grammar_extension]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(*con.Query("SELECT 1"));
	auto base_grammar = CompiledGrammar::Get(*con.context);
	REQUIRE(base_grammar == CompiledGrammar::Get(*con.context));

	RegisterGrammarExtensionTestSyntax(*db.instance);
	REQUIRE(base_grammar == CompiledGrammar::Get(*con.context));
	ActivateGrammarExtensionTestSyntax(con);
	CheckGrammarExtensionTestSyntax(con);
	auto extension_grammar = CompiledGrammar::Get(*con.context);
	REQUIRE(extension_grammar == CompiledGrammar::Get(*con.context));
}

TEST_CASE("Active grammar extensions are cached on their connection", "[api][grammar_extension]") {
	DuckDB db(nullptr);
	RegisterGrammarExtensionTestSyntax(*db.instance);
	Connection enabled(db);
	Connection disabled(db);

	auto base_grammar = CompiledGrammar::Get(*disabled.context);
	ActivateGrammarExtensionTestSyntax(enabled);
	CheckGrammarExtensionTestSyntax(enabled);
	REQUIRE_FAIL(disabled.Query("ANSWER"));
	CheckGrammarExtensionTestSyntax(enabled);
	ActiveGrammarExtensionsSetting::SetLocal(*enabled.context, Value::LIST(LogicalType::VARCHAR, vector<Value> {}));
	REQUIRE_FAIL(enabled.Query("ANSWER"));
	REQUIRE(base_grammar == CompiledGrammar::Get(*enabled.context));
}

TEST_CASE("Parser options retain their compiled grammar", "[api][grammar_extension]") {
	DuckDB db(nullptr);
	RegisterGrammarExtensionTestSyntax(*db.instance);
	Connection con(db);
	ActivateGrammarExtensionTestSyntax(con);

	Parser context_parser(*con.context);
	REQUIRE_NOTHROW(context_parser.ParseQuery("ANSWER"));
	REQUIRE(context_parser.statements.size() == 1);
	auto &context_select = context_parser.statements[0]->Cast<SelectStatement>().node->Cast<SelectNode>();
	REQUIRE(context_select.select_list[0]->GetExpressionClass() == ExpressionClass::CONSTANT);

	auto options = ParserOptions::Builtin();
	options.compiled_grammar = CompiledGrammar::Get(*con.context);
	REQUIRE(options.compiled_grammar == CompiledGrammar::Get(*con.context));
	options.extensions = nullptr;

	Parser parser(std::move(options));
	REQUIRE_NOTHROW(parser.ParseQuery("ANSWER"));
	REQUIRE(parser.statements.size() == 1);

	// Without the extension, a bare ANSWER is not a statement.
	auto base_parser = Parser::GetBuiltinParser();
	REQUIRE_THROWS(base_parser.ParseQuery("ANSWER"));
}

TEST_CASE("Runtime grammar rules get IDs past the built-in range", "[api][grammar_extension]") {
	DuckDB db(nullptr);
	RegisterGrammarExtensionTestSyntax(*db.instance);
	Connection con(db);
	ActivateGrammarExtensionTestSyntax(con);
	auto grammar = CompiledGrammar::Get(*con.context);

	// a built-in rule keeps its generated ID, also when an extension modifies it
	auto select_atom = grammar->GetRule("SelectAtom");
	REQUIRE(select_atom);
	REQUIRE(select_atom->rule_id == PEGRuleId::SelectAtom);
	REQUIRE(grammar->GetRule(PEGRuleId::SelectAtom).get() == select_atom.get());
	REQUIRE(string(PEGRuleIdToString(PEGRuleId::SelectAtom)) == "SelectAtom");
	REQUIRE(PEGRuleIdFromString("selectatom") == PEGRuleId::SelectAtom);

	// rules added at runtime get distinct dynamic IDs that resolve back to the rule
	auto value_rule = grammar->GetRule("GrammarExtensionTestValue");
	auto atom_rule = grammar->GetRule("GrammarExtensionTestAtom");
	REQUIRE(value_rule);
	REQUIRE(atom_rule);
	REQUIRE(value_rule->rule_id >= PEGRuleId::BUILTIN_COUNT);
	REQUIRE(atom_rule->rule_id >= PEGRuleId::BUILTIN_COUNT);
	REQUIRE(value_rule->rule_id != atom_rule->rule_id);
	// runtime IDs follow the rule names, independent of the rule map's iteration order
	REQUIRE(value_rule->rule_id > atom_rule->rule_id);
	auto recompiled = CompiledGrammar::Create(*con.context, {"extension_test_value", "extension_test_atom"});
	REQUIRE(recompiled->GetRule("GrammarExtensionTestValue")->rule_id == value_rule->rule_id);
	REQUIRE(recompiled->GetRule("GrammarExtensionTestAtom")->rule_id == atom_rule->rule_id);
	// parameterized rules are compiled like any other rule
	REQUIRE(grammar->GetRule(PEGRuleId::List));
	REQUIRE(grammar->GetRule(value_rule->rule_id).get() == value_rule.get());
	REQUIRE(grammar->GetRule(atom_rule->rule_id).get() == atom_rule.get());
	REQUIRE(PEGRuleIdToString(value_rule->rule_id) == nullptr);
	REQUIRE(PEGRuleIdFromString("GrammarExtensionTestValue") == PEGRuleId::INVALID);

	// dynamic IDs are only meaningful within the grammar that assigned them
	auto base_grammar = CompiledGrammar::DefaultGrammar();
	REQUIRE(!base_grammar->GetRule("GrammarExtensionTestValue"));
	auto base_rule = base_grammar->GetRule(value_rule->rule_id);
	REQUIRE((!base_rule || base_rule->name != value_rule->name));
	REQUIRE(!base_grammar->GetRule(PEGRuleId::INVALID));
	REQUIRE(!base_grammar->GetRule(static_cast<PEGRuleId>(1u << 30)));
	REQUIRE(base_grammar->GetRule(PEGRuleId::SelectAtom));
}

class AddInvalidGrammarExtensionTestRule final : public GrammarExtension {
public:
	AddInvalidGrammarExtensionTestRule() : GrammarExtension("invalid_grammar_extension", "Invalid grammar extension") {
	}

	vector<GrammarChange> GetChanges() const override {
		vector<GrammarChange> changes;
		changes.push_back(GrammarChange::AddRule("GrammarExtensionInvalid <- GrammarExtensionMissingRule"));
		return changes;
	}
};

TEST_CASE("Invalid Grammar extensions fail grammar compilation", "[api][grammar_extension]") {
	DuckDB db(nullptr);
	Connection con(db);
	RegisterGrammarExtensionTestSyntax(*db.instance);
	GrammarExtension::Register(*db.instance, make_shared_ptr<AddInvalidGrammarExtensionTestRule>());
	ActivateGrammarExtensionTestSyntax(con);
	auto result = con.Query("SET active_grammar_extensions = ['invalid_grammar_extension']");
	REQUIRE_FAIL(result);
	CheckGrammarExtensionTestSyntax(con);
	auto setting = con.Query("SELECT current_setting('active_grammar_extensions')")->Collection().GetValue(0, 0);
	REQUIRE(ListValue::GetChildren(setting).size() == 2);
}

TEST_CASE("Grammar extensions that fail to compile do not evict cached grammars", "[api][grammar_extension]") {
	DuckDB db(nullptr);
	std::atomic<idx_t> compilations(0);
	for (idx_t i = 0; i < ParserCache::MAX_CACHED_EXTENSION_MATCHERS; i++) {
		GrammarExtension::Register(
		    *db.instance, make_shared_ptr<CountingSevenGrammarExtension>("counted_" + to_string(i), compilations));
	}
	GrammarExtension::Register(*db.instance, make_shared_ptr<AddInvalidGrammarExtensionTestRule>());
	for (idx_t i = 0; i < ParserCache::MAX_CACHED_EXTENSION_MATCHERS; i++) {
		Connection con(db);
		REQUIRE_NO_FAIL(*con.Query("SET active_grammar_extensions = ['counted_" + to_string(i) + "']"));
	}
	REQUIRE(compilations == ParserCache::MAX_CACHED_EXTENSION_MATCHERS);

	// the cache is full, but the least recently used grammar stays cached when compiling a new one fails
	Connection invalid_con(db);
	REQUIRE_FAIL(invalid_con.Query("SET active_grammar_extensions = ['invalid_grammar_extension']"));
	Connection con(db);
	REQUIRE_NO_FAIL(*con.Query("SET active_grammar_extensions = ['counted_0']"));
	RequireSeven(con);
	REQUIRE(compilations == ParserCache::MAX_CACHED_EXTENSION_MATCHERS);
}

class SuppressSuggestionsTestStatement final : public GrammarExtension {
public:
	explicit SuppressSuggestionsTestStatement(bool suppress_suggestions)
	    : GrammarExtension(suppress_suggestions ? "suppressed_test_statement" : "suggested_test_statement",
	                       "Grammar extension test statement"),
	      suppress_suggestions(suppress_suggestions) {
	}

	vector<GrammarChange> GetChanges() const override {
		vector<GrammarChange> changes;
		changes.push_back(GrammarChange::AddRule("GrammarExtensionTestStatement <- 'GRAMMAREXTENSIONTESTKEYWORD'",
		                                         nullptr, suppress_suggestions));
		changes.push_back(GrammarChange::AddChoice("Statement", "GrammarExtensionTestStatement"));
		return changes;
	}

private:
	bool suppress_suggestions;
};

static vector<string> GetStatementSuggestions(ClientContext &context) {
	auto compiled_grammar = CompiledGrammar::Get(context);
	vector<MatcherToken> tokens {MatcherToken("", 0, TokenType::END_OF_INPUT_AUTOCOMPLETE)};
	TokenIterator token_iterator(tokens);
	vector<MatcherSuggestion> suggestions;
	ParseResultAllocator parse_allocator;
	idx_t max_token_index = 0;
	ArenaAllocator process_allocator(Allocator::DefaultAllocator());
	MatchContext match_context(suggestions, parse_allocator, process_allocator, max_token_index,
	                           MatchMode::RECOGNIZE_ONLY, IdentifierCaseMode::PRESERVE_CASE);
	MatchState state(token_iterator, match_context);
	compiled_grammar->ProgramMatcher().MatchParseResult(state);
	vector<string> result;
	for (auto &suggestion : suggestions) {
		result.push_back(StringUtil::Upper(suggestion.keyword.candidate));
	}
	return result;
}

static bool HasSuggestion(const vector<string> &suggestions, const string &keyword) {
	return std::find(suggestions.begin(), suggestions.end(), keyword) != suggestions.end();
}

TEST_CASE("Grammar extension rules can suppress autocomplete suggestions", "[api][grammar_extension]") {
	DuckDB db(nullptr);
	GrammarExtension::Register(*db.instance, make_shared_ptr<SuppressSuggestionsTestStatement>(false));
	GrammarExtension::Register(*db.instance, make_shared_ptr<SuppressSuggestionsTestStatement>(true));
	Connection con(db);

	REQUIRE_NO_FAIL(*con.Query("SET active_grammar_extensions = ['suggested_test_statement']"));
	auto suggested = GetStatementSuggestions(*con.context);
	REQUIRE(HasSuggestion(suggested, "SELECT"));
	REQUIRE(HasSuggestion(suggested, "GRAMMAREXTENSIONTESTKEYWORD"));

	REQUIRE_NO_FAIL(*con.Query("SET active_grammar_extensions = ['suppressed_test_statement']"));
	auto suppressed = GetStatementSuggestions(*con.context);
	REQUIRE(HasSuggestion(suppressed, "SELECT"));
	REQUIRE_FALSE(HasSuggestion(suppressed, "GRAMMAREXTENSIONTESTKEYWORD"));
}

TEST_CASE("Parse result source positions retain independent optional full-width values", "[api][grammar_extension]") {
	vector<optional_idx> positions {optional_idx(),
	                                idx_t(0),
	                                idx_t(17),
	                                idx_t(QueryLocation::INVALID_OFFSET) - 1,
	                                idx_t(QueryLocation::INVALID_OFFSET),
	                                NumericLimits<idx_t>::Maximum()};
	for (auto offset : positions) {
		for (auto length : positions) {
			ParseResult result(ParseResultType::INVALID, offset, length);
			REQUIRE(result.GetOffset() == offset);
			REQUIRE(result.GetLength() == length);
		}
	}
}

TEST_CASE("Parse result source extents preserve unknown and out-of-range positions", "[api][grammar_extension]") {
	ParseResult child(ParseResultType::INVALID, 20, 5);

	SECTION("Enclosing a child gives an unknown extent a known length") {
		ParseResult parent(ParseResultType::INVALID, 10);
		REQUIRE_FALSE(parent.GetLength().IsValid());
		REQUIRE(parent.GetLocation() == QueryLocation(10, 0));
		parent.EncloseChild(child);
		REQUIRE(parent.GetOffset() == optional_idx(10));
		REQUIRE(parent.GetLength() == optional_idx(15));
		REQUIRE(parent.GetLocation() == QueryLocation(10, 15));
	}
	SECTION("An unknown start is not inferred from a child") {
		ParseResult parent(ParseResultType::INVALID, optional_idx());
		parent.EncloseChild(child);
		REQUIRE_FALSE(parent.GetOffset().IsValid());
		REQUIRE_FALSE(parent.GetLength().IsValid());
		REQUIRE_FALSE(parent.GetLocation().IsValid());
	}
	SECTION("An unrepresentable start retains its full-width value") {
		auto offset = NumericLimits<idx_t>::Maximum();
		ParseResult parent(ParseResultType::INVALID, offset);
		parent.EncloseChild(child);
		REQUIRE(parent.GetOffset() == optional_idx(offset));
		REQUIRE(parent.GetLength() == optional_idx(5));
		REQUIRE_FALSE(parent.GetLocation().IsValid());
	}
	SECTION("An unrepresentable extent can be replaced by an enclosed child") {
		ParseResult parent(ParseResultType::INVALID, 10, NumericLimits<idx_t>::Maximum());
		REQUIRE_FALSE(parent.GetLocation().IsValid());
		parent.EncloseChild(child);
		REQUIRE(parent.GetOffset() == optional_idx(10));
		REQUIRE(parent.GetLength() == optional_idx(5));
		REQUIRE(parent.GetLocation() == QueryLocation(10, 5));
	}
}

class DerivedOptionalTailTestMatcher final : public OptionalMatcher {
public:
	explicit DerivedOptionalTailTestMatcher(Matcher &nominal, idx_t &calls_p)
	    : OptionalMatcher(nominal), child("TAIL", KeywordInfo()), calls(calls_p) {
	}
	arena_ptr<MatchProcess> StartMatch(MatchState &state) const override {
		calls++;
		return state.Make<GrammarExtensionTestMatchProcess>(child, state);
	}

private:
	KeywordMatcher child;
	idx_t &calls;
};

enum class EmptyTailTestVariant {
	ORDINARY,
	NAMED_OPTIONALS,
	NONCOLLAPSIBLE,
	UNNAMED_FIRST,
	EMPTY_FIRST,
	NULLABLE_TAIL,
	REQUIRED_TAIL,
	DERIVED_TAIL,
	LEADING_CHAIN
};

static vector<string> MatchEmptyTailTest(bool structural, EmptyTailTestVariant variant, MatchMode mode,
                                         const string &text, bool cursor) {
	auto grammar = CompiledGrammar::DefaultGrammar();
	CompiledGrammarRule atom_rule("Atom", nullptr), list_rule("List", nullptr), optional_rule("Optional", nullptr),
	    outer_rule("Outer", nullptr);
	KeywordMatcher atom("ANSWER", KeywordInfo());
	NumberLiteralMatcher number;
	OptionalMatcher first_optional(number), tail(number), last(number), nested(tail);
	idx_t override_calls = 0;
	DerivedOptionalTailTestMatcher derived(number, override_calls);
	if (variant != EmptyTailTestVariant::UNNAMED_FIRST) {
		atom.SetRule(atom_rule);
	}
	first_optional.SetRule(atom_rule);
	if (variant == EmptyTailTestVariant::NAMED_OPTIONALS) {
		tail.SetRule(optional_rule);
		last.SetRule(optional_rule);
	}
	if (structural) {
		first_optional.SetStructural();
		tail.SetStructural();
		last.SetStructural();
		nested.SetStructural();
	}
	ListMatcher list;
	list.SetRule(list_rule);
	if (variant != EmptyTailTestVariant::NONCOLLAPSIBLE) {
		list.SetCollapsible();
	}
	list.matchers.push_back(variant == EmptyTailTestVariant::EMPTY_FIRST ? reference<Matcher>(first_optional)
	                                                                     : reference<Matcher>(atom));
	if (variant == EmptyTailTestVariant::NULLABLE_TAIL) {
		list.matchers.push_back(nested);
	} else if (variant == EmptyTailTestVariant::REQUIRED_TAIL) {
		list.matchers.push_back(number);
	} else if (variant == EmptyTailTestVariant::DERIVED_TAIL) {
		list.matchers.push_back(derived);
	} else {
		list.matchers.push_back(tail);
	}
	list.matchers.push_back(last);
	ListMatcher outer({list, last});
	outer.SetRule(outer_rule);
	outer.SetCollapsible();
	if (structural) {
		list.SetStructural();
		outer.SetStructural();
	}
	const Matcher &root = variant == EmptyTailTestVariant::LEADING_CHAIN ? static_cast<const Matcher &>(outer) : list;
	ComputeFirstSets(outer, grammar->GetKeywordHelper().GetLiteralTable());
	list.InitializeListChain();
	outer.InitializeListChain();
	auto tokens = TokenizeFirstSetTestInput(*grammar, text);
	REQUIRE(tokens.back().type == TokenType::END_OF_INPUT);
	if (cursor) {
		tokens.back().type = TokenType::END_OF_INPUT_AUTOCOMPLETE;
	}
	TokenIterator iterator(tokens);
	vector<MatcherSuggestion> suggestions;
	ParseResultAllocator allocator;
	ArenaAllocator processes(Allocator::DefaultAllocator());
	idx_t max_position = 0;
	MatchContext context(suggestions, allocator, processes, max_position, mode);
	MatchState state(iterator, context);
	auto result = root.MatchParseResult(state);
	REQUIRE(processes.SizeInBytes() == 0);
	vector<string> snapshot {StringUtil::Format("%d:%llu:%llu:%llu", result.IsSuccess(),
	                                            state.token_iterator.Position(), max_position, override_calls)};
	for (auto &suggestion : suggestions) {
		snapshot.push_back(suggestion.keyword.candidate);
	}
	vector<reference<ParseResult>> pending;
	if (result.HasParseResult()) {
		pending.push_back(*result.GetParseResult());
	}
	while (!pending.empty()) {
		auto &node = pending.back().get();
		pending.pop_back();
		auto location = node.GetLocation();
		snapshot.push_back(StringUtil::Format("%s:%s:%s:%d:%llu:%llu", ParseResultToString(node.type), node.Name(),
		                                      node.GetRule() ? node.GetRule()->name : string(), node.collapsed,
		                                      location.offset, location.length));
		if (node.type == ParseResultType::LIST) {
			for (auto &child : node.Cast<ListParseResult>().GetChildren()) {
				pending.push_back(child);
			}
		} else if (node.type == ParseResultType::OPTIONAL) {
			auto &optional = node.Cast<OptionalParseResult>();
			if (optional.HasResult()) {
				pending.push_back(optional.GetResult());
			}
		}
	}
	return snapshot;
}

TEST_CASE("Collapsing known-empty list tails preserves child semantics", "[api][grammar_extension]") {
	for (auto variant :
	     {EmptyTailTestVariant::ORDINARY, EmptyTailTestVariant::NAMED_OPTIONALS, EmptyTailTestVariant::NONCOLLAPSIBLE,
	      EmptyTailTestVariant::UNNAMED_FIRST, EmptyTailTestVariant::EMPTY_FIRST, EmptyTailTestVariant::NULLABLE_TAIL,
	      EmptyTailTestVariant::REQUIRED_TAIL, EmptyTailTestVariant::DERIVED_TAIL,
	      EmptyTailTestVariant::LEADING_CHAIN}) {
		CAPTURE(static_cast<int>(variant));
		for (auto mode : {MatchMode::BUILD_PARSE_RESULT, MatchMode::RECOGNIZE_ONLY}) {
			for (auto cursor : {false, true}) {
				for (auto &text :
				     vector<string> {"", "ANSWER", "ANSWER 1", "ANSWER 1 2", "ANSWER TAIL", "ANSWER nope"}) {
					CAPTURE(text);
					CAPTURE(cursor);
					CAPTURE(static_cast<int>(mode));
					auto expected = MatchEmptyTailTest(false, variant, mode, text, cursor);
					auto actual = MatchEmptyTailTest(true, variant, mode, text, cursor);
					REQUIRE(actual == expected);
				}
			}
		}
	}
}

static vector<string> MatchSingleStepTest(const Matcher &matcher, MatchMode mode, const string &text, bool cursor,
                                          bool single_step) {
	vector<MatcherToken> tokens;
	idx_t offset = 0;
	for (auto &word : StringUtil::Split(text, ' ')) {
		if (!word.empty()) {
			tokens.emplace_back(word, offset, TokenType::KEYWORD);
		}
		offset += word.size() + 1;
	}
	tokens.emplace_back("", text.size(), cursor ? TokenType::END_OF_INPUT_AUTOCOMPLETE : TokenType::END_OF_INPUT);
	TokenIterator iterator(tokens);
	vector<MatcherSuggestion> suggestions;
	ParseResultAllocator allocator;
	ArenaAllocator processes(Allocator::DefaultAllocator());
	idx_t max_position = 0;
	MatchContext context(suggestions, allocator, processes, max_position, mode);
	MatchState state(iterator, context);
	auto result = MatcherResult::Failure();
	if (single_step) {
		state.rule = matcher.GetRule();
		auto process = matcher.StartMatch(state);
		optional<MatcherResult> child_result;
		while (true) {
			auto step = process->Resume(child_result);
			auto child = step.GetChild();
			if (!child) {
				result = step.GetResult();
				break;
			}
			child_result = child->matcher.MatchParseResult(child->state);
		}
	} else {
		result = matcher.MatchParseResult(state);
		REQUIRE(processes.SizeInBytes() == 0);
	}
	vector<string> snapshot {
	    StringUtil::Format("%d:%llu:%llu", result.IsSuccess(), state.token_iterator.Position(), max_position),
	    result.HasParseResult() ? result.GetParseResult()->ToString() : string()};
	for (auto &token : tokens) {
		snapshot.push_back(StringUtil::Format("%d", static_cast<uint8_t>(token.type)));
	}
	for (auto &suggestion : suggestions) {
		snapshot.push_back(StringUtil::Format("%s:%d:%d:%llu", suggestion.keyword.candidate,
		                                      static_cast<uint8_t>(suggestion.type), suggestion.keyword.score_bonus,
		                                      suggestion.keyword.suggestion_pos));
	}
	return snapshot;
}

TEST_CASE("Built-in matchers preserve the public single-step process interface", "[api][grammar_extension]") {
	auto compiled = CompiledGrammar::Create();
	for (auto &definition :
	     vector<string> {"Program <- 'SELECT' 'FROM'", "Program <- ('SELECT' 'FROM') / ('WHERE' 'FROM')",
	                     "Program <- 'SELECT' / 'FROM' / 'WHERE'", "Program <- ('SELECT' 'FROM')?",
	                     "Program <- ('SELECT' 'FROM')+"}) {
		auto grammar = ParsedGrammar::Parse(definition);
		auto rules = CompileTestProgramRule(grammar);
		MatcherAllocator allocator;
		MatcherFactory factory(allocator, grammar, rules, compiled->GetKeywordHelper(), {});
		auto &root = factory.CreateRootMatcher("Program").Cast<ListMatcher>();
		vector<reference<Matcher>> matchers {root};
		for (auto &child : root.matchers) {
			matchers.push_back(child);
		}
		for (auto &matcher : matchers) {
			for (auto mode : {MatchMode::BUILD_PARSE_RESULT, MatchMode::RECOGNIZE_ONLY}) {
				for (auto cursor : {false, true}) {
					for (auto &text :
					     vector<string> {"", "SELECT", "SELECT FROM", "SELECT FROM SELECT FROM", "WHERE FROM", "X"}) {
						CAPTURE(definition);
						CAPTURE(text);
						CAPTURE(cursor);
						REQUIRE(MatchSingleStepTest(matcher, mode, text, cursor, true) ==
						        MatchSingleStepTest(matcher, mode, text, cursor, false));
					}
				}
			}
		}
	}
}

class ForwardingListTestMatcher final : public ListMatcher {
public:
	explicit ForwardingListTestMatcher(idx_t &starts_p) : starts(starts_p) {
	}

	arena_ptr<MatchProcess> StartMatch(MatchState &state) const override {
		starts++;
		return ListMatcher::StartMatch(state);
	}

private:
	idx_t &starts;
};

TEST_CASE("Derived matchers can forward to built-in processes through deep heap frames", "[api][grammar_extension]") {
	CompiledGrammarRule rule("Forwarded", nullptr);
	MatchProcessLifetimeState lifetime;
	lifetime.depth = 1;
	lifetime.create_result = true;
	NestedTestMatcher leaf(lifetime);
	idx_t starts = 0;
	vector<unique_ptr<ForwardingListTestMatcher>> lists;
	for (idx_t i = 0; i < 4096; i++) {
		auto list = make_uniq<ForwardingListTestMatcher>(starts);
		list->SetRule(rule);
		list->matchers.push_back(lists.empty() ? reference<Matcher>(leaf) : reference<Matcher>(*lists.back()));
		lists.push_back(std::move(list));
	}
	for (auto mode : {MatchMode::BUILD_PARSE_RESULT, MatchMode::RECOGNIZE_ONLY}) {
		vector<MatcherToken> tokens;
		TokenIterator iterator(tokens);
		vector<MatcherSuggestion> suggestions;
		ParseResultAllocator allocator;
		ArenaAllocator processes(Allocator::DefaultAllocator());
		reference<uint64_t> guard(*processes.Make<uint64_t>(0x123456789abcdef0));
		auto parent_bytes = processes.SizeInBytes();
		idx_t max_position = 0;
		MatchContext context(suggestions, allocator, processes, max_position, mode);
		MatchState state(iterator, context);
		MatchStack stack;
		lifetime.throw_at_leaf = true;
		starts = 0;
		REQUIRE_THROWS_AS(stack.Execute({*lists.back(), state}), InvalidInputException);
		REQUIRE(starts == lists.size());
		REQUIRE(lifetime.active == 0);
		REQUIRE(processes.SizeInBytes() == parent_bytes);
		REQUIRE(guard.get() == 0x123456789abcdef0);

		lifetime.throw_at_leaf = false;
		starts = 0;
		auto result = stack.Execute({*lists.back(), state});
		REQUIRE(result.IsSuccess());
		REQUIRE(starts == lists.size());
		REQUIRE(lifetime.active == 0);
		REQUIRE(lifetime.state_valid);
		REQUIRE(processes.SizeInBytes() == parent_bytes);
		REQUIRE(guard.get() == 0x123456789abcdef0);
		REQUIRE(result.HasParseResult() == (mode == MatchMode::BUILD_PARSE_RESULT));
		if (result.HasParseResult()) {
			auto node = result.GetParseResult();
			for (idx_t i = 0; i < lists.size(); i++) {
				REQUIRE(node->GetRule().get() == &rule);
				auto children = node->Cast<ListParseResult>().GetChildren();
				REQUIRE(children.size() == 1);
				node = children[0].get();
			}
			REQUIRE(node->Name() == "nested result");
		}
	}
}

TEST_CASE("Packrat cache preserves entries across indexed and fallback growth", "[api][grammar_extension]") {
	MatcherAllocator matchers;
	auto &first = matchers.Allocate(make_uniq<NumberLiteralMatcher>());
	auto &second = matchers.Allocate(make_uniq<NumberLiteralMatcher>());
	first.SetPackratMemoized();
	second.SetPackratMemoized();
	ArenaAllocator arena(Allocator::DefaultAllocator());
	ParserPackratCache cache(arena, 100, 1100);
	IdentifierParseResult result("cached", optional_idx(123), optional_idx(6));
	ParserPackratEntry original;
	original.success = true;
	original.token_index_after = NumericLimits<idx_t>::Maximum();
	original.max_token_index_seen = NumericLimits<idx_t>::Maximum() - 1;
	original.result = &result;
	cache.Store(first, 100, original);
	auto pinned = cache.Lookup(first, 100);
	REQUIRE(pinned);

	ParserPackratEntry failure;
	failure.token_index_after = 101;
	failure.max_token_index_seen = 102;
	cache.Store(second, 100, failure);
	for (idx_t position = 0; position < 2200; position++) {
		cache.Store(first, position, failure);
	}
	cache.Store(first, NumericLimits<idx_t>::Maximum(), failure);
	auto retained = cache.Lookup(first, 100);
	REQUIRE(retained.get() == pinned.get());
	REQUIRE(retained->success);
	REQUIRE(retained->token_index_after == original.token_index_after);
	REQUIRE(retained->max_token_index_seen == original.max_token_index_seen);
	REQUIRE(retained->result.get() == &result);
	for (auto position : {idx_t(0), idx_t(99), idx_t(1099), idx_t(1100), NumericLimits<idx_t>::Maximum()}) {
		auto entry = cache.Lookup(first, position);
		REQUIRE(entry);
		REQUIRE_FALSE(entry->success);
		REQUIRE(entry->token_index_after == failure.token_index_after);
		REQUIRE(entry->max_token_index_seen == failure.max_token_index_seen);
		REQUIRE_FALSE(entry->result);
	}
	auto other_matcher = cache.Lookup(second, 100);
	REQUIRE(other_matcher);
	REQUIRE_FALSE(other_matcher->success);
	REQUIRE_FALSE(cache.Lookup(second, 101));
}

TEST_CASE("Packrat cache accepts empty and reversed index ranges", "[api][grammar_extension]") {
	MatcherAllocator matchers;
	auto &matcher = matchers.Allocate(make_uniq<NumberLiteralMatcher>());
	matcher.SetPackratMemoized();
	for (auto range : {pair<idx_t, idx_t> {0, 0}, pair<idx_t, idx_t> {100, 100}, pair<idx_t, idx_t> {200, 100}}) {
		ArenaAllocator arena(Allocator::DefaultAllocator());
		ParserPackratCache cache(arena, range.first, range.second);
		ParserPackratEntry value;
		value.success = true;
		value.token_index_after = NumericLimits<idx_t>::Maximum();
		for (auto position : {idx_t(0), idx_t(100), idx_t(200), NumericLimits<idx_t>::Maximum()}) {
			REQUIRE_FALSE(cache.Lookup(matcher, position));
			cache.Store(matcher, position, value);
			auto entry = cache.Lookup(matcher, position);
			REQUIRE(entry);
			REQUIRE(entry->success);
			REQUIRE(entry->token_index_after == value.token_index_after);
		}
	}
}
class GroupKeywordTestMatcher final : public KeywordMatcher {
public:
	explicit GroupKeywordTestMatcher(const string &word_p) : KeywordMatcher(word_p, KeywordInfo()), word(word_p) {
	}
	void InitializeFirstSet(MatcherFirstSet &first, const GrammarLiteralTable &table) const override {
		auto literal = table.Lookup(word).LiteralId();
		REQUIRE(literal != 0);
		first.AddLiteral(literal);
	}
	MatcherResult MatchAtomic(MatchState &state) const override {
		if (should_throw) {
			throw InvalidInputException("group leaf failure");
		}
		return KeywordMatcher::MatchAtomic(state);
	}

	bool should_throw = false;

private:
	string word;
};

TEST_CASE("Prepared list groups preserve entry boundaries, fallbacks and allocator moves", "[api][grammar_extension]") {
	CompiledGrammarRule atom_rule("Atom", nullptr), inner_rule("Inner", nullptr), middle_rule("Middle", nullptr),
	    outer_rule("Outer", nullptr);
	GroupKeywordTestMatcher atom("SELECT"), b("FROM"), c("WHERE"), d("LIMIT");
	atom.SetRule(atom_rule);
	OptionalMatcher optional_b(b), optional_c(c), optional_d(d);
	for (auto optional : {&optional_b, &optional_c, &optional_d}) {
		optional->SetStructural();
	}
	MatcherAllocator allocator;
	auto &inner = allocator.Allocate(make_uniq<ListMatcher>()).Cast<ListMatcher>();
	auto &middle = allocator.Allocate(make_uniq<ListMatcher>()).Cast<ListMatcher>();
	auto &outer = allocator.Allocate(make_uniq<ListMatcher>()).Cast<ListMatcher>();
	inner.SetRule(inner_rule);
	middle.SetRule(middle_rule);
	outer.SetRule(outer_rule);
	vector<reference<ListMatcher>> lists {inner, middle, outer};
	for (auto &list : lists) {
		list.get().SetStructural();
		list.get().SetCollapsible();
	}
	inner.matchers = {atom, optional_b};
	middle.matchers = {inner, optional_c};
	outer.matchers = {middle, optional_d};
	auto grammar = CompiledGrammar::DefaultGrammar();
	auto &table = grammar->GetKeywordHelper().GetLiteralTable();
	ComputeFirstSets(outer, table);
	auto snapshots = [&]() {
		vector<vector<string>> result;
		for (auto &entry : lists) {
			for (auto mode : {MatchMode::BUILD_PARSE_RESULT, MatchMode::RECOGNIZE_ONLY}) {
				for (auto cursor : {false, true}) {
					for (auto single_step : {false, true}) {
						for (auto &text : vector<string> {"", "SELECT", "SELECT FROM", "SELECT WHERE", "SELECT LIMIT",
						                                  "SELECT FROM WHERE LIMIT", "SELECT X", "X"}) {
							result.push_back(MatchSingleStepTest(entry, mode, text, cursor, single_step));
						}
					}
				}
			}
		}
		return result;
	};
	auto expected = snapshots();
	allocator.InitializeListChains();
	REQUIRE(outer.GetPreparedChain());
	REQUIRE(outer.GetPreparedChain() == inner.GetPreparedChain());
	REQUIRE(middle.GetPreparedChain() == inner.GetPreparedChain());
	REQUIRE(outer.GetPreparedChain()->lists.size() == 3);
	REQUIRE(snapshots() == expected);

	auto moved = std::move(allocator);
	REQUIRE(snapshots() == expected);
	moved.InitializeListChains();
	REQUIRE(outer.GetPreparedChain()->lists.size() == 3);
	REQUIRE(snapshots() == expected);

	SECTION("A second parent ends the group before the shared child") {
		auto &other = moved.Allocate(make_uniq<ListMatcher>()).Cast<ListMatcher>();
		other.SetStructural();
		other.SetCollapsible();
		other.matchers = {inner, optional_d};
		ComputeFirstSets(other, table);
		moved.InitializeListChains();
		REQUIRE(outer.GetPreparedChain());
		REQUIRE(outer.GetPreparedChain()->lists.size() == 2);
		REQUIRE_FALSE(inner.GetPreparedChain());
		REQUIRE(snapshots() == expected);
	}
	SECTION("Changing a tail invalidates the old group before rebuilding") {
		middle.matchers[1] = c;
		for (auto &list : lists) {
			list.get().InitializeListChain();
		}
		ComputeFirstSets(outer, table);
		expected = snapshots();
		moved.InitializeListChains();
		for (auto &list : lists) {
			REQUIRE_FALSE(list.get().GetPreparedChain());
		}
		REQUIRE(snapshots() == expected);
	}
}

TEST_CASE("Prepared list groups do not collapse to a present named prefix", "[api][grammar_extension]") {
	// Program <- Middle 'WHERE'?   Middle <- 'NOT'? Inner   Inner <- Atom 'FROM'?   Atom <- 'SELECT'
	CompiledGrammarRule atom_rule("Atom", nullptr), prefix_rule("Prefix", nullptr), inner_rule("Inner", nullptr),
	    middle_rule("Middle", nullptr), outer_rule("Program", nullptr);
	GroupKeywordTestMatcher atom("SELECT"), not_keyword("NOT"), from_keyword("FROM"), where_keyword("WHERE");
	atom.SetRule(atom_rule);
	OptionalMatcher optional_not(not_keyword), optional_from(from_keyword), optional_where(where_keyword);
	for (auto optional : {&optional_not, &optional_from, &optional_where}) {
		optional->SetStructural();
	}
	// the prefix carries a rule, so its result passes the named-result guard of the collapse
	optional_not.SetRule(prefix_rule);
	MatcherAllocator allocator;
	auto &inner = allocator.Allocate(make_uniq<ListMatcher>()).Cast<ListMatcher>();
	auto &middle = allocator.Allocate(make_uniq<ListMatcher>()).Cast<ListMatcher>();
	auto &outer = allocator.Allocate(make_uniq<ListMatcher>()).Cast<ListMatcher>();
	inner.SetRule(inner_rule);
	middle.SetRule(middle_rule);
	outer.SetRule(outer_rule);
	vector<reference<ListMatcher>> lists {inner, middle, outer};
	for (auto &list : lists) {
		list.get().SetStructural();
		list.get().SetCollapsible();
	}
	inner.matchers = {atom, optional_from};
	middle.matchers = {optional_not, inner};
	outer.matchers = {middle, optional_where};
	auto grammar = CompiledGrammar::DefaultGrammar();
	auto &table = grammar->GetKeywordHelper().GetLiteralTable();
	ComputeFirstSets(outer, table);
	auto snapshots = [&]() {
		vector<vector<string>> result;
		for (auto &entry : lists) {
			for (auto mode : {MatchMode::BUILD_PARSE_RESULT, MatchMode::RECOGNIZE_ONLY}) {
				for (auto cursor : {false, true}) {
					for (auto single_step : {false, true}) {
						for (auto &text :
						     vector<string> {"", "SELECT", "NOT SELECT", "NOT SELECT FROM", "NOT SELECT WHERE",
						                     "NOT SELECT FROM WHERE", "NOT", "NOT X", "SELECT FROM WHERE", "X"}) {
							result.push_back(MatchSingleStepTest(entry, mode, text, cursor, single_step));
						}
					}
				}
			}
		}
		return result;
	};
	auto expected = snapshots();
	allocator.InitializeListChains();
	REQUIRE(outer.GetPreparedChain());
	REQUIRE(outer.GetPreparedChain()->lists.size() == 3);
	REQUIRE(middle.GetChainChildIndex() == 1);
	// the present prefix must not stand in for the group: both tokens are consumed
	auto prepared = MatchSingleStepTest(outer, MatchMode::BUILD_PARSE_RESULT, "NOT SELECT", false, false);
	REQUIRE(prepared[0] == "1:2:2");
	REQUIRE(snapshots() == expected);
}

TEST_CASE("Deep prepared list groups keep success and exception state on the heap", "[api][grammar_extension]") {
	CompiledGrammarRule rule("GroupLeaf", nullptr);
	GroupKeywordTestMatcher atom("SELECT");
	atom.SetRule(rule);
	auto grammar = CompiledGrammar::DefaultGrammar();
	auto &table = grammar->GetKeywordHelper().GetLiteralTable();
	ComputeFirstSets(atom, table);
	MatcherAllocator allocator;
	optional_ptr<ListMatcher> root;
	for (idx_t i = 0; i < 4096; i++) {
		auto &list = allocator.Allocate(make_uniq<ListMatcher>()).Cast<ListMatcher>();
		list.SetStructural();
		list.SetCollapsible();
		list.SetRule(rule);
		list.matchers.push_back(root ? reference<Matcher>(*root) : reference<Matcher>(atom));
		// A one-child list has exactly its child's FIRST set; avoid quadratic test setup.
		list.first_set = atom.first_set;
		root = list;
	}
	allocator.InitializeListChains();
	REQUIRE(root->GetPreparedChain());
	REQUIRE(root->GetPreparedChain()->lists.size() == 4096);
	vector<MatcherToken> tokens {MatcherToken("SELECT", 0, TokenType::KEYWORD)};
	TokenIterator iterator(tokens);
	vector<MatcherSuggestion> suggestions;
	ParseResultAllocator results;
	ArenaAllocator processes(Allocator::DefaultAllocator());
	auto &guard = *processes.Make<uint64_t>(0x123456789abcdef0);
	auto parent_bytes = processes.SizeInBytes();
	idx_t max_position = 0;
	MatchContext context(suggestions, results, processes, max_position);
	MatchStack stack;
	for (auto fail : {true, false}) {
		atom.should_throw = fail;
		MatchState state(iterator, context);
		if (fail) {
			REQUIRE_THROWS_AS(stack.Execute({*root, state}), InvalidInputException);
		} else {
			auto result = stack.Execute({*root, state});
			REQUIRE(result.IsSuccess());
			REQUIRE(result.HasParseResult());
			REQUIRE(result.GetParseResult()->GetRule().get() == &rule);
			REQUIRE(result.GetParseResult()->collapsed);
			REQUIRE(state.token_iterator.Position() == 1);
		}
		REQUIRE(processes.SizeInBytes() == parent_bytes);
		REQUIRE(guard == 0x123456789abcdef0);
	}
}

TEST_CASE("Generated list finalization validates its tree before consuming child results", "[api][grammar_extension]") {
	static const TransformFrameOps ops {"NameListValidation", [](PEGTransformer &, GeneratedTransformProcess &) {},
	                                    PEGTransformerFactory::FinalizeNameListTrampoline};
	auto grammar = CompiledGrammar::DefaultGrammar();
	auto options = ParserOptions::Builtin();
	vector<MatcherToken> tokens;
	TokenIterator iterator(tokens);
	ArenaAllocator transform_results(Allocator::DefaultAllocator());
	PEGTransformer transformer(transform_results, iterator, options, *grammar);
#ifdef DUCKDB_CRASH_ON_ASSERT
	// Malformed trees intentionally raise InternalException, which aborts in this mode.
	constexpr idx_t shape_count = 2;
#else
	constexpr idx_t shape_count = 6;
#endif
	for (idx_t shape = 0; shape < shape_count; shape++) {
		INFO("List shape: " << shape);
		ParseResultAllocator results;
		ArenaAllocator temporary(Allocator::DefaultAllocator());
		auto children = [&](std::initializer_list<reference<ParseResult>> input) {
			arena_vector<reference<ParseResult>> collected(temporary);
			for (auto &child : input) {
				collected.push_back(child);
			}
			return results.MakeChildren(collected);
		};
		auto list = [&](std::initializer_list<reference<ParseResult>> input) -> ParseResult & {
			return *results.Make<ListParseResult>(children(input), nullptr, optional_idx());
		};
		IdentifierParseResult first("alpha", optional_idx()), second("beta", optional_idx());
		KeywordParseResult punctuation(",", optional_idx());
		OptionalParseResult absent;
		auto &pair = shape == 3 ? list({punctuation}) : list({punctuation, second});
		auto repeated_child = shape == 2 ? reference<ParseResult>(second) : reference<ParseResult>(pair);
		auto repeat = results.Make<RepeatParseResult>(children({repeated_child}), optional_idx());
		OptionalParseResult tail(repeat, optional_idx());
		auto &body = shape == 4   ? list({first})
		             : shape == 5 ? list({first, second})
		             : shape == 0 ? list({first, absent})
		                          : list({first, tail});
		auto &parens = list({punctuation, body, punctuation});
		auto &root = list({parens});
		GeneratedTransformProcess process(transformer, {root}, ops);
		process.ReserveChildSlots(2);
		process.SetChildResult(0, transformer.MakeResult<Identifier>("alpha"));
		process.SetChildResult(1, transformer.MakeResult<Identifier>("beta"));
		if (shape >= 2) {
			REQUIRE_THROWS_AS(PEGTransformerFactory::FinalizeNameListTrampoline(transformer, process),
			                  InternalException);
			REQUIRE(process.GetResult<Identifier>(0) == Identifier("alpha"));
			REQUIRE(process.GetResult<Identifier>(1) == Identifier("beta"));
		} else {
			auto finalized = PEGTransformerFactory::FinalizeNameListTrampoline(transformer, process);
			auto values = TryGetTransformResult<vector<string>>(*finalized);
			REQUIRE(values);
			vector<string> expected {"alpha"};
			if (shape == 1) {
				expected.push_back("beta");
			}
			REQUIRE(*values == expected);
		}
	}
}
