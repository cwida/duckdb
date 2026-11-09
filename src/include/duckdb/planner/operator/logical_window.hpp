//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/operator/logical_window.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/logical_operator.hpp"

namespace duckdb {

//! LogicalAggregate represents an aggregate operation with (optional) GROUP BY
//! operator.
class LogicalWindow : public LogicalOperator {
public:
	LogicalPlanSQLExportResult ToSQL(LogicalPlanSQLExportContext &context,
	                                 const LogicalPlanVerificationPath &path) override;

	static constexpr const LogicalOperatorType TYPE = LogicalOperatorType::LOGICAL_WINDOW;

public:
	LogicalWindow(TableIndex window_index, optional_idx partition_limit)
	    : LogicalOperator(LogicalOperatorType::LOGICAL_WINDOW), window_index(window_index),
	      partition_limit(partition_limit) {
	}

	TableIndex window_index;

	//! The maximum number of rows to return per partition
	optional_idx partition_limit;

public:
	vector<ColumnBinding> GetColumnBindings() override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<LogicalOperator> Deserialize(Deserializer &deserializer);
	vector<TableIndex> GetTableIndex() const override;
	string GetName() const override;

protected:
	void ResolveTypes() override;
};
} // namespace duckdb
