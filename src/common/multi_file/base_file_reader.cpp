#include "duckdb/common/multi_file/base_file_reader.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/parallel/async_result.hpp"

namespace duckdb {

void ColumnNameIndex::Build(const vector<MultiFileColumnDefinition> &columns) {
	positions.clear();
	for (idx_t i = 0; i < columns.size(); i++) {
		positions.emplace(columns[i].name, i);
	}
}

void ColumnNameIndex::Build(const vector<Identifier> &names) {
	positions.clear();
	for (idx_t i = 0; i < names.size(); i++) {
		positions.emplace(names[i], i);
	}
}

void ColumnNameIndex::Build(const vector<string> &names) {
	positions.clear();
	for (idx_t i = 0; i < names.size(); i++) {
		positions.emplace(Identifier(names[i]), i);
	}
}

optional_idx ColumnNameIndex::Find(const Identifier &name) const {
	auto entry = positions.find(name);
	if (entry == positions.end()) {
		return optional_idx();
	}
	return entry->second;
}

unique_ptr<BaseStatistics> BaseFileReader::GetStatistics(ClientContext &context, const Identifier &name) {
	return nullptr;
}

optional_idx BaseFileReader::FindColumn(const Identifier &name) const {
	auto position = column_name_index.Find(name);
	// a stale index would point at another column
	D_ASSERT(!position.IsValid() ||
	         (position.GetIndex() < columns.size() && columns[position.GetIndex()].name == name));
	return position;
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
	auto position = column_name_index.Find(name);
	D_ASSERT(!position.IsValid() || (position.GetIndex() < names.size() && names[position.GetIndex()] == name));
	return position;
}

} // namespace duckdb
