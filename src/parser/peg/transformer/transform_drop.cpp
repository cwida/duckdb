#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/parsed_data/extra_drop_info.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"

namespace duckdb {

unique_ptr<SQLStatement> PEGTransformerFactory::TransformDropStatement(PEGTransformer &transformer,
                                                                       unique_ptr<DropStatement> drop_entries,
                                                                       const optional<bool> &drop_behavior) {
	if (drop_behavior) {
		if (drop_entries->info->type == CatalogType::SECRET_ENTRY) {
			throw ParserException("CASCADE and RESTRICT are not supported for DROP SECRET");
		}
		drop_entries->info->cascade = *drop_behavior;
	}
	return std::move(drop_entries);
}

unique_ptr<DropStatement> PEGTransformerFactory::TransformDropTable(PEGTransformer &transformer,
                                                                    const CatalogType &table_or_view,
                                                                    const optional<bool> &if_exists,
                                                                    vector<unique_ptr<BaseTableRef>> base_table_name) {
	auto result = make_uniq<DropStatement>();
	auto info = make_uniq<DropInfo>();
	if (base_table_name.size() > 1) {
		throw NotImplementedException("Can only drop one object at a time");
	}
	auto base_table = std::move(base_table_name[0]);
	info->SetQualifiedName(base_table->GetQualifiedName());
	info->type = table_or_view;
	info->if_not_found = if_exists ? OnEntryNotFound::RETURN_NULL : OnEntryNotFound::THROW_EXCEPTION;
	result->info = std::move(info);
	return result;
}

CatalogType PEGTransformerFactory::TransformMaterializedViewEntry(PEGTransformer &transformer) {
	throw NotImplementedException("Cannot drop MATERIALIZED VIEW yet");
}

bool PEGTransformerFactory::TransformFunctionTypeMacroKeyword(PEGTransformer &transformer) {
	return true;
}

bool PEGTransformerFactory::TransformFunctionTypeFunction(PEGTransformer &transformer) {
	return false;
}

unique_ptr<DropStatement>
PEGTransformerFactory::TransformDropTableFunction(PEGTransformer &transformer, const CatalogType &comment_macro_table,
                                                  const optional<bool> &if_exists,
                                                  const vector<QualifiedName> &table_function_name) {
	auto result = make_uniq<DropStatement>();
	auto info = make_uniq<DropInfo>();
	if (table_function_name.size() > 1) {
		throw NotImplementedException("Can only drop one object at a time");
	}
	info->SetQualifiedName(table_function_name[0]);
	info->type = comment_macro_table;
	info->if_not_found = if_exists ? OnEntryNotFound::RETURN_NULL : OnEntryNotFound::THROW_EXCEPTION;
	result->info = std::move(info);
	return result;
}

unique_ptr<DropStatement>
PEGTransformerFactory::TransformDropFunction(PEGTransformer &transformer, const bool &function_type_macro,
                                             const optional<bool> &if_exists,
                                             const vector<QualifiedName> &function_identifier) {
	auto result = make_uniq<DropStatement>();
	auto info = make_uniq<DropInfo>();
	auto catalog_type = CatalogType::MACRO_ENTRY;
	if (function_identifier.size() > 1) {
		throw NotImplementedException("Can only drop one object at a time");
	}
	const auto &function = function_identifier[0];
	info->SetQualifiedName(function);
	info->if_not_found = if_exists ? OnEntryNotFound::RETURN_NULL : OnEntryNotFound::THROW_EXCEPTION;
	info->type = catalog_type;
	result->info = std::move(info);
	return result;
}

unique_ptr<DropStatement> PEGTransformerFactory::TransformDropSchema(PEGTransformer &transformer,
                                                                     const optional<bool> &if_exists,
                                                                     const vector<QualifiedName> &qualified_name) {
	auto result = make_uniq<DropStatement>();
	auto info = make_uniq<DropInfo>();
	if (qualified_name.size() > 1) {
		throw NotImplementedException("Can only drop one object at a time");
	}
	// store the full dotted path (e.g. [schema] / [parent, schema] / [catalog, parent, schema]); the leading
	// component is resolved into a catalog + parent-schema chain during binding (see Binder::Bind(DropStatement))
	info->SetQualifiedName(qualified_name[0]);
	info->if_not_found = if_exists ? OnEntryNotFound::RETURN_NULL : OnEntryNotFound::THROW_EXCEPTION;
	info->type = CatalogType::SCHEMA_ENTRY;
	result->info = std::move(info);
	return result;
}

unique_ptr<DropStatement> PEGTransformerFactory::TransformDropIndex(PEGTransformer &transformer,
                                                                    const optional<bool> &if_exists,
                                                                    const vector<QualifiedName> &qualified_index_name) {
	auto result = make_uniq<DropStatement>();
	auto info = make_uniq<DropInfo>();
	if (qualified_index_name.size() > 1) {
		throw NotImplementedException("Can only drop one object at a time");
	}
	info->SetQualifiedName(qualified_index_name[0]);
	info->type = CatalogType::INDEX_ENTRY;
	info->if_not_found = if_exists ? OnEntryNotFound::RETURN_NULL : OnEntryNotFound::THROW_EXCEPTION;
	result->info = std::move(info);
	return result;
}

QualifiedName PEGTransformerFactory::TransformQualifiedIndexNameString(PEGTransformer &transformer,
                                                                       const Identifier &index_name) {
	QualifiedName result(index_name);
	return result;
}

QualifiedName PEGTransformerFactory::TransformSchemaReservedIndex(PEGTransformer &transformer,
                                                                  const Identifier &schema_qualification,
                                                                  const Identifier &reserved_index_name) {
	QualifiedName result({schema_qualification}, reserved_index_name);
	return result;
}

QualifiedName PEGTransformerFactory::TransformCatalogReservedSchemaIndex(
    PEGTransformer &transformer, const Identifier &catalog_qualification,
    const Identifier &reserved_schema_qualification, const Identifier &reserved_index_name) {
	QualifiedName result(catalog_qualification, reserved_schema_qualification, reserved_index_name);
	return result;
}

unique_ptr<DropStatement>
PEGTransformerFactory::TransformDropSequence(PEGTransformer &transformer, const optional<bool> &if_exists,
                                             const vector<QualifiedName> &qualified_sequence_name) {
	auto result = make_uniq<DropStatement>();
	auto info = make_uniq<DropInfo>();
	if (qualified_sequence_name.size() > 1) {
		throw NotImplementedException("Can only drop one object at a time");
	}
	info->SetQualifiedName(qualified_sequence_name[0]);
	info->if_not_found = if_exists ? OnEntryNotFound::RETURN_NULL : OnEntryNotFound::THROW_EXCEPTION;
	info->type = CatalogType::SEQUENCE_ENTRY;
	result->info = std::move(info);
	return result;
}

Identifier PEGTransformerFactory::TransformCollationName(PEGTransformer &transformer, const Identifier &identifier) {
	return identifier;
}

unique_ptr<DropStatement> PEGTransformerFactory::TransformDropCollation(PEGTransformer &transformer,
                                                                        const optional<bool> &if_exists,
                                                                        const vector<Identifier> &collation_name) {
	throw NotImplementedException("Cannot drop collation yet");
	/*
	auto result = make_uniq<DropStatement>();
	auto info = make_uniq<DropInfo>();
	if (collation_name.size() > 1) {
	    throw NotImplementedException("Can only drop one object at a time");
	}
	auto collation = collation_name[0];
	info->catalog = INVALID_CATALOG;
	info->schema = INVALID_SCHEMA;
	info->name = collation;
	info->if_not_found = if_exists ? OnEntryNotFound::RETURN_NULL : OnEntryNotFound::THROW_EXCEPTION;
	info->type = CatalogType::COLLATION_ENTRY;
	result->info = std::move(info);
	return result;
	*/
}

unique_ptr<DropStatement> PEGTransformerFactory::TransformDropType(PEGTransformer &transformer,
                                                                   const optional<bool> &if_exists,
                                                                   const vector<QualifiedName> &qualified_type_name) {
	auto result = make_uniq<DropStatement>();
	auto info = make_uniq<DropInfo>();
	if (qualified_type_name.size() > 1) {
		throw NotImplementedException("Can only drop one object at a time");
	}
	info->SetQualifiedName(qualified_type_name[0]);
	info->if_not_found = if_exists ? OnEntryNotFound::RETURN_NULL : OnEntryNotFound::THROW_EXCEPTION;
	info->type = CatalogType::TYPE_ENTRY;
	result->info = std::move(info);
	return result;
}

bool PEGTransformerFactory::TransformCascadeDropBehavior(PEGTransformer &transformer) {
	return true;
}

bool PEGTransformerFactory::TransformRestrictDropBehavior(PEGTransformer &transformer) {
	return false;
}

bool PEGTransformerFactory::TransformIfExists(PEGTransformer &transformer) {
	return true;
}

unique_ptr<DropStatement> PEGTransformerFactory::TransformDropSecret(PEGTransformer &transformer,
                                                                     const optional<SecretPersistType> &temporary,
                                                                     const optional<bool> &if_exists,
                                                                     const Identifier &secret_name,
                                                                     const optional<Identifier> &drop_secret_storage) {
	auto result = make_uniq<DropStatement>();
	auto info = make_uniq<DropInfo>();
	info->type = CatalogType::SECRET_ENTRY;
	auto extra_drop_info = make_uniq<ExtraDropSecretInfo>();
	if (temporary) {
		extra_drop_info->persist_mode = *temporary;
	}

	info->if_not_found = if_exists ? OnEntryNotFound::RETURN_NULL : OnEntryNotFound::THROW_EXCEPTION;
	info->SetName(secret_name);
	if (drop_secret_storage) {
		if (extra_drop_info->persist_mode == SecretPersistType::TEMPORARY) {
			throw ParserException("Can not combine TEMPORARY with specifying a storage for drop secret");
		}
		extra_drop_info->secret_storage = drop_secret_storage->GetIdentifierName();
	}
	info->extra_drop_info = std::move(extra_drop_info);
	result->info = std::move(info);
	return result;
}

Identifier PEGTransformerFactory::TransformDropSecretStorage(PEGTransformer &transformer,
                                                             const Identifier &identifier) {
	return identifier;
}

unique_ptr<DropStatement> PEGTransformerFactory::TransformDropTrigger(PEGTransformer &transformer,
                                                                      const optional<bool> &if_exists,
                                                                      const Identifier &trigger_name,
                                                                      unique_ptr<BaseTableRef> base_table_name) {
	auto result = make_uniq<DropStatement>();
	auto info = make_uniq<DropInfo>();
	info->type = CatalogType::TRIGGER_ENTRY;
	info->if_not_found = if_exists ? OnEntryNotFound::RETURN_NULL : OnEntryNotFound::THROW_EXCEPTION;

	info->SetName(trigger_name);

	auto extra_info = make_uniq<ExtraDropTriggerInfo>();
	extra_info->base_table = std::move(base_table_name);
	info->extra_drop_info = std::move(extra_info);

	result->info = std::move(info);
	return result;
}

} // namespace duckdb
