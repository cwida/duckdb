//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/copy_function.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/optional_idx.hpp"
#include "duckdb/function/function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parsed_data/copy_info.hpp"
#include "duckdb/parser/statement/copy_statement.hpp"
#include "duckdb/common/enums/copy_option_mode.hpp"
#include "duckdb/common/insertion_order_preserving_map.hpp"
#include "duckdb/common/optional_ptr.hpp"

namespace duckdb {

struct BoundStatement;
struct CopyFunctionFileStatistics;
class BaseStatistics;
class Binder;
class ColumnDataCollection;
class ExecutionContext;
class PhysicalOperatorLogger;

struct CopyFunctionInfo {
	virtual ~CopyFunctionInfo() = default;

	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}
	template <class TARGET>
	const TARGET &Cast() const {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<const TARGET &>(*this);
	}
};

struct LocalFunctionData {
	virtual ~LocalFunctionData() = default;

	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}
	template <class TARGET>
	const TARGET &Cast() const {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<const TARGET &>(*this);
	}
};

struct GlobalFunctionData {
	virtual ~GlobalFunctionData() = default;

	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}
	template <class TARGET>
	const TARGET &Cast() const {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<const TARGET &>(*this);
	}
};

struct PreparedBatchData {
	virtual ~PreparedBatchData() = default;

	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}
	template <class TARGET>
	const TARGET &Cast() const {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<const TARGET &>(*this);
	}
};

struct CopyFunctionBindInput {
	explicit CopyFunctionBindInput(const CopyInfo &info_p, shared_ptr<CopyFunctionInfo> function_info = nullptr)
	    : info(info_p), function_info(std::move(function_info)) {
	}

	const CopyInfo &info;
	shared_ptr<CopyFunctionInfo> function_info;
	string file_extension;
};

struct CopyFromFunctionBindInput {
	explicit CopyFromFunctionBindInput(const CopyInfo &info_p, TableFunction &tf_p) : info(info_p), tf(tf_p) {
	}

	const CopyInfo &info;
	TableFunction &tf;
};

struct CopyToSelectInput {
	ClientContext &context;
	identifier_map_t<vector<Value>> &options;
	vector<unique_ptr<Expression>> select_list;
	CopyToType copy_to_type;
};

struct CopyOption {
	CopyOption();
	explicit CopyOption(LogicalType type_p, CopyOptionMode mode = CopyOptionMode::READ_WRITE);

	LogicalType type;
	CopyOptionMode mode;
};

struct CopyOptionsInput {
	explicit CopyOptionsInput(identifier_map_t<CopyOption> &options) : options(options) {
	}

	identifier_map_t<CopyOption> &options;
};

struct CopyToPropagateStatsInput {
	ClientContext &context;
	FunctionData &bind_data;
	const vector<optional_ptr<BaseStatistics>> &column_stats;
};

enum class CopyFunctionExecutionMode { REGULAR_COPY_TO_FILE, PARALLEL_COPY_TO_FILE, BATCH_COPY_TO_FILE };

typedef BoundStatement (*copy_to_plan_t)(Binder &binder, CopyStatement &stmt);
typedef void (*copy_options_t)(ClientContext &context, CopyOptionsInput &input);
typedef unique_ptr<FunctionData> (*copy_to_bind_t)(ClientContext &context, CopyFunctionBindInput &input,
                                                   const vector<Identifier> &names,
                                                   const vector<LogicalType> &sql_types);
typedef unique_ptr<LocalFunctionData> (*copy_to_initialize_local_t)(ExecutionContext &context, FunctionData &bind_data);
typedef unique_ptr<GlobalFunctionData> (*copy_to_initialize_global_t)(ClientContext &context, FunctionData &bind_data,
                                                                      const string &file_path);
typedef void (*copy_to_sink_t)(ExecutionContext &context, FunctionData &bind_data, GlobalFunctionData &gstate,
                               LocalFunctionData &lstate, DataChunk &input);
typedef void (*copy_to_combine_t)(ExecutionContext &context, FunctionData &bind_data, GlobalFunctionData &gstate,
                                  LocalFunctionData &lstate);
typedef void (*copy_to_finalize_t)(ClientContext &context, FunctionData &bind_data, GlobalFunctionData &gstate);

typedef void (*copy_to_serialize_t)(Serializer &serializer, const FunctionData &bind_data,
                                    const CopyFunction &function);

typedef unique_ptr<FunctionData> (*copy_to_deserialize_t)(Deserializer &deserializer, CopyFunction &function);

typedef unique_ptr<FunctionData> (*copy_from_bind_t)(ClientContext &context, CopyFromFunctionBindInput &info,
                                                     vector<Identifier> &expected_names,
                                                     vector<LogicalType> &expected_types);
typedef CopyFunctionExecutionMode (*copy_to_execution_mode_t)(bool preserve_insertion_order, bool supports_batch_index);

typedef unique_ptr<PreparedBatchData> (*copy_prepare_batch_t)(ClientContext &context, FunctionData &bind_data,
                                                              GlobalFunctionData &gstate,
                                                              unique_ptr<ColumnDataCollection> collection);
typedef void (*copy_flush_batch_t)(ClientContext &context, FunctionData &bind_data, GlobalFunctionData &gstate,
                                   PreparedBatchData &batch);

typedef optional_idx (*copy_default_batch_size_t)();
typedef optional_idx (*copy_default_batch_size_bytes_t)();
typedef idx_t (*copy_file_size_bytes_t)(GlobalFunctionData &gstate);

typedef optional_idx (*copy_desired_batch_size_t)(ClientContext &context, FunctionData &bind_data);

typedef void (*copy_to_get_written_statistics_t)(ClientContext &context, FunctionData &bind_data,
                                                 GlobalFunctionData &gstate, CopyFunctionFileStatistics &statistics);

typedef vector<unique_ptr<Expression>> (*copy_to_select_t)(CopyToSelectInput &input);

typedef void (*copy_to_initialize_operator_t)(GlobalFunctionData &gstate, const PhysicalOperator &op);

typedef void (*copy_to_propagate_statistics_t)(CopyToPropagateStatsInput &input);

enum class CopyFunctionReturnType : uint8_t {
	CHANGED_ROWS = 0,
	CHANGED_ROWS_AND_FILE_LIST = 1,
	WRITTEN_FILE_STATISTICS = 2,
	//! The above, followed by a "columns" column with the name and type of every copied column (RETURN_COLUMN_TYPES)
	CHANGED_ROWS_AND_COLUMN_TYPES = 3,
	CHANGED_ROWS_AND_FILE_LIST_AND_COLUMN_TYPES = 4,
	WRITTEN_FILE_STATISTICS_AND_COLUMN_TYPES = 5
};
vector<Identifier> GetCopyFunctionReturnNames(CopyFunctionReturnType return_type);
vector<LogicalType> GetCopyFunctionReturnLogicalTypes(CopyFunctionReturnType return_type);
//! Whether the result ends with the "columns" column of RETURN_COLUMN_TYPES
bool CopyFunctionReturnsColumnTypes(CopyFunctionReturnType return_type);
//! The return type without the "columns" column of RETURN_COLUMN_TYPES
CopyFunctionReturnType GetCopyFunctionBaseReturnType(CopyFunctionReturnType return_type);
//! The return type with the "columns" column of RETURN_COLUMN_TYPES
CopyFunctionReturnType GetCopyFunctionReturnTypeWithColumnTypes(CopyFunctionReturnType return_type);

struct CopyFunctionFileStatistics {
	idx_t row_count = 0;
	idx_t file_size_bytes = 0;
	Value footer_size_bytes;
	//! Format-specific statistics (e.g. row_group_count for Parquet)
	InsertionOrderPreservingMap<Value> extra_info;
	//! map of column name -> statistics name -> statistics value
	case_insensitive_map_t<case_insensitive_map_t<Value>> column_statistics;
};

enum class CopyFunctionFlushBatchReason : uint8_t {
	//! Flush because of current batch size
	BATCH_SIZE,
	//! Flush because of current batch size in bytes
	BATCH_SIZE_BYTES,
	//! Flush because it's the last batch
	FORCED_FLUSH
};

struct CopyFunctionBatchAnalyzer {
public:
	CopyFunctionBatchAnalyzer(const idx_t &current_batch_size, const idx_t &current_batch_size_bytes,
	                          const optional_idx &batch_size, const optional_idx &batch_size_bytes);
	CopyFunctionBatchAnalyzer(const ColumnDataCollection &batch, const optional_idx &batch_size,
	                          const optional_idx &batch_size_bytes);

public:
	bool MeetsFlushCriteria() const;
	CopyFunctionFlushBatchReason ToReason() const;
	bool IsAcceptable() const;

private:
	bool AnyBatchQualifies() const;
	bool ExceedsBatchSize() const;
	bool ExceedsBatchSizeBytes() const;

	int64_t BatchSizeVectorDiff() const;
	int64_t BatchSizeBytesVectorDiff() const;

public:
	const idx_t current_batch_size;
	const idx_t current_batch_size_bytes;

	const optional_idx batch_size;
	const optional_idx batch_size_bytes;
};

class CopyFunction : public Function { // NOLINT: work-around bug in clang-tidy
public:
	explicit CopyFunction(const Identifier &name);

	//! Plan rewrite copy function
	copy_to_plan_t plan;
	copy_to_select_t copy_to_select;
	copy_to_bind_t copy_to_bind;
	copy_options_t copy_options;
	copy_to_initialize_local_t copy_to_initialize_local;
	copy_to_initialize_global_t copy_to_initialize_global;
	copy_to_get_written_statistics_t copy_to_get_written_statistics;
	copy_to_sink_t copy_to_sink;
	copy_to_combine_t copy_to_combine;
	copy_to_finalize_t copy_to_finalize;
	copy_to_execution_mode_t execution_mode;
	copy_to_initialize_operator_t initialize_operator;
	copy_to_propagate_statistics_t copy_to_propagate_statistics = nullptr;

	copy_prepare_batch_t prepare_batch;
	copy_flush_batch_t flush_batch;
	copy_file_size_bytes_t file_size_bytes;
	copy_desired_batch_size_t desired_batch_size;

	copy_to_serialize_t serialize;
	copy_to_deserialize_t deserialize;

	copy_from_bind_t copy_from_bind;
	TableFunction copy_from_function;

	string extension;

	//! Additional function info, passed to the bind
	shared_ptr<CopyFunctionInfo> function_info;
};

} // namespace duckdb
