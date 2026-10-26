#include "duckdb/catalog/catalog_entry/duck_schema_entry.hpp"
#include "duckdb/catalog/catalog.hpp"

#include "duckdb/catalog/catalog_entry/aggregate_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/collate_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/coordinate_system_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/copy_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/duck_index_entry.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/catalog_entry/macro_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/pragma_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/scalar_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/sequence_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/type_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/view_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/window_function_catalog_entry.hpp"
#include "duckdb/catalog/default/default_coordinate_systems.hpp"
#include "duckdb/catalog/default/default_functions.hpp"
#include "duckdb/catalog/default/default_table_functions.hpp"
#include "duckdb/catalog/default/default_types.hpp"
#include "duckdb/catalog/default/default_views.hpp"
#include "duckdb/catalog/dependency_list.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parser/constraints/foreign_key_constraint.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"
#include "duckdb/parser/parsed_data/create_aggregate_function_info.hpp"
#include "duckdb/parser/parsed_data/create_collation_info.hpp"
#include "duckdb/parser/parsed_data/create_copy_function_info.hpp"
#include "duckdb/parser/parsed_data/create_index_info.hpp"
#include "duckdb/parser/parsed_data/create_pragma_function_info.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/parsed_data/create_sequence_info.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/parser/parsed_data/create_type_info.hpp"
#include "duckdb/parser/parsed_data/create_view_info.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/parser/parsed_data/extra_drop_info.hpp"
#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/transaction/meta_transaction.hpp"
#include "duckdb/parser/parsed_data/create_window_function_info.hpp"

namespace duckdb {

static void FindForeignKeyInformation(TableCatalogEntry &table, AlterForeignKeyType alter_fk_type,
                                      vector<unique_ptr<AlterForeignKeyInfo>> &fk_arrays) {
	auto &constraints = table.GetConstraints();
	auto &name = table.name;
	for (idx_t i = 0; i < constraints.size(); i++) {
		auto &cond = constraints[i];
		if (cond->type != ConstraintType::FOREIGN_KEY) {
			continue;
		}
		auto &fk = cond->Cast<ForeignKeyConstraint>();
		if (fk.info.type == ForeignKeyType::FK_TYPE_FOREIGN_KEY_TABLE) {
			// the referenced table lives in the same (possibly nested) schema as this table
			AlterEntryData alter_data(table.schema.GetQualifiedName(fk.info.table), OnEntryNotFound::THROW_EXCEPTION);
			fk_arrays.push_back(make_uniq<AlterForeignKeyInfo>(std::move(alter_data), name, fk.pk_columns,
			                                                   fk.fk_columns, fk.info.pk_keys, fk.info.fk_keys,
			                                                   alter_fk_type));
		} else if (fk.info.type == ForeignKeyType::FK_TYPE_PRIMARY_KEY_TABLE &&
		           alter_fk_type == AlterForeignKeyType::AFT_DELETE) {
			throw CatalogException("Could not drop the table because this table is main key table of the table %s",
			                       fk.info.table);
		}
	}
}

DuckSchemaEntry::DuckSchemaEntry(Catalog &catalog, CreateSchemaInfo &info,
                                 optional_ptr<SchemaCatalogEntry> parent_schema_p)
    : SchemaCatalogEntry(catalog, info, parent_schema_p), schemas(catalog),
      tables(catalog, catalog.IsSystemCatalog() ? make_uniq<DefaultViewGenerator>(catalog, *this) : nullptr),
      indexes(catalog),
      table_functions(catalog,
                      catalog.IsSystemCatalog() ? make_uniq<DefaultTableFunctionGenerator>(catalog, *this) : nullptr),
      copy_functions(catalog), pragma_functions(catalog),
      functions(catalog, catalog.IsSystemCatalog() ? make_uniq<DefaultFunctionGenerator>(catalog, *this) : nullptr),
      sequences(catalog), collations(catalog), types(catalog, make_uniq<DefaultTypeGenerator>(catalog, *this)),
      coordinate_systems(
          catalog, catalog.IsSystemCatalog() ? make_uniq<DefaultCoordinateSystemGenerator>(catalog, *this) : nullptr) {
}

unique_ptr<CatalogEntry> DuckSchemaEntry::Copy(ClientContext &context) const {
	auto info_copy = GetInfo();
	auto &cast_info = info_copy->Cast<CreateSchemaInfo>();

	auto result = make_uniq<DuckSchemaEntry>(catalog, cast_info, parent_schema);

	return std::move(result);
}

optional_ptr<CatalogEntry> DuckSchemaEntry::CreateSchema(CatalogTransaction transaction, CreateSchemaInfo &info) {
	LogicalDependencyList dependencies;
	dependencies.AddDependency(*this);
	auto entry = make_uniq<DuckSchemaEntry>(catalog, info, *this);
	auto result = entry.get();
	if (!schemas.CreateEntry(transaction, info.SchemaName(), std::move(entry), dependencies)) {
		return nullptr;
	}
	return result;
}

optional_ptr<CatalogEntry> DuckSchemaEntry::AddEntryInternal(CatalogTransaction transaction,
                                                             unique_ptr<StandardEntry> entry,
                                                             OnCreateConflict on_conflict,
                                                             LogicalDependencyList dependencies) {
	auto entry_name = entry->name;
	auto entry_type = entry->type;
	auto result = entry.get();

	if (transaction.context) {
		auto &meta = MetaTransaction::Get(transaction.GetContext());
		auto modified_database = meta.ModifiedDatabase();
		auto &db = ParentCatalog().GetAttached();
		if (!db.IsTemporary() && !db.IsSystem()) {
			if (!modified_database || !RefersToSameObject(*modified_database, ParentCatalog().GetAttached())) {
				throw InternalException(
				    "DuckSchemaEntry::AddEntryInternal called but this database is not marked as modified");
			}
		}
	}
	// first find the set for this entry
	auto &set = GetCatalogSet(entry_type);
	dependencies.AddDependency(*this);
	if (on_conflict == OnCreateConflict::IGNORE_ON_CONFLICT) {
		auto old_entry = set.GetEntry(transaction, entry_name);
		if (old_entry) {
			return nullptr;
		}
	}

	if (on_conflict == OnCreateConflict::REPLACE_ON_CONFLICT) {
		// CREATE OR REPLACE: first try to drop the entry
		auto old_entry = set.GetEntry(transaction, entry_name);
		if (old_entry) {
			if (dependencies.Contains(*old_entry)) {
				throw CatalogException("CREATE OR REPLACE is not allowed to depend on itself");
			}
			if (old_entry->type != entry_type) {
				throw CatalogException("Existing object %s is of type %s, trying to replace with type %s", entry_name,
				                       CatalogTypeToString(old_entry->type), CatalogTypeToString(entry_type));
			}
			(void)DropEntryInternal(transaction, *old_entry, entry_name, false, entry->internal);
		}
	}
	// now try to add the entry
	if (!set.CreateEntry(transaction, entry_name, std::move(entry), dependencies)) {
		// entry already exists!
		if (on_conflict == OnCreateConflict::ERROR_ON_CONFLICT) {
			auto existing_entry = set.GetEntry(transaction, entry_name);
			auto existing_type = existing_entry ? existing_entry->type : entry_type;
			throw CatalogException::EntryAlreadyExists(existing_type, entry_name);
		} else {
			return nullptr;
		}
	}
	return result;
}

bool DuckSchemaEntry::DropEntryInternal(CatalogTransaction transaction, CatalogEntry &entry, const Identifier &name,
                                        bool cascade, bool allow_drop_internal) {
	vector<unique_ptr<AlterForeignKeyInfo>> fk_arrays;
	if (entry.type == CatalogType::TABLE_ENTRY) {
		// if there is a foreign key constraint, get that information
		FindForeignKeyInformation(entry.Cast<TableCatalogEntry>(), AlterForeignKeyType::AFT_DELETE, fk_arrays);
	}

	OnDropEntry(transaction, entry);
	auto &set = GetCatalogSet(entry.type);
	if (!set.DropEntry(transaction, name, cascade, allow_drop_internal)) {
		return false;
	}

	// remove the foreign key constraint in main key table if main key table's name is valid
	for (auto &fk_info : fk_arrays) {
		Alter(transaction, *fk_info);
	}
	return true;
}

optional_ptr<CatalogEntry> DuckSchemaEntry::CreateTable(CatalogTransaction transaction, BoundCreateTableInfo &info) {
	auto table = make_uniq<DuckTableEntry>(catalog, *this, info);
	auto &dependencies = info.Base().dependencies;

	vector<unique_ptr<AlterForeignKeyInfo>> fk_arrays;
	FindForeignKeyInformation(*table, AlterForeignKeyType::AFT_ADD, fk_arrays);
	for (idx_t i = 0; i < fk_arrays.size(); i++) {
		auto &fk_info = *fk_arrays[i];
		auto &set = GetCatalogSet(CatalogType::TABLE_ENTRY);
		dependencies.AddDependency(*set.GetEntry(transaction, fk_info.GetQualifiedName().Name()));
	}
	for (auto &dep : dependencies.Set()) {
		table->dependencies.AddDependency(dep);
	}

	auto entry = AddEntryInternal(transaction, std::move(table), info.Base().on_conflict, dependencies);
	if (!entry) {
		return nullptr;
	}

	// add a foreign key constraint in main key table if there is a foreign key constraint
	for (auto &fk_info : fk_arrays) {
		Alter(transaction, *fk_info);
	}

	return entry;
}

optional_ptr<CatalogEntry> DuckSchemaEntry::CreateFunction(CatalogTransaction transaction, CreateFunctionInfo &info) {
	if (info.on_conflict == OnCreateConflict::ALTER_ON_CONFLICT) {
		// check if the original entry exists
		auto &catalog_set = GetCatalogSet(info.type);
		auto current_entry = catalog_set.GetEntry(transaction, info.GetFunctionName());
		if (current_entry) {
			// the current entry exists - alter it instead
			auto alter_info = info.GetAlterInfo();
			Alter(transaction, *alter_info);
			return nullptr;
		}
	}
	unique_ptr<StandardEntry> function;
	switch (info.type) {
	case CatalogType::SCALAR_FUNCTION_ENTRY:
		function = make_uniq_base<StandardEntry, ScalarFunctionCatalogEntry>(catalog, *this,
		                                                                     info.Cast<CreateScalarFunctionInfo>());
		break;
	case CatalogType::TABLE_FUNCTION_ENTRY:
		function = make_uniq_base<StandardEntry, TableFunctionCatalogEntry>(catalog, *this,
		                                                                    info.Cast<CreateTableFunctionInfo>());
		break;
	case CatalogType::MACRO_ENTRY:
	case CatalogType::TABLE_MACRO_ENTRY:
		function = MacroCatalogEntry::Create(catalog, *this, info.Cast<CreateMacroInfo>());
		break;
	case CatalogType::AGGREGATE_FUNCTION_ENTRY:
		D_ASSERT(info.type == CatalogType::AGGREGATE_FUNCTION_ENTRY);
		// create an aggregate function
		function = make_uniq_base<StandardEntry, AggregateFunctionCatalogEntry>(
		    catalog, *this, info.Cast<CreateAggregateFunctionInfo>());
		break;
	case CatalogType::WINDOW_FUNCTION_ENTRY:
		D_ASSERT(info.type == CatalogType::WINDOW_FUNCTION_ENTRY);
		// create a window function
		function = make_uniq_base<StandardEntry, WindowFunctionCatalogEntry>(catalog, *this,
		                                                                     info.Cast<CreateWindowFunctionInfo>());
		break;
	default:
		throw InternalException("Unknown function type \"%s\"", CatalogTypeToString(info.type));
	}
	function->internal = info.internal;
	return AddEntry(transaction, std::move(function), info.on_conflict);
}

optional_ptr<CatalogEntry> DuckSchemaEntry::AddEntry(CatalogTransaction transaction, unique_ptr<StandardEntry> entry,
                                                     OnCreateConflict on_conflict) {
	LogicalDependencyList dependencies = entry->dependencies;
	return AddEntryInternal(transaction, std::move(entry), on_conflict, dependencies);
}

optional_ptr<CatalogEntry> DuckSchemaEntry::CreateSequence(CatalogTransaction transaction, CreateSequenceInfo &info) {
	auto sequence = make_uniq<SequenceCatalogEntry>(catalog, *this, info);
	return AddEntry(transaction, std::move(sequence), info.on_conflict);
}

optional_ptr<CatalogEntry> DuckSchemaEntry::CreateType(CatalogTransaction transaction, CreateTypeInfo &info) {
	auto type_entry = make_uniq<TypeCatalogEntry>(catalog, *this, info);
	return AddEntry(transaction, std::move(type_entry), info.on_conflict);
}

optional_ptr<CatalogEntry> DuckSchemaEntry::CreateView(CatalogTransaction transaction, CreateViewInfo &info) {
	auto view = make_uniq<ViewCatalogEntry>(catalog, *this, info);
	return AddEntry(transaction, std::move(view), info.on_conflict);
}

optional_ptr<CatalogEntry> DuckSchemaEntry::CreateIndex(CatalogTransaction transaction, CreateIndexInfo &info,
                                                        TableCatalogEntry &table) {
	// indexes do not require CASCADE to be dropped, they are simply always dropped along with the table
	info.dependencies.AddDependency(table, DependencyDependentFlags());

	auto index = make_uniq<DuckIndexEntry>(catalog, *this, info, table);
	auto dependencies = index->dependencies;
	return AddEntryInternal(transaction, std::move(index), info.on_conflict, dependencies);
}

optional_ptr<CatalogEntry> DuckSchemaEntry::CreateCollation(CatalogTransaction transaction, CreateCollationInfo &info) {
	auto collation = make_uniq<CollateCatalogEntry>(catalog, *this, info);
	collation->internal = info.internal;
	return AddEntry(transaction, std::move(collation), info.on_conflict);
}

optional_ptr<CatalogEntry> DuckSchemaEntry::CreateCoordinateSystem(CatalogTransaction transaction,
                                                                   CreateCoordinateSystemInfo &info) {
	auto coordinate_system = make_uniq<CoordinateSystemCatalogEntry>(catalog, *this, info);
	coordinate_system->internal = info.internal;
	return AddEntry(transaction, std::move(coordinate_system), info.on_conflict);
}

optional_ptr<CatalogEntry> DuckSchemaEntry::CreateTableFunction(CatalogTransaction transaction,
                                                                CreateTableFunctionInfo &info) {
	auto table_function = make_uniq<TableFunctionCatalogEntry>(catalog, *this, info);
	table_function->internal = info.internal;
	return AddEntry(transaction, std::move(table_function), info.on_conflict);
}

optional_ptr<CatalogEntry> DuckSchemaEntry::CreateCopyFunction(CatalogTransaction transaction,
                                                               CreateCopyFunctionInfo &info) {
	auto copy_function = make_uniq<CopyFunctionCatalogEntry>(catalog, *this, info);
	copy_function->internal = info.internal;
	return AddEntry(transaction, std::move(copy_function), info.on_conflict);
}

optional_ptr<CatalogEntry> DuckSchemaEntry::CreatePragmaFunction(CatalogTransaction transaction,
                                                                 CreatePragmaFunctionInfo &info) {
	auto pragma_function = make_uniq<PragmaFunctionCatalogEntry>(catalog, *this, info);
	pragma_function->internal = info.internal;
	return AddEntry(transaction, std::move(pragma_function), info.on_conflict);
}

void DuckSchemaEntry::Alter(CatalogTransaction transaction, AlterInfo &info) {
	CatalogType type = info.GetCatalogType();

	auto &set = GetCatalogSet(type);
	if (info.type == AlterType::CHANGE_OWNERSHIP) {
		if (!set.AlterOwnership(transaction, info.Cast<ChangeOwnershipInfo>())) {
			throw CatalogException("Couldn't change ownership!");
		}
	} else {
		auto &name = info.GetQualifiedName().Name();
		if (!set.AlterEntry(transaction, name, info)) {
			throw CatalogException::MissingEntry(type, name, string());
		}
	}
}

void DuckSchemaEntry::Scan(ClientContext &context, CatalogType type,
                           const std::function<void(CatalogEntry &)> &callback) {
	Scan(GetCatalogTransaction(context), type, callback);
}

void DuckSchemaEntry::Scan(CatalogTransaction transaction, CatalogType type,
                           const std::function<void(CatalogEntry &)> &callback) {
	GetCatalogSet(type).Scan(transaction, callback);
}

void DuckSchemaEntry::Scan(CatalogType type, const std::function<void(CatalogEntry &)> &callback) {
	Scan(type, VisibilityBound::AllCommitted(), callback);
}

void DuckSchemaEntry::Scan(CatalogType type, VisibilityBound bound,
                           const std::function<void(CatalogEntry &)> &callback) {
	auto &set = GetCatalogSet(type);
	set.Scan(bound, callback);
}

void DuckSchemaEntry::DropEntry(ClientContext &context, DropInfo &info) {
	if (info.type == CatalogType::TRIGGER_ENTRY) {
		throw InternalException("Triggers should be dropped through their table, not through the schema");
	}
	auto &set = GetCatalogSet(info.type);

	// first find the entry
	auto transaction = GetCatalogTransaction(context);
	auto existing_entry = set.GetEntry(transaction, info.GetQualifiedName().Name());
	if (!existing_entry) {
		throw InternalException("Failed to drop entry %s - entry could not be found", info.GetQualifiedName().Name());
	}
	if (existing_entry->type != info.type) {
		throw CatalogException("Existing object %s is of type %s, trying to drop type %s",
		                       info.GetQualifiedName().Name(), CatalogTypeToString(existing_entry->type),
		                       CatalogTypeToString(info.type));
	}

	if (info.extra_drop_info && info.extra_drop_info->info_type == ExtraDropInfoType::FUNCTION_SIGNATURE_INFO) {
		if (existing_entry->type != CatalogType::MACRO_ENTRY &&
		    existing_entry->type != CatalogType::TABLE_MACRO_ENTRY) {
			throw CatalogException("Cannot drop %s with a parameter type list: dropping a single overload is only "
			                       "supported for macros and functions",
			                       info.GetQualifiedName().Name());
		}
		if (DropMacroFunctionMember(transaction, info, existing_entry->Cast<MacroCatalogEntry>())) {
			return;
		}
		// the signature identified the last member, dropping it removes the whole entry
	}

	if (!DropEntryInternal(transaction, *existing_entry, info.GetQualifiedName().Name(), info.cascade,
	                       info.allow_drop_internal)) {
		throw InternalException("Could not drop element because of an internal error");
	}
}

bool DuckSchemaEntry::DropMacroFunctionMember(CatalogTransaction transaction, DropInfo &info,
                                              MacroCatalogEntry &existing) {
	if (existing.internal && !info.allow_drop_internal) {
		// the member rewrite is a drop+create: an internal entry's protection applies to it too
		throw CatalogException("function %s is an internal system entry: its overloads cannot be dropped",
		                       info.GetQualifiedName().Name());
	}
	auto &extra = info.extra_drop_info->Cast<ExtraDropFunctionSignatureInfo>();
	auto member_idx = existing.FindMember(extra.signature);
	if (!member_idx.IsValid()) {
		if (info.if_not_found == OnEntryNotFound::RETURN_NULL) {
			// IF EXISTS at member granularity: nothing to drop, siblings are unaffected
			return true;
		}
		string signature_text;
		for (auto &type : extra.signature) {
			if (!signature_text.empty()) {
				signature_text += ", ";
			}
			signature_text += type.ToString();
		}
		throw CatalogException("Function %s has no overload with signature (%s)", info.GetQualifiedName().Name(),
		                       signature_text);
	}
	if (existing.macros.size() == 1) {
		// dropping the last member drops the entry, which honors CASCADE like any entry drop
		return false;
	}
	if (info.cascade) {
		// dependencies are tracked per entry, not per member, so a cascade that leaves siblings
		// behind has no well-defined scope: require an explicit choice
		throw CatalogException("Cannot drop an overload of %s with CASCADE while other overloads remain: drop "
		                       "the whole entry or its dependents first",
		                       info.GetQualifiedName().Name());
	}
	// replace the entry with one that carries the remaining members
	auto new_info_base = existing.GetInfo();
	auto &new_info = new_info_base->Cast<CreateMacroInfo>();
	new_info.macros.erase(new_info.macros.begin() + NumericCast<int64_t>(member_idx.GetIndex()));
	new_info.temporary = existing.temporary;
	auto function = MacroCatalogEntry::Create(catalog, *this, new_info);
	function->internal = existing.internal;
	// the replacement is a fresh entry on purpose: the changed oid is what makes the existing
	// commit-time checks abort schedules that raced this mutation, like any macro replacement
	AddEntry(transaction, std::move(function), OnCreateConflict::REPLACE_ON_CONFLICT);
	return true;
}

void DuckSchemaEntry::OnDropEntry(CatalogTransaction transaction, CatalogEntry &entry) {
	if (!transaction.transaction) {
		return;
	}
	if (entry.type != CatalogType::TABLE_ENTRY) {
		return;
	}
	// if we have transaction local insertions for this table - clear them
	auto &table_entry = entry.Cast<TableCatalogEntry>();
	auto &local_storage = LocalStorage::Get(transaction.transaction->Cast<DuckTransaction>());
	local_storage.DropTable(table_entry.GetStorage());
}

optional_ptr<CatalogEntry> DuckSchemaEntry::LookupEntry(CatalogTransaction transaction,
                                                        const EntryLookupInfo &lookup_info) {
	return GetCatalogSet(lookup_info.GetCatalogType()).GetEntry(transaction, lookup_info.GetEntryIdentifier());
}

CatalogSet::EntryLookup DuckSchemaEntry::LookupEntryDetailed(CatalogTransaction transaction,
                                                             const EntryLookupInfo &lookup_info) {
	return GetCatalogSet(lookup_info.GetCatalogType()).GetEntryDetailed(transaction, lookup_info.GetEntryIdentifier());
}

SimilarCatalogEntry DuckSchemaEntry::GetSimilarEntry(CatalogTransaction transaction,
                                                     const EntryLookupInfo &lookup_info) {
	return GetCatalogSet(lookup_info.GetCatalogType()).SimilarEntry(transaction, lookup_info.GetEntryIdentifier());
}

CatalogSet &DuckSchemaEntry::GetCatalogSet(CatalogType type) {
	switch (type) {
	case CatalogType::SCHEMA_ENTRY:
		return schemas;
	case CatalogType::VIEW_ENTRY:
	case CatalogType::TABLE_ENTRY:
		return tables;
	case CatalogType::INDEX_ENTRY:
		return indexes;
	case CatalogType::TABLE_FUNCTION_ENTRY:
	case CatalogType::TABLE_MACRO_ENTRY:
		return table_functions;
	case CatalogType::COPY_FUNCTION_ENTRY:
		return copy_functions;
	case CatalogType::PRAGMA_FUNCTION_ENTRY:
		return pragma_functions;
	case CatalogType::AGGREGATE_FUNCTION_ENTRY:
	case CatalogType::SCALAR_FUNCTION_ENTRY:
	case CatalogType::MACRO_ENTRY:
	case CatalogType::WINDOW_FUNCTION_ENTRY:
		return functions;
	case CatalogType::SEQUENCE_ENTRY:
		return sequences;
	case CatalogType::COLLATION_ENTRY:
		return collations;
	case CatalogType::COORDINATE_SYSTEM_ENTRY:
		return coordinate_systems;
	case CatalogType::TYPE_ENTRY:
		return types;
	default:
		throw InternalException({{"catalog_type", CatalogTypeToString(type)}}, "Unsupported catalog type in schema");
	}
}

void DuckSchemaEntry::Verify(Catalog &catalog) {
	InCatalogEntry::Verify(catalog);

	schemas.Verify(catalog);
	tables.Verify(catalog);
	indexes.Verify(catalog);
	table_functions.Verify(catalog);
	copy_functions.Verify(catalog);
	pragma_functions.Verify(catalog);
	functions.Verify(catalog);
	sequences.Verify(catalog);
	collations.Verify(catalog);
	types.Verify(catalog);
}

} // namespace duckdb
