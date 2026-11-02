#include "duckdb/common/multi_file/base_file_reader.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/parallel/async_result.hpp"

namespace duckdb {

unique_ptr<BaseStatistics> BaseFileReader::GetStatistics(ClientContext &context, const Identifier &name) {
	return nullptr;
}

unique_ptr<BaseStatistics> BaseFileReader::GetVirtualColumnStatistics(ClientContext &context,
                                                                      column_t virtual_column_id) {
	return nullptr;
}

shared_ptr<BaseUnionData> BaseFileReader::GetUnionData(idx_t file_idx) {
	throw NotImplementedException("Union by name not supported for reader of type %s", GetReaderType());
}

void BaseFileReader::PrepareScan(ClientContext &, GlobalTableFunctionState &, LocalTableFunctionState &) {
}

AsyncResult BaseFileReader::ScheduleIO(ClientContext &, GlobalTableFunctionState &, LocalTableFunctionState &) {
	return SourceResultType::HAVE_MORE_OUTPUT;
}

void BaseFileReader::PrepareReader(ClientContext &context, GlobalTableFunctionState &) {
}

void BaseFileReader::PrepareReadAhead(ClientContext &context, GlobalTableFunctionState &) {
}

void BaseFileReader::FinishFile(ClientContext &context, GlobalTableFunctionState &gstate) {
}

double BaseFileReader::GetProgressInFile(ClientContext &context) {
	return 0;
}

InsertionOrderPreservingMap<Value> BaseFileReader::GetMetadata() const {
	return {};
}

unique_ptr<BaseStatistics> BaseUnionData::GetStatistics(ClientContext &context, const Identifier &name) {
	return nullptr;
}

} // namespace duckdb
