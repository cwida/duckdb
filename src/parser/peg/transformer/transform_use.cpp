#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/sql_statement.hpp"

namespace duckdb {

// UseStatement <- 'USE' UseTarget
unique_ptr<SQLStatement> PEGTransformerFactory::TransformUseStatement(PEGTransformer &transformer,
                                                                      const QualifiedName &use_target) {
	auto value_expr = ConstantExpression::String(use_target.ToString());
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
	vector<Identifier> path {catalog_name, reserved_schema_name};
	if (dot_identifier) {
		path.insert(path.end(), dot_identifier->begin(), dot_identifier->end());
	}
	return QualifiedName::FromPath(std::move(path));
}

Identifier PEGTransformerFactory::TransformDotIdentifier(PEGTransformer &transformer, const Identifier &identifier) {
	return identifier;
}
} // namespace duckdb
