#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/logging/log_manager.hpp"
#include "duckdb/transaction/commit_state.hpp"

#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/exception/transaction_exception.hpp"
#include "duckdb/common/index_map.hpp"
#include "duckdb/common/type_visitor.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/execution/index/art/art.hpp"
#include "duckdb/function/table/table_scan.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/parser/constraints/list.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/parsed_data/comment_on_column_info.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/constraints/bound_check_constraint.hpp"
#include "duckdb/planner/constraints/bound_foreign_key_constraint.hpp"
#include "duckdb/planner/constraints/bound_not_null_constraint.hpp"
#include "duckdb/planner/constraints/bound_unique_constraint.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression_binder/alter_binder.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_update.hpp"
#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/catalog/catalog_entry/trigger_catalog_entry.hpp"
#include "duckdb/storage/storage_manager.hpp"
#include "duckdb/storage/table_storage_info.hpp"
#include "duckdb/storage/table/data_table_info.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/transaction/local_storage.hpp"
#include "duckdb/main/attached_database.hpp"

namespace duckdb {

IndexStorageInfo GetIndexInfo(const IndexConstraintType type, const bool v1_0_0_storage, unique_ptr<CreateInfo> &info,
                              const idx_t id) {
	auto &table_info = info->Cast<CreateTableInfo>();
	auto constraint_name = EnumUtil::ToString(type) + "_";
	auto name = constraint_name + table_info.GetTableName() + "_" + to_string(id);
	IndexStorageInfo index_info {Identifier(name)};
	if (!v1_0_0_storage) {
		index_info.options.emplace("v1_0_0_storage", v1_0_0_storage);
	}
	return index_info;
}

static void CheckTypeIsSupported(const LogicalType &logical_type, AttachedDatabase &db) {
	TypeVisitor::Contains(logical_type, [&](const LogicalType &type) {
		if (type.IsAggregateState()) {
			const auto storage_version = db.GetStorageManager().GetStorageVersion();

			if (storage_version < StorageVersion::V2_0_0) {
				auto required = GetStorageVersionName(StorageVersion::V2_0_0, false);
				auto current = GetStorageVersionName(storage_version, false);

				throw InvalidInputException("Aggregate state columns are not supported in storage versions prior to %s "
				                            "(database %s is using storage version %s)",
				                            required, db.GetName(), current);
			}
			return false;
		}
		switch (type.id()) {
		case LogicalTypeId::TYPE: {
			throw InvalidInputException("A table cannot be created with a 'TYPE' column");
		} break;
		case LogicalTypeId::STRUCT: {
			const auto storage_version = db.GetStorageManager().GetStorageVersion();

			if (storage_version < StorageVersion::V2_0_0 && StructType::GetChildCount(type) == 0) {
				auto required = GetStorageVersionName(StorageVersion::V2_0_0, false);
				auto current = GetStorageVersionName(storage_version, false);

				throw InvalidInputException("Empty STRUCT columns are not supported in storage versions prior to %s "
				                            "(database %s is using storage version %s)",
				                            required, db.GetName(), current);
			}
		} break;
		case LogicalTypeId::TUPLE: {
			// TUPLEs are stored as unnamed STRUCTs on disk, which older engines reject - gate creation to v2.0.0+
			// existing unnamed STRUCTs (deserialized as TUPLE while loading an older file) must stay attachable
			const auto storage_version = db.GetStorageManager().GetStorageVersion();

			if (storage_version < StorageVersion::V2_0_0 && db.GetStorageManager().IsLoaded()) {
				auto required = GetStorageVersionName(StorageVersion::V2_0_0, false);
				auto current = GetStorageVersionName(storage_version, false);

				throw InvalidInputException("TUPLE columns are not supported in storage versions prior to %s "
				                            "(database %s is using storage version %s)",
				                            required, db.GetName(), current);
			}
		} break;
		case LogicalTypeId::VARIANT: {
			const auto storage_version = db.GetStorageManager().GetStorageVersion();

			if (storage_version < Variant::VERSION_ADDED) {
				auto required = GetStorageVersionName(Variant::VERSION_ADDED, false);
				auto current = GetStorageVersionName(storage_version, false);

				throw InvalidInputException("VARIANT columns are not supported in storage versions prior to %s "
				                            "(database %s is using storage version %s)",
				                            required, db.GetName(), current);
			}
		} break;
		case LogicalTypeId::GEOMETRY: {
			const auto storage_version = db.GetStorageManager().GetStorageVersion();

			if (GeoType::HasCRS(type) && storage_version < Geometry::VERSION_ADDED) {
				auto required = GetStorageVersionName(Geometry::VERSION_ADDED, false);
				auto current = GetStorageVersionName(storage_version, false);

				// TODO: Turn this into a hard error
				auto &logger = Logger::Get(db.GetDatabase());
				logger.WriteLog(DefaultLogType::NAME, LogLevel::LOG_WARNING,
				                "GEOMETRY columns with coordinate reference system identifiers are not supported in "
				                "storage versions prior "
				                "to %s (database %s is using storage version %s). CRS will not be persisted.",
				                required, db.GetName(), current);
			}
		} break;
		case LogicalTypeId::GEOGRAPHY: {
			const auto storage_version = db.GetStorageManager().GetStorageVersion();
			if (storage_version < Geometry::GEOGRAPHY_VERSION_ADDED) {
				auto required = GetStorageVersionName(Geometry::GEOGRAPHY_VERSION_ADDED, false);
				auto current = GetStorageVersionName(storage_version, false);
				throw NotImplementedException("GEOGRAPHY columns are not supported in storage versions prior to %s "
				                              "(database %s is using storage version %s)",
				                              required, db.GetName(), current);
			}
		} break;
		default:
			break;
		}
		return false;
	});
}

static void SetAlterDependencies(ClientContext &context, BoundCreateTableInfo &info, AlterInfo &alter_info) {
	// The foreign key dependency on the referenced parent table is registered when the table is created
	// (DuckSchemaEntry::CreateTable), but it is not part of the stored create info - re-add it here,
	// otherwise ALTER silently drops the dependency edge and allows the parent to be renamed or dropped.
	for (auto &constraint : info.Base().constraints) {
		if (constraint->type != ConstraintType::FOREIGN_KEY) {
			continue;
		}
		auto &fk = constraint->Cast<ForeignKeyConstraint>();
		if (fk.info.type != ForeignKeyType::FK_TYPE_FOREIGN_KEY_TABLE) {
			continue;
		}
		// a foreign key can only reference a table in the same schema
		auto txn = info.schema.catalog.GetCatalogTransaction(context);
		auto parent = info.schema.GetEntry(txn, CatalogType::TABLE_ENTRY, fk.info.table);
		if (parent) {
			info.Base().dependencies.AddDependency(*parent);
		}
	}
	alter_info.new_dependencies = make_uniq<LogicalDependencyList>(info.Base().dependencies);
}

virtual_column_map_t DuckTableEntry::GetVirtualColumns() const {
	virtual_column_map_t virtual_columns;
	virtual_columns.insert(make_pair(COLUMN_IDENTIFIER_ROW_ID, TableColumn("rowid", LogicalType::ROW_TYPE)));
	virtual_columns.insert(make_pair(COLUMN_IDENTIFIER_ROW_NUMBER, TableColumn("row_number", LogicalType::BIGINT)));
	return virtual_columns;
}

DuckTableEntry::DuckTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, BoundCreateTableInfo &info,
                               shared_ptr<DataTable> inherited_storage, shared_ptr<CatalogSet> inherited_triggers)
    : TableCatalogEntry(catalog, schema, info.Base()), columns(std::move(info.Base().columns)),
      storage(std::move(inherited_storage)), triggers(std::move(inherited_triggers)),
      column_dependency_manager(std::move(info.column_dependency_manager)) {
	if (!triggers) {
		triggers = make_shared_ptr<CatalogSet>(catalog);
	}
	if (storage) {
		if (!info.indexes.empty()) {
			storage->SetIndexStorageInfo(std::move(info.indexes));
		}
		return;
	}

	// create the physical storage
	vector<ColumnDefinition> column_defs;
	for (auto &col_def : columns.Physical()) {
		CheckTypeIsSupported(col_def.Type(), catalog.GetAttached());

		column_defs.push_back(col_def.Copy());
	}
	storage = make_shared_ptr<DataTable>(catalog.GetAttached(), StorageManager::Get(catalog).GetTableIOManager(&info),
	                                     schema.GetSchemaPath(), name, std::move(column_defs), std::move(info.data));

	// Create the unique indexes for the UNIQUE, PRIMARY KEY, and FOREIGN KEY constraints.
	idx_t indexes_idx = 0;
	for (idx_t i = 0; i < constraints.size(); i++) {
		auto &constraint = constraints[i];
		if (constraint->type == ConstraintType::UNIQUE) {
			// UNIQUE constraint: Create a unique index.
			auto &unique = constraint->Cast<UniqueConstraint>();
			auto index_oid = DatabaseManager::Get(catalog.GetDatabase()).NextOid();
			constraint->SetBackingIndexOid(index_oid);
			IndexConstraintType constraint_type = IndexConstraintType::UNIQUE;
			if (unique.is_primary_key) {
				constraint_type = IndexConstraintType::PRIMARY;
			}

			auto column_indexes = unique.GetLogicalIndexes(columns);
			if (info.indexes.empty()) {
				auto index_info = GetIndexInfo(constraint_type, false, info.base, i);
				storage->AddIndex(columns, column_indexes, constraint_type, std::move(index_info), index_oid,
				                  unique.check_mode);
				continue;
			}

			// We read the index from an old storage version applying a dummy name.
			auto index_storage_info = std::move(info.indexes[indexes_idx++]);
			if (index_storage_info.name.empty()) {
				auto name_info = GetIndexInfo(constraint_type, true, info.base, i);
				index_storage_info.name = name_info.name;
			}

			storage->AddIndex(columns, column_indexes, constraint_type, std::move(index_storage_info), index_oid,
			                  unique.check_mode);
			continue;
		}

		if (constraint->type == ConstraintType::FOREIGN_KEY) {
			// Create a FOREIGN KEY index.
			auto &bfk = constraint->Cast<ForeignKeyConstraint>();
			if (bfk.info.type == ForeignKeyType::FK_TYPE_FOREIGN_KEY_TABLE ||
			    bfk.info.type == ForeignKeyType::FK_TYPE_SELF_REFERENCE_TABLE) {
				auto index_oid = DatabaseManager::Get(catalog.GetDatabase()).NextOid();
				constraint->SetBackingIndexOid(index_oid);
				vector<LogicalIndex> column_indexes;
				for (const auto &physical_index : bfk.info.fk_keys) {
					auto &col = columns.GetColumn(physical_index);
					column_indexes.push_back(col.Logical());
				}

				if (info.indexes.empty()) {
					auto constraint_type = IndexConstraintType::FOREIGN;
					auto index_info = GetIndexInfo(constraint_type, false, info.base, i);
					storage->AddIndex(columns, column_indexes, constraint_type, std::move(index_info), index_oid,
					                  ConstraintCheckMode::DEFAULT);
					continue;
				}

				// We read the index from an old storage version applying a dummy name.
				auto index_storage_info = std::move(info.indexes[indexes_idx++]);
				if (index_storage_info.name.empty()) {
					auto name_info = GetIndexInfo(IndexConstraintType::FOREIGN, true, info.base, i);
					index_storage_info.name = name_info.name;
				}

				storage->AddIndex(columns, column_indexes, IndexConstraintType::FOREIGN, std::move(index_storage_info),
				                  index_oid, ConstraintCheckMode::DEFAULT);
			}
		}
	}

	// Move any remaining unused IndexStorageInfos to storage.
	// These are non-constraint indexes that are still unbound at this point.
	vector<IndexStorageInfo> remaining_indexes;
	while (indexes_idx < info.indexes.size()) {
		remaining_indexes.push_back(std::move(info.indexes[indexes_idx++]));
	}
	if (!remaining_indexes.empty()) {
		storage->SetIndexStorageInfo(std::move(remaining_indexes));
	}
}

const ColumnList &DuckTableEntry::GetColumns() const {
	return columns;
}

unique_ptr<BaseStatistics> DuckTableEntry::GetStatistics(ClientContext &context, const StorageIndex &column_id) {
	// Get the committed statistics
	auto stats = storage->GetStatistics(context, column_id);
	if (!stats) {
		return nullptr;
	}
	// Merge the transaction-local statistics in so the result describes the transaction-visible data.
	auto &local_storage = LocalStorage::Get(context, ParentCatalog());
	auto local_table_storage = local_storage.GetStorage(*storage);
	if (!local_table_storage) {
		return stats;
	}
	auto local_stats = local_table_storage->GetCollection().CopyStats(column_id);
	if (!local_stats) {
		return stats;
	}
	stats->Merge(*local_stats);
	return stats;
}

unique_ptr<BaseStatistics> DuckTableEntry::GetStatistics(ClientContext &context, column_t column_id) {
	if (IsVirtualColumn(column_id)) {
		// no stats for virtual columns (yet?)
		return nullptr;
	}
	auto &column = columns.GetColumn(LogicalIndex(column_id));
	if (column.Generated()) {
		return nullptr;
	}
	auto storage_index = GetStorageIndex(ColumnIndex(column_id));
	return GetStatistics(context, storage_index);
}

unique_ptr<BlockingSample> DuckTableEntry::GetSample() {
	return storage->GetSample();
}

//! The index of the column name in a column reference of a table expression - the names before it qualify the table,
//! the names after it are struct fields
static idx_t ColumnNameIndex(const ColumnRefExpression &colref, const Identifier &table_name) {
	auto &names = colref.ColumnNames();
	return names.size() > 1 && names[0] == table_name ? 1 : 0;
}

unique_ptr<CatalogEntry> DuckTableEntry::AlterEntry(CatalogTransaction transaction, AlterInfo &info) {
	if (transaction.HasContext()) {
		return AlterEntry(transaction.GetContext(), info);
	}
	if (info.type != AlterType::ALTER_TABLE) {
		return CatalogEntry::AlterEntry(transaction, info);
	}

	auto &table_info = info.Cast<AlterTableInfo>();
	if (table_info.alter_table_type != AlterTableType::FOREIGN_KEY_CONSTRAINT) {
		return CatalogEntry::AlterEntry(transaction, info);
	}

	auto &foreign_key_constraint_info = table_info.Cast<AlterForeignKeyInfo>();
	if (foreign_key_constraint_info.type != AlterForeignKeyType::AFT_ADD) {
		return CatalogEntry::AlterEntry(transaction, info);
	}

	// We add foreign key constraints without a client context during checkpoint loading.
	return AddForeignKeyConstraint(foreign_key_constraint_info);
}

unique_ptr<CatalogEntry> DuckTableEntry::AlterEntry(ClientContext &context, AlterInfo &info) {
	D_ASSERT(!internal);

	// Column comments have a special alter type
	if (info.type == AlterType::SET_COLUMN_COMMENT) {
		auto &comment_on_column_info = info.Cast<SetColumnCommentInfo>();
		return SetColumnComment(context, comment_on_column_info);
	}

	if (info.type != AlterType::ALTER_TABLE) {
		throw CatalogException("Can only modify table with ALTER TABLE statement");
	}
	auto &table_info = info.Cast<AlterTableInfo>();
	switch (table_info.alter_table_type) {
	case AlterTableType::RENAME_COLUMN: {
		auto &rename_info = table_info.Cast<RenameColumnInfo>();
		return RenameColumn(context, rename_info);
	}
	case AlterTableType::RENAME_FIELD: {
		auto &rename_info = table_info.Cast<RenameFieldInfo>();
		return RenameField(context, rename_info);
	}
	case AlterTableType::RENAME_TABLE: {
		auto &rename_info = table_info.Cast<RenameTableInfo>();
		auto copied_table = Copy(context);
		copied_table->name = rename_info.new_table_name;
		// CHECK constraints can qualify columns with the table name
		for (auto &constraint : copied_table->Cast<DuckTableEntry>().constraints) {
			if (constraint->type != ConstraintType::CHECK) {
				continue;
			}
			auto &check = constraint->Cast<CheckConstraint>();
			ParsedExpressionIterator::VisitExpressionMutable<ColumnRefExpression>(
			    *check.expression, [&](ColumnRefExpression &colref) {
				    if (ColumnNameIndex(colref, name) == 1) {
					    colref.ColumnNamesMutable()[0] = rename_info.new_table_name;
				    }
			    });
		}
		return copied_table;
	}
	case AlterTableType::ADD_COLUMN: {
		auto &add_info = table_info.Cast<AddColumnInfo>();
		return AddColumn(context, add_info);
	}
	case AlterTableType::ADD_FIELD: {
		auto &add_info = table_info.Cast<AddFieldInfo>();
		return AddField(context, add_info);
	}
	case AlterTableType::REMOVE_COLUMN: {
		auto &remove_info = table_info.Cast<RemoveColumnInfo>();
		return RemoveColumn(context, remove_info);
	}
	case AlterTableType::REMOVE_FIELD: {
		auto &remove_info = table_info.Cast<RemoveFieldInfo>();
		return RemoveField(context, remove_info);
	}
	case AlterTableType::SET_DEFAULT: {
		auto &set_default_info = table_info.Cast<SetDefaultInfo>();
		return SetDefault(context, set_default_info);
	}
	case AlterTableType::ALTER_COLUMN_TYPE: {
		auto &change_type_info = table_info.Cast<ChangeColumnTypeInfo>();
		return ChangeColumnType(context, change_type_info, AlterTableType::ALTER_COLUMN_TYPE);
	}
	case AlterTableType::FOREIGN_KEY_CONSTRAINT: {
		auto &foreign_key_constraint_info = table_info.Cast<AlterForeignKeyInfo>();
		if (foreign_key_constraint_info.type == AlterForeignKeyType::AFT_ADD) {
			return AddForeignKeyConstraint(foreign_key_constraint_info);
		} else {
			return DropForeignKeyConstraint(context, foreign_key_constraint_info);
		}
	}
	case AlterTableType::SET_NOT_NULL: {
		auto &set_not_null_info = table_info.Cast<SetNotNullInfo>();
		return SetNotNull(context, set_not_null_info);
	}
	case AlterTableType::DROP_NOT_NULL: {
		auto &drop_not_null_info = table_info.Cast<DropNotNullInfo>();
		return DropNotNull(context, drop_not_null_info);
	}
	case AlterTableType::ADD_CONSTRAINT: {
		auto &add_constraint_info = table_info.Cast<AddConstraintInfo>();
		return AddConstraint(context, add_constraint_info);
	}
	case AlterTableType::SET_PARTITIONED_BY:
		throw NotImplementedException("SET PARTITIONED BY is not supported for DuckDB tables");
	case AlterTableType::SET_SORTED_BY:
		throw NotImplementedException("SET SORTED BY is not supported for DuckDB tables");
	case AlterTableType::SET_TABLE_OPTIONS:
		throw NotImplementedException("SET (<options>) is not supported for DuckDB tables");
	case AlterTableType::RESET_TABLE_OPTIONS: {
		throw NotImplementedException("RESET (<options>) is not supported for DuckDB tables");
	}
	default:
		throw InternalException("Unrecognized alter table type!");
	}
}

void DuckTableEntry::UndoAlter(ClientContext &context, AlterInfo &info) {
	D_ASSERT(!internal);
	D_ASSERT(info.type == AlterType::ALTER_TABLE);
	auto &table_info = info.Cast<AlterTableInfo>();
	switch (table_info.alter_table_type) {
	case AlterTableType::RENAME_TABLE: {
		storage->SetTableName(this->name);
		break;
	default:
		break;
	}
	}
}

static void RenameExpression(ParsedExpression &root_expr, RenameColumnInfo &info, const Identifier &table_name) {
	ParsedExpressionIterator::VisitExpressionMutable<ColumnRefExpression>(root_expr, [&](ColumnRefExpression &colref) {
		auto column_idx = ColumnNameIndex(colref, table_name);
		if (colref.ColumnNames()[column_idx] == info.old_name) {
			colref.ColumnNamesMutable()[column_idx] = info.new_name;
		}
	});
}

//! Remaps the default of a struct column with the mapping and the field defaults of the remap_struct call
static unique_ptr<ParsedExpression> RemapStructDefault(unique_ptr<ParsedExpression> default_value,
                                                       const LogicalType &new_type, ChangeColumnTypeInfo &info) {
	D_ASSERT(info.expression);
	D_ASSERT(info.expression->GetExpressionClass() == ExpressionClass::FUNCTION);
	auto &function = info.expression->Cast<FunctionExpression>();
	D_ASSERT(function.FunctionName() == "remap_struct");
	auto &arguments = function.GetArguments();
	D_ASSERT(arguments.size() == 4);
	vector<unique_ptr<ParsedExpression>> children;
	children.push_back(std::move(default_value));
	children.push_back(ConstantExpression::FromValue(Value(new_type)));
	children.push_back(arguments[2].GetExpression().Copy());
	children.push_back(arguments[3].GetExpression().Copy());
	return make_uniq<FunctionExpression>("remap_struct", std::move(children));
}

unique_ptr<CatalogEntry> DuckTableEntry::RenameColumn(ClientContext &context, RenameColumnInfo &info) {
	auto rename_idx = GetColumnIndex(info.old_name);
	if (rename_idx.index == COLUMN_IDENTIFIER_ROW_ID) {
		throw CatalogException("Cannot rename rowid column");
	}
	auto create_info = make_uniq<CreateTableInfo>(schema, name);
	create_info->temporary = temporary;
	create_info->comment = comment;
	create_info->tags = tags;
	for (auto &col : columns.Logical()) {
		auto copy = col.Copy();
		if (rename_idx == col.Logical()) {
			copy.SetName(info.new_name);
		}
		if (col.Generated() && column_dependency_manager.IsDependencyOf(col.Logical(), rename_idx)) {
			RenameExpression(copy.GeneratedExpressionMutable(), info, name);
		}
		create_info->columns.AddColumn(std::move(copy));
	}
	for (idx_t c_idx = 0; c_idx < constraints.size(); c_idx++) {
		auto copy = constraints[c_idx]->Copy();
		switch (copy->type) {
		case ConstraintType::NOT_NULL:
			// NOT NULL constraint: no adjustments necessary
			break;
		case ConstraintType::CHECK: {
			// CHECK constraint: need to rename column references that refer to the renamed column
			auto &check = copy->Cast<CheckConstraint>();
			RenameExpression(*check.expression, info, name);
			break;
		}
		case ConstraintType::UNIQUE: {
			// UNIQUE constraint: possibly need to rename columns
			auto &unique = copy->Cast<UniqueConstraint>();
			for (auto &column_name : unique.GetColumnNamesMutable()) {
				if (column_name == info.old_name) {
					column_name = info.new_name;
				}
			}
			break;
		}
		case ConstraintType::FOREIGN_KEY: {
			// FOREIGN KEY constraint: possibly need to rename columns
			auto &fk = copy->Cast<ForeignKeyConstraint>();
			vector<Identifier> columns = fk.pk_columns;
			if (fk.info.type == ForeignKeyType::FK_TYPE_FOREIGN_KEY_TABLE) {
				columns = fk.fk_columns;
			} else if (fk.info.type == ForeignKeyType::FK_TYPE_SELF_REFERENCE_TABLE) {
				for (idx_t i = 0; i < fk.fk_columns.size(); i++) {
					columns.push_back(fk.fk_columns[i]);
				}
			}
			for (idx_t i = 0; i < columns.size(); i++) {
				if (columns[i] == info.old_name) {
					throw CatalogException(
					    "Cannot rename column %s because this is involved in the foreign key constraint",
					    info.old_name);
				}
			}
			break;
		}
		default:
			throw InternalException("Unsupported constraint for entry!");
		}
		create_info->constraints.push_back(std::move(copy));
	}
	auto binder = Binder::CreateBinder(context);
	auto bound_create_info = binder->BindCreateTableInfo(std::move(create_info), schema, info.bind_mode);
	SetAlterDependencies(context, *bound_create_info, info);

	// Update any UPDATE OF triggers whose column list references the renamed column.
	// Also detect concurrent uncommitted (or recently-committed) triggers that reference the same
	// column: the snapshot scan cannot see them, so we raise a write-write conflict so the caller
	// retries after the concurrent transaction completes.
	auto txn = catalog.GetCatalogTransaction(context);
	vector<Identifier> triggers_to_update;
	triggers->ScanWithConflictDetection(
	    txn,
	    [&](CatalogEntry &raw_entry) {
		    auto &trig = raw_entry.Cast<TriggerCatalogEntry>();
		    for (const auto &col : trig.columns) {
			    if (col == info.old_name) {
				    triggers_to_update.push_back(trig.name);
				    break;
			    }
		    }
	    },
	    [&](CatalogEntry &concurrent_entry) {
		    if (concurrent_entry.type != CatalogType::TRIGGER_ENTRY || concurrent_entry.deleted) {
			    return;
		    }
		    auto &trig = concurrent_entry.Cast<TriggerCatalogEntry>();
		    for (const auto &col : trig.columns) {
			    if (col == info.old_name) {
				    throw TransactionException("Catalog write-write conflict on alter with %s: trigger %s "
				                               "references column %s which is being renamed",
				                               name, trig.name, info.old_name);
			    }
		    }
	    });
	// Use a copy of info without new_dependencies so AlterObject does not
	// replace the trigger's own dependency edges with the table's dep list.
	auto trigger_alter_info = info.Copy();
	for (const auto &trigger_name : triggers_to_update) {
		triggers->AlterEntry(txn, trigger_name, *trigger_alter_info);
	}

	return make_uniq<DuckTableEntry>(catalog, schema, *bound_create_info, storage, triggers);
}

unique_ptr<CatalogEntry> DuckTableEntry::AddColumn(ClientContext &context, AddColumnInfo &info) {
	auto col_name = info.new_column.GetName();

	// We're checking for the opposite condition (ADD COLUMN IF _NOT_ EXISTS ...).
	if (info.if_column_not_exists && ColumnExists(col_name)) {
		return nullptr;
	}

	auto create_info = make_uniq<CreateTableInfo>(schema, name);
	create_info->temporary = temporary;
	create_info->comment = comment;
	create_info->tags = tags;

	for (auto &col : columns.Logical()) {
		create_info->columns.AddColumn(col.Copy());
	}
	for (auto &constraint : constraints) {
		create_info->constraints.push_back(constraint->Copy());
	}

	auto binder = Binder::CreateBinder(context);
	binder->SetSearchPath(catalog, schema.GetSchemaPath());
	binder->BindLogicalType(info.new_column.TypeMutable());

	// Check if type is supported in this database version
	CheckTypeIsSupported(info.new_column.GetType(), catalog.GetAttached());

	info.new_column.SetOid(columns.LogicalColumnCount());
	info.new_column.SetStorageOid(columns.PhysicalColumnCount());
	auto col = info.new_column.Copy();

	create_info->columns.AddColumn(std::move(col));

	vector<unique_ptr<Expression>> bound_defaults;
	auto bound_create_info =
	    binder->BindCreateTableInfo(std::move(create_info), schema, bound_defaults, info.bind_mode);
	if (info.bind_mode == AlterBindMode::SKIP_BINDING) {
		// When replaying WAL, only Bind DEFAULT for added Column
		auto &catalog_name = schema.ParentCatalog().GetName();
		binder->BindDefaultValue(info.new_column, bound_defaults, catalog_name.GetIdentifierName(),
		                         schema.GetSchemaPath());
	}
	SetAlterDependencies(context, *bound_create_info, info);
	auto new_storage = make_shared_ptr<DataTable>(context, *storage, info.new_column, *bound_defaults.back());
	return make_uniq<DuckTableEntry>(catalog, schema, *bound_create_info, new_storage, triggers);
}

struct StructMappingInfo {
	LogicalType new_type;
	unique_ptr<ParsedExpression> default_value;
	ErrorData error;
};

unique_ptr<ParsedExpression> PackExpression(unique_ptr<ParsedExpression> expr, Identifier name) {
	vector<FunctionArgument> children;
	children.emplace_back(std::move(name), std::move(expr));
	return make_uniq<FunctionExpression>("struct_pack", std::move(children));
}

static child_list_t<LogicalType> GetChildList(const LogicalType &type) {
	child_list_t<LogicalType> child_types;
	switch (type.id()) {
	case LogicalTypeId::LIST: {
		child_types.emplace_back("list", ListType::GetChildType(type));
		break;
	}
	case LogicalTypeId::MAP: {
		child_types.emplace_back("key", MapType::KeyType(type));
		child_types.emplace_back("value", MapType::ValueType(type));
		break;
	}
	case LogicalTypeId::STRUCT: {
		child_types = StructType::GetChildTypes(type);
		break;
	}
	default:
		throw BinderException("Can't ConstructMapping for type '%s'", type.ToString());
	}
	return child_types;
}

Value ConstructMapping(const Identifier &name, const LogicalType &type) {
	if (!type.IsNested()) {
		return Value(name);
	}
	child_list_t<Value> child_mapping;
	auto child_types = GetChildList(type);
	for (auto &entry : child_types) {
		auto mapping_value = ConstructMapping(entry.first, entry.second);
		if (entry.second.IsNested()) {
			vector<Value> child_values;
			child_values.push_back(Value(entry.first));
			child_values.push_back(std::move(mapping_value));
			mapping_value = Value::TUPLE(std::move(child_values));
		}
		child_mapping.emplace_back(entry.first, std::move(mapping_value));
	}
	return Value::STRUCT(std::move(child_mapping));
}

StructMappingInfo AddFieldToStruct(const LogicalType &type, const vector<Identifier> &column_path,
                                   const ColumnDefinition &new_field, idx_t depth = 0) {
	if (!type.IsNested()) {
		throw BinderException("Column %s is not a nested type, ADD COLUMN can only be used on nested types",
		                      column_path[depth]);
	}

	StructMappingInfo result;
	auto child_list = GetChildList(type);
	auto &current_component = column_path[depth];
	bool last_entry = depth + 1 == column_path.size();

	if (last_entry) {
		if (type.id() != LogicalTypeId::STRUCT) {
			throw BinderException("Column %s is not a struct - ALTER TABLE can only add fields to structs",
			                      current_component);
		}
		// root path - we are adding at this level
		// check if a field with this name already exists
		for (auto &entry : child_list) {
			if (entry.first == new_field.Name()) {
				// already exists!
				result.error = ErrorData(CatalogException("Duplicate field %s - field already exists in struct %s",
				                                          new_field.Name(), current_component));
				return result;
			}
		}
		// add the new type
		child_list.emplace_back(new_field.Name(), new_field.Type());
		result.new_type = LogicalType::STRUCT(std::move(child_list));
		// set the default value
		unique_ptr<ParsedExpression> default_value;
		if (new_field.HasDefaultValue()) {
			default_value = new_field.DefaultValue().Copy();
		} else {
			default_value = ConstantExpression::FromValue(Value(new_field.Type()));
		}
		result.default_value = PackExpression(std::move(default_value), new_field.Name());
		return result;
	}

	// not the root path - we need to recurse
	auto &next_component = column_path[depth + 1];
	bool found = false;
	for (auto &entry : child_list) {
		if ((type.id() == LogicalTypeId::LIST && next_component == "element") || entry.first == next_component) {
			// found the entry - recurse
			auto child_res = AddFieldToStruct(entry.second, column_path, new_field, depth + 1);
			if (child_res.error.HasError()) {
				return child_res;
			}
			entry.second = std::move(child_res.new_type);
			if (type.id() == LogicalTypeId::LIST) {
				result.default_value = PackExpression(std::move(child_res.default_value), "list");
			} else {
				result.default_value = PackExpression(std::move(child_res.default_value), entry.first);
			}
			found = true;
			break;
		}
	}
	if (!found) {
		throw BinderException("Sub-field %s does not exist in column %s", next_component, column_path[depth]);
	}
	result.new_type = LogicalType::ConstructNestedType(type, std::move(child_list));
	return result;
}

unique_ptr<CatalogEntry> DuckTableEntry::AddField(ClientContext &context, AddFieldInfo &info) {
	// follow the path - the parent column must exist regardless of IF NOT EXISTS (which only applies to the new field)
	auto col_idx = GetColumnIndex(info.column_path[0], false);
	auto &col = GetColumn(col_idx);
	auto res = AddFieldToStruct(col.Type(), info.column_path, info.new_field);
	if (res.error.HasError()) {
		if (!info.if_field_not_exists) {
			res.error.Throw();
		}
		return nullptr;
	}

	// construct the struct remapping expression
	vector<unique_ptr<ParsedExpression>> children;
	children.push_back(make_uniq<ColumnRefExpression>(info.column_path[0]));
	children.push_back(ConstantExpression::FromValue(Value(res.new_type)));
	children.push_back(ConstantExpression::FromValue(ConstructMapping(col.Name(), col.Type())));
	D_ASSERT(res.default_value);
	children.push_back(std::move(res.default_value));

	auto function = make_uniq<FunctionExpression>("remap_struct", std::move(children));

	ChangeColumnTypeInfo change_column_type(info.GetAlterEntryData(), vector<Identifier> {info.column_path[0]},
	                                        std::move(res.new_type), std::move(function));
	return ChangeColumnType(context, change_column_type, AlterTableType::ADD_FIELD);
}

void DuckTableEntry::UpdateConstraintsOnColumnDrop(const LogicalIndex &removed_index,
                                                   const vector<LogicalIndex> &adjusted_indices,
                                                   const RemoveColumnInfo &info, CreateTableInfo &create_info,
                                                   const vector<unique_ptr<BoundConstraint>> &bound_constraints,
                                                   bool is_generated) {
	// handle constraints for the new table
	D_ASSERT(constraints.size() == bound_constraints.size());
	for (idx_t constr_idx = 0; constr_idx < constraints.size(); constr_idx++) {
		auto &constraint = constraints[constr_idx];
		auto &bound_constraint = bound_constraints[constr_idx];
		switch (constraint->type) {
		case ConstraintType::NOT_NULL: {
			auto &not_null_constraint = bound_constraint->Cast<BoundNotNullConstraint>();
			auto not_null_index = columns.PhysicalToLogical(not_null_constraint.index);
			if (not_null_index != removed_index) {
				// the constraint is not about this column: we need to copy it
				// we might need to shift the index back by one though, to account for the removed column
				auto new_index = adjusted_indices[not_null_index.index];
				create_info.constraints.push_back(make_uniq<NotNullConstraint>(new_index));
			}
			break;
		}
		case ConstraintType::CHECK: {
			// Generated columns can not be part of an index
			// CHECK constraint
			auto &bound_check = bound_constraint->Cast<BoundCheckConstraint>();
			// check if the removed column is part of the check constraint
			if (is_generated) {
				// generated columns can not be referenced by constraints, we can just add the constraint back
				create_info.constraints.push_back(constraint->Copy());
				break;
			}
			auto physical_index = columns.LogicalToPhysical(removed_index);
			if (bound_check.bound_columns.find(physical_index) != bound_check.bound_columns.end()) {
				if (bound_check.bound_columns.size() > 1) {
					// CHECK constraint that concerns mult
					throw CatalogException(
					    "Cannot drop column %s because there is a CHECK constraint that depends on it",
					    info.removed_column);
				} else {
					// CHECK constraint that ONLY concerns this column, strip the constraint
				}
			} else {
				// check constraint does not concern the removed column: simply re-add it
				create_info.constraints.push_back(constraint->Copy());
			}
			break;
		}
		case ConstraintType::UNIQUE: {
			auto copy = constraint->Copy();
			auto &unique = copy->Cast<UniqueConstraint>();
			if (unique.HasIndex()) {
				// Single-column UNIQUE constraint
				if (unique.GetIndex() == removed_index) {
					string constraint_type = unique.IsPrimaryKey() ? "PRIMARY KEY" : "UNIQUE";
					throw CatalogException("Cannot drop column %s because there is a %s constraint that depends on it",
					                       info.removed_column, constraint_type);
				}
				unique.SetIndex(adjusted_indices[unique.GetIndex().index]);
			} else {
				// Multi-column UNIQUE constraint - check if any column matches the one being dropped
				for (const auto &col_name : unique.GetColumnNames()) {
					if (col_name == info.removed_column) {
						string constraint_kind = unique.IsPrimaryKey() ? "primary key" : "unique";
						throw CatalogException("Cannot drop column %s because it is referenced in %s constraint %s",
						                       info.removed_column, constraint_kind, unique.ToString());
					}
				}
			}
			create_info.constraints.push_back(std::move(copy));
			break;
		}
		case ConstraintType::FOREIGN_KEY: {
			auto copy = constraint->Copy();
			auto &fk = copy->Cast<ForeignKeyConstraint>();
			vector<Identifier> columns = fk.pk_columns;
			if (fk.info.type == ForeignKeyType::FK_TYPE_FOREIGN_KEY_TABLE) {
				columns = fk.fk_columns;
			} else if (fk.info.type == ForeignKeyType::FK_TYPE_SELF_REFERENCE_TABLE) {
				for (idx_t i = 0; i < fk.fk_columns.size(); i++) {
					columns.push_back(fk.fk_columns[i]);
				}
			}
			for (idx_t i = 0; i < columns.size(); i++) {
				if (columns[i] == info.removed_column) {
					throw CatalogException(
					    "Cannot drop column %s because there is a FOREIGN KEY constraint that depends on it",
					    info.removed_column);
				}
			}
			create_info.constraints.push_back(std::move(copy));
			break;
		}
		default:
			throw InternalException("Unsupported constraint for entry!");
		}
	}
}

unique_ptr<CatalogEntry> DuckTableEntry::RemoveColumn(ClientContext &context, RemoveColumnInfo &info) {
	auto removed_index = GetColumnIndex(info.removed_column, info.if_column_exists);
	if (!removed_index.IsValid()) {
		if (!info.if_column_exists) {
			throw CatalogException("Cannot drop column: rowid column cannot be dropped");
		}
		return nullptr;
	}

	auto create_info = make_uniq<CreateTableInfo>(schema, name);
	create_info->temporary = temporary;
	create_info->comment = comment;
	create_info->tags = tags;

	logical_index_set_t removed_columns;
	if (column_dependency_manager.HasDependents(removed_index)) {
		removed_columns = column_dependency_manager.GetDependents(removed_index);
	}
	if (!removed_columns.empty() && !info.cascade) {
		throw CatalogException("Cannot drop column: column is a dependency of 1 or more generated column(s)");
	}
	bool dropped_column_is_generated = columns.GetColumn(LogicalIndex(removed_index)).Generated();
	for (auto &col : columns.Logical()) {
		if (col.Logical() == removed_index || removed_columns.count(col.Logical())) {
			continue;
		}
		create_info->columns.AddColumn(col.Copy());
	}
	if (create_info->columns.empty()) {
		throw CatalogException("Cannot drop column: table only has one column remaining!");
	}
	// operate on a copy: this is the live catalog entry, and mutating it here races with concurrent ALTERs and leaves
	// the dependency map corrupted if a later step throws and the ALTER is rolled back
	auto column_dependencies = column_dependency_manager.Copy();
	auto adjusted_indices = column_dependencies.RemoveColumn(removed_index, columns.LogicalColumnCount());

	auto binder = Binder::CreateBinder(context);
	auto bound_constraints = binder->BindConstraints(constraints, name, columns);

	UpdateConstraintsOnColumnDrop(removed_index, adjusted_indices, info, *create_info, bound_constraints,
	                              dropped_column_is_generated);

	auto bound_create_info = binder->BindCreateTableInfo(std::move(create_info), schema, info.bind_mode);
	SetAlterDependencies(context, *bound_create_info, info);
	if (columns.GetColumn(LogicalIndex(removed_index)).Generated()) {
		return make_uniq<DuckTableEntry>(catalog, schema, *bound_create_info, storage, triggers);
	}
	auto new_storage =
	    make_shared_ptr<DataTable>(context, *storage, columns.LogicalToPhysical(LogicalIndex(removed_index)).index);
	return make_uniq<DuckTableEntry>(catalog, schema, *bound_create_info, new_storage, triggers);
}

struct DroppedFieldMapping {
	Value mapping;
	LogicalType new_type;
	ErrorData error;
};

DroppedFieldMapping DropFieldFromStruct(const LogicalType &type, const vector<Identifier> &column_path, idx_t depth) {
	if (!type.IsNested()) {
		throw CatalogException("Cannot drop field from column %s - not a nested type", column_path[0]);
	}
	auto &dropped_entry = column_path[depth];
	bool last_entry = depth + 1 == column_path.size();
	bool found = false;
	DroppedFieldMapping result;
	child_list_t<Value> child_mapping;
	child_list_t<LogicalType> new_type_children;
	auto child_types = GetChildList(type);

	for (auto &entry : child_types) {
		Value mapping_value;
		LogicalType type_value;
		if ((type.id() == LogicalTypeId::LIST && dropped_entry == "element") || entry.first == dropped_entry) {
			// this is the entry we are dropping
			found = true;
			if (last_entry) {
				if (type.id() != LogicalTypeId::STRUCT) {
					throw CatalogException("Cannot drop field %s from column %s - it's not a struct",
					                       column_path.back(), column_path.front());
				}
				// we are dropping this entry in its entirety - just skip
				if (child_types.size() == 1) {
					throw CatalogException("Cannot drop field %s from column %s - it is the last field of the struct",
					                       column_path.back().GetIdentifierName(),
					                       column_path.front().GetIdentifierName());
				}
				continue;
			} else {
				// we are dropping a field in this entry - recurse
				auto child_result = DropFieldFromStruct(entry.second, column_path, depth + 1);
				if (child_result.error.HasError()) {
					// bubble up error
					return child_result;
				}
				mapping_value = std::move(child_result.mapping);
				type_value = std::move(child_result.new_type);
			}
		} else {
			// we are not adjusting this entry - copy the type and create a straightforward mapping
			mapping_value = ConstructMapping(entry.first, entry.second);
			type_value = entry.second;
		}

		if (entry.second.IsNested()) {
			vector<Value> child_values;
			child_values.push_back(Value(entry.first));
			child_values.push_back(std::move(mapping_value));
			mapping_value = Value::TUPLE(std::move(child_values));
		}
		child_mapping.emplace_back(entry.first, std::move(mapping_value));
		new_type_children.emplace_back(entry.first, type_value);
	}

	if (!found) {
		result.error = ErrorData(CatalogException("Cannot drop field %s - it does not exist", dropped_entry));
	} else {
		result.mapping = Value::STRUCT(std::move(child_mapping));
		result.new_type = LogicalType::ConstructNestedType(type, std::move(new_type_children));
	}
	return result;
}

unique_ptr<CatalogEntry> DuckTableEntry::RemoveField(ClientContext &context, RemoveFieldInfo &info) {
	if (!ColumnExists(info.column_path[0])) {
		if (!info.if_column_exists) {
			throw CatalogException("Cannot drop field from column %s - it does not exist", info.column_path[0]);
		}
		return nullptr;
	}
	// follow the path
	auto &col = GetColumn(info.column_path[0]);
	auto res = DropFieldFromStruct(col.Type(), info.column_path, 1);
	if (res.error.HasError()) {
		if (!info.if_column_exists) {
			res.error.Throw();
		}
		return nullptr;
	}

	// construct the struct remapping expression
	vector<unique_ptr<ParsedExpression>> children;
	children.push_back(make_uniq<ColumnRefExpression>(info.column_path[0]));
	children.push_back(ConstantExpression::FromValue(Value(res.new_type)));
	children.push_back(ConstantExpression::FromValue(res.mapping));
	children.push_back(ConstantExpression::Null());

	auto function = make_uniq<FunctionExpression>("remap_struct", std::move(children));

	ChangeColumnTypeInfo change_column_type(info.GetAlterEntryData(), vector<Identifier> {info.column_path[0]},
	                                        std::move(res.new_type), std::move(function));
	return ChangeColumnType(context, change_column_type, AlterTableType::REMOVE_FIELD);
}

DroppedFieldMapping RenameFieldFromStruct(const LogicalType &type, const vector<Identifier> &column_path,
                                          const string &new_name, idx_t depth) {
	if (!type.IsNested()) {
		throw CatalogException("Cannot rename field from column %s - not a nested type", column_path[0]);
	}
	auto &rename_entry = column_path[depth];
	bool last_entry = depth + 1 == column_path.size();
	bool found = false;
	DroppedFieldMapping result;
	child_list_t<Value> child_mapping;
	child_list_t<LogicalType> new_type_children;
	auto child_types = GetChildList(type);
	for (auto &entry : child_types) {
		auto field_name = entry.first;
		Value mapping_value;
		LogicalType type_value;
		if ((type.id() == LogicalTypeId::LIST && rename_entry == "element") || field_name == rename_entry) {
			// this is the entry we are renaming
			found = true;
			if (last_entry) {
				if (type.id() != LogicalTypeId::STRUCT) {
					throw CatalogException(
					    "Cannot rename field %s from column %s - can only rename fields inside a struct",
					    column_path.back(), column_path.front());
				}
				// we are renaming this entry
				for (auto &sub_entry : child_types) {
					if (sub_entry.first == new_name) {
						throw CatalogException(
						    "Cannot rename field %s from column %s to %s - a field with this name already exists",
						    column_path.back(), column_path.front(), new_name);
					}
				}
				field_name = Identifier(new_name);
				mapping_value = ConstructMapping(entry.first, entry.second);
				type_value = entry.second;
			} else {
				// we are renaming a field in this entry - recurse
				auto child_result = RenameFieldFromStruct(entry.second, column_path, new_name, depth + 1);
				if (child_result.error.HasError()) {
					// bubble up error
					return child_result;
				}
				mapping_value = std::move(child_result.mapping);
				type_value = std::move(child_result.new_type);
			}
		} else {
			// we are not adjusting this entry - copy the type and create a straightforward mapping
			mapping_value = ConstructMapping(entry.first, entry.second);
			type_value = entry.second;
		}
		if (entry.second.IsNested()) {
			vector<Value> child_values;
			child_values.push_back(Value(entry.first));
			child_values.push_back(std::move(mapping_value));
			mapping_value = Value::TUPLE(std::move(child_values));
		}
		child_mapping.emplace_back(field_name, std::move(mapping_value));
		new_type_children.emplace_back(field_name, type_value);
	}
	if (!found) {
		result.error = ErrorData(CatalogException("Cannot rename field %s - it does not exist", rename_entry));
	} else {
		result.mapping = Value::STRUCT(std::move(child_mapping));
		result.new_type = LogicalType::ConstructNestedType(type, std::move(new_type_children));
	}
	return result;
}

unique_ptr<CatalogEntry> DuckTableEntry::RenameField(ClientContext &context, RenameFieldInfo &info) {
	if (!ColumnExists(info.column_path[0])) {
		throw CatalogException("Cannot rename field from column %s - it does not exist", info.column_path[0]);
	}

	// follow the path
	auto &col = GetColumn(info.column_path[0]);
	auto res = RenameFieldFromStruct(col.Type(), info.column_path, info.new_name.GetIdentifierName(), 1);
	if (res.error.HasError()) {
		res.error.Throw();
	}

	// construct the struct remapping expression
	vector<unique_ptr<ParsedExpression>> children;
	children.push_back(make_uniq<ColumnRefExpression>(info.column_path[0]));
	children.push_back(ConstantExpression::FromValue(Value(res.new_type)));
	children.push_back(ConstantExpression::FromValue(res.mapping));
	children.push_back(ConstantExpression::Null());

	auto function = make_uniq<FunctionExpression>("remap_struct", std::move(children));
	ChangeColumnTypeInfo change_column_type(info.GetAlterEntryData(), vector<Identifier> {info.column_path[0]},
	                                        std::move(res.new_type), std::move(function));
	return ChangeColumnType(context, change_column_type, AlterTableType::RENAME_FIELD);
}

unique_ptr<CatalogEntry> DuckTableEntry::SetDefault(ClientContext &context, SetDefaultInfo &info) {
	if (info.column_path.size() > 1) {
		throw NotImplementedException("Setting a default value on a nested field is not yet supported");
	}
	auto default_idx = GetColumnIndex(info.column_path[0]);
	if (default_idx.index == COLUMN_IDENTIFIER_ROW_ID) {
		throw CatalogException("Cannot SET DEFAULT for rowid column");
	}

	auto create_info = GetInfo();
	auto &table_info = create_info->Cast<CreateTableInfo>();

	// Modify the column that was specified by 'column_path'
	auto &col = table_info.columns.GetColumnMutable(default_idx);
	if (col.Generated()) {
		throw BinderException("Cannot SET DEFAULT for generated column %s", col.Name());
	}
	col.SetDefaultValue(info.expression ? info.expression->Copy() : nullptr);

	auto binder = Binder::CreateBinder(context);
	auto bound_create_info = binder->BindCreateTableInfo(std::move(create_info), schema, info.bind_mode);
	SetAlterDependencies(context, *bound_create_info, info);
	return make_uniq<DuckTableEntry>(catalog, schema, *bound_create_info, storage, triggers);
}

unique_ptr<CatalogEntry> DuckTableEntry::SetNotNull(ClientContext &context, SetNotNullInfo &info) {
	if (info.column_path.size() > 1) {
		throw NotImplementedException("Setting a NOT NULL constraint on a nested field is not yet supported");
	}
	auto not_null_idx = GetColumnIndex(info.column_path[0]);
	if (columns.GetColumn(LogicalIndex(not_null_idx)).Generated()) {
		throw BinderException("Unsupported constraint for generated column!");
	}

	auto create_info = GetInfo();
	auto &table_info = create_info->Cast<CreateTableInfo>();

	bool has_not_null = table_info.FindNotNullConstraint(not_null_idx).IsValid();
	if (!has_not_null) {
		table_info.constraints.push_back(make_uniq<NotNullConstraint>(not_null_idx));
	}

	auto binder = Binder::CreateBinder(context);
	auto bound_create_info = binder->BindCreateTableInfo(std::move(create_info), schema, info.bind_mode);
	SetAlterDependencies(context, *bound_create_info, info);

	// Early return
	if (has_not_null) {
		return make_uniq<DuckTableEntry>(catalog, schema, *bound_create_info, storage, triggers);
	}

	// Return with new storage info. Note that we need the bound column index here.
	auto physical_columns = columns.LogicalToPhysical(LogicalIndex(not_null_idx));
	auto bound_constraint = make_uniq<BoundNotNullConstraint>(physical_columns);
	auto new_storage = make_shared_ptr<DataTable>(context, *storage, *bound_constraint);
	return make_uniq<DuckTableEntry>(catalog, schema, *bound_create_info, new_storage, triggers);
}

unique_ptr<CatalogEntry> DuckTableEntry::DropNotNull(ClientContext &context, DropNotNullInfo &info) {
	if (info.column_path.size() > 1) {
		throw NotImplementedException("Dropping a NOT NULL constraint on a nested field is not yet supported");
	}
	auto not_null_idx = GetColumnIndex(info.column_path[0]);
	if (const auto pk = GetPrimaryKey()) {
		auto &unique = pk->Cast<UniqueConstraint>();
		for (const auto &pk_index : unique.GetLogicalIndexes(columns)) {
			if (pk_index == not_null_idx) {
				throw CatalogException("column %s is in a primary key", info.column_path[0]);
			}
		}
	}

	auto create_info = GetInfo();
	auto &table_info = create_info->Cast<CreateTableInfo>();

	// Remove the NOT NULL constraint for the specified column
	auto not_null_constraint = table_info.FindNotNullConstraint(not_null_idx);
	if (not_null_constraint.IsValid()) {
		table_info.constraints.erase_at(not_null_constraint.GetIndex());
	}

	auto binder = Binder::CreateBinder(context);
	auto bound_create_info = binder->BindCreateTableInfo(std::move(create_info), schema, info.bind_mode);
	SetAlterDependencies(context, *bound_create_info, info);
	return make_uniq<DuckTableEntry>(catalog, schema, *bound_create_info, storage, triggers);
}

unique_ptr<CatalogEntry> DuckTableEntry::ChangeColumnType(ClientContext &context, ChangeColumnTypeInfo &info,
                                                          AlterTableType alter_table_type) {
	// Only reject a user issued ALTER TYPE ADD/REMOVE/RENAME_FIELD's internal calls use a single element
	// column_path (the parent struct column), which never exceeds this size check.
	if (alter_table_type == AlterTableType::ALTER_COLUMN_TYPE && info.column_path.size() > 1) {
		throw NotImplementedException("Changing the type of a nested field is not yet supported");
	}
	// Bind type
	auto type_binder = Binder::CreateBinder(context);
	type_binder->SetSearchPath(catalog, schema.GetSchemaPath());
	type_binder->BindLogicalType(info.target_type);

	auto change_idx = GetColumnIndex(info.column_path[0]);
	auto create_info = make_uniq<CreateTableInfo>(schema, name);
	create_info->temporary = temporary;
	create_info->comment = comment;
	create_info->tags = tags;

	// Bind the USING expression.
	auto binder = Binder::CreateBinder(context);
	vector<LogicalIndex> bound_columns;
	AlterBinder expr_binder(*binder, context, *this, bound_columns, info.target_type);
	auto expression = info.expression->Copy();
	auto bound_expression = expr_binder.Bind(expression);

	// Infer the target_type from the USING expression, if not set explicitly.
	if (info.target_type == LogicalType::UNKNOWN) {
		info.target_type = bound_expression->GetReturnType();
	}

	// Check if type is supported in this database version
	CheckTypeIsSupported(info.target_type, catalog.GetAttached());

	auto bound_constraints = binder->BindConstraints(constraints, name, columns);
	for (auto &col : columns.Logical()) {
		auto copy = col.Copy();
		if (change_idx == col.Logical()) {
			// set the type of this column
			if (copy.Generated()) {
				throw NotImplementedException("Changing types of generated columns is not supported yet");
			}
			copy.SetType(info.target_type);
			if ((alter_table_type == AlterTableType::RENAME_FIELD || alter_table_type == AlterTableType::ADD_FIELD) &&
			    copy.HasDefaultValue()) {
				// the stored default is not cast yet, the mapping is for the old type of the column
				auto old_default = make_uniq<CastExpression>(col.Type(), copy.DefaultValue().Copy());
				copy.SetDefaultValue(RemapStructDefault(std::move(old_default), info.target_type, info));
			}
		}
		// TODO: check if the generated_expression breaks, only delete it if it does
		if (copy.Generated() && column_dependency_manager.IsDependencyOf(col.Logical(), change_idx)) {
			throw BinderException(
			    "This column is referenced by the generated column %s, so its type can not be changed", copy.Name());
		}
		create_info->columns.AddColumn(std::move(copy));
	}

	// If the changed column has a NOT NULL constraint, keep it so we can re-verify the rewritten values before
	// committing the ALTER; as of now other constraint types below are still rejected up front.
	unique_ptr<BoundConstraint> constraint_to_verify;
	for (idx_t constr_idx = 0; constr_idx < constraints.size(); constr_idx++) {
		auto constraint = constraints[constr_idx]->Copy();
		switch (constraint->type) {
		case ConstraintType::CHECK: {
			auto &bound_check = bound_constraints[constr_idx]->Cast<BoundCheckConstraint>();
			auto physical_index = columns.LogicalToPhysical(change_idx);
			if (bound_check.bound_columns.find(physical_index) != bound_check.bound_columns.end()) {
				throw BinderException("Cannot change the type of a column that has a CHECK constraint specified");
			}
			break;
		}
		case ConstraintType::NOT_NULL: {
			auto &bound_not_null = bound_constraints[constr_idx]->Cast<BoundNotNullConstraint>();
			if (bound_not_null.index == columns.LogicalToPhysical(change_idx)) {
				constraint_to_verify = bound_constraints[constr_idx]->Copy();
			}
			break;
		}
		case ConstraintType::UNIQUE: {
			auto &bound_unique = bound_constraints[constr_idx]->Cast<BoundUniqueConstraint>();
			auto physical_index = columns.LogicalToPhysical(change_idx);
			if (bound_unique.key_set.find(physical_index) != bound_unique.key_set.end()) {
				throw BinderException(
				    "Cannot change the type of a column that has a UNIQUE or PRIMARY KEY constraint specified");
			}
			break;
		}
		case ConstraintType::FOREIGN_KEY: {
			auto &bfk = bound_constraints[constr_idx]->Cast<BoundForeignKeyConstraint>();
			auto key_set = bfk.pk_key_set;
			if (bfk.info.type == ForeignKeyType::FK_TYPE_FOREIGN_KEY_TABLE) {
				key_set = bfk.fk_key_set;
			} else if (bfk.info.type == ForeignKeyType::FK_TYPE_SELF_REFERENCE_TABLE) {
				key_set.insert(bfk.info.fk_keys.begin(), bfk.info.fk_keys.end());
			}
			if (key_set.find(columns.LogicalToPhysical(change_idx)) != key_set.end()) {
				throw BinderException("Cannot change the type of a column that has a FOREIGN KEY constraint specified");
			}
			break;
		}
		default:
			throw InternalException("Unsupported constraint for entry!");
		}
		create_info->constraints.push_back(std::move(constraint));
	}

	auto bound_create_info = binder->BindCreateTableInfo(std::move(create_info), schema, info.bind_mode);
	SetAlterDependencies(context, *bound_create_info, info);

	vector<StorageIndex> storage_oids;
	for (idx_t i = 0; i < bound_columns.size(); i++) {
		storage_oids.emplace_back(columns.LogicalToPhysical(bound_columns[i]).index);
	}
	if (storage_oids.empty()) {
		storage_oids.emplace_back(COLUMN_IDENTIFIER_ROW_ID);
	}

	auto new_storage = make_shared_ptr<DataTable>(
	    context, *storage, columns.LogicalToPhysical(LogicalIndex(change_idx)).index, info.target_type,
	    std::move(storage_oids), *bound_expression, constraint_to_verify.get());
	auto result = make_uniq<DuckTableEntry>(catalog, schema, *bound_create_info, new_storage, triggers);
	return std::move(result);
}

unique_ptr<CatalogEntry> DuckTableEntry::SetColumnComment(ClientContext &context, SetColumnCommentInfo &info) {
	auto col_idx = GetColumnIndex(info.column_name);
	if (col_idx.index == COLUMN_IDENTIFIER_ROW_ID) {
		throw CatalogException("Cannot SET COMMENT for rowid column");
	}

	auto create_info = GetInfo();
	auto &table_info = create_info->Cast<CreateTableInfo>();

	// Modify the column that was specified by 'column_name'
	auto &col = table_info.columns.GetColumnMutable(col_idx);
	col.SetComment(info.comment_value);

	auto binder = Binder::CreateBinder(context);
	auto bound_create_info = binder->BindCreateTableInfo(std::move(create_info), schema, info.bind_mode);
	return make_uniq<DuckTableEntry>(catalog, schema, *bound_create_info, storage, triggers);
}

unique_ptr<CatalogEntry> DuckTableEntry::AddForeignKeyConstraint(AlterForeignKeyInfo &info) {
	D_ASSERT(info.type == AlterForeignKeyType::AFT_ADD);
	auto create_info = make_uniq<CreateTableInfo>(schema, name);
	create_info->temporary = temporary;
	create_info->comment = comment;
	create_info->tags = tags;

	create_info->columns = columns.Copy();
	for (idx_t i = 0; i < constraints.size(); i++) {
		create_info->constraints.push_back(constraints[i]->Copy());
	}
	ForeignKeyInfo fk_info;
	fk_info.type = ForeignKeyType::FK_TYPE_PRIMARY_KEY_TABLE;
	fk_info.schema = info.GetQualifiedName().Schema();
	fk_info.table = info.fk_table;
	fk_info.pk_keys = info.pk_keys;
	fk_info.fk_keys = info.fk_keys;
	create_info->constraints.push_back(
	    make_uniq<ForeignKeyConstraint>(info.pk_columns, info.fk_columns, std::move(fk_info)));

	unique_ptr<BoundCreateTableInfo> bound_create_info;
	bound_create_info = Binder::BindCreateTableCheckpoint(std::move(create_info), schema);
	return make_uniq<DuckTableEntry>(catalog, schema, *bound_create_info, storage, triggers);
}

unique_ptr<CatalogEntry> DuckTableEntry::DropForeignKeyConstraint(ClientContext &context, AlterForeignKeyInfo &info) {
	D_ASSERT(info.type == AlterForeignKeyType::AFT_DELETE);
	auto create_info = make_uniq<CreateTableInfo>(schema, name);
	create_info->temporary = temporary;
	create_info->comment = comment;
	create_info->tags = tags;

	create_info->columns = columns.Copy();
	for (idx_t i = 0; i < constraints.size(); i++) {
		auto constraint = constraints[i]->Copy();
		if (constraint->type == ConstraintType::FOREIGN_KEY) {
			ForeignKeyConstraint &fk = constraint->Cast<ForeignKeyConstraint>();
			if (fk.info.type == ForeignKeyType::FK_TYPE_PRIMARY_KEY_TABLE && fk.info.table == info.fk_table) {
				continue;
			}
		}
		create_info->constraints.push_back(std::move(constraint));
	}

	auto binder = Binder::CreateBinder(context);
	auto bound_create_info = binder->BindCreateTableInfo(std::move(create_info), schema, info.bind_mode);
	SetAlterDependencies(context, *bound_create_info, info);
	return make_uniq<DuckTableEntry>(catalog, schema, *bound_create_info, storage, triggers);
}

void DuckTableEntry::Rollback(CatalogEntry &prev_entry) {
	if (prev_entry.type != CatalogType::TABLE_ENTRY) {
		return;
	}

	// Rolls back any physical index creation for index-based constraints.
	auto &prev_table = prev_entry.Cast<DuckTableEntry>();
	auto &prev_indexes = prev_table.GetStorage().GetDataTableInfo()->GetIndexes();
	for (auto oid : GetAddedUniqueIndexOids(prev_table)) {
		prev_indexes.RemoveIndex(oid);
	}
}

vector<idx_t> DuckTableEntry::GetAddedUniqueIndexOids(const DuckTableEntry &prev_table) const {
	unordered_set<idx_t> prev_oids;
	for (const auto &constraint : prev_table.GetConstraints()) {
		if (constraint->type == ConstraintType::UNIQUE) {
			prev_oids.insert(constraint->GetBackingIndexOid());
		}
	}

	vector<idx_t> result;
	for (const auto &constraint : GetConstraints()) {
		if (constraint->type != ConstraintType::UNIQUE) {
			continue;
		}
		auto oid = constraint->GetBackingIndexOid();
		if (prev_oids.find(oid) == prev_oids.end()) {
			result.push_back(oid);
		}
	}
	return result;
}

void DuckTableEntry::OnDrop() {
	storage->SetAsDropped();
}

unique_ptr<CatalogEntry> DuckTableEntry::AddConstraint(ClientContext &context, AddConstraintInfo &info) {
	auto create_info = GetInfo();
	auto &table_info = create_info->Cast<CreateTableInfo>();

	if (info.constraint->type == ConstraintType::UNIQUE) {
		auto &unique = info.constraint->Cast<UniqueConstraint>();
		const auto existing_pk = GetPrimaryKey();

		if (unique.is_primary_key && existing_pk) {
			auto existing_name = existing_pk->ToString();
			throw CatalogException("table %s can have only one primary key: %s", name, existing_name);
		}

		// Constraint index names are not unique, detect a duplicate constraint by its kind and columns.
		for (const auto &constraint : GetConstraints()) {
			if (constraint->type != ConstraintType::UNIQUE) {
				continue;
			}
			auto &existing = constraint->Cast<UniqueConstraint>();
			if (existing.is_primary_key == unique.is_primary_key &&
			    existing.GetLogicalIndexes(columns) == unique.GetLogicalIndexes(columns)) {
				throw CatalogException("table %s already has the constraint %s", name, existing.ToString());
			}
		}
		table_info.constraints.push_back(info.constraint->Copy());

	} else {
		throw NotImplementedException("No support for adding %s constraints with ALTER TABLE",
		                              EnumUtil::ToString(info.constraint->type));
	}

	// The indexes of constraints must precede all other indexes. A concurrent CREATE INDEX is not visible as a
	// dependency yet, but it has already registered its index with the storage.
	idx_t constraint_index_count = 0;
	for (const auto &constraint : GetConstraints()) {
		if (constraint->type == ConstraintType::UNIQUE) {
			constraint_index_count++;
		} else if (constraint->type == ConstraintType::FOREIGN_KEY) {
			auto &fk = constraint->Cast<ForeignKeyConstraint>();
			if (fk.info.type == ForeignKeyType::FK_TYPE_FOREIGN_KEY_TABLE ||
			    fk.info.type == ForeignKeyType::FK_TYPE_SELF_REFERENCE_TABLE) {
				constraint_index_count++;
			}
		}
	}
	if (storage->GetDataTableInfo()->GetIndexes().Count() > constraint_index_count) {
		throw DependencyException("Cannot alter entry %s because there are entries that depend on it.", name);
	}

	// We create a physical table with a new constraint and a new unique index.
	const auto binder = Binder::CreateBinder(context);
	const auto bound_constraint =
	    binder->BindConstraint(*info.constraint, table_info.GetTableName(), table_info.columns);
	const auto bound_create_info = binder->BindCreateTableInfo(std::move(create_info), schema, info.bind_mode);
	SetAlterDependencies(context, *bound_create_info, info);

	auto new_storage = make_shared_ptr<DataTable>(context, *storage, *bound_constraint);
	auto new_entry = make_uniq<DuckTableEntry>(catalog, schema, *bound_create_info, new_storage, triggers);
	return std::move(new_entry);
}

unique_ptr<CatalogEntry> DuckTableEntry::Copy(ClientContext &context) const {
	D_ASSERT(!internal);
	auto create_info = GetInfo();

	auto binder = Binder::CreateBinder(context);
	auto bound_create_info = binder->BindCreateTableCheckpoint(std::move(create_info), schema);
	return make_uniq<DuckTableEntry>(catalog, schema, *bound_create_info, storage, triggers);
}

void DuckTableEntry::SetAsRoot() {
	storage->SetAsMainTable();
	storage->SetTableName(name);
}

void DuckTableEntry::CommitAlter(string &column_name, CommitDropState &drop_state) {
	D_ASSERT(!column_name.empty());
	optional_idx logical_column_idx;
	idx_t column_position = 0;
	for (auto &col : columns.Logical()) {
		if (col.Name() == column_name) {
			// No need to alter storage, removed column is generated column
			if (col.Generated()) {
				return;
			}
			logical_column_idx = column_position;
			break;
		}
		column_position++;
	}

	if (!logical_column_idx.IsValid()) {
		return;
	}
	auto logical_column_index = LogicalIndex(logical_column_idx.GetIndex());
	auto column_index = columns.LogicalToPhysical(logical_column_index).index;
	storage->CommitDropColumn(column_index, drop_state);
}

void DuckTableEntry::CommitDrop(CommitDropState &drop_state) {
	storage->CommitDropTable(drop_state);
}

DataTable &DuckTableEntry::GetStorage() {
	return *storage;
}

TableFunction DuckTableEntry::GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) {
	bind_data = make_uniq<TableScanBindData>(*this);
	return TableScanFunction::GetFunction();
}

vector<ColumnSegmentInfo> DuckTableEntry::GetColumnSegmentInfo(const QueryContext &context,
                                                               const ColumnSegmentInfoScanOptions &options) {
	return storage->GetColumnSegmentInfo(context, options);
}

void DuckTableEntry::InitializeColumnSegmentInfoScan(ColumnSegmentInfoScanState &state) {
	storage->InitializeColumnSegmentInfoScan(state);
}

bool DuckTableEntry::ScanColumnSegmentInfo(const QueryContext &context, ColumnSegmentInfoScanState &state,
                                           vector<ColumnSegmentInfo> &result) {
	return storage->ScanColumnSegmentInfo(context, state, result);
}

TableStorageInfo DuckTableEntry::GetStorageInfo(ClientContext &context) {
	return storage->GetStorageInfo();
}

optional_ptr<CatalogEntry> DuckTableEntry::CreateTrigger(CatalogTransaction transaction, CreateTriggerInfo &info) {
	auto trigger = make_uniq<TriggerCatalogEntry>(catalog, schema, info);
	auto entry_name = trigger->name;
	LogicalDependencyList dependencies = trigger->dependencies;
	if (info.on_conflict == OnCreateConflict::IGNORE_ON_CONFLICT) {
		auto old_entry = triggers->GetEntry(transaction, entry_name);
		if (old_entry) {
			return nullptr;
		}
	} else if (info.on_conflict == OnCreateConflict::REPLACE_ON_CONFLICT) {
		auto old_entry = triggers->GetEntry(transaction, entry_name);
		if (old_entry) {
			triggers->DropEntry(transaction, entry_name, false);
		}
	}
	if (!triggers->CreateEntry(transaction, entry_name, std::move(trigger), dependencies)) {
		throw CatalogException::EntryAlreadyExists(CatalogType::TRIGGER_ENTRY, entry_name);
	}
	return triggers->GetEntry(transaction, entry_name);
}

void DuckTableEntry::ScanTriggers(CatalogTransaction transaction,
                                  const std::function<void(CatalogEntry &)> &callback) const {
	triggers->Scan(transaction, callback);
}

optional_ptr<CatalogEntry> DuckTableEntry::GetTrigger(CatalogTransaction transaction, const Identifier &name) const {
	return triggers->GetEntry(transaction, name);
}

void DuckTableEntry::ScanTriggers(VisibilityBound bound, const std::function<void(CatalogEntry &)> &callback) {
	triggers->Scan(bound, callback);
}

bool DuckTableEntry::DropTrigger(CatalogTransaction transaction, const Identifier &name, bool cascade) {
	return triggers->DropEntry(transaction, name, cascade);
}

} // namespace duckdb
