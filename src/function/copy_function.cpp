#include "duckdb/function/copy_function.hpp"

#include "duckdb/common/types/column/column_data_collection.hpp"

namespace duckdb {

CopyFunction::CopyFunction(const Identifier &name)
    : Function(name), plan(nullptr), copy_to_select(nullptr), copy_to_bind(nullptr), copy_options(nullptr),
      copy_to_initialize_local(nullptr), copy_to_initialize_global(nullptr), copy_to_get_written_statistics(nullptr),
      copy_to_sink(nullptr), copy_to_combine(nullptr), copy_to_finalize(nullptr), execution_mode(nullptr),
      initialize_operator(nullptr), prepare_batch(nullptr), flush_batch(nullptr), file_size_bytes(nullptr),
      desired_batch_size(nullptr), serialize(nullptr), deserialize(nullptr), copy_from_bind(nullptr) {
}

CopyOption::CopyOption() : type(LogicalType::ANY), mode(CopyOptionMode::READ_WRITE) {
}

CopyOption::CopyOption(LogicalType type_p, CopyOptionMode mode_p) : type(std::move(type_p)), mode(mode_p) {
}

bool CopyFunctionReturnsColumnTypes(CopyFunctionReturnType return_type) {
	switch (return_type) {
	case CopyFunctionReturnType::CHANGED_ROWS_AND_COLUMN_TYPES:
	case CopyFunctionReturnType::CHANGED_ROWS_AND_FILE_LIST_AND_COLUMN_TYPES:
	case CopyFunctionReturnType::WRITTEN_FILE_STATISTICS_AND_COLUMN_TYPES:
		return true;
	default:
		return false;
	}
}

CopyFunctionReturnType GetCopyFunctionBaseReturnType(CopyFunctionReturnType return_type) {
	switch (return_type) {
	case CopyFunctionReturnType::CHANGED_ROWS_AND_COLUMN_TYPES:
		return CopyFunctionReturnType::CHANGED_ROWS;
	case CopyFunctionReturnType::CHANGED_ROWS_AND_FILE_LIST_AND_COLUMN_TYPES:
		return CopyFunctionReturnType::CHANGED_ROWS_AND_FILE_LIST;
	case CopyFunctionReturnType::WRITTEN_FILE_STATISTICS_AND_COLUMN_TYPES:
		return CopyFunctionReturnType::WRITTEN_FILE_STATISTICS;
	default:
		return return_type;
	}
}

CopyFunctionReturnType GetCopyFunctionReturnTypeWithColumnTypes(CopyFunctionReturnType return_type) {
	switch (return_type) {
	case CopyFunctionReturnType::CHANGED_ROWS:
		return CopyFunctionReturnType::CHANGED_ROWS_AND_COLUMN_TYPES;
	case CopyFunctionReturnType::CHANGED_ROWS_AND_FILE_LIST:
		return CopyFunctionReturnType::CHANGED_ROWS_AND_FILE_LIST_AND_COLUMN_TYPES;
	case CopyFunctionReturnType::WRITTEN_FILE_STATISTICS:
		return CopyFunctionReturnType::WRITTEN_FILE_STATISTICS_AND_COLUMN_TYPES;
	default:
		return return_type;
	}
}

vector<Identifier> GetCopyFunctionReturnNames(CopyFunctionReturnType return_type) {
	if (CopyFunctionReturnsColumnTypes(return_type)) {
		auto names = GetCopyFunctionReturnNames(GetCopyFunctionBaseReturnType(return_type));
		names.emplace_back("columns");
		return names;
	}
	switch (return_type) {
	case CopyFunctionReturnType::CHANGED_ROWS:
		return {"Count"};
	case CopyFunctionReturnType::CHANGED_ROWS_AND_FILE_LIST:
		return {"Count", "Files"};
	case CopyFunctionReturnType::WRITTEN_FILE_STATISTICS:
		return {"filename",          "count",          "file_size_bytes", "footer_size_bytes",
		        "column_statistics", "partition_keys", "extra_info"};
	default:
		throw NotImplementedException("Unknown CopyFunctionReturnType");
	}
}

vector<LogicalType> GetCopyFunctionReturnLogicalTypes(CopyFunctionReturnType return_type) {
	if (CopyFunctionReturnsColumnTypes(return_type)) {
		auto types = GetCopyFunctionReturnLogicalTypes(GetCopyFunctionBaseReturnType(return_type));
		//! columns: the name and type of every copied column
		types.push_back(
		    LogicalType::LIST(LogicalType::STRUCT({{"name", LogicalType::VARCHAR}, {"type", LogicalType::TYPE()}})));
		return types;
	}
	switch (return_type) {
	case CopyFunctionReturnType::CHANGED_ROWS:
		return {LogicalType::BIGINT};
	case CopyFunctionReturnType::CHANGED_ROWS_AND_FILE_LIST:
		return {LogicalType::BIGINT, LogicalType::LIST(LogicalType::VARCHAR)};
	case CopyFunctionReturnType::WRITTEN_FILE_STATISTICS:
		return {//! filename
		        LogicalType::VARCHAR,
		        //! count
		        LogicalType::UBIGINT,
		        //! file size bytes
		        LogicalType::UBIGINT,
		        //! footer size bytes
		        LogicalType::UBIGINT,
		        //! column_path (potentially nested) -> map(stats_type -> value)
		        LogicalType::MAP(LogicalType::VARCHAR, LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR)),
		        //! partition key -> value
		        LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR),
		        //! format-specific extra info (e.g. row_group_count)
		        LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARIANT())};
	default:
		throw NotImplementedException("Unknown CopyFunctionReturnType");
	}
}

CopyFunctionBatchAnalyzer::CopyFunctionBatchAnalyzer(const idx_t &current_batch_size,
                                                     const idx_t &current_batch_size_bytes,
                                                     const optional_idx &batch_size,
                                                     const optional_idx &batch_size_bytes)
    : current_batch_size(current_batch_size), current_batch_size_bytes(current_batch_size_bytes),
      batch_size(batch_size), batch_size_bytes(batch_size_bytes) {
}

CopyFunctionBatchAnalyzer::CopyFunctionBatchAnalyzer(const ColumnDataCollection &batch, const optional_idx &batch_size,
                                                     const optional_idx &batch_size_bytes)
    : CopyFunctionBatchAnalyzer(batch.Count(), batch.SizeInBytes(), batch_size, batch_size_bytes) {
}

bool CopyFunctionBatchAnalyzer::AnyBatchQualifies() const {
	return !batch_size.IsValid() && !batch_size_bytes.IsValid();
}

bool CopyFunctionBatchAnalyzer::ExceedsBatchSize() const {
	return batch_size.IsValid() && current_batch_size >= batch_size.GetIndex();
}

bool CopyFunctionBatchAnalyzer::ExceedsBatchSizeBytes() const {
	return batch_size_bytes.IsValid() && current_batch_size_bytes >= batch_size_bytes.GetIndex();
}

bool CopyFunctionBatchAnalyzer::MeetsFlushCriteria() const {
	return AnyBatchQualifies() || ExceedsBatchSize() || ExceedsBatchSizeBytes();
}

int64_t CopyFunctionBatchAnalyzer::BatchSizeVectorDiff() const {
	if (!batch_size.IsValid()) {
		return -1000000;
	}
	const auto batch_size_diff = NumericCast<int64_t>(current_batch_size) - NumericCast<int64_t>(batch_size.GetIndex());
	return (batch_size_diff + STANDARD_VECTOR_SIZE - 1) / STANDARD_VECTOR_SIZE;
}

int64_t CopyFunctionBatchAnalyzer::BatchSizeBytesVectorDiff() const {
	if (!batch_size_bytes.IsValid()) {
		return -1000000;
	}
	const auto size_bytes_diff =
	    NumericCast<int64_t>(current_batch_size_bytes) - NumericCast<int64_t>(batch_size_bytes.GetIndex());
	const auto bytes_per_tuple = NumericCast<int64_t>(current_batch_size_bytes / current_batch_size) + 1;
	return (size_bytes_diff / bytes_per_tuple + STANDARD_VECTOR_SIZE - 1) / STANDARD_VECTOR_SIZE;
}

bool CopyFunctionBatchAnalyzer::IsAcceptable() const {
	if (AnyBatchQualifies()) {
		return true;
	}

	if (BatchSizeVectorDiff() == 0) {
		// Acceptable row count, require low or acceptable byte size
		return BatchSizeBytesVectorDiff() <= 0;
	}

	if (BatchSizeBytesVectorDiff() == 0) {
		// Acceptable byte size, require low or acceptable row count
		return BatchSizeVectorDiff() <= 0;
	}

	return false;
}

CopyFunctionFlushBatchReason CopyFunctionBatchAnalyzer::ToReason() const {
	if (AnyBatchQualifies() || ExceedsBatchSize()) {
		return CopyFunctionFlushBatchReason::BATCH_SIZE;
	}

	if (ExceedsBatchSizeBytes()) {
		return CopyFunctionFlushBatchReason::BATCH_SIZE_BYTES;
	}

	return CopyFunctionFlushBatchReason::FORCED_FLUSH;
}

} // namespace duckdb
