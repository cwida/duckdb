#include "duckdb.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/peg/dialect_extension.hpp"
#include "duckdb/parser/peg/parsed_grammar.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"

using namespace duckdb;

static arena_ptr<TransformResultValue> TransformDialectDemoExpression(PEGTransformer &transformer,
                                                                      ParseResult &parse_result) {
	auto result = ConstantExpression::String("Hello from the dialect extension demo");
	return transformer.MakeResult<unique_ptr<ParsedExpression>>(std::move(result));
}

static arena_ptr<TransformProcess> TransformDialectDemoProcess(PEGTransformer &transformer, ParseResult &parse_result) {
	return transformer.MakeProcess<FinalizeTransformProcess>(transformer, parse_result, TransformDialectDemoExpression);
}

class LoadableDialectExtensionDemo final : public DialectExtension {
public:
	LoadableDialectExtensionDemo() : DialectExtension("loadable_dialect_extension_demo") {
	}

	void ApplyGrammarChanges(GrammarChangesInput &input) override {
		input.parsed_grammar.AddRule("DialectDemoExpression <- 'DIALECT_DEMO'", TransformDialectDemoProcess);
		input.parsed_grammar.PrependChoice("SingleExpression", "DialectDemoExpression");
	}
};

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(loadable_dialect_extension_demo, loader) {
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	DialectExtension::Register(config, make_uniq<LoadableDialectExtensionDemo>());
}
}
