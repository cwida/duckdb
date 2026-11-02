#include "duckdb/parser/parsed_data/create_secret_info.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"

namespace duckdb {

Value PEGTransformerFactory::GetConstantExpressionValue(unique_ptr<ParsedExpression> &expr) {
	if (expr->GetExpressionType() == ExpressionType::VALUE_CONSTANT) {
		return expr->Cast<ConstantExpression>().GetLiteral().ToValue();
	}
	if (expr->GetExpressionType() == ExpressionType::COLUMN_REF) {
		return expr->Cast<ColumnRefExpression>().GetName();
	}
	return Value();
}

unique_ptr<CreateStatement> PEGTransformerFactory::TransformCreateSecretStmt(
    PEGTransformer &transformer, const optional<bool> &if_not_exists, const optional<Identifier> &secret_name,
    const optional<Identifier> &secret_storage_specifier, const vector<GenericCopyOption> &generic_copy_option_list) {
	auto result = make_uniq<CreateStatement>();
	auto on_conflict = if_not_exists ? OnCreateConflict::IGNORE_ON_CONFLICT : OnCreateConflict::ERROR_ON_CONFLICT;
	auto info = make_uniq<CreateSecretInfo>(on_conflict, SecretPersistType::DEFAULT);
	if (secret_name) {
		info->SetSecretName(*secret_name);
	}
	if (secret_storage_specifier) {
		info->storage_type = Identifier(StringUtil::Lower(secret_storage_specifier->GetIdentifierName()));
	}
	for (const auto &option : generic_copy_option_list) {
		auto lower_name = StringUtil::Lower(option.name.GetIdentifierName());
		if (lower_name == "scope") {
			info->scope = option.GetFirstChildOrExpression();
			continue;
		}
		if (lower_name == "type") {
			info->type = option.GetFirstChildOrExpression();
			continue;
		}
		if (lower_name == "provider") {
			info->provider = option.GetFirstChildOrExpression();
			continue;
		}
		if (info->options.find(lower_name) != info->options.end()) {
			throw BinderException("Duplicate query param found while parsing create secret: '%s'", lower_name);
		}
		info->options.insert({lower_name, option.GetFirstChildOrExpression()});
	}
	if (info->GetSecretName().empty()) {
		if (!info->type) {
			throw ParserException("Failed to create secret - secret must have a type defined");
		}
		auto value = GetConstantExpressionValue(info->type);
		if (value.IsNull()) {
			throw InvalidInputException(
			    "Can not combine a non-constant expression for the secret type with a default-named secret. Either "
			    "provide an explicit secret name or use a constant expression for the secret type.");
		}
		info->SetSecretName(Identifier("__default_" + StringUtil::Lower(value.ToString())));
	}
	result->info = std::move(info);
	return result;
}

Identifier PEGTransformerFactory::TransformSecretStorageSpecifier(PEGTransformer &transformer,
                                                                  const Identifier &identifier) {
	return identifier;
}

Identifier PEGTransformerFactory::TransformSecretName(PEGTransformer &transformer, const Identifier &col_id) {
	return Identifier(col_id);
}

} // namespace duckdb
