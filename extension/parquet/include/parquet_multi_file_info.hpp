//===----------------------------------------------------------------------===//
//                         DuckDB
//
// parquet_multi_file_info.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include <stdint.h>
#include <string>
#include <utility>

#include "duckdb/common/multi_file/multi_file_function.hpp"
#include "parquet_reader.hpp"
#include "duckdb/common/enums/file_glob_options.hpp"
#include "duckdb/common/multi_file/base_file_reader.hpp"
#include "duckdb/common/open_file_info.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/shared_ptr_ipp.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/table_column.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/function/function.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/storage/statistics/node_statistics.hpp"

namespace duckdb {
struct TableFunctionMultiFileSettings;
class ClientContext;
class ExecutionContext;
enum class FileExpandResult : uint8_t;
struct LogicalType;
struct MultiFileBindData;
struct MultiFileGlobalState;
struct MultiFileOptions;

class ParquetFileReaderOptions : public BaseFileReaderOptions {
public:
	explicit ParquetFileReaderOptions(ParquetOptions options_p) : options(std::move(options_p)) {
	}
	explicit ParquetFileReaderOptions(ClientContext &context) : options(context) {
	}

	ParquetOptions options;
};

struct ParquetReadGlobalState : public GlobalTableFunctionState {
	explicit ParquetReadGlobalState(optional_ptr<const PhysicalOperator> op_p) : row_group_index(0), op(op_p) {
	}
	//! Index of row group within file currently up for scanning
	idx_t row_group_index;
	//! (Optional) pointer to physical operator performing the scan
	optional_ptr<const PhysicalOperator> op;
	//! Row groups read but not yet reported to the profiler
	atomic<idx_t> row_groups_scanned_unreported {0};
	//! Compressed size of their column chunks
	atomic<idx_t> bytes_scanned_unreported {0};
	//! Total considered, across all scan states
	atomic<idx_t> total_row_groups_to_scan {0};
};

struct ParquetReadLocalState : public LocalTableFunctionState {
	ParquetReaderScanState scan_state;
	idx_t group_index;
};

struct ParquetMultiFileInfo : MultiFileReaderInterface {
	static unique_ptr<MultiFileReaderInterface> CreateInterface(ClientContext &context);

	unique_ptr<BaseFileReaderOptions> InitializeOptions(ClientContext &context,
	                                                    optional_ptr<TableFunctionInfo> info) override;
	bool ParseCopyOption(ClientContext &context, const Identifier &key, const vector<Value> &values,
	                     BaseFileReaderOptions &options, vector<Identifier> &expected_names,
	                     vector<LogicalType> &expected_types) override;
	bool ParseOption(ClientContext &context, const Identifier &key, const Value &val, MultiFileOptions &file_options,
	                 BaseFileReaderOptions &options) override;
	void BindReader(ClientContext &context, vector<LogicalType> &return_types, vector<Identifier> &names,
	                MultiFileBindData &bind_data) override;
	unique_ptr<TableFunctionData> InitializeBindData(MultiFileBindData &multi_file_data,
	                                                 unique_ptr<BaseFileReaderOptions> options) override;
	void FinalizeBindData(MultiFileBindData &multi_file_data) override;
	void GetBindInfo(const TableFunctionData &bind_data, BindInfo &info) override;
	optional_idx MaxThreads(const MultiFileBindData &bind_data, const MultiFileGlobalState &global_state,
	                        FileExpandResult expand_result) override;
	unique_ptr<GlobalTableFunctionState> InitializeGlobalState(ClientContext &context, MultiFileBindData &bind_data,
	                                                           MultiFileGlobalState &global_state) override;
	unique_ptr<LocalTableFunctionState> InitializeLocalState(ClientContext &, GlobalTableFunctionState &) override;

	bool SupportsReadAhead(const MultiFileBindData &bind_data) const override {
		return true;
	}
	shared_ptr<BaseFileReader> CreateReader(ClientContext &context, GlobalTableFunctionState &gstate,
	                                        BaseUnionData &union_data, const MultiFileBindData &bind_data_p) override;
	shared_ptr<BaseFileReader> CreateReader(ClientContext &context, GlobalTableFunctionState &gstate,
	                                        const OpenFileInfo &file, idx_t file_idx,
	                                        const MultiFileBindData &bind_data) override;
	shared_ptr<BaseFileReader> CreateReader(ClientContext &context, const OpenFileInfo &file,
	                                        BaseFileReaderOptions &options,
	                                        const MultiFileOptions &file_options) override;
	unique_ptr<NodeStatistics> GetCardinality(ClientContext &context, const MultiFileBindData &bind_data,
	                                          idx_t file_count) override;
	void GetVirtualColumns(ClientContext &context, MultiFileBindData &bind_data, virtual_column_map_t &result) override;
	unique_ptr<MultiFileReaderInterface> Copy() override;
	FileGlobInput GetGlobInput() override;
};

class ParquetScanFunction {
public:
	//! The single-file parquet reader that the multi-file reader is built on
	static TableFunction GetSingleFileFunction();
	//! How the single-file parquet reader is wrapped into the multi-file reader
	static TableFunctionMultiFileSettings GetMultiFileSettings();
	//! The named parameters that both of the above accept
	static void AddNamedParameters(TableFunction &table_function);
	//! The multi-file parquet reader, built by wrapping the single-file reader above
	static TableFunction GetMultiFileFunction(Identifier name);
	//! Push strlen/octet_length on a column into the multi-file parquet reader of a single file
	static bool ProjectionExpressionPushdown(ClientContext &context,
	                                         const TableFunctionProjectionExpressionInput &input);
	//! The options a file is read with, given the bind data of the single-file reader for it
	static const ParquetOptions &GetFileOptions(const FunctionData &file_bind_data);
};

} // namespace duckdb
