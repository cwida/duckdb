#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/catalog_entry/macro_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/view_catalog_entry.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/execution/index/art/art.hpp"
#include "duckdb/function/table/table_scan.hpp"
#include "duckdb/parser/constraints/unique_constraint.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/parsed_data/alter_macro_info.hpp"
#include "duckdb/parser/parsed_data/alter_schema_info.hpp"
#include "duckdb/parser/parsed_data/comment_on_column_info.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/parser/statement/alter_statement.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/constraints/bound_unique_constraint.hpp"
#include "duckdb/planner/expression_binder/index_binder.hpp"
#include "duckdb/planner/operator/logical_create_index.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_alter.hpp"
#include "duckdb/planner/operator/logical_drop.hpp"

namespace duckdb {

unique_ptr<LogicalOperator> DuckCatalog::BindAlterAddIndex(Binder &binder, TableCatalogEntry &table_entry,
                                                           unique_ptr<LogicalOperator> plan,
                                                           unique_ptr<CreateIndexInfo> create_info,
                                                           unique_ptr<AlterTableInfo> alter_info) {
	D_ASSERT(plan->type == LogicalOperatorType::LOGICAL_GET);
	IndexBinder index_binder(binder, binder.context);
	return index_binder.BindCreateIndex(binder.context, std::move(create_info), table_entry, std::move(plan),
	                                    std::move(alter_info));
}

BoundStatement Binder::BindAlterAddIndex(BoundStatement &result, CatalogEntry &entry,
                                         unique_ptr<AlterInfo> alter_info) {
	if (entry.type != CatalogType::TABLE_ENTRY) {
		throw BinderException("Cannot execute the `ALTER TABLE` statement on `%s`, only `Table` entries are accepted.",
		                      CatalogTypeToString(entry.type));
	}
	auto &table_info = alter_info->Cast<AlterTableInfo>();
	auto &constraint_info = table_info.Cast<AddConstraintInfo>();
	auto &table = entry.Cast<TableCatalogEntry>();
	auto &column_list = table.GetColumns();

	auto bound_constraint =
	    BindUniqueConstraint(*constraint_info.constraint, table_info.GetQualifiedName().Name(), column_list);
	VerifyConstraintCheckModeStorageVersion(*constraint_info.constraint, table.ParentCatalog(), table.temporary);
	auto &bound_unique = bound_constraint->Cast<BoundUniqueConstraint>();
	auto &unique_constraint = constraint_info.constraint->Cast<UniqueConstraint>();

	// Create the CreateIndexInfo.
	auto create_index_info = make_uniq<CreateIndexInfo>();
	create_index_info->table = table_info.GetQualifiedName().Name();
	create_index_info->index_type = ART::TYPE_NAME;
	create_index_info->constraint_type = unique_constraint.GetIndexConstraintType();

	for (const auto &physical_index : bound_unique.keys) {
		auto &col = column_list.GetColumn(physical_index);
		unique_ptr<ParsedExpression> parsed =
		    make_uniq<ColumnRefExpression>(col.GetName(), table_info.GetQualifiedName().Name());
		create_index_info->expressions.push_back(parsed->Copy());
		create_index_info->parsed_expressions.push_back(parsed->Copy());
	}

	auto index_name = unique_constraint.GetName(table_info.GetQualifiedName().Name());
	create_index_info->SetIndexName(index_name);
	D_ASSERT(!create_index_info->GetIndexName().empty());

	// Plan the table scan.
	TableDescription table_description(table_info.GetQualifiedName());
	auto table_ref = make_uniq<BaseTableRef>(table_description);
	auto bound_table = Bind(*table_ref);
	if (bound_table.plan->type != LogicalOperatorType::LOGICAL_GET) {
		throw BinderException("can only add an index to a base table");
	}
	auto &get = bound_table.plan->Cast<LogicalGet>();
	get.names = StringsToIdentifiers(column_list.GetColumnNames());

	auto alter_table_info = unique_ptr_cast<AlterInfo, AlterTableInfo>(std::move(alter_info));
	result.plan = table.catalog.BindAlterAddIndex(*this, table, std::move(bound_table.plan),
	                                              std::move(create_index_info), std::move(alter_table_info));
	return std::move(result);
}

static void BindAlterTypes(Binder &binder, AlterStatement &stmt) {
	if (stmt.info->type == AlterType::ALTER_TABLE) {
		auto &table_info = stmt.info->Cast<AlterTableInfo>();
		switch (table_info.alter_table_type) {
		case AlterTableType::ADD_COLUMN: {
			auto &add_info = table_info.Cast<AddColumnInfo>();
			binder.BindLogicalType(add_info.new_column.TypeMutable());
		} break;
		case AlterTableType::ADD_FIELD: {
			auto &add_info = table_info.Cast<AddFieldInfo>();
			binder.BindLogicalType(add_info.new_field.TypeMutable());
		} break;
		case AlterTableType::ALTER_COLUMN_TYPE: {
			auto &alter_column_info = table_info.Cast<ChangeColumnTypeInfo>();
			binder.BindLogicalType(alter_column_info.target_type);
			if (!alter_column_info.expression) {
				// without USING, the column is cast to the target type
				alter_column_info.expression = make_uniq<CastExpression>(
				    alter_column_info.target_type, make_uniq<ColumnRefExpression>(alter_column_info.column_path));
			}
		} break;
		default:
			break;
		}
	}
}

BoundStatement Binder::BindAlterMacro(AlterStatement &stmt) {
	BoundStatement result;
	result.names = {"Success"};
	result.types = {LogicalType::BOOLEAN};
	auto &properties = GetStatementProperties();
	properties.return_type = StatementReturnType::NOTHING;

	auto &info = stmt.info->Cast<RemoveMacroOverloadInfo>();
	info.SetQualifiedName(BindTableName(info.GetQualifiedName()));
	auto catalog = Catalog::GetCatalogEntry(context, info.GetQualifiedName().Catalog());
	if (catalog) {
		properties.RegisterDBRead(*catalog, context);
	}
	optional_ptr<CatalogEntry> entry;
	if (info.entry_catalog_type == CatalogType::MACRO_ENTRY) {
		// DROP MACRO also names a table macro, the scalar one wins
		EntryLookupInfo macro_lookup(CatalogType::MACRO_ENTRY, info.GetQualifiedName());
		entry = Catalog::GetEntry(context, macro_lookup, OnEntryNotFound::RETURN_NULL);
		if (!entry) {
			EntryLookupInfo table_macro_lookup(CatalogType::TABLE_MACRO_ENTRY, info.GetQualifiedName());
			entry = Catalog::GetEntry(context, table_macro_lookup, OnEntryNotFound::RETURN_NULL);
			if (entry) {
				info.entry_catalog_type = CatalogType::TABLE_MACRO_ENTRY;
			}
		}
		if (!entry) {
			entry = Catalog::GetEntry(context, macro_lookup, info.if_not_found);
		}
	} else {
		EntryLookupInfo lookup(info.entry_catalog_type, info.GetQualifiedName());
		entry = Catalog::GetEntry(context, lookup, info.if_not_found);
	}
	if (!entry) {
		// IF EXISTS on a missing entry: the signature types are not resolved
		result.plan = make_uniq<LogicalAlter>(std::move(stmt.info));
		return result;
	}
	if (entry->internal) {
		throw CatalogException("Cannot drop internal catalog entry %s!", entry->name);
	}
	// keep the entry's full (possibly nested) schema path so execution navigates the same schema
	info.SetQualifiedName(entry->ParentSchema().GetQualifiedName(info.GetQualifiedName().Name()));
	// resolve the signature's type names from the entry's own schema first, with the session path behind
	// it, so a same-named type elsewhere cannot make an overload unreachable or name a different member
	auto scoped_binder = Binder::CreateBinder(context, this);
	scoped_binder->EntryRetriever().SetSearchPath(
	    scoped_binder->GetSearchPath(entry->ParentCatalog(), info.GetQualifiedName().SchemaPath(), true));
	for (auto &type : info.signature) {
		scoped_binder->BindLogicalType(type);
	}
	auto &macro = entry->Cast<MacroCatalogEntry>();
	if (macro.macros.size() == 1 && macro.FindMember(info.signature).IsValid()) {
		// removing the last overload drops the macro: plan the DROP, which honors CASCADE like any entry drop
		auto drop_info = make_uniq<DropInfo>();
		drop_info->type = info.entry_catalog_type;
		drop_info->SetQualifiedName(info.GetQualifiedName());
		drop_info->if_not_found = info.if_not_found;
		drop_info->cascade = info.cascade;
		properties.RegisterDBModify(entry->ParentCatalog(), context, drop_info->GetModifications());
		result.plan = make_uniq<LogicalDrop>(std::move(drop_info));
		return result;
	}
	properties.RegisterDBModify(entry->ParentCatalog(), context, info.GetModifications());
	result.plan = make_uniq<LogicalAlter>(std::move(stmt.info));
	return result;
}

BoundStatement Binder::Bind(AlterStatement &stmt) {
	BoundStatement result;
	result.names = {"Success"};
	result.types = {LogicalType::BOOLEAN};

	// Special handling for ALTER DATABASE - doesn't use schema binding
	if (stmt.info->type == AlterType::ALTER_DATABASE) {
		auto &properties = GetStatementProperties();
		properties.return_type = StatementReturnType::NOTHING;
		properties.RegisterDBModify(Catalog::GetSystemCatalog(context), context, DatabaseModificationType::ALTER_TABLE);
		result.plan = make_uniq<LogicalAlter>(std::move(stmt.info));
		return result;
	}

	if (stmt.info->type == AlterType::ALTER_SCHEMA) {
		// resolve the schema path the same way as CREATE SCHEMA does
		auto &info = stmt.info->Cast<AlterSchemaInfo>();
		info.SetQualifiedName(ResolveCatalog(context, info.GetQualifiedName()));
		auto &catalog = Catalog::GetCatalog(context, info.SchemaCatalog());
		auto &properties = GetStatementProperties();
		properties.return_type = StatementReturnType::NOTHING;
		properties.RegisterDBModify(catalog, context, DatabaseModificationType::ALTER_TABLE);
		result.plan = make_uniq<LogicalAlter>(std::move(stmt.info));
		return result;
	}

	if (stmt.info->type == AlterType::ALTER_MACRO) {
		return BindAlterMacro(stmt);
	}

	// resolve the (possibly nested) catalog/schema qualification of the altered entry
	stmt.info->SetQualifiedName(BindTableName(stmt.info->GetQualifiedName()));

	optional_ptr<CatalogEntry> entry;
	if (stmt.info->type == AlterType::SET_COLUMN_COMMENT) {
		// Extra step for column comments: They can alter a table or a view, and we resolve that here.
		auto &info = stmt.info->Cast<SetColumnCommentInfo>();
		entry = info.TryResolveCatalogEntry(entry_retriever);
		if (entry && info.catalog_entry_type == CatalogType::VIEW_ENTRY) {
			// when running SET COLUMN on a VIEW - ensure the view is bound
			auto &view = entry->Cast<ViewCatalogEntry>();
			view.BindView(context);
		}
	} else {
		// For any other ALTER, we retrieve the catalog entry directly.
		EntryLookupInfo lookup_info(stmt.info->GetCatalogType(), stmt.info->GetQualifiedName());
		entry = entry_retriever.GetEntry(lookup_info, stmt.info->if_not_found);
	}

	auto &properties = GetStatementProperties();
	properties.return_type = StatementReturnType::NOTHING;
	if (!entry) {
		// Bind types in this binder
		BindAlterTypes(*this, stmt);

		result.plan = make_uniq<LogicalAlter>(std::move(stmt.info));
		return result;
	}

	D_ASSERT(!entry->deleted);
	auto &catalog = entry->ParentCatalog();

	// Bind types in the same catalog as the entry
	auto type_binder = Binder::CreateBinder(context, *this);
	type_binder->SetSearchPath(catalog, stmt.info->GetQualifiedName().SchemaPath());

	BindAlterTypes(*type_binder, stmt);

	if (catalog.IsSystemCatalog()) {
		throw BinderException("Can not comment on System Catalog entries");
	}
	properties.RegisterDBModify(catalog, context, stmt.info->GetModifications());
	stmt.info->SetQualifiedName(entry->ParentSchema().GetQualifiedName(stmt.info->GetQualifiedName().Name()));

	if (!stmt.info->IsAddUniqueConstraint()) {
		result.plan = make_uniq<LogicalAlter>(std::move(stmt.info));
		return result;
	}

	return BindAlterAddIndex(result, *entry, std::move(stmt.info));
}

} // namespace duckdb
