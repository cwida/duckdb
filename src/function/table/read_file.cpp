#include "duckdb/function/table/read_file.hpp"
#include "duckdb/function/table/direct_file_reader.hpp"
#include "duckdb/common/multi_file/multi_file_reader.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/function/table/range.hpp"
#include "utf8proc_wrapper.hpp"
#include "duckdb/storage/external_file_cache/caching_file_system.hpp"

namespace duckdb {

namespace {

//------------------------------------------------------------------------------
// DirectMultiFileInfo
//------------------------------------------------------------------------------

template <class OP>
struct DirectMultiFileInfo : MultiFileReaderInterface {
	static unique_ptr<MultiFileReaderInterface> CreateInterface(ClientContext &context);
	unique_ptr<BaseFileReaderOptions> InitializeOptions(ClientContext &context,
	                                                    optional_ptr<TableFunctionInfo> info) override;
	bool ParseCopyOption(ClientContext &context, const Identifier &key, const vector<Value> &values,
	                     BaseFileReaderOptions &options, vector<Identifier> &expected_names,
	                     vector<LogicalType> &expected_types) override;
	bool ParseOption(ClientContext &context, const Identifier &key, const Value &val, MultiFileOptions &file_options,
	                 BaseFileReaderOptions &options) override;
	unique_ptr<TableFunctionData> InitializeBindData(MultiFileBindData &multi_file_data,
	                                                 unique_ptr<BaseFileReaderOptions> options) override;
	void BindReader(ClientContext &context, vector<LogicalType> &return_types, vector<Identifier> &names,
	                MultiFileBindData &bind_data) override;
	optional_idx MaxThreads(const MultiFileBindData &bind_data_p, const MultiFileGlobalState &global_state,
	                        FileExpandResult expand_result) override;
	unique_ptr<GlobalTableFunctionState> InitializeGlobalState(ClientContext &context, MultiFileBindData &bind_data,
	                                                           MultiFileGlobalState &global_state) override;
	unique_ptr<LocalTableFunctionState> InitializeLocalState(ClientContext &, GlobalTableFunctionState &) override;
	shared_ptr<BaseFileReader> CreateReader(ClientContext &context, GlobalTableFunctionState &gstate,
	                                        BaseUnionData &union_data, const MultiFileBindData &bind_data_p) override;
	shared_ptr<BaseFileReader> CreateReader(ClientContext &context, GlobalTableFunctionState &gstate,
	                                        const OpenFileInfo &file, idx_t file_idx,
	                                        const MultiFileBindData &bind_data) override;
	shared_ptr<BaseFileReader> CreateReader(ClientContext &context, const OpenFileInfo &file,
	                                        BaseFileReaderOptions &options,
	                                        const MultiFileOptions &file_options) override;
	unique_ptr<NodeStatistics> GetCardinality(const MultiFileBindData &bind_data, idx_t file_count) override;
	FileGlobInput GetGlobInput() override;
};

template <class OP>
unique_ptr<MultiFileReaderInterface> DirectMultiFileInfo<OP>::CreateInterface(ClientContext &context) {
	return make_uniq<DirectMultiFileInfo>();
}

template <class OP>
unique_ptr<BaseFileReaderOptions> DirectMultiFileInfo<OP>::InitializeOptions(ClientContext &context,
                                                                             optional_ptr<TableFunctionInfo> info) {
	return make_uniq<BaseFileReaderOptions>();
}

template <class OP>
bool DirectMultiFileInfo<OP>::ParseCopyOption(ClientContext &context, const Identifier &key,
                                              const vector<Value> &values, BaseFileReaderOptions &options,
                                              vector<Identifier> &expected_names, vector<LogicalType> &expected_types) {
	return true;
}

template <class OP>
bool DirectMultiFileInfo<OP>::ParseOption(ClientContext &context, const Identifier &key, const Value &val,
                                          MultiFileOptions &file_options, BaseFileReaderOptions &options) {
	return true;
}

template <class OP>
unique_ptr<TableFunctionData> DirectMultiFileInfo<OP>::InitializeBindData(MultiFileBindData &multi_file_data,
                                                                          unique_ptr<BaseFileReaderOptions> options) {
	auto result = make_uniq<ReadFileBindData>();
	result->options = std::move(options);
	return std::move(result);
}

template <class OP>
void DirectMultiFileInfo<OP>::BindReader(ClientContext &context, vector<LogicalType> &return_types,
                                         vector<Identifier> &names, MultiFileBindData &bind_data) {
	auto &read_bind = bind_data.bind_data->Cast<ReadFileBindData>();
	bind_data.reader_bind = bind_data.multi_file_reader->BindReader(
	    context, return_types, names, *bind_data.file_list, bind_data, *read_bind.options, bind_data.file_options);
}

template <class OP>
optional_idx DirectMultiFileInfo<OP>::MaxThreads(const MultiFileBindData &bind_data_p,
                                                 const MultiFileGlobalState &global_state,
                                                 FileExpandResult expand_result) {
	return 1;
}

template <class OP>
unique_ptr<GlobalTableFunctionState>
DirectMultiFileInfo<OP>::InitializeGlobalState(ClientContext &context, MultiFileBindData &bind_data,
                                               MultiFileGlobalState &global_state) {
	auto result = make_uniq<ReadFileGlobalState>();

	result->file_list = bind_data.file_list;
	vector<idx_t> column_ids;
	for (idx_t i = 0; i < global_state.column_indexes.size(); i++) {
		// We only look at file-related columns
		if (global_state.column_indexes[i].GetPrimaryIndex() <= ReadFileBindData::FILE_LAST_MODIFIED_COLUMN) {
			column_ids.push_back(global_state.column_indexes[i].GetPrimaryIndex());
		}
	}

	for (const auto &column_id : column_ids) {
		if (column_id == ReadFileBindData::FILE_CONTENT_COLUMN) {
			result->requires_file_open = true;
		} else if (column_id == ReadFileBindData::FILE_SIZE_COLUMN ||
		           column_id == ReadFileBindData::FILE_LAST_MODIFIED_COLUMN) {
			result->requires_file_metadata = true;
		}
	}

	result->column_ids = std::move(column_ids);
	return std::move(result);
}

template <class OP>
unique_ptr<LocalTableFunctionState> DirectMultiFileInfo<OP>::InitializeLocalState(ClientContext &,
                                                                                  GlobalTableFunctionState &) {
	return make_uniq<LocalTableFunctionState>();
}

template <class OP>
shared_ptr<BaseFileReader>
DirectMultiFileInfo<OP>::CreateReader(ClientContext &context, GlobalTableFunctionState &gstate,
                                      BaseUnionData &union_data, const MultiFileBindData &bind_data_p) {
	return make_shared_ptr<DirectFileReader>(union_data.file, OP::TYPE());
}

template <class OP>
shared_ptr<BaseFileReader>
DirectMultiFileInfo<OP>::CreateReader(ClientContext &context, GlobalTableFunctionState &gstate,
                                      const OpenFileInfo &file, idx_t file_idx, const MultiFileBindData &bind_data) {
	return make_shared_ptr<DirectFileReader>(file, OP::TYPE());
}

template <class OP>
shared_ptr<BaseFileReader> DirectMultiFileInfo<OP>::CreateReader(ClientContext &context, const OpenFileInfo &file,
                                                                 BaseFileReaderOptions &options_p,
                                                                 const MultiFileOptions &) {
	return make_shared_ptr<DirectFileReader>(file, OP::TYPE());
}

template <class OP>
unique_ptr<NodeStatistics> DirectMultiFileInfo<OP>::GetCardinality(const MultiFileBindData &bind_data,
                                                                   idx_t file_count) {
	auto result = make_uniq<NodeStatistics>();
	result->has_max_cardinality = true;
	result->max_cardinality = bind_data.file_list->GetTotalFileCount();
	result->has_estimated_cardinality = true;
	result->estimated_cardinality = bind_data.file_list->GetTotalFileCount();
	return result;
}

template <class OP>
FileGlobInput DirectMultiFileInfo<OP>::GetGlobInput() {
	return FileGlobOptions::ALLOW_EMPTY;
}

//------------------------------------------------------------------------------
// Operations
//------------------------------------------------------------------------------

struct ReadBlobOperation {
	static constexpr const char *NAME = "read_blob";

	static inline LogicalType TYPE() {
		return LogicalType::BLOB;
	}
};

struct ReadTextOperation {
	static constexpr const char *NAME = "read_text";

	static inline LogicalType TYPE() {
		return LogicalType::VARCHAR;
	}
};

template <class OP>
static TableFunction GetFunction() {
	// this function reads one file per call, so it takes none of the options that describe combining several
	MultiFileFunction<DirectMultiFileInfo<OP>> table_function(OP::NAME,
	                                                          MultiFileReader::MultiFileParameters::ALLOW_EMPTY_ONLY);
	return table_function;
}

} // namespace

//------------------------------------------------------------------------------
// Register
//------------------------------------------------------------------------------

void ReadBlobFunction::RegisterFunction(BuiltinFunctions &set) {
	auto scan_fun = GetFunction<ReadBlobOperation>();
	set.AddFunction(MultiFileReader::CreateFunctionSet(scan_fun));
}

void ReadTextFunction::RegisterFunction(BuiltinFunctions &set) {
	auto scan_fun = GetFunction<ReadTextOperation>();
	set.AddFunction(MultiFileReader::CreateFunctionSet(scan_fun));
}

} // namespace duckdb
