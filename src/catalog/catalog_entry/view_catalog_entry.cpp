#include "duckdb/catalog/catalog_entry/view_catalog_entry.hpp"

#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"
#include "duckdb/parser/parsed_data/create_view_info.hpp"
#include "duckdb/parser/parsed_data/comment_on_column_info.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/catalog/catalog.hpp"

namespace duckdb {

void ViewCatalogEntry::Initialize(CreateViewInfo &info) {
	query = std::move(info.query);
	this->aliases = info.aliases;
	this->security_type = info.security_type;
	if (!info.types.empty()) {
		bind_state = ViewBindState::BOUND;
		view_columns = make_shared_ptr<ViewColumnInfo>();
		view_columns->types = info.types;
		view_columns->names = info.names;
		if (view_columns->names.empty()) {
			// DuckDB v0.9.2 and below store their names in the "aliases" field
			view_columns->names = info.aliases;
		}
		if (view_columns->types.size() != view_columns->names.size()) {
			throw InvalidInputException(
			    "Error creating view %s - view types / names size mismatch (%d types, %d names)", name,
			    view_columns->types.size(), view_columns->names.size());
		}
	}
	this->temporary = info.temporary;
	this->sql = info.sql;
	this->internal = info.internal;
	this->dependencies = info.dependencies;
	this->comment = info.comment;
	this->tags = info.tags;
	this->column_comments = info.column_comments_map;
}

ViewCatalogEntry::ViewCatalogEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateViewInfo &info)
    : StandardEntry(CatalogType::VIEW_ENTRY, schema, catalog, info.GetViewName()), bind_state(ViewBindState::UNBOUND) {
	Initialize(info);
}

unique_ptr<CreateInfo> ViewCatalogEntry::GetInfo() const {
	auto result = make_uniq<CreateViewInfo>();
	result->SetQualifiedName(schema.GetQualifiedName(name));
	result->sql = sql;
	result->query = query ? unique_ptr_cast<SQLStatement, SelectStatement>(query->Copy()) : nullptr;
	result->aliases = aliases;
	result->security_type = security_type;
	auto view_columns = GetColumnInfo();
	if (view_columns) {
		result->names = view_columns->names;
		result->types = view_columns->types;
	}
	result->temporary = temporary;
	result->dependencies = dependencies;
	result->comment = comment;
	result->tags = tags;
	result->column_comments_map = column_comments;
	return std::move(result);
}

unique_ptr<CatalogEntry> ViewCatalogEntry::AlterEntry(ClientContext &context, AlterInfo &info) {
	D_ASSERT(!internal);

	// Column comments have a special alter type
	if (info.type == AlterType::SET_COLUMN_COMMENT) {
		auto &comment_on_column_info = info.Cast<SetColumnCommentInfo>();
		auto resolved_column_name = ResolveColumnName(comment_on_column_info.column_name);
		auto copied_view = Copy(context);
		// apply the comment to the view
		auto &copied_view_entry = copied_view->Cast<ViewCatalogEntry>();
		copied_view_entry.column_comments[resolved_column_name] = comment_on_column_info.comment_value;
		return copied_view;
	}

	// PostgreSQL allows `ALTER TABLE ... RENAME TO` on views, so we convert it
	// to the equivalent ALTER VIEW operation to support tools like dbt-postgres.
	if (info.type == AlterType::ALTER_TABLE) {
		auto &table_info = info.Cast<AlterTableInfo>();
		if (table_info.alter_table_type == AlterTableType::RENAME_TABLE) {
			auto &rename_info = table_info.Cast<RenameTableInfo>();
			auto copied_view = Copy(context);
			copied_view->name = rename_info.new_table_name;
			return copied_view;
		}
	}

	if (info.type != AlterType::ALTER_VIEW) {
		throw CatalogException("Can only modify view with ALTER VIEW statement");
	}
	auto &view_info = info.Cast<AlterViewInfo>();
	switch (view_info.alter_view_type) {
	case AlterViewType::RENAME_VIEW: {
		auto &rename_info = view_info.Cast<RenameViewInfo>();
		auto copied_view = Copy(context);
		copied_view->name = rename_info.new_view_name;
		return copied_view;
	}
	default:
		throw InternalException("Unrecognized alter view type!");
	}
}

shared_ptr<ViewColumnInfo> ViewCatalogEntry::GetColumnInfo() const {
	return view_columns.atomic_load();
}

Value ViewCatalogEntry::GetColumnComment(idx_t column_index) {
	auto view_columns = GetColumnInfo();
	if (!view_columns) {
		throw InternalException("ViewCatalogEntry::GetColumnComment called - but view has not been bound yet");
	}
	auto &names = view_columns->names;
	if (column_index >= names.size()) {
		return Value();
	}
	auto &name = names[column_index];
	auto entry = column_comments.find(name);
	if (entry != column_comments.end()) {
		return entry->second;
	}
	return Value();
}

Identifier ViewCatalogEntry::ResolveColumnName(const Identifier &column_name) const {
	auto view_columns = GetColumnInfo();
	if (!view_columns) {
		return column_name;
	}
	auto &names = view_columns->names;
	for (idx_t i = 0; i < names.size(); i++) {
		auto &visible_name = i < aliases.size() ? aliases[i] : names[i];
		if (visible_name == column_name) {
			return names[i];
		}
	}
	throw BinderException("View %s does not have a column with name %s", name, column_name);
}

void ViewCatalogEntry::BindView(ClientContext &context, BindViewAction action) {
	if (bind_state == ViewBindState::BINDING && bind_thread == ThreadUtil::GetThreadId()) {
		throw InvalidInputException("View %s was requested to be bound but this thread is already binding that "
		                            "view - this likely means the view was attempted to be bound recursively",
		                            name);
	}
	lock_guard<mutex> guard(bind_lock);
	if (action == BindViewAction::BIND_IF_UNBOUND && view_columns) {
		// already bound
		return;
	}
	auto prev_bind_state = bind_state.load();
	bind_state = ViewBindState::BINDING;
	bind_thread = ThreadUtil::GetThreadId();
	try {
		auto columns = make_shared_ptr<ViewColumnInfo>();
		Binder::BindView(context, GetQuery(), ParentCatalog().GetName(), ParentSchema().GetSchemaPath(), nullptr,
		                 aliases, columns->types, columns->names);
		view_columns.atomic_store(columns);
	} catch (...) {
		bind_state = prev_bind_state;
		bind_thread = thread_id {};
		throw;
	}
	bind_state = ViewBindState::BOUND;
	bind_thread = thread_id {};
}

void ViewCatalogEntry::UpdateBinding(const vector<LogicalType> &types_p, const vector<Identifier> &names_p) {
	auto columns = view_columns.atomic_load();
	if (columns && columns->types == types_p && columns->names == names_p) {
		// already bound with the current info
		return;
	}
	lock_guard<mutex> guard(bind_lock);
	auto new_columns = make_shared_ptr<ViewColumnInfo>();
	new_columns->types = types_p;
	new_columns->names = names_p;
	view_columns.atomic_store(new_columns);
	bind_state = ViewBindState::BOUND;
}

string ViewCatalogEntry::ToSQL() const {
	if (sql.empty()) {
		//! Return empty sql with view name so pragma view_tables don't complain
		return sql;
	}
	auto info = GetInfo();
	info->StripCatalogQualification();
	auto result = info->ToString();
	return result;
}

const SelectStatement &ViewCatalogEntry::GetQuery() {
	return *query;
}

unique_ptr<CatalogEntry> ViewCatalogEntry::Copy(ClientContext &context) const {
	D_ASSERT(!internal);
	auto create_info = GetInfo();

	return make_uniq<ViewCatalogEntry>(catalog, schema, create_info->Cast<CreateViewInfo>());
}

} // namespace duckdb
