//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/operator/logical_update.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/planner/bound_constraint.hpp"
#include "duckdb/common/index_map.hpp"
#include "duckdb/common/enums/row_id_handling.hpp"

namespace duckdb {
struct CreateInfo;
class TableCatalogEntry;
class LogicalGet;
class LogicalProjection;

class LogicalUpdate : public LogicalOperator {
public:
	static constexpr const LogicalOperatorType TYPE = LogicalOperatorType::LOGICAL_UPDATE;

public:
	explicit LogicalUpdate(TableCatalogEntry &table);

	//! The base table to update
	TableCatalogEntry &table;
	//! projection index
	TableIndex table_index;
	//! if returning option is used, or transition tables are captured, return the update chunk
	bool return_chunk;
	//! if set, the operator also emits the pre-update (OLD) row image after the NEW image
	bool capture_old_rows = false;
	//! input-chunk index of each captured OLD physical column, in physical table order (only when capture_old_rows)
	vector<idx_t> old_row_columns;
	vector<PhysicalIndex> columns;
	vector<unique_ptr<Expression>> bound_defaults;
	vector<unique_ptr<BoundConstraint>> bound_constraints;
	bool update_is_del_and_insert;
	//! how to handle a target row-id appearing more than once in the input (e.g. UPDATE ... FROM): keep the
	//! lock-free path (ASSUME_UNIQUE), deduplicate keeping the first match (KEEP_FIRST), or error (ERROR)
	RowIdHandling row_id_handling = RowIdHandling::ASSUME_UNIQUE;

public:
	void Serialize(Serializer &serializer) const override;
	static unique_ptr<LogicalOperator> Deserialize(Deserializer &deserializer);

	LogicalPlanSQLExportResult ToSQL(LogicalPlanSQLExportContext &context,
	                                 const LogicalPlanVerificationPath &path) override;

	idx_t EstimateCardinality(ClientContext &context) override;
	string GetName() const override;

	DUCKDB_API static void BindExtraColumns(TableCatalogEntry &table, LogicalGet &get, LogicalProjection &proj,
	                                        LogicalUpdate &update, physical_index_set_t &bound_columns,
	                                        bool reuse_projected_columns = false);
	DUCKDB_API static void BindAllColumns(TableCatalogEntry &table, LogicalGet &get, LogicalProjection &proj,
	                                      LogicalUpdate &update, bool reuse_projected_columns = false);

	static void RewriteInPlaceUpdates(LogicalOperator &update_op);

protected:
	vector<ColumnBinding> GetColumnBindings() override;
	void ResolveTypes() override;

private:
	LogicalUpdate(ClientContext &context, const unique_ptr<CreateInfo> &table_info);
};
} // namespace duckdb
