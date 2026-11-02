#include "duckdb/execution/operator/persistent/physical_copy_to_file.hpp"

#include "duckdb/common/file_opener.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/path.hpp"
#include "duckdb/common/optional.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/sorting/sort_strategy.hpp"
#include "duckdb/common/types/row/integer_partitioned_tuple_data.hpp"
#include "duckdb/common/types/row/tuple_data_iterator.hpp"
#include "duckdb/common/types/row/tuple_data_collection.hpp"
#include "duckdb/common/enums/memory_tag.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/common/value_operations/value_operations.hpp"
#include "duckdb/common/vector/vector_iterator.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/function/scalar/string_functions.hpp"
#include "duckdb/function/window/window_collection.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/operator/logical_copy_to_file.hpp"
#include "duckdb/parallel/base_pipeline_event.hpp"
#include "duckdb/parallel/task_executor.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/execution/operator/persistent/copy_batch_slicer.hpp"
#include "duckdb/execution/operator/persistent/copy_output_lifecycle.hpp"
#include "fmt/format.h"

#include <algorithm>
#include <condition_variable>
#include <exception>
#include <functional>
#include <type_traits>
#include "duckdb/common/types/hyperloglog.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parallel/thread_context.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/logging/log_type.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Declarations
//===--------------------------------------------------------------------===//

//===--------------------------------------------------------------------===//
// Utility Declarations
//===--------------------------------------------------------------------===//
enum class PhysicalCopyToFilePhase : uint8_t { SINK, COMBINE, FINALIZE };

struct VectorOfValuesHashFunction {
	uint64_t operator()(const vector<Value> &values) const {
		hash_t result = 0;
		for (auto &val : values) {
			result ^= val.Hash();
		}
		return result;
	}
};

struct VectorOfValuesEquality {
	bool operator()(const vector<Value> &a, const vector<Value> &b) const {
		if (a.size() != b.size()) {
			return false;
		}
		for (idx_t i = 0; i < a.size(); i++) {
			if (ValueOperations::DistinctFrom(a[i], b[i])) {
				return false;
			}
		}
		return true;
	}
};

struct VectorOfValuesLess {
	bool operator()(const vector<Value> &a, const vector<Value> &b) const {
		const auto count = MinValue(a.size(), b.size());
		for (idx_t i = 0; i < count; i++) {
			if (ValueOperations::DistinctLessThan(a[i], b[i])) {
				return true;
			}
			if (ValueOperations::DistinctLessThan(b[i], a[i])) {
				return false;
			}
		}
		return a.size() < b.size();
	}
};

template <class T>
using vector_of_value_map_t = unordered_map<vector<Value>, T, VectorOfValuesHashFunction, VectorOfValuesEquality>;

//===--------------------------------------------------------------------===//
// Copy File State Types
//===--------------------------------------------------------------------===//
struct GlobalFileState {
public:
	explicit GlobalFileState(unique_ptr<GlobalFunctionData> data_p, const string &path_p)
	    : data(std::move(data_p)), path(path_p), num_batches(0) {
	}

public:
	annotated_mutex lock;
	unique_ptr<GlobalFunctionData> data;
	const string path;
	idx_t num_batches DUCKDB_GUARDED_BY(lock);
};

//===--------------------------------------------------------------------===//
// Copy File State Declarations
//===--------------------------------------------------------------------===//
struct PendingFileState {
	string output_path;
	optional_ptr<CopyToFileInfo> written_file_info;
};

struct PartitionDirectory {
	string path;
	vector<string> directories;
	//! The directory relative to the COPY target, with "/" separators
	string relative_path;
};

enum class CopyDirectoryState : uint8_t { PENDING, COMPLETE, FAILED };

class CopyDirectoryManager {
public:
	bool EnsureDirectory(FileSystem &fs, const string &dir_path);

private:
	struct DirectoryEntry {
		CopyDirectoryState state = CopyDirectoryState::PENDING;
		std::exception_ptr error;
		bool created = false;
	};

private:
	mutex lock;
	std::condition_variable condition;
	unordered_map<string, DirectoryEntry> directories;
};

class CopyOutputFileRegistry {
public:
	explicit CopyOutputFileRegistry(const PhysicalCopyToFile &op_p) : op(op_p) {
	}

public:
	//! The registry is guarded by CopyToFileGlobalState::lock.
	PendingFileState ReserveFile(string output_path, optional_ptr<const vector<Value>> partition_values);
	void PublishCreatedPath(PendingFileState &pending_file_state, string output_path);

	idx_t WrittenFileCount() const {
		return written_files.size();
	}

	CopyToFileInfo &GetWrittenFile(idx_t file_idx) {
		return *written_files[file_idx];
	}

	const vector<unique_ptr<CopyToFileInfo>> &GetWrittenFiles() const {
		return written_files;
	}

private:
	optional_ptr<CopyToFileInfo> AddFile(const string &file_name);

private:
	const PhysicalCopyToFile &op;
	vector<unique_ptr<CopyToFileInfo>> written_files;
};

//===--------------------------------------------------------------------===//
// Copy File Lifecycle Declarations
//===--------------------------------------------------------------------===//
enum class CopyFileLifecycleWaitMode : uint8_t { INTERRUPTIBLE, DRAIN };

class CopyFileLifecycleJob {
public:
	bool IsFinished() const {
		return finished.load(std::memory_order_acquire);
	}

	void Complete() {
		finished.store(true, std::memory_order_release);
	}

	void CompleteException(const std::exception_ptr &error_p) {
		error = error_p;
		Complete();
	}

	void Rethrow() const {
		if (error) {
			std::rethrow_exception(error);
		}
	}

private:
	atomic<bool> finished {false};
	std::exception_ptr error;
};

class FileStateOpenJob : public CopyFileLifecycleJob {
public:
	void Complete(unique_ptr<GlobalFileState> file_state_p) {
		file_state = std::move(file_state_p);
		CopyFileLifecycleJob::Complete();
	}

	GlobalFileState &GetFileState() const {
		D_ASSERT(IsFinished());
		Rethrow();
		D_ASSERT(file_state);
		return *file_state;
	}

	unique_ptr<GlobalFileState> TakeFileState() {
		D_ASSERT(IsFinished());
		Rethrow();
		return std::move(file_state);
	}

private:
	unique_ptr<GlobalFileState> file_state;
};

struct PendingFileStateOpen {
	PendingFileState pending_file_state;
	shared_ptr<FileStateOpenJob> open_job;

	explicit operator bool() const {
		return open_job.get();
	}
};

class CopyToFileGlobalState;

struct PartitionFileOpenRequest {
	PartitionFileOpenRequest(PendingFileStateOpen pending_file_state_open, PartitionDirectory directory_p,
	                         idx_t offset_p)
	    : pending_file_state(std::move(pending_file_state_open.pending_file_state)),
	      open_job(std::move(pending_file_state_open.open_job)), directory(std::move(directory_p)), offset(offset_p) {
	}

	void Run(CopyToFileGlobalState &copy_gstate);

	PendingFileState pending_file_state;
	shared_ptr<FileStateOpenJob> open_job;
	PartitionDirectory directory;
	idx_t offset;
};

struct FileStateHandle {
public:
	FileStateHandle() = default;
	FileStateHandle(FileStateHandle &&) = default;
	FileStateHandle &operator=(FileStateHandle &&) = default;
	FileStateHandle(const FileStateHandle &) = delete;
	FileStateHandle &operator=(const FileStateHandle &) = delete;

public:
	bool HasFileState() const {
		return open_job.get();
	}

	bool IsReady() const {
		return open_job && open_job->IsFinished();
	}

	GlobalFileState &GetFileState() const {
		D_ASSERT(open_job);
		return open_job->GetFileState();
	}

	optional_ptr<GlobalFileState> GetFileStatePtr() const {
		if (!IsReady()) {
			return nullptr;
		}
		return open_job->GetFileState();
	}

	unique_ptr<GlobalFileState> TakeFileState() {
		if (!open_job) {
			return nullptr;
		}
		auto result = open_job->TakeFileState();
		open_job.reset();
		return result;
	}

	explicit operator bool() const {
		return HasFileState();
	}

public:
	shared_ptr<FileStateOpenJob> open_job;
};

struct PartitionFileRequest {
	PartitionFileRequest(PartitionFileOpenRequest open_request_p, vector<FileStateHandle> files_to_finalize_p)
	    : open_request(std::move(open_request_p)), files_to_finalize(std::move(files_to_finalize_p)) {
	}

	shared_ptr<FileStateOpenJob> OpenJob() const {
		return open_request.open_job;
	}

	PartitionFileOpenRequest open_request;
	vector<FileStateHandle> files_to_finalize;
};

template <class FUNC>
class CopyFileLifecycleTask;

class CopyFileLifecycleExecutor {
	static constexpr idx_t MIN_PENDING_TASKS = 4096;

public:
	explicit CopyFileLifecycleExecutor(ClientContext &context_p)
	    : context(context_p), executor(context_p, TaskSchedulerType::ASYNC) {
		auto &scheduler = TaskScheduler::GetScheduler(context);
		async_threads = scheduler.NumberOfAsyncThreads();
		auto regular_threads = scheduler.NumberOfThreads();
		max_pending_tasks = MaxValue<idx_t>(MIN_PENDING_TASKS, (async_threads + regular_threads) * 4);
	}

	~CopyFileLifecycleExecutor() {
		// A queued task's Cancel reaches back into this object (GetError, FinishTask). Join here, while every
		// member is still alive, rather than leaving it to ~TaskExecutor, which runs after error_lock and error
		// have already been destroyed. CancelAndDrain does not throw.
		executor.CancelAndDrain();
	}

public:
	template <class FUNC>
	void Schedule(shared_ptr<CopyFileLifecycleJob> job, CopyFileLifecycleWaitMode mode, FUNC &&task);
	void WaitForJob(CopyFileLifecycleJob &job, CopyFileLifecycleWaitMode mode);
	void WaitAll(CopyFileLifecycleWaitMode mode);
	void WorkOnTaskOrYield();
	void FinishTask();
	void PushError(const std::exception_ptr &error);
	//! The first error pushed by a task, if any
	std::exception_ptr GetError();

private:
	bool WorkOnTask(bool throw_error = true);
	void WaitForTaskSlot(CopyFileLifecycleWaitMode mode);
	void ThrowError();

private:
	ClientContext &context;
	TaskExecutor executor;
	idx_t async_threads;
	idx_t max_pending_tasks;
	atomic<idx_t> pending_tasks {0};
	mutex error_lock;
	std::exception_ptr error;
};

class CopyFileLifecycleTaskFinishGuard {
public:
	explicit CopyFileLifecycleTaskFinishGuard(CopyFileLifecycleExecutor &lifecycle_p) : lifecycle(lifecycle_p) {
	}

	~CopyFileLifecycleTaskFinishGuard() {
		Finish();
	}

	void Finish() {
		if (!finished) {
			lifecycle.FinishTask();
			finished = true;
		}
	}

private:
	CopyFileLifecycleExecutor &lifecycle;
	bool finished = false;
};

template <class FUNC>
class CopyFileLifecycleTask : public BaseExecutorTask {
public:
	CopyFileLifecycleTask(TaskExecutor &executor_p, CopyFileLifecycleExecutor &lifecycle_p,
	                      shared_ptr<CopyFileLifecycleJob> job_p, FUNC task_p)
	    : BaseExecutorTask(executor_p), lifecycle(lifecycle_p), job(std::move(job_p)), task(std::move(task_p)) {
	}

	~CopyFileLifecycleTask() override {
		// Neither ExecuteTask nor Cancel ran, because the executor could not take the task at all. The job
		// waiter and pending_tasks still have to be settled, or a waiter spins with nothing left to run.
		if (settled) {
			return;
		}
		try {
			Cancel();
		} catch (...) { // NOLINT
		}
	}

public:
	void ExecuteTask() override {
		settled = true;
		CopyFileLifecycleTaskFinishGuard finish_guard(lifecycle);
		try {
			task();
			if (!job->IsFinished()) {
				job->Complete();
			}
		} catch (...) {
			auto error = std::current_exception();
			job->CompleteException(error);
			lifecycle.PushError(error);
		}
	}

	void Cancel() override {
		// the task is retired without running - settle the job, WaitForJob spins until it is finished
		settled = true;
		CopyFileLifecycleTaskFinishGuard finish_guard(lifecycle);
		if (job->IsFinished()) {
			return;
		}
		auto error = lifecycle.GetError();
		if (!error) {
			error = std::make_exception_ptr(InternalException("COPY file task was cancelled before it could run"));
		}
		job->CompleteException(error);
	}

	string TaskType() const override {
		return "CopyFileLifecycleTask";
	}

private:
	CopyFileLifecycleExecutor &lifecycle;
	shared_ptr<CopyFileLifecycleJob> job;
	FUNC task;
	//! Whether ExecuteTask or Cancel ran, so the destructor knows the job and the count are settled
	bool settled = false;
};

template <class FUNC>
void CopyFileLifecycleExecutor::Schedule(shared_ptr<CopyFileLifecycleJob> job, CopyFileLifecycleWaitMode mode,
                                         FUNC &&task) {
	WaitForTaskSlot(mode);
	auto job_ref = job;
	using TaskType = CopyFileLifecycleTask<typename std::decay<FUNC>::type>;
	auto lifecycle_task = make_uniq<TaskType>(executor, *this, std::move(job), std::forward<FUNC>(task));
	// past this point the task settles pending_tasks itself, on the execute path and on the cancel path
	++pending_tasks;
	executor.ScheduleTask(std::move(lifecycle_task));
	if (async_threads == 0) {
		WaitForJob(*job_ref, mode);
	}
}

//===--------------------------------------------------------------------===//
// Copy State Declarations
//===--------------------------------------------------------------------===//
class PartitionedCopy;

class CopyToFileGlobalState : public GlobalSinkState {
public:
	CopyToFileGlobalState(const PhysicalCopyToFile &op_p, ClientContext &context_p);
	~CopyToFileGlobalState() override;

public:
	void Initialize() DUCKDB_EXCLUDES(lock);

	PendingFileState PrepareFileStateLocked(string output_path = string(),
	                                        optional_ptr<const vector<Value>> partition_values = nullptr)
	    DUCKDB_REQUIRES(lock);
	PendingFileStateOpen CreateFileStateOpenLocked(FileStateHandle &file_state, string output_path = string(),
	                                               optional_ptr<const vector<Value>> partition_values = nullptr)
	    DUCKDB_REQUIRES(lock);
	PendingFileStateOpen CreatePartitionFileStateOpenLocked(FileStateHandle &file_state, string output_path,
	                                                        optional_ptr<const vector<Value>> partition_values)
	    DUCKDB_REQUIRES(lock);
	unique_ptr<GlobalFileState> InitializeFileState(PendingFileState pending_file_state) DUCKDB_EXCLUDES(lock);
	void RegisterPrepareGlobalStateLocked(GlobalFileState &file_state) DUCKDB_REQUIRES(lock);
	void ScheduleOutputDirectorySetup() DUCKDB_EXCLUDES(lock);
	void EnsureOutputDirectoryReady() DUCKDB_EXCLUDES(lock);
	void ScheduleFileStateOpen(PendingFileStateOpen pending_file_state_open) DUCKDB_EXCLUDES(lock);
	void SchedulePartitionFileStateOpen(PartitionFileOpenRequest request) DUCKDB_EXCLUDES(lock);
	void RequestFileState(FileStateHandle &file_state, string output_path = string(),
	                      optional_ptr<const vector<Value>> partition_values = nullptr) DUCKDB_EXCLUDES(lock);
	GlobalFileState &EnsureFileStateReady(FileStateHandle &file_state,
	                                      const std::function<void(FileStateHandle &)> &create_file_state_fun)
	    DUCKDB_EXCLUDES(lock);
	FileStateHandle FinalizeFileStateLocked(FileStateHandle file_state) DUCKDB_REQUIRES(lock);
	void FinalizeFileState(FileStateHandle file_state) DUCKDB_EXCLUDES(lock);
	void AbandonFileState(FileStateHandle file_state) noexcept DUCKDB_EXCLUDES(lock);

	FileStateHandle TryFinalizeOwnedFileStateLocked() DUCKDB_REQUIRES(lock);
	void TryFinalizeOwnedFileState() DUCKDB_EXCLUDES(lock);
	void WaitForLifecycleTasks() DUCKDB_EXCLUDES(lock);
	void MarkSuccessful() noexcept;

private:
	void PrepareOutputDirectory() DUCKDB_EXCLUDES(lock);
	void EnsureDirectory(const string &dir_path) DUCKDB_EXCLUDES(lock);
	void RegisterPendingFileStatePathLocked(PendingFileState &pending_file_state, string output_path)
	    DUCKDB_REQUIRES(lock);

	friend struct PartitionFileOpenRequest;

public:
	const PhysicalCopyToFile &op;
	ClientContext &context;
	//! Destroy after file states so their handles close before cleanup.
	CopyOutputLifecycle output_lifecycle;

	//! Lock guarding the global state
	mutable annotated_mutex lock;
	//! Whether the copy was initialized
	atomic<bool> initialized;

	//! We write to files using the Prepare/Flush batch API:
	//! - Prepare gets the data ready and can take a lot of time
	//! - Flush appends to the file, which increments FILE_SIZE_BYTES/BATCHES_PER_FILE
	//! Therefore, we must delay deciding which file to flush; otherwise, parallel writes overshoot
	//! All Prepare are done against this state
	atomic<optional_ptr<GlobalFileState>> prepare_global_state;
	FileStateHandle prepare_global_state_owned DUCKDB_GUARDED_BY(lock);

	//! The (current) global state
	FileStateHandle global_state;
	//! Lambda to create a new global file state
	const std::function<void(FileStateHandle &)> create_file_state_fun;
	//! Asynchronously prepares the root output directory for directory-style COPY outputs.
	shared_ptr<CopyFileLifecycleJob> output_directory_job;
	CopyFileLifecycleExecutor lifecycle_executor;
	CopyDirectoryManager directory_manager;
	CopyOutputFileRegistry output_files;

	//! The final batch
	mutable annotated_mutex last_batch_lock;
	unique_ptr<ColumnDataCollection> last_batch DUCKDB_GUARDED_BY(last_batch_lock);

	//! Partitioning state
	unique_ptr<PartitionedCopy> partitioned_copy;

	//! Counters
	atomic<idx_t> rows_copied;
	atomic<idx_t> last_file_offset;
};

//===--------------------------------------------------------------------===//
// Copy Local State Declarations
//===--------------------------------------------------------------------===//
class PartitionedCopyLocalState;

class CopyToFileLocalState : public LocalSinkState {
public:
	CopyToFileLocalState(const PhysicalCopyToFile &op_p, ExecutionContext &context_p, CopyToFileGlobalState &gstate_p);
	~CopyToFileLocalState() override;

public:
	const PhysicalCopyToFile &op;
	ExecutionContext &context;
	CopyToFileGlobalState &gstate;

	//! Global/local file state (unpartitioned write)
	FileStateHandle global_file_state;
	unique_ptr<LocalFunctionData> local_file_state;

	//! Current append batch (unpartitioned write)
	unique_ptr<ColumnDataCollection> batch;
	ColumnDataAppendState batch_append_state;
	//! Cuts chunks so BATCH_SIZE_BYTES is reachable
	unique_ptr<CopyBatchSlicer> batch_slicer;

	//! Local state (partitioned write)
	unique_ptr<PartitionedCopyLocalState> partitioned_copy_local_state;

	//! Total number of rows copied by this thread
	idx_t total_rows_copied = 0;
};

//===--------------------------------------------------------------------===//
// Partitioned Copy Type Declarations
//===--------------------------------------------------------------------===//
enum class PartitionedCopyStage : uint8_t { PREPARE_INPUT, PLAN, BATCH, PREPARE, FLUSH, DONE };
enum class FileCreationReason : uint8_t { NORMAL, SORTED_RUN_BOUNDARY, ROTATION };

struct PartitionedCopyTask {
	PartitionedCopyStage stage = PartitionedCopyStage::DONE;
	idx_t group_idx = 0;
	//! The preparation step (PREPARE_INPUT) or the partition/batch state (PREPARE, FLUSH)
	idx_t step = 0;
	idx_t thread_idx = 0;
	//! The block range of a preparation task
	idx_t begin_idx = 0;
	idx_t end_idx = 0;
	//! The planned batch (BATCH) or the batch within a partition (PREPARE)
	idx_t batch_idx = 0;
};

struct PartitionedCopyBatch {
	PartitionedCopyBatch(const CopyFunctionBatchAnalyzer batch_analyzer_p,
	                     unique_ptr<PreparedBatchData> prepared_batch_p)
	    : batch_analyzer(batch_analyzer_p), prepared_batch(std::move(prepared_batch_p)) {
	}
	const CopyFunctionBatchAnalyzer batch_analyzer;
	unique_ptr<PreparedBatchData> prepared_batch;
};

struct PartitionWriteInfo {
	//! Serializes operations that need a complete partition writer run boundary.
	annotated_mutex lock;
	FileStateHandle file_state;
	idx_t active_writes = 0;
};

struct PartitionFileStateReservation {
	vector<FileStateHandle> files_to_finalize;
	idx_t offset = 0;
};

//===--------------------------------------------------------------------===//
// Partition Write Manager Declarations
//===--------------------------------------------------------------------===//
class PartitionWriteManager;

class PartitionWriteLease {
public:
	PartitionWriteLease() = default;
	PartitionWriteLease(PartitionWriteManager &manager_p, PartitionWriteInfo &write_info_p);
	PartitionWriteLease(PartitionWriteLease &&other) noexcept;
	PartitionWriteLease &operator=(PartitionWriteLease &&other) noexcept;
	PartitionWriteLease(const PartitionWriteLease &) = delete;
	PartitionWriteLease &operator=(const PartitionWriteLease &) = delete;
	~PartitionWriteLease();

public:
	explicit operator bool() const {
		return write_info.get();
	}

	PartitionWriteInfo &Get() const {
		D_ASSERT(write_info);
		return *write_info;
	}

	PartitionWriteInfo &operator*() const {
		return Get();
	}

	PartitionWriteInfo *operator->() const {
		return &Get();
	}

	void Reset();

private:
	optional_ptr<PartitionWriteManager> manager;
	mutable optional_ptr<PartitionWriteInfo> write_info;
};

class PartitionWriteManager {
	using ActiveWrites = vector_of_value_map_t<unique_ptr<PartitionWriteInfo>>;
	using ActiveWriteIterator = ActiveWrites::iterator;

public:
	class ReservationLock {
	public:
		ReservationLock(ReservationLock &&) noexcept = default;
		ReservationLock &operator=(ReservationLock &&) noexcept = default;
		ReservationLock(const ReservationLock &) = delete;
		ReservationLock &operator=(const ReservationLock &) = delete;

	private:
		explicit ReservationLock(annotated_mutex &lock_p) : guard(lock_p) {
		}

	private:
		annotated_unique_lock<annotated_mutex> guard;
		friend class PartitionWriteManager;
	};

public:
	PartitionWriteManager(const PhysicalCopyToFile &op_p, ClientContext &context_p) : op(op_p), context(context_p) {
	}

public:
	PartitionWriteLease Acquire(const vector<Value> &values) DUCKDB_EXCLUDES(lock);
	ReservationLock LockForReservation() DUCKDB_EXCLUDES(lock);
	PartitionFileStateReservation ReserveFileState(ReservationLock &reservation_lock, const vector<Value> &values,
	                                               FileCreationReason reason) DUCKDB_NO_THREAD_SAFETY_ANALYSIS;
	//! Claims the directory of a partition, throws if another partition already owns it
	void ClaimDirectory(ReservationLock &reservation_lock, const vector<Value> &values,
	                    const string &directory) DUCKDB_NO_THREAD_SAFETY_ANALYSIS;
	FileStateHandle TryTakeInactiveFileState(const vector<Value> &values) DUCKDB_EXCLUDES(lock);
	vector<FileStateHandle> TakeOpenFileStates() DUCKDB_EXCLUDES(lock);

private:
	FileStateHandle TakeInactiveFileStateLocked(ActiveWriteIterator entry) DUCKDB_REQUIRES(lock);
	void Release(PartitionWriteInfo &write_info) DUCKDB_EXCLUDES(lock);

private:
	const PhysicalCopyToFile &op;
	ClientContext &context;
	mutable annotated_mutex lock;
	ActiveWrites active_writes DUCKDB_GUARDED_BY(lock);
	vector_of_value_map_t<idx_t> previous_partitions DUCKDB_GUARDED_BY(lock);
	idx_t global_offset DUCKDB_GUARDED_BY(lock) = 0;
	//! The claimed directory of each partition, and the partition owning each directory
	vector_of_value_map_t<string> partition_directories DUCKDB_GUARDED_BY(lock);
	unordered_map<string, vector<Value>> directory_partitions DUCKDB_GUARDED_BY(lock);

	friend class PartitionWriteLease;
};

//===--------------------------------------------------------------------===//
// Partitioned Copy Batch State Declarations
//===--------------------------------------------------------------------===//
enum class PartitionedCopyBatchMode : uint8_t { BUFFERING, PREPARING, DELAYED, PREPARED };
enum class PartitionedCopyBatchActionType : uint8_t { STORE_COLLECTION, ACQUIRE_AND_PREPARE, PREPARE_WITH_WRITE_INFO };
enum class PartitionedCopyPrepareTaskActionType : uint8_t {
	SKIP_PARTITION,
	WAIT_FOR_WRITE_INFO,
	ACQUIRE_WRITE_INFO,
	PREPARE_BATCH
};
enum class PartitionedCopyPrepareActionType : uint8_t { ACQUIRE_WRITE_INFO, PREPARE_BATCH };
enum class PartitionedCopyFlushActionType : uint8_t { DELAYED_COLLECTIONS, PREPARED_BATCHES };

enum class PartitionedCopyCollectionSchema : uint8_t { RAW_SCHEMA, WRITE_SCHEMA };

struct PartitionedCopyCollection {
	PartitionedCopyCollection() = default;
	explicit PartitionedCopyCollection(PartitionedCopyCollectionSchema schema_p) : schema(schema_p) {
	}
	PartitionedCopyCollection(PartitionedCopyCollectionSchema schema_p, unique_ptr<ColumnDataCollection> collection_p)
	    : schema(schema_p), collection(std::move(collection_p)) {
	}

	idx_t Count() const {
		return collection ? collection->Count() : 0;
	}

	PartitionedCopyCollectionSchema schema = PartitionedCopyCollectionSchema::WRITE_SCHEMA;
	unique_ptr<ColumnDataCollection> collection;
};

struct PartitionedCopyBatchAction {
	PartitionedCopyBatchActionType type = PartitionedCopyBatchActionType::STORE_COLLECTION;
	vector<Value> values;
	optional_ptr<PartitionWriteInfo> write_info;
};

struct PartitionedCopyPrepareTaskAction {
	PartitionedCopyPrepareTaskActionType type = PartitionedCopyPrepareTaskActionType::SKIP_PARTITION;
	idx_t batch_idx = DConstants::INVALID_INDEX;
};

struct PartitionedCopyPrepareAction {
	PartitionedCopyPrepareActionType type = PartitionedCopyPrepareActionType::ACQUIRE_WRITE_INFO;
	vector<Value> values;
	optional_ptr<PartitionWriteInfo> write_info;
	PartitionedCopyCollection data;
};

struct PartitionedCopyFlushAction {
	PartitionedCopyFlushActionType type = PartitionedCopyFlushActionType::DELAYED_COLLECTIONS;
	vector<Value> values;
	vector<PartitionedCopyCollection> collections;
	vector<unique_ptr<PartitionedCopyBatch>> batches;
	PartitionWriteLease write_lease;
};

struct PartitionedCopyBatchState {
private:
	//! Partition values can be NULL, so they are compared with IS NOT DISTINCT FROM semantics
	static bool PartitionValuesEqual(const vector<Value> &left, const vector<Value> &right) {
		if (left.size() != right.size()) {
			return false;
		}
		for (idx_t i = 0; i < left.size(); i++) {
			if (!Value::NotDistinctFrom(left[i], right[i])) {
				return false;
			}
		}
		return true;
	}

	void SetValues(vector<Value> values_p) {
		D_ASSERT(!values || PartitionValuesEqual(*values, values_p));
		if (!values) {
			values = std::move(values_p);
		}
	}

	void StartPreparing() {
		D_ASSERT(mode == PartitionedCopyBatchMode::BUFFERING);
		mode = PartitionedCopyBatchMode::PREPARING;
		batches.resize(collections.size());
	}

	bool TryReserveWriteInfo() {
		D_ASSERT(mode == PartitionedCopyBatchMode::PREPARING);
		if (write_lease || write_info_requested) {
			return false;
		}
		write_info_requested = true;
		return true;
	}

	PartitionWriteInfo &SetWriteInfo(PartitionWriteLease write_lease_p) {
		D_ASSERT(mode == PartitionedCopyBatchMode::PREPARING);
		D_ASSERT(!write_lease);
		D_ASSERT(write_lease_p);
		write_lease = std::move(write_lease_p);
		write_info_requested = false;
		batches.resize(collections.size());
		return write_lease.Get();
	}

	void EnsurePreparingBatchSlots() {
		D_ASSERT(mode == PartitionedCopyBatchMode::PREPARING);
		D_ASSERT(write_lease);
		batches.resize(collections.size());
	}

	void MarkDelayed() {
		D_ASSERT(mode == PartitionedCopyBatchMode::BUFFERING);
		mode = PartitionedCopyBatchMode::DELAYED;
	}

	bool NeedsWriteInfo() const {
		return mode == PartitionedCopyBatchMode::PREPARING && !write_lease;
	}

	bool NeedsPrepare(idx_t batch_idx) const {
		D_ASSERT(mode == PartitionedCopyBatchMode::PREPARING);
		D_ASSERT(write_lease);
		D_ASSERT(batch_idx < collections.size());
		return collections[batch_idx].collection && (batch_idx >= batches.size() || !batches[batch_idx]);
	}

	bool HasWriteInfo() const {
		return write_lease.operator bool();
	}

	PartitionWriteInfo &GetWriteInfo() const {
		return write_lease.Get();
	}

	PartitionWriteLease TakeWriteLease() {
		D_ASSERT(write_lease);
		return std::move(write_lease);
	}

public:
	idx_t NextCollectionIndex() const {
		return collections.size();
	}

	const vector<Value> &Values() const {
		if (!values) {
			throw InternalException("Partitioned copy batch has no partition values");
		}
		return *values;
	}

	idx_t AddCollectionSlot(PartitionedCopyCollectionSchema schema, idx_t row_count) {
		D_ASSERT(mode == PartitionedCopyBatchMode::BUFFERING || mode == PartitionedCopyBatchMode::PREPARING);
		collections.emplace_back(schema);
		if (mode == PartitionedCopyBatchMode::PREPARING && batches.size() < collections.size()) {
			batches.emplace_back();
		}
		count += row_count;
		return collections.size() - 1;
	}

	PartitionedCopyBatchAction RegisterBatch(vector<Value> values_p, idx_t flush_threshold,
	                                         bool has_delayed_partition) {
		SetValues(std::move(values_p));
		auto result = PartitionedCopyBatchAction {PartitionedCopyBatchActionType::STORE_COLLECTION, Values(), nullptr};
		if (mode == PartitionedCopyBatchMode::BUFFERING && count >= flush_threshold && !has_delayed_partition) {
			StartPreparing();
			if (TryReserveWriteInfo()) {
				result.type = PartitionedCopyBatchActionType::ACQUIRE_AND_PREPARE;
				return result;
			}
		}

		if (mode == PartitionedCopyBatchMode::PREPARING && write_lease) {
			result.type = PartitionedCopyBatchActionType::PREPARE_WITH_WRITE_INFO;
			result.write_info = GetWriteInfo();
		}
		return result;
	}

	PartitionWriteInfo &CompleteWriteInfoAcquisition(PartitionWriteLease write_lease_p) {
		D_ASSERT(write_info_requested);
		return SetWriteInfo(std::move(write_lease_p));
	}

	void StoreCollection(idx_t batch_idx, unique_ptr<ColumnDataCollection> collection) {
		D_ASSERT(mode == PartitionedCopyBatchMode::BUFFERING || mode == PartitionedCopyBatchMode::PREPARING);
		D_ASSERT(batch_idx < collections.size());
		collections[batch_idx].collection = std::move(collection);
	}

	idx_t FinalizeBatching(idx_t flush_threshold, bool has_delayed_partition) {
		D_ASSERT(values);
		D_ASSERT(count > 0);
		if (mode == PartitionedCopyBatchMode::PREPARING) {
			D_ASSERT(HasWriteInfo());
			EnsurePreparingBatchSlots();
			return 0;
		}
		D_ASSERT(mode == PartitionedCopyBatchMode::BUFFERING);
		if (count < flush_threshold || has_delayed_partition) {
			MarkDelayed();
			return count;
		}
		StartPreparing();
		return 0;
	}

	PartitionedCopyPrepareTaskAction SelectPrepareTask(idx_t &prepare_batch_idx) {
		if (mode == PartitionedCopyBatchMode::DELAYED || mode == PartitionedCopyBatchMode::PREPARED) {
			return {PartitionedCopyPrepareTaskActionType::SKIP_PARTITION};
		}
		D_ASSERT(mode == PartitionedCopyBatchMode::PREPARING);
		if (NeedsWriteInfo()) {
			if (!write_info_requested && TryReserveWriteInfo()) {
				return {PartitionedCopyPrepareTaskActionType::ACQUIRE_WRITE_INFO};
			}
			return {PartitionedCopyPrepareTaskActionType::WAIT_FOR_WRITE_INFO};
		}
		while (prepare_batch_idx < collections.size()) {
			if (NeedsPrepare(prepare_batch_idx)) {
				return {PartitionedCopyPrepareTaskActionType::PREPARE_BATCH, prepare_batch_idx++};
			}
			++prepare_batch_idx;
		}
		return {PartitionedCopyPrepareTaskActionType::SKIP_PARTITION};
	}

	PartitionedCopyPrepareAction BeginPrepareTask(idx_t batch_idx) {
		D_ASSERT(mode == PartitionedCopyBatchMode::PREPARING);
		PartitionedCopyPrepareAction result;
		result.values = Values();
		if (batch_idx == DConstants::INVALID_INDEX) {
			D_ASSERT(NeedsWriteInfo());
			D_ASSERT(write_info_requested);
			result.type = PartitionedCopyPrepareActionType::ACQUIRE_WRITE_INFO;
			return result;
		}
		D_ASSERT(HasWriteInfo());
		D_ASSERT(batch_idx < collections.size());
		result.type = PartitionedCopyPrepareActionType::PREPARE_BATCH;
		result.write_info = GetWriteInfo();
		result.data = TakeCollection(batch_idx);
		return result;
	}

private:
	PartitionedCopyCollection TakeCollection(idx_t batch_idx) {
		D_ASSERT(mode == PartitionedCopyBatchMode::PREPARING);
		D_ASSERT(batch_idx < collections.size());
		return std::move(collections[batch_idx]);
	}

public:
	void StorePreparedBatch(idx_t batch_idx, unique_ptr<PartitionedCopyBatch> batch) {
		D_ASSERT(mode == PartitionedCopyBatchMode::PREPARING || mode == PartitionedCopyBatchMode::PREPARED);
		D_ASSERT(batch_idx < batches.size());
		batches[batch_idx] = std::move(batch);
	}

	void MarkPrepared() {
		if (mode == PartitionedCopyBatchMode::PREPARING) {
			mode = PartitionedCopyBatchMode::PREPARED;
		}
	}

	bool ReadyForFlush() const {
		if (mode == PartitionedCopyBatchMode::DELAYED) {
			return true;
		}
		if (mode != PartitionedCopyBatchMode::PREPARING && mode != PartitionedCopyBatchMode::PREPARED) {
			return false;
		}
		if (!HasWriteInfo() || batches.size() < collections.size()) {
			return false;
		}
		for (idx_t batch_idx = 0; batch_idx < collections.size(); batch_idx++) {
			if (!batches[batch_idx]) {
				return false;
			}
		}
		return true;
	}

	PartitionedCopyFlushAction TakeFlushAction() {
		PartitionedCopyFlushAction result;
		result.values = Values();
		if (mode == PartitionedCopyBatchMode::DELAYED) {
			result.type = PartitionedCopyFlushActionType::DELAYED_COLLECTIONS;
			result.collections = TakeDelayedCollections();
			return result;
		}
		D_ASSERT(mode == PartitionedCopyBatchMode::PREPARED);
		result.type = PartitionedCopyFlushActionType::PREPARED_BATCHES;
		result.write_lease = TakeWriteLease();
		result.batches = TakePreparedBatches();
		return result;
	}

private:
	vector<PartitionedCopyCollection> TakeDelayedCollections() {
		D_ASSERT(mode == PartitionedCopyBatchMode::DELAYED);
		return std::move(collections);
	}

	vector<unique_ptr<PartitionedCopyBatch>> TakePreparedBatches() {
		D_ASSERT(mode == PartitionedCopyBatchMode::PREPARED);
		return std::move(batches);
	}

private:
	optional<vector<Value>> values;
	PartitionWriteLease write_lease;
	vector<PartitionedCopyCollection> collections;
	vector<unique_ptr<PartitionedCopyBatch>> batches;
	idx_t count = 0;
	bool write_info_requested = false;
	PartitionedCopyBatchMode mode = PartitionedCopyBatchMode::BUFFERING;
};

//===--------------------------------------------------------------------===//
// Delayed Partition Buffering Declarations
//===--------------------------------------------------------------------===//
struct DelayedPartitionFlush {
	vector<Value> values;
	PartitionedCopyCollection data;
};

struct DelayedPartitionState {
	bool HasBufferedData() const {
		return buffer.Count() > 0;
	}

	bool InFlight() const {
		return in_flight > 0;
	}

	void Buffer(PartitionedCopyCollection data) {
		if (!HasBufferedData()) {
			buffer = std::move(data);
			return;
		}
		D_ASSERT(buffer.schema == data.schema);
		buffer.collection->Combine(*data.collection);
	}

	bool Ready(idx_t flush_threshold, bool force) const {
		return HasBufferedData() && !InFlight() && (force || buffer.Count() >= flush_threshold);
	}

	DelayedPartitionFlush Take(const vector<Value> &values) {
		D_ASSERT(HasBufferedData());
		D_ASSERT(!InFlight());
		DelayedPartitionFlush result;
		result.values = values;
		result.data = std::move(buffer);
		in_flight++;
		return result;
	}

	void Complete() {
		D_ASSERT(in_flight > 0);
		in_flight--;
	}

	bool Active() const {
		return HasBufferedData() || InFlight();
	}

	PartitionedCopyCollection buffer;
	idx_t in_flight = 0;
};

class DelayedPartitionBuffers {
public:
	bool Has(const vector<Value> &values) DUCKDB_EXCLUDES(lock) {
		annotated_lock_guard<annotated_mutex> guard(lock);
		return partitions.find(values) != partitions.end();
	}

	bool Empty() const DUCKDB_EXCLUDES(lock) {
		annotated_lock_guard<annotated_mutex> guard(lock);
		return partitions.empty();
	}

	optional<DelayedPartitionFlush> BufferOrTakeReady(const vector<Value> &values, PartitionedCopyCollection data,
	                                                  const idx_t flush_threshold, const bool force = false)
	    DUCKDB_EXCLUDES(lock) {
		if (!data.collection || data.collection->Count() == 0) {
			return nullopt;
		}

		DelayedPartitionFlush result;
		bool has_result = false;
		{
			annotated_lock_guard<annotated_mutex> guard(lock);
			auto &state = partitions[values];
			state.Buffer(std::move(data));
			if (state.Ready(flush_threshold, force)) {
				result = state.Take(values);
				has_result = true;
			}
			EraseIfInactive(values, state);
		}

		if (!has_result) {
			return nullopt;
		}
		return std::move(result);
	}

	optional<DelayedPartitionFlush> TakeNext() DUCKDB_EXCLUDES(lock) {
		DelayedPartitionFlush result;
		{
			annotated_lock_guard<annotated_mutex> guard(lock);
			auto next = partitions.end();
			for (auto entry = partitions.begin(); entry != partitions.end(); entry++) {
				auto &state = entry->second;
				if (!state.HasBufferedData() || state.InFlight()) {
					continue;
				}
				if (next == partitions.end() || VectorOfValuesLess()(entry->first, next->first)) {
					next = entry;
				}
			}
			if (next != partitions.end()) {
				result = next->second.Take(next->first);
				return std::move(result);
			}
		}
		return nullopt;
	}

	optional<DelayedPartitionFlush> Complete(const vector<Value> &values, const idx_t flush_threshold,
	                                         const bool take_ready) DUCKDB_EXCLUDES(lock) {
		DelayedPartitionFlush result;
		bool has_result = false;
		annotated_lock_guard<annotated_mutex> guard(lock);
		auto entry = partitions.find(values);
		D_ASSERT(entry != partitions.end());
		if (entry == partitions.end()) {
			return nullopt;
		}
		entry->second.Complete();
		if (take_ready && entry->second.Ready(flush_threshold, false)) {
			// Data for the same partition reached the threshold while an earlier flush was in-flight.
			// Hand it back to the caller immediately so the partition does not wait for a global drain.
			result = entry->second.Take(values);
			has_result = true;
		} else if (!entry->second.Active()) {
			partitions.erase(entry);
		}
		if (!has_result) {
			return nullopt;
		}
		return std::move(result);
	}

private:
	void EraseIfInactive(const vector<Value> &values, const DelayedPartitionState &state) DUCKDB_REQUIRES(lock) {
		if (!state.Active()) {
			partitions.erase(values);
		}
	}

	mutable annotated_mutex lock;
	vector_of_value_map_t<DelayedPartitionState> partitions DUCKDB_GUARDED_BY(lock);
};

//===--------------------------------------------------------------------===//
// Partitioned Copy Declarations
//===--------------------------------------------------------------------===//
//! The position of a chunk within the storage of a hash group
struct PartitionedCopyChunkPosition {
	idx_t segment_idx = 0;
	idx_t chunk_idx = 0;
	//! The number of rows before the chunk
	idx_t row_offset = 0;
};

//! A row position within a hash group: the chunk and the row offset within it
struct PartitionedCopyRowPosition {
	PartitionedCopyChunkPosition chunk;
	idx_t row_idx = 0;
};

//! A planned output batch: row_count rows of a hash group from begin on, all of one partition
struct PartitionedCopyPlannedBatch {
	idx_t batch_state_idx = 0;
	idx_t batch_idx = 0;
	PartitionedCopyRowPosition begin;
	idx_t row_count = 0;
};

//! Scan state over the rows of a hash group
class PartitionedCopyGroupScanState {
public:
	virtual ~PartitionedCopyGroupScanState() = default;

public:
	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}
	template <class TARGET>
	const TARGET &Cast() const {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<const TARGET &>(*this);
	}
};

//! Walks the chunks of a hash group in storage order while its batches are planned
class PartitionedCopyGroupCursor {
public:
	virtual ~PartitionedCopyGroupCursor() = default;

public:
	//! Moves to the next chunk; returns false once past the last chunk
	virtual bool Next() = 0;
	//! The position of the current chunk
	virtual PartitionedCopyChunkPosition Position() const = 0;
	//! The number of rows in the current chunk
	virtual idx_t Count() const = 0;
	//! An estimate of the bytes the written columns of the current chunk take
	virtual idx_t ByteEstimate() const = 0;
	//! The row offsets within the current chunk at which a new partition key starts (none in a single-key group)
	virtual void PartitionBoundaries(vector<idx_t> &boundaries) const = 0;
	//! The byte size of every written row of the current chunk (may pin and read the chunk)
	virtual void RowSizes(vector<idx_t> &sizes) = 0;
};

//! The rows of one hash group: a hash bin of the sorted input, or one partition of the direct input. A group may
//! need preparation steps (sorting, materializing, finding partition boundaries) before its chunks can be read. The
//! group owns its rows: it is destroyed once every batch has been assembled.
class PartitionedCopyGroup {
public:
	virtual ~PartitionedCopyGroup() = default;

public:
	//! The number of rows in the group
	virtual idx_t Count() const = 0;
	//! Preparation steps that run before the rows can be read; every step is split over block ranges
	virtual idx_t PreparationStepCount() const = 0;
	virtual void ExecutePreparation(ExecutionContext &context, InterruptState &interrupt, idx_t step, idx_t begin_block,
	                                idx_t end_block) = 0;
	//! Called once all block ranges of a step have been executed
	virtual void FinishPreparation(idx_t step) = 0;

	//! Chunk access, valid after the preparation steps: a cursor over the chunks, and scans from a chunk on
	virtual unique_ptr<PartitionedCopyGroupCursor> InitializeCursor(const CopyBatchSlicer &slicer) const = 0;
	virtual unique_ptr<PartitionedCopyGroupScanState>
	InitializeScan(const PartitionedCopyChunkPosition &position) const = 0;
	virtual void InitializeScanChunk(DataChunk &chunk) const = 0;
	virtual bool Scan(PartitionedCopyGroupScanState &state, DataChunk &chunk) const = 0;
};

class PartitionedCopyInputGlobalState {
public:
	virtual ~PartitionedCopyInputGlobalState() = default;

public:
	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}
	template <class TARGET>
	const TARGET &Cast() const {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<const TARGET &>(*this);
	}
};

class PartitionedCopyInputLocalState {
public:
	virtual ~PartitionedCopyInputLocalState() = default;

public:
	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}
	template <class TARGET>
	const TARGET &Cast() const {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<const TARGET &>(*this);
	}
};

//! The partition input of a COPY: buffers the rows of one flush generation and hands them out as groups
class PartitionedCopyInput {
public:
	virtual ~PartitionedCopyInput() = default;

public:
	virtual unique_ptr<PartitionedCopyInputGlobalState> GetGlobalState(ClientContext &context) const = 0;
	virtual unique_ptr<PartitionedCopyInputLocalState> GetLocalState(ExecutionContext &context,
	                                                                 PartitionedCopyInputGlobalState &gstate,
	                                                                 ParallelHyperLogLogLocalState &hll) const = 0;
	virtual void Sink(ExecutionContext &context, DataChunk &chunk, PartitionedCopyInputGlobalState &gstate,
	                  PartitionedCopyInputLocalState &lstate, InterruptState &interrupt) const = 0;
	virtual void Combine(ExecutionContext &context, PartitionedCopyInputGlobalState &gstate,
	                     PartitionedCopyInputLocalState &lstate, InterruptState &interrupt) const = 0;
	//! Prepares the combined rows and returns the groups (entries may be null or empty)
	virtual vector<unique_ptr<PartitionedCopyGroup>>
	Finalize(ClientContext &context, PartitionedCopyInputGlobalState &gstate, InterruptState &interrupt) const = 0;
	//! Sorts a collection by the partition and order columns (only needed with ORDER BY)
	virtual unique_ptr<ColumnDataCollection> SortCollection(ExecutionContext &context, InterruptState &interrupt,
	                                                        unique_ptr<ColumnDataCollection> collection) const = 0;
};

//! Manages a single partitioned COPY hash group
class PartitionedCopyHashGroup {
public:
	PartitionedCopyHashGroup(PartitionedCopy &partitioned_copy, unique_ptr<PartitionedCopyGroup> group,
	                         idx_t group_idx);

public:
	template <typename T>
	static T BinValue(T n, T val) {
		return (n + (val - 1)) / val;
	}

	PartitionedCopyStage GetStage() const;
	idx_t InitTasks(idx_t per_thread_p);

	bool TryPrepareNextStage() DUCKDB_REQUIRES(lock);
	optional<PartitionedCopyTask> TryNextTask() DUCKDB_REQUIRES(lock);
	optional<PartitionedCopyTask> TryNextPrepareTask() DUCKDB_REQUIRES(lock);
	optional<PartitionedCopyTask> TryNextFlushTask() DUCKDB_REQUIRES(lock);
	bool PreparedBatchesReady() const DUCKDB_REQUIRES(lock);

	void PrepareInput(ExecutionContext &context, InterruptState &interrupt, const PartitionedCopyTask &task);
	void Plan();
	void Batch(const PartitionedCopyTask &task);
	void Prepare(ExecutionContext &execution_context, InterruptState &interrupt_state, const PartitionedCopyTask &task);
	void Flush(ExecutionContext &execution_context, InterruptState &interrupt_state, const PartitionedCopyTask &task);
	void PrepareBatchStates() DUCKDB_REQUIRES(lock);
	void CompletePreparedBatchStates() DUCKDB_REQUIRES(lock);

private:
	idx_t NewBatchState() DUCKDB_EXCLUDES(lock);
	void AddPlannedBatch(idx_t batch_state_idx, PartitionedCopyRowPosition begin, idx_t row_count)
	    DUCKDB_EXCLUDES(lock);
	idx_t StoreBatch(idx_t batch_state_idx, idx_t batch_idx, unique_ptr<ColumnDataCollection> batch,
	                 vector<Value> values, idx_t row_count) DUCKDB_EXCLUDES(lock);

public:
	//! The PartitionedCopy that this hash group belongs to
	PartitionedCopy &partitioned_copy;
	//! The rows of the group
	unique_ptr<PartitionedCopyGroup> group;

	//! The row count of the group
	const idx_t count;
	//! The number of blocks (chunks) in the group
	const idx_t blocks;
	//! The bin number
	const idx_t group_idx;

	//! Lock for stage transitions and task assignment
	mutable annotated_mutex lock;
	//! The processing stage for this group
	atomic<PartitionedCopyStage> stage;

	//! The preparation step being executed, its next task and the number of blocks finished for it
	idx_t preparation_step DUCKDB_GUARDED_BY(lock) = 0;
	idx_t next_task DUCKDB_GUARDED_BY(lock) = 0;
	//! The number of blocks per preparation task and the number of tasks per step
	idx_t per_thread = 0;
	idx_t group_threads = 0;
	atomic<idx_t> prepared_blocks;

	//! Whether the planning task has been handed out, and whether it has finished
	bool plan_started DUCKDB_GUARDED_BY(lock) = false;
	atomic<bool> planned;
	//! The planned batches and the next one to assemble
	vector<PartitionedCopyPlannedBatch> planned_batches DUCKDB_GUARDED_BY(lock);
	idx_t next_batch DUCKDB_GUARDED_BY(lock) = 0;
	//! The batched data, one state per partition (in order)
	vector<unique_ptr<PartitionedCopyBatchState>> batch_states DUCKDB_GUARDED_BY(lock);
	//! Count of batched rows
	atomic<idx_t> batched;
	//! Count of rows that either have a prepared batch or do not need one
	atomic<idx_t> prepared;

	//! The current partition/batch index for preparing
	idx_t prepare_partition_idx DUCKDB_GUARDED_BY(lock) = 0;
	idx_t prepare_batch_idx DUCKDB_GUARDED_BY(lock) = 0;

	//! The current partition index for flushing
	idx_t flush_partition_idx DUCKDB_GUARDED_BY(lock) = 0;
	//! Count of flushed partitions
	atomic<idx_t> flushed;
};

enum class PartitionedCopyInputState : uint8_t { OPEN, CLOSED };

//! Manages a partitioned COPY flush
class PartitionedCopyState {
public:
	using PartitionBlock = std::pair<idx_t, idx_t>;

public:
	PartitionedCopyState(PartitionedCopy &partitioned_copy, unique_ptr<PartitionedCopyInputGlobalState> input_state);

public:
	bool CanSink() const {
		return input_state_flag.load(std::memory_order_relaxed) == PartitionedCopyInputState::OPEN;
	}

	void CloseInput() {
		input_state_flag = PartitionedCopyInputState::CLOSED;
	}

	bool ShouldInitiateFlush(const idx_t &local_append_count);
	bool IsCombineComplete() const DUCKDB_REQUIRES(lock);
	void CreateTaskList(vector<unique_ptr<PartitionedCopyGroup>> groups) DUCKDB_REQUIRES(lock);
	bool HasCompleted() const;
	optional<PartitionedCopyTask> TryAssignTask();
	void ExecuteTask(ExecutionContext &execution_context, const PartitionedCopyTask &task, InterruptState &interrupt);
	vector<vector<Value>> FinishTask(const PartitionedCopyTask &task);

public:
	//! The PartitionedCopy that this state belongs to
	PartitionedCopy &partitioned_copy;

	//! Lock for managing this state
	mutable annotated_mutex lock;

	//! To estimate number of partitions
	ParallelHyperLogLogGlobalState hll;
	//! Check if we should flush once a thread's append count exceeds this value
	atomic<idx_t> next_flush_check;
	//! Closed states accept no more input, including while waiting for the previous flush
	atomic<PartitionedCopyInputState> input_state_flag {PartitionedCopyInputState::OPEN};

	//! The input's buffered rows
	unique_ptr<PartitionedCopyInputGlobalState> input_state;
	//! Whether the input has been finalized into hash groups
	bool groups_ready DUCKDB_GUARDED_BY(lock) = false;
	//! The error of a failed finalization: it has taken the rows out of the input, so it is not retried
	std::exception_ptr finalize_error DUCKDB_GUARDED_BY(lock);
	//! Per-hash-bin processing groups (populated by CreateTaskList)
	vector<unique_ptr<PartitionedCopyHashGroup>> hash_groups;
	//! The total number of blocks to process;
	idx_t total_blocks;
	//! The sorted list of (blocks, group_idx) pairs
	vector<PartitionBlock> partition_blocks;
	//! The ordered set of active groups
	vector<idx_t> active_groups DUCKDB_GUARDED_BY(lock);
	//! The number of started tasks
	idx_t next_group DUCKDB_GUARDED_BY(lock);
	//! The number of local states
	idx_t locals DUCKDB_GUARDED_BY(lock);
	//! The number of combined local states
	idx_t combined DUCKDB_GUARDED_BY(lock);
};

enum class PartitionedCopyCombineType : uint8_t { DURING_SINK, DURING_PIPELINE_COMBINE };

//! Manages partitioned COPY
class PartitionedCopy : public StateWithBlockableTasks {
public:
	PartitionedCopy(const PhysicalCopyToFile &op, ClientContext &context, CopyToFileGlobalState &copy_gstate);

public:
	//! PhysicalOperator-like interface
	SinkResultType Sink(ExecutionContext &execution_context, DataChunk &chunk, PartitionedCopyLocalState &lstate,
	                    InterruptState &interrupt_state);
	void Combine(ExecutionContext &execution_context, PartitionedCopyLocalState &lstate,
	             InterruptState &interrupt_state, PartitionedCopyCombineType combine_type);
	SinkCombineResultType FinishSink(ExecutionContext &execution_context, PartitionedCopyLocalState &lstate,
	                                 InterruptState &interrupt_state);
	void Finalize(Pipeline &pipeline, Event &event, InterruptState &interrupt_state);
	void Flush(ExecutionContext &execution_context, InterruptState &interrupt_state);
	void NotifyFlushProgress();

public:
	//! Partitioning-specific functions
	void InitializeFlush() DUCKDB_REQUIRES(lock);
	void FinalizeState(PartitionedCopyState &state, InterruptState &interrupt_state) DUCKDB_REQUIRES(state.lock);

	bool HasDelayedPartition(const vector<Value> &values);
	bool HasDelayedPartitions() const;
	optional<DelayedPartitionFlush> BufferOrTakeReadyPartition(const vector<Value> &values,
	                                                           PartitionedCopyCollection data, bool force = false);
	optional<DelayedPartitionFlush> TakeNextDelayedPartition();
	optional<DelayedPartitionFlush> CompleteDelayedPartition(const vector<Value> &values, bool take_ready);
	bool DrainDelayedPartitions(ExecutionContext &execution_context, InterruptState &interrupt_state);
	unique_ptr<ColumnDataCollection> SortPartitionCollection(ExecutionContext &execution_context,
	                                                         InterruptState &interrupt_state,
	                                                         unique_ptr<ColumnDataCollection> collection);
	PartitionedCopyCollectionSchema GetPartitionCollectionSchema() const;
	const vector<LogicalType> &GetPartitionCollectionTypes(PartitionedCopyCollectionSchema schema) const;
	unique_ptr<ColumnDataCollection> PrepareCollectionForWrite(PartitionedCopyCollection data);
	unique_ptr<ColumnDataCollection> ProjectToWriteColumns(unique_ptr<ColumnDataCollection> collection);
	unique_ptr<PartitionedCopyBatch> PreparePartitionBatch(const vector<Value> &values, PartitionWriteInfo &write_info,
	                                                       PartitionedCopyCollection data);
	void FlushPreparedPartitionRun(const vector<Value> &values, PartitionWriteInfo &write_info,
	                               vector<unique_ptr<PartitionedCopyBatch>> batches);
	void FlushPreparedPartitionBatch(const vector<Value> &values, PartitionWriteInfo &write_info,
	                                 unique_ptr<PartitionedCopyBatch> batch);
	void FlushPartitionCollection(ExecutionContext &execution_context, InterruptState &interrupt_state,
	                              DelayedPartitionFlush flush);
	void RequestPartitionFileState(FileStateHandle &file_state, const vector<Value> &values,
	                               FileCreationReason reason = FileCreationReason::NORMAL)
	    DUCKDB_EXCLUDES(copy_gstate.lock);
	void TryFinalizePartitionWrite(const vector<Value> &values) DUCKDB_EXCLUDES(copy_gstate.lock);
	void FinalizeActiveWrites() DUCKDB_EXCLUDES(copy_gstate.lock);
	void FinalizeFileStates(vector<FileStateHandle> files_to_finalize) DUCKDB_EXCLUDES(copy_gstate.lock);

private:
	unique_ptr<const PartitionedCopyInput> ConstructInput() const;
	template <class FUNC>
	bool TryBlockOnFlush(ExecutionContext &execution_context, InterruptState &interrupt_state, idx_t observed_progress,
	                     FUNC &&can_continue) DUCKDB_EXCLUDES(lock);
	bool TryPrepareSink(ExecutionContext &execution_context, PartitionedCopyLocalState &lstate,
	                    InterruptState &interrupt_state);
	bool RequiresSerializedPartitionWrites() const;
	void EnsureFreshPartitionFileForSortedRun(PartitionWriteInfo &write_info, const vector<Value> &values)
	    DUCKDB_EXCLUDES(copy_gstate.lock);
	void EnsureFreshPartitionFileForRotation(PartitionWriteInfo &write_info, const vector<Value> &values)
	    DUCKDB_EXCLUDES(copy_gstate.lock);
	//! Swaps write_info.file_state after temporarily dropping copy_gstate.lock to request the replacement file.
	//! Callers that can reach the swap path must serialize the full partition writer run for this write_info.
	void EnsureFreshPartitionFile(PartitionWriteInfo &write_info, const vector<Value> &values,
	                              FileCreationReason reason) DUCKDB_EXCLUDES(copy_gstate.lock);
	template <class FUNC>
	void WithSerializedPartitionWriteRun(PartitionWriteInfo &write_info, FUNC &&func) {
		annotated_unique_lock<annotated_mutex> run_guard(write_info.lock, std::defer_lock);
		if (RequiresSerializedPartitionWrites()) {
			run_guard.lock();
		}
		func();
	}
	void FlushDelayedPartitionRun(const vector<Value> &values, PartitionWriteInfo &write_info,
	                              ColumnDataCollection &collection);

public:
	const PhysicalCopyToFile &op;
	ClientContext &context;
	CopyToFileGlobalState &copy_gstate;
	PartitionWriteManager partition_writes;

	//! Which columns/types to write to the file
	vector<column_t> write_columns;
	vector<LogicalType> write_types;
	vector<column_t> raw_columns;

	//! Expression describing the directories of a partition's files relative to the COPY target:
	//! PARTITION_PATH, or the hive layout by default. useful if partitions are registered in
	//! locations that do not follow hive partition naming structures
	unique_ptr<Expression> partition_path;

	//! The partition input buffering the rows of a flush generation
	const unique_ptr<const PartitionedCopyInput> input;

	//! Whether a flushing state currently exists
	atomic<bool> flushing;
	//! Progress used to register blocked producers without missing a wakeup
	idx_t flush_progress DUCKDB_GUARDED_BY(lock) = 0;
	//! How many threads are active
	atomic<idx_t> locals;
	//! Whether Finalize has been called
	atomic<bool> finalized;

	//! Current sink and combine states
	shared_ptr<PartitionedCopyState> sinking_state DUCKDB_GUARDED_BY(lock);
	shared_ptr<PartitionedCopyState> flushing_state DUCKDB_GUARDED_BY(lock);

	//! Delayed below-threshold partitions
	DelayedPartitionBuffers delayed_partition_buffers;
};

//===--------------------------------------------------------------------===//
// Partition File Request Builder Declarations
//===--------------------------------------------------------------------===//
class PartitionFileRequestBuilder {
public:
	PartitionFileRequestBuilder(PartitionedCopy &partitioned_copy_p, FileStateHandle &file_state_p,
	                            const vector<Value> &values_p, FileCreationReason reason_p)
	    : partitioned_copy(partitioned_copy_p), file_state(file_state_p), values(values_p), reason(reason_p) {
	}

public:
	optional<PartitionFileRequest> Build();
	vector<FileStateHandle> TakeFilesToFinalize();

private:
	PartitionDirectory BuildDirectory(string path) const;

private:
	PartitionedCopy &partitioned_copy;
	FileStateHandle &file_state;
	const vector<Value> &values;
	FileCreationReason reason;
	PartitionFileStateReservation reservation;
};

//===--------------------------------------------------------------------===//
// Partitioned Copy Local State Declarations
//===--------------------------------------------------------------------===//
class PartitionedCopyLocalState : public LocalSinkState {
public:
	shared_ptr<PartitionedCopyState> current_state;
	unique_ptr<PartitionedCopyInputLocalState> input_local_state;
	idx_t append_count = 0;
};

//===--------------------------------------------------------------------===//
// Partitioned Copy Scoped Guard Declarations
//===--------------------------------------------------------------------===//
class DelayedPartitionFlushGuard {
public:
	DelayedPartitionFlushGuard(PartitionedCopy &partitioned_copy_p, const vector<Value> &values_p)
	    : partitioned_copy(partitioned_copy_p), values(values_p) {
	}

	~DelayedPartitionFlushGuard();
	optional<DelayedPartitionFlush> Complete();

private:
	PartitionedCopy &partitioned_copy;
	const vector<Value> &values;
	bool active = true;
};

//===--------------------------------------------------------------------===//
// Implementations
//===--------------------------------------------------------------------===//

//===--------------------------------------------------------------------===//
// Utility Helpers
//===--------------------------------------------------------------------===//
static bool UsePerPartitionFileOffsets(const PhysicalCopyToFile &op) {
	// every partition has a directory of its own, unless all files are written to the COPY target
	return op.hive_file_pattern || op.partition_path_expression;
}

//! The hive layout, e.g. year=2024/month=1, as an expression over the partition values
static unique_ptr<Expression> CreateHivePartitionPath(ClientContext &context, const PhysicalCopyToFile &op) {
	if (op.partition_columns.empty()) {
		return nullptr;
	}
	auto component_function = HivePartitionComponentFun::GetFunction();

	FunctionBinder function_binder(context);
	vector<unique_ptr<Expression>> components;
	for (idx_t i = 0; i < op.partition_columns.size(); i++) {
		auto col_idx = op.partition_columns[i];
		vector<unique_ptr<Expression>> children;
		children.push_back(make_uniq<BoundConstantExpression>(Value(op.names[col_idx].GetIdentifierName())));
		children.push_back(make_uniq<BoundReferenceExpression>(op.expected_types[col_idx], i));
		components.push_back(function_binder.BindScalarFunction(component_function, std::move(children)));
	}
	return function_binder.BindScalarFunction(PathJoinFun::GetFunction(), std::move(components));
}

//! e.g. "year=2024, month=NULL"
static string PartitionValuesToString(const PhysicalCopyToFile &op, const vector<Value> &values) {
	string result;
	for (idx_t i = 0; i < values.size(); i++) {
		result += i > 0 ? ", " : "";
		result += op.names[op.partition_columns[i]].GetIdentifierName() + "=" + values[i].ToString();
	}
	return result;
}

static string EvaluatePartitionPath(ClientContext &context, const PhysicalCopyToFile &op,
                                    const Expression &partition_path, const vector<Value> &values) {
	D_ASSERT(values.size() == op.partition_columns.size());
	vector<LogicalType> types;
	for (auto &col_idx : op.partition_columns) {
		types.push_back(op.expected_types[col_idx]);
	}
	DataChunk partition_values;
	partition_values.Initialize(Allocator::Get(context), types, 1);
	for (idx_t i = 0; i < values.size(); i++) {
		partition_values.data[i].Append(values[i]);
	}
	partition_values.CheckCardinality(1);

	ExpressionExecutor executor(context, partition_path);
	Vector result(LogicalType::VARCHAR, 1);
	executor.ExecuteExpression(partition_values, result);
	auto path = result.GetValue(0);
	if (path.IsNull()) {
		throw InvalidInputException("PARTITION_PATH evaluated to NULL for partition %s",
		                            PartitionValuesToString(op, values));
	}
	return StringValue::Get(path);
}

void CheckDirectory(FileSystem &fs, const string &file_path, CopyOverwriteMode overwrite_mode) {
	if (overwrite_mode == CopyOverwriteMode::COPY_OVERWRITE_OR_IGNORE ||
	    overwrite_mode == CopyOverwriteMode::COPY_APPEND) {
		// with overwrite or ignore we fully ignore the presence of any files instead of erasing them
		return;
	}
	vector<string> file_list;
	vector<string> directory_list;
	directory_list.push_back(file_path);
	for (idx_t dir_idx = 0; dir_idx < directory_list.size(); dir_idx++) {
		auto directory = directory_list[dir_idx];
		fs.ListFiles(directory, [&](const string &path, bool is_directory) {
			auto full_path = fs.JoinPath(directory, path);
			if (is_directory) {
				directory_list.emplace_back(std::move(full_path));
			} else {
				file_list.emplace_back(std::move(full_path));
			}
		});
	}
	if (file_list.empty()) {
		return;
	}
	if (overwrite_mode == CopyOverwriteMode::COPY_OVERWRITE) {
		fs.RemoveFiles(file_list);
	} else {
		throw IOException("Directory \"%s\" is not empty! Enable OVERWRITE option to overwrite files", file_path);
	}
}

struct PhysicalCopyToFileColumnStatsMapData {
	vector<Value> keys;
	vector<Value> values;
};

static PhysicalCopyToFileColumnStatsMapData
CreateColumnStatistics(const case_insensitive_map_t<case_insensitive_map_t<Value>> &column_statistics) {
	PhysicalCopyToFileColumnStatsMapData result;

	//! Use a map to make sure the result has a consistent ordering
	map<string, Value> stats;
	for (auto &entry : column_statistics) {
		map<string, Value> per_column_stats;
		for (auto &stats_entry : entry.second) {
			per_column_stats.emplace(stats_entry.first, stats_entry.second);
		}
		vector<Value> stats_keys;
		vector<Value> stats_values;
		for (auto &stats_entry : per_column_stats) {
			stats_keys.emplace_back(stats_entry.first);
			stats_values.emplace_back(std::move(stats_entry.second));
		}
		auto map_value =
		    Value::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR, std::move(stats_keys), std::move(stats_values));
		stats.emplace(entry.first, std::move(map_value));
	}
	for (auto &entry : stats) {
		result.keys.emplace_back(entry.first);
		result.values.emplace_back(std::move(entry.second));
	}
	return result;
}

//===--------------------------------------------------------------------===//
// Copy File Lifecycle
//===--------------------------------------------------------------------===//
static void FinalizeLifecycleFileState(ClientContext &context, copy_to_finalize_t finalize, FunctionData &bind_data,
                                       unique_ptr<GlobalFileState> state) {
	if (!finalize) {
		throw InternalException("COPY file lifecycle finalize requires a finalize callback");
	}
	if (!state || !state->data) {
		throw InternalException("COPY file lifecycle finalize reached an empty file state");
	}
	finalize(context, bind_data, *state->data);
}
void CopyFileLifecycleExecutor::WaitForJob(CopyFileLifecycleJob &job, CopyFileLifecycleWaitMode mode) {
	while (!job.IsFinished()) {
		if (mode == CopyFileLifecycleWaitMode::INTERRUPTIBLE) {
			context.InterruptCheck();
		}
		WorkOnTaskOrYield();
	}
	job.Rethrow();
}

void CopyFileLifecycleExecutor::WaitAll(CopyFileLifecycleWaitMode mode) {
	if (mode == CopyFileLifecycleWaitMode::DRAIN) {
		executor.WorkOnTasks();
		ThrowError();
		return;
	}
	while (pending_tasks.load(std::memory_order_acquire) > 0) {
		context.InterruptCheck();
		WorkOnTaskOrYield();
	}
	ThrowError();
}

void CopyFileLifecycleExecutor::FinishTask() {
	--pending_tasks;
}

void CopyFileLifecycleExecutor::PushError(const std::exception_ptr &error_p) {
	lock_guard<mutex> guard(error_lock);
	if (!error) {
		error = error_p;
	}
}

std::exception_ptr CopyFileLifecycleExecutor::GetError() {
	lock_guard<mutex> guard(error_lock);
	return error;
}

bool CopyFileLifecycleExecutor::WorkOnTask(bool throw_error) {
	shared_ptr<Task> task;
	if (!executor.GetTask(task)) {
		return false;
	}
#ifdef D_ASSERT_IS_ENABLED
	const auto result = task->Execute(TaskExecutionMode::PROCESS_ALL);
	D_ASSERT(result != TaskExecutionResult::TASK_BLOCKED);
#else
	task->Execute(TaskExecutionMode::PROCESS_ALL);
#endif
	task.reset();
	if (throw_error) {
		ThrowError();
	}
	return true;
}

void CopyFileLifecycleExecutor::WorkOnTaskOrYield() {
	if (!WorkOnTask()) {
		TaskScheduler::YieldThread();
	}
}

void CopyFileLifecycleExecutor::WaitForTaskSlot(CopyFileLifecycleWaitMode mode) {
	while (pending_tasks >= max_pending_tasks) {
		if (mode == CopyFileLifecycleWaitMode::INTERRUPTIBLE) {
			context.InterruptCheck();
		}
		if (async_threads == 0 || mode == CopyFileLifecycleWaitMode::DRAIN) {
			WorkOnTaskOrYield();
		} else {
			TaskScheduler::YieldThread();
		}
	}
}

void CopyFileLifecycleExecutor::ThrowError() {
	lock_guard<mutex> guard(error_lock);
	if (error) {
		std::rethrow_exception(error);
	}
}

//===--------------------------------------------------------------------===//
// Copy File State Helpers
//===--------------------------------------------------------------------===//
bool CopyDirectoryManager::EnsureDirectory(FileSystem &fs, const string &dir_path) {
#ifdef D_ASSERT_IS_ENABLED
	bool created_entry = false;
#endif
	{
		std::unique_lock<mutex> guard(lock);
		while (true) {
			auto entry = directories.find(dir_path);
			if (entry == directories.end()) {
				directories.emplace(dir_path, DirectoryEntry());
#ifdef D_ASSERT_IS_ENABLED
				created_entry = true;
#endif
				break;
			}

			if (entry->second.state == CopyDirectoryState::COMPLETE) {
				return entry->second.created;
			}
			if (entry->second.state == CopyDirectoryState::FAILED) {
				std::rethrow_exception(entry->second.error);
			}
			condition.wait(guard);
		}
	}

	std::exception_ptr error;
	bool created = false;
	try {
		created = fs.CreateDirectoryExtended(dir_path, {CreateDirectoryMode::SINGLE});
	} catch (...) {
		error = std::current_exception();
	}

	{
		lock_guard<mutex> guard(lock);
		auto entry = directories.find(dir_path);
		D_ASSERT(entry != directories.end());
#ifdef D_ASSERT_IS_ENABLED
		D_ASSERT(created_entry);
#endif
		entry->second.state = error ? CopyDirectoryState::FAILED : CopyDirectoryState::COMPLETE;
		entry->second.error = error;
		entry->second.created = created;
	}
	condition.notify_all();

	if (error) {
		std::rethrow_exception(error);
	}
	return created;
}

PendingFileState CopyOutputFileRegistry::ReserveFile(string output_path,
                                                     optional_ptr<const vector<Value>> partition_values) {
	PendingFileState result;
	result.output_path = std::move(output_path);

	if (GetCopyFunctionBaseReturnType(op.return_type) != CopyFunctionReturnType::CHANGED_ROWS) {
		result.written_file_info = AddFile(result.output_path);
	}

	if (result.written_file_info && !op.partition_columns.empty()) {
		D_ASSERT(partition_values);
		vector<Value> partition_keys;
		vector<Value> partition_values_as_varchar;
		for (idx_t i = 0; i < op.partition_columns.size(); i++) {
			const auto &partition_col_name = op.names[op.partition_columns[i]];
			const auto &partition_value = (*partition_values)[i];
			partition_keys.emplace_back(partition_col_name);
			partition_values_as_varchar.push_back(partition_value.DefaultCastAs(LogicalType::VARCHAR));
		}
		result.written_file_info->partition_keys =
		    Value::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR, std::move(partition_keys),
		               std::move(partition_values_as_varchar));
	}
	return result;
}

void CopyOutputFileRegistry::PublishCreatedPath(PendingFileState &pending_file_state, string output_path) {
	pending_file_state.output_path = std::move(output_path);
	if (pending_file_state.written_file_info) {
		pending_file_state.written_file_info->file_path = pending_file_state.output_path;
	}
}

optional_ptr<CopyToFileInfo> CopyOutputFileRegistry::AddFile(const string &file_name) {
	auto file_info = make_uniq<CopyToFileInfo>(file_name);
	if (GetCopyFunctionBaseReturnType(op.return_type) == CopyFunctionReturnType::WRITTEN_FILE_STATISTICS) {
		file_info->file_stats = make_uniq<CopyFunctionFileStatistics>();
	}
	auto result = file_info.get();
	written_files.push_back(std::move(file_info));
	return result;
}

static bool PhysicalCopyRotateNow(const PhysicalCopyToFile &op, GlobalFileState &global_state)
    DUCKDB_REQUIRES(global_state.lock);

//===--------------------------------------------------------------------===//
// Partitioned Copy Scoped Guards
//===--------------------------------------------------------------------===//
DelayedPartitionFlushGuard::~DelayedPartitionFlushGuard() {
	if (active) {
		partitioned_copy.CompleteDelayedPartition(values, false);
	}
}

optional<DelayedPartitionFlush> DelayedPartitionFlushGuard::Complete() {
	if (!active) {
		return nullopt;
	}
	active = false;
	return partitioned_copy.CompleteDelayedPartition(values, true);
}

//===--------------------------------------------------------------------===//
// Partition Write Manager
//===--------------------------------------------------------------------===//
PartitionWriteLease::PartitionWriteLease(PartitionWriteManager &manager_p, PartitionWriteInfo &write_info_p)
    : manager(manager_p), write_info(write_info_p) {
}

PartitionWriteLease::PartitionWriteLease(PartitionWriteLease &&other) noexcept
    : manager(other.manager), write_info(other.write_info) {
	other.manager = nullptr;
	other.write_info = nullptr;
}

PartitionWriteLease &PartitionWriteLease::operator=(PartitionWriteLease &&other) noexcept {
	if (this != &other) {
		Reset();
		manager = other.manager;
		write_info = other.write_info;
		other.manager = nullptr;
		other.write_info = nullptr;
	}
	return *this;
}

PartitionWriteLease::~PartitionWriteLease() {
	Reset();
}

void PartitionWriteLease::Reset() {
	if (manager && write_info) {
		manager->Release(*write_info);
	}
	manager = nullptr;
	write_info = nullptr;
}

PartitionWriteLease PartitionWriteManager::Acquire(const vector<Value> &values) {
	PartitionWriteInfo *result;
	{
		annotated_lock_guard<annotated_mutex> guard(lock);
		auto active_write_entry = active_writes.find(values);
		if (active_write_entry != active_writes.end()) {
			active_write_entry->second->active_writes++;
			result = active_write_entry->second.get();
		} else {
			auto info = make_uniq<PartitionWriteInfo>();
			result = info.get();
			info->active_writes = 1;
			active_writes.insert(make_pair(values, std::move(info)));
		}
	}
	return PartitionWriteLease(*this, *result);
}

PartitionWriteManager::ReservationLock PartitionWriteManager::LockForReservation() {
	return ReservationLock(lock);
}

void PartitionWriteManager::ClaimDirectory(ReservationLock &reservation_lock, const vector<Value> &values,
                                           const string &directory) DUCKDB_NO_THREAD_SAFETY_ANALYSIS {
	D_ASSERT(reservation_lock.guard.owns_lock());
	D_ASSERT(!directory.empty());
	auto claimed = partition_directories.find(values);
	if (claimed != partition_directories.end() && claimed->second == directory) {
		return;
	}
	auto owner = directory_partitions.find(directory);
	if (owner != directory_partitions.end()) {
		throw InvalidInputException("PARTITION_PATH puts partitions (%s) and (%s) in the same directory \"%s\"",
		                            PartitionValuesToString(op, owner->second), PartitionValuesToString(op, values),
		                            directory);
	}
	directory_partitions.emplace(directory, values);
	partition_directories[values] = directory;
}

PartitionFileStateReservation
PartitionWriteManager::ReserveFileState(ReservationLock &reservation_lock, const vector<Value> &values,
                                        FileCreationReason reason) DUCKDB_NO_THREAD_SAFETY_ANALYSIS {
	D_ASSERT(reservation_lock.guard.owns_lock());
	PartitionFileStateReservation reservation;
	if (active_writes.size() >= Settings::Get<PartitionedWriteMaxOpenFilesSetting>(context)) {
		for (auto it = active_writes.begin(); it != active_writes.end(); ++it) {
			if (it->second->active_writes == 0) {
				reservation.files_to_finalize.push_back(TakeInactiveFileStateLocked(it));
				break;
			}
		}
	}

	if (UsePerPartitionFileOffsets(op)) {
		if (reason == FileCreationReason::SORTED_RUN_BOUNDARY || reason == FileCreationReason::ROTATION) {
			++previous_partitions[values];
		}
		auto prev_offset = previous_partitions.find(values);
		if (prev_offset != previous_partitions.end()) {
			reservation.offset = prev_offset->second;
		}
	} else {
		reservation.offset = global_offset++;
	}

	return reservation;
}

vector<FileStateHandle> PartitionWriteManager::TakeOpenFileStates() {
	vector<FileStateHandle> files_to_finalize;
	{
		annotated_lock_guard<annotated_mutex> guard(lock);
		for (auto &entry : active_writes) {
			D_ASSERT(entry.second->active_writes == 0);
			files_to_finalize.push_back(std::move(entry.second->file_state));
		}
		active_writes.clear();
	}
	return files_to_finalize;
}

FileStateHandle PartitionWriteManager::TakeInactiveFileStateLocked(ActiveWriteIterator entry) {
	D_ASSERT(entry != active_writes.end());
	D_ASSERT(entry->second->active_writes == 0);

	auto file_state = std::move(entry->second->file_state);
	if (UsePerPartitionFileOffsets(op)) {
		++previous_partitions[entry->first];
	}
	active_writes.erase(entry);
	return file_state;
}

FileStateHandle PartitionWriteManager::TryTakeInactiveFileState(const vector<Value> &values) {
	annotated_lock_guard<annotated_mutex> guard(lock);
	auto entry = active_writes.find(values);
	if (entry == active_writes.end() || entry->second->active_writes > 0) {
		return FileStateHandle();
	}
	return TakeInactiveFileStateLocked(entry);
}

void PartitionWriteManager::Release(PartitionWriteInfo &write_info) {
	annotated_lock_guard<annotated_mutex> guard(lock);
	D_ASSERT(write_info.active_writes > 0);
	write_info.active_writes--;
}

//===--------------------------------------------------------------------===//
// Sort Strategy Input
//===--------------------------------------------------------------------===//
//! The rows of a flush generation buffered and sorted by a SortStrategy (hash bins sorted by the partition keys)
class SortStrategyInputGlobalState : public PartitionedCopyInputGlobalState {
public:
	explicit SortStrategyInputGlobalState(unique_ptr<GlobalSinkState> sink_p) : sink(std::move(sink_p)) {
	}

	unique_ptr<GlobalSinkState> sink;
	unique_ptr<GlobalSourceState> source;
};

class SortStrategyInputLocalState : public PartitionedCopyInputLocalState {
public:
	explicit SortStrategyInputLocalState(unique_ptr<LocalSinkState> sink_p) : sink(std::move(sink_p)) {
	}

	unique_ptr<LocalSinkState> sink;
};

class SortStrategyInput : public PartitionedCopyInput {
public:
	SortStrategyInput(const PartitionedCopy &partitioned_copy, unique_ptr<const SortStrategy> strategy);

public:
	unique_ptr<PartitionedCopyInputGlobalState> GetGlobalState(ClientContext &context) const override;
	unique_ptr<PartitionedCopyInputLocalState> GetLocalState(ExecutionContext &context,
	                                                         PartitionedCopyInputGlobalState &gstate,
	                                                         ParallelHyperLogLogLocalState &hll) const override;
	void Sink(ExecutionContext &context, DataChunk &chunk, PartitionedCopyInputGlobalState &gstate,
	          PartitionedCopyInputLocalState &lstate, InterruptState &interrupt) const override;
	void Combine(ExecutionContext &context, PartitionedCopyInputGlobalState &gstate,
	             PartitionedCopyInputLocalState &lstate, InterruptState &interrupt) const override;
	vector<unique_ptr<PartitionedCopyGroup>> Finalize(ClientContext &context, PartitionedCopyInputGlobalState &gstate,
	                                                  InterruptState &interrupt) const override;
	unique_ptr<ColumnDataCollection> SortCollection(ExecutionContext &context, InterruptState &interrupt,
	                                                unique_ptr<ColumnDataCollection> collection) const override;

	//! Finalizes the strategy and returns its non-empty hash bins and their total row count
	vector<idx_t> FinalizeBins(ClientContext &context, SortStrategyInputGlobalState &gstate, InterruptState &interrupt,
	                           idx_t &total_count) const;

public:
	const PartitionedCopy &partitioned_copy;
	const unique_ptr<const SortStrategy> strategy;
};

//! A hash bin of a SortStrategyInput (or all bins of an overflow sort): sorted, materialized and masked for partition
//! boundaries before reading. The materialized collection packs its chunks densely, so the mask and the preparation
//! tasks address rows by block.
class SortedBinGroup : public PartitionedCopyGroup {
public:
	enum Step : idx_t { SORT = 0, MATERIALIZE = 1, MASK = 2, STEP_COUNT = 3 };

	class ScanState : public PartitionedCopyGroupScanState {
	public:
		ColumnDataScanState state;
	};

	//! Walks the chunks of the materialized collection through the column data cursor
	class Cursor : public PartitionedCopyGroupCursor {
	public:
		explicit Cursor(const SortedBinGroup &group) : group(group), cursor(*group.collection) {
		}

	public:
		bool Next() override {
			return cursor.Next();
		}
		PartitionedCopyChunkPosition Position() const override {
			const auto &position = cursor.Position();
			return {position.segment_index, position.chunk_index, position.row_offset};
		}
		idx_t Count() const override {
			return cursor.Count();
		}
		idx_t ByteEstimate() const override {
			return cursor.AllocationSize(group.input.partitioned_copy.write_columns);
		}
		void PartitionBoundaries(vector<idx_t> &boundaries) const override {
			group.PartitionBoundaries(cursor.Position().row_offset, Count(), boundaries);
		}
		void RowSizes(vector<idx_t> &sizes) override;

	private:
		const SortedBinGroup &group;
		ColumnDataChunkCursor cursor;
		//! The sizing state, created on first use
		unique_ptr<CopyBatchSlicer> slicer;
		DataChunk scan_chunk;
		DataChunk size_chunk;
	};

public:
	SortedBinGroup(const SortStrategyInput &input, SortStrategyInputGlobalState &gstate, vector<idx_t> bins_p,
	               idx_t count_p)
	    : input(input), gstate(gstate), bins(std::move(bins_p)), count(count_p), materialized(0) {
	}

public:
	idx_t Count() const override {
		return count;
	}
	idx_t PreparationStepCount() const override {
		return STEP_COUNT;
	}
	void ExecutePreparation(ExecutionContext &context, InterruptState &interrupt, idx_t step, idx_t begin_block,
	                        idx_t end_block) override;
	void FinishPreparation(idx_t step) override;

	unique_ptr<PartitionedCopyGroupCursor> InitializeCursor(const CopyBatchSlicer &) const override {
		return make_uniq<Cursor>(*this);
	}
	unique_ptr<PartitionedCopyGroupScanState>
	InitializeScan(const PartitionedCopyChunkPosition &position) const override;
	void InitializeScanChunk(DataChunk &chunk) const override {
		collection->InitializeScanChunk(chunk);
	}
	bool Scan(PartitionedCopyGroupScanState &state, DataChunk &chunk) const override;

private:
	void TakeMaterializedBins(ExecutionContext &context, OperatorSourceInput &source_input);
	void Mask(idx_t begin_block, idx_t end_block);
	void PartitionBoundaries(idx_t row_begin, idx_t chunk_rows, vector<idx_t> &boundaries) const;
	//! The chunk holding a row
	idx_t ChunkOf(idx_t row) const;

private:
	const SortStrategyInput &input;
	SortStrategyInputGlobalState &gstate;
	//! The hash bins read through this group: one, or all of them for an overflow sort
	const vector<idx_t> bins;
	const idx_t count;
	//! Count of materialized blocks
	atomic<idx_t> materialized;
	mutex collection_lock;
	//! The materialized bins (set after MATERIALIZE)
	unique_ptr<ColumnDataCollection> collection;
	//! The row offset of every chunk: the chunks at bin boundaries of an overflow sort are not full
	vector<idx_t> chunk_row_offsets;
	//! The partition boundary mask (marks where the partition key changes within the group)
	ValidityMask partition_mask;
};

SortStrategyInput::SortStrategyInput(const PartitionedCopy &partitioned_copy_p,
                                     unique_ptr<const SortStrategy> strategy_p)
    : partitioned_copy(partitioned_copy_p), strategy(std::move(strategy_p)) {
}

unique_ptr<PartitionedCopyInputGlobalState> SortStrategyInput::GetGlobalState(ClientContext &context) const {
	return make_uniq<SortStrategyInputGlobalState>(strategy->GetGlobalSinkState(context));
}

unique_ptr<PartitionedCopyInputLocalState> SortStrategyInput::GetLocalState(ExecutionContext &context,
                                                                            PartitionedCopyInputGlobalState &gstate,
                                                                            ParallelHyperLogLogLocalState &hll) const {
	auto result = make_uniq<SortStrategyInputLocalState>(strategy->GetLocalSinkState(context));
	strategy->RegisterHyperLogLog(*result->sink, hll);
	return std::move(result);
}

void SortStrategyInput::Sink(ExecutionContext &context, DataChunk &chunk, PartitionedCopyInputGlobalState &gstate_p,
                             PartitionedCopyInputLocalState &lstate_p, InterruptState &interrupt) const {
	auto &gstate = gstate_p.Cast<SortStrategyInputGlobalState>();
	auto &lstate = lstate_p.Cast<SortStrategyInputLocalState>();
	OperatorSinkInput sink_input {*gstate.sink, *lstate.sink, interrupt};
	strategy->Sink(context, chunk, sink_input);
}

void SortStrategyInput::Combine(ExecutionContext &context, PartitionedCopyInputGlobalState &gstate_p,
                                PartitionedCopyInputLocalState &lstate_p, InterruptState &interrupt) const {
	auto &gstate = gstate_p.Cast<SortStrategyInputGlobalState>();
	auto &lstate = lstate_p.Cast<SortStrategyInputLocalState>();
	OperatorSinkCombineInput combine_input {*gstate.sink, *lstate.sink, interrupt};
	strategy->Combine(context, combine_input);
}

vector<idx_t> SortStrategyInput::FinalizeBins(ClientContext &context, SortStrategyInputGlobalState &gstate,
                                              InterruptState &interrupt, idx_t &total_count) const {
	OperatorSinkFinalizeInput finalize_input {*gstate.sink, interrupt};
	if (strategy->Finalize(context, finalize_input) == SinkFinalizeType::BLOCKED) {
		// the flush runs the strategy's tasks itself, so there is nothing that could resume it
		throw InternalException("PartitionedCopy cannot resume a blocked sort strategy finalize");
	}
	gstate.source = strategy->GetGlobalSourceState(context, *gstate.sink);
	const auto &chunk_rows = strategy->GetHashGroups(*gstate.source);

	vector<idx_t> bins;
	total_count = 0;
	for (idx_t bin = 0; bin < chunk_rows.size(); bin++) {
		if (chunk_rows[bin].count) {
			bins.push_back(bin);
			total_count += chunk_rows[bin].count;
		}
	}
	return bins;
}

unique_ptr<ColumnDataCollection> SortStrategyInput::SortCollection(ExecutionContext &execution_context,
                                                                   InterruptState &interrupt_state,
                                                                   unique_ptr<ColumnDataCollection> collection) const {
	D_ASSERT(collection);
	D_ASSERT(collection->Count() > 0);
	auto &context = execution_context.client;

	auto global_sink_state = strategy->GetGlobalSinkState(context);
	auto local_sink_state = strategy->GetLocalSinkState(execution_context);

	ColumnDataScanState scan_state;
	collection->InitializeScan(scan_state);
	DataChunk scan_chunk;
	collection->InitializeScanChunk(scan_state, scan_chunk);

	while (collection->Scan(scan_state, scan_chunk)) {
		OperatorSinkInput sink_input {*global_sink_state, *local_sink_state, interrupt_state};
		strategy->Sink(execution_context, scan_chunk, sink_input);
	}

	OperatorSinkCombineInput combine_input {*global_sink_state, *local_sink_state, interrupt_state};
	strategy->Combine(execution_context, combine_input);

	OperatorSinkFinalizeInput finalize_input {*global_sink_state, interrupt_state};
	strategy->Finalize(context, finalize_input);

	auto global_source_state = strategy->GetGlobalSourceState(context, *global_sink_state);
	const auto &hash_groups = strategy->GetHashGroups(*global_source_state);

	auto local_source_state = strategy->GetLocalSourceState(execution_context, *global_source_state);
	OperatorSourceInput source_input {*global_source_state, *local_source_state, interrupt_state};
	for (idx_t group_idx = 0; group_idx < hash_groups.size(); group_idx++) {
		if (!hash_groups[group_idx].count) {
			continue;
		}

		strategy->SortColumnData(execution_context, group_idx, finalize_input);
		while (strategy->MaterializeColumnData(execution_context, group_idx, source_input) ==
		       SourceResultType::HAVE_MORE_OUTPUT) {
		}

		auto result = strategy->GetColumnData(group_idx, source_input);
		if (!result || result->Count() != collection->Count()) {
			throw InternalException("Failed to materialize delayed partition data");
		}
		return result;
	}

	throw InternalException("Failed to find delayed partition data after sorting");
}

void SortedBinGroup::ExecutePreparation(ExecutionContext &context, InterruptState &interrupt, idx_t step,
                                        idx_t begin_block, idx_t end_block) {
	auto &strategy = *input.strategy;
	switch (step) {
	case SORT: {
		OperatorSinkFinalizeInput finalize_input {*gstate.sink, interrupt};
		for (const auto &bin : bins) {
			strategy.SortColumnData(context, bin, finalize_input);
		}
		break;
	}
	case MATERIALIZE: {
		auto unused = make_uniq<LocalSourceState>();
		OperatorSourceInput source_input {*gstate.source, *unused, interrupt};
		for (const auto &bin : bins) {
			strategy.MaterializeColumnData(context, bin, source_input);
		}
		// the last block range to finish takes the materialized collection
		const auto blocks = PartitionedCopyHashGroup::BinValue<idx_t>(count, STANDARD_VECTOR_SIZE);
		const auto materialized_blocks = materialized += end_block - begin_block;
		if (materialized_blocks >= blocks) {
			TakeMaterializedBins(context, source_input);
		}
		break;
	}
	case MASK:
		Mask(begin_block, end_block);
		break;
	default:
		throw InternalException("Invalid SortedBinGroup preparation step");
	}
}

void SortedBinGroup::TakeMaterializedBins(ExecutionContext &context, OperatorSourceInput &source_input) {
	auto &strategy = *input.strategy;
	lock_guard<mutex> guard(collection_lock);
	if (collection) {
		return;
	}
	for (const auto &bin : bins) {
		// every task has finished its materialization call; finish whatever work the bin has left
		while (strategy.MaterializeColumnData(context, bin, source_input) == SourceResultType::HAVE_MORE_OUTPUT) {
		}
		auto bin_data = strategy.GetColumnData(bin, source_input);
		if (!bin_data) {
			throw InternalException("Failed to materialize a sorted partition bin");
		}
		if (!collection) {
			collection = std::move(bin_data);
		} else {
			// several bins (an overflow sort): the segments move over; the chunks at the bin boundaries stay partial,
			// which the cursor and the mask handle through the actual row offsets
			collection->Combine(*bin_data);
		}
	}
}

void SortedBinGroup::FinishPreparation(idx_t step) {
	if (step == MATERIALIZE) {
		if (!collection || collection->Count() != count) {
			throw InternalException("Failed to materialize a sorted partition bin");
		}
		chunk_row_offsets.clear();
		ColumnDataChunkCursor cursor(*collection);
		while (cursor.Next()) {
			chunk_row_offsets.push_back(cursor.Position().row_offset);
		}
		partition_mask.Initialize(count);
	}
}

idx_t SortedBinGroup::ChunkOf(idx_t row) const {
	D_ASSERT(row < count);
	auto entry = std::upper_bound(chunk_row_offsets.begin(), chunk_row_offsets.end(), row);
	D_ASSERT(entry != chunk_row_offsets.begin());
	return NumericCast<idx_t>(entry - chunk_row_offsets.begin()) - 1;
}

void SortedBinGroup::Mask(idx_t begin_block, idx_t end_block) {
	D_ASSERT(collection);
	D_ASSERT(partition_mask.IsMaskSet());

	// The task owns whole mask words: those whose first row lies in its block range, so that no two tasks write the
	// same word. The rows of a word may straddle chunks (the overflow sort combines bins whose last chunks are not
	// full), so the rows to mark and the chunks to compare come from the actual row offsets, not from block numbers.
	constexpr auto word_bits = ValidityMask::BITS_PER_VALUE;
	const auto row_begin = MinValue<idx_t>(begin_block * STANDARD_VECTOR_SIZE, count);
	const auto row_end = MinValue<idx_t>(end_block * STANDARD_VECTOR_SIZE, count);
	const auto owned_begin = MinValue<idx_t>(ValidityMask::EntryCount(row_begin) * word_bits, count);
	const auto owned_end = MinValue<idx_t>(ValidityMask::EntryCount(row_end) * word_bits, count);
	if (owned_begin >= owned_end) {
		return;
	}

	partition_mask.SetRangeInvalid(count, owned_begin / word_bits, ValidityMask::EntryCount(owned_end));
	if (owned_begin == 0) {
		partition_mask.SetValidUnsafe(0);
	}

	// Only compare partition columns (not order columns)
	const auto key_count = input.partitioned_copy.op.partition_columns.size();
	if (key_count == 0) {
		return;
	}
	auto &scan_cols = input.strategy->sort_ids;
	const auto chunk_begin = ChunkOf(owned_begin);
	const auto chunk_end = ChunkOf(owned_end - 1) + 1;
	WindowDeltaScanner(*collection, chunk_begin, chunk_end, scan_cols, key_count,
	                   [&](const idx_t row_idx, DataChunk &, DataChunk &, const idx_t ndistinct,
	                       const SelectionVector &distinct, const SelectionVector &) {
		                   for (idx_t i = 0; i < ndistinct; i++) {
			                   const auto row = row_idx + distinct.get_index_unsafe(i);
			                   if (row >= owned_begin && row < owned_end) {
				                   partition_mask.SetValidUnsafe(row);
			                   }
		                   }
	                   });
}

void SortedBinGroup::Cursor::RowSizes(vector<idx_t> &sizes) {
	auto &partitioned_copy = group.input.partitioned_copy;
	if (!slicer) {
		slicer = make_uniq<CopyBatchSlicer>(partitioned_copy.write_types, partitioned_copy.op.batch_size_bytes);
		group.InitializeScanChunk(scan_chunk);
		size_chunk.Initialize(partitioned_copy.context, partitioned_copy.write_types);
	}
	auto scan_state = group.InitializeScan(Position());
	if (!group.Scan(*scan_state, scan_chunk) || scan_chunk.size() != Count()) {
		throw InternalException("Partitioned copy could not scan a chunk to size its rows");
	}
	size_chunk.ReferenceColumns(scan_chunk, partitioned_copy.write_columns);
	sizes.clear();
	for (const auto &size : slicer->ComputeRowSizes(size_chunk).Values<idx_t>()) {
		sizes.push_back(size.GetValue());
	}
}

void SortedBinGroup::PartitionBoundaries(idx_t row_begin, idx_t chunk_rows, vector<idx_t> &boundaries) const {
	const auto row_end = row_begin + chunk_rows;
	const auto begin_entry = row_begin / ValidityMask::BITS_PER_VALUE;
	const auto end_entry = ValidityMask::EntryCount(row_end);
	for (idx_t entry_idx = begin_entry; entry_idx < end_entry; entry_idx++) {
		const auto validity_entry = partition_mask.GetValidityEntry(entry_idx);
		if (ValidityMask::NoneValid(validity_entry)) {
			continue;
		}
		const auto entry_start = entry_idx * ValidityMask::BITS_PER_VALUE;
		const auto row_start = MaxValue(row_begin, entry_start);
		const auto row_stop = MinValue(row_end, entry_start + ValidityMask::BITS_PER_VALUE);
		for (idx_t row = row_start; row < row_stop; row++) {
			// the first row of the group starts the first partition and is no boundary
			if (row && ValidityMask::RowIsValid(validity_entry, row - entry_start)) {
				boundaries.push_back(row - row_begin);
			}
		}
	}
}

unique_ptr<PartitionedCopyGroupScanState>
SortedBinGroup::InitializeScan(const PartitionedCopyChunkPosition &position) const {
	auto result = make_uniq<ScanState>();
	collection->InitializeScan(result->state);
	collection->SeekScan(result->state, {position.segment_idx, position.chunk_idx, position.row_offset});
	return std::move(result);
}

bool SortedBinGroup::Scan(PartitionedCopyGroupScanState &state_p, DataChunk &chunk) const {
	auto &state = state_p.Cast<ScanState>();
	return collection->Scan(state.state, chunk);
}

//===--------------------------------------------------------------------===//
// Integer Partition Input
//===--------------------------------------------------------------------===//
//! The rows of a flush generation partitioned on the value of a bounded integer key: every partition holds one key
class IntegerPartitionInputGlobalState : public PartitionedCopyInputGlobalState {
public:
	//! The combined partitions, until Finalize hands them to the groups
	unique_ptr<IntegerPartitionedTupleData> partitions;
	mutex lock;
};

class IntegerPartitionInputLocalState : public PartitionedCopyInputLocalState {
public:
	unique_ptr<IntegerPartitionedTupleData> partitions;
	PartitionedTupleDataAppendState append_state;
	//! The key hashes feeding the partition count estimate
	DataChunk hashes;
	optional_ptr<ParallelHyperLogLogLocalState> hll;
};

class IntegerPartitionInput : public PartitionedCopyInput {
public:
	IntegerPartitionInput(const PartitionedCopy &partitioned_copy, idx_t key_col, Value min_value, idx_t value_count);

public:
	unique_ptr<PartitionedCopyInputGlobalState> GetGlobalState(ClientContext &context) const override;
	unique_ptr<PartitionedCopyInputLocalState> GetLocalState(ExecutionContext &context,
	                                                         PartitionedCopyInputGlobalState &gstate,
	                                                         ParallelHyperLogLogLocalState &hll) const override;
	void Sink(ExecutionContext &context, DataChunk &chunk, PartitionedCopyInputGlobalState &gstate,
	          PartitionedCopyInputLocalState &lstate, InterruptState &interrupt) const override;
	void Combine(ExecutionContext &context, PartitionedCopyInputGlobalState &gstate,
	             PartitionedCopyInputLocalState &lstate, InterruptState &interrupt) const override;
	vector<unique_ptr<PartitionedCopyGroup>> Finalize(ClientContext &context, PartitionedCopyInputGlobalState &gstate,
	                                                  InterruptState &interrupt) const override;
	unique_ptr<ColumnDataCollection> SortCollection(ExecutionContext &context, InterruptState &interrupt,
	                                                unique_ptr<ColumnDataCollection> collection) const override {
		throw InternalException("IntegerPartitionInput does not sort: it is only used without ORDER BY");
	}

	//! A sort strategy input keyed on the partition column alone and without statistics, for the keys outside the
	//! bounds (it never retries integer partitioning)
	unique_ptr<SortStrategyInput> CreateOverflowInput(ClientContext &context, idx_t count) const;

private:
	unique_ptr<IntegerPartitionedTupleData> CreatePartitions(ClientContext &context) const;

public:
	const PartitionedCopy &partitioned_copy;
	//! The partition key column, the smallest key within the bounds and the number of keys within them
	const idx_t key_col;
	const Value min_value;
	const idx_t value_count;
	//! The row layout: the COPY's columns, no hash
	shared_ptr<TupleDataLayout> layout;
};

//! A group holding a single partition key, read straight from the tuple data it owns: a partition of an
//! IntegerPartitionInput, or a hash bin of a SortStrategyInput proven to hold one key (whose tuples carry a trailing
//! hash column that the scan projects away)
class DirectPartitionGroup : public PartitionedCopyGroup {
public:
	class ScanState : public PartitionedCopyGroupScanState {
	public:
		TupleDataScanState state;
	};

	//! Walks the tuple chunks without gathering their values
	class Cursor : public PartitionedCopyGroupCursor {
	public:
		Cursor(const DirectPartitionGroup &group, const CopyBatchSlicer &slicer)
		    : data(*group.data), cursor(data), fixed_row_bytes(slicer.GetFixedRowBytes()),
		      omitted_heap_bytes(slicer.FixedRowSize().IsValid() ? 0 : group.OmittedHeapBytes()),
		      has_heap(!data.GetLayout().AllConstant()) {
		}

	public:
		bool Next() override {
			if (started) {
				row_offset += cursor.Count();
			} else {
				started = true;
			}
			return cursor.Next();
		}
		PartitionedCopyChunkPosition Position() const override {
			const auto position = cursor.Position();
			return {position.segment_index, position.chunk_index, row_offset};
		}
		idx_t Count() const override {
			return cursor.Count();
		}
		idx_t ByteEstimate() const override {
			const auto heap_bytes = cursor.HeapSize();
			// Clamp before multiplying so an estimated key size cannot overflow or underflow the heap total.
			const auto omitted_bytes = MinValue(omitted_heap_bytes, heap_bytes / Count()) * Count();
			return fixed_row_bytes * Count() + heap_bytes - omitted_bytes;
		}
		void PartitionBoundaries(vector<idx_t> &boundaries) const override {
			// a single key: no boundaries
		}
		void RowSizes(vector<idx_t> &sizes) override;

	private:
		TupleDataCollection &data;
		TupleDataChunkCursor cursor;
		const idx_t fixed_row_bytes;
		const idx_t omitted_heap_bytes;
		const bool has_heap;
		idx_t row_offset = 0;
		bool started = false;
	};

public:
	DirectPartitionGroup(const PartitionedCopy &partitioned_copy, unique_ptr<TupleDataCollection> data_p,
	                     vector<column_t> column_ids_p = {})
	    : partitioned_copy(partitioned_copy), data(std::move(data_p)), count(data->Count()),
	      column_ids(std::move(column_ids_p)) {
		if (column_ids.empty()) {
			const auto column_count = data->GetLayout().ColumnCount();
			for (column_t col_idx = 0; col_idx < column_count; col_idx++) {
				column_ids.push_back(col_idx);
			}
		}
		for (const auto &col_idx : column_ids) {
			scan_types.push_back(data->GetLayout().GetTypes()[col_idx]);
		}
	}

public:
	idx_t Count() const override {
		return count;
	}
	idx_t PreparationStepCount() const override {
		return 0;
	}
	void ExecutePreparation(ExecutionContext &, InterruptState &, idx_t, idx_t, idx_t) override {
		throw InternalException("DirectPartitionGroup has no preparation steps");
	}
	void FinishPreparation(idx_t) override {
		throw InternalException("DirectPartitionGroup has no preparation steps");
	}

	unique_ptr<PartitionedCopyGroupCursor> InitializeCursor(const CopyBatchSlicer &slicer) const override {
		return make_uniq<Cursor>(*this, slicer);
	}
	unique_ptr<PartitionedCopyGroupScanState>
	InitializeScan(const PartitionedCopyChunkPosition &position) const override;
	void InitializeScanChunk(DataChunk &chunk) const override {
		chunk.Initialize(Allocator::Get(partitioned_copy.context), scan_types);
	}
	bool Scan(PartitionedCopyGroupScanState &state, DataChunk &chunk) const override;

private:
	idx_t OmittedHeapBytes() const;

private:
	const PartitionedCopy &partitioned_copy;
	unique_ptr<TupleDataCollection> data;
	const idx_t count;
	//! The columns read from the tuples, and their types
	vector<column_t> column_ids;
	vector<LogicalType> scan_types;
};

//! The keys of an IntegerPartitionInput outside the statistics' bounds: the preparation tasks re-sink them into a
//! sort strategy input of their own, after which they are sorted, materialized and masked like the hash bins of a
//! SortStrategyInput (all bins through one SortedBinGroup)
class OverflowSortGroup : public PartitionedCopyGroup {
public:
	enum Step : idx_t { RESINK = 0, STEP_COUNT = 1 + SortedBinGroup::STEP_COUNT };

public:
	OverflowSortGroup(const IntegerPartitionInput &input, ClientContext &context,
	                  unique_ptr<TupleDataCollection> overflow_p);

public:
	idx_t Count() const override {
		return count;
	}
	idx_t PreparationStepCount() const override {
		return STEP_COUNT;
	}
	void ExecutePreparation(ExecutionContext &context, InterruptState &interrupt, idx_t step, idx_t begin_block,
	                        idx_t end_block) override;
	void FinishPreparation(idx_t step) override;

	unique_ptr<PartitionedCopyGroupCursor> InitializeCursor(const CopyBatchSlicer &slicer) const override {
		return sorted->InitializeCursor(slicer);
	}
	unique_ptr<PartitionedCopyGroupScanState>
	InitializeScan(const PartitionedCopyChunkPosition &position) const override {
		return sorted->InitializeScan(position);
	}
	void InitializeScanChunk(DataChunk &chunk) const override {
		sorted->InitializeScanChunk(chunk);
	}
	bool Scan(PartitionedCopyGroupScanState &state, DataChunk &chunk) const override {
		return sorted->Scan(state, chunk);
	}

private:
	void Resink(ExecutionContext &context, InterruptState &interrupt, idx_t begin_block, idx_t end_block);
	void FinalizeSort(ClientContext &context, InterruptState &interrupt);

private:
	//! The overflow rows, until they have been re-sunk
	unique_ptr<TupleDataCollection> overflow;
	const idx_t count;
	//! The chunk positions of the overflow rows, split over the re-sink tasks
	vector<TupleDataChunkPosition> chunks;
	//! The sort strategy input the rows are re-sunk into
	const unique_ptr<SortStrategyInput> sort_input;
	unique_ptr<PartitionedCopyInputGlobalState> sort_state;
	//! Count of re-sunk blocks
	atomic<idx_t> resunk;
	//! The sorted bins, set once by the last re-sink task
	mutex sorted_lock;
	unique_ptr<SortedBinGroup> sorted;
};

IntegerPartitionInput::IntegerPartitionInput(const PartitionedCopy &partitioned_copy_p, idx_t key_col_p,
                                             Value min_value_p, idx_t value_count_p)
    : partitioned_copy(partitioned_copy_p), key_col(key_col_p), min_value(std::move(min_value_p)),
      value_count(value_count_p) {
	layout = make_shared_ptr<TupleDataLayout>();
	layout->Initialize(partitioned_copy.op.expected_types, TupleDataValidityType::CAN_HAVE_NULL_VALUES);
}

unique_ptr<IntegerPartitionedTupleData> IntegerPartitionInput::CreatePartitions(ClientContext &context) const {
	return make_uniq<IntegerPartitionedTupleData>(BufferManager::GetBufferManager(context), layout,
	                                              MemoryTag::COLUMN_DATA, key_col, min_value, value_count, context);
}

unique_ptr<SortStrategyInput> IntegerPartitionInput::CreateOverflowInput(ClientContext &context, idx_t count) const {
	auto &op = partitioned_copy.op;
	vector<unique_ptr<Expression>> partition_bys;
	partition_bys.push_back(make_uniq<BoundReferenceExpression>(op.expected_types[key_col], key_col));
	vector<unique_ptr<BaseStatistics>> no_stats;
	OperatorPartitionInfo unpartitioned_info;
	auto strategy = SortStrategy::Factory(context, partition_bys, op.order_columns, op.expected_types, no_stats,
	                                      unpartitioned_info, count);
	return make_uniq<SortStrategyInput>(partitioned_copy, std::move(strategy));
}

unique_ptr<PartitionedCopyInputGlobalState> IntegerPartitionInput::GetGlobalState(ClientContext &context) const {
	auto result = make_uniq<IntegerPartitionInputGlobalState>();
	result->partitions = CreatePartitions(context);
	return std::move(result);
}

unique_ptr<PartitionedCopyInputLocalState>
IntegerPartitionInput::GetLocalState(ExecutionContext &context, PartitionedCopyInputGlobalState &gstate,
                                     ParallelHyperLogLogLocalState &hll) const {
	auto result = make_uniq<IntegerPartitionInputLocalState>();
	result->partitions = CreatePartitions(context.client);
	result->partitions->InitializeAppendState(result->append_state);
	result->hashes.Initialize(Allocator::Get(context.client), {LogicalType::HASH});
	result->hll = hll;
	return std::move(result);
}

void IntegerPartitionInput::Sink(ExecutionContext &context, DataChunk &chunk, PartitionedCopyInputGlobalState &gstate_p,
                                 PartitionedCopyInputLocalState &lstate_p, InterruptState &interrupt) const {
	auto &lstate = lstate_p.Cast<IntegerPartitionInputLocalState>();
	lstate.partitions->Append(lstate.append_state, chunk);

	// the flush heuristic estimates the number of partitions from key hashes
	auto &hashes = lstate.hashes;
	hashes.Reset();
	VectorOperations::Hash(chunk.data[key_col], hashes.data[0], chunk.size());
	hashes.CheckCardinality(chunk.size());
	lstate.hll->Update(hashes.data[0]);
}

void IntegerPartitionInput::Combine(ExecutionContext &context, PartitionedCopyInputGlobalState &gstate_p,
                                    PartitionedCopyInputLocalState &lstate_p, InterruptState &interrupt) const {
	auto &gstate = gstate_p.Cast<IntegerPartitionInputGlobalState>();
	auto &lstate = lstate_p.Cast<IntegerPartitionInputLocalState>();
	lstate.partitions->FlushAppendState(lstate.append_state);
	lock_guard<mutex> guard(gstate.lock);
	gstate.partitions->Combine(*lstate.partitions);
}

vector<unique_ptr<PartitionedCopyGroup>> IntegerPartitionInput::Finalize(ClientContext &context,
                                                                         PartitionedCopyInputGlobalState &gstate_p,
                                                                         InterruptState &interrupt) const {
	auto &gstate = gstate_p.Cast<IntegerPartitionInputGlobalState>();
	// the groups take over the rows: a second finalization would see nothing
	auto partitioned_data = std::move(gstate.partitions);
	if (!partitioned_data) {
		throw InternalException("IntegerPartitionInput was finalized twice");
	}
	auto &partitions = partitioned_data->GetPartitions();
	const auto overflow_idx = partitioned_data->OverflowPartition();

	vector<unique_ptr<PartitionedCopyGroup>> groups;
	for (idx_t partition_idx = 0; partition_idx < partitions.size(); partition_idx++) {
		auto &partition = partitions[partition_idx];
		if (!partition || !partition->Count()) {
			continue;
		}
		if (partition_idx == overflow_idx) {
			groups.push_back(make_uniq<OverflowSortGroup>(*this, context, std::move(partition)));
		} else {
			groups.push_back(make_uniq<DirectPartitionGroup>(partitioned_copy, std::move(partition)));
		}
	}
	return groups;
}

idx_t DirectPartitionGroup::OmittedHeapBytes() const {
	const auto &op = partitioned_copy.op;
	if (!op.batch_size_bytes.IsValid() || op.write_partition_columns) {
		return 0;
	}
	vector<column_t> omitted_columns;
	const auto &variable_columns = data->GetLayout().GetVariableColumns();
	for (const auto &partition_column : op.partition_columns) {
		const auto column_id = column_ids[partition_column];
		if (std::find(variable_columns.begin(), variable_columns.end(), column_id) != variable_columns.end()) {
			omitted_columns.push_back(column_id);
		}
	}
	if (omitted_columns.empty()) {
		return 0;
	}

	// Every row has the same partition key. Gather its variable columns once.
	TupleDataChunkIterator iterator(*data, TupleDataPinProperties::UNPIN_AFTER_DONE, {0, 0}, true);
	DataChunk key_chunk;
	data->InitializeChunk(key_chunk, omitted_columns);
	auto &chunk_state = iterator.GetChunkState();
	const auto &selection = *FlatVector::IncrementalSelectionVector();
	for (idx_t i = 0; i < omitted_columns.size(); i++) {
		data->Gather(chunk_state.row_locations, selection, 1, omitted_columns[i], key_chunk.data[i], selection,
		             nullptr);
	}
	key_chunk.SetChildCardinality(1);
	CopyBatchSlicer slicer(key_chunk.GetTypes(), op.batch_size_bytes);
	return slicer.ComputeRowSizes(key_chunk).Values<idx_t>()[0].GetValue() - slicer.GetFixedRowBytes();
}

void DirectPartitionGroup::Cursor::RowSizes(vector<idx_t> &sizes) {
	const auto chunk_rows = cursor.Count();
	sizes.assign(chunk_rows, fixed_row_bytes);
	if (!has_heap) {
		return;
	}
	TupleDataChunkIterator iterator(data, TupleDataPinProperties::UNPIN_AFTER_DONE, cursor.Position(), true);
	if (iterator.GetCurrentChunkCount() != chunk_rows) {
		throw InternalException("Partitioned copy could not read a chunk to size its rows");
	}
	const auto heap_sizes = iterator.GetHeapSizes();
	for (idx_t row = 0; row < chunk_rows; row++) {
		sizes[row] += heap_sizes[row] - MinValue(heap_sizes[row], omitted_heap_bytes);
	}
}

unique_ptr<PartitionedCopyGroupScanState>
DirectPartitionGroup::InitializeScan(const PartitionedCopyChunkPosition &position) const {
	auto result = make_uniq<ScanState>();
	data->InitializeScan(result->state, column_ids, TupleDataPinProperties::UNPIN_AFTER_DONE);
	data->SeekScan(result->state, {position.segment_idx, position.chunk_idx});
	return std::move(result);
}

bool DirectPartitionGroup::Scan(PartitionedCopyGroupScanState &state_p, DataChunk &chunk) const {
	auto &state = state_p.Cast<ScanState>();
	return data->Scan(state.state, chunk);
}

OverflowSortGroup::OverflowSortGroup(const IntegerPartitionInput &input, ClientContext &context,
                                     unique_ptr<TupleDataCollection> overflow_p)
    : overflow(std::move(overflow_p)), count(overflow->Count()), sort_input(input.CreateOverflowInput(context, count)),
      sort_state(sort_input->GetGlobalState(context)), resunk(0) {
	TupleDataChunkCursor cursor(*overflow);
	while (cursor.Next()) {
		chunks.push_back(cursor.Position());
	}
}

void OverflowSortGroup::ExecutePreparation(ExecutionContext &context, InterruptState &interrupt, idx_t step,
                                           idx_t begin_block, idx_t end_block) {
	if (step != RESINK) {
		sorted->ExecutePreparation(context, interrupt, step - 1, begin_block, end_block);
		return;
	}
	Resink(context, interrupt, begin_block, end_block);
	// the last block range to finish finalizes the sort input
	const auto blocks = PartitionedCopyHashGroup::BinValue<idx_t>(count, STANDARD_VECTOR_SIZE);
	const auto resunk_blocks = resunk += end_block - begin_block;
	if (resunk_blocks >= blocks) {
		FinalizeSort(context.client, interrupt);
	}
}

void OverflowSortGroup::Resink(ExecutionContext &context, InterruptState &interrupt, idx_t begin_block,
                               idx_t end_block) {
	// the block range maps to a share of the overflow chunks
	const auto blocks = PartitionedCopyHashGroup::BinValue<idx_t>(count, STANDARD_VECTOR_SIZE);
	const auto chunk_begin = chunks.size() * begin_block / blocks;
	const auto chunk_end = MinValue<idx_t>(chunks.size() * end_block / blocks, chunks.size());
	if (chunk_begin >= chunk_end) {
		return;
	}

	ParallelHyperLogLogLocalState unused_hll;
	auto lstate = sort_input->GetLocalState(context, *sort_state, unused_hll);

	TupleDataScanState scan_state;
	overflow->InitializeScan(scan_state);
	overflow->SeekScan(scan_state, chunks[chunk_begin]);
	DataChunk chunk;
	overflow->InitializeScanChunk(scan_state, chunk);
	for (idx_t chunk_idx = chunk_begin; chunk_idx < chunk_end; chunk_idx++) {
		if (!overflow->Scan(scan_state, chunk)) {
			throw InternalException("Partitioned copy overflow has fewer chunks than expected");
		}
		sort_input->Sink(context, chunk, *sort_state, *lstate, interrupt);
	}
	sort_input->Combine(context, *sort_state, *lstate, interrupt);
}

void OverflowSortGroup::FinalizeSort(ClientContext &context, InterruptState &interrupt) {
	lock_guard<mutex> guard(sorted_lock);
	if (sorted) {
		return;
	}
	// the rows now live in the sort input
	overflow.reset();
	auto &gstate = sort_state->Cast<SortStrategyInputGlobalState>();
	idx_t total_count;
	auto bins = sort_input->FinalizeBins(context, gstate, interrupt, total_count);
	if (total_count != count) {
		throw InternalException("Partitioned copy re-sunk %llu overflow rows, expected %llu", total_count, count);
	}
	sorted = make_uniq<SortedBinGroup>(*sort_input, gstate, std::move(bins), count);
}

void OverflowSortGroup::FinishPreparation(idx_t step) {
	if (step == RESINK) {
		if (!sorted) {
			throw InternalException("Partitioned copy did not finalize its overflow sort");
		}
		return;
	}
	sorted->FinishPreparation(step - 1);
}

// defined after DirectPartitionGroup, which single-key bins are read through
vector<unique_ptr<PartitionedCopyGroup>> SortStrategyInput::Finalize(ClientContext &context,
                                                                     PartitionedCopyInputGlobalState &gstate_p,
                                                                     InterruptState &interrupt) const {
	auto &gstate = gstate_p.Cast<SortStrategyInputGlobalState>();
	idx_t total_count;
	const auto bins = FinalizeBins(context, gstate, interrupt, total_count);
	const auto &chunk_rows = strategy->GetHashGroups(*gstate.source);

	vector<unique_ptr<PartitionedCopyGroup>> groups(chunk_rows.size());
	for (const auto &bin : bins) {
		// a bin proven to hold a single key is read straight from its tuple data, like a direct partition
		auto single_key = strategy->TryTakeSinglePartition(bin, *gstate.source);
		if (single_key) {
			groups[bin] = make_uniq<DirectPartitionGroup>(partitioned_copy, std::move(single_key), strategy->scan_ids);
		} else {
			groups[bin] = make_uniq<SortedBinGroup>(*this, gstate, vector<idx_t> {bin}, chunk_rows[bin].count);
		}
	}
	return groups;
}

//===--------------------------------------------------------------------===//
// Partitioned Copy Hash Group
//===--------------------------------------------------------------------===//
PartitionedCopyHashGroup::PartitionedCopyHashGroup(PartitionedCopy &partitioned_copy,
                                                   unique_ptr<PartitionedCopyGroup> group_p, idx_t group_idx_p)
    : partitioned_copy(partitioned_copy), group(std::move(group_p)), count(group->Count()),
      blocks(BinValue<idx_t>(count, STANDARD_VECTOR_SIZE)), group_idx(group_idx_p),
      stage(group->PreparationStepCount() ? PartitionedCopyStage::PREPARE_INPUT : PartitionedCopyStage::PLAN),
      prepared_blocks(0), planned(false), batched(0), prepared(0), flushed(0) {
}

PartitionedCopyStage PartitionedCopyHashGroup::GetStage() const {
	return stage;
}

idx_t PartitionedCopyHashGroup::InitTasks(idx_t per_thread_p) {
	per_thread = per_thread_p;
	group_threads = BinValue(blocks, per_thread);
	return group_threads * group->PreparationStepCount();
}

bool PartitionedCopyHashGroup::TryPrepareNextStage() {
	switch (stage.load()) {
	case PartitionedCopyStage::PREPARE_INPUT:
		if (prepared_blocks < blocks) {
			return false;
		}
		group->FinishPreparation(preparation_step);
		++preparation_step;
		prepared_blocks = 0;
		next_task = 0;
		if (preparation_step >= group->PreparationStepCount()) {
			stage = PartitionedCopyStage::PLAN;
		}
		return true;
	case PartitionedCopyStage::PLAN:
		if (planned) {
			stage = PartitionedCopyStage::BATCH;
			return true;
		}
		return false;
	case PartitionedCopyStage::BATCH:
		if (batched == count) {
			// every batch has been assembled and every scan has ended: the group and its rows go
			group.reset();
			PrepareBatchStates();
			stage = PartitionedCopyStage::PREPARE;
			return true;
		}
		return false;
	case PartitionedCopyStage::PREPARE:
		if (prepared == count && PreparedBatchesReady()) {
			CompletePreparedBatchStates();
			stage = PartitionedCopyStage::FLUSH;
			return true;
		}
		return false;
	case PartitionedCopyStage::FLUSH:
		if (flushed == batch_states.size()) {
			stage = PartitionedCopyStage::DONE;
			return true;
		}
		return false;
	case PartitionedCopyStage::DONE:
	default:
		return true;
	}
}

optional<PartitionedCopyTask> PartitionedCopyHashGroup::TryNextTask() {
	PartitionedCopyTask task;
	task.group_idx = group_idx;
	switch (GetStage()) {
	case PartitionedCopyStage::PREPARE_INPUT:
		if (next_task >= group_threads) {
			return nullopt;
		}
		task.stage = PartitionedCopyStage::PREPARE_INPUT;
		task.step = preparation_step;
		task.thread_idx = next_task;
		task.begin_idx = next_task * per_thread;
		task.end_idx = MinValue<idx_t>(task.begin_idx + per_thread, blocks);
		++next_task;
		return task;
	case PartitionedCopyStage::PLAN:
		if (plan_started) {
			return nullopt;
		}
		plan_started = true;
		task.stage = PartitionedCopyStage::PLAN;
		return task;
	case PartitionedCopyStage::BATCH:
		if (next_batch >= planned_batches.size()) {
			return nullopt;
		}
		task.stage = PartitionedCopyStage::BATCH;
		task.batch_idx = next_batch++;
		return task;
	case PartitionedCopyStage::PREPARE:
		return TryNextPrepareTask();
	case PartitionedCopyStage::FLUSH:
		return TryNextFlushTask();
	default:
		return nullopt;
	}
}

optional<PartitionedCopyTask> PartitionedCopyHashGroup::TryNextPrepareTask() {
	while (prepare_partition_idx < batch_states.size()) {
		auto &batch_state = *batch_states[prepare_partition_idx];
		auto action = batch_state.SelectPrepareTask(prepare_batch_idx);
		if (action.type == PartitionedCopyPrepareTaskActionType::SKIP_PARTITION) {
			++prepare_partition_idx;
			prepare_batch_idx = 0;
			continue;
		}
		if (action.type == PartitionedCopyPrepareTaskActionType::WAIT_FOR_WRITE_INFO) {
			return nullopt;
		}

		PartitionedCopyTask task;
		task.stage = PartitionedCopyStage::PREPARE;
		task.group_idx = group_idx;
		task.step = prepare_partition_idx;
		task.batch_idx = action.type == PartitionedCopyPrepareTaskActionType::ACQUIRE_WRITE_INFO
		                     ? DConstants::INVALID_INDEX
		                     : action.batch_idx;

		return task;
	}

	return nullopt;
}

optional<PartitionedCopyTask> PartitionedCopyHashGroup::TryNextFlushTask() {
	if (flush_partition_idx == batch_states.size()) {
		return nullopt;
	}

	PartitionedCopyTask task;
	task.stage = PartitionedCopyStage::FLUSH;
	task.group_idx = group_idx;
	task.step = flush_partition_idx++;

	return task;
}

bool PartitionedCopyHashGroup::PreparedBatchesReady() const {
	for (auto &batch_state_ptr : batch_states) {
		if (!batch_state_ptr) {
			return false;
		}
		auto &batch_state = *batch_state_ptr;
		if (!batch_state.ReadyForFlush()) {
			return false;
		}
	}
	return true;
}

void PartitionedCopyHashGroup::PrepareInput(ExecutionContext &context, InterruptState &interrupt,
                                            const PartitionedCopyTask &task) {
	D_ASSERT(task.stage == PartitionedCopyStage::PREPARE_INPUT);
	group->ExecutePreparation(context, interrupt, task.step, task.begin_idx, task.end_idx);
	prepared_blocks += task.end_idx - task.begin_idx;
}

idx_t PartitionedCopyHashGroup::NewBatchState() {
	annotated_lock_guard<annotated_mutex> guard(lock);
	batch_states.push_back(make_uniq<PartitionedCopyBatchState>());
	return batch_states.size() - 1;
}

void PartitionedCopyHashGroup::AddPlannedBatch(idx_t batch_state_idx, PartitionedCopyRowPosition begin,
                                               idx_t row_count) {
	annotated_lock_guard<annotated_mutex> guard(lock);
	auto &batch_state = *batch_states[batch_state_idx];
	PartitionedCopyPlannedBatch batch;
	batch.batch_state_idx = batch_state_idx;
	batch.batch_idx = batch_state.AddCollectionSlot(partitioned_copy.GetPartitionCollectionSchema(), row_count);
	batch.begin = begin;
	batch.row_count = row_count;
	planned_batches.push_back(batch);
}

//! Plans the batches of the group: one partition per batch, split by the row and byte limits of the COPY. Cached
//! written-column estimates are refined near byte boundaries. Fixed-width rows need no scan. Both limits go through
//! the batch analyzer: like the Parquet writer and the delayed partition path, a batch
//! is closed once it reaches a limit, so it may exceed the row limit by less than one input vector.
void PartitionedCopyHashGroup::Plan() {
	const auto &op = partitioned_copy.op;
	const auto &batch_size = op.batch_size;
	const auto &batch_size_bytes = op.batch_size_bytes;

	// the current batch
	idx_t batch_state_idx = DConstants::INVALID_INDEX;
	PartitionedCopyRowPosition begin;
	bool begin_at_next_chunk = true;
	idx_t batch_rows = 0;
	idx_t batch_bytes = 0;

	auto limit_reached = [&]() {
		const CopyFunctionBatchAnalyzer analyzer(batch_rows, batch_bytes, batch_size, batch_size_bytes);
		return analyzer.MeetsFlushCriteria();
	};
	auto close_batch = [&](const PartitionedCopyChunkPosition &position, idx_t row, idx_t chunk_rows) {
		if (batch_rows) {
			AddPlannedBatch(batch_state_idx, begin, batch_rows);
		}
		batch_rows = 0;
		batch_bytes = 0;
		// the next batch starts here, or at the first row of the next chunk
		begin = {position, row};
		begin_at_next_chunk = row == chunk_rows;
	};

	vector<idx_t> boundaries;
	vector<idx_t> row_sizes;
	CopyBatchSlicer slicer(partitioned_copy.write_types, batch_size_bytes);
	const auto fixed_row_size = slicer.FixedRowSize();
	auto cursor = group->InitializeCursor(slicer);
	while (cursor->Next()) {
		const auto position = cursor->Position();
		const auto chunk_rows = cursor->Count();
		if (!chunk_rows) {
			continue;
		}
		if (begin_at_next_chunk) {
			begin = {position, 0};
			begin_at_next_chunk = false;
		}
		const auto chunk_bytes = fixed_row_size.IsValid()     ? chunk_rows * fixed_row_size.GetIndex()
		                         : batch_size_bytes.IsValid() ? cursor->ByteEstimate()
		                                                      : 0;
		boundaries.clear();
		cursor->PartitionBoundaries(boundaries);
		boundaries.push_back(chunk_rows);
		bool sized = false;

		idx_t row = 0;
		for (const auto &boundary : boundaries) {
			if (batch_state_idx == DConstants::INVALID_INDEX) {
				// the first partition
				batch_state_idx = NewBatchState();
			}
			// the run [row, boundary) belongs to the current partition
			while (row < boundary) {
				idx_t run_rows = boundary - row;
				if (!sized && batch_size_bytes.IsValid() && run_rows > 1 &&
				    batch_bytes + chunk_bytes * run_rows / chunk_rows >= batch_size_bytes.GetIndex()) {
					// The run would cross the byte limit. Refine the remaining rows of this chunk.
					if (fixed_row_size.IsValid()) {
						row_sizes.assign(chunk_rows, fixed_row_size.GetIndex());
					} else {
						cursor->RowSizes(row_sizes);
						if (row_sizes.size() != chunk_rows) {
							throw InternalException("Partitioned copy sized %llu rows of a chunk of %llu rows",
							                        row_sizes.size(), chunk_rows);
						}
					}
					sized = true;
				}
				if (sized) {
					// take rows by their refined size until a limit is reached
					while (run_rows) {
						batch_bytes += row_sizes[row++];
						++batch_rows;
						--run_rows;
						if (limit_reached()) {
							break;
						}
					}
				} else {
					batch_rows += run_rows;
					batch_bytes += chunk_bytes * run_rows / chunk_rows;
					row += run_rows;
				}
				if (limit_reached()) {
					close_batch(position, row, chunk_rows);
				}
			}
			if (boundary < chunk_rows) {
				// a new partition starts here
				close_batch(position, row, chunk_rows);
				batch_state_idx = NewBatchState();
			}
		}
	}
	if (batch_rows) {
		AddPlannedBatch(batch_state_idx, begin, batch_rows);
	}
	planned = true;
}

void PartitionedCopyHashGroup::Batch(const PartitionedCopyTask &task) {
	D_ASSERT(task.stage == PartitionedCopyStage::BATCH);
	PartitionedCopyPlannedBatch plan;
	{
		annotated_lock_guard<annotated_mutex> guard(lock);
		plan = planned_batches[task.batch_idx];
	}

	const auto collection_schema = partitioned_copy.GetPartitionCollectionSchema();
	const auto &batch_types = partitioned_copy.GetPartitionCollectionTypes(collection_schema);
	const auto &batch_columns = collection_schema == PartitionedCopyCollectionSchema::RAW_SCHEMA
	                                ? partitioned_copy.raw_columns
	                                : partitioned_copy.write_columns;

	DataChunk batch_chunk;
	batch_chunk.Initialize(partitioned_copy.context, batch_types);
	auto batch = make_uniq<ColumnDataCollection>(partitioned_copy.context, batch_types);
	ColumnDataAppendState append_state;
	batch->InitializeAppend(append_state);
	vector<Value> values;
	{
		// the scan and its pins end before the batch is published: once every batch is, the group is destroyed
		auto scan_state = group->InitializeScan(plan.begin.chunk);
		DataChunk scan_chunk;
		group->InitializeScanChunk(scan_chunk);

		idx_t from = plan.begin.row_idx;
		idx_t remaining = plan.row_count;
		while (remaining) {
			if (!group->Scan(*scan_state, scan_chunk)) {
				throw InternalException("Partitioned copy group has fewer rows than planned");
			}
			const auto to = MinValue<idx_t>(scan_chunk.size(), from + remaining);
			if (to > from) {
				scan_chunk.Slice(from, to - from);
				if (values.empty()) {
					for (const auto &pc : partitioned_copy.op.partition_columns) {
						values.push_back(scan_chunk.GetValue(pc, 0));
					}
				}
				batch_chunk.ReferenceColumns(scan_chunk, batch_columns);
				batch->Append(append_state, batch_chunk);
				remaining -= to - from;
			}
			from = 0;
		}
	}

	StoreBatch(plan.batch_state_idx, plan.batch_idx, std::move(batch), std::move(values), plan.row_count);
}

idx_t PartitionedCopyHashGroup::StoreBatch(idx_t batch_state_idx, idx_t batch_idx,
                                           unique_ptr<ColumnDataCollection> batch, vector<Value> values,
                                           const idx_t row_count) {
	const auto collection_schema = partitioned_copy.GetPartitionCollectionSchema();
	optional_ptr<PartitionWriteInfo> write_info;
	const auto flush_threshold = Settings::Get<PartitionedWriteFlushThresholdSetting>(partitioned_copy.context);
	const auto has_delayed_partition = partitioned_copy.HasDelayedPartition(values);
	PartitionedCopyBatchAction batch_action;
	{
		annotated_lock_guard<annotated_mutex> guard(lock);
		auto &batch_state = *batch_states[batch_state_idx];
		batch_action = batch_state.RegisterBatch(std::move(values), flush_threshold, has_delayed_partition);
	}

	if (batch_action.type == PartitionedCopyBatchActionType::STORE_COLLECTION) {
		{
			annotated_lock_guard<annotated_mutex> guard(lock);
			auto &current_batch_state = *batch_states[batch_state_idx];
			current_batch_state.StoreCollection(batch_idx, std::move(batch));
		}
		return batched += row_count;
	}

	if (batch_action.type == PartitionedCopyBatchActionType::ACQUIRE_AND_PREPARE) {
		auto write_lease = partitioned_copy.partition_writes.Acquire(batch_action.values);
		{
			annotated_lock_guard<annotated_mutex> guard(lock);
			auto &current_batch_state = *batch_states[batch_state_idx];
			write_info = current_batch_state.CompleteWriteInfoAcquisition(std::move(write_lease));
		}
	} else {
		D_ASSERT(batch_action.type == PartitionedCopyBatchActionType::PREPARE_WITH_WRITE_INFO);
		write_info = batch_action.write_info;
	}
	D_ASSERT(write_info);
	auto prepared_batch = partitioned_copy.PreparePartitionBatch(
	    batch_action.values, *write_info, PartitionedCopyCollection(collection_schema, std::move(batch)));

	{
		annotated_lock_guard<annotated_mutex> guard(lock);
		auto &current_batch_state = *batch_states[batch_state_idx];
		current_batch_state.StorePreparedBatch(batch_idx, std::move(prepared_batch));
	}

	prepared += row_count;
	return batched += row_count;
}

void PartitionedCopyHashGroup::PrepareBatchStates() {
	const auto flush_threshold = Settings::Get<PartitionedWriteFlushThresholdSetting>(partitioned_copy.context);
	for (auto &batch_state : batch_states) {
		D_ASSERT(batch_state);
		const auto has_delayed_partition = partitioned_copy.HasDelayedPartition(batch_state->Values());
		prepared += batch_state->FinalizeBatching(flush_threshold, has_delayed_partition);
	}
}

void PartitionedCopyHashGroup::CompletePreparedBatchStates() {
	for (auto &batch_state : batch_states) {
		D_ASSERT(batch_state);
		batch_state->MarkPrepared();
	}
}

void PartitionedCopyHashGroup::Prepare(ExecutionContext &execution_context, InterruptState &interrupt_state,
                                       const PartitionedCopyTask &task) {
	D_ASSERT(task.stage == PartitionedCopyStage::PREPARE);

	PartitionedCopyPrepareAction prepare_action;
	{
		annotated_lock_guard<annotated_mutex> guard(lock);
		auto &batch_state = *batch_states[task.step];
		prepare_action = batch_state.BeginPrepareTask(task.batch_idx);
	}

	if (prepare_action.type == PartitionedCopyPrepareActionType::ACQUIRE_WRITE_INFO) {
		auto write_lease = partitioned_copy.partition_writes.Acquire(prepare_action.values);
		annotated_lock_guard<annotated_mutex> guard(lock);
		auto &batch_state = *batch_states[task.step];
		batch_state.CompleteWriteInfoAcquisition(std::move(write_lease));
		return;
	}
	D_ASSERT(prepare_action.type == PartitionedCopyPrepareActionType::PREPARE_BATCH);
	D_ASSERT(prepare_action.data.collection);
	const auto row_count = prepare_action.data.Count();

	auto prepared_batch = partitioned_copy.PreparePartitionBatch(prepare_action.values, *prepare_action.write_info,
	                                                             std::move(prepare_action.data));

	{
		annotated_lock_guard<annotated_mutex> guard(lock);
		auto &batch_state = *batch_states[task.step];
		batch_state.StorePreparedBatch(task.batch_idx, std::move(prepared_batch));
	}

	prepared += row_count;
}

void PartitionedCopyHashGroup::Flush(ExecutionContext &execution_context, InterruptState &interrupt_state,
                                     const PartitionedCopyTask &task) {
	D_ASSERT(task.stage == PartitionedCopyStage::FLUSH);

	PartitionedCopyFlushAction flush_action;
	{
		annotated_lock_guard<annotated_mutex> guard(lock);
		auto &batch_state = *batch_states[task.step];
		flush_action = batch_state.TakeFlushAction();
	}
	D_ASSERT(flush_action.values.size() == partitioned_copy.op.partition_columns.size());

	if (flush_action.type == PartitionedCopyFlushActionType::DELAYED_COLLECTIONS) {
		const auto collection_schema = partitioned_copy.GetPartitionCollectionSchema();
		const auto &collection_types = partitioned_copy.GetPartitionCollectionTypes(collection_schema);
		auto collection = make_uniq<ColumnDataCollection>(partitioned_copy.context, collection_types);
		for (auto &batch : flush_action.collections) {
			D_ASSERT(batch.schema == collection_schema);
			if (batch.collection) {
				collection->Combine(*batch.collection);
			}
		}
		if (collection->Count() == 0) {
			return;
		}

		auto data = PartitionedCopyCollection(collection_schema, std::move(collection));
		auto ready_partition = partitioned_copy.BufferOrTakeReadyPartition(flush_action.values, std::move(data), false);
		if (ready_partition) {
			partitioned_copy.FlushPartitionCollection(execution_context, interrupt_state, std::move(*ready_partition));
		}
		return;
	}

	D_ASSERT(flush_action.type == PartitionedCopyFlushActionType::PREPARED_BATCHES);
	D_ASSERT(flush_action.write_lease);
	partitioned_copy.FlushPreparedPartitionRun(flush_action.values, *flush_action.write_lease,
	                                           std::move(flush_action.batches));
	flush_action.write_lease.Reset();
	partitioned_copy.TryFinalizePartitionWrite(flush_action.values);
}

//===--------------------------------------------------------------------===//
// Partitioned Copy State
//===--------------------------------------------------------------------===//
PartitionedCopyState::PartitionedCopyState(PartitionedCopy &partitioned_copy_p,
                                           unique_ptr<PartitionedCopyInputGlobalState> input_state_p)
    : partitioned_copy(partitioned_copy_p),
      next_flush_check(Settings::Get<PartitionedWriteFlushThresholdSetting>(partitioned_copy.context)),
      input_state(std::move(input_state_p)), total_blocks(0), next_group(0), locals(0), combined(0) {
}

bool PartitionedCopyState::ShouldInitiateFlush(const idx_t &local_append_count) {
	auto expected = next_flush_check.load(std::memory_order_relaxed);
	if (local_append_count < expected) {
		return false; // Not enough rows accumulated yet
	}

	// CAS so only one thread increments "next_flush_check"
	const auto flush_threshold = Settings::Get<PartitionedWriteFlushThresholdSetting>(partitioned_copy.context);
	const auto desired = expected + flush_threshold;
	const auto exchanged = next_flush_check.compare_exchange_strong(expected, desired, std::memory_order_relaxed,
	                                                                std::memory_order_relaxed);
	if (!exchanged) {
		return false; // Another thread beat us to it
	}
	if (partitioned_copy.op.partition_columns.empty()) {
		return true; // The implicit partition is always dense enough to flush
	}

	// Get counts from the HLL states
	const auto merged_state = hll.GetMergedState();
	auto [unique_count, total_count] = merged_state->GetCounts();
	unique_count = MaxValue<idx_t>(unique_count, 1);

	// If we have enough rows per partition on average, we can start flushing
	return total_count / unique_count >= 2 * flush_threshold;
}

bool PartitionedCopyState::IsCombineComplete() const {
	return combined == locals;
}

void PartitionedCopyState::CreateTaskList(vector<unique_ptr<PartitionedCopyGroup>> groups) {
	hash_groups.resize(groups.size());
	for (idx_t group_idx = 0; group_idx < groups.size(); ++group_idx) {
		auto &group = groups[group_idx];
		if (!group || !group->Count()) {
			continue;
		}
		auto hash_group = make_uniq<PartitionedCopyHashGroup>(partitioned_copy, std::move(group), group_idx);
		total_blocks += hash_group->blocks;
		hash_groups[group_idx] = std::move(hash_group);
	}

	for (idx_t group_idx = 0; group_idx < hash_groups.size(); ++group_idx) {
		auto &hash_group = hash_groups[group_idx];
		if (!hash_group) {
			continue;
		}
		partition_blocks.emplace_back(hash_group->blocks, group_idx);
	}
	if (partition_blocks.empty()) {
		groups_ready = true;
		return;
	}
	std::sort(partition_blocks.begin(), partition_blocks.end(), std::greater<PartitionBlock>());

	auto &ts = TaskScheduler::GetScheduler(partitioned_copy.context);
	const auto &max_block = partition_blocks.front();

	const auto threads = MinValue<idx_t>(locals, ts.NumberOfThreads());
	const auto aligned_scale = MaxValue<idx_t>(ValidityMask::BITS_PER_VALUE / STANDARD_VECTOR_SIZE, 1);
	const auto aligned_count = PartitionedCopyHashGroup::BinValue(max_block.first, aligned_scale);
	// Split the largest group over the threads, but with many small groups a task per group keeps the threads on
	// different groups instead of queueing on one group's locks
	const auto per_group = PartitionedCopyHashGroup::BinValue(aligned_count, threads);
	const auto aligned_total = PartitionedCopyHashGroup::BinValue(total_blocks, aligned_scale);
	const auto per_total = PartitionedCopyHashGroup::BinValue(aligned_total, threads * 4);
	const auto per_thread = aligned_scale * MaxValue<idx_t>(per_group, per_total);
	if (!per_thread) {
		throw InternalException("No blocks per thread! %ld threads, %ld groups, %ld blocks, %ld hash group", threads,
		                        partition_blocks.size(), max_block.first, max_block.second);
	}

	for (const auto &b : partition_blocks) {
		hash_groups[b.second]->InitTasks(per_thread);
	}
	// readiness is published only now that every group has its tasks
	groups_ready = true;
}

bool PartitionedCopyState::HasCompleted() const {
	annotated_lock_guard<annotated_mutex> global_guard(lock);
	return active_groups.empty() && next_group == partition_blocks.size();
}

optional<PartitionedCopyTask> PartitionedCopyState::TryAssignTask() {
	annotated_lock_guard<annotated_mutex> global_guard(lock);
	for (const auto &group_idx : active_groups) {
		auto &hash_group = hash_groups[group_idx];
		annotated_lock_guard<annotated_mutex> group_guard(hash_group->lock);
		hash_group->TryPrepareNextStage();
		auto task = hash_group->TryNextTask();
		if (!task) {
			continue;
		}
		return task;
	}

	while (next_group < partition_blocks.size()) {
		const auto group_idx = partition_blocks[next_group++].second;
		active_groups.emplace_back(group_idx);
		auto &hash_group = hash_groups[group_idx];
		annotated_lock_guard<annotated_mutex> guard(hash_group->lock);
		hash_group->TryPrepareNextStage();
		auto task = hash_group->TryNextTask();
		if (!task) {
			continue;
		}

		return task;
	}

	return nullopt;
}

void PartitionedCopyState::ExecuteTask(ExecutionContext &execution_context, const PartitionedCopyTask &task,
                                       InterruptState &interrupt) {
	auto &hash_group = *hash_groups[task.group_idx];
	switch (task.stage) {
	case PartitionedCopyStage::PREPARE_INPUT:
		hash_group.PrepareInput(execution_context, interrupt, task);
		break;
	case PartitionedCopyStage::PLAN:
		hash_group.Plan();
		break;
	case PartitionedCopyStage::BATCH:
		hash_group.Batch(task);
		break;
	case PartitionedCopyStage::PREPARE:
		hash_group.Prepare(execution_context, interrupt, task);
		break;
	case PartitionedCopyStage::FLUSH:
		hash_group.Flush(execution_context, interrupt, task);
		break;
	default:
		throw InternalException("Invalid PartitionedCopyStage in PartitionedCopyState::ExecuteTask");
	}
	auto partitions_to_finalize = FinishTask(task);
	partitioned_copy.NotifyFlushProgress();
	for (const auto &values : partitions_to_finalize) {
		partitioned_copy.TryFinalizePartitionWrite(values);
	}
}

vector<vector<Value>> PartitionedCopyState::FinishTask(const PartitionedCopyTask &task) {
	const auto group_idx = task.group_idx;
	auto &finished_hash_group = hash_groups[group_idx];
	vector<vector<Value>> partitions_to_finalize;

	bool hash_group_completed = false;
	if (task.stage == PartitionedCopyStage::FLUSH) {
		annotated_lock_guard<annotated_mutex> group_guard(finished_hash_group->lock);
		hash_group_completed = ++finished_hash_group->flushed == finished_hash_group->batch_states.size();
		if (hash_group_completed) {
			partitions_to_finalize.reserve(finished_hash_group->batch_states.size());
			for (const auto &batch_state : finished_hash_group->batch_states) {
				D_ASSERT(batch_state);
				partitions_to_finalize.push_back(batch_state->Values());
			}
		}
	}

	if (hash_group_completed) {
		annotated_lock_guard<annotated_mutex> global_guard(lock);
		auto &v = active_groups;
		v.erase(std::remove(v.begin(), v.end(), group_idx), v.end());
		finished_hash_group.reset();
	}
	return partitions_to_finalize;
}

//===--------------------------------------------------------------------===//
// Partitioned Copy Lifecycle
//===--------------------------------------------------------------------===//
PartitionedCopy::PartitionedCopy(const PhysicalCopyToFile &op_p, ClientContext &context_p,
                                 CopyToFileGlobalState &copy_gstate_p)
    : op(op_p), context(context_p), copy_gstate(copy_gstate_p), partition_writes(op_p, context_p),
      input(ConstructInput()), flushing(false), locals(0), finalized(false) {
	unordered_set<idx_t> part_col_set(op.partition_columns.begin(), op.partition_columns.end());
	for (idx_t col_idx = 0; col_idx < op.expected_types.size(); col_idx++) {
		raw_columns.push_back(col_idx);
		if (op.write_partition_columns || part_col_set.find(col_idx) == part_col_set.end()) {
			write_columns.push_back(col_idx);
			write_types.push_back(op.expected_types[col_idx]);
		}
	}
	if (op.partition_path_expression) {
		partition_path = op.partition_path_expression->Copy();
	} else if (op.hive_file_pattern) {
		partition_path = CreateHivePartitionPath(context, op);
	}
}

unique_ptr<const PartitionedCopyInput> PartitionedCopy::ConstructInput() const {
	// A single integer partition key with known bounds is partitioned on directly: no hashing, no sorting
	if (op.partition_columns.size() == 1 && op.order_columns.empty() && op.partition_stats.size() == 1) {
		const auto key_col = op.partition_columns[0];
		Value min_value;
		idx_t value_count;
		if (IntegerPartitionedTupleData::TryBoundedKey(context, op.expected_types[key_col], op.partition_stats[0].get(),
		                                               min_value, value_count)) {
			return make_uniq<IntegerPartitionInput>(*this, key_col, std::move(min_value), value_count);
		}
	}

	vector<unique_ptr<Expression>> partition_bys;
	for (auto &col : op.partition_columns) {
		partition_bys.push_back(make_uniq<BoundReferenceExpression>(op.expected_types[col], col));
	}
	OperatorPartitionInfo unpartitioned_info;
	auto strategy =
	    SortStrategy::Factory(context, partition_bys, op.order_columns, op.expected_types, op.partition_stats,
	                          unpartitioned_info, op.children.empty() ? 0 : op.children[0].get().estimated_cardinality);
	return make_uniq<SortStrategyInput>(*this, std::move(strategy));
}

bool PartitionedCopy::RequiresSerializedPartitionWrites() const {
	// A full partition writer run must remain serialized when the run boundary has file-state semantics:
	// ORDER BY starts a fresh file per sorted run, and rotation can start a fresh file between batches.
	return !op.order_columns.empty() || op.Rotate();
}

void PartitionedCopy::InitializeFlush() {
	if (flushing) {
		return;
	}

	// Move current sinking state to flushing state
	flushing_state = std::move(sinking_state);
	if (!flushing_state) {
		return; // Nothing to do
	}

	flushing_state->CloseInput();
	flushing = true;
}

void PartitionedCopy::FinalizeState(PartitionedCopyState &state, InterruptState &interrupt_state) {
	D_ASSERT(state.combined == state.locals);
	// The flush finalizes a state once all producers have combined.
	D_ASSERT(!state.groups_ready);
	if (state.finalize_error) {
		// the first attempt took the rows out of the input: a retry would only see what it left behind
		std::rethrow_exception(state.finalize_error);
	}
	try {
		auto groups = input->Finalize(context, *state.input_state, interrupt_state);
		state.CreateTaskList(std::move(groups));
	} catch (...) {
		state.finalize_error = std::current_exception();
		throw;
	}
}

void PartitionedCopy::NotifyFlushProgress() {
	vector<InterruptState> to_wake;
	{
		annotated_lock_guard<annotated_mutex> guard(lock);
		++flush_progress;
		to_wake = TakeBlockedTasks();
	}
	// Waking outside the lock is safe: parking re-checks flush_progress under the lock
	for (auto &blocked_sink : to_wake) {
		blocked_sink.Callback();
	}
}

template <class FUNC>
bool PartitionedCopy::TryBlockOnFlush(ExecutionContext &execution_context, InterruptState &interrupt_state,
                                      idx_t observed_progress, FUNC &&can_continue) {
	context.InterruptCheck();
	Flush(execution_context, interrupt_state);
	if (!interrupt_state.CanCallback()) {
		copy_gstate.lifecycle_executor.WorkOnTaskOrYield();
		return false;
	}
	annotated_lock_guard<annotated_mutex> guard(lock);
	if (observed_progress != flush_progress || can_continue()) {
		return false;
	}
	return BlockTask(interrupt_state);
}

bool PartitionedCopy::TryPrepareSink(ExecutionContext &execution_context, PartitionedCopyLocalState &lstate,
                                     InterruptState &interrupt_state) {
	while (true) {
		if (lstate.current_state && lstate.current_state->CanSink()) {
			return true;
		}
		// A producer must combine into the active flush before it can wait for more input space.
		Combine(execution_context, lstate, interrupt_state, PartitionedCopyCombineType::DURING_SINK);

		idx_t observed_progress;
		bool joined_state = false;
		{
			annotated_lock_guard<annotated_mutex> guard(lock);
			if (!lstate.current_state && (!sinking_state || sinking_state->CanSink())) {
				if (!sinking_state) {
					sinking_state = make_shared_ptr<PartitionedCopyState>(*this, input->GetGlobalState(context));
				}
				lstate.current_state = sinking_state;
				annotated_lock_guard<annotated_mutex> state_guard(lstate.current_state->lock);
				lstate.current_state->locals++;
				joined_state = true;
			}
			observed_progress = flush_progress;
		}
		if (joined_state) {
			lstate.input_local_state = input->GetLocalState(execution_context, *lstate.current_state->input_state,
			                                                lstate.current_state->hll.GetLocalState());
			lstate.append_count = 0;
			return true;
		}

		auto can_continue = [&]() DUCKDB_REQUIRES(lock) {
			return !sinking_state || sinking_state->CanSink() ||
			       (lstate.current_state && flushing_state &&
			        RefersToSameObject(*lstate.current_state, *flushing_state));
		};
		if (TryBlockOnFlush(execution_context, interrupt_state, observed_progress, can_continue)) {
			return false;
		}
	}
}

SinkResultType PartitionedCopy::Sink(ExecutionContext &execution_context, DataChunk &chunk,
                                     PartitionedCopyLocalState &lstate, InterruptState &interrupt_state) {
	if (!TryPrepareSink(execution_context, lstate, interrupt_state)) {
		return SinkResultType::BLOCKED;
	}

	// Sink into the partition input
	input->Sink(execution_context, chunk, *lstate.current_state->input_state, *lstate.input_local_state,
	            interrupt_state);
	lstate.append_count += chunk.size();

	if (lstate.current_state->CanSink() && lstate.current_state->ShouldInitiateFlush(lstate.append_count)) {
		annotated_lock_guard<annotated_mutex> guard(lock);
		lstate.current_state->CloseInput();
		InitializeFlush();
	}

	if (!flushing.load(std::memory_order_relaxed)) {
		return SinkResultType::NEED_MORE_INPUT;
	}

	Combine(execution_context, lstate, interrupt_state, PartitionedCopyCombineType::DURING_SINK);
	Flush(execution_context, interrupt_state);
	return SinkResultType::NEED_MORE_INPUT;
}

void PartitionedCopy::Combine(ExecutionContext &execution_context, PartitionedCopyLocalState &lstate,
                              InterruptState &interrupt_state, PartitionedCopyCombineType combine_type) {
	if (!lstate.current_state) {
		return;
	}

	if (combine_type == PartitionedCopyCombineType::DURING_SINK) {
		annotated_lock_guard<annotated_mutex> guard(lock);
		if (!flushing_state || !RefersToSameObject(*lstate.current_state, *flushing_state)) {
			return; // Not flushing anymore, or can't combine this state into the flushing state
		}
	}

	input->Combine(execution_context, *lstate.current_state->input_state, *lstate.input_local_state, interrupt_state);

	{
		annotated_lock_guard<annotated_mutex> guard(lstate.current_state->lock);
		++lstate.current_state->combined;
	}

	// Reset local state
	lstate.input_local_state.reset();
	lstate.current_state.reset();
	NotifyFlushProgress();
}

SinkCombineResultType PartitionedCopy::FinishSink(ExecutionContext &execution_context,
                                                  PartitionedCopyLocalState &lstate, InterruptState &interrupt_state) {
	Combine(execution_context, lstate, interrupt_state, PartitionedCopyCombineType::DURING_PIPELINE_COMBINE);
	// Exhausted producers keep helping until the active flush finishes.
	while (true) {
		idx_t observed_progress;
		{
			annotated_lock_guard<annotated_mutex> guard(lock);
			if (!flushing) {
				return SinkCombineResultType::FINISHED;
			}
			observed_progress = flush_progress;
		}
		auto can_finish = [&]() DUCKDB_REQUIRES(lock) {
			return !flushing;
		};
		if (TryBlockOnFlush(execution_context, interrupt_state, observed_progress, can_finish)) {
			return SinkCombineResultType::BLOCKED;
		}
	}
}

class PartitionedCopyFinalizeTask : public ExecutorTask {
public:
	PartitionedCopyFinalizeTask(ClientContext &context, shared_ptr<Event> event_p, PartitionedCopy &partitioned_copy_p)
	    : ExecutorTask(context, std::move(event_p), partitioned_copy_p.op), partitioned_copy(partitioned_copy_p) {
	}

	TaskExecutionResult ExecuteTask(TaskExecutionMode mode) override {
		ExecutionContext execution_context(event->GetClientContext(), *thread_context,
		                                   event->Cast<BasePipelineEvent>().pipeline);
		InterruptState interrupt_state(shared_from_this());
		while (partitioned_copy.flushing.load(std::memory_order_relaxed) || partitioned_copy.HasDelayedPartitions()) {
			event->GetClientContext().InterruptCheck();
			if (partitioned_copy.flushing.load(std::memory_order_relaxed)) {
				partitioned_copy.Flush(execution_context, interrupt_state);
			} else {
				if (!partitioned_copy.DrainDelayedPartitions(execution_context, interrupt_state)) {
					TaskScheduler::YieldThread();
				}
			}
		}
		event->FinishTask();
		return TaskExecutionResult::TASK_FINISHED;
	}

	string TaskType() const override {
		return "PartitionedCopyFinalizeTask";
	}

private:
	PartitionedCopy &partitioned_copy;
};

class PartitionedCopyFinalizeEvent : public BasePipelineEvent {
public:
	PartitionedCopyFinalizeEvent(Pipeline &pipeline_p, PartitionedCopy &partitioned_copy_p)
	    : BasePipelineEvent(pipeline_p), partitioned_copy(partitioned_copy_p) {
	}

public:
	void Schedule() override {
		annotated_lock_guard<annotated_mutex> global_guard(partitioned_copy.lock);
		if (!partitioned_copy.flushing_state) {
			partitioned_copy.InitializeFlush();
		}
		idx_t task_count;
		if (partitioned_copy.flushing_state) {
			auto &flushing_state = *partitioned_copy.flushing_state;
			annotated_lock_guard<annotated_mutex> state_guard(flushing_state.lock);
			D_ASSERT(flushing_state.combined == flushing_state.locals);
			task_count = flushing_state.locals;
		} else {
			task_count = MaxValue<idx_t>(partitioned_copy.locals.load(std::memory_order_relaxed), 1);
		}

		vector<shared_ptr<Task>> tasks;
		for (idx_t i = 0; i < task_count; i++) {
			tasks.push_back(
			    make_shared_ptr<PartitionedCopyFinalizeTask>(GetClientContext(), shared_from_this(), partitioned_copy));
		}
		SetTasks(std::move(tasks));
	}

	void FinishEvent() override {
		bool done;
		{
			annotated_lock_guard<annotated_mutex> global_guard(partitioned_copy.lock);
			done = !partitioned_copy.sinking_state;
			if (!done) {
				auto partitioned_copy_finalize_event =
				    make_shared_ptr<PartitionedCopyFinalizeEvent>(*pipeline, partitioned_copy);
				InsertEvent(std::move(partitioned_copy_finalize_event));
			}
		}

		if (done) {
			partitioned_copy.FinalizeActiveWrites();
			partitioned_copy.copy_gstate.TryFinalizeOwnedFileState();
			partitioned_copy.copy_gstate.WaitForLifecycleTasks();
			partitioned_copy.copy_gstate.MarkSuccessful();
		}
	}

private:
	PartitionedCopy &partitioned_copy;
};

void PartitionedCopy::Finalize(Pipeline &pipeline, Event &event, InterruptState &interrupt_state) {
	bool should_finalize_writes;
	{
		annotated_lock_guard<annotated_mutex> global_guard(lock);
		finalized = true;
		if (!sinking_state && !flushing_state && !HasDelayedPartitions()) {
			should_finalize_writes = true;
		} else {
			should_finalize_writes = false;
			if (sinking_state) {
				sinking_state->CloseInput();
			}

			auto partitioned_copy_finalize_event = make_shared_ptr<PartitionedCopyFinalizeEvent>(pipeline, *this);
			event.InsertEvent(std::move(partitioned_copy_finalize_event));
		}
	}

	if (should_finalize_writes) {
		FinalizeActiveWrites();
		copy_gstate.TryFinalizeOwnedFileState();
		copy_gstate.WaitForLifecycleTasks();
		copy_gstate.MarkSuccessful();
	}
}

void PartitionedCopy::Flush(ExecutionContext &execution_context, InterruptState &interrupt_state) {
	shared_ptr<PartitionedCopyState> flushing_state_copy;
	{
		annotated_lock_guard<annotated_mutex> guard(lock);
		flushing_state_copy = flushing_state;
	}

	if (!flushing_state_copy) {
		return; // Finalization not yet complete, nothing to do
	}

	{
		annotated_lock_guard<annotated_mutex> guard(flushing_state_copy->lock);
		D_ASSERT(flushing_state_copy->combined <= flushing_state_copy->locals);
		if (!flushing_state_copy->groups_ready) {
			if (!flushing_state_copy->IsCombineComplete()) {
				return; // Combine not complete yet
			}
			D_ASSERT(flushing_state_copy->IsCombineComplete());
			FinalizeState(*flushing_state_copy, interrupt_state);
			if (!flushing_state_copy->groups_ready) {
				return; // Finalization not yet complete, nothing to do
			}
		}
		D_ASSERT(flushing_state_copy->groups_ready);
	}

	while (auto task = flushing_state_copy->TryAssignTask()) {
		flushing_state_copy->ExecuteTask(execution_context, *task, interrupt_state);
	}

	if (!flushing_state_copy->HasCompleted()) {
		return;
	}

	bool should_finalize_writes;
	{
		annotated_lock_guard<annotated_mutex> guard(lock);
		if (!flushing || !RefersToSameObject(*flushing_state, *flushing_state_copy)) {
			return;
		}
		flushing = false;
		flushing_state.reset();
		if (!finalized && sinking_state && !sinking_state->CanSink()) {
			InitializeFlush();
		}
		should_finalize_writes = finalized && !sinking_state && !flushing_state;
	}
	NotifyFlushProgress();
	if (should_finalize_writes) {
		DrainDelayedPartitions(execution_context, interrupt_state);
	}
}

//===--------------------------------------------------------------------===//
// Delayed Partition Buffering
//===--------------------------------------------------------------------===//
bool PartitionedCopy::HasDelayedPartition(const vector<Value> &values) {
	return delayed_partition_buffers.Has(values);
}

bool PartitionedCopy::HasDelayedPartitions() const {
	return !delayed_partition_buffers.Empty();
}

optional<DelayedPartitionFlush>
PartitionedCopy::BufferOrTakeReadyPartition(const vector<Value> &values, PartitionedCopyCollection data, bool force) {
	const auto flush_threshold = Settings::Get<PartitionedWriteFlushThresholdSetting>(context);
	return delayed_partition_buffers.BufferOrTakeReady(values, std::move(data), flush_threshold, force);
}

optional<DelayedPartitionFlush> PartitionedCopy::TakeNextDelayedPartition() {
	return delayed_partition_buffers.TakeNext();
}

optional<DelayedPartitionFlush> PartitionedCopy::CompleteDelayedPartition(const vector<Value> &values,
                                                                          const bool take_ready) {
	const auto flush_threshold = Settings::Get<PartitionedWriteFlushThresholdSetting>(context);
	return delayed_partition_buffers.Complete(values, flush_threshold, take_ready);
}

bool PartitionedCopy::DrainDelayedPartitions(ExecutionContext &execution_context, InterruptState &interrupt_state) {
	bool drained = false;
	while (auto partition = TakeNextDelayedPartition()) {
		drained = true;
		FlushPartitionCollection(execution_context, interrupt_state, std::move(*partition));
	}
	return drained;
}

//===--------------------------------------------------------------------===//
// Partitioned Collection Helpers
//===--------------------------------------------------------------------===//
unique_ptr<ColumnDataCollection> PartitionedCopy::SortPartitionCollection(ExecutionContext &execution_context,
                                                                          InterruptState &interrupt_state,
                                                                          unique_ptr<ColumnDataCollection> collection) {
	return input->SortCollection(execution_context, interrupt_state, std::move(collection));
}

PartitionedCopyCollectionSchema PartitionedCopy::GetPartitionCollectionSchema() const {
	return op.order_columns.empty() ? PartitionedCopyCollectionSchema::WRITE_SCHEMA
	                                : PartitionedCopyCollectionSchema::RAW_SCHEMA;
}

const vector<LogicalType> &PartitionedCopy::GetPartitionCollectionTypes(PartitionedCopyCollectionSchema schema) const {
	return schema == PartitionedCopyCollectionSchema::RAW_SCHEMA ? op.expected_types : write_types;
}

unique_ptr<ColumnDataCollection> PartitionedCopy::PrepareCollectionForWrite(PartitionedCopyCollection data) {
	D_ASSERT(data.collection);
	if (data.schema == PartitionedCopyCollectionSchema::WRITE_SCHEMA) {
		return std::move(data.collection);
	}
	return ProjectToWriteColumns(std::move(data.collection));
}

unique_ptr<ColumnDataCollection> PartitionedCopy::ProjectToWriteColumns(unique_ptr<ColumnDataCollection> collection) {
	D_ASSERT(collection);

	auto result = make_uniq<ColumnDataCollection>(context, write_types);
	ColumnDataAppendState append_state;
	result->InitializeAppend(append_state);

	ColumnDataScanState scan_state;
	collection->InitializeScan(scan_state);
	DataChunk scan_chunk;
	collection->InitializeScanChunk(scan_state, scan_chunk);

	DataChunk write_chunk;
	write_chunk.Initialize(context, write_types);

	while (collection->Scan(scan_state, scan_chunk)) {
		write_chunk.ReferenceColumns(scan_chunk, write_columns);
		result->Append(append_state, write_chunk);
	}

	return result;
}

//===--------------------------------------------------------------------===//
// Partition File Request Builder
//===--------------------------------------------------------------------===//
optional<PartitionFileRequest> PartitionFileRequestBuilder::Build() {
	auto &op = partitioned_copy.op;
	auto &context = partitioned_copy.context;
	// built before locking and reserving, as evaluating the partition path can throw
	auto directory = BuildDirectory(op.GetTrimmedPath(context, op.file_path));

	auto reservation_lock = partitioned_copy.partition_writes.LockForReservation();
	annotated_lock_guard<annotated_mutex> global_guard(partitioned_copy.copy_gstate.lock);
	if (file_state) {
		return nullopt;
	}
	if (op.partition_path_expression) {
		// claimed before reserving, which can take evicted file states that must not get lost when this throws
		partitioned_copy.partition_writes.ClaimDirectory(reservation_lock, values, directory.relative_path);
	}

	reservation = partitioned_copy.partition_writes.ReserveFileState(reservation_lock, values, reason);

	auto &fs = FileSystem::GetFileSystem(context);
	auto full_path = op.filename_pattern.CreateFilename(fs, directory.path, op.file_extension, reservation.offset);
	auto pending_file_state_open =
	    partitioned_copy.copy_gstate.CreatePartitionFileStateOpenLocked(file_state, std::move(full_path), values);
	D_ASSERT(pending_file_state_open);

	PartitionFileOpenRequest open_request(std::move(pending_file_state_open), std::move(directory), reservation.offset);
	return PartitionFileRequest(std::move(open_request), std::move(reservation.files_to_finalize));
}

vector<FileStateHandle> PartitionFileRequestBuilder::TakeFilesToFinalize() {
	return std::move(reservation.files_to_finalize);
}

PartitionDirectory PartitionFileRequestBuilder::BuildDirectory(string path) const {
	PartitionDirectory result;
	result.path = std::move(path);
	if (!partitioned_copy.partition_path) {
		return result;
	}
	auto &context = partitioned_copy.context;
	auto &fs = FileSystem::GetFileSystem(context);
	auto partition_path = EvaluatePartitionPath(context, partitioned_copy.op, *partitioned_copy.partition_path, values);
	// This is a relative path fragment, not a file to resolve through the VFS. Resolving it
	// would require the local filesystem even when the COPY target is remote.
	auto parsed_path = Path::FromString(partition_path);
	if (parsed_path.IsAbsolute() || FileSystem::IsRemoteFile(partition_path)) {
		throw InvalidInputException("PARTITION_PATH must be relative to the COPY target, but got \"%s\"",
		                            partition_path);
	}
	auto separator = fs.PathSeparator(result.path);
	if (!FileSystem::IsRemoteFile(result.path)) {
		partition_path = fs.ConvertSeparators(partition_path);
	}
	vector<string> components;
	for (auto &component : StringUtil::Split(partition_path, separator)) {
		if (component.empty() || component == ".") {
			continue;
		}
		if (component == "..") {
			throw InvalidInputException("PARTITION_PATH cannot contain \"..\", but got \"%s\"", partition_path);
		}
		components.push_back(component);
		result.path = fs.JoinPath(result.path, component);
		result.directories.push_back(result.path);
	}
	if (components.empty()) {
		// every partition needs a directory of its own
		throw InvalidInputException("PARTITION_PATH evaluated to an empty path for partition %s",
		                            PartitionValuesToString(partitioned_copy.op, values));
	}
	result.relative_path = StringUtil::Join(components, "/");
	return result;
}

//===--------------------------------------------------------------------===//
// Partitioned Write Helpers
//===--------------------------------------------------------------------===//
unique_ptr<PartitionedCopyBatch> PartitionedCopy::PreparePartitionBatch(const vector<Value> &values,
                                                                        PartitionWriteInfo &write_info,
                                                                        PartitionedCopyCollection data) {
	const auto create_file_state_fun = [&](FileStateHandle &file_state) {
		RequestPartitionFileState(file_state, values);
	};
	create_file_state_fun(write_info.file_state);
	auto collection = PrepareCollectionForWrite(std::move(data));
	auto [batch_analyzer, prepared_batch] =
	    op.PrepareBatch(context, copy_gstate, write_info.file_state, create_file_state_fun, std::move(collection));
	return make_uniq<PartitionedCopyBatch>(batch_analyzer, std::move(prepared_batch));
}

void PartitionedCopy::FlushPreparedPartitionRun(const vector<Value> &values, PartitionWriteInfo &write_info,
                                                vector<unique_ptr<PartitionedCopyBatch>> batches) {
	WithSerializedPartitionWriteRun(write_info, [&]() {
		EnsureFreshPartitionFileForSortedRun(write_info, values);
		for (auto &batch : batches) {
			if (!batch) {
				throw InternalException("Partitioned COPY reached FLUSH with a missing prepared batch");
			}
			EnsureFreshPartitionFileForRotation(write_info, values);
			FlushPreparedPartitionBatch(values, write_info, std::move(batch));
		}
	});
}

void PartitionedCopy::FlushPreparedPartitionBatch(const vector<Value> &values, PartitionWriteInfo &write_info,
                                                  unique_ptr<PartitionedCopyBatch> batch) {
	D_ASSERT(batch);
	const auto create_file_state_fun = [&](FileStateHandle &file_state) {
		RequestPartitionFileState(file_state, values);
	};
	op.FlushBatch(context, copy_gstate, write_info.file_state, create_file_state_fun, batch->batch_analyzer,
	              std::move(batch->prepared_batch));
}

void PartitionedCopy::FlushDelayedPartitionRun(const vector<Value> &values, PartitionWriteInfo &write_info,
                                               ColumnDataCollection &collection) {
	WithSerializedPartitionWriteRun(write_info, [&]() {
		EnsureFreshPartitionFileForSortedRun(write_info, values);

		DataChunk scan_chunk;
		ColumnDataScanState scan_state;
		collection.InitializeScan(scan_state);
		collection.InitializeScanChunk(scan_state, scan_chunk);

		auto batch = make_uniq<ColumnDataCollection>(context, write_types);
		ColumnDataAppendState append_state;
		batch->InitializeAppend(append_state);
		CopyBatchSlicer batch_slicer(write_types, op.batch_size_bytes);

		const auto flush_batch = [&]() {
			if (batch->Count() == 0) {
				return;
			}
			EnsureFreshPartitionFileForRotation(write_info, values);
			auto prepared_batch = PreparePartitionBatch(
			    values, write_info,
			    PartitionedCopyCollection(PartitionedCopyCollectionSchema::WRITE_SCHEMA, std::move(batch)));
			FlushPreparedPartitionBatch(values, write_info, std::move(prepared_batch));

			batch = make_uniq<ColumnDataCollection>(context, write_types);
			batch->InitializeAppend(append_state);
		};

		while (collection.Scan(scan_state, scan_chunk)) {
			idx_t offset = 0;
			while (offset < scan_chunk.size()) {
				auto &slice = batch_slicer.Slice(scan_chunk, offset, *batch);
				batch->Append(append_state, slice);
				const CopyFunctionBatchAnalyzer batch_analyzer(*batch, op.batch_size, op.batch_size_bytes);
				if (batch_analyzer.MeetsFlushCriteria()) {
					flush_batch();
				}
			}
		}
		flush_batch();
	});
}

void PartitionedCopy::FlushPartitionCollection(ExecutionContext &execution_context, InterruptState &interrupt_state,
                                               DelayedPartitionFlush flush) {
	while (true) {
		DelayedPartitionFlushGuard delayed_guard(*this, flush.values);
		if (flush.data.collection && flush.data.collection->Count() > 0) {
			if (flush.data.schema == PartitionedCopyCollectionSchema::RAW_SCHEMA) {
				flush.data.collection =
				    SortPartitionCollection(execution_context, interrupt_state, std::move(flush.data.collection));
				flush.data.collection = ProjectToWriteColumns(std::move(flush.data.collection));
				flush.data.schema = PartitionedCopyCollectionSchema::WRITE_SCHEMA;
			}
			D_ASSERT(flush.data.schema == PartitionedCopyCollectionSchema::WRITE_SCHEMA);

			auto write_lease = partition_writes.Acquire(flush.values);
			FlushDelayedPartitionRun(flush.values, *write_lease, *flush.data.collection);
			write_lease.Reset();
		}

		auto next = delayed_guard.Complete();
		if (!next) {
			TryFinalizePartitionWrite(flush.values);
			return;
		}
		flush = std::move(*next);
	}
}

void PartitionedCopy::RequestPartitionFileState(FileStateHandle &file_state, const vector<Value> &values,
                                                FileCreationReason reason) {
	PartitionFileRequestBuilder builder(*this, file_state, values, reason);
	optional<PartitionFileRequest> request;
	try {
		request = builder.Build();
	} catch (...) {
		auto error = std::current_exception();
		try {
			FinalizeFileStates(builder.TakeFilesToFinalize());
		} catch (...) {
		}
		std::rethrow_exception(error);
	}
	if (!request) {
		return;
	}

	auto open_job = request->OpenJob();
	try {
		FinalizeFileStates(std::move(request->files_to_finalize));
		copy_gstate.SchedulePartitionFileStateOpen(std::move(request->open_request));
	} catch (...) {
		if (open_job && !open_job->IsFinished()) {
			open_job->CompleteException(std::current_exception());
		}
		throw;
	}
}

void PartitionedCopy::EnsureFreshPartitionFileForSortedRun(PartitionWriteInfo &write_info,
                                                           const vector<Value> &values) {
	if (op.order_columns.empty()) {
		return;
	}
	EnsureFreshPartitionFile(write_info, values, FileCreationReason::SORTED_RUN_BOUNDARY);
}

void PartitionedCopy::EnsureFreshPartitionFileForRotation(PartitionWriteInfo &write_info, const vector<Value> &values) {
	if (!op.Rotate()) {
		return;
	}
	EnsureFreshPartitionFile(write_info, values, FileCreationReason::ROTATION);
}

void PartitionedCopy::EnsureFreshPartitionFile(PartitionWriteInfo &write_info, const vector<Value> &values,
                                               FileCreationReason reason) {
	D_ASSERT(reason == FileCreationReason::SORTED_RUN_BOUNDARY || reason == FileCreationReason::ROTATION);
	D_ASSERT(RequiresSerializedPartitionWrites());

	if (!write_info.file_state) {
		return;
	}
	auto &old_file_state_ref = copy_gstate.EnsureFileStateReady(
	    write_info.file_state, [&](FileStateHandle &file_state) { RequestPartitionFileState(file_state, values); });
	optional_ptr<GlobalFileState> old_file_state_ptr = old_file_state_ref;
	{
		annotated_lock_guard<annotated_mutex> global_guard(copy_gstate.lock);
		if (!write_info.file_state) {
			return;
		}
		D_ASSERT(RefersToSameObject(*old_file_state_ptr.get(), write_info.file_state.GetFileState()));
		annotated_lock_guard<annotated_mutex> file_guard(old_file_state_ptr->lock);
		if (reason == FileCreationReason::SORTED_RUN_BOUNDARY && old_file_state_ptr->num_batches == 0) {
			return;
		}
		if (reason == FileCreationReason::ROTATION && !PhysicalCopyRotateNow(op, *old_file_state_ptr)) {
			return;
		}
	}

	FileStateHandle new_file_state;
	RequestPartitionFileState(new_file_state, values, reason);

	FileStateHandle old_file_state;
	{
		annotated_lock_guard<annotated_mutex> global_guard(copy_gstate.lock);
		D_ASSERT(write_info.file_state);
		auto &current_file_state = write_info.file_state.GetFileState();
		D_ASSERT(RefersToSameObject(*old_file_state_ptr.get(), current_file_state));
		annotated_lock_guard<annotated_mutex> file_guard(current_file_state.lock);
		if (reason == FileCreationReason::SORTED_RUN_BOUNDARY) {
			D_ASSERT(current_file_state.num_batches > 0);
		} else {
			D_ASSERT(PhysicalCopyRotateNow(op, current_file_state));
		}

		old_file_state = std::move(write_info.file_state);
		write_info.file_state = std::move(new_file_state);
	}

	copy_gstate.FinalizeFileState(std::move(old_file_state));
}

void PartitionedCopy::TryFinalizePartitionWrite(const vector<Value> &values) {
	if (!finalized.load(std::memory_order_relaxed)) {
		return;
	}
	{
		annotated_lock_guard<annotated_mutex> guard(lock);
		if (sinking_state) {
			return;
		}
	}
	if (HasDelayedPartition(values)) {
		return;
	}

	auto file_state = partition_writes.TryTakeInactiveFileState(values);
	if (file_state) {
		copy_gstate.FinalizeFileState(std::move(file_state));
	}
}

void PartitionedCopy::FinalizeActiveWrites() {
	FinalizeFileStates(partition_writes.TakeOpenFileStates());
}

void PartitionedCopy::FinalizeFileStates(vector<FileStateHandle> files_to_finalize) {
	for (auto &file_state : files_to_finalize) {
		if (file_state) {
			copy_gstate.FinalizeFileState(std::move(file_state));
		}
	}
}

//===--------------------------------------------------------------------===//
// Copy Global State
//===--------------------------------------------------------------------===//
CopyToFileGlobalState::CopyToFileGlobalState(const PhysicalCopyToFile &op_p, ClientContext &context_p)
    : op(op_p), context(context_p), output_lifecycle(context_p), initialized(false), prepare_global_state(nullptr),
      create_file_state_fun([&](FileStateHandle &file_state) DUCKDB_EXCLUDES(lock) { RequestFileState(file_state); }),
      lifecycle_executor(context_p), output_files(op_p), rows_copied(0), last_file_offset(0) {
}

CopyToFileGlobalState::~CopyToFileGlobalState() {
	try {
		WaitForLifecycleTasks();
	} catch (...) {
	}
}

void CopyToFileGlobalState::Initialize() {
	if (initialized) {
		return;
	}
	RequestFileState(global_state, op.file_path);
	initialized = true;
}

void CopyToFileGlobalState::PrepareOutputDirectory() {
	auto &fs = FileSystem::GetFileSystem(context);
	if (!fs.IsRemoteFile(op.file_path)) {
		if (fs.FileExists(op.file_path)) {
			if (op.overwrite_mode == CopyOverwriteMode::COPY_OVERWRITE) {
				fs.RemoveFile(op.file_path);
			} else {
				throw IOException("Cannot write to \"%s\" - it exists and is a file, not a directory! Enable "
				                  "OVERWRITE option to overwrite the file",
				                  op.file_path);
			}
		}
	}

	if (!fs.DirectoryExists(op.file_path)) {
		if (fs.CreateDirectoryExtended(op.file_path, {CreateDirectoryMode::SINGLE})) {
			output_lifecycle.RegisterCreatedDirectory(op.file_path);
		} else {
			CheckDirectory(fs, op.file_path, op.overwrite_mode);
		}
	} else {
		CheckDirectory(fs, op.file_path, op.overwrite_mode);
	}
}

void CopyToFileGlobalState::ScheduleOutputDirectorySetup() {
	D_ASSERT(!output_directory_job);
	output_directory_job = make_shared_ptr<CopyFileLifecycleJob>();
	try {
		lifecycle_executor.Schedule(output_directory_job, CopyFileLifecycleWaitMode::INTERRUPTIBLE,
		                            [this]() { PrepareOutputDirectory(); });
	} catch (...) {
		if (!output_directory_job->IsFinished()) {
			output_directory_job->CompleteException(std::current_exception());
		}
		throw;
	}
}

void CopyToFileGlobalState::EnsureOutputDirectoryReady() {
	if (!output_directory_job) {
		return;
	}
	lifecycle_executor.WaitForJob(*output_directory_job, CopyFileLifecycleWaitMode::INTERRUPTIBLE);
}

void CopyToFileGlobalState::EnsureDirectory(const string &dir_path) {
	auto &fs = FileSystem::GetFileSystem(context);
	if (directory_manager.EnsureDirectory(fs, dir_path)) {
		output_lifecycle.RegisterCreatedDirectory(dir_path);
	}
}

PendingFileState CopyToFileGlobalState::PrepareFileStateLocked(string output_path,
                                                               optional_ptr<const vector<Value>> partition_values) {
	auto &fs = FileSystem::GetFileSystem(context);
	if (output_path.empty()) {
		output_path = op.filename_pattern.CreateFilename(fs, op.file_path, op.file_extension, last_file_offset++);
	}
	auto result = output_files.ReserveFile(std::move(output_path), partition_values);
	output_files.PublishCreatedPath(result, result.output_path);
	return result;
}

void CopyToFileGlobalState::RegisterPendingFileStatePathLocked(PendingFileState &pending_file_state,
                                                               string output_path) {
	output_files.PublishCreatedPath(pending_file_state, std::move(output_path));
}

unique_ptr<GlobalFileState> CopyToFileGlobalState::InitializeFileState(PendingFileState pending_file_state) {
	output_lifecycle.RegisterFile(pending_file_state.output_path);
	auto data = op.function.copy_to_initialize_global(context, *op.bind_data, pending_file_state.output_path);
	if (pending_file_state.written_file_info && pending_file_state.written_file_info->file_stats) {
		op.function.copy_to_get_written_statistics(context, *op.bind_data, *data,
		                                           *pending_file_state.written_file_info->file_stats);
	}
	if (op.function.initialize_operator) {
		op.function.initialize_operator(*data, op);
	}

	return make_uniq<GlobalFileState>(std::move(data), pending_file_state.output_path);
}

void CopyToFileGlobalState::RegisterPrepareGlobalStateLocked(GlobalFileState &file_state) {
	if (!prepare_global_state.load(std::memory_order_acquire)) {
		prepare_global_state.store(file_state, std::memory_order_release);
	}
}

PendingFileStateOpen
CopyToFileGlobalState::CreateFileStateOpenLocked(FileStateHandle &file_state, string output_path,
                                                 optional_ptr<const vector<Value>> partition_values) {
	if (file_state.HasFileState()) {
		return PendingFileStateOpen();
	}
	PendingFileStateOpen result;
	result.pending_file_state = PrepareFileStateLocked(std::move(output_path), partition_values);
	result.open_job = make_shared_ptr<FileStateOpenJob>();
	file_state.open_job = result.open_job;
	return result;
}

PendingFileStateOpen
CopyToFileGlobalState::CreatePartitionFileStateOpenLocked(FileStateHandle &file_state, string output_path,
                                                          optional_ptr<const vector<Value>> partition_values) {
	if (file_state.HasFileState()) {
		return PendingFileStateOpen();
	}
	PendingFileStateOpen result;
	result.pending_file_state = output_files.ReserveFile(std::move(output_path), partition_values);
	result.open_job = make_shared_ptr<FileStateOpenJob>();
	file_state.open_job = result.open_job;
	return result;
}

void CopyToFileGlobalState::ScheduleFileStateOpen(PendingFileStateOpen pending_file_state_open) {
	D_ASSERT(pending_file_state_open);
	auto open_job = pending_file_state_open.open_job;
	try {
		lifecycle_executor.Schedule(
		    open_job, CopyFileLifecycleWaitMode::INTERRUPTIBLE,
		    [this, open_job, pending_file_state = std::move(pending_file_state_open.pending_file_state)]() mutable {
			    EnsureOutputDirectoryReady();
			    open_job->Complete(InitializeFileState(std::move(pending_file_state)));
		    });
	} catch (...) {
		if (!open_job->IsFinished()) {
			open_job->CompleteException(std::current_exception());
		}
		throw;
	}
}

void CopyToFileGlobalState::SchedulePartitionFileStateOpen(PartitionFileOpenRequest request) {
	auto open_job = request.open_job;
	D_ASSERT(open_job);
	try {
		lifecycle_executor.Schedule(open_job, CopyFileLifecycleWaitMode::INTERRUPTIBLE,
		                            [this, request = std::move(request)]() mutable { request.Run(*this); });
	} catch (...) {
		if (!open_job->IsFinished()) {
			open_job->CompleteException(std::current_exception());
		}
		throw;
	}
}

void PartitionFileOpenRequest::Run(CopyToFileGlobalState &copy_gstate) {
	copy_gstate.EnsureOutputDirectoryReady();
	auto &fs = FileSystem::GetFileSystem(copy_gstate.context);
	for (auto &dir : directory.directories) {
		copy_gstate.EnsureDirectory(dir);
	}

	auto output_path = std::move(pending_file_state.output_path);
	if (copy_gstate.op.overwrite_mode == CopyOverwriteMode::COPY_APPEND) {
		while (fs.FileExists(output_path)) {
			if (!copy_gstate.op.filename_pattern.HasUUID()) {
				throw InternalException("CopyOverwriteMode::COPY_APPEND without {uuid} - and file exists");
			}
			output_path = copy_gstate.op.filename_pattern.CreateFilename(fs, directory.path,
			                                                             copy_gstate.op.file_extension, offset);
		}
	}
	{
		annotated_lock_guard<annotated_mutex> guard(copy_gstate.lock);
		copy_gstate.RegisterPendingFileStatePathLocked(pending_file_state, std::move(output_path));
	}
	open_job->Complete(copy_gstate.InitializeFileState(std::move(pending_file_state)));
}

void CopyToFileGlobalState::RequestFileState(FileStateHandle &file_state, string output_path,
                                             optional_ptr<const vector<Value>> partition_values) {
	PendingFileStateOpen pending_file_state_open;
	{
		annotated_lock_guard<annotated_mutex> guard(lock);
		pending_file_state_open = CreateFileStateOpenLocked(file_state, std::move(output_path), partition_values);
	}
	if (pending_file_state_open) {
		ScheduleFileStateOpen(std::move(pending_file_state_open));
	}
}

GlobalFileState &
CopyToFileGlobalState::EnsureFileStateReady(FileStateHandle &file_state,
                                            const std::function<void(FileStateHandle &)> &create_file_state_fun) {
	while (true) {
		shared_ptr<FileStateOpenJob> open_job;
		{
			annotated_lock_guard<annotated_mutex> guard(lock);
			if (file_state.HasFileState()) {
				open_job = file_state.open_job;
			}
		}
		if (!open_job) {
			create_file_state_fun(file_state);
			continue;
		}
		lifecycle_executor.WaitForJob(*open_job, CopyFileLifecycleWaitMode::INTERRUPTIBLE);
		{
			annotated_lock_guard<annotated_mutex> guard(lock);
			if (file_state.open_job == open_job) {
				auto &result = open_job->GetFileState();
				RegisterPrepareGlobalStateLocked(result);
				return result;
			}
		}
	}
}

FileStateHandle CopyToFileGlobalState::FinalizeFileStateLocked(FileStateHandle file_state) {
	auto prepare_state = prepare_global_state.load(std::memory_order_acquire);
	if (prepare_state && RefersToSameObject(*prepare_state.get(), file_state.GetFileState())) {
		prepare_global_state_owned = std::move(file_state);
		return FileStateHandle();
	}
	return file_state;
}

void CopyToFileGlobalState::FinalizeFileState(FileStateHandle file_state) {
	if (!file_state) {
		return;
	}
	lifecycle_executor.WaitForJob(*file_state.open_job, CopyFileLifecycleWaitMode::DRAIN);
	{
		annotated_lock_guard<annotated_mutex> guard(lock);
		file_state = FinalizeFileStateLocked(std::move(file_state));
	}
	if (file_state) {
		auto finalize_job = make_shared_ptr<CopyFileLifecycleJob>();
		auto state = file_state.TakeFileState();
		auto state_holder = make_shared_ptr<unique_ptr<GlobalFileState>>(std::move(state));
		auto finalize = op.function.copy_to_finalize;
		auto &context_ref = context;
		auto &bind_data = *op.bind_data;
		try {
			lifecycle_executor.Schedule(finalize_job, CopyFileLifecycleWaitMode::DRAIN,
			                            [finalize, &context_ref, &bind_data, state_holder]() mutable {
				                            FinalizeLifecycleFileState(context_ref, finalize, bind_data,
				                                                       std::move(*state_holder));
			                            });
		} catch (...) {
			if (!finalize_job->IsFinished() && state_holder && *state_holder) {
				try {
					FinalizeLifecycleFileState(context_ref, finalize, bind_data, std::move(*state_holder));
				} catch (...) {
				}
			}
			throw;
		}
	}
}

void CopyToFileGlobalState::AbandonFileState(FileStateHandle file_state) noexcept {
	try {
		annotated_lock_guard<annotated_mutex> guard(lock);
		auto state = file_state.GetFileStatePtr();
		auto prepare_state = prepare_global_state.load(std::memory_order_acquire);
		if (state && prepare_state && RefersToSameObject(*prepare_state.get(), *state.get())) {
			// concurrent prepares still use this state - keep it alive until the global state is destroyed
			prepare_global_state_owned = std::move(file_state);
		}
	} catch (...) {
	}
}

FileStateHandle CopyToFileGlobalState::TryFinalizeOwnedFileStateLocked() {
	if (prepare_global_state_owned) {
		prepare_global_state.store(nullptr, std::memory_order_release);
		return std::move(prepare_global_state_owned);
	}
	return FileStateHandle();
}

void CopyToFileGlobalState::TryFinalizeOwnedFileState() {
	FileStateHandle file_state;
	{
		annotated_lock_guard<annotated_mutex> guard(lock);
		file_state = TryFinalizeOwnedFileStateLocked();
	}
	if (file_state) {
		FinalizeFileState(std::move(file_state));
	}
}

void CopyToFileGlobalState::WaitForLifecycleTasks() {
	lifecycle_executor.WaitAll(CopyFileLifecycleWaitMode::DRAIN);
}

void CopyToFileGlobalState::MarkSuccessful() noexcept {
	output_lifecycle.MarkSuccessful();
}

//===--------------------------------------------------------------------===//
// Copy Local State
//===--------------------------------------------------------------------===//
CopyToFileLocalState::~CopyToFileLocalState() {
	if (global_file_state) {
		gstate.AbandonFileState(std::move(global_file_state));
	}
}

CopyToFileLocalState::CopyToFileLocalState(const PhysicalCopyToFile &op_p, ExecutionContext &context_p,
                                           CopyToFileGlobalState &gstate_p)
    : op(op_p), context(context_p), gstate(gstate_p) {
	if (op.partition_output) {
		++gstate.partitioned_copy->locals;
		partitioned_copy_local_state = make_uniq<PartitionedCopyLocalState>();
		return;
	}
	batch_slicer = make_uniq<CopyBatchSlicer>(op.expected_types, op.batch_size_bytes);
}

//===--------------------------------------------------------------------===//
// PhysicalCopyToFile
//===--------------------------------------------------------------------===//
PhysicalCopyToFile::PhysicalCopyToFile(PhysicalPlan &physical_plan, vector<LogicalType> types, CopyFunction function_p,
                                       unique_ptr<FunctionData> bind_data, idx_t estimated_cardinality)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::COPY_TO_FILE, std::move(types), estimated_cardinality),
      function(std::move(function_p)), bind_data(std::move(bind_data)), parallel(false) {
}

PhysicalCopyToFile::~PhysicalCopyToFile() {
	sink_state.reset();
}

InsertionOrderPreservingMap<string> PhysicalCopyToFile::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	result["FORMAT"] = StringUtil::Upper(function.name.GetIdentifierName());
	return result;
}

string PhysicalCopyToFile::GetTrimmedPath(ClientContext &context, const string &file_path) {
	auto &fs = FileSystem::GetFileSystem(context);
	string trimmed_path = file_path;
	StringUtil::RTrim(trimmed_path, fs.PathSeparator(trimmed_path));
	return trimmed_path;
}

void PhysicalCopyToFile::MoveTmpFile(ClientContext &context, const string &tmp_file_path) {
	auto &fs = FileSystem::GetFileSystem(context);
	auto file_path = GetNonTmpFile(context, tmp_file_path);
	fs.MoveFile(tmp_file_path, file_path);
}

string PhysicalCopyToFile::GetNonTmpFile(ClientContext &context, const string &tmp_file_path) {
	auto base = StringUtil::GetFileName(tmp_file_path);

	auto prefix = base.find("tmp_");
	if (prefix == 0) {
		base = base.substr(4);
	}

	return StringUtil::ReplaceFileName(tmp_file_path, base);
}

void PhysicalCopyToFile::ReturnColumnTypes(DataChunk &chunk, const vector<Identifier> &query_names,
                                           const vector<LogicalType> &query_types) {
	D_ASSERT(query_names.size() == query_types.size());
	auto &result = chunk.data.back();
	vector<Value> columns;
	for (idx_t column_index = 0; column_index < query_types.size(); column_index++) {
		child_list_t<Value> column;
		column.emplace_back("name", Value(query_names[column_index].GetIdentifierName()));
		column.emplace_back("type", Value::TYPE(query_types[column_index]));
		columns.push_back(Value::STRUCT(std::move(column)));
	}
	auto column_types = Value::LIST(ListType::GetChildType(result.GetType()), std::move(columns));
	auto row_count = chunk.data[0].size();
	for (idx_t row = 0; row < row_count; row++) {
		result.Append(column_types);
	}
}

void PhysicalCopyToFile::ReturnStatistics(DataChunk &chunk, CopyToFileInfo &info) {
	auto &file_stats = *info.file_stats;

	// filename VARCHAR
	chunk.data[0].Append(Value(info.file_path));
	// count BIGINT
	chunk.data[1].Append(Value::UBIGINT(file_stats.row_count));
	// file size bytes BIGINT
	chunk.data[2].Append(Value::UBIGINT(file_stats.file_size_bytes));
	// footer size bytes BIGINT
	chunk.data[3].Append(file_stats.footer_size_bytes);
	// column statistics map(varchar, map(varchar, varchar))
	auto column_stats = CreateColumnStatistics(file_stats.column_statistics);
	auto map_val_type = LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR);
	chunk.data[4].Append(
	    Value::MAP(LogicalType::VARCHAR, map_val_type, std::move(column_stats.keys), std::move(column_stats.values)));

	// partition_keys map(varchar, varchar)
	chunk.data[5].Append(info.partition_keys);

	// extra info map(varchar, variant)
	vector<Value> extra_keys;
	vector<Value> extra_values;
	for (auto &entry : file_stats.extra_info) {
		extra_keys.emplace_back(entry.first);
		extra_values.push_back(entry.second.DefaultCastAs(LogicalType::VARIANT()));
	}
	chunk.data[6].Append(
	    Value::MAP(LogicalType::VARCHAR, LogicalType::VARIANT(), std::move(extra_keys), std::move(extra_values)));
}

void PhysicalCopyToFile::ReturnNoFileStatistics(DataChunk &chunk) {
	// filename VARCHAR
	chunk.data[0].Append(Value(LogicalType::VARCHAR));
	// count BIGINT
	chunk.data[1].Append(Value::UBIGINT(0));
	// file size bytes, footer size bytes, column statistics, partition keys and extra info are NULL
	auto statistics_column_count =
	    GetCopyFunctionReturnLogicalTypes(CopyFunctionReturnType::WRITTEN_FILE_STATISTICS).size();
	for (idx_t col_idx = 2; col_idx < statistics_column_count; col_idx++) {
		chunk.data[col_idx].Append(Value(chunk.data[col_idx].GetType()));
	}
}

bool PhysicalCopyToFile::Rotate() const {
	return file_size_bytes.IsValid() || batches_per_file.IsValid();
}

static bool PhysicalCopyRotateNow(const PhysicalCopyToFile &op, GlobalFileState &global_state)
    DUCKDB_REQUIRES(global_state.lock) {
	if (global_state.num_batches == 0) {
		// never rotate a file that holds no data yet - e.g. a CSV header or a Parquet header can exceed the
		// file size limit by itself, and rotating would then create new empty files forever
		return false;
	}
	// either rotation limit rotates the file
	if (op.file_size_bytes.IsValid() &&
	    op.function.file_size_bytes(*global_state.data) >= op.file_size_bytes.GetIndex()) {
		return true;
	}
	if (op.batches_per_file.IsValid() && global_state.num_batches >= op.batches_per_file.GetIndex()) {
		return true;
	}
	return false;
}

//===--------------------------------------------------------------------===//
// Sink Interface
//===--------------------------------------------------------------------===//
unique_ptr<GlobalSinkState> PhysicalCopyToFile::GetGlobalSinkState(ClientContext &context) const {
	if (partition_output || per_thread_output || Rotate()) {
		auto state = make_uniq<CopyToFileGlobalState>(*this, context);
		state->ScheduleOutputDirectorySetup();
		if (!partition_output && !per_thread_output && Rotate() && write_empty_file) {
			state->RequestFileState(state->global_state);
		}

		if (partition_output) {
			state->partitioned_copy = make_uniq<PartitionedCopy>(*this, context, *state);
		}

		return std::move(state);
	}

	auto state = make_uniq<CopyToFileGlobalState>(*this, context);
	if (write_empty_file) {
		// if we are writing the file also if it is empty - initialize now
		state->Initialize();
	}
	return std::move(state);
}

unique_ptr<LocalSinkState> PhysicalCopyToFile::GetLocalSinkState(ExecutionContext &context) const {
	auto &gstate = sink_state->Cast<CopyToFileGlobalState>();
	return make_uniq<CopyToFileLocalState>(*this, context, gstate);
}

SinkResultType PhysicalCopyToFile::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
	auto &gstate = input.global_state.Cast<CopyToFileGlobalState>();
	auto &lstate = input.local_state.Cast<CopyToFileLocalState>();

	if (!write_empty_file && !Rotate()) {
		// if we are only writing the file when there are rows to write we need to initialize here
		gstate.Initialize();
	}
	if (partition_output) {
		auto result =
		    gstate.partitioned_copy->Sink(context, chunk, *lstate.partitioned_copy_local_state, input.interrupt_state);
		if (result != SinkResultType::BLOCKED) {
			lstate.total_rows_copied += chunk.size();
		}
		return result;
	}
	lstate.total_rows_copied += chunk.size();

	if (per_thread_output) {
		if (!lstate.global_file_state) {
			gstate.create_file_state_fun(lstate.global_file_state);
		}
	} else {
		gstate.create_file_state_fun(gstate.global_state);
	}
	auto &file_state = per_thread_output ? lstate.global_file_state : gstate.global_state;

	idx_t offset = 0;
	while (offset < chunk.size()) {
		if (!lstate.batch) {
			lstate.batch = make_uniq<ColumnDataCollection>(context.client, expected_types);
			lstate.batch->InitializeAppend(lstate.batch_append_state);
		}
		auto &slice = lstate.batch_slicer->Slice(chunk, offset, *lstate.batch);
		lstate.batch->Append(lstate.batch_append_state, slice);
		const CopyFunctionBatchAnalyzer batch_analyzer(*lstate.batch, batch_size, batch_size_bytes);
		if (batch_analyzer.MeetsFlushCriteria()) {
			lstate.batch_append_state.current_chunk_state.handles.clear();
			PrepareAndFlushBatch(context.client, gstate, file_state, gstate.create_file_state_fun,
			                     std::move(lstate.batch));
		}
	}

	return SinkResultType::NEED_MORE_INPUT;
}

SinkCombineResultType PhysicalCopyToFile::Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const {
	auto &gstate = input.global_state.Cast<CopyToFileGlobalState>();
	auto &lstate = input.local_state.Cast<CopyToFileLocalState>();
	if (partition_output) {
		if (lstate.total_rows_copied) {
			gstate.rows_copied += lstate.total_rows_copied;
			lstate.total_rows_copied = 0;
		}
		return gstate.partitioned_copy->FinishSink(context, *lstate.partitioned_copy_local_state,
		                                           input.interrupt_state);
	}
	if (lstate.total_rows_copied == 0) {
		// no rows copied
		return SinkCombineResultType::FINISHED;
	}
	gstate.rows_copied += lstate.total_rows_copied;

	if (per_thread_output) {
		if (lstate.batch) {
			PrepareAndFlushBatch(context.client, gstate, lstate.global_file_state, gstate.create_file_state_fun,
			                     std::move(lstate.batch));
		}
		if (lstate.global_file_state) {
			gstate.FinalizeFileState(std::move(lstate.global_file_state));
		}
		return SinkCombineResultType::FINISHED;
	}

	if (!lstate.batch) {
		return SinkCombineResultType::FINISHED;
	}

	unique_ptr<ColumnDataCollection> batch;
	{
		annotated_lock_guard<annotated_mutex> guard(gstate.last_batch_lock);
		if (gstate.last_batch) {
			const auto count = gstate.last_batch->Count() + lstate.batch->Count();
			const auto size_in_bytes = gstate.last_batch->SizeInBytes() + lstate.batch->SizeInBytes();
			const CopyFunctionBatchAnalyzer batch_analyzer(count, size_in_bytes, batch_size, batch_size_bytes);
			if (batch_analyzer.MeetsFlushCriteria()) {
				// Combining makes us overshoot, make sure the smallest one gets flushed now
				auto &small = lstate.batch->Count() < gstate.last_batch->Count() ? lstate.batch : gstate.last_batch;
				auto &large = lstate.batch->Count() < gstate.last_batch->Count() ? gstate.last_batch : lstate.batch;
				batch = std::move(large);
				gstate.last_batch = std::move(small);
			} else {
				gstate.last_batch->Combine(*lstate.batch);
			}
		} else {
			gstate.last_batch = std::move(lstate.batch);
		}
	}

	if (!batch) {
		return SinkCombineResultType::FINISHED;
	}

	auto &file_state_ptr = per_thread_output ? lstate.global_file_state : gstate.global_state;
	PrepareAndFlushBatch(context.client, gstate, file_state_ptr, gstate.create_file_state_fun, std::move(batch));

	return SinkCombineResultType::FINISHED;
}

SinkFinalizeType PhysicalCopyToFile::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                                              OperatorSinkFinalizeInput &input) const {
	auto &gstate = input.global_state.Cast<CopyToFileGlobalState>();

	if (partition_output) {
		gstate.partitioned_copy->Finalize(pipeline, event, input.interrupt_state);
		return SinkFinalizeType::READY;
	}

	{
		annotated_lock_guard<annotated_mutex> guard(gstate.last_batch_lock);
		if (gstate.last_batch) {
			unique_ptr<LocalFunctionData> lstate;
			PrepareAndFlushBatch(context, gstate, gstate.global_state, gstate.create_file_state_fun,
			                     std::move(gstate.last_batch));
		}
	}

	if (per_thread_output) {
		// already happened in combine
		if (NumericCast<int64_t>(gstate.rows_copied.load()) == 0 && sink_state != nullptr) {
			// no rows from source, write schema to file
			gstate.RequestFileState(gstate.global_state);
		}
	}

	if (gstate.global_state) {
		gstate.FinalizeFileState(std::move(gstate.global_state));
	}

	gstate.TryFinalizeOwnedFileState();
	gstate.WaitForLifecycleTasks();

	if (use_tmp_file) {
		D_ASSERT(!per_thread_output);
		D_ASSERT(!partition_output);
		D_ASSERT(!file_size_bytes.IsValid());
		D_ASSERT(!Rotate());
		MoveTmpFile(context, file_path);
	}
	gstate.MarkSuccessful();

	return SinkFinalizeType::READY;
}

void PhysicalCopyToFile::PrepareAndFlushBatch(ClientContext &context, GlobalSinkState &gstate_p,
                                              FileStateHandle &file_state,
                                              const std::function<void(FileStateHandle &)> &create_file_state_fun,
                                              unique_ptr<ColumnDataCollection> batch) const {
	auto [batch_analyzer, prepared_batch] =
	    PrepareBatch(context, gstate_p, file_state, create_file_state_fun, std::move(batch));
	FlushBatch(context, gstate_p, file_state, create_file_state_fun, batch_analyzer, std::move(prepared_batch));
}

//===--------------------------------------------------------------------===//
// Legacy Batch API
//===--------------------------------------------------------------------===//
struct LegacyCopyPreparedBatch : public PreparedBatchData {
	explicit LegacyCopyPreparedBatch(unique_ptr<ColumnDataCollection> collection_p)
	    : collection(std::move(collection_p)) {
	}

	unique_ptr<ColumnDataCollection> collection;
};

static bool UsesLegacyCopyBatchAPI(const CopyFunction &function) {
	if (!function.prepare_batch && !function.flush_batch) {
		return true;
	}
	if (!function.prepare_batch || !function.flush_batch) {
		throw InternalException("Copy function must implement both prepare_batch and flush_batch");
	}
	return false;
}

static unique_ptr<PreparedBatchData> PrepareLegacyCopyBatch(unique_ptr<ColumnDataCollection> batch) {
	return make_uniq<LegacyCopyPreparedBatch>(std::move(batch));
}

static void FlushLegacyCopyBatch(ClientContext &context, const CopyFunction &function, FunctionData &bind_data,
                                 GlobalFunctionData &gstate, PreparedBatchData &prepared_batch) {
	if (!function.copy_to_initialize_local || !function.copy_to_sink || !function.copy_to_combine) {
		throw InternalException("Legacy copy function is missing required sink/combine callbacks");
	}

	auto &legacy_batch = prepared_batch.Cast<LegacyCopyPreparedBatch>();
	ThreadContext thread_context(context);
	ExecutionContext execution_context(context, thread_context, nullptr);
	auto local_state = function.copy_to_initialize_local(execution_context, bind_data);
	for (auto &chunk : legacy_batch.collection->Chunks()) {
		function.copy_to_sink(execution_context, bind_data, gstate, *local_state, chunk);
	}
	function.copy_to_combine(execution_context, bind_data, gstate, *local_state);
}

//===--------------------------------------------------------------------===//
// Batch Interface
//===--------------------------------------------------------------------===//
pair<const CopyFunctionBatchAnalyzer, unique_ptr<PreparedBatchData>>
PhysicalCopyToFile::PrepareBatch(ClientContext &context, GlobalSinkState &gstate_p, FileStateHandle &file_state,
                                 const std::function<void(FileStateHandle &)> &create_file_state_fun,
                                 unique_ptr<ColumnDataCollection> batch) const {
	auto &gstate = gstate_p.Cast<CopyToFileGlobalState>();
	const CopyFunctionBatchAnalyzer batch_analyzer(*batch, batch_size, batch_size_bytes);

	if (UsesLegacyCopyBatchAPI(function)) {
		return {batch_analyzer, PrepareLegacyCopyBatch(std::move(batch))};
	}

	// Ensure we have a global state for prepares
	auto prepare_global_state = gstate.prepare_global_state.load(std::memory_order_acquire);
	if (!prepare_global_state) {
		gstate.EnsureFileStateReady(file_state, create_file_state_fun);
		prepare_global_state = gstate.prepare_global_state.load(std::memory_order_acquire);
		D_ASSERT(prepare_global_state);
	}

	// Prepare the batch
	return {batch_analyzer, function.prepare_batch(context, *bind_data, *prepare_global_state->data, std::move(batch))};
}

void PhysicalCopyToFile::FlushBatch(ClientContext &context, GlobalSinkState &gstate_p, FileStateHandle &file_state,
                                    const std::function<void(FileStateHandle &)> &create_file_state_fun,
                                    const CopyFunctionBatchAnalyzer &batch_analyzer,
                                    unique_ptr<PreparedBatchData> prepared_batch) const {
	auto &gstate = gstate_p.Cast<CopyToFileGlobalState>();

	while (true) {
		gstate.EnsureFileStateReady(file_state, create_file_state_fun);

		// Decide which file to flush to
		annotated_unique_lock<annotated_mutex> global_guard(gstate.lock);
		// Another thread may have rotated the file state since EnsureFileStateReady, so re-check readiness
		if (!file_state.IsReady()) {
			global_guard.unlock();
			gstate.lifecycle_executor.WorkOnTaskOrYield();
			continue;
		}

		auto ready_file_state = file_state.GetFileStatePtr();
		if (!ready_file_state) {
			global_guard.unlock();
			gstate.lifecycle_executor.WorkOnTaskOrYield();
			continue;
		}
		annotated_unique_lock<annotated_mutex> file_guard(ready_file_state->lock);
		if (PhysicalCopyRotateNow(*this, *ready_file_state)) {
			// Global state must be rotated. Move to local scope, create an new one, and immediately release global lock
			// If this is the file state all prepares run against, hand it over to the global state while we still hold
			// the lock: creating the replacement below can throw (e.g. when another thread's error interrupts the
			// query), and a handle that goes out of scope would free a file state other threads still prepare into
			auto owned_file_state = gstate.FinalizeFileStateLocked(std::move(file_state));
			file_guard.unlock();
			global_guard.unlock();

			gstate.EnsureFileStateReady(file_state, create_file_state_fun);

			// Finalize this file!
			if (owned_file_state) {
				gstate.FinalizeFileState(std::move(owned_file_state));
			}
		} else {
			global_guard.unlock();
			ready_file_state->num_batches++;

			DUCKDB_LOG(context, PhysicalOperatorLogType, *this, "PhysicalCopyToFile", "FlushBatch",
			           {{"file", ready_file_state->path},
			            {"rows", to_string(batch_analyzer.current_batch_size)},
			            {"size", to_string(batch_analyzer.current_batch_size_bytes)},
			            {"reason", EnumUtil::ToString(batch_analyzer.ToReason())}});
			if (UsesLegacyCopyBatchAPI(function)) {
				FlushLegacyCopyBatch(context, function, *bind_data, *ready_file_state->data, *prepared_batch);
			} else {
				function.flush_batch(context, *bind_data, *ready_file_state->data, *prepared_batch);
			}
			break;
		}
	}
}

//===--------------------------------------------------------------------===//
// Source Interface
//===--------------------------------------------------------------------===//
class CopyToFileGlobalSourceState : public GlobalSourceState {
public:
	CopyToFileGlobalSourceState() {
	}

	idx_t offset = 0;

	idx_t MaxThreads() override {
		return 1;
	}
};

unique_ptr<GlobalSourceState> PhysicalCopyToFile::GetGlobalSourceState(ClientContext &context) const {
	return make_uniq<CopyToFileGlobalSourceState>();
}

SourceResultType PhysicalCopyToFile::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                     OperatorSourceInput &input) const {
	auto &gstate = sink_state->Cast<CopyToFileGlobalState>();
	gstate.WaitForLifecycleTasks();
	annotated_lock_guard<annotated_mutex> global_guard(gstate.lock);
	auto base_return_type = GetCopyFunctionBaseReturnType(return_type);
	auto return_column_types = CopyFunctionReturnsColumnTypes(return_type);
	if (base_return_type == CopyFunctionReturnType::WRITTEN_FILE_STATISTICS) {
		auto &source_state = input.global_state.Cast<CopyToFileGlobalSourceState>();
		idx_t next_end =
		    MinValue<idx_t>(source_state.offset + STANDARD_VECTOR_SIZE, gstate.output_files.WrittenFileCount());
		idx_t count = next_end - source_state.offset;
		for (idx_t i = 0; i < count; i++) {
			auto &file_entry = gstate.output_files.GetWrittenFile(source_state.offset + i);
			if (use_tmp_file) {
				file_entry.file_path = GetNonTmpFile(context.client, file_entry.file_path);
			}
			ReturnStatistics(chunk, file_entry);
		}
		if (return_column_types) {
			if (gstate.output_files.WrittenFileCount() == 0) {
				// WRITE_EMPTY_FILE false, or PARTITION_BY without rows, writes no file
				ReturnNoFileStatistics(chunk);
			}
			ReturnColumnTypes(chunk, query_names, query_types);
		}
		source_state.offset += count;
		return source_state.offset < gstate.output_files.WrittenFileCount() ? SourceResultType::HAVE_MORE_OUTPUT
		                                                                    : SourceResultType::FINISHED;
	}

	switch (base_return_type) {
	case CopyFunctionReturnType::CHANGED_ROWS:
		chunk.data[0].Append(Value::BIGINT(NumericCast<int64_t>(gstate.rows_copied.load())));
		break;
	case CopyFunctionReturnType::CHANGED_ROWS_AND_FILE_LIST: {
		chunk.data[0].Append(Value::BIGINT(NumericCast<int64_t>(gstate.rows_copied.load())));
		vector<Value> file_name_list;
		for (auto &file_info : gstate.output_files.GetWrittenFiles()) {
			if (use_tmp_file) {
				file_name_list.emplace_back(GetNonTmpFile(context.client, file_info->file_path));
			} else {
				file_name_list.emplace_back(file_info->file_path);
			}
		}
		chunk.data[1].Append(Value::LIST(LogicalType::VARCHAR, std::move(file_name_list)));
		break;
	}
	default:
		throw NotImplementedException("Unknown CopyFunctionReturnType");
	}

	if (return_column_types) {
		ReturnColumnTypes(chunk, query_names, query_types);
	}
	return SourceResultType::FINISHED;
}

} // namespace duckdb
