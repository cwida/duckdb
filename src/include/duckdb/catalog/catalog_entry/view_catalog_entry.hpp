//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/catalog_entry/view_catalog_entry.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/standard_entry.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/thread.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/enums/view_security_type.hpp"

namespace duckdb {

class DataTable;
struct CreateViewInfo;

enum class ViewBindState { BOUND, BINDING, UNBOUND };
enum class BindViewAction { BIND_IF_UNBOUND, FORCE_REBIND };

struct ViewColumnInfo {
	vector<LogicalType> types;
	vector<Identifier> names;
};

//! A view catalog entry
class ViewCatalogEntry : public StandardEntry {
public:
	static constexpr const CatalogType Type = CatalogType::VIEW_ENTRY;
	static constexpr const char *Name = "view";

public:
	//! Create a real TableCatalogEntry and initialize storage for it
	ViewCatalogEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateViewInfo &info);

	//! The query of the view
	unique_ptr<SelectStatement> query;
	//! The SQL query (if any)
	string sql;
	//! The set of aliases associated with the view
	vector<Identifier> aliases;
	//! The security type of the view
	ViewSecurityType security_type = ViewSecurityType::REGULAR_VIEW;

	//! Returns the view column info, if the view is bound. Otherwise returns `nullptr`
	virtual shared_ptr<ViewColumnInfo> GetColumnInfo() const;
	//! Bind a view so we know the types / names returned by it
	virtual void BindView(ClientContext &context, BindViewAction action = BindViewAction::BIND_IF_UNBOUND);
	//! Update the view with a new set of types / names
	virtual void UpdateBinding(const vector<LogicalType> &types, const vector<Identifier> &names);
	Value GetColumnComment(idx_t column_index);
	//! Returns the bound name of a visible column
	Identifier ResolveColumnName(const Identifier &column_name) const;

public:
	unique_ptr<CreateInfo> GetInfo() const override;

	unique_ptr<CatalogEntry> AlterEntry(ClientContext &context, AlterInfo &info) override;

	unique_ptr<CatalogEntry> Copy(ClientContext &context) const override;

	virtual const SelectStatement &GetQuery();

	string ToSQL() const override;

private:
	mutable mutex bind_lock;
	//! Columns returned by the view, if bound
	shared_ptr<ViewColumnInfo> view_columns;
	//! The current bind state of the view
	atomic<ViewBindState> bind_state;
	//! Current binding thread
	atomic<thread_id> bind_thread;
	//! The comments on the columns of the view: can be empty if there are no comments
	identifier_map_t<Value> column_comments;

private:
	void Initialize(CreateViewInfo &info);
};
} // namespace duckdb
