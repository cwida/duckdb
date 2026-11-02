#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/statement/call_statement.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"

namespace duckdb {

unique_ptr<SQLStatement> PEGTransformerFactory::TransformCheckpointStatement(PEGTransformer &transformer,
                                                                             const optional<bool> &checkpoint_force,
                                                                             const optional<Identifier> &catalog_name) {
	auto checkpoint_name = checkpoint_force ? "force_checkpoint" : "checkpoint";
	auto result = make_uniq<CallStatement>();
	vector<unique_ptr<ParsedExpression>> children;
	auto function = make_uniq<FunctionExpression>(checkpoint_name, std::move(children));
	function->SetQualifiedName(
	    QualifiedName(Identifier(SYSTEM_CATALOG), Identifier(DEFAULT_SCHEMA), function->GetQualifiedName().Name()));
	if (catalog_name) {
		function->GetArgumentsMutable().emplace_back(ConstantExpression::String(catalog_name->GetIdentifierName()));
	}
	result->function = std::move(function);
	return std::move(result);
}
bool PEGTransformerFactory::TransformCheckpointForce(PEGTransformer &transformer) {
	return true;
}

} // namespace duckdb
