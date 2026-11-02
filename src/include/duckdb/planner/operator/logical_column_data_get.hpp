//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/operator/logical_column_data_get.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/common/optionally_owned_ptr.hpp"

namespace duckdb {

class ManagedResultSet;

//! LogicalColumnDataGet represents a scan operation from a ColumnDataCollection
class LogicalColumnDataGet : public LogicalOperator {
public:
	LogicalPlanSQLExportResult ToSQL(LogicalPlanSQLExportContext &context,
	                                 const LogicalPlanVerificationPath &path) override;

	static constexpr const LogicalOperatorType TYPE = LogicalOperatorType::LOGICAL_CHUNK_GET;

public:
	LogicalColumnDataGet(TableIndex table_index, vector<LogicalType> types,
	                     unique_ptr<ColumnDataCollection> collection);
	LogicalColumnDataGet(TableIndex table_index, vector<LogicalType> types, ColumnDataCollection &to_scan);
	LogicalColumnDataGet(TableIndex table_index, vector<LogicalType> types,
	                     optionally_owned_ptr<ColumnDataCollection> to_scan);

	//! The table index in the current bind context
	TableIndex table_index;
	//! The types of the chunk
	vector<LogicalType> chunk_types;
	//! Column ids that are scanned from the collection
	vector<column_t> column_ids;
	//! (optionally owned) column data collection
	optionally_owned_ptr<ColumnDataCollection> collection;

public:
	void SetColumnIds(vector<column_t> column_ids);
	const vector<column_t> &GetColumnIds() const;
	vector<ColumnBinding> GetColumnBindings() override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<LogicalOperator> Deserialize(Deserializer &deserializer);

	vector<TableIndex> GetTableIndex() const override;
	string GetName() const override;

protected:
	void ResolveTypes() override {
		types.clear();
		types.reserve(column_ids.size());
		for (auto column_id : column_ids) {
			D_ASSERT(column_id < chunk_types.size());
			types.push_back(chunk_types[column_id]);
		}
	}
};
} // namespace duckdb
