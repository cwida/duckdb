#include "catch.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/execution/operator/persistent/copy_output_lifecycle.hpp"
#include "duckdb/execution/operator/persistent/physical_copy_to_file.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/function/copy_function.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/extension_manager.hpp"
#include "duckdb/parallel/event.hpp"
#include "duckdb/parallel/pipeline.hpp"
#include "duckdb/parallel/thread_context.hpp"
#include "test_helpers.hpp"

#include <chrono>
#include <condition_variable>
#include <future>

using namespace duckdb;

namespace {

struct LifecycleCopyInfo : CopyFunctionInfo {
	idx_t fail_finalize_after = DConstants::INVALID_INDEX;
	atomic<idx_t> finalize_attempts {0};
	mutex lock;
	vector<string> initialized_paths;
	vector<string> finalized_paths;
};

struct LifecycleCopyBindData : FunctionData {
	explicit LifecycleCopyBindData(shared_ptr<CopyFunctionInfo> info_p) : info(std::move(info_p)) {
	}

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<LifecycleCopyBindData>(info);
	}

	bool Equals(const FunctionData &other_p) const override {
		return info == other_p.Cast<LifecycleCopyBindData>().info;
	}

	LifecycleCopyInfo &GetInfo() {
		return info->Cast<LifecycleCopyInfo>();
	}

	shared_ptr<CopyFunctionInfo> info;
};

struct LifecycleCopyLocalData : LocalFunctionData {};

struct LifecycleCopyPreparedData : PreparedBatchData {};

// A remote output sink for the lifecycle COPY function, which does not write payloads.
// No network service or local backing files are needed to exercise partition path construction.
class PartitionCopyFileSystem : public FileSystem {
public:
	struct Handle : FileHandle {
		Handle(FileSystem &fs, const string &path, FileOpenFlags flags) : FileHandle(fs, path, flags) {
		}
		void Close() override {
		}
	};

	string GetName() const override {
		return "PartitionCopyFileSystem";
	}
	bool CanHandleFile(const string &path) override {
		return StringUtil::StartsWith(path, "s3://partition-copy-test/");
	}
	bool IsManuallySet() override {
		return true;
	}
	string PathSeparator(const string &) override {
		return "/";
	}
	unique_ptr<FileHandle> OpenFile(const string &path, FileOpenFlags flags, optional_ptr<FileOpener>) override {
		return make_uniq<Handle>(*this, path, flags);
	}
	bool FileExists(const string &, optional_ptr<FileOpener>) override {
		return false;
	}
	bool IsPipe(const string &, optional_ptr<FileOpener>) override {
		return false;
	}
	bool DirectoryExists(const string &, optional_ptr<FileOpener>) override {
		return false;
	}
	bool CreateDirectoryExtended(const string &, const CreateDirectoryOptions &, optional_ptr<FileOpener>) override {
		return false;
	}
	bool ListFiles(const string &, const std::function<void(const string &, bool)> &, FileOpener *) override {
		return true;
	}
};

// A remote output sink that counts the existence checks of a COPY, the files it opens and removes, and keeps nothing.
class ExistenceCountingFileSystem : public FileSystem {
public:
	struct Handle : FileHandle {
		Handle(FileSystem &fs, const string &path, FileOpenFlags flags) : FileHandle(fs, path, flags) {
		}
		void Close() override {
		}
		idx_t size = 0;
	};

	string GetName() const override {
		return "ExistenceCountingFileSystem";
	}
	bool CanHandleFile(const string &path) override {
		return StringUtil::StartsWith(path, "s3://existence-check-test/");
	}
	bool IsManuallySet() override {
		return true;
	}
	string PathSeparator(const string &) override {
		return "/";
	}
	unique_ptr<FileHandle> OpenFile(const string &path, FileOpenFlags flags, optional_ptr<FileOpener>) override {
		opened_files++;
		if (flags.ExclusiveCreate()) {
			exclusive_opens++;
		}
		return make_uniq<Handle>(*this, path, flags);
	}
	void RemoveFile(const string &, optional_ptr<FileOpener>) override {
		removed_files++;
	}
	bool TryRemoveFile(const string &, optional_ptr<FileOpener>) override {
		removed_files++;
		return true;
	}
	void RemoveFiles(const vector<string> &paths, optional_ptr<FileOpener>) override {
		removed_files += paths.size();
	}
	int64_t Write(FileHandle &handle, void *, int64_t nr_bytes) override {
		handle.Cast<Handle>().size += NumericCast<idx_t>(nr_bytes);
		return nr_bytes;
	}
	int64_t GetFileSize(FileHandle &handle) override {
		return NumericCast<int64_t>(handle.Cast<Handle>().size);
	}
	void FileSync(FileHandle &) override {
	}
	bool FileExists(const string &, optional_ptr<FileOpener>) override {
		existence_checks++;
		return false;
	}
	bool IsPipe(const string &, optional_ptr<FileOpener>) override {
		return false;
	}
	bool DirectoryExists(const string &, optional_ptr<FileOpener>) override {
		return false;
	}
	bool CreateDirectoryExtended(const string &, const CreateDirectoryOptions &, optional_ptr<FileOpener>) override {
		return false;
	}
	bool ListFiles(const string &, const std::function<void(const string &, bool)> &, FileOpener *) override {
		return true;
	}

	atomic<idx_t> existence_checks {0};
	atomic<idx_t> opened_files {0};
	atomic<idx_t> exclusive_opens {0};
	atomic<idx_t> removed_files {0};
};

struct LifecycleCopyGlobalData : GlobalFunctionData {
	LifecycleCopyGlobalData(ClientContext &context, string path_p)
	    : path(std::move(path_p)),
	      handle(FileSystem::GetFileSystem(context).OpenFile(path, FileFlags::FILE_FLAGS_WRITE |
	                                                                   FileFlags::FILE_FLAGS_FILE_CREATE_NEW |
	                                                                   FileFlags::FILE_FLAGS_EXCLUSIVE_CREATE)) {
	}

	~LifecycleCopyGlobalData() override {
		if (!handle) {
			return;
		}
		try {
			handle->AbortWrite();
		} catch (...) { // NOLINT
		}
	}

	string path;
	unique_ptr<FileHandle> handle;
	idx_t flushed_batches = 0;
};

unique_ptr<FunctionData> LifecycleCopyBind(ClientContext &, CopyFunctionBindInput &input, const vector<Identifier> &,
                                           const vector<LogicalType> &) {
	return make_uniq<LifecycleCopyBindData>(input.function_info);
}

unique_ptr<LocalFunctionData> LifecycleCopyInitializeLocal(ExecutionContext &, FunctionData &) {
	return make_uniq<LifecycleCopyLocalData>();
}

unique_ptr<GlobalFunctionData> LifecycleCopyInitializeGlobal(ClientContext &context, FunctionData &bind_data,
                                                             const string &path) {
	auto state = make_uniq<LifecycleCopyGlobalData>(context, path);
	auto &info = bind_data.Cast<LifecycleCopyBindData>().GetInfo();
	lock_guard<mutex> guard(info.lock);
	info.initialized_paths.push_back(path);
	return std::move(state);
}

void LifecycleCopySink(ExecutionContext &, FunctionData &, GlobalFunctionData &, LocalFunctionData &, DataChunk &) {
}

void LifecycleCopyCombine(ExecutionContext &, FunctionData &, GlobalFunctionData &, LocalFunctionData &) {
}

void LifecycleCopyFinalize(ClientContext &, FunctionData &bind_data, GlobalFunctionData &global_data) {
	auto &info = bind_data.Cast<LifecycleCopyBindData>().GetInfo();
	auto attempt = info.finalize_attempts.fetch_add(1);
	if (attempt >= info.fail_finalize_after) {
		throw IOException("Injected COPY finalization failure");
	}
	auto &state = global_data.Cast<LifecycleCopyGlobalData>();
	state.handle->Close();
	state.handle.reset();
	lock_guard<mutex> guard(info.lock);
	info.finalized_paths.push_back(state.path);
}

void LifecycleCopyGetStatistics(ClientContext &, FunctionData &, GlobalFunctionData &,
                                CopyFunctionFileStatistics &statistics) {
	statistics.footer_size_bytes = Value();
}

CopyFunctionExecutionMode LifecycleRegularExecutionMode(bool, bool) {
	return CopyFunctionExecutionMode::REGULAR_COPY_TO_FILE;
}

CopyFunctionExecutionMode LifecycleBatchExecutionMode(bool, bool) {
	return CopyFunctionExecutionMode::BATCH_COPY_TO_FILE;
}

unique_ptr<PreparedBatchData> LifecycleCopyPrepareBatch(ClientContext &, FunctionData &, GlobalFunctionData &,
                                                        unique_ptr<ColumnDataCollection>) {
	return make_uniq<LifecycleCopyPreparedData>();
}

void LifecycleCopyFlushBatch(ClientContext &, FunctionData &, GlobalFunctionData &global_data, PreparedBatchData &) {
	global_data.Cast<LifecycleCopyGlobalData>().flushed_batches++;
}

idx_t LifecycleCopyFileSize(GlobalFunctionData &global_data) {
	return global_data.Cast<LifecycleCopyGlobalData>().flushed_batches;
}

CopyFunction CreateLifecycleCopyFunction(const string &name, const shared_ptr<LifecycleCopyInfo> &info,
                                         bool batch_mode = false) {
	CopyFunction function {Identifier(name)};
	function.copy_to_bind = LifecycleCopyBind;
	function.copy_to_initialize_local = LifecycleCopyInitializeLocal;
	function.copy_to_initialize_global = LifecycleCopyInitializeGlobal;
	function.copy_to_get_written_statistics = LifecycleCopyGetStatistics;
	function.copy_to_sink = LifecycleCopySink;
	function.copy_to_combine = LifecycleCopyCombine;
	function.copy_to_finalize = LifecycleCopyFinalize;
	function.execution_mode = batch_mode ? LifecycleBatchExecutionMode : LifecycleRegularExecutionMode;
	function.prepare_batch = LifecycleCopyPrepareBatch;
	function.flush_batch = LifecycleCopyFlushBatch;
	function.file_size_bytes = LifecycleCopyFileSize;
	function.extension = "test";
	function.function_info = info;
	return function;
}

void RegisterLifecycleCopyFunction(DuckDB &db, const string &name, const shared_ptr<LifecycleCopyInfo> &info,
                                   bool batch_mode = false) {
	ExtensionInfo extension_info {};
	ExtensionActiveLoad load_info {*db.instance, extension_info, "copy_output_lifecycle_test", ""};
	ExtensionLoader loader {load_info};
	loader.RegisterFunction(CreateLifecycleCopyFunction(name, info, batch_mode));
}

void RemoveDirectoryIfPresent(FileSystem &fs, const string &path) {
	if (fs.DirectoryExists(path)) {
		fs.RemoveDirectoryExtended(path, {RemoveDirectoryMode::RECURSIVE});
	}
}

} // namespace

#ifndef DUCKDB_NO_THREADS
namespace {

struct BackpressureCopyInfo : LifecycleCopyInfo {
	std::condition_variable cv;
	bool flush_started = false;
	bool flush_released = false;
	vector<int64_t> rows;

	bool WaitForFlush() {
		unique_lock<mutex> guard(lock);
		return cv.wait_for(guard, std::chrono::seconds(10), [&]() { return flush_started; });
	}

	void ReleaseFlush() {
		lock_guard<mutex> guard(lock);
		flush_released = true;
		cv.notify_all();
	}
};

struct BackpressurePreparedData : PreparedBatchData {
	vector<int64_t> rows;
};

unique_ptr<PreparedBatchData> BackpressurePrepare(ClientContext &, FunctionData &bind_data, GlobalFunctionData &,
                                                  unique_ptr<ColumnDataCollection> collection) {
	auto &info = bind_data.Cast<LifecycleCopyBindData>().info->Cast<BackpressureCopyInfo>();
	{
		unique_lock<mutex> guard(info.lock);
		if (!info.flush_started) {
			info.flush_started = true;
			info.cv.notify_all();
			info.cv.wait(guard, [&]() { return info.flush_released; });
		}
	}
	auto result = make_uniq<BackpressurePreparedData>();
	ColumnDataScanState scan;
	collection->InitializeScan(scan);
	DataChunk chunk;
	collection->InitializeScanChunk(scan, chunk);
	while (collection->Scan(scan, chunk)) {
		for (auto row : chunk.data[0].Values<int64_t>()) {
			result->rows.push_back(row.GetValue());
		}
	}
	return std::move(result);
}

void BackpressureFlush(ClientContext &, FunctionData &bind_data, GlobalFunctionData &, PreparedBatchData &batch) {
	auto &info = bind_data.Cast<LifecycleCopyBindData>().info->Cast<BackpressureCopyInfo>();
	lock_guard<mutex> guard(info.lock);
	auto &rows = batch.Cast<BackpressurePreparedData>().rows;
	info.rows.insert(info.rows.end(), rows.begin(), rows.end());
}

class BackpressureTask : public Task {
public:
	TaskExecutionResult Execute(TaskExecutionMode) override {
		return TaskExecutionResult::TASK_FINISHED;
	}
	void Reschedule() override {
		++wakeups;
	}
	atomic<idx_t> wakeups {0};
};

class BackpressureEvent : public Event {
public:
	explicit BackpressureEvent(Executor &executor) : Event(executor) {
	}
	void Schedule() override {
	}
};

struct BackpressureFlushGuard {
	BackpressureFlushGuard(BackpressureCopyInfo &info, std::future<SinkResultType> sink)
	    : info(info), sink(std::move(sink)) {
	}
	~BackpressureFlushGuard() {
		info.ReleaseFlush();
		if (sink.valid()) {
			sink.wait();
		}
	}
	BackpressureCopyInfo &info;
	std::future<SinkResultType> sink;
};

} // namespace

TEST_CASE("Partitioned COPY bounds overlapping input and wakes combined producers", "[api][copy]") {
	DuckDB db(nullptr);
	Connection connection(db);
	REQUIRE_NO_FAIL(connection.Query("SET threads=2"));
	REQUIRE_NO_FAIL(connection.Query("SET async_threads=0"));
	REQUIRE_NO_FAIL(
	    connection.Query(StringUtil::Format("SET partitioned_write_flush_threshold=%llu", STANDARD_VECTOR_SIZE)));
	connection.BeginTransaction();
	auto pending = connection.Submit("SELECT 1");
	REQUIRE_NO_FAIL(*pending);

	auto info = make_shared_ptr<BackpressureCopyInfo>();
	auto function = CreateLifecycleCopyFunction("copy_backpressure", info);
	function.prepare_batch = BackpressurePrepare;
	function.flush_batch = BackpressureFlush;
	auto &context = *connection.context;
	PhysicalPlan plan(Allocator::Get(context));
	auto &copy = plan.Make<PhysicalCopyToFile>(vector<LogicalType> {LogicalType::BIGINT}, function,
	                                           make_uniq<LifecycleCopyBindData>(info), 0)
	                 .Cast<PhysicalCopyToFile>();
	copy.expected_types = {LogicalType::BIGINT, LogicalType::BIGINT};
	copy.names = {Identifier("p"), Identifier("i")};
	copy.file_path = TestCreatePath("copy_backpressure");
	copy.file_extension = "test";
	copy.per_thread_output = false;
	copy.partition_output = true;
	copy.partition_columns = {0};
	copy.write_partition_columns = false;
	copy.hive_file_pattern = true;
	copy.use_tmp_file = false;
	copy.write_empty_file = true;
	copy.overwrite_mode = CopyOverwriteMode::COPY_OVERWRITE;
	copy.return_type = CopyFunctionReturnType::CHANGED_ROWS;
	copy.batch_size = STANDARD_VECTOR_SIZE;
	copy.sink_state = copy.GetGlobalSinkState(context);

	ThreadContext first_thread(context), second_thread(context);
	ExecutionContext first_context(context, first_thread, nullptr), second_context(context, second_thread, nullptr);
	auto first_local = copy.GetLocalSinkState(first_context);
	auto second_local = copy.GetLocalSinkState(second_context);
	auto task = make_shared_ptr<BackpressureTask>();
	auto second_task = make_shared_ptr<BackpressureTask>();
	InterruptState interrupt(task);
	InterruptState second_interrupt(second_task);
	OperatorSinkInput first_input {*copy.sink_state, *first_local, interrupt};
	OperatorSinkInput second_input {*copy.sink_state, *second_local, second_interrupt};
	DataChunk first_chunk, second_chunk;
	first_chunk.Initialize(context, copy.expected_types);
	second_chunk.Initialize(context, copy.expected_types);
	const auto fill_chunk = [&](DataChunk &chunk, idx_t chunk_idx) {
		chunk.Reset();
		auto partitions = FlatVector::Writer<int64_t>(chunk.data[0], STANDARD_VECTOR_SIZE);
		auto values = FlatVector::Writer<int64_t>(chunk.data[1], STANDARD_VECTOR_SIZE);
		for (idx_t i = 0; i < STANDARD_VECTOR_SIZE; i++) {
			partitions.WriteValue(0);
			values.WriteValue(NumericCast<int64_t>(chunk_idx * STANDARD_VECTOR_SIZE + i));
		}
		REQUIRE(chunk.size() == STANDARD_VECTOR_SIZE);
	};
	const auto sink_first = [&](idx_t chunk_idx) {
		fill_chunk(first_chunk, chunk_idx);
		return copy.Sink(first_context, first_chunk, first_input);
	};

	REQUIRE(sink_first(0) == SinkResultType::NEED_MORE_INPUT);
	fill_chunk(second_chunk, 1);
	REQUIRE(copy.Sink(second_context, second_chunk, second_input) == SinkResultType::NEED_MORE_INPUT);
	REQUIRE(sink_first(2) == SinkResultType::NEED_MORE_INPUT);
	fill_chunk(second_chunk, 3);
	BackpressureFlushGuard stalled(
	    *info, std::async(std::launch::async, [&]() { return copy.Sink(second_context, second_chunk, second_input); }));
	if (!info->WaitForFlush()) {
		if (stalled.sink.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
			(void)stalled.sink.get();
		}
		FAIL("The first flush did not reach the stall");
	}

	// Input overlaps the stalled flush, but the next state cannot grow indefinitely.
	REQUIRE(sink_first(4) == SinkResultType::NEED_MORE_INPUT);
	REQUIRE(sink_first(5) == SinkResultType::NEED_MORE_INPUT);
	REQUIRE(sink_first(6) == SinkResultType::BLOCKED);
	REQUIRE(task->wakeups == 0);

	const auto interrupt_while_blocked = GENERATE(false, true);
	if (interrupt_while_blocked) {
		connection.Interrupt();
		REQUIRE_THROWS_AS(copy.Sink(first_context, first_chunk, first_input), InterruptException);
		context.ClearInterrupt();
	}

	info->ReleaseFlush();
	REQUIRE(stalled.sink.get() == SinkResultType::NEED_MORE_INPUT);
	REQUIRE(task->wakeups > 0);
	REQUIRE(copy.Sink(first_context, first_chunk, first_input) == SinkResultType::NEED_MORE_INPUT);
	REQUIRE(sink_first(7) == SinkResultType::NEED_MORE_INPUT);
	REQUIRE(sink_first(8) == SinkResultType::BLOCKED);
	auto wakeups_before_combine = task->wakeups.load();

	// The exhausted producer supplies the last combine needed by the active flush.
	OperatorSinkCombineInput second_combine {*copy.sink_state, *second_local, second_interrupt};
	REQUIRE(copy.Combine(second_context, second_combine) == SinkCombineResultType::BLOCKED);
	REQUIRE(task->wakeups > wakeups_before_combine);
	REQUIRE(second_task->wakeups == 0);
	if (interrupt_while_blocked) {
		connection.Interrupt();
		REQUIRE_THROWS_AS(copy.Combine(second_context, second_combine), InterruptException);
		context.ClearInterrupt();
	}
	REQUIRE(copy.Sink(first_context, first_chunk, first_input) == SinkResultType::NEED_MORE_INPUT);
	REQUIRE(sink_first(9) == SinkResultType::NEED_MORE_INPUT);
	OperatorSinkCombineInput first_combine {*copy.sink_state, *first_local, interrupt};
	REQUIRE(copy.Combine(first_context, first_combine) == SinkCombineResultType::FINISHED);
	REQUIRE(second_task->wakeups > 0);
	REQUIRE(copy.Combine(second_context, second_combine) == SinkCombineResultType::FINISHED);

	auto &executor = Executor::Get(context);
	auto pipeline = make_shared_ptr<Pipeline>(executor);
	auto event = make_shared_ptr<BackpressureEvent>(executor);
	auto completed = make_shared_ptr<BackpressureEvent>(executor);
	completed->AddDependency(*event);
	OperatorSinkFinalizeInput finalize_input {*copy.sink_state, interrupt};
	REQUIRE(copy.Finalize(*pipeline, *event, context, finalize_input) == SinkFinalizeType::READY);
	event->Finish();
	while (!completed->IsFinished() && !executor.HasError()) {
		executor.WorkOnTasks();
	}
	if (executor.HasError()) {
		executor.ThrowException();
	}
	std::sort(info->rows.begin(), info->rows.end());
	REQUIRE(info->rows.size() == 10 * STANDARD_VECTOR_SIZE);
	for (idx_t i = 0; i < info->rows.size(); i++) {
		REQUIRE(info->rows[i] == NumericCast<int64_t>(i));
	}
	auto source = copy.GetGlobalSourceState(context);
	auto local_source = copy.GetLocalSourceState(first_context, *source);
	OperatorSourceInput source_input {*source, *local_source, interrupt};
	DataChunk count;
	count.Initialize(context, copy.GetTypes());
	REQUIRE(copy.GetData(first_context, count, source_input) == SourceResultType::FINISHED);
	REQUIRE(count.GetValue(0, 0) == Value::BIGINT(10 * STANDARD_VECTOR_SIZE));
	pending->Complete();
	REQUIRE_NO_FAIL(*pending);
}
#endif

TEST_CASE("Partitioned COPY to remote storage does not require the local filesystem", "[api][copy]") {
	DuckDB db(nullptr);
	Connection connection(db);
	auto &fs = FileSystem::GetFileSystem(*connection.context);
	fs.RegisterSubSystem(make_uniq<PartitionCopyFileSystem>());
	auto info = make_shared_ptr<LifecycleCopyInfo>();
	RegisterLifecycleCopyFunction(db, "copy_remote_partitions", info);
	REQUIRE_NO_FAIL(connection.Query("SET disabled_filesystems='LocalFileSystem'"));

	string path_option;
	string directory;
	SECTION("Default Hive partition path") {
		directory = "p=1";
	}
	SECTION("Explicit partition path") {
		path_option = ", PARTITION_PATH 'custom/part_' || p::VARCHAR";
		directory = "custom/part_1";
	}
	REQUIRE_NO_FAIL(connection.Query("COPY (SELECT 1 AS p, 42 AS v) TO 's3://partition-copy-test/output' "
	                                 "(FORMAT copy_remote_partitions, PARTITION_BY (p), FILENAME_PATTERN 'part_{i}'" +
	                                 path_option + ")"));
	REQUIRE(info->finalized_paths.size() == 1);
	REQUIRE(info->finalized_paths[0] == "s3://partition-copy-test/output/" + directory + "/part_0.test");

	// A genuinely local target must still enforce the disabled filesystem setting.
	auto local_result = connection.Query("COPY (SELECT 1 AS p, 42 AS v) TO 'partition_copy_local' "
	                                     "(FORMAT copy_remote_partitions, PARTITION_BY (p))");
	REQUIRE_FAIL(local_result);
	REQUIRE(local_result->GetError().find("LocalFileSystem has been disabled") != string::npos);
}

TEST_CASE("COPY to remote storage does not check whether each output file exists", "[api][copy]") {
	DuckDB db(nullptr);
	Connection connection(db);
	auto &fs = FileSystem::GetFileSystem(*connection.context);
	auto counting_fs = make_uniq<ExistenceCountingFileSystem>();
	auto &counting = *counting_fs;
	fs.RegisterSubSystem(std::move(counting_fs));

	string format;
	SECTION("Parquet") {
		format = "parquet";
	}
	SECTION("CSV") {
		format = "csv";
	}
	REQUIRE_NO_FAIL(connection.Query("COPY (SELECT i % 3 AS p, i FROM range(10) t(i)) TO "
	                                 "'s3://existence-check-test/partitioned' (FORMAT " +
	                                 format + ", PARTITION_BY (p))"));
	REQUIRE_NO_FAIL(connection.Query("COPY (SELECT 42 AS i) TO 's3://existence-check-test/single." + format +
	                                 "' (FORMAT " + format + ")"));
	REQUIRE(counting.opened_files == 4);
	REQUIRE(counting.existence_checks == 0);
	REQUIRE(counting.exclusive_opens == 0);
}

TEST_CASE("A failed COPY to remote storage keeps its outputs", "[api][copy]") {
	DuckDB db(nullptr);
	Connection connection(db);
	REQUIRE_NO_FAIL(connection.Query("SET threads=1"));
	REQUIRE_NO_FAIL(connection.Query("SET async_threads=0"));
	auto &fs = FileSystem::GetFileSystem(*connection.context);
	auto counting_fs = make_uniq<ExistenceCountingFileSystem>();
	auto &counting = *counting_fs;
	fs.RegisterSubSystem(std::move(counting_fs));

	auto info = make_shared_ptr<LifecycleCopyInfo>();
	info->fail_finalize_after = 1;
	RegisterLifecycleCopyFunction(db, "copy_remote_failure", info);
	auto result = connection.Query("COPY (SELECT i AS p, 42 AS v FROM range(4) t(i)) TO "
	                               "'s3://existence-check-test/failed' (FORMAT copy_remote_failure, PARTITION_BY (p))");
	REQUIRE_FAIL(result);
	result.reset();
	REQUIRE(!info->finalized_paths.empty());
	// whether a remote output existed before is not checked, so none is removed
	REQUIRE(counting.removed_files == 0);
}

TEST_CASE("COPY output lifecycle removes owned files after failure", "[api][copy]") {
	DuckDB db(nullptr);
	Connection connection(db);
	auto &fs = FileSystem::GetFileSystem(*connection.context);

	auto finalized_path = TestCreatePath("copy_lifecycle_finalized.test");
	fs.TryRemoveFile(finalized_path);
	{
		CopyOutputLifecycle lifecycle(*connection.context);
		lifecycle.RegisterFile(finalized_path);
		auto handle = fs.OpenFile(finalized_path, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW);
		handle->Close();
	}
	REQUIRE(!fs.FileExists(finalized_path));

	auto incomplete_path = TestCreatePath("copy_lifecycle_incomplete.test");
	fs.TryRemoveFile(incomplete_path);
	{
		CopyOutputLifecycle lifecycle(*connection.context);
		lifecycle.RegisterFile(incomplete_path);
		auto handle = fs.OpenFile(incomplete_path, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW);
		handle->Close();
	}
	REQUIRE(!fs.FileExists(incomplete_path));

	auto existing_path = TestCreatePath("copy_lifecycle_existing.test");
	fs.TryRemoveFile(existing_path);
	{
		auto handle = fs.OpenFile(existing_path, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW);
		handle->Close();
	}
	{
		CopyOutputLifecycle lifecycle(*connection.context);
		lifecycle.RegisterFile(existing_path);
	}
	REQUIRE(fs.FileExists(existing_path));
	fs.RemoveFile(existing_path);

	auto successful_path = TestCreatePath("copy_lifecycle_success.test");
	fs.TryRemoveFile(successful_path);
	{
		CopyOutputLifecycle lifecycle(*connection.context);
		lifecycle.RegisterFile(successful_path);
		auto handle = fs.OpenFile(successful_path, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW);
		handle->Close();
		lifecycle.MarkSuccessful();
	}
	REQUIRE(fs.FileExists(successful_path));
	fs.RemoveFile(successful_path);
}

TEST_CASE("COPY output lifecycle removes only empty query-created directories", "[api][copy]") {
	DuckDB db(nullptr);
	Connection connection(db);
	auto &fs = FileSystem::GetFileSystem(*connection.context);

	auto root = TestCreatePath("copy_lifecycle_directories");
	RemoveDirectoryIfPresent(fs, root);
	auto child = fs.JoinPath(root, "child");
	fs.CreateDirectoryExtended(child, {CreateDirectoryMode::RECURSIVE});
	{
		CopyOutputLifecycle lifecycle(*connection.context);
		lifecycle.RegisterCreatedDirectory(root);
		lifecycle.RegisterCreatedDirectory(child);
	}
	REQUIRE(!fs.DirectoryExists(child));
	REQUIRE(!fs.DirectoryExists(root));

	fs.CreateDirectory(root);
	fs.CreateDirectory(child);
	auto retained_file = fs.JoinPath(child, "retained.test");
	{
		auto handle = fs.OpenFile(retained_file, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW);
		handle->Close();
	}
	{
		CopyOutputLifecycle lifecycle(*connection.context);
		lifecycle.RegisterCreatedDirectory(root);
		lifecycle.RegisterCreatedDirectory(child);
	}
	REQUIRE(fs.FileExists(retained_file));
	REQUIRE(fs.DirectoryExists(child));
	REQUIRE(fs.DirectoryExists(root));
	RemoveDirectoryIfPresent(fs, root);
}

TEST_CASE("COPY removes an open output after finalization fails", "[api][copy]") {
	for (auto batch_mode : {false, true}) {
		DuckDB db(nullptr);
		Connection connection(db);
		REQUIRE_NO_FAIL(connection.Query(batch_mode ? "SET threads=4" : "SET threads=1"));
		if (batch_mode) {
			REQUIRE_NO_FAIL(connection.Query("CREATE TABLE copy_lifecycle_batch_input AS FROM range(4096)"));
		}
		auto &fs = FileSystem::GetFileSystem(*connection.context);
		string name = batch_mode ? "copy_lifecycle_batch_finalize_failure" : "copy_lifecycle_regular_finalize_failure";
		auto info = make_shared_ptr<LifecycleCopyInfo>();
		info->fail_finalize_after = 0;
		RegisterLifecycleCopyFunction(db, name, info, batch_mode);

		auto output = TestCreatePath(name + ".test");
		fs.TryRemoveFile(output);
		auto source = batch_mode ? "copy_lifecycle_batch_input" : "(SELECT i FROM range(4096) t(i))";
		auto result = connection.Query(
		    StringUtil::Format("COPY %s TO '%s' (FORMAT %s, PRESERVE_ORDER false)", source, output, name));
		REQUIRE_FAIL(result);
		result.reset();
		REQUIRE(info->finalize_attempts == 1);
		REQUIRE(info->initialized_paths.size() == 1);
		REQUIRE(!fs.FileExists(output));
	}
}

TEST_CASE("COPY removes finalized partition outputs after a later finalization failure", "[api][copy]") {
	DuckDB db(nullptr);
	Connection connection(db);
	REQUIRE_NO_FAIL(connection.Query("SET threads=1"));
	REQUIRE_NO_FAIL(connection.Query("SET async_threads=0"));
	auto &fs = FileSystem::GetFileSystem(*connection.context);

	auto info = make_shared_ptr<LifecycleCopyInfo>();
	info->fail_finalize_after = 1;
	RegisterLifecycleCopyFunction(db, "copy_lifecycle_partition_failure", info);

	auto output = TestCreatePath("copy_lifecycle_partition_failure");
	RemoveDirectoryIfPresent(fs, output);
	auto result = connection.Query(StringUtil::Format("COPY (SELECT i AS p, 42 AS v FROM range(4) t(i)) TO '%s' "
	                                                  "(FORMAT copy_lifecycle_partition_failure, PARTITION_BY (p))",
	                                                  output));
	REQUIRE_FAIL(result);
	result.reset();
	REQUIRE(info->finalize_attempts > 1);
	REQUIRE(!info->finalized_paths.empty());
	REQUIRE(info->initialized_paths.size() >= 2);
	for (auto &path : info->initialized_paths) {
		REQUIRE(!fs.FileExists(path));
	}
	REQUIRE(!fs.DirectoryExists(output));
}

TEST_CASE("COPY removes rotated outputs after a later finalization failure", "[api][copy]") {
	DuckDB db(nullptr);
	Connection connection(db);
	REQUIRE_NO_FAIL(connection.Query("SET threads=1"));
	REQUIRE_NO_FAIL(connection.Query("SET async_threads=0"));
	auto &fs = FileSystem::GetFileSystem(*connection.context);

	auto info = make_shared_ptr<LifecycleCopyInfo>();
	info->fail_finalize_after = 1;
	RegisterLifecycleCopyFunction(db, "copy_lifecycle_rotation_failure", info);
	auto output = TestCreatePath("copy_lifecycle_rotation_failure");
	RemoveDirectoryIfPresent(fs, output);
	auto result = connection.Query(StringUtil::Format(
	    "COPY (SELECT i FROM range(8192) t(i)) TO '%s' "
	    "(FORMAT copy_lifecycle_rotation_failure, FILE_SIZE_BYTES '1B', BATCH_SIZE 2048, PRESERVE_ORDER false)",
	    output));
	REQUIRE_FAIL(result);
	result.reset();
	REQUIRE(info->finalize_attempts > 1);
	REQUIRE(!info->finalized_paths.empty());
	REQUIRE(info->initialized_paths.size() >= 2);
	for (auto &path : info->initialized_paths) {
		REQUIRE(!fs.FileExists(path));
	}
	REQUIRE(!fs.DirectoryExists(output));
}

TEST_CASE("COPY cleans a finalized temporary file when its move fails", "[api][copy]") {
	for (auto batch_mode : {false, true}) {
		DuckDB db(nullptr);
		Connection connection(db);
		REQUIRE_NO_FAIL(connection.Query(batch_mode ? "SET threads=4" : "SET threads=1"));
		if (batch_mode) {
			REQUIRE_NO_FAIL(connection.Query("CREATE TABLE copy_lifecycle_batch_input AS FROM range(4096)"));
		}
		auto &fs = FileSystem::GetFileSystem(*connection.context);
		string name = batch_mode ? "copy_lifecycle_batch_move" : "copy_lifecycle_regular_move";
		auto info = make_shared_ptr<LifecycleCopyInfo>();
		RegisterLifecycleCopyFunction(db, name, info, batch_mode);

		auto target = TestCreatePath(name + ".test");
		auto temporary = fs.JoinPath(StringUtil::GetFilePath(target), "tmp_" + StringUtil::GetFileName(target));
		fs.TryRemoveFile(temporary);
		RemoveDirectoryIfPresent(fs, target);
		fs.CreateDirectory(target);
		auto source = batch_mode ? "copy_lifecycle_batch_input" : "(SELECT i FROM range(4096) t(i))";
		auto result = connection.Query(StringUtil::Format(
		    "COPY %s TO '%s' (FORMAT %s, USE_TMP_FILE true, PRESERVE_ORDER false)", source, target, name));
		REQUIRE_FAIL(result);
		result.reset();
		REQUIRE(!fs.FileExists(temporary));
		REQUIRE(fs.DirectoryExists(target));
		fs.RemoveDirectory(target);
	}
}

TEST_CASE("Abandoning multi-chunk COPY statistics preserves finalized files", "[api][copy]") {
	DuckDB db(nullptr);
	Connection connection(db);
	REQUIRE_NO_FAIL(connection.Query("SET threads=1"));
	REQUIRE_NO_FAIL(connection.Query("SET async_threads=0"));
	auto &fs = FileSystem::GetFileSystem(*connection.context);

	auto info = make_shared_ptr<LifecycleCopyInfo>();
	RegisterLifecycleCopyFunction(db, "copy_lifecycle_abandon_stats", info);
	auto output = TestCreatePath("copy_lifecycle_abandon_stats");
	RemoveDirectoryIfPresent(fs, output);

	auto result =
	    connection.Query(StringUtil::Format("COPY (SELECT i AS p, 42 AS v FROM range(%d) t(i)) TO '%s' "
	                                        "(FORMAT copy_lifecycle_abandon_stats, PARTITION_BY (p), RETURN_STATS)",
	                                        STANDARD_VECTOR_SIZE + 1, output));
	REQUIRE_NO_FAIL(*result);
	auto first_chunk = result->Fetch();
	REQUIRE(first_chunk);
	REQUIRE(first_chunk->size() == STANDARD_VECTOR_SIZE);
	result.reset();

	REQUIRE(info->finalized_paths.size() == STANDARD_VECTOR_SIZE + 1);
	for (auto &path : info->finalized_paths) {
		REQUIRE(fs.FileExists(path));
	}
	RemoveDirectoryIfPresent(fs, output);
}
