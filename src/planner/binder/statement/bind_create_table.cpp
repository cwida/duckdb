#include "duckdb/parser/constraints/list.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/common/types/numeric_type.hpp"
#include "duckdb/planner/constraints/list.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression_binder/check_binder.hpp"
#include "duckdb/planner/expression_binder/constant_binder.hpp"
#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"
#include "duckdb/catalog/catalog_entry/type_catalog_entry.hpp"
#include "duckdb/catalog/dependency_manager.hpp"
#include "duckdb/function/table/table_scan.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/queue.hpp"
#include "duckdb/common/exception/parser_exception.hpp"
#include "duckdb/parser/expression/list.hpp"
#include "duckdb/common/index_map.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/expression_binder/index_binder.hpp"
#include "duckdb/parser/parsed_data/create_index_info.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/storage_manager.hpp"
#include "duckdb/common/type_visitor.hpp"
#include "duckdb/main/config.hpp"

namespace duckdb {

static void CreateColumnDependencyManager(BoundCreateTableInfo &info) {
	auto &base = info.base->Cast<CreateTableInfo>();
	for (auto &col : base.columns.Logical()) {
		if (!col.Generated()) {
			continue;
		}
		info.column_dependency_manager.AddGeneratedColumn(col, base.columns);
	}
}

static void VerifyCompressionType(ClientContext &context, optional_ptr<StorageManager> storage_manager,
                                  DBConfig &config, BoundCreateTableInfo &info) {
	auto &base = info.base->Cast<CreateTableInfo>();
	for (auto &col : base.columns.Logical()) {
		auto compression_type = col.CompressionType();
		if (col.Generated() && compression_type != CompressionType::COMPRESSION_AUTO) {
			throw BinderException("Can't set a compression type on generated column %s, generated columns are "
			                      "not stored",
			                      col.Name());
		}
		auto compression_availability_result = CompressionTypeIsAvailable(compression_type, storage_manager);
		if (!compression_availability_result.IsAvailable()) {
			if (compression_availability_result.IsDeprecated()) {
				throw BinderException(
				    "Can't compress using user-provided compression type '%s', that type is deprecated "
				    "and only has decompress support",
				    CompressionTypeToString(compression_type));
			} else {
				throw BinderException(
				    "Can't compress using user-provided compression type '%s', that type is not available yet",
				    CompressionTypeToString(compression_type));
			}
		}
		auto logical_type = col.GetType();
		if (logical_type.id() == LogicalTypeId::UNBOUND && logical_type.HasAlias()) {
			// Resolve user type if possible
			const auto type_entry = Catalog::GetEntry<TypeCatalogEntry>(
			    context,
			    QualifiedName(Identifier::InvalidCatalog(), Identifier::InvalidSchema(),
			                  Identifier(logical_type.GetAlias())),
			    OnEntryNotFound::RETURN_NULL);
			if (type_entry) {
				logical_type = type_entry->user_type;
			}
		}
		auto physical_type = logical_type.InternalType();
		if (compression_type == CompressionType::COMPRESSION_AUTO) {
			continue;
		}
		auto compression_method = config.TryGetCompressionFunction(compression_type, physical_type);
		if (!compression_method) {
			throw BinderException("Can't compress column %s with type '%s' (physical: %s) using compression type '%s'",
			                      col.Name(), logical_type.ToString(), EnumUtil::ToString(physical_type),
			                      CompressionTypeToString(compression_type));
		}
	}
}

vector<unique_ptr<BoundConstraint>> Binder::BindConstraints(ClientContext &context,
                                                            const vector<unique_ptr<Constraint>> &constraints,
                                                            const Identifier &table_name, const ColumnList &columns) {
	auto binder = Binder::CreateBinder(context);
	return binder->BindConstraints(constraints, table_name, columns);
}

vector<unique_ptr<BoundConstraint>> Binder::BindConstraints(const TableCatalogEntry &table) {
	return BindConstraints(table.GetConstraints(), table.name, table.GetColumns());
}

vector<unique_ptr<BoundConstraint>> Binder::BindConstraints(const vector<unique_ptr<Constraint>> &constraints,
                                                            const Identifier &table_name, const ColumnList &columns) {
	vector<unique_ptr<BoundConstraint>> bound_constraints;
	for (const auto &constr : constraints) {
		bound_constraints.push_back(BindConstraint(*constr, table_name, columns));
	}
	return bound_constraints;
}

vector<unique_ptr<BoundConstraint>> Binder::BindNewConstraints(vector<unique_ptr<Constraint>> &constraints,
                                                               const Identifier &table_name,
                                                               const ColumnList &columns) {
	auto bound_constraints = BindConstraints(constraints, table_name, columns);

	// Handle PK and NOT NULL constraints.
	bool has_primary_key = false;
	physical_index_set_t not_null_columns;
	vector<PhysicalIndex> primary_keys;

	for (const auto &bound_constr : bound_constraints) {
		switch (bound_constr->type) {
		case ConstraintType::NOT_NULL: {
			auto &not_null = bound_constr->Cast<BoundNotNullConstraint>();
			not_null_columns.insert(not_null.index);
			break;
		}
		case ConstraintType::UNIQUE: {
			const auto &unique = bound_constr->Cast<BoundUniqueConstraint>();
			if (unique.is_primary_key) {
				if (has_primary_key) {
					throw ParserException("table %s has more than one primary key", table_name);
				}
				has_primary_key = true;
				primary_keys = unique.keys;
			}
			break;
		}
		default:
			break;
		}
	}

	if (has_primary_key) {
		// Create a PK constraint, and a NOT NULL constraint for each indexed column.
		for (auto &column_index : primary_keys) {
			if (not_null_columns.count(column_index) != 0) {
				continue;
			}

			auto logical_index = columns.PhysicalToLogical(column_index);
			constraints.push_back(make_uniq<NotNullConstraint>(logical_index));
			bound_constraints.push_back(make_uniq<BoundNotNullConstraint>(column_index));
		}
	}

	return bound_constraints;
}

unique_ptr<BoundConstraint> BindCheckConstraint(Binder &binder, const Constraint &constraint, const Identifier &table,
                                                const ColumnList &columns) {
	auto bound_constraint = make_uniq<BoundCheckConstraint>();
	auto &bound_check = bound_constraint->Cast<BoundCheckConstraint>();

	// Bind the CHECK expression.
	CheckBinder check_binder(binder, binder.context, table, columns, bound_check.bound_columns);
	auto &check = constraint.Cast<CheckConstraint>();

	// Create a copy of the unbound expression because binding can invalidate it.
	auto check_copy = check.expression->Copy();

	// Bind the constraint and reset the original expression.
	bound_check.expression = check_binder.Bind(check_copy);
	return std::move(bound_constraint);
}

void Binder::VerifyConstraintCheckModeStorageVersion(const Constraint &constraint, Catalog &catalog, bool temporary) {
	if (constraint.type != ConstraintType::UNIQUE || temporary || !catalog.IsDuckCatalog() || catalog.InMemory()) {
		return;
	}
	auto &unique = constraint.Cast<UniqueConstraint>();
	if (unique.check_mode == ConstraintCheckMode::DEFAULT) {
		return;
	}
	if (StorageManager::Get(catalog).GetStorageVersion() < StorageVersion::V2_0_0) {
		throw BinderException(
		    "Explicit constraint check mode is only supported for storage versions v2.0.0 and higher.\n"
		    "Use an in-memory database, or ATTACH with (STORAGE_VERSION 'v2.0.0')");
	}
}

unique_ptr<BoundConstraint> Binder::BindUniqueConstraint(const Constraint &constraint, const Identifier &table,
                                                         const ColumnList &columns) {
	auto &unique = constraint.Cast<UniqueConstraint>();

	// Resolve the columns.
	vector<PhysicalIndex> indexes;
	physical_index_set_t index_set;

	// HasIndex refers to a column index, not an index(-structure).
	// If set, then the UNIQUE constraint is defined on a single column.
	if (unique.HasIndex()) {
		auto &col = columns.GetColumn(unique.GetIndex());
		indexes.push_back(col.Physical());
		index_set.insert(col.Physical());
		return make_uniq<BoundUniqueConstraint>(std::move(indexes), std::move(index_set), unique.IsPrimaryKey());
	}

	// The UNIQUE constraint is defined on a list of columns.
	for (auto &col_name : unique.GetColumnNames()) {
		if (!columns.ColumnExists(col_name)) {
			throw CatalogException("table %s does not have a column named %s", table, col_name);
		}
		auto &col = columns.GetColumn(col_name);
		if (col.Generated()) {
			string constraint_type = unique.IsPrimaryKey() ? "PRIMARY KEY" : "UNIQUE constraint";
			throw BinderException("cannot create a %s on a generated column: %s", constraint_type,
			                      SQLIdentifier(col.GetName()));
		}

		auto physical_index = col.Physical();
		if (index_set.find(physical_index) != index_set.end()) {
			throw ParserException("column %s appears twice in primary key constraint", col_name);
		}
		indexes.push_back(physical_index);
		index_set.insert(physical_index);
	}

	return make_uniq<BoundUniqueConstraint>(std::move(indexes), std::move(index_set), unique.IsPrimaryKey());
}

unique_ptr<BoundConstraint> BindForeignKey(const Constraint &constraint) {
	auto &fk = constraint.Cast<ForeignKeyConstraint>();
	D_ASSERT((fk.info.type == ForeignKeyType::FK_TYPE_FOREIGN_KEY_TABLE && !fk.info.pk_keys.empty()) ||
	         (fk.info.type == ForeignKeyType::FK_TYPE_PRIMARY_KEY_TABLE && !fk.info.pk_keys.empty()) ||
	         fk.info.type == ForeignKeyType::FK_TYPE_SELF_REFERENCE_TABLE);

	physical_index_set_t pk_key_set;
	for (auto &pk_key : fk.info.pk_keys) {
		if (pk_key_set.find(pk_key) != pk_key_set.end()) {
			throw ParserException("duplicate primary key referenced in FOREIGN KEY constraint");
		}
		pk_key_set.insert(pk_key);
	}

	physical_index_set_t fk_key_set;
	for (auto &fk_key : fk.info.fk_keys) {
		if (fk_key_set.find(fk_key) != fk_key_set.end()) {
			throw ParserException("duplicate key specified in FOREIGN KEY constraint");
		}
		fk_key_set.insert(fk_key);
	}

	return make_uniq<BoundForeignKeyConstraint>(fk.info, std::move(pk_key_set), std::move(fk_key_set));
}

unique_ptr<BoundConstraint> Binder::BindConstraint(const Constraint &constraint, const Identifier &table,
                                                   const ColumnList &columns) {
	switch (constraint.type) {
	case ConstraintType::CHECK: {
		return BindCheckConstraint(*this, constraint, table, columns);
	}
	case ConstraintType::NOT_NULL: {
		auto &not_null = constraint.Cast<NotNullConstraint>();
		auto &col = columns.GetColumn(not_null.index);
		return make_uniq<BoundNotNullConstraint>(col.Physical());
	}
	case ConstraintType::UNIQUE: {
		return BindUniqueConstraint(constraint, table, columns);
	}
	case ConstraintType::FOREIGN_KEY: {
		return BindForeignKey(constraint);
	}
	default:
		throw NotImplementedException("unrecognized constraint type in bind");
	}
}

void Binder::BindGeneratedColumns(BoundCreateTableInfo &info) {
	auto &base = info.base->Cast<CreateTableInfo>();

	vector<Identifier> names;
	vector<LogicalType> types;

	D_ASSERT(base.type == CatalogType::TABLE_ENTRY);
	for (auto &col : base.columns.Logical()) {
		names.emplace_back(col.Name());
		types.push_back(col.Type());
	}
	auto table_index = GenerateTableIndex();

	// Create a new binder because we dont need (or want) these bindings in this scope
	auto binder = Binder::CreateBinder(context);
	binder->SetCatalogLookupCallback(entry_retriever.GetCallback());
	binder->bind_context.AddGenericBinding(table_index, base.GetTableName(), names, types);
	auto expr_binder = ExpressionBinder(*binder, context);
	ErrorData ignore;
	auto table_binding = binder->bind_context.GetBinding(base.GetTableName(), ignore);
	D_ASSERT(table_binding && !ignore.HasError());

	auto bind_order = info.column_dependency_manager.GetBindOrder(base.columns);
	logical_index_set_t bound_indices;

	while (!bind_order.empty()) {
		auto i = bind_order.top();
		bind_order.pop();
		auto &col = base.columns.GetColumnMutable(i);

		//! Already bound this previously
		//! This can not be optimized out of the GetBindOrder function
		//! These occurrences happen because we need to make sure that ALL dependencies of a column are resolved before
		//! it gets resolved
		if (bound_indices.count(i)) {
			continue;
		}
		D_ASSERT(col.Generated());

		auto expression = col.GeneratedExpression().Copy();

		auto bound_expression = expr_binder.Bind(expression);
		D_ASSERT(bound_expression);
		if (bound_expression->HasSubquery()) {
			throw BinderException("Failed to bind generated column %s because the expression contains a subquery",
			                      col.Name());
		}
		if (col.Type().id() == LogicalTypeId::ANY) {
			// Do this before changing the type, so we know it's the first time the type is set
			col.ChangeGeneratedExpressionType(bound_expression->GetReturnType());
			col.SetType(bound_expression->GetReturnType());

			// Update the type in the binding, for future expansions
			table_binding->SetColumnType(i.index, col.Type());
		} else if (col.Type().id() == LogicalTypeId::UNBOUND) {
			// Bind column type
			BindLogicalType(col.TypeMutable());
			table_binding->SetColumnType(i.index, col.Type());
		}

		bound_indices.insert(i);
	}
}

void Binder::BindDefaultValue(const ColumnDefinition &column, vector<unique_ptr<Expression>> &bound_defaults,
                              const string &catalog, const vector<Identifier> &schema_path) {
	ColumnList col_list;
	col_list.AddColumn(column.Copy());
	BindDefaultValues(col_list, bound_defaults, catalog, schema_path);
}

void Binder::BindDefaultValues(const ColumnList &columns, vector<unique_ptr<Expression>> &bound_defaults,
                               const string &catalog_name, const vector<Identifier> &schema_p) {
	vector<Identifier> schema_path = schema_p;
	if (schema_p.empty()) {
		schema_path.emplace_back(DEFAULT_SCHEMA);
	}

	auto default_binder = CreateBinderWithSearchPath(Identifier(catalog_name), schema_path);

	for (auto &column : columns.Physical()) {
		unique_ptr<Expression> bound_default;
		if (column.HasDefaultValue()) {
			// we bind a copy of the DEFAULT value because binding is destructive
			// and we want to keep the original expression around for serialization
			auto default_copy = column.DefaultValue().Copy();
			if (default_copy->HasParameter()) {
				throw BinderException("DEFAULT values cannot contain parameters");
			}
			ConstantBinder default_value_binder(*default_binder, context, "DEFAULT value");
			default_value_binder.target_type = column.Type();
			bound_default = default_value_binder.Bind(default_copy);
		} else {
			// no default value specified: push a default value of constant null
			bound_default = make_uniq<BoundConstantExpression>(Value(column.Type()));
		}
		bound_defaults.push_back(std::move(bound_default));
	}
}

unique_ptr<BoundCreateTableInfo> Binder::BindCreateTableInfo(unique_ptr<CreateInfo> info, SchemaCatalogEntry &schema,
                                                             AlterBindMode bind_mode) {
	vector<unique_ptr<Expression>> bound_defaults;
	return BindCreateTableInfo(std::move(info), schema, bound_defaults, bind_mode);
}

unique_ptr<BoundCreateTableInfo> Binder::BindCreateTableCheckpoint(unique_ptr<CreateInfo> info,
                                                                   SchemaCatalogEntry &schema) {
	auto result = make_uniq<BoundCreateTableInfo>(schema, std::move(info));
	CreateColumnDependencyManager(*result);
	return result;
}

static void ExpressionContainsGeneratedColumn(const ParsedExpression &root_expr, const identifier_set_t &gcols,
                                              bool &contains_gcol) {
	ParsedExpressionIterator::VisitExpression<ColumnRefExpression>(root_expr,
	                                                               [&](const ColumnRefExpression &column_ref) {
		                                                               auto &name = column_ref.GetColumnName();
		                                                               if (gcols.count(name)) {
			                                                               contains_gcol = true;
			                                                               return;
		                                                               }
	                                                               });
}

static bool AnyConstraintReferencesGeneratedColumn(CreateTableInfo &table_info) {
	identifier_set_t generated_columns;
	for (auto &col : table_info.columns.Logical()) {
		if (!col.Generated()) {
			continue;
		}
		generated_columns.insert(col.Name());
	}
	if (generated_columns.empty()) {
		return false;
	}

	for (auto &constr : table_info.constraints) {
		switch (constr->type) {
		case ConstraintType::CHECK: {
			auto &constraint = constr->Cast<CheckConstraint>();
			auto &expr = constraint.expression;
			bool contains_generated_column = false;
			ExpressionContainsGeneratedColumn(*expr, generated_columns, contains_generated_column);
			if (contains_generated_column) {
				return true;
			}
			break;
		}
		case ConstraintType::NOT_NULL: {
			auto &constraint = constr->Cast<NotNullConstraint>();
			if (table_info.columns.GetColumn(constraint.index).Generated()) {
				return true;
			}
			break;
		}
		case ConstraintType::UNIQUE: {
			auto &constraint = constr->Cast<UniqueConstraint>();
			if (!constraint.HasIndex()) {
				for (auto &col : constraint.GetColumnNames()) {
					if (generated_columns.count(col)) {
						return true;
					}
				}
			} else {
				if (table_info.columns.GetColumn(constraint.GetIndex()).Generated()) {
					return true;
				}
			}
			break;
		}
		case ConstraintType::FOREIGN_KEY: {
			// If it contained a generated column, an exception would have been thrown inside AddDataTableIndex earlier
			break;
		}
		default: {
			throw NotImplementedException("ConstraintType not implemented");
		}
		}
	}
	return false;
}

static void FindForeignKeyIndexes(const ColumnList &columns, const vector<Identifier> &names,
                                  vector<PhysicalIndex> &indexes) {
	D_ASSERT(indexes.empty());
	D_ASSERT(!names.empty());
	for (auto &name : names) {
		if (!columns.ColumnExists(name)) {
			throw BinderException("column %s named in key does not exist", name);
		}
		auto &column = columns.GetColumn(name);
		if (column.Generated()) {
			throw BinderException("Failed to create foreign key: referenced column %s is a generated column",
			                      column.Name());
		}
		indexes.push_back(column.Physical());
	}
}

static void ThrowDeferredReferencedKey() {
	throw BinderException(
	    "Failed to create foreign key: the referenced PRIMARY KEY or UNIQUE constraint cannot be DEFERRED");
}

static void FindMatchingPrimaryKeyColumns(const ColumnList &columns, const vector<unique_ptr<Constraint>> &constraints,
                                          ForeignKeyConstraint &fk) {
	// find the matching primary key constraint
	bool found_constraint = false;
	bool found_deferred = false;
	// if no columns are defined, we will automatically try to bind to the primary key
	bool find_primary_key = fk.pk_columns.empty();
	if (!find_primary_key && fk.pk_columns.size() != fk.fk_columns.size()) {
		throw BinderException(
		    "Failed to create foreign key: number of referencing (%s) and referenced columns (%s) differ",
		    StringUtil::Join(fk.fk_columns, ","), StringUtil::Join(fk.pk_columns, ","));
	}
	for (auto &constr : constraints) {
		if (constr->type != ConstraintType::UNIQUE) {
			continue;
		}
		auto &unique = constr->Cast<UniqueConstraint>();
		if (find_primary_key && !unique.IsPrimaryKey()) {
			continue;
		}
		found_constraint = true;

		vector<Identifier> pk_names;
		if (unique.HasIndex()) {
			pk_names.emplace_back(columns.GetColumn(LogicalIndex(unique.GetIndex())).Name());
		} else {
			pk_names = unique.GetColumnNames();
		}
		if (find_primary_key) {
			// found matching primary key
			if (pk_names.size() != fk.fk_columns.size()) {
				auto pk_name_str = StringUtil::Join(pk_names, ",");
				auto fk_name_str = StringUtil::Join(fk.fk_columns, ",");
				throw BinderException(
				    "Failed to create foreign key: number of referencing (%s) and referenced columns (%s) differ",
				    fk_name_str, pk_name_str);
			}
			if (unique.IsDeferred()) {
				ThrowDeferredReferencedKey();
			}
			fk.pk_columns = pk_names;
			return;
		}
		if (pk_names != fk.pk_columns) {
			continue;
		}
		if (unique.IsDeferred()) {
			// keep looking for a matching constraint that is not deferred
			found_deferred = true;
			continue;
		}
		// found match
		return;
	}
	if (found_deferred) {
		ThrowDeferredReferencedKey();
	}
	// no match found! examine why
	if (!found_constraint) {
		// no unique constraint or primary key
		string search_term = find_primary_key ? "primary key" : "primary key or unique constraint";
		throw BinderException("Failed to create foreign key: there is no %s for referenced table %s", search_term,
		                      fk.info.table);
	}
	// check if all the columns exist
	for (auto &name : fk.pk_columns) {
		bool found = columns.ColumnExists(name);
		if (!found) {
			throw BinderException("Failed to create foreign key: referenced table %s does not have a column named %s",
			                      fk.info.table, name);
		}
	}
	auto fk_names = StringUtil::Join(fk.pk_columns, ",");
	throw BinderException("Failed to create foreign key: referenced table %s does not have a primary key or unique "
	                      "constraint on the columns %s",
	                      fk.info.table, fk_names);
}

static void CheckForeignKeyTypes(const ColumnList &pk_columns, const ColumnList &fk_columns, ForeignKeyConstraint &fk) {
	D_ASSERT(fk.info.pk_keys.size() == fk.info.fk_keys.size());
	for (idx_t c_idx = 0; c_idx < fk.info.pk_keys.size(); c_idx++) {
		auto &pk_col = pk_columns.GetColumn(fk.info.pk_keys[c_idx]);
		auto &fk_col = fk_columns.GetColumn(fk.info.fk_keys[c_idx]);
		if (pk_col.Type() != fk_col.Type()) {
			throw BinderException("Failed to create foreign key: incompatible types between column %s (\"%s\") and "
			                      "column %s (\"%s\")",
			                      pk_col.Name(), pk_col.Type().ToString(), fk_col.Name(), fk_col.Type().ToString());
		}
	}
}

static void BindCreateTableConstraints(CreateTableInfo &create_info, CatalogEntryRetriever &entry_retriever,
                                       SchemaCatalogEntry &schema) {
	// If there is a foreign key constraint, resolve primary key column's index from primary key column's name
	reference_set_t<SchemaCatalogEntry> fk_schemas;
	for (idx_t i = 0; i < create_info.constraints.size(); i++) {
		auto &cond = create_info.constraints[i];
		if (cond->type != ConstraintType::FOREIGN_KEY) {
			continue;
		}
		auto &fk = cond->Cast<ForeignKeyConstraint>();
		if (fk.info.type != ForeignKeyType::FK_TYPE_FOREIGN_KEY_TABLE) {
			continue;
		}
		if (!fk.info.pk_keys.empty() && !fk.info.fk_keys.empty()) {
			return;
		}
		D_ASSERT(fk.info.pk_keys.empty());
		D_ASSERT(fk.info.fk_keys.empty());
		FindForeignKeyIndexes(create_info.columns, fk.fk_columns, fk.info.fk_keys);

		// Resolve the self-reference.
		if (create_info.GetTableName() == fk.info.table) {
			fk.info.type = ForeignKeyType::FK_TYPE_SELF_REFERENCE_TABLE;
			FindMatchingPrimaryKeyColumns(create_info.columns, create_info.constraints, fk);
			FindForeignKeyIndexes(create_info.columns, fk.pk_columns, fk.info.pk_keys);
			CheckForeignKeyTypes(create_info.columns, create_info.columns, fk);
			continue;
		}

		// Resolve the table reference in the same catalog/schema as the table being
		// created, so FK references work for external catalogs (not just the default).
		EntryLookupInfo table_lookup(CatalogType::TABLE_ENTRY, QualifiedName(fk.info.table));
		QualifiedName fk_name;
		if (fk.info.schema.empty() || fk.info.schema == schema.name) {
			// a foreign key can only reference a table in the same (possibly nested) schema
			fk_name = schema.GetQualifiedName(fk.info.table);
		} else {
			fk_name = QualifiedName(Identifier::InvalidCatalog(), fk.info.schema, fk.info.table);
		}
		auto table_entry = entry_retriever.GetEntry(EntryLookupInfo(table_lookup, fk_name));
		if (table_entry->type == CatalogType::VIEW_ENTRY) {
			throw BinderException("cannot reference a VIEW with a FOREIGN KEY");
		}

		auto &pk_table_entry_ptr = table_entry->Cast<TableCatalogEntry>();
		fk.info.schema = pk_table_entry_ptr.schema.name;
		if (&pk_table_entry_ptr.schema != &schema) {
			throw BinderException("Creating foreign keys across different schemas or catalogs is not supported");
		}
		FindMatchingPrimaryKeyColumns(pk_table_entry_ptr.GetColumns(), pk_table_entry_ptr.GetConstraints(), fk);
		FindForeignKeyIndexes(pk_table_entry_ptr.GetColumns(), fk.pk_columns, fk.info.pk_keys);
		CheckForeignKeyTypes(pk_table_entry_ptr.GetColumns(), create_info.columns, fk);
		if (pk_table_entry_ptr.IsDuckTable()) {
			auto &storage = pk_table_entry_ptr.GetStorage();

			if (!storage.HasForeignKeyIndex(fk.info.pk_keys, ForeignKeyType::FK_TYPE_PRIMARY_KEY_TABLE)) {
				auto fk_column_names = StringUtil::Join(fk.pk_columns, ",");
				throw BinderException("Failed to create foreign key on %s(%s): no UNIQUE or PRIMARY KEY constraint "
				                      "present on these columns",
				                      pk_table_entry_ptr.name, fk_column_names);
			}
		}

		D_ASSERT(fk.info.pk_keys.size() == fk.info.fk_keys.size());
		D_ASSERT(fk.info.pk_keys.size() == fk.pk_columns.size());
		D_ASSERT(fk.info.fk_keys.size() == fk.fk_columns.size());
	}
}

unique_ptr<BoundCreateTableInfo> Binder::BindCreateTableInfo(unique_ptr<CreateInfo> info, SchemaCatalogEntry &schema,
                                                             vector<unique_ptr<Expression>> &bound_defaults,
                                                             AlterBindMode bind_mode) {
	auto result = make_uniq<BoundCreateTableInfo>(schema, std::move(info));
	auto &base = result->Base();
	base.dependencies = LogicalDependencyList();
	auto &dependencies = base.dependencies;
	dependencies.AddDependency(schema);
	auto &catalog = schema.ParentCatalog();
	optional_ptr<StorageManager> storage_manager;
	if (catalog.IsDuckCatalog() && !catalog.InMemory()) {
		storage_manager = StorageManager::Get(catalog);
	}

	if (!base.query) {
		// the type binder below takes this callback over, so the table depends on the types of its columns
		SetCatalogLookupCallback([&dependencies, &schema](CatalogEntry &entry) {
			if (&schema.ParentCatalog() != &entry.ParentCatalog()) {
				// Don't register dependencies between catalogs
				return;
			}

			if (entry.type == CatalogType::TYPE_ENTRY && entry.internal) {
				// Don't register dependencies on internal types
				return;
			}

			dependencies.AddDependency(entry);
		});
	}

	// Bind all types by first looking into the same catalog/schema as the table
	auto type_binder = Binder::CreateBinder(context, *this);
	type_binder->SetSearchPath(result->schema.catalog, result->schema.GetSchemaPath());

	vector<unique_ptr<BoundConstraint>> bound_constraints;
	// a test setting: every DECIMAL column of a DuckDB table is declared as NUMERIC
	bool decimal_as_numeric = catalog.IsDuckCatalog() && Settings::Get<DebugDecimalAsNumericSetting>(context);
	if (base.query) {
		// construct the result object
		auto query_obj = Bind(*base.query);
		base.query.reset();
		result->query = std::move(query_obj.plan);

		// construct the set of columns based on the names and types of the query
		auto &names = query_obj.names;
		auto sql_types = query_obj.types;
		if (decimal_as_numeric) {
			for (auto &type : sql_types) {
				type = NumericType::DecimalsAsNumeric(type);
			}
		}
		// e.g. create table (col1 ,col2) as QUERY
		// col1 and col2 are the target_col_names
		auto target_col_names = base.columns.GetColumnNames();
		// TODO check  types and target_col_names are mismatch in size
		D_ASSERT(names.size() == sql_types.size());
		base.columns.SetAllowDuplicates(true);
		if (!target_col_names.empty()) {
			if (target_col_names.size() > sql_types.size()) {
				throw BinderException("Target table has more column names than query result.");
			} else if (target_col_names.size() < sql_types.size()) {
				// filled the target_col_names with the name of query names
				for (idx_t i = target_col_names.size(); i < sql_types.size(); i++) {
					target_col_names.emplace_back(names[i]);
				}
			}
			ColumnList new_columns;
			for (idx_t i = 0; i < target_col_names.size(); i++) {
				new_columns.AddColumn(ColumnDefinition(Identifier(target_col_names[i]), sql_types[i]));
			}
			base.columns = std::move(new_columns);
		} else {
			for (idx_t i = 0; i < names.size(); i++) {
				base.columns.AddColumn(ColumnDefinition(names[i], sql_types[i]));
			}
		}

		// Bind all types
		for (idx_t i = 0; i < base.columns.PhysicalColumnCount(); i++) {
			auto &column = base.columns.GetColumnMutable(PhysicalIndex(i));
			type_binder->BindLogicalType(column.TypeMutable());
			if (decimal_as_numeric) {
				column.SetType(NumericType::DecimalsAsNumeric(column.Type()));
			}
		}

	} else {
		// Bind all physical column types
		for (idx_t i = 0; i < base.columns.PhysicalColumnCount(); i++) {
			auto &column = base.columns.GetColumnMutable(PhysicalIndex(i));
			type_binder->BindLogicalType(column.TypeMutable());
			if (decimal_as_numeric) {
				column.SetType(NumericType::DecimalsAsNumeric(column.Type()));
			}
		}

		auto &config = DBConfig::Get(catalog.GetAttached());
		VerifyCompressionType(context, storage_manager, config, *result);
		CreateColumnDependencyManager(*result);
		// bind the generated column expressions
		BindGeneratedColumns(*result);
		// bind any constraints

		BindCreateTableConstraints(base, entry_retriever, schema);

		if (AnyConstraintReferencesGeneratedColumn(base)) {
			throw BinderException("Constraints on generated columns are not supported yet");
		}
		bound_constraints = BindNewConstraints(base.constraints, base.GetTableName(), base.columns);
		for (auto &constraint : base.constraints) {
			VerifyConstraintCheckModeStorageVersion(*constraint, catalog, base.temporary);
		}
		if (bind_mode != AlterBindMode::SKIP_BINDING) {
			// bind the default values
			auto &catalog_name = schema.ParentCatalog().GetName();
			BindDefaultValues(base.columns, bound_defaults, catalog_name.GetIdentifierName(), schema.GetSchemaPath());
		}
	}

	if (base.columns.PhysicalColumnCount() == 0) {
		throw BinderException("Creating a table without physical (non-generated) columns is not supported");
	}

	base.dependencies.VerifyDependencies(schema.catalog, base.GetTableName());

#ifdef DEBUG
	// Ensure all types are bound
	for (idx_t i = 0; i < base.columns.LogicalColumnCount(); i++) {
		auto &column = base.columns.GetColumn(LogicalIndex(i));
		if (TypeVisitor::Contains(column.Type(), LogicalTypeId::UNBOUND)) {
			throw InternalException("Unbound type remaining in column \"%s\" during Create Table bind", column.Name());
		}
	}
#endif

	return result;
}

unique_ptr<BoundCreateTableInfo> Binder::BindCreateTableInfo(unique_ptr<CreateInfo> info) {
	auto &base = info->Cast<CreateTableInfo>();
	auto &schema = BindCreateSchema(base);
	return BindCreateTableInfo(std::move(info), schema);
}

} // namespace duckdb
