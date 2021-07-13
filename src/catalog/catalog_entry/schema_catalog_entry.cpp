#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/aggregate_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/collate_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/copy_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/index_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/macro_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/pragma_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/scalar_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/sequence_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/view_catalog_entry.hpp"
#include "duckdb/catalog/default/default_functions.hpp"
#include "duckdb/catalog/default/default_views.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"
#include "duckdb/parser/parsed_data/create_collation_info.hpp"
#include "duckdb/parser/parsed_data/create_copy_function_info.hpp"
#include "duckdb/parser/parsed_data/create_index_info.hpp"
#include "duckdb/parser/parsed_data/create_pragma_function_info.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/parsed_data/create_sequence_info.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/parser/parsed_data/create_view_info.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"
#include "duckdb/transaction/transaction.hpp"
#include "duckdb/storage/data_table.hpp"
#include <algorithm>
#include <sstream>

namespace duckdb {

SchemaCatalogEntry::SchemaCatalogEntry(Catalog *catalog, string name_p, bool internal)
    : CatalogEntry(CatalogType::SCHEMA_ENTRY, catalog, move(name_p)),
      tables(*catalog, make_unique<DefaultViewGenerator>(*catalog, this)), indexes(*catalog), table_functions(*catalog),
      copy_functions(*catalog), pragma_functions(*catalog),
      functions(*catalog, make_unique<DefaultFunctionGenerator>(*catalog, this)), sequences(*catalog),
      collations(*catalog) {
	this->internal = internal;
}

CatalogEntry *SchemaCatalogEntry::AddEntry(ClientContext &context, unique_ptr<StandardEntry> entry,
                                           OnCreateConflict on_conflict, unordered_set<CatalogEntry *> dependencies) {
	auto entry_name = entry->name;
	auto entry_type = entry->type;
	auto result = entry.get();

	// first find the set for this entry
	auto &set = GetCatalogSet(entry_type);

	if (name != TEMP_SCHEMA) {
		dependencies.insert(this);
	} else {
		entry->temporary = true;
	}
	if (on_conflict == OnCreateConflict::REPLACE_ON_CONFLICT) {
		// CREATE OR REPLACE: first try to drop the entry
		auto old_entry = set.GetEntry(context, entry_name);
		if (old_entry) {
			if (old_entry->type != entry_type) {
				throw CatalogException("Existing object %s is of type %s, trying to replace with type %s", entry_name,
				                       CatalogTypeToString(old_entry->type), CatalogTypeToString(entry_type));
			}
			(void)set.DropEntry(context, entry_name, false);
		}
	}
	// now try to add the entry
	if (!set.CreateEntry(context, entry_name, move(entry), dependencies)) {
		// entry already exists!
		if (on_conflict == OnCreateConflict::ERROR_ON_CONFLICT) {
			throw CatalogException("%s with name \"%s\" already exists!", CatalogTypeToString(entry_type), entry_name);
		} else {
			return nullptr;
		}
	}
	return result;
}

CatalogEntry *SchemaCatalogEntry::AddEntry(ClientContext &context, unique_ptr<StandardEntry> entry,
                                           OnCreateConflict on_conflict) {
	unordered_set<CatalogEntry *> dependencies;
	return AddEntry(context, move(entry), on_conflict, dependencies);
}

CatalogEntry *SchemaCatalogEntry::CreateSequence(ClientContext &context, CreateSequenceInfo *info) {
	auto sequence = make_unique<SequenceCatalogEntry>(catalog, this, info);
	return AddEntry(context, move(sequence), info->on_conflict);
}

CatalogEntry *SchemaCatalogEntry::CreateTable(ClientContext &context, BoundCreateTableInfo *info) {
	auto table = make_unique<TableCatalogEntry>(catalog, this, info);
	table->storage->info->cardinality = table->storage->GetTotalRows();
	return AddEntry(context, move(table), info->Base().on_conflict, info->dependencies);
}

CatalogEntry *SchemaCatalogEntry::CreateView(ClientContext &context, CreateViewInfo *info) {
	auto view = make_unique<ViewCatalogEntry>(catalog, this, info);
	return AddEntry(context, move(view), info->on_conflict);
}

CatalogEntry *SchemaCatalogEntry::CreateIndex(ClientContext &context, CreateIndexInfo *info, TableCatalogEntry *table) {
	unordered_set<CatalogEntry *> dependencies;
	dependencies.insert(table);
	auto index = make_unique<IndexCatalogEntry>(catalog, this, info);
	return AddEntry(context, move(index), info->on_conflict, dependencies);
}

CatalogEntry *SchemaCatalogEntry::CreateCollation(ClientContext &context, CreateCollationInfo *info) {
	auto collation = make_unique<CollateCatalogEntry>(catalog, this, info);
	return AddEntry(context, move(collation), info->on_conflict);
}

CatalogEntry *SchemaCatalogEntry::CreateTableFunction(ClientContext &context, CreateTableFunctionInfo *info) {
	auto table_function = make_unique<TableFunctionCatalogEntry>(catalog, this, info);
	return AddEntry(context, move(table_function), info->on_conflict);
}

CatalogEntry *SchemaCatalogEntry::CreateCopyFunction(ClientContext &context, CreateCopyFunctionInfo *info) {
	auto copy_function = make_unique<CopyFunctionCatalogEntry>(catalog, this, info);
	return AddEntry(context, move(copy_function), info->on_conflict);
}

CatalogEntry *SchemaCatalogEntry::CreatePragmaFunction(ClientContext &context, CreatePragmaFunctionInfo *info) {
	auto pragma_function = make_unique<PragmaFunctionCatalogEntry>(catalog, this, info);
	return AddEntry(context, move(pragma_function), info->on_conflict);
}

CatalogEntry *SchemaCatalogEntry::CreateFunction(ClientContext &context, CreateFunctionInfo *info) {
	unique_ptr<StandardEntry> function;
	switch (info->type) {
	case CatalogType::SCALAR_FUNCTION_ENTRY:
		function = make_unique_base<StandardEntry, ScalarFunctionCatalogEntry>(catalog, this,
		                                                                       (CreateScalarFunctionInfo *)info);
		break;
	case CatalogType::MACRO_ENTRY:
		// create a macro function
		function = make_unique_base<StandardEntry, MacroCatalogEntry>(catalog, this, (CreateMacroInfo *)info);
		break;
	case CatalogType::AGGREGATE_FUNCTION_ENTRY:
		D_ASSERT(info->type == CatalogType::AGGREGATE_FUNCTION_ENTRY);
		// create an aggregate function
		function = make_unique_base<StandardEntry, AggregateFunctionCatalogEntry>(catalog, this,
		                                                                          (CreateAggregateFunctionInfo *)info);
		break;
	default:
		throw InternalException("Unknown function type \"%s\"", CatalogTypeToString(info->type));
	}
	return AddEntry(context, move(function), info->on_conflict);
}

void SchemaCatalogEntry::DropEntry(ClientContext &context, DropInfo *info) {
	auto &set = GetCatalogSet(info->type);

	// first find the entry
	auto existing_entry = set.GetEntry(context, info->name);
	if (!existing_entry) {
		if (!info->if_exists) {
			throw CatalogException("%s with name \"%s\" does not exist!", CatalogTypeToString(info->type), info->name);
		}
		return;
	}
	if (existing_entry->type != info->type) {
		throw CatalogException("Existing object %s is of type %s, trying to replace with type %s", info->name,
		                       CatalogTypeToString(existing_entry->type), CatalogTypeToString(info->type));
	}
	if (!set.DropEntry(context, info->name, info->cascade)) {
		throw InternalException("Could not drop element because of an internal error");
	}
}

void SchemaCatalogEntry::Alter(ClientContext &context, AlterInfo *info) {
	CatalogType type = info->GetCatalogType();
	string name = info->name;
	auto &set = GetCatalogSet(type);
	if (!set.AlterEntry(context, name, info)) {
		throw CatalogException("Entry with name \"%s\" does not exist!", name);
	}
}

CatalogEntry *SchemaCatalogEntry::GetEntry(ClientContext &context, CatalogType type, const string &entry_name,
                                           bool if_exists, QueryErrorContext error_context) {
	auto &set = GetCatalogSet(type);

	auto entry = set.GetEntry(context, entry_name);
	if (!entry) {
		if (!if_exists) {
			auto entry = set.SimilarEntry(context, entry_name);
			string did_you_mean;
			if (!entry.empty()) {
				did_you_mean = "\nDid you mean \"" + entry + "\"?";
			}
			throw CatalogException(error_context.FormatError("%s with name %s does not exist!%s",
			                                                 CatalogTypeToString(type), entry_name, did_you_mean));
		}
		return nullptr;
	}
	return entry;
}

void SchemaCatalogEntry::Scan(ClientContext &context, CatalogType type,
                              const std::function<void(CatalogEntry *)> &callback) {
	auto &set = GetCatalogSet(type);
	set.Scan(context, callback);
}

void SchemaCatalogEntry::Scan(CatalogType type, const std::function<void(CatalogEntry *)> &callback) {
	auto &set = GetCatalogSet(type);
	set.Scan(callback);
}

void SchemaCatalogEntry::Serialize(Serializer &serializer) {
	serializer.WriteString(name);
}

unique_ptr<CreateSchemaInfo> SchemaCatalogEntry::Deserialize(Deserializer &source) {
	auto info = make_unique<CreateSchemaInfo>();
	info->schema = source.Read<string>();
	return info;
}

string SchemaCatalogEntry::ToSQL() {
	std::stringstream ss;
	ss << "CREATE SCHEMA " << name << ";";
	return ss.str();
}

CatalogSet &SchemaCatalogEntry::GetCatalogSet(CatalogType type) {
	switch (type) {
	case CatalogType::VIEW_ENTRY:
	case CatalogType::TABLE_ENTRY:
		return tables;
	case CatalogType::INDEX_ENTRY:
		return indexes;
	case CatalogType::TABLE_FUNCTION_ENTRY:
		return table_functions;
	case CatalogType::COPY_FUNCTION_ENTRY:
		return copy_functions;
	case CatalogType::PRAGMA_FUNCTION_ENTRY:
		return pragma_functions;
	case CatalogType::AGGREGATE_FUNCTION_ENTRY:
	case CatalogType::SCALAR_FUNCTION_ENTRY:
	case CatalogType::MACRO_ENTRY:
		return functions;
	case CatalogType::SEQUENCE_ENTRY:
		return sequences;
	case CatalogType::COLLATION_ENTRY:
		return collations;
	default:
		throw InternalException("Unsupported catalog type in schema");
	}
}

} // namespace duckdb
