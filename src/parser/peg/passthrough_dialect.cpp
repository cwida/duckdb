#include "duckdb/parser/peg/passthrough_dialect.hpp"

#include "duckdb/parser/peg/parsed_grammar.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/statement/passthrough_statement.hpp"

namespace duckdb {

namespace {

//! Yields the statement without looking at its tokens - the text is filled in from the parse result's extent
class PassthroughTransformProcess final : public TransformProcess {
public:
	explicit PassthroughTransformProcess(PEGTransformer &transformer_p) : transformer(transformer_p) {
	}

	TransformStep Resume(arena_ptr<TransformResultValue> child_result) override {
		D_ASSERT(!child_result);
		unique_ptr<SQLStatement> statement = make_uniq<PassthroughStatement>();
		return TransformStep::Complete(transformer.MakeResult<unique_ptr<SQLStatement>>(std::move(statement)));
	}

private:
	PEGTransformer &transformer;
};

arena_ptr<TransformProcess> StartPassthroughTransform(PEGTransformer &transformer, ParseResult &) {
	return transformer.MakeProcess<PassthroughTransformProcess>(transformer);
}

} // namespace

void PassthroughDialect::ApplyGrammarChanges(GrammarChangesInput &input) {
	auto &grammar = input.parsed_grammar;
	// the body is a placeholder: the terminal override below is what matches a token
	grammar.AddRule("PassthroughToken <- Identifier");
	grammar.AddTerminalRuleOverride("PassthroughToken", [](const PEGKeywordHelper &) -> unique_ptr<Matcher> {
		return make_uniq<PassthroughTokenMatcher>();
	});
	grammar.AddRule("PassthroughStatement <- PassthroughToken+", StartPassthroughTransform);
	// keep Statement's dispatch transform - it forwards to whichever alternative matched
	auto statement_rule = grammar.GetRule("Statement");
	if (!statement_rule) {
		throw InternalException("The passthrough dialect requires a Statement rule");
	}
	grammar.ReplaceRule("Statement <- DisconnectStatement / PassthroughStatement", statement_rule->transform_process);
}

} // namespace duckdb
