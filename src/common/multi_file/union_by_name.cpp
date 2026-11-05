#include "duckdb/common/multi_file/union_by_name.hpp"
#include "duckdb/common/multi_file/multi_file_function.hpp"
#include "duckdb/parallel/task_executor.hpp"
#include "duckdb/common/error_data.hpp"

namespace duckdb {

class UnionByReaderTask : public BaseExecutorTask {
public:
	UnionByReaderTask(TaskExecutor &executor, ClientContext &context, const OpenFileInfo &file, idx_t file_idx,
	                  vector<shared_ptr<BaseUnionData>> &readers, BaseFileReaderOptions &options,
	                  MultiFileOptions &file_options, MultiFileReader &multi_file_reader,
	                  MultiFileReaderInterface &interface, bool ignore_errors)
	    : BaseExecutorTask(executor), context(context), file(file), file_idx(file_idx), readers(readers),
	      options(options), file_options(file_options), multi_file_reader(multi_file_reader), interface(interface),
	      ignore_errors(ignore_errors) {
	}

	void ExecuteTask() override {
		try {
			auto reader = multi_file_reader.CreateReader(context, file, options, file_options, interface);
			if (reader) {
				readers[file_idx] = reader->GetUnionData(file_idx);
			}
		} catch (std::exception &ex) {
			ErrorData error(ex);
			if (!ignore_errors || error.Type() == ExceptionType::INTERRUPT) {
				throw;
			}
		}
	}

	string TaskType() const override {
		return "UnionByReaderTask";
	}

private:
	ClientContext &context;
	const OpenFileInfo &file;
	idx_t file_idx;
	vector<shared_ptr<BaseUnionData>> &readers;
	BaseFileReaderOptions &options;
	MultiFileOptions &file_options;
	MultiFileReader &multi_file_reader;
	MultiFileReaderInterface &interface;
	bool ignore_errors;
};

vector<shared_ptr<BaseUnionData>>
UnionByName::ReadUnionData(ClientContext &context, const vector<OpenFileInfo> &files, BaseFileReaderOptions &options,
                           MultiFileOptions &file_options, MultiFileReader &multi_file_reader,
                           MultiFileReaderInterface &interface, bool ignore_errors, TaskSchedulerType scheduler_type) {
	vector<shared_ptr<BaseUnionData>> union_readers;
	union_readers.resize(files.size());

	TaskExecutor executor(context, scheduler_type);
	// schedule tasks for all files
	for (idx_t file_idx = 0; file_idx < files.size(); ++file_idx) {
		auto task = make_uniq<UnionByReaderTask>(executor, context, files[file_idx], file_idx, union_readers, options,
		                                         file_options, multi_file_reader, interface, ignore_errors);
		executor.ScheduleTask(std::move(task));
	}
	// complete all tasks
	executor.WorkOnTasks();
	return union_readers;
}

vector<shared_ptr<BaseUnionData>> UnionByName::UnionCols(ClientContext &context, const vector<OpenFileInfo> &files,
                                                         vector<LogicalType> &union_col_types,
                                                         vector<Identifier> &union_col_names,
                                                         BaseFileReaderOptions &options, MultiFileOptions &file_options,
                                                         MultiFileReader &multi_file_reader,
                                                         MultiFileReaderInterface &interface) {
	auto union_readers = ReadUnionData(context, files, options, file_options, multi_file_reader, interface);

	// now combine the result schemas
	interface.CombineSchemas(context, union_readers, file_options.union_by_name, union_col_types, union_col_names);
	return union_readers;
}

} // namespace duckdb
