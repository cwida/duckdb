//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/operator/logical_copy_to_file.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enums/copy_overwrite_mode.hpp"
#include "duckdb/common/filename_pattern.hpp"
#include "duckdb/common/local_file_system.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/function/copy_function.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/common/enums/preserve_order.hpp"
#include "duckdb/planner/bound_result_modifier.hpp"

namespace duckdb {

class LogicalCopyToFile : public LogicalOperator {
public:
	static constexpr const LogicalOperatorType TYPE = LogicalOperatorType::LOGICAL_COPY_TO_FILE;

public:
	LogicalCopyToFile(CopyFunction function, unique_ptr<FunctionData> bind_data, unique_ptr<CopyInfo> copy_info,
	                  TableIndex table_index)
	    : LogicalOperator(LogicalOperatorType::LOGICAL_COPY_TO_FILE), function(std::move(function)),
	      bind_data(std::move(bind_data)), copy_info(std::move(copy_info)), table_index(table_index) {
	}

public:
	CopyFunction function;
	unique_ptr<FunctionData> bind_data;
	unique_ptr<CopyInfo> copy_info;
	TableIndex table_index;

	std::string file_path;
	bool use_tmp_file;
	FilenamePattern filename_pattern;
	string file_extension;
	CopyOverwriteMode overwrite_mode;
	bool per_thread_output;
	optional_idx batch_size;
	optional_idx batch_size_bytes;
	optional_idx batches_per_file;
	optional_idx file_size_bytes;
	bool rotate;
	CopyFunctionReturnType return_type;

	bool partition_output;
	bool write_partition_columns;
	bool write_empty_file = true;
	bool hive_file_pattern = true;
	PreserveOrderType preserve_order = PreserveOrderType::AUTOMATIC;
	vector<idx_t> partition_columns;
	vector<BoundOrderByNode> order_columns;
	//! Directory of a partition's files relative to file_path (see PhysicalCopyToFile)
	unique_ptr<Expression> partition_path_expression;
	//! Statistics of the partition columns as derived by the optimizer (not serialized)
	vector<unique_ptr<BaseStatistics>> partition_stats;

	vector<Identifier> names;
	vector<LogicalType> expected_types;
	//! The names and types of the copied query before the copy function changes them, for RETURN_COLUMN_TYPES
	vector<Identifier> query_names;
	vector<LogicalType> query_types;

public:
	LogicalPlanSQLExportResult ToSQL(LogicalPlanSQLExportContext &context,
	                                 const LogicalPlanVerificationPath &path) override;

	vector<ColumnBinding> GetColumnBindings() override;
	idx_t EstimateCardinality(ClientContext &context) override;
	void Serialize(Serializer &serializer) const override;
	static unique_ptr<LogicalOperator> Deserialize(Deserializer &deserializer);
	static vector<LogicalType> GetTypesWithoutPartitions(const vector<LogicalType> &col_types,
	                                                     const vector<idx_t> &part_cols, bool write_part_cols);
	static vector<Identifier> GetNamesWithoutPartitions(const vector<Identifier> &col_names,
	                                                    const vector<column_t> &part_cols, bool write_part_cols);
	vector<TableIndex> GetTableIndex() const override;

protected:
	void ResolveTypes() override {
		types = GetCopyFunctionReturnLogicalTypes(return_type);
	}
};
} // namespace duckdb
