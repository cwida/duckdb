#include "duckdb/parser/statement/load_statement.hpp"
#include "duckdb/parser/statement/update_extensions_statement.hpp"
#include "duckdb/parser/parsed_data/update_extensions_info.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/peg/ast/extension_repository_info.hpp"

namespace duckdb {

unique_ptr<SQLStatement>
PEGTransformerFactory::TransformLoadStatement(PEGTransformer &transformer, const Identifier &col_id_or_string,
                                              const optional<ExtensionRepositoryInfo> &from_source,
                                              const optional<Identifier> &extension_alias) {
	auto result = make_uniq<LoadStatement>();
	auto info = make_uniq<LoadInfo>();
	info->repo_is_alias = false;
	info->filename = col_id_or_string.GetIdentifierName();
	if (from_source) {
		info->repository = from_source->name.GetIdentifierName();
		info->repo_is_alias = from_source->repository_is_alias;
	}
	if (extension_alias) {
		info->alias = *extension_alias;
		info->load_type = LoadType::LOAD_AS;
	} else {
		info->load_type = LoadType::LOAD;
	}
	result->info = std::move(info);
	return std::move(result);
}

Identifier PEGTransformerFactory::TransformExtensionAlias(PEGTransformer &transformer, const Identifier &identifier) {
	return identifier;
}

bool PEGTransformerFactory::TransformInstallAndLoad(PEGTransformer &transformer) {
	return true;
}

unique_ptr<SQLStatement> PEGTransformerFactory::TransformInstallStatement(
    PEGTransformer &transformer, const bool &has_result, const optional<bool> &install_and_load,
    const QualifiedName &identifier_or_string_literal, const optional<ExtensionRepositoryInfo> &from_source,
    const optional<string> &version_number) {
	auto result = make_uniq<LoadStatement>();
	auto info = make_uniq<LoadInfo>();
	info->load_type = has_result ? LoadType::FORCE_INSTALL : LoadType::INSTALL;
	info->load_after_install = install_and_load.has_value();
	info->filename = identifier_or_string_literal.Name().GetIdentifierName();
	info->repo_is_alias = false;
	if (from_source) {
		info->repository = from_source->name.GetIdentifierName();
		info->repo_is_alias = from_source->repository_is_alias;
	}
	if (version_number) {
		info->version = *version_number;
	}
	result->info = std::move(info);
	return std::move(result);
}

ExtensionRepositoryInfo PEGTransformerFactory::TransformFromSourceIdentifier(PEGTransformer &transformer,
                                                                             const Identifier &identifier) {
	ExtensionRepositoryInfo result;
	result.name = identifier;
	result.repository_is_alias = true;
	return result;
}

ExtensionRepositoryInfo PEGTransformerFactory::TransformFromSourceString(PEGTransformer &transformer,
                                                                         const string &string_literal) {
	ExtensionRepositoryInfo result;
	result.name = Identifier(string_literal);
	result.repository_is_alias = false;
	return result;
}

unique_ptr<SQLStatement>
PEGTransformerFactory::TransformUpdateExtensionsStatement(PEGTransformer &transformer,
                                                          const optional<vector<Identifier>> &identifier) {
	auto result = make_uniq<UpdateExtensionsStatement>();
	auto info = make_uniq<UpdateExtensionsInfo>();
	if (identifier) {
		info->extensions_to_update = *identifier;
	}
	result->info = std::move(info);
	return std::move(result);
}

string PEGTransformerFactory::TransformVersionNumber(PEGTransformer &transformer,
                                                     const QualifiedName &identifier_or_string_literal) {
	return identifier_or_string_literal.Name().GetIdentifierName();
}

unique_ptr<SQLStatement> PEGTransformerFactory::TransformCreateExtensionRepositoryStmt(
    PEGTransformer &transformer, const optional<bool> &or_replace, const optional<bool> &if_not_exists,
    const Identifier &col_id_or_string, const string &repository_prefix,
    const optional<vector<string>> &repository_public_key) {
	auto result = make_uniq<LoadStatement>();
	auto info = make_uniq<LoadInfo>();
	info->load_type = LoadType::CREATE_REPOSITORY;
	info->repo_is_alias = false;
	info->repository = col_id_or_string.GetIdentifierName();
	info->repository_url = repository_prefix;
	if (repository_public_key) {
		info->public_keys = *repository_public_key;
	}
	if (or_replace && if_not_exists) {
		throw ParserException("Cannot combine OR REPLACE with IF NOT EXISTS");
	}
	if (or_replace) {
		info->on_conflict = OnCreateConflict::REPLACE_ON_CONFLICT;
	} else if (if_not_exists) {
		info->on_conflict = OnCreateConflict::IGNORE_ON_CONFLICT;
	}
	result->info = std::move(info);
	return std::move(result);
}

// carlopi.github.io: the words joined back into the host name
Identifier PEGTransformerFactory::TransformExtensionRepositoryHostName(PEGTransformer &transformer,
                                                                       const Identifier &col_id,
                                                                       const vector<string> &dot_col_label) {
	auto name = col_id.GetIdentifierName();
	for (auto &label : dot_col_label) {
		name += "." + label;
	}
	return Identifier(name);
}

// CREATE EXTENSION REPOSITORY <name>: the host names the prefix when the statement runs
unique_ptr<SQLStatement> PEGTransformerFactory::TransformCreateExtensionRepositoryFromHostStmt(
    PEGTransformer &transformer, const optional<bool> &or_replace, const optional<bool> &if_not_exists,
    const Identifier &extension_repository_name) {
	return TransformCreateExtensionRepositoryStmt(transformer, or_replace, if_not_exists, extension_repository_name,
	                                              string(), optional<vector<string>>());
}

string PEGTransformerFactory::TransformRepositoryPrefix(PEGTransformer &transformer, const bool &has_result,
                                                        const string &string_literal) {
	// an empty prefix is not "no prefix": the form without one resolves the repository from its host, and must not be
	// reachable with keys of the caller's choosing attached
	if (string_literal.empty()) {
		throw ParserException(
		    "CREATE EXTENSION REPOSITORY: the prefix cannot be empty. Leave WITH PREFIX out to resolve "
		    "the repository from its host");
	}
	return string_literal;
}

vector<string> PEGTransformerFactory::TransformRepositoryPublicKey(PEGTransformer &transformer, const bool &has_result,
                                                                   const vector<string> &string_literal) {
	return string_literal;
}

unique_ptr<SQLStatement> PEGTransformerFactory::TransformDropExtensionRepositoryStmt(
    PEGTransformer &transformer, const optional<bool> &if_exists, const Identifier &col_id_or_string) {
	auto result = make_uniq<LoadStatement>();
	auto info = make_uniq<LoadInfo>();
	info->load_type = LoadType::DROP_REPOSITORY;
	info->repo_is_alias = false;
	info->repository = col_id_or_string.GetIdentifierName();
	info->missing_ok = if_exists.has_value();
	result->info = std::move(info);
	return std::move(result);
}

} // namespace duckdb
