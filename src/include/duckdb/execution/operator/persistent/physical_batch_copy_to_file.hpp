//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/persistent/physical_fixed_batch_copy.hpp
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

namespace duckdb {
struct FixedRawBatchData;

class PhysicalBatchCopyToFile : public PhysicalOperator {
public:
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::BATCH_COPY_TO_FILE;

public:
	PhysicalBatchCopyToFile(PhysicalPlan &physical_plan, vector<LogicalType> types, CopyFunction function,
	                        unique_ptr<FunctionData> bind_data, idx_t estimated_cardinality);

public:
	InsertionOrderPreservingMap<string> ParamsToString() const override;

	CopyFunction function;
	unique_ptr<FunctionData> bind_data;
	string file_path;
	bool use_tmp_file;
	CopyFunctionReturnType return_type;
	//! The names and types of the copied query, for RETURN_COLUMN_TYPES
	vector<Identifier> query_names;
	vector<LogicalType> query_types;
	bool write_empty_file;

	//! Fine-grained control over writes
	optional_idx batch_size;
	optional_idx batch_size_bytes;
	//! With file_size_bytes the files are written into the directory file_path, named by the pattern and extension,
	//! and once a file reached the size the next batch starts a new one
	optional_idx file_size_bytes;
	FilenamePattern filename_pattern;
	string file_extension;
	CopyOverwriteMode overwrite_mode = CopyOverwriteMode::COPY_ERROR_ON_CONFLICT;

	bool RotatesFiles() const {
		return file_size_bytes.IsValid();
	}

public:
	// Source interface
	unique_ptr<GlobalSourceState> GetGlobalSourceState(ClientContext &context) const override;
	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                 OperatorSourceInput &input) const override;

	bool IsSource() const override {
		return true;
	}

public:
	// Sink interface
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override;
	SinkCombineResultType Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const override;
	SinkFinalizeType Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
	                          OperatorSinkFinalizeInput &input) const override;
	unique_ptr<LocalSinkState> GetLocalSinkState(ExecutionContext &context) const override;
	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override;
	SinkNextBatchType NextBatch(ExecutionContext &context, OperatorSinkNextBatchInput &input) const override;
	SinkNextBatchType UpdateMinBatchIndex(ExecutionContext &context, OperatorSinkNextBatchInput &input) const override;

	OperatorPartitionInfo RequiredPartitionInfo() const override;

	bool IsSink() const override {
		return true;
	}

	bool ParallelSink() const override {
		return true;
	}

	PipelineExternalInputSupport GetExternalInputSupport() const override {
		return PipelineExternalInputSupport::SUPPORTED;
	}

public:
	void AddLocalBatch(ClientContext &context, GlobalSinkState &gstate, LocalSinkState &state) const;
	void AddRawBatchData(ClientContext &context, GlobalSinkState &gstate_p, idx_t batch_index,
	                     unique_ptr<FixedRawBatchData> collection) const;
	void RepartitionBatches(ClientContext &context, GlobalSinkState &gstate_p, idx_t min_index,
	                        bool final = false) const;
	void FlushBatchData(ClientContext &context, GlobalSinkState &gstate_p) const;
	bool ExecuteTask(ClientContext &context, GlobalSinkState &gstate_p) const;
	void ExecuteTasks(ClientContext &context, GlobalSinkState &gstate_p) const;
	SinkFinalizeType FinalFlush(ClientContext &context, GlobalSinkState &gstate_p) const;
};
} // namespace duckdb
