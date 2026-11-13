#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/sql_statement.hpp"

namespace duckdb {

// UseStatement <- 'USE' UseTarget
unique_ptr<SQLStatement> PEGTransformerFactory::TransformUseStatement(PEGTransformer &transformer,
                                                                      const QualifiedName &use_target) {
	string value_str;
	if (IsInvalidSchema(use_target.Schema())) {
		value_str = SQLIdentifier::ToString(use_target.Name());
	} else {
		value_str = SQLIdentifier(use_target.Schema()) + "." + SQLIdentifier(use_target.Name());
	}

	auto value_expr = ConstantExpression::String(value_str);
	return make_uniq<SetVariableStatement>("schema", std::move(value_expr), SetScope::AUTOMATIC);
}

// UseTarget <- UseTargetCatalogSchema / SchemaNameAsUseTarget / CatalogNameAsUseTarget
// SchemaNameAsUseTarget <- SchemaName / StringLiteral
QualifiedName PEGTransformerFactory::TransformSchemaNameAsUseTarget(PEGTransformer &transformer,
                                                                    const string &schema_name) {
	return QualifiedName(Identifier(schema_name));
}

// CatalogNameAsUseTarget <- CatalogName / StringLiteral
QualifiedName PEGTransformerFactory::TransformCatalogNameAsUseTarget(PEGTransformer &transformer,
                                                                     const string &catalog_name) {
	return QualifiedName(Identifier(catalog_name));
}

// UseTargetCatalogSchema <- CatalogName '.' ReservedSchemaName DotIdentifier*
QualifiedName
PEGTransformerFactory::TransformUseTargetCatalogSchema(PEGTransformer &transformer, const Identifier &catalog_name,
                                                       const Identifier &reserved_schema_name,
                                                       const optional<vector<Identifier>> &dot_identifier) {
	if (dot_identifier && !dot_identifier->empty()) {
		throw ParserException("Expected \"USE database\" or \"USE database.schema\"");
	}
	QualifiedName result({catalog_name}, reserved_schema_name);
	return result;
}

Identifier PEGTransformerFactory::TransformDotIdentifier(PEGTransformer &transformer, const Identifier &identifier) {
	return identifier;
}
} // namespace duckdb
