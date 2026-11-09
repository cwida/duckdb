//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/persistent/physical_copy_to_file.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enums/copy_overwrite_mode.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/filename_pattern.hpp"
#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/function/copy_function.hpp"
#include "duckdb/parser/parsed_data/copy_info.hpp"
#include "duckdb/storage/storage_lock.hpp"

namespace duckdb {

struct GlobalFileState;
struct FileStateHandle;
struct BoundOrderByNode;
class CopyOutputLifecycle;

struct CopyToFileInfo {
	explicit CopyToFileInfo(string file_path_p) : file_path(std::move(file_path_p)) {
	}

	string file_path;
	unique_ptr<CopyFunctionFileStatistics> file_stats;
	Value partition_keys;
};

//! The naming, size limit and overwrite mode of the files a COPY writes
struct CopyToFileOptions {
	//! Pattern to use for the file names
	FilenamePattern filename_pattern;
	//! File extension, e.g., "parquet"
	string file_extension;
	//! The size at which a file is rotated, if files are rotated by size
	optional_idx file_size_bytes;
	//! What to do when the "file_path" conflicts with an existing file
	CopyOverwriteMode overwrite_mode = CopyOverwriteMode::COPY_ERROR_ON_CONFLICT;

	bool RotatesFiles() const {
		return file_size_bytes.IsValid();
	}
};

//! The source state of a COPY, paging through the written files for RETURN_STATS
class CopyToFileGlobalSourceState : public GlobalSourceState {
public:
	idx_t offset = 0;

	idx_t MaxThreads() override {
		return 1;
	}
};

//! Copy the contents of a query into a table
class PhysicalCopyToFile : public PhysicalOperator {
public:
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::COPY_TO_FILE;

public:
	PhysicalCopyToFile(PhysicalPlan &physical_plan, vector<LogicalType> types, CopyFunction function,
	                   unique_ptr<FunctionData> bind_data, idx_t estimated_cardinality);
	~PhysicalCopyToFile() override;

public:
	InsertionOrderPreservingMap<string> ParamsToString() const override;

public:
	static string GetTrimmedPath(ClientContext &context, const string &file_path);
	//! Checks an output directory for existing files according to the overwrite mode
	static void CheckDirectory(FileSystem &fs, const string &file_path, CopyOverwriteMode overwrite_mode);
	//! Creates the output directory of a copy that writes several files, or checks the existing one
	static void PrepareOutputDirectory(FileSystem &fs, const string &file_path, CopyOverwriteMode overwrite_mode,
	                                   CopyOutputLifecycle &output_lifecycle);
	static void MoveTmpFile(ClientContext &context, const string &tmp_file_path);
	static string GetNonTmpFile(ClientContext &context, const string &tmp_file_path);
	static void ReturnStatistics(Context &context, DataChunk &chunk, const string &file_path,
	                             const CopyToFileInfo &info);
	//! Appends the next chunk of RETURN_STATS rows of the written files
	static SourceResultType ReturnWrittenFileStatistics(ClientContext &context, DataChunk &chunk,
	                                                    CopyToFileGlobalSourceState &source_state,
	                                                    const vector<unique_ptr<CopyToFileInfo>> &written_files,
	                                                    bool use_tmp_file);
	//! Adds the RETURN_STATS row of a COPY that wrote no file, so that RETURN_COLUMN_TYPES can still return the types
	static void ReturnNoFileStatistics(DataChunk &chunk);
	//! Sets the last column of every row in the chunk to the RETURN_COLUMN_TYPES value
	static void ReturnColumnTypes(DataChunk &chunk, const vector<Identifier> &query_names,
	                              const vector<LogicalType> &query_types);

	bool Rotate() const;

	void PrepareAndFlushBatch(ClientContext &context, GlobalSinkState &gstate_p, FileStateHandle &file_state,
	                          const std::function<void(FileStateHandle &)> &create_file_state_fun,
	                          unique_ptr<ColumnDataCollection> batch) const;
	pair<const CopyFunctionBatchAnalyzer, unique_ptr<PreparedBatchData>>
	PrepareBatch(ClientContext &context, GlobalSinkState &gstate_p, FileStateHandle &file_state,
	             const std::function<void(FileStateHandle &)> &create_file_state_fun,
	             unique_ptr<ColumnDataCollection> batch) const;
	void FlushBatch(ClientContext &context, GlobalSinkState &gstate_p, FileStateHandle &file_state,
	                const std::function<void(FileStateHandle &)> &create_file_state_fun,
	                const CopyFunctionBatchAnalyzer &batch_analyzer,
	                unique_ptr<PreparedBatchData> prepared_batch) const;

public:
	//===--------------------------------------------------------------------===//
	// Sink Interface
	//===--------------------------------------------------------------------===//
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override;
	SinkCombineResultType Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const override;
	SinkFinalizeType Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
	                          OperatorSinkFinalizeInput &input) const override;
	//! Synchronously complete any pending partitioned-copy flush work after Finalize has been called.
	//! Used by callers (e.g. DuckLakeUpdate) that drive Sink/Finalize/GetData manually outside a pipeline.
	void FinalizePartitionedSync(ExecutionContext &execution_context, InterruptState &interrupt_state) const;
	unique_ptr<LocalSinkState> GetLocalSinkState(ExecutionContext &context) const override;
	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override;

	bool IsSink() const override {
		return true;
	}

	bool SinkOrderDependent() const override {
		return true;
	}

	bool ParallelSink() const override {
		return per_thread_output || partition_output || parallel;
	}

public:
	//===--------------------------------------------------------------------===//
	// Source Interface
	//===--------------------------------------------------------------------===//
	unique_ptr<GlobalSourceState> GetGlobalSourceState(ClientContext &context) const override;
	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                 OperatorSourceInput &input) const override;

	bool IsSource() const override {
		return true;
	}

public:
	//! Function and associated bind data
	CopyFunction function;
	unique_ptr<FunctionData> bind_data;

	//! Names and types going into the file(s)
	vector<Identifier> names;
	vector<LogicalType> expected_types;

	//! Where to write the file
	string file_path;
	//! The naming, size limit and overwrite mode of the files
	CopyToFileOptions file_options;

	//! Whether we can write in parallel
	bool parallel;
	//! Whether each threads creates their own output file
	bool per_thread_output;

	//! Fine-grained control over writes
	optional_idx batch_size;
	optional_idx batch_size_bytes;
	optional_idx batches_per_file;

	//! Whether to write to a temp file before writing to the file at "file_path"
	bool use_tmp_file;
	//! What to return, e.g., number of written rows
	CopyFunctionReturnType return_type;
	//! The names and types of the copied query, for RETURN_COLUMN_TYPES
	vector<Identifier> query_names;
	vector<LogicalType> query_types;
	//! Whether to write an empty file if there was no data
	bool write_empty_file;

	//! Partitioning-related
	vector<idx_t> partition_columns;
	bool partition_output;
	bool write_partition_columns;
	bool hive_file_pattern;
	//! Directory (relative to file_path) of a partition's files, replacing the hive layout if set. Evaluated per
	//! partition over a single row holding the partition values, in partition_columns order.
	unique_ptr<Expression> partition_path_expression;
	//! Statistics of the partition columns, if known
	vector<unique_ptr<BaseStatistics>> partition_stats;

	//! If the data should be sorted
	vector<BoundOrderByNode> order_columns;
};

} // namespace duckdb
