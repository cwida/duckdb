#include "duckdb/function/table/range.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/common/multi_file/multi_file_reader.hpp"
#include "duckdb/execution/progress_data.hpp"

#include <cmath>

namespace duckdb {

struct GlobFunctionBindData : public TableFunctionData {
	shared_ptr<MultiFileList> file_list;

	bool SupportStatementCache() const override {
		return false;
	}
};

static unique_ptr<FunctionData> GlobFunctionBind(ClientContext &context, TableFunctionBindInput &input,
                                                 vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto result = make_uniq<GlobFunctionBindData>();
	auto multi_file_reader = MultiFileReader::Create(input.table_function);
	result->file_list = multi_file_reader->CreateFileList(context, input.inputs[0], FileGlobOptions::ALLOW_EMPTY);
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("file");
	return std::move(result);
}

struct GlobFunctionState : public GlobalTableFunctionState {
	GlobFunctionState() : files_returned(0) {
	}

	MultiFileListScanData file_list_scan;
	atomic<idx_t> files_returned;
	//! The progress is estimated while the glob is being expanded - keeps it monotonic
	MonotonicProgress progress;
};

static unique_ptr<GlobalTableFunctionState> GlobFunctionInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<GlobFunctionBindData>();
	auto res = make_uniq<GlobFunctionState>();

	bind_data.file_list->InitializeScan(res->file_list_scan);

	return std::move(res);
}

static void GlobFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->Cast<GlobFunctionBindData>();
	auto &state = data_p.global_state->Cast<GlobFunctionState>();

	state.file_list_scan.scan_type = MultiFileListScanType::ALWAYS_FETCH;
	idx_t count = 0;
	auto &file_column = output.data[0];
	while (count < STANDARD_VECTOR_SIZE) {
		OpenFileInfo file;
		if (!bind_data.file_list->Scan(state.file_list_scan, file)) {
			break;
		}
		file_column.Append(Value(file.path));
		count++;
		state.file_list_scan.scan_type = MultiFileListScanType::FETCH_IF_AVAILABLE;
	}
	state.files_returned.fetch_add(count, std::memory_order_relaxed);
}

static double GlobFunctionProgress(ClientContext &context, const FunctionData *bind_data_p,
                                   const GlobalTableFunctionState *global_state) {
	auto &bind_data = bind_data_p->Cast<GlobFunctionBindData>();
	auto &state = global_state->Cast<GlobFunctionState>();
	auto files_returned = static_cast<double>(state.files_returned.load(std::memory_order_relaxed));
	// only look at the files that have been expanded so far - expanding the glob here would perform I/O
	auto file_count = bind_data.file_list->GetFileCount(0);
	double fraction;
	if (file_count.type == FileExpansionType::ALL_FILES_EXPANDED) {
		fraction = file_count.count == 0 ? 1.0 : files_returned / static_cast<double>(file_count.count);
	} else {
		// the number of files is not known yet - converge towards 100% as files are returned
		fraction = 1 - 1 / (1 + std::log2(1 + files_returned / static_cast<double>(STANDARD_VECTOR_SIZE)));
		fraction = MinValue<double>(fraction, 0.99);
	}
	auto result = state.progress.Update(ProgressData {MinValue<double>(fraction, 1.0), 1.0, false});
	return 100.0 * result.done;
}

void GlobTableFunction::RegisterFunction(BuiltinFunctions &set) {
	TableFunction glob_function("glob", FunctionSignature().AddPositionalOnly("pattern", LogicalType::VARCHAR),
	                            GlobFunction, GlobFunctionBind, GlobFunctionInit);
	glob_function.table_scan_progress = GlobFunctionProgress;
	set.AddFunction(MultiFileReader::CreateFunctionSet(glob_function));
}

} // namespace duckdb
