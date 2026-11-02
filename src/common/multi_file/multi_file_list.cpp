#include "duckdb/common/multi_file/multi_file_reader.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/hive_partitioning.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"

#include <algorithm>
#include "duckdb/main/client_context.hpp"

namespace duckdb {

MultiFilePushdownInfo::MultiFilePushdownInfo(TableIndex table_index, const vector<Identifier> &column_names,
                                             const vector<ColumnIndex> &column_indexes, ExtraOperatorInfo &extra_info)
    : table_index(table_index), column_names(column_names), column_indexes(column_indexes), extra_info(extra_info) {
	for (auto &col_id : column_indexes) {
		column_ids.push_back(col_id.GetPrimaryIndex());
	}
}

// Helper method to do Filter Pushdown into a MultiFileList
bool PushdownInternal(ClientContext &context, const MultiFileOptions &options, MultiFilePushdownInfo &info,
                      vector<unique_ptr<Expression>> &filters, vector<OpenFileInfo> &expanded_files) {
	HivePartitioningFilterInfo filter_info;
	for (idx_t i = 0; i < info.column_ids.size(); i++) {
		if (IsVirtualColumn(info.column_ids[i])) {
			continue;
		}
		filter_info.column_map.insert({info.column_names[info.column_ids[i]], i});
	}
	filter_info.hive_enabled = options.hive_partitioning;
	filter_info.filename_enabled = options.filename;
	filter_info.filename_column = Identifier(options.filename_column);

	auto start_files = expanded_files.size();
	HivePartitioning::ApplyFiltersToFileList(context, expanded_files, filters, filter_info, info);

	if (expanded_files.size() != start_files) {
		return true;
	}

	return false;
}

bool PushdownInternal(ClientContext &context, const MultiFileOptions &options, const vector<Identifier> &names,
                      const vector<LogicalType> &types, const vector<ColumnIndex> &column_indexes,
                      const TableFilterSet &filters, vector<OpenFileInfo> &expanded_files) {
	TableIndex table_index(0);
	ExtraOperatorInfo extra_info;

	// construct the pushdown info
	MultiFilePushdownInfo info(table_index, names, column_indexes, extra_info);

	// construct the set of expressions from the table filters
	vector<unique_ptr<Expression>> filter_expressions;
	for (auto &entry : filters) {
		auto filter_idx = entry.GetIndex();
		auto &column_idx = column_indexes[filter_idx];
		auto primary_index = column_idx.GetPrimaryIndex();
		if (IsVirtualColumn(primary_index)) {
			continue;
		}
		auto column_ref =
		    make_uniq<BoundColumnRefExpression>(types[primary_index], ColumnBinding(table_index, entry.GetIndex()));
		auto &expr_filter = ExpressionFilter::GetExpressionFilter(entry.Filter(), "MultiFilePushdownInfo::Pushdown");
		auto filter_expr = expr_filter.ToExpression(*column_ref);
		filter_expressions.push_back(std::move(filter_expr));
	}

	// call the original PushdownInternal method
	return PushdownInternal(context, options, info, filter_expressions, expanded_files);
}

//===--------------------------------------------------------------------===//
// MultiFileListIterator
//===--------------------------------------------------------------------===//
MultiFileListIterationHelper MultiFileList::Files() const {
	return MultiFileListIterationHelper(*this);
}

MultiFileListIterationHelper::MultiFileListIterationHelper(const MultiFileList &file_list_p) : file_list(file_list_p) {
}

MultiFileListIterationHelper::MultiFileListIterator::MultiFileListIterator(
    optional_ptr<const MultiFileList> file_list_p)
    : file_list(file_list_p) {
	if (!file_list) {
		return;
	}

	file_list->InitializeScan(file_scan_data);
	if (!file_list->Scan(file_scan_data, current_file)) {
		// There is no first file: move iterator to nop state
		file_list = nullptr;
		file_scan_data.current_file_idx = DConstants::INVALID_INDEX;
	}
}

void MultiFileListIterationHelper::MultiFileListIterator::Next() {
	if (!file_list) {
		return;
	}

	if (!file_list->Scan(file_scan_data, current_file)) {
		// exhausted collection: move iterator to nop state
		file_list = nullptr;
		file_scan_data.current_file_idx = DConstants::INVALID_INDEX;
	}
}

MultiFileListIterationHelper::MultiFileListIterator MultiFileListIterationHelper::begin() { // NOLINT: match stl API
	return MultiFileListIterationHelper::MultiFileListIterator(
	    file_list.GetExpandResult() == FileExpandResult::NO_FILES ? nullptr : &file_list);
}
MultiFileListIterationHelper::MultiFileListIterator MultiFileListIterationHelper::end() { // NOLINT: match stl API
	return MultiFileListIterationHelper::MultiFileListIterator(nullptr);
}

MultiFileListIterationHelper::MultiFileListIterator &MultiFileListIterationHelper::MultiFileListIterator::operator++() {
	Next();
	return *this;
}

bool MultiFileListIterationHelper::MultiFileListIterator::operator!=(const MultiFileListIterator &other) const {
	return file_list != other.file_list || file_scan_data.current_file_idx != other.file_scan_data.current_file_idx;
}

const OpenFileInfo &MultiFileListIterationHelper::MultiFileListIterator::operator*() const {
	return current_file;
}

//===--------------------------------------------------------------------===//
// MultiFileList
//===--------------------------------------------------------------------===//
MultiFileList::MultiFileList() {
}

MultiFileList::~MultiFileList() {
}

void MultiFileList::InitializeScan(MultiFileListScanData &iterator) const {
	iterator.current_file_idx = 0;
}

vector<OpenFileInfo> MultiFileList::GetDisplayFileList(optional_idx max_files) const {
	vector<OpenFileInfo> files;
	for (idx_t i = 0;; i++) {
		if (max_files.IsValid() && files.size() >= max_files.GetIndex()) {
			break;
		}
		auto file = GetFile(i);
		if (file.path.empty()) {
			break;
		}
		files.push_back(std::move(file));
	}
	return files;
}

vector<OpenFileInfo> MultiFileList::GetAllFiles() const {
	return MultiFileList::GetDisplayFileList();
}

MultiFileCount MultiFileList::GetFileCount(idx_t min_exact_count) const {
	return MultiFileCount(GetTotalFileCount());
}

bool MultiFileList::Scan(MultiFileListScanData &iterator, OpenFileInfo &result_file) const {
	D_ASSERT(iterator.current_file_idx != DConstants::INVALID_INDEX);
	if (iterator.scan_type == MultiFileListScanType::FETCH_IF_AVAILABLE &&
	    !FileIsAvailable(iterator.current_file_idx)) {
		return false;
	}
	auto maybe_file = GetFile(iterator.current_file_idx);

	if (maybe_file.path.empty()) {
		D_ASSERT(iterator.current_file_idx >= GetTotalFileCount());
		return false;
	}

	result_file = maybe_file;
	iterator.current_file_idx++;
	return true;
}

unique_ptr<MultiFileList> MultiFileList::ComplexFilterPushdown(ClientContext &context, const MultiFileOptions &options,
                                                               MultiFilePushdownInfo &info,
                                                               vector<unique_ptr<Expression>> &filters) const {
	if (!options.hive_partitioning && !options.filename) {
		return nullptr;
	}

	// FIXME: don't copy list until first file is filtered
	auto file_copy = GetAllFiles();
	auto res = PushdownInternal(context, options, info, filters, file_copy);
	if (res) {
		return make_uniq<SimpleMultiFileList>(std::move(file_copy));
	}
	return nullptr;
}

unique_ptr<MultiFileList>
MultiFileList::DynamicFilterPushdown(MultiFileDynamicPushdownInfo &dynamic_pushdown_info) const {
	auto &options = dynamic_pushdown_info.options;
	auto &names = dynamic_pushdown_info.column_names;
	auto &types = dynamic_pushdown_info.column_types;
	auto &column_indexes = dynamic_pushdown_info.column_indexes;
	auto &context = dynamic_pushdown_info.context;
	auto &filters = dynamic_pushdown_info.filters;

	if (!options.hive_partitioning && !options.filename) {
		return nullptr;
	}

	// FIXME: don't copy list until first file is filtered
	auto file_copy = GetAllFiles();
	auto res = PushdownInternal(context, options, names, types, column_indexes, filters, file_copy);
	if (res) {
		return make_uniq<SimpleMultiFileList>(std::move(file_copy));
	}

	return nullptr;
}

unique_ptr<NodeStatistics> MultiFileList::GetCardinality(ClientContext &context) const {
	return nullptr;
}

OpenFileInfo MultiFileList::GetFirstFile() const {
	return GetFile(0);
}

bool MultiFileList::IsEmpty() const {
	return GetExpandResult() == FileExpandResult::NO_FILES;
}

unique_ptr<MultiFileList> MultiFileList::Copy() const {
	return make_uniq<SimpleMultiFileList>(GetAllFiles());
}

bool MultiFileList::FileIsAvailable(idx_t i) const {
	return true;
}

//===--------------------------------------------------------------------===//
// SimpleMultiFileList
//===--------------------------------------------------------------------===//
SimpleMultiFileList::SimpleMultiFileList(vector<OpenFileInfo> paths_p) : paths(std::move(paths_p)) {
}

vector<OpenFileInfo> SimpleMultiFileList::GetAllFiles() const {
	return paths;
}

FileExpandResult SimpleMultiFileList::GetExpandResult() const {
	if (paths.size() > 1) {
		return FileExpandResult::MULTIPLE_FILES;
	} else if (paths.size() == 1) {
		return FileExpandResult::SINGLE_FILE;
	}

	return FileExpandResult::NO_FILES;
}

OpenFileInfo SimpleMultiFileList::GetFile(idx_t i) const {
	if (paths.empty() || i >= paths.size()) {
		return OpenFileInfo("");
	}

	return paths[i];
}

idx_t SimpleMultiFileList::GetTotalFileCount() const {
	return paths.size();
}

//===--------------------------------------------------------------------===//
// LazyFileList
//===--------------------------------------------------------------------===//
LazyMultiFileList::LazyMultiFileList(optional_ptr<ClientContext> context_p) : context(context_p) {
}

vector<OpenFileInfo> LazyMultiFileList::GetAllFiles() const {
	lock_guard<mutex> lck(lock);
	while (ExpandNextPathInternal()) {
	}
	return expanded_files;
}

idx_t LazyMultiFileList::GetTotalFileCount() const {
	lock_guard<mutex> lck(lock);
	while (ExpandNextPathInternal()) {
	}
	return expanded_files.size();
}

MultiFileCount LazyMultiFileList::GetFileCount(idx_t min_exact_count) const {
	lock_guard<mutex> lck(lock);
	// expand files so that we get to min_exact_count
	while (!all_files_expanded && expanded_files.size() < min_exact_count && ExpandNextPathInternal()) {
	}
	auto type = all_files_expanded ? FileExpansionType::ALL_FILES_EXPANDED : FileExpansionType::NOT_ALL_FILES_KNOWN;
	return MultiFileCount(expanded_files.size(), type);
}

FileExpandResult LazyMultiFileList::GetExpandResult() const {
	// GetFile(1) will ensure at least the first 2 files are expanded if they are available
	(void)GetFile(1);

	lock_guard<mutex> lck(lock);
	if (expanded_files.size() > 1) {
		return FileExpandResult::MULTIPLE_FILES;
	} else if (expanded_files.size() == 1) {
		return FileExpandResult::SINGLE_FILE;
	}
	return FileExpandResult::NO_FILES;
}

bool LazyMultiFileList::FileIsAvailable(idx_t i) const {
	lock_guard<mutex> lck(lock);
	return i < expanded_files.size();
}

OpenFileInfo LazyMultiFileList::GetFile(idx_t i) const {
	lock_guard<mutex> lck(lock);
	while (expanded_files.size() <= i) {
		if (!ExpandNextPathInternal()) {
			return OpenFileInfo("");
		}
	}
	D_ASSERT(expanded_files.size() > i);
	return expanded_files[i];
}

bool LazyMultiFileList::ExpandNextPathInternal() const {
	if (all_files_expanded) {
		return false;
	}
	if (context && context->IsInterrupted()) {
		throw InterruptException();
	}
	if (!ExpandNextPath()) {
		all_files_expanded = true;
		return false;
	}
	return true;
}

//===--------------------------------------------------------------------===//
// GlobMultiFileList
//===--------------------------------------------------------------------===//
static vector<OpenFileInfo> PathsToFiles(vector<string> paths) {
	vector<OpenFileInfo> result;
	result.reserve(paths.size());
	for (auto &path : paths) {
		result.emplace_back(std::move(path));
	}
	return result;
}

GlobMultiFileList::GlobMultiFileList(ClientContext &context_p, vector<string> globs_p, FileGlobInput glob_input_p)
    : GlobMultiFileList(context_p, PathsToFiles(std::move(globs_p)), std::move(glob_input_p)) {
}

GlobMultiFileList::GlobMultiFileList(ClientContext &context_p, vector<OpenFileInfo> globs_p, FileGlobInput glob_input_p)
    : LazyMultiFileList(&context_p), context(context_p), globs(std::move(globs_p)), glob_input(std::move(glob_input_p)),
      current_glob(0) {
}

vector<OpenFileInfo> GlobMultiFileList::GetDisplayFileList(optional_idx max_files) const {
	// for globs we display the actual globs in the ToString() - instead of expanding to the files read
	vector<OpenFileInfo> result;
	for (auto &glob : globs) {
		if (max_files.IsValid() && result.size() >= max_files.GetIndex()) {
			break;
		}
		result.emplace_back(glob);
	}
	return result;
}

bool GlobMultiFileList::ExpandNextPath() const {
	if (current_glob >= globs.size()) {
		return false;
	}
	auto &current_entry = globs[current_glob];
	if (current_entry.extended_info) {
		// the file was specified together with the options to open it with - use it as-is
		expanded_files.push_back(current_entry);
		current_glob++;
		return true;
	}
	if (current_glob >= file_lists.size()) {
		file_lists.resize(current_glob + 1);
	}
	if (!file_lists[current_glob]) {
		// glob is not yet started for this file - start it and initiate the scan over this file
		auto &fs = FileSystem::GetFileSystem(context);
		auto glob_result = fs.GlobFileList(current_entry.path, glob_input);
		scan_state = MultiFileListScanData();
		glob_result->InitializeScan(scan_state);
		file_lists[current_glob] = std::move(glob_result);
	}
	// get the next batch of files we can fetch through the glob
	auto &glob_list = *file_lists[current_glob];
	scan_state.scan_type = MultiFileListScanType::ALWAYS_FETCH;
	OpenFileInfo file;
	if (!glob_list.Scan(scan_state, file)) {
		// no more files available in this glob - move to the next glob
		current_glob++;
		return true;
	}
	// we found a file as part of this glob - add it to the result
	vector<OpenFileInfo> glob_files;
	glob_files.push_back(std::move(file));

	// now continue scanning files that are already available (i.e. that don't require extra I/O operations to fetch)
	scan_state.scan_type = MultiFileListScanType::FETCH_IF_AVAILABLE;
	while (glob_list.Scan(scan_state, file)) {
		glob_files.push_back(std::move(file));
	}

	// sort the files and add them to the list of files
	std::sort(glob_files.begin(), glob_files.end());
	expanded_files.insert(expanded_files.end(), glob_files.begin(), glob_files.end());

	return true;
}

} // namespace duckdb
