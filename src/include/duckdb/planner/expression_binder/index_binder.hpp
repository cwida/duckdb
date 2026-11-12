//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/expression_binder/index_binder.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/execution/index/bound_index.hpp"
#include "duckdb/execution/index/unbound_index.hpp"
#include "duckdb/parser/parsed_data/create_index_info.hpp"
#include "duckdb/planner/expression_binder.hpp"

namespace duckdb {

class BoundColumnRefExpression;
class LogicalOperator;

//! The IndexBinder binds indexes and expressions within index statements.
class IndexBinder : public ExpressionBinder {
public:
	IndexBinder(Binder &binder, ClientContext &context, optional_ptr<TableCatalogEntry> table = nullptr,
	            optional_ptr<CreateIndexInfo> info = nullptr);

	//! Binds a stored index. An index whose stored keys the engines of its storage version compute otherwise is
	//! refused, or left unbound (nullptr) when skip_mismatched_keys is set, for a read that can do without it
	unique_ptr<BoundIndex> BindIndex(const UnboundIndex &index, bool skip_mismatched_keys = false);
	unique_ptr<LogicalOperator> BindCreateIndex(ClientContext &context, unique_ptr<CreateIndexInfo> create_index_info,
	                                            TableCatalogEntry &table_entry, unique_ptr<LogicalOperator> plan,
	                                            unique_ptr<AlterTableInfo> alter_table_info);

	static void InitCreateIndexInfo(LogicalGet &get, CreateIndexInfo &info);
	//! Deprecated: the schema is derived from the table the index is created on
	static void InitCreateIndexInfo(LogicalGet &get, CreateIndexInfo &info, const Identifier &schema);

protected:
	BindResult BindExpression(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth,
	                          bool root_expression = false) override;
	string UnsupportedAggregateMessage() override;

private:
	// Only for WAL replay.
	optional_ptr<TableCatalogEntry> table;
	optional_ptr<CreateIndexInfo> info;
};

} // namespace duckdb
