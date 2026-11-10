#include "duckdb/common/multi_file/base_file_reader.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/parallel/async_result.hpp"

namespace duckdb {

unique_ptr<BaseStatistics> BaseFileReader::GetStatistics(ClientContext &context, const Identifier &name) {
	return nullptr;
}

optional_idx BaseFileReader::FindColumn(const Identifier &name) const {
	return column_name_index.Find(name, columns, [](const MultiFileColumnDefinition &column) -> const string & {
		return column.name.GetIdentifierName();
	});
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

optional_idx BaseUnionData::FindColumn(const Identifier &name) const {
	return column_name_index.Find(name, names, [](const string &column_name) -> const string & { return column_name; });
}

} // namespace duckdb
