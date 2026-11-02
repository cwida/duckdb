#include "duckdb/parser/parsed_data/comment_on_column_info.hpp"
#include "duckdb/parser/statement/alter_statement.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"

namespace duckdb {

vector<string>
PEGTransformerFactory::TransformCommentStringLiteralIdentifier(PEGTransformer &transformer,
                                                               const Identifier &string_literal_identifier) {
	return {string_literal_identifier.GetIdentifierName()};
}

unique_ptr<SQLStatement> PEGTransformerFactory::TransformCommentStatement(PEGTransformer &transformer,
                                                                          const CatalogType &comment_on_type,
                                                                          const vector<string> &comment_target,
                                                                          const Value &comment_value) {
	auto result = make_uniq<AlterStatement>();
	unique_ptr<AlterInfo> info;

	Identifier column_name;
	if (comment_on_type == CatalogType::INVALID) {
		// Column type returned
		auto identifier = comment_target;
		column_name = Identifier(identifier.back());
		identifier.pop_back();
		if (identifier.empty()) {
			throw ParserException("Invalid column reference: %s", SQLIdentifier(column_name));
		}
		auto qualified_name = StringToQualifiedName(identifier);
		info = make_uniq<SetColumnCommentInfo>(std::move(qualified_name), std::move(column_name), comment_value,
		                                       OnEntryNotFound::THROW_EXCEPTION);
	} else if (comment_on_type == CatalogType::DATABASE_ENTRY) {
		throw NotImplementedException("Adding comments to databases is not implemented");
	} else if (comment_on_type == CatalogType::SCHEMA_ENTRY) {
		throw NotImplementedException("Adding comments to schemas is not implemented");
	} else {
		auto qualified_name = StringToQualifiedName(comment_target);
		info = make_uniq<SetCommentInfo>(comment_on_type, std::move(qualified_name), comment_value,
		                                 OnEntryNotFound::THROW_EXCEPTION);
	}
	if (!info) {
		throw NotImplementedException("Cannot comment on this type");
	}
	result->info = std::move(info);
	return std::move(result);
}

CatalogType PEGTransformerFactory::TransformCommentTable(PEGTransformer &transformer) {
	return CatalogType::TABLE_ENTRY;
}

CatalogType PEGTransformerFactory::TransformCommentSequence(PEGTransformer &transformer) {
	return CatalogType::SEQUENCE_ENTRY;
}

CatalogType PEGTransformerFactory::TransformCommentFunction(PEGTransformer &transformer) {
	return CatalogType::MACRO_ENTRY;
}

CatalogType PEGTransformerFactory::TransformCommentMacroTable(PEGTransformer &transformer) {
	return CatalogType::TABLE_MACRO_ENTRY;
}

CatalogType PEGTransformerFactory::TransformCommentMacro(PEGTransformer &transformer) {
	return CatalogType::MACRO_ENTRY;
}

CatalogType PEGTransformerFactory::TransformCommentView(PEGTransformer &transformer) {
	return CatalogType::VIEW_ENTRY;
}

CatalogType PEGTransformerFactory::TransformCommentDatabase(PEGTransformer &transformer) {
	return CatalogType::DATABASE_ENTRY;
}

CatalogType PEGTransformerFactory::TransformCommentIndex(PEGTransformer &transformer) {
	return CatalogType::INDEX_ENTRY;
}

CatalogType PEGTransformerFactory::TransformCommentSchema(PEGTransformer &transformer) {
	return CatalogType::SCHEMA_ENTRY;
}

CatalogType PEGTransformerFactory::TransformCommentType(PEGTransformer &transformer) {
	return CatalogType::TYPE_ENTRY;
}

CatalogType PEGTransformerFactory::TransformCommentColumn(PEGTransformer &transformer) {
	return CatalogType::INVALID;
}

Value PEGTransformerFactory::TransformStringLiteralValue(PEGTransformer &transformer, const string &string_literal) {
	return Value(string_literal);
}

} // namespace duckdb
