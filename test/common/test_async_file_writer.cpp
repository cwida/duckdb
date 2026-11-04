#include "catch.hpp"
#include "duckdb.hpp"
#include "duckdb/common/compressed_file_system.hpp"
#include "duckdb/common/csv_writer.hpp"
#include "duckdb/common/local_file_system.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/serializer/async_memory_governor.hpp"
#include "duckdb/common/serializer/async_file_writer.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_reader_options.hpp"
#include "duckdb/main/query_profiler.hpp"
#include "duckdb/parallel/task_executor.hpp"
#include "duckdb/parallel/task_scheduler.hpp"
#include "test_helpers.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <thread>

using namespace duckdb;

namespace duckdb {

class ManagedAsyncWriteQueueTest {
public:
	static bool TryAdopt(ManagedAsyncWriteQueue &queue, AsyncWriteRequest &request, ErrorData &error) {
		return queue.TryAdoptAccountedWrite(request, error) == ManagedAsyncWriteQueue::AccountedWriteAdoption::ACCEPTED;
	}

	static void AddExternalPendingBytes(ManagedAsyncWriteQueue &queue, idx_t size) {
		queue.AddExternalPendingBytes(size);
	}

	static void DiscardExternalPendingBytes(ManagedAsyncWriteQueue &queue, idx_t size) {
		queue.DiscardExternalPendingBytes(size);
	}

	static idx_t ExternalPendingBytes(ManagedAsyncWriteQueue &queue) {
		lock_guard<mutex> guard(queue.lock);
		return queue.external_pending_bytes;
	}

	static idx_t PendingBytes(ManagedAsyncWriteQueue &queue) {
		lock_guard<mutex> guard(queue.lock);
		return queue.pending_bytes;
	}
	static ManagedAsyncWriteQueue &UnderlyingQueue(ManagedAsyncWriteStreamQueue &queue) {
		return *queue.write_queue;
	}

	static bool IsClosed(ManagedAsyncWriteQueue &queue) {
		lock_guard<mutex> guard(queue.lock);
		return queue.closed;
	}

	static idx_t PendingBytes(ManagedAsyncWriteStreamQueue &queue) {
		annotated_lock_guard<annotated_mutex> guard(queue.lock);
		return queue.pending_bytes;
	}

	static bool TryCompletionRefill(ManagedAsyncWriteStreamQueue &queue) {
		// Bound the wait so the broken lock order fails instead of hanging this completion.
		auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (!queue.submission_lock.try_lock()) {
			if (std::chrono::steady_clock::now() >= deadline) {
				return false;
			}
			std::this_thread::yield();
		}
		queue.submission_lock.unlock();
		queue.SchedulePendingWritesInternal();
		return true;
	}
};

} // namespace duckdb

namespace {

class FailingInitializeStreamWrapper : public StreamWrapper {
public:
	void Initialize(QueryContext, CompressedFile &, bool) override {
		throw IOException("Injected compressed stream initialization failure");
	}

	bool Read(StreamData &) override {
		return true;
	}

	void Write(CompressedFile &, StreamData &, data_ptr_t, int64_t) override {
	}

	void Close() override {
	}
};

class FailingInitializeCompressedFileSystem : public CompressedFileSystem {
public:
	string GetName() const override {
		return "FailingInitializeCompressedFileSystem";
	}
	FileCompressionType GetCompressionType() override {
		return FileCompressionType("failing_initialize");
	}

	unique_ptr<StreamWrapper> CreateStream() override {
		return make_uniq<FailingInitializeStreamWrapper>();
	}

	idx_t InBufferSize() override {
		return 64;
	}

	idx_t OutBufferSize() override {
		return 64;
	}
};

class TrackingWriteFileSystem : public LocalFileSystem {
public:
	explicit TrackingWriteFileSystem(bool local_file_p = true) : local_file(local_file_p) {
	}

	string GetName() const override {
		return "TrackingWriteFileSystem";
	}

	bool IsLocalFileSystem() const override {
		return local_file;
	}

	bool OnDiskFile(FileHandle &) override {
		return local_file;
	}

	int64_t Write(FileHandle &handle, void *buffer, int64_t nr_bytes) override {
		if (fail_writes) {
			throw IOException("Injected async write failure");
		}
		RecordWrite(nr_bytes, NumericLimits<idx_t>::Maximum());
		return LocalFileSystem::Write(handle, buffer, nr_bytes);
	}

	void Write(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override {
		if (nr_bytes == 0) {
			LocalFileSystem::Write(handle, buffer, nr_bytes, location);
			return;
		}
		if (fail_writes) {
			throw IOException("Injected async write failure");
		}
		RecordWrite(nr_bytes, location);
		LocalFileSystem::Write(handle, buffer, nr_bytes, location);
	}

	FileWriteMode GetWriteMode(FileHandle &handle) override {
		if (fail_get_write_mode) {
			throw IOException("Injected write mode failure");
		}
		return LocalFileSystem::GetWriteMode(handle);
	}

	void AbortFileWrite(FileHandle &handle) override {
		abort_count++;
		LocalFileSystem::AbortFileWrite(handle);
		if (fail_abort) {
			throw IOException("Injected abort failure");
		}
	}

	idx_t AbortCount() const {
		return abort_count.load();
	}

protected:
	void RecordWrite(int64_t nr_bytes, idx_t location) {
		lock_guard<mutex> guard(lock);
		write_sizes.push_back(UnsafeNumericCast<idx_t>(nr_bytes));
		write_offsets.push_back(location);
	}

public:
	mutex lock;
	vector<idx_t> write_sizes;
	vector<idx_t> write_offsets;
	bool fail_writes = false;
	bool fail_get_write_mode = false;
	bool fail_abort = false;

private:
	bool local_file;
	atomic<idx_t> abort_count {0};
};

class SizeMismatchWriteFileSystem : public TrackingWriteFileSystem {
public:
	explicit SizeMismatchWriteFileSystem(int64_t size_difference_p) : size_difference(size_difference_p) {
	}

	void Write(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override {
		TrackingWriteFileSystem::Write(handle, buffer, nr_bytes, location);
		LocalFileSystem::Truncate(handle, NumericCast<int64_t>(location) + nr_bytes + size_difference);
	}

private:
	int64_t size_difference;
};

class PublishingFileHandle : public FileHandle {
public:
	PublishingFileHandle(FileSystem &fs, const string &path, FileOpenFlags flags, atomic<idx_t> &publish_count_p,
	                     bool &fail_close_after_publish_p)
	    : FileHandle(fs, path, flags), publish_count(publish_count_p),
	      fail_close_after_publish(fail_close_after_publish_p) {
	}

	void Close() override {
		if (finished) {
			return;
		}
		finished = true;
		publish_count++;
		if (fail_close_after_publish) {
			throw IOException("Injected ambiguous publish failure");
		}
	}

	void Abort() {
		finished = true;
	}

private:
	atomic<idx_t> &publish_count;
	bool &fail_close_after_publish;
	bool finished = false;
};

class PublishingWriteFileSystem : public LocalFileSystem {
public:
	unique_ptr<FileHandle> OpenFile(const string &path, FileOpenFlags flags,
	                                optional_ptr<FileOpener> = nullptr) override {
		return make_uniq<PublishingFileHandle>(*this, path, flags, publish_count, fail_close_after_publish);
	}

	bool IsLocalFileSystem() const override {
		return false;
	}

	bool OnDiskFile(FileHandle &) override {
		return false;
	}

	FileWriteMode GetWriteMode(FileHandle &) override {
		return FileWriteMode::SEQUENTIAL;
	}

	int64_t Write(FileHandle &, void *, int64_t nr_bytes) override {
		if (fail_writes) {
			throw IOException("Injected publishing write failure");
		}
		written_bytes += UnsafeNumericCast<idx_t>(nr_bytes);
		return nr_bytes;
	}

	void Write(FileHandle &, void *, int64_t nr_bytes, idx_t) override {
		if (fail_writes) {
			throw IOException("Injected publishing write failure");
		}
		written_bytes += UnsafeNumericCast<idx_t>(nr_bytes);
	}

	void AbortFileWrite(FileHandle &handle) override {
		abort_count++;
		handle.Cast<PublishingFileHandle>().Abort();
	}

	atomic<idx_t> publish_count {0};
	atomic<idx_t> abort_count {0};
	atomic<idx_t> written_bytes {0};
	bool fail_writes = false;
	bool fail_close_after_publish = false;
};

class PartialSequentialWriteFileSystem : public TrackingWriteFileSystem {
public:
	int64_t Write(FileHandle &handle, void *buffer, int64_t nr_bytes) override {
		if (return_negative) {
			return -1;
		}
		return TrackingWriteFileSystem::Write(handle, buffer, MinValue<int64_t>(nr_bytes, 2));
	}

	FileWriteMode GetWriteMode(FileHandle &) override {
		return FileWriteMode::SEQUENTIAL;
	}

public:
	bool return_negative = false;
};

class BlockingWriteFileSystem : public TrackingWriteFileSystem {
public:
	explicit BlockingWriteFileSystem(FileWriteMode write_mode_p, bool local_file_p = true)
	    : TrackingWriteFileSystem(local_file_p), write_mode(write_mode_p) {
	}

	void Write(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override {
		if (write_mode == FileWriteMode::SEQUENTIAL) {
			throw NotImplementedException("Injected missing positional write support");
		}
		if (nr_bytes == 0) {
			TrackingWriteFileSystem::Write(handle, buffer, nr_bytes, location);
			return;
		}

		EnterWrite();
		try {
			TrackingWriteFileSystem::Write(handle, buffer, nr_bytes, location);
			LeaveWrite();
		} catch (...) {
			LeaveWrite();
			throw;
		}
	}

	FileWriteMode GetWriteMode(FileHandle &handle) override {
		return write_mode;
	}

	int64_t Write(FileHandle &handle, void *buffer, int64_t nr_bytes) override {
		EnterWrite();
		try {
			auto result = TrackingWriteFileSystem::Write(handle, buffer, nr_bytes);
			LeaveWrite();
			return result;
		} catch (...) {
			LeaveWrite();
			throw;
		}
	}

	bool WaitForBlockedWrites(idx_t count) {
		unique_lock<mutex> guard(block_lock);
		return cv.wait_for(guard, std::chrono::seconds(5), [&]() { return blocked_writes >= count; });
	}

	idx_t BlockedWrites() {
		lock_guard<mutex> guard(block_lock);
		return blocked_writes;
	}

	void ReleaseWrites() {
		{
			lock_guard<mutex> guard(block_lock);
			release_writes = true;
		}
		cv.notify_all();
	}

	idx_t MaxActiveWrites() {
		lock_guard<mutex> guard(block_lock);
		return max_active_writes;
	}

private:
	void EnterWrite() {
		unique_lock<mutex> guard(block_lock);
		active_writes++;
		max_active_writes = MaxValue(max_active_writes, active_writes);
		blocked_writes++;
		cv.notify_all();
		cv.wait(guard, [&]() { return release_writes; });
	}

	void LeaveWrite() {
		{
			lock_guard<mutex> guard(block_lock);
			D_ASSERT(active_writes > 0);
			active_writes--;
		}
		cv.notify_all();
	}

private:
	const FileWriteMode write_mode;

	mutex block_lock;
	std::condition_variable cv;
	idx_t active_writes = 0;
	idx_t max_active_writes = 0;
	idx_t blocked_writes = 0;
	bool release_writes = false;
};

class NonPositionalWriteFileSystem : public TrackingWriteFileSystem {
public:
	explicit NonPositionalWriteFileSystem(bool local_file_p = true) : TrackingWriteFileSystem(local_file_p) {
	}

	void Write(FileHandle &, void *, int64_t, idx_t) override {
		throw NotImplementedException("Injected missing positional write support");
	}

	FileWriteMode GetWriteMode(FileHandle &) override {
		return FileWriteMode::SEQUENTIAL;
	}
};

class SequentialExplicitOffsetWriteFileSystem : public BlockingWriteFileSystem {
public:
	SequentialExplicitOffsetWriteFileSystem() : BlockingWriteFileSystem(FileWriteMode::SEQUENTIAL, false) {
	}

	void Write(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override {
		if (nr_bytes == 0) {
			return;
		}
		if (location != next_write_offset) {
			throw InternalException("Injected non-sequential write");
		}
		next_write_offset += UnsafeNumericCast<idx_t>(nr_bytes);
		BlockingWriteFileSystem::Write(handle, buffer, nr_bytes);
	}

	int64_t Write(FileHandle &handle, void *buffer, int64_t nr_bytes) override {
		return BlockingWriteFileSystem::Write(handle, buffer, nr_bytes);
	}

private:
	idx_t next_write_offset = 0;
};

class ConcurrentSequentialWriteFileSystem : public TrackingWriteFileSystem {
public:
	explicit ConcurrentSequentialWriteFileSystem(bool block_backend_writes_p = true)
	    : TrackingWriteFileSystem(false), block_backend_writes(block_backend_writes_p) {
	}

	FileWriteMode GetWriteMode(FileHandle &) override {
		return FileWriteMode::CONCURRENT_SEQUENTIAL;
	}

	void Write(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override {
		if (nr_bytes == 0) {
			TrackingWriteFileSystem::Write(handle, buffer, nr_bytes, location);
			return;
		}

		auto write_size = UnsafeNumericCast<idx_t>(nr_bytes);
		{
			unique_lock<mutex> guard(state_lock);
			if (!state_cv.wait_for(guard, std::chrono::seconds(5),
			                       [&]() { return location == next_admission_offset; })) {
				throw InternalException("Concurrent-sequential write was not admitted in stream order");
			}
			admitted_offsets.push_back(location);
			next_admission_offset += write_size;
			active_backend_writes++;
			max_active_backend_writes = MaxValue(max_active_backend_writes, active_backend_writes);
			entered_backend_writes++;
			state_cv.notify_all();
			if (block_backend_writes) {
				state_cv.wait(guard, [&]() { return release_all_writes || IsReleased(location); });
			}
		}

		try {
			TrackingWriteFileSystem::Write(handle, buffer, nr_bytes, location);
			{
				lock_guard<mutex> guard(state_lock);
				D_ASSERT(active_backend_writes > 0);
				active_backend_writes--;
				completed_offsets.push_back(location);
			}
			state_cv.notify_all();
		} catch (...) {
			{
				lock_guard<mutex> guard(state_lock);
				D_ASSERT(active_backend_writes > 0);
				active_backend_writes--;
			}
			state_cv.notify_all();
			throw;
		}
	}

	bool WaitForBackendWrites(idx_t count) {
		unique_lock<mutex> guard(state_lock);
		return state_cv.wait_for(guard, std::chrono::seconds(5), [&]() { return entered_backend_writes >= count; });
	}

	bool WaitForCompletedWrites(idx_t count) {
		unique_lock<mutex> guard(state_lock);
		return state_cv.wait_for(guard, std::chrono::seconds(5), [&]() { return completed_offsets.size() >= count; });
	}

	void ReleaseWrite(idx_t offset) {
		{
			lock_guard<mutex> guard(state_lock);
			released_offsets.push_back(offset);
		}
		state_cv.notify_all();
	}

	void ReleaseAllWrites() {
		{
			lock_guard<mutex> guard(state_lock);
			release_all_writes = true;
		}
		state_cv.notify_all();
	}

	idx_t EnteredBackendWrites() {
		lock_guard<mutex> guard(state_lock);
		return entered_backend_writes;
	}

	idx_t MaxActiveBackendWrites() {
		lock_guard<mutex> guard(state_lock);
		return max_active_backend_writes;
	}

	vector<idx_t> AdmittedOffsets() {
		lock_guard<mutex> guard(state_lock);
		return admitted_offsets;
	}

	vector<idx_t> CompletedOffsets() {
		lock_guard<mutex> guard(state_lock);
		return completed_offsets;
	}

private:
	bool IsReleased(idx_t offset) const {
		for (auto released_offset : released_offsets) {
			if (released_offset == offset) {
				return true;
			}
		}
		return false;
	}

private:
	const bool block_backend_writes;
	mutex state_lock;
	std::condition_variable state_cv;
	vector<idx_t> admitted_offsets;
	vector<idx_t> released_offsets;
	vector<idx_t> completed_offsets;
	idx_t next_admission_offset = 0;
	idx_t entered_backend_writes = 0;
	idx_t active_backend_writes = 0;
	idx_t max_active_backend_writes = 0;
	bool release_all_writes = false;
};

class FailingBlockedWriteFileSystem : public TrackingWriteFileSystem {
public:
	FailingBlockedWriteFileSystem() : TrackingWriteFileSystem(false) {
	}

	string GetName() const override {
		return "FailingBlockedWriteFileSystem";
	}

	void Write(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override {
		if (nr_bytes == 0) {
			TrackingWriteFileSystem::Write(handle, buffer, nr_bytes, location);
			return;
		}

		idx_t write_id;
		{
			unique_lock<mutex> guard(block_lock);
			write_id = ++entered_writes;
			cv.notify_all();
			if (write_id == 1) {
				cv.wait(guard, [&]() { return fail_first_write; });
			} else if (write_id == 2) {
				cv.wait(guard, [&]() { return release_second_write; });
			}
		}

		if (write_id == 1) {
			throw IOException("Injected async write failure");
		}
		TrackingWriteFileSystem::Write(handle, buffer, nr_bytes, location);
	}

	bool WaitForEnteredWrites(idx_t count) {
		unique_lock<mutex> guard(block_lock);
		return cv.wait_for(guard, std::chrono::seconds(5), [&]() { return entered_writes >= count; });
	}

	void FailFirstWrite() {
		{
			lock_guard<mutex> guard(block_lock);
			fail_first_write = true;
		}
		cv.notify_all();
	}

	void ReleaseSecondWrite() {
		{
			lock_guard<mutex> guard(block_lock);
			release_second_write = true;
		}
		cv.notify_all();
	}

private:
	mutex block_lock;
	std::condition_variable cv;
	idx_t entered_writes = 0;
	bool fail_first_write = false;
	bool release_second_write = false;
};

class StringAsyncWriteBuffer : public AsyncWriteBuffer {
public:
	explicit StringAsyncWriteBuffer(string data_p) : data(std::move(data_p)) {
	}

	data_ptr_t Ptr() override {
		return data_ptr_cast(data.data());
	}

	idx_t Size() const override {
		return data.size();
	}

private:
	string data;
};

class BlockingMaterializationState {
public:
	void Materialize() {
		unique_lock<mutex> guard(lock);
		entered = true;
		cv.notify_all();
		cv.wait(guard, [&]() { return released; });
		if (fail) {
			throw IOException("Injected payload materialization failure");
		}
	}

	bool WaitForEntered() {
		unique_lock<mutex> guard(lock);
		return cv.wait_for(guard, std::chrono::seconds(5), [&]() { return entered; });
	}

	void Release(bool fail_p) {
		{
			lock_guard<mutex> guard(lock);
			fail = fail_p;
			released = true;
		}
		cv.notify_all();
	}

private:
	mutex lock;
	std::condition_variable cv;
	bool entered = false;
	bool released = false;
	bool fail = false;
};

class BlockingMaterializationAsyncWriteBuffer : public AsyncWriteBuffer {
public:
	BlockingMaterializationAsyncWriteBuffer(string data_p, shared_ptr<BlockingMaterializationState> state_p)
	    : data(std::move(data_p)), state(std::move(state_p)) {
	}

	data_ptr_t Ptr() override {
		state->Materialize();
		return data_ptr_cast(data.data());
	}

	idx_t Size() const override {
		return data.size();
	}

private:
	string data;
	shared_ptr<BlockingMaterializationState> state;
};

class TrackingAsyncWriteTarget : public AsyncWriteTarget {
public:
	void Write(data_ptr_t buffer, idx_t size, idx_t offset) override {
		lock_guard<mutex> guard(lock);
		writes.emplace_back(const_char_ptr_cast(buffer), UnsafeNumericCast<size_t>(size));
		offsets.push_back(offset);
	}

	mutex lock;
	vector<string> writes;
	vector<idx_t> offsets;
};

class FailingAsyncWriteTarget : public AsyncWriteTarget {
public:
	void Write(data_ptr_t buffer, idx_t size, idx_t offset) override {
		(void)buffer;
		(void)size;
		(void)offset;
		throw IOException("Injected managed queue failure");
	}
};

class BlockingAsyncWriteTarget : public AsyncWriteTarget {
public:
	void Write(data_ptr_t buffer, idx_t size, idx_t offset) override {
		(void)buffer;
		unique_lock<mutex> guard(lock);
		active_writes++;
		max_active_writes = MaxValue(max_active_writes, active_writes);
		entered_writes++;
		cv.notify_all();
		cv.wait(guard, [&]() { return release_writes; });
		write_sizes.push_back(size);
		offsets.push_back(offset);
		active_writes--;
		cv.notify_all();
	}

	bool WaitForEnteredWrites(idx_t count) {
		unique_lock<mutex> guard(lock);
		return cv.wait_for(guard, std::chrono::seconds(5), [&]() { return entered_writes >= count; });
	}

	void ReleaseWrites() {
		{
			lock_guard<mutex> guard(lock);
			release_writes = true;
		}
		cv.notify_all();
	}

	idx_t MaxActiveWrites() {
		lock_guard<mutex> guard(lock);
		return max_active_writes;
	}

	mutex lock;
	std::condition_variable cv;
	vector<idx_t> write_sizes;
	vector<idx_t> offsets;
	idx_t entered_writes = 0;
	idx_t active_writes = 0;
	idx_t max_active_writes = 0;
	bool release_writes = false;
};

class SequentialAsyncWriteStreamTarget : public ManagedAsyncWriteStreamTarget {
public:
	SequentialAsyncWriteStreamTarget(BlockingAsyncWriteTarget &target_p, bool local_file_p)
	    : target(target_p), local_file(local_file_p) {
	}

	FileWriteMode GetWriteMode() override {
		return FileWriteMode::SEQUENTIAL;
	}

	bool IsLocalFile() override {
		return local_file;
	}

	void Write(data_ptr_t, idx_t, idx_t) override {
		throw InternalException("Unexpected positional write to sequential test target");
	}

	void Write(data_ptr_t buffer, idx_t size) override {
		target.Write(buffer, size, offset);
		offset += size;
	}

private:
	BlockingAsyncWriteTarget &target;
	const bool local_file;
	idx_t offset = 0;
};

class BlockingAsyncTaskState {
public:
	bool WaitForStarted(idx_t count) {
		unique_lock<mutex> guard(lock);
		return cv.wait_for(guard, std::chrono::seconds(5), [&]() { return started_tasks >= count; });
	}

	void Release() {
		{
			lock_guard<mutex> guard(lock);
			released = true;
		}
		cv.notify_all();
	}

	void Enter() {
		unique_lock<mutex> guard(lock);
		started_tasks++;
		cv.notify_all();
		cv.wait(guard, [&]() { return released; });
	}

private:
	mutex lock;
	std::condition_variable cv;
	idx_t started_tasks = 0;
	bool released = false;
};

class BlockingAsyncTask : public BaseExecutorTask {
public:
	BlockingAsyncTask(TaskExecutor &executor, BlockingAsyncTaskState &state_p)
	    : BaseExecutorTask(executor), state(state_p) {
	}

	void ExecuteTask() override {
		state.Enter();
	}

private:
	BlockingAsyncTaskState &state;
};

class AsyncThreadBlocker {
public:
	AsyncThreadBlocker(ClientContext &context, idx_t task_count_p)
	    : task_count(task_count_p), executor(context, TaskSchedulerType::ASYNC) {
		for (idx_t task_idx = 0; task_idx < task_count; task_idx++) {
			executor.ScheduleTask(make_uniq<BlockingAsyncTask>(executor, state));
		}
	}

	~AsyncThreadBlocker() {
		Release();
	}

	bool WaitForStarted() {
		return state.WaitForStarted(task_count);
	}

	void Release() {
		if (released) {
			return;
		}
		state.Release();
		try {
			executor.WorkOnTasks();
		} catch (...) {
		}
		released = true;
	}

private:
	idx_t task_count;
	BlockingAsyncTaskState state;
	TaskExecutor executor;
	bool released = false;
};

static string ReadFile(const string &path) {
	LocalFileSystem fs;
	auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ);
	auto file_size = NumericCast<idx_t>(handle->GetFileSize());
	string result(file_size, '\0');
	handle->Read(data_ptr_cast(result.data()), file_size);
	return result;
}

static string CaptureException(const std::function<void()> &action) {
	try {
		action();
	} catch (const std::exception &ex) {
		return ex.what();
	} catch (...) {
		return "unknown exception";
	}
	return string();
}

static unique_ptr<Connection> CreateConnectionWithAsyncThreads(DuckDB &db, idx_t async_threads = 1) {
	auto con = make_uniq<Connection>(db);
	REQUIRE_NO_FAIL(con->Query("SET async_threads=" + to_string(async_threads)));
	return con;
}

static unique_ptr<Connection> CreateConnectionWithNoAsyncThreads(DuckDB &db) {
	auto con = make_uniq<Connection>(db);
	REQUIRE_NO_FAIL(con->Query("SET async_threads=0"));
	return con;
}

TEST_CASE("AsyncWriteQueue writes synchronously without async threads", "[async_write_queue]") {
	DuckDB db(nullptr);
	auto con = CreateConnectionWithNoAsyncThreads(db);
	TrackingAsyncWriteTarget target;
	AsyncWriteQueue queue(*con->context, target);

	mutex completion_lock;
	vector<idx_t> completion_offsets;
	vector<idx_t> completion_sizes;
	bool saw_error = false;
	auto completion = [&](idx_t offset, idx_t size, optional_ptr<const ErrorData> error) {
		lock_guard<mutex> guard(completion_lock);
		completion_offsets.push_back(offset);
		completion_sizes.push_back(size);
		saw_error = saw_error || error;
	};

	queue.Submit(AsyncWriteRequest(make_uniq<StringAsyncWriteBuffer>("abc"), 7, completion));
	queue.Close();

	REQUIRE(!queue.IsAsync());
	REQUIRE(target.writes.size() == 1);
	REQUIRE(target.writes[0] == "abc");
	REQUIRE(target.offsets[0] == 7);
	REQUIRE(completion_offsets.size() == 1);
	REQUIRE(completion_offsets[0] == 7);
	REQUIRE(completion_sizes[0] == 3);
	REQUIRE(!saw_error);
}

TEST_CASE("AsyncWriteQueue drains positional requests on multiple async threads", "[async_write_queue]") {
	DuckDB db(nullptr);
	auto con = CreateConnectionWithAsyncThreads(db, 2);
	BlockingAsyncWriteTarget target;
	AsyncWriteQueue queue(*con->context, target);

	mutex completion_lock;
	vector<idx_t> completion_offsets;
	vector<idx_t> completion_sizes;
	bool saw_error = false;
	auto completion = [&](idx_t offset, idx_t size, optional_ptr<const ErrorData> error) {
		lock_guard<mutex> guard(completion_lock);
		completion_offsets.push_back(offset);
		completion_sizes.push_back(size);
		saw_error = saw_error || error;
	};

	auto write_size = AsyncWriteConfig::TASK_BYTE_BUDGET + 1;
	queue.Submit(AsyncWriteRequest(
	    make_uniq<StringAsyncWriteBuffer>(string(UnsafeNumericCast<size_t>(write_size), 'a')), 0, completion));
	queue.Submit(AsyncWriteRequest(
	    make_uniq<StringAsyncWriteBuffer>(string(UnsafeNumericCast<size_t>(write_size), 'b')), write_size, completion));
	REQUIRE(target.WaitForEnteredWrites(2));
	REQUIRE(target.MaxActiveWrites() >= 2);

	target.ReleaseWrites();
	queue.Close();

	REQUIRE(target.write_sizes.size() == 2);
	REQUIRE(completion_offsets.size() == 2);
	REQUIRE(completion_sizes.size() == 2);
	REQUIRE(!saw_error);
	bool saw_first = false;
	bool saw_second = false;
	for (auto offset : completion_offsets) {
		if (offset == 0) {
			saw_first = true;
		}
		if (offset == write_size) {
			saw_second = true;
		}
	}
	REQUIRE(saw_first);
	REQUIRE(saw_second);
}

TEST_CASE("AsyncWriteQueue schedules a small prefix before a large pending request", "[async_write_queue]") {
	DuckDB db(nullptr);
	auto con = CreateConnectionWithAsyncThreads(db, 3);
	BlockingAsyncWriteTarget target;
	AsyncWriteQueue queue(*con->context, target);

	auto write_size = AsyncWriteConfig::TASK_BYTE_BUDGET + 1;
	queue.Submit(AsyncWriteRequest(make_uniq<StringAsyncWriteBuffer>(string(write_size, 'a')), 0));
	queue.Submit(AsyncWriteRequest(make_uniq<StringAsyncWriteBuffer>("header"), write_size));
	queue.Submit(AsyncWriteRequest(make_uniq<StringAsyncWriteBuffer>(string(write_size, 'b')), write_size + 6));

	auto saw_three_writes = target.WaitForEnteredWrites(3);
	target.ReleaseWrites();
	queue.Close();

	REQUIRE(saw_three_writes);
	REQUIRE(target.MaxActiveWrites() == 3);
	REQUIRE(target.write_sizes.size() == 3);
}

TEST_CASE("ManagedAsyncWriteQueue accepts non-contiguous positional writes", "[async_write_queue]") {
	DuckDB db(nullptr);
	auto con = CreateConnectionWithAsyncThreads(db, 2);
	TrackingAsyncWriteTarget target;
	ManagedAsyncWriteQueue queue(*con->context, target);

	queue.RegisterWrite(make_uniq<StringAsyncWriteBuffer>("abc"), 32);
	queue.RegisterWrite(make_uniq<StringAsyncWriteBuffer>("de"), 0);
	queue.Close();

	REQUIRE(target.writes.size() == 2);
	bool saw_first = false;
	bool saw_second = false;
	for (idx_t write_idx = 0; write_idx < target.writes.size(); write_idx++) {
		if (target.offsets[write_idx] == 32 && target.writes[write_idx] == "abc") {
			saw_first = true;
		}
		if (target.offsets[write_idx] == 0 && target.writes[write_idx] == "de") {
			saw_second = true;
		}
	}
	REQUIRE(saw_first);
	REQUIRE(saw_second);
}

TEST_CASE("ManagedAsyncWriteStreamQueue abort permits an in-flight completion refill", "[async_write_queue]") {
	DuckDB db(nullptr);
	auto con = CreateConnectionWithAsyncThreads(db);
	BlockingAsyncWriteTarget target;
	target.ReleaseWrites();
	SequentialAsyncWriteStreamTarget stream_target(target, true);
	ManagedAsyncWriteStreamQueue queue(*con->context, stream_target);
	auto &underlying_queue = ManagedAsyncWriteQueueTest::UnderlyingQueue(queue);
	REQUIRE(underlying_queue.IsAsync());
	mutex completion_lock;
	std::condition_variable completion_cv;
	bool completion_entered = false;
	bool release_completion = false;
	atomic<bool> completion_released {false};
	atomic<bool> refill_finished {false};
	// Stand in for a completion that already selected a refill before the stream was closed.
	auto completion = [&](idx_t, idx_t, optional_ptr<const ErrorData>) {
		{
			unique_lock<mutex> guard(completion_lock);
			completion_entered = true;
			completion_cv.notify_all();
			completion_released =
			    completion_cv.wait_for(guard, std::chrono::seconds(5), [&]() { return release_completion; });
		}
		if (completion_released) {
			refill_finished = ManagedAsyncWriteQueueTest::TryCompletionRefill(queue);
		}
	};
	underlying_queue.RegisterWrite(AsyncWriteRequest(make_uniq<StringAsyncWriteBuffer>("abc"), 0, completion));
	bool entered_completion;
	{
		unique_lock<mutex> guard(completion_lock);
		entered_completion =
		    completion_cv.wait_for(guard, std::chrono::seconds(5), [&]() { return completion_entered; });
	}

	queue.RegisterWrite(make_uniq<StringAsyncWriteBuffer>("pending"), 0,
	                    ManagedAsyncWriteStreamQueue::ScheduleMode::DEFER);
	auto pending_bytes = ManagedAsyncWriteQueueTest::PendingBytes(queue);

	string abort_error;
	std::thread abort_thread([&]() { abort_error = CaptureException([&]() { queue.AbortWrites(); }); });
	// Closing the underlying queue follows closing the stream and discarding its pending writes.
	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	bool underlying_closed = false;
	while (std::chrono::steady_clock::now() < deadline) {
		if (ManagedAsyncWriteQueueTest::IsClosed(underlying_queue)) {
			underlying_closed = true;
			break;
		}
		std::this_thread::yield();
	}
	{
		lock_guard<mutex> guard(completion_lock);
		release_completion = true;
		completion_cv.notify_all();
	}
	abort_thread.join();

	REQUIRE(entered_completion);
	REQUIRE(underlying_closed);
	REQUIRE(completion_released);
	REQUIRE(refill_finished);
	REQUIRE(pending_bytes == 7);
	REQUIRE(abort_error.empty());
	REQUIRE(ManagedAsyncWriteQueueTest::PendingBytes(queue) == 0);
	REQUIRE(ManagedAsyncWriteQueueTest::ExternalPendingBytes(underlying_queue) == 0);
	REQUIRE(target.write_sizes == vector<idx_t> {3});
}

TEST_CASE("ManagedAsyncWriteQueue reports accounted request adoption ownership", "[async_write_queue]") {
	DuckDB db(nullptr);
	auto con = CreateConnectionWithAsyncThreads(db);
	{
		TrackingAsyncWriteTarget target;
		ManagedAsyncWriteQueue queue(*con->context, target);
		idx_t completion_count = 0;
		bool completion_error = false;
		auto completion = [&](idx_t, idx_t, optional_ptr<const ErrorData> error) {
			completion_count++;
			completion_error = completion_error || error;
		};
		AsyncWriteRequest request(make_uniq<StringAsyncWriteBuffer>("abc"), 7, completion);

		ManagedAsyncWriteQueueTest::AddExternalPendingBytes(queue, 3);
		ErrorData acceptance_error;
		auto adopted = ManagedAsyncWriteQueueTest::TryAdopt(queue, request, acceptance_error);
		REQUIRE(adopted);
		REQUIRE(!acceptance_error.HasError());
		REQUIRE(request.payload == nullptr);
		REQUIRE(ManagedAsyncWriteQueueTest::ExternalPendingBytes(queue) == 0);
		REQUIRE(ManagedAsyncWriteQueueTest::PendingBytes(queue) == 3);

		queue.SchedulePendingWrites(ManagedAsyncWriteQueue::SchedulePolicy::FORCE);
		queue.Close();
		REQUIRE(target.writes == vector<string> {"abc"});
		REQUIRE(target.offsets == vector<idx_t> {7});
		REQUIRE(completion_count == 1);
		REQUIRE(!completion_error);
	}

	{
		FailingAsyncWriteTarget target;
		ManagedAsyncWriteQueue queue(*con->context, target);
		queue.RegisterWrite(make_uniq<StringAsyncWriteBuffer>("failed"), 0);
		auto wait_error = CaptureException([&]() { queue.WaitAll(); });
		REQUIRE(wait_error.find("Injected managed queue failure") != string::npos);

		idx_t completion_count = 0;
		AsyncWriteRequest request(make_uniq<StringAsyncWriteBuffer>("abc"), 7,
		                          [&](idx_t, idx_t, optional_ptr<const ErrorData>) { completion_count++; });
		ManagedAsyncWriteQueueTest::AddExternalPendingBytes(queue, 3);
		ErrorData rejection_error;
		auto adopted = ManagedAsyncWriteQueueTest::TryAdopt(queue, request, rejection_error);
		REQUIRE(!adopted);
		REQUIRE(rejection_error.HasError());
		REQUIRE(request.payload != nullptr);
		REQUIRE(ManagedAsyncWriteQueueTest::ExternalPendingBytes(queue) == 3);
		REQUIRE(ManagedAsyncWriteQueueTest::PendingBytes(queue) == 0);
		REQUIRE(completion_count == 0);

		ManagedAsyncWriteQueueTest::DiscardExternalPendingBytes(queue, 3);
		auto close_error = CaptureException([&]() { queue.Close(); });
		REQUIRE(close_error == wait_error);
	}
}

static void TestQueuedDrainTaskCoversNewTinyTail(bool local_file, const string &path_name) {
	DuckDB db(nullptr);
	auto con = CreateConnectionWithAsyncThreads(db, 2);
	auto &scheduler = TaskScheduler::GetScheduler(*con->context);
	AsyncThreadBlocker async_thread_blocker(*con->context, NumericCast<idx_t>(scheduler.NumberOfAsyncThreads()));
	REQUIRE(async_thread_blocker.WaitForStarted());

	BlockingWriteFileSystem fs(FileWriteMode::POSITIONAL, local_file);
	auto path = TestCreatePath(path_name);
	if (fs.FileExists(path)) {
		fs.RemoveFile(path);
	}

	string large(AsyncWriteConfig::DRAIN_TASK_BYTE_BUDGET * 2 + 1, 'x');
	string tail = "tail";

	AsyncFileWriter writer(*con->context, fs, path);
	writer.WriteData(make_uniq<StringAsyncWriteBuffer>(large));
	writer.WriteData(make_uniq<StringAsyncWriteBuffer>(tail));
	auto blocked_before_release = fs.BlockedWrites();
	CHECK(blocked_before_release <= 1);

	async_thread_blocker.Release();
	if (blocked_before_release == 0) {
		CHECK(fs.WaitForBlockedWrites(1));
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	CHECK(fs.BlockedWrites() == 1);

	fs.ReleaseWrites();
	writer.Close();
	REQUIRE(fs.write_sizes.size() == 2);
	REQUIRE(fs.write_offsets.size() == 2);
	bool saw_large = false;
	bool saw_tail = false;
	for (idx_t write_idx = 0; write_idx < fs.write_sizes.size(); write_idx++) {
		if (fs.write_sizes[write_idx] == large.size() && fs.write_offsets[write_idx] == 0) {
			saw_large = true;
		}
		if (fs.write_sizes[write_idx] == tail.size() && fs.write_offsets[write_idx] == large.size()) {
			saw_tail = true;
		}
	}
	REQUIRE(saw_large);
	REQUIRE(saw_tail);
	fs.RemoveFile(path);
}

} // namespace

TEST_CASE("AsyncFileWriter requires a client context", "[async_file_writer]") {
	TrackingWriteFileSystem fs;
	auto path = TestCreatePath("async_file_writer_no_context.tmp");
	if (fs.FileExists(path)) {
		fs.RemoveFile(path);
	}

	REQUIRE_THROWS_AS(AsyncFileWriter(QueryContext(), fs, path), InvalidInputException);
	REQUIRE(!fs.FileExists(path));
}

TEST_CASE("CSVWriter applies backpressure on automatic async flushes", "[async_file_writer][csv_writer]") {
	DuckDB db(nullptr);
	auto con = make_uniq<Connection>(db);
	REQUIRE_NO_FAIL(con->Query("SET threads=1"));
	REQUIRE_NO_FAIL(con->Query("SET async_threads=1"));

	AsyncThreadBlocker async_thread_blocker(*con->context, 1);
	REQUIRE(async_thread_blocker.WaitForStarted());

	BlockingWriteFileSystem fs(FileWriteMode::POSITIONAL, false);
	auto path = TestCreatePath("csv_writer_async_backpressure.tmp");
	fs.TryRemoveFile(path);

	AsyncFileWriter file_writer(*con->context, fs, path);
	CSVReaderOptions options;
	options.force_quote.push_back(false);
	CSVWriter writer(options, file_writer);
	CSVWriterState local_state(*con->context, writer.writer_options.flush_size);
	string data(writer.writer_options.flush_size, 'x');
	DataChunk input;
	input.Initialize(Allocator::Get(*con->context), {LogicalType::VARCHAR});
	input.SetChildCardinality(1);
	input.data[0].SetValue(0, Value(data));
	auto &scheduler = TaskScheduler::GetScheduler(*con->context);
	const auto regular_threads = MaxValue<idx_t>(NumericCast<idx_t>(scheduler.NumberOfThreads()), 1);
	const auto max_pending_bytes = ManagedAsyncMemoryConfig::MAX_PENDING_BYTES_PER_THREAD * regular_threads;
	const auto write_count = (max_pending_bytes + 2 * AsyncWriteConfig::DRAIN_TASK_BYTE_BUDGET) / data.size() + 1;

	atomic<bool> write_finished {false};
	std::exception_ptr write_error;
	std::thread write_thread([&]() {
		try {
			for (idx_t write_idx = 0; write_idx < write_count; write_idx++) {
				writer.WriteChunk(input, local_state);
			}
		} catch (...) {
			write_error = std::current_exception();
		}
		write_finished = true;
	});

	auto entered_backend = fs.WaitForBlockedWrites(1);
	auto finished_before_release = write_finished.load();
	fs.ReleaseWrites();
	async_thread_blocker.Release();
	write_thread.join();

	REQUIRE(entered_backend);
	REQUIRE(!finished_before_release);
	if (write_error) {
		std::rethrow_exception(write_error);
	}
	file_writer.Close();
	REQUIRE(file_writer.GetTotalWritten() == writer.BytesWritten());
	fs.RemoveFile(path);
}

TEST_CASE("AsyncFileWriter batches write registration before scheduling", "[async_file_writer]") {
	DuckDB db(nullptr);
	auto con = CreateConnectionWithAsyncThreads(db);
	TrackingWriteFileSystem fs;
	auto path = TestCreatePath("async_file_writer_batch.tmp");
	if (fs.FileExists(path)) {
		fs.RemoveFile(path);
	}

	AsyncFileWriter writer(*con->context, fs, path);
	{
		auto batch_guard = writer.StartBatch();
		writer.WriteData(make_uniq<StringAsyncWriteBuffer>("abcd"));
		writer.WriteData(make_uniq<StringAsyncWriteBuffer>("efgh"));

		REQUIRE(writer.GetTotalWritten() == 8);
		writer.ApplyBackpressure();
		REQUIRE(fs.write_sizes.empty());
		batch_guard.Finish();
	}

	writer.Close();
	REQUIRE(ReadFile(path) == "abcdefgh");
	REQUIRE(fs.write_sizes.size() == 1);
	fs.RemoveFile(path);
}

TEST_CASE("AsyncFileWriter writes synchronously without async threads", "[async_file_writer]") {
	DuckDB db(nullptr);
	auto con = CreateConnectionWithNoAsyncThreads(db);
	TrackingWriteFileSystem fs;
	auto path = TestCreatePath("async_file_writer_sync.tmp");
	if (fs.FileExists(path)) {
		fs.RemoveFile(path);
	}

	AsyncFileWriter writer(*con->context, fs, path);
	writer.WriteData(const_data_ptr_cast("ab"), 2);
	writer.WriteData(make_uniq<StringAsyncWriteBuffer>("cd"));

	REQUIRE(writer.GetTotalWritten() == 4);
	REQUIRE(fs.write_sizes.empty());

	writer.Close();
	REQUIRE(ReadFile(path) == "abcd");
	REQUIRE(fs.write_sizes.size() == 1);
	REQUIRE(fs.write_sizes[0] == 4);
	fs.RemoveFile(path);
}

TEST_CASE("AsyncFileWriter preserves order around large copied writes", "[async_file_writer]") {
	DuckDB db(nullptr);
	auto con = CreateConnectionWithAsyncThreads(db);
	TrackingWriteFileSystem fs;
	auto path = TestCreatePath("async_file_writer_small_large_small.tmp");
	if (fs.FileExists(path)) {
		fs.RemoveFile(path);
	}

	AsyncFileWriter writer(*con->context, fs, path);
	{
		auto batch_guard = writer.StartBatch();
		string large(8192, 'x');
		writer.WriteData(const_data_ptr_cast("PAR1"), 4);
		writer.WriteData(const_data_ptr_cast(large.data()), large.size());
		writer.WriteData(const_data_ptr_cast("PARE"), 4);
		REQUIRE(writer.GetTotalWritten() == 8200);
		batch_guard.Finish();
	}
	writer.Close();

	REQUIRE(ReadFile(path) == "PAR1" + string(8192, 'x') + "PARE");
	fs.RemoveFile(path);
}

TEST_CASE("AsyncFileWriter copies transient WriteData input", "[async_file_writer]") {
	DuckDB db(nullptr);
	auto con = CreateConnectionWithNoAsyncThreads(db);
	TrackingWriteFileSystem fs;
	auto path = TestCreatePath("async_file_writer_copy.tmp");
	if (fs.FileExists(path)) {
		fs.RemoveFile(path);
	}

	string payload = "abcdef";
	AsyncFileWriter writer(*con->context, fs, path);
	writer.WriteData(const_data_ptr_cast(payload.data()), payload.size());
	payload = "XXXXXX";
	writer.Close();

	REQUIRE(ReadFile(path) == "abcdef");
	fs.RemoveFile(path);
}

TEST_CASE("AsyncFileWriter writes at truncated offset", "[async_file_writer]") {
	DuckDB db(nullptr);
	auto con = CreateConnectionWithAsyncThreads(db);
	TrackingWriteFileSystem fs;
	auto path = TestCreatePath("async_file_writer_truncate_write.tmp");
	if (fs.FileExists(path)) {
		fs.RemoveFile(path);
	}

	AsyncFileWriter writer(*con->context, fs, path);
	writer.WriteData(const_data_ptr_cast("abcdef"), 6);
	writer.Truncate(3);
	writer.WriteData(const_data_ptr_cast("XYZ"), 3);
	writer.Close();

	REQUIRE(ReadFile(path) == "abcXYZ");
	fs.RemoveFile(path);
}

TEST_CASE("AsyncFileWriter non-positional writes at truncated offset", "[async_file_writer]") {
	DuckDB db(nullptr);
	auto con = CreateConnectionWithAsyncThreads(db);
	NonPositionalWriteFileSystem fs;
	auto path = TestCreatePath("async_file_writer_non_positional_truncate_write.tmp");
	if (fs.FileExists(path)) {
		fs.RemoveFile(path);
	}

	AsyncFileWriter writer(*con->context, fs, path);
	writer.WriteData(const_data_ptr_cast("abcdef"), 6);
	writer.Truncate(3);
	writer.WriteData(const_data_ptr_cast("XYZ"), 3);
	writer.Close();

	REQUIRE(ReadFile(path) == "abcXYZ");
	REQUIRE(!fs.write_offsets.empty());
	for (auto offset : fs.write_offsets) {
		REQUIRE(offset == NumericLimits<idx_t>::Maximum());
	}
	fs.RemoveFile(path);
}

TEST_CASE("AsyncFileWriter flush preserves an open batch", "[async_file_writer]") {
	DuckDB db(nullptr);
	auto con = CreateConnectionWithAsyncThreads(db);
	TrackingWriteFileSystem fs;
	auto path = TestCreatePath("async_file_writer_flush_preserves_batch.tmp");
	if (fs.FileExists(path)) {
		fs.RemoveFile(path);
	}

	AsyncFileWriter writer(*con->context, fs, path);
	{
		auto batch_guard = writer.StartBatch();
		writer.WriteData(make_uniq<StringAsyncWriteBuffer>("ab"));

		writer.Flush();
		REQUIRE(fs.write_sizes.size() == 1);
		REQUIRE(fs.write_sizes[0] == 2);
		REQUIRE(fs.write_offsets[0] == 0);

		writer.WriteData(make_uniq<StringAsyncWriteBuffer>("cd"));
		REQUIRE(fs.write_sizes.size() == 1);
		batch_guard.Finish();
	}

	writer.Close();
	REQUIRE(ReadFile(path) == "abcd");
	REQUIRE(fs.write_sizes.size() == 2);
	fs.RemoveFile(path);
}

TEST_CASE("AsyncFileWriter drains positional writes on multiple async threads", "[async_file_writer]") {
	DuckDB db(nullptr);
	auto con = CreateConnectionWithAsyncThreads(db, 2);
	BlockingWriteFileSystem fs(FileWriteMode::POSITIONAL, false);
	auto path = TestCreatePath("async_file_writer_parallel_positional.tmp");
	if (fs.FileExists(path)) {
		fs.RemoveFile(path);
	}

	string first(AsyncWriteConfig::DRAIN_TASK_BYTE_BUDGET + 1, 'a');
	string second(AsyncWriteConfig::DRAIN_TASK_BYTE_BUDGET + 1, 'b');

	AsyncFileWriter writer(*con->context, fs, path);
	{
		auto batch_guard = writer.StartBatch();
		writer.WriteData(make_uniq<StringAsyncWriteBuffer>(first));
		writer.WriteData(make_uniq<StringAsyncWriteBuffer>(second));
		batch_guard.Finish();
	}

	auto saw_two_blocked_writes = fs.WaitForBlockedWrites(2);
	auto max_active_writes = fs.MaxActiveWrites();
	fs.ReleaseWrites();
	writer.Close();

	REQUIRE(saw_two_blocked_writes);
	REQUIRE(max_active_writes >= 2);
	REQUIRE(ReadFile(path) == first + second);
	REQUIRE(fs.write_sizes.size() == 2);

	bool saw_first_offset = false;
	bool saw_second_offset = false;
	for (auto offset : fs.write_offsets) {
		if (offset == 0) {
			saw_first_offset = true;
		}
		if (offset == first.size()) {
			saw_second_offset = true;
		}
	}
	REQUIRE(saw_first_offset);
	REQUIRE(saw_second_offset);
	fs.RemoveFile(path);
}

TEST_CASE("AsyncFileWriter drains non-positional writes on one async thread", "[async_file_writer]") {
	DuckDB db(nullptr);
	auto con = CreateConnectionWithAsyncThreads(db, 2);
	BlockingWriteFileSystem fs(FileWriteMode::SEQUENTIAL, false);
	auto path = TestCreatePath("async_file_writer_parallel_non_positional.tmp");
	if (fs.FileExists(path)) {
		fs.RemoveFile(path);
	}

	string first(AsyncWriteConfig::DRAIN_TASK_BYTE_BUDGET + 1, 'a');
	string second(AsyncWriteConfig::DRAIN_TASK_BYTE_BUDGET + 1, 'b');

	AsyncFileWriter writer(*con->context, fs, path);
	{
		auto batch_guard = writer.StartBatch();
		writer.WriteData(make_uniq<StringAsyncWriteBuffer>(first));
		writer.WriteData(make_uniq<StringAsyncWriteBuffer>(second));
		batch_guard.Finish();
	}

	REQUIRE(fs.WaitForBlockedWrites(1));
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	auto blocked_writes = fs.BlockedWrites();
	auto max_active_writes = fs.MaxActiveWrites();

	fs.ReleaseWrites();
	writer.Close();

	REQUIRE(blocked_writes == 1);
	REQUIRE(max_active_writes == 1);
	REQUIRE(ReadFile(path) == first + second);
	REQUIRE(fs.write_sizes.size() == 2);
	for (auto offset : fs.write_offsets) {
		REQUIRE(offset == NumericLimits<idx_t>::Maximum());
	}
	fs.RemoveFile(path);
}

TEST_CASE("AsyncFileWriter admits concurrent-sequential writes in order before overlapping backend work",
          "[async_file_writer]") {
	DuckDB db(nullptr);
	auto con = CreateConnectionWithAsyncThreads(db, 2);
	ConcurrentSequentialWriteFileSystem fs;
	auto path = TestCreatePath("async_file_writer_parallel_concurrent_sequential.tmp");
	if (fs.FileExists(path)) {
		fs.RemoveFile(path);
	}

	string first(AsyncWriteConfig::DRAIN_TASK_BYTE_BUDGET + 1, 'a');
	string second(AsyncWriteConfig::DRAIN_TASK_BYTE_BUDGET + 1, 'b');

	AsyncFileWriter writer(*con->context, fs, path);
	{
		auto batch_guard = writer.StartBatch();
		writer.WriteData(make_uniq<StringAsyncWriteBuffer>(first));
		writer.WriteData(make_uniq<StringAsyncWriteBuffer>(second));
		batch_guard.Finish();
	}

	auto saw_two_backend_writes = fs.WaitForBackendWrites(2);
	auto admitted_offsets = fs.AdmittedOffsets();
	auto max_active_backend_writes = fs.MaxActiveBackendWrites();
	bool completed_later_write_first = false;
	if (saw_two_backend_writes) {
		fs.ReleaseWrite(first.size());
		if (fs.WaitForCompletedWrites(1)) {
			auto completed_offsets = fs.CompletedOffsets();
			completed_later_write_first = completed_offsets[0] == first.size();
		}
	}
	fs.ReleaseAllWrites();
	writer.Close();

	REQUIRE(saw_two_backend_writes);
	REQUIRE(admitted_offsets == vector<idx_t> {0, first.size()});
	REQUIRE(max_active_backend_writes >= 2);
	REQUIRE(completed_later_write_first);
	REQUIRE(ReadFile(path) == first + second);
	fs.RemoveFile(path);
}

TEST_CASE("AsyncFileWriter concurrent-sequential writes use the configured async worker capacity",
          "[async_file_writer]") {
	DuckDB db(nullptr);
	auto con = CreateConnectionWithAsyncThreads(db, 3);
	ConcurrentSequentialWriteFileSystem fs;
	auto path = TestCreatePath("async_file_writer_concurrent_sequential_worker_capacity.tmp");
	if (fs.FileExists(path)) {
		fs.RemoveFile(path);
	}

	auto write_size = AsyncWriteConfig::DRAIN_TASK_BYTE_BUDGET + 1;
	AsyncFileWriter writer(*con->context, fs, path);
	{
		auto batch_guard = writer.StartBatch();
		for (idx_t write_idx = 0; write_idx < 4; write_idx++) {
			writer.WriteData(make_uniq<StringAsyncWriteBuffer>(
			    string(UnsafeNumericCast<size_t>(write_size), UnsafeNumericCast<char>('a' + write_idx))));
		}
		batch_guard.Finish();
	}

	auto saw_three_backend_writes = fs.WaitForBackendWrites(3);
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	auto entered_before_release = fs.EnteredBackendWrites();
	fs.ReleaseAllWrites();
	writer.Close();

	REQUIRE(saw_three_backend_writes);
	REQUIRE(entered_before_release == 3);
	REQUIRE(fs.EnteredBackendWrites() == 4);
	REQUIRE(fs.MaxActiveBackendWrites() == 3);
	REQUIRE(fs.OpenFile(path, FileFlags::FILE_FLAGS_READ)->GetFileSize() == 4 * write_size);
	fs.RemoveFile(path);
}

TEST_CASE("AsyncFileWriter uses concurrent-sequential offsets without async threads", "[async_file_writer]") {
	DuckDB db(nullptr);
	auto con = CreateConnectionWithNoAsyncThreads(db);
	ConcurrentSequentialWriteFileSystem fs(false);
	auto path = TestCreatePath("async_file_writer_sync_concurrent_sequential.tmp");
	if (fs.FileExists(path)) {
		fs.RemoveFile(path);
	}

	AsyncFileWriter writer(*con->context, fs, path);
	writer.WriteData(const_data_ptr_cast("abcdef"), 6);
	writer.Close();

	REQUIRE(fs.AdmittedOffsets() == vector<idx_t> {0});
	REQUIRE(fs.write_offsets == vector<idx_t> {0});
	REQUIRE(ReadFile(path) == "abcdef");
	fs.RemoveFile(path);
}

TEST_CASE("AsyncFileWriter stops concurrent-sequential publication after refill materialization failure",
          "[async_file_writer]") {
	DuckDB db(nullptr);
	auto con = CreateConnectionWithAsyncThreads(db, 1);
	ConcurrentSequentialWriteFileSystem fs;
	auto path = TestCreatePath("async_file_writer_concurrent_sequential_materialization_error.tmp");
	if (fs.FileExists(path)) {
		fs.RemoveFile(path);
	}

	auto first_size = AsyncWriteConfig::DRAIN_TASK_BYTE_BUDGET + 1;
	auto second_size = AsyncWriteConfig::REMOTE_COALESCE_THRESHOLD + 1;
	auto materialization = make_shared_ptr<BlockingMaterializationState>();
	AsyncFileWriter writer(*con->context, fs, path);
	writer.WriteData(make_uniq<StringAsyncWriteBuffer>(string(UnsafeNumericCast<size_t>(first_size), 'a')));
	auto first_entered = fs.WaitForBackendWrites(1);
	writer.WriteData(make_uniq<BlockingMaterializationAsyncWriteBuffer>(
	    string(UnsafeNumericCast<size_t>(second_size), 'b'), materialization));
	writer.WriteData(make_uniq<StringAsyncWriteBuffer>("tail"));

	fs.ReleaseAllWrites();
	auto materialization_entered = materialization->WaitForEntered();
	std::atomic<bool> wait_started(false);
	std::atomic<bool> wait_finished(false);
	string wait_error;
	std::thread wait_thread([&]() {
		wait_started.store(true);
		wait_error = CaptureException([&]() { writer.WaitAll(); });
		wait_finished.store(true);
	});
	while (!wait_started.load()) {
		std::this_thread::yield();
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	auto wait_blocked_on_materialization = !wait_finished.load();
	materialization->Release(true);
	wait_thread.join();

	auto write_error = CaptureException([&]() { writer.WriteData(make_uniq<StringAsyncWriteBuffer>("later")); });
	auto close_error = CaptureException([&]() { writer.Close(); });
	auto repeated_close_error = CaptureException([&]() { writer.Close(); });

	REQUIRE(first_entered);
	REQUIRE(materialization_entered);
	REQUIRE(wait_blocked_on_materialization);
	REQUIRE(wait_error.find("Injected payload materialization failure") != string::npos);
	REQUIRE(write_error == wait_error);
	REQUIRE(close_error == wait_error);
	REQUIRE(repeated_close_error == wait_error);
	REQUIRE(fs.EnteredBackendWrites() == 1);
	if (fs.FileExists(path)) {
		fs.RemoveFile(path);
	}
}

TEST_CASE("AsyncFileWriter drains accepted concurrent-sequential writes before discarding a failed tail",
          "[async_file_writer]") {
	DuckDB db(nullptr);
	auto con = CreateConnectionWithAsyncThreads(db, 2);
	ConcurrentSequentialWriteFileSystem fs;
	auto path = TestCreatePath("async_file_writer_concurrent_sequential_accepted_error.tmp");
	if (fs.FileExists(path)) {
		fs.RemoveFile(path);
	}

	auto write_size = AsyncWriteConfig::DRAIN_TASK_BYTE_BUDGET + 1;
	AsyncFileWriter writer(*con->context, fs, path);
	writer.WriteData(make_uniq<StringAsyncWriteBuffer>(string(UnsafeNumericCast<size_t>(write_size), 'a')));
	writer.WriteData(make_uniq<StringAsyncWriteBuffer>(string(UnsafeNumericCast<size_t>(write_size), 'b')));
	auto accepted_writes_entered = fs.WaitForBackendWrites(2);
	writer.WriteData(make_uniq<StringAsyncWriteBuffer>("tail"));

	fs.fail_writes = true;
	fs.ReleaseAllWrites();
	auto close_error = CaptureException([&]() { writer.Close(); });
	auto repeated_close_error = CaptureException([&]() { writer.Close(); });

	REQUIRE(accepted_writes_entered);
	REQUIRE(close_error.find("Injected async write failure") != string::npos);
	REQUIRE(repeated_close_error == close_error);
	REQUIRE(fs.EnteredBackendWrites() == 2);
	if (fs.FileExists(path)) {
		fs.RemoveFile(path);
	}
}

TEST_CASE("AsyncFileWriter waits for remote coalesce threshold before first drain", "[async_file_writer]") {
	DuckDB db(nullptr);
	auto con = CreateConnectionWithAsyncThreads(db, 2);
	BlockingWriteFileSystem fs(FileWriteMode::POSITIONAL, false);
	auto path = TestCreatePath("async_file_writer_remote_coalesce_start.tmp");
	if (fs.FileExists(path)) {
		fs.RemoveFile(path);
	}

	string first(AsyncWriteConfig::REMOTE_COALESCE_THRESHOLD / 2, 'a');
	string second(AsyncWriteConfig::REMOTE_COALESCE_THRESHOLD / 2, 'b');

	AsyncFileWriter writer(*con->context, fs, path);
	writer.WriteData(make_uniq<StringAsyncWriteBuffer>(first));
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	REQUIRE(fs.BlockedWrites() == 0);

	writer.WriteData(make_uniq<StringAsyncWriteBuffer>(second));
	REQUIRE(fs.WaitForBlockedWrites(1));
	fs.ReleaseWrites();
	writer.Close();

	REQUIRE(ReadFile(path) == first + second);
	REQUIRE(fs.write_sizes.size() == 1);
	REQUIRE(fs.write_sizes[0] == AsyncWriteConfig::REMOTE_COALESCE_THRESHOLD);
	fs.RemoveFile(path);
}

TEST_CASE("AsyncFileWriter schedules a large remote write behind a small prefix", "[async_file_writer]") {
	DuckDB db(nullptr);
	auto con = CreateConnectionWithAsyncThreads(db, 4);
	ConcurrentSequentialWriteFileSystem fs;
	auto path = TestCreatePath("async_file_writer_remote_prefix_large.tmp");
	if (fs.FileExists(path)) {
		fs.RemoveFile(path);
	}

	string prefix = "header";
	string first(AsyncWriteConfig::DRAIN_TASK_BYTE_BUDGET + 1, 'a');
	string second(AsyncWriteConfig::DRAIN_TASK_BYTE_BUDGET + 1, 'b');
	string tail = "footer";
	AsyncFileWriter writer(*con->context, fs, path);
	{
		auto batch_guard = writer.StartBatch();
		writer.WriteData(make_uniq<StringAsyncWriteBuffer>(prefix));
		writer.WriteData(make_uniq<StringAsyncWriteBuffer>(first));
		writer.WriteData(make_uniq<StringAsyncWriteBuffer>(prefix));
		writer.WriteData(make_uniq<StringAsyncWriteBuffer>(second));
		writer.WriteData(make_uniq<StringAsyncWriteBuffer>(tail));
		batch_guard.Finish();
	}

	auto saw_four_backend_writes = fs.WaitForBackendWrites(4);
	auto admitted_offsets = fs.AdmittedOffsets();
	fs.ReleaseAllWrites();
	writer.Close();

	REQUIRE(saw_four_backend_writes);
	REQUIRE(admitted_offsets ==
	        vector<idx_t> {0, prefix.size(), prefix.size() + first.size(), 2 * prefix.size() + first.size()});
	REQUIRE(ReadFile(path) == prefix + first + prefix + second + tail);
	fs.RemoveFile(path);
}

TEST_CASE("AsyncFileWriter coalesces remote non-positional writes", "[async_file_writer]") {
	DuckDB db(nullptr);
	auto con = CreateConnectionWithAsyncThreads(db, 2);
	BlockingWriteFileSystem fs(FileWriteMode::SEQUENTIAL, false);
	auto path = TestCreatePath("async_file_writer_remote_non_positional_coalesce.tmp");
	if (fs.FileExists(path)) {
		fs.RemoveFile(path);
	}

	string first(AsyncWriteConfig::REMOTE_COALESCE_THRESHOLD / 2, 'a');
	string second(AsyncWriteConfig::REMOTE_COALESCE_THRESHOLD / 2, 'b');

	AsyncFileWriter writer(*con->context, fs, path);
	writer.WriteData(make_uniq<StringAsyncWriteBuffer>(first));
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	REQUIRE(fs.BlockedWrites() == 0);

	writer.WriteData(make_uniq<StringAsyncWriteBuffer>(second));
	REQUIRE(fs.WaitForBlockedWrites(1));
	fs.ReleaseWrites();
	writer.Close();

	REQUIRE(ReadFile(path) == first + second);
	REQUIRE(fs.write_sizes.size() == 1);
	REQUIRE(fs.write_sizes[0] == AsyncWriteConfig::REMOTE_COALESCE_THRESHOLD);
	REQUIRE(fs.write_offsets[0] == NumericLimits<idx_t>::Maximum());
	fs.RemoveFile(path);
}

TEST_CASE("AsyncFileWriter does not eagerly schedule tiny remote tails after one large write", "[async_file_writer]") {
	DuckDB db(nullptr);
	auto con = CreateConnectionWithAsyncThreads(db, 4);
	BlockingWriteFileSystem fs(FileWriteMode::POSITIONAL, false);
	auto path = TestCreatePath("async_file_writer_remote_large_then_tiny.tmp");
	if (fs.FileExists(path)) {
		fs.RemoveFile(path);
	}

	string large(AsyncWriteConfig::DRAIN_TASK_BYTE_BUDGET * 2 + 1, 'x');
	string tail = "tail";

	AsyncFileWriter writer(*con->context, fs, path);
	{
		auto batch_guard = writer.StartBatch();
		writer.WriteData(make_uniq<StringAsyncWriteBuffer>(large));
		writer.WriteData(make_uniq<StringAsyncWriteBuffer>(tail));
		batch_guard.Finish();
	}

	REQUIRE(fs.WaitForBlockedWrites(1));
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	REQUIRE(fs.BlockedWrites() == 1);

	fs.ReleaseWrites();
	writer.Close();
	REQUIRE(ReadFile(path) == large + tail);
	fs.RemoveFile(path);
}

TEST_CASE("AsyncFileWriter close force-drains remote non-positional tail after submitted write",
          "[async_file_writer]") {
	DuckDB db(nullptr);
	auto con = CreateConnectionWithAsyncThreads(db, 2);
	BlockingWriteFileSystem fs(FileWriteMode::SEQUENTIAL, false);
	auto path = TestCreatePath("async_file_writer_remote_non_positional_close_tail.tmp");
	if (fs.FileExists(path)) {
		fs.RemoveFile(path);
	}

	string large(AsyncWriteConfig::REMOTE_COALESCE_THRESHOLD + 17, 'x');
	string tail(1024, 't');

	AsyncFileWriter writer(*con->context, fs, path);
	writer.WriteData(make_uniq<StringAsyncWriteBuffer>(large));
	REQUIRE(fs.WaitForBlockedWrites(1));
	writer.WriteData(make_uniq<StringAsyncWriteBuffer>(tail));

	std::atomic<bool> close_started(false);
	std::exception_ptr close_error;
	std::thread close_thread([&]() {
		close_started.store(true);
		try {
			writer.Close();
		} catch (...) {
			close_error = std::current_exception();
		}
	});

	while (!close_started.load()) {
		std::this_thread::yield();
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	auto blocked_writes = fs.BlockedWrites();
	fs.ReleaseWrites();
	close_thread.join();
	if (close_error) {
		std::rethrow_exception(close_error);
	}

	REQUIRE(blocked_writes == 1);
	REQUIRE(ReadFile(path) == large + tail);
	for (auto offset : fs.write_offsets) {
		REQUIRE(offset == NumericLimits<idx_t>::Maximum());
	}
	fs.RemoveFile(path);
}

TEST_CASE("AsyncFileWriter close force-drains remote sequential tail after submitted write", "[async_file_writer]") {
	DuckDB db(nullptr);
	auto con = CreateConnectionWithAsyncThreads(db, 2);
	SequentialExplicitOffsetWriteFileSystem fs;
	auto path = TestCreatePath("async_file_writer_remote_sequential_close_tail.tmp");
	if (fs.FileExists(path)) {
		fs.RemoveFile(path);
	}

	string large(AsyncWriteConfig::REMOTE_COALESCE_THRESHOLD + 17, 'x');
	string tail(1024, 't');

	AsyncFileWriter writer(*con->context, fs, path);
	writer.WriteData(make_uniq<StringAsyncWriteBuffer>(large));
	REQUIRE(fs.WaitForBlockedWrites(1));
	writer.WriteData(make_uniq<StringAsyncWriteBuffer>(tail));

	std::atomic<bool> close_started(false);
	std::exception_ptr close_error;
	std::thread close_thread([&]() {
		close_started.store(true);
		try {
			writer.Close();
		} catch (...) {
			close_error = std::current_exception();
		}
	});

	while (!close_started.load()) {
		std::this_thread::yield();
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	fs.ReleaseWrites();
	close_thread.join();
	if (close_error) {
		std::rethrow_exception(close_error);
	}

	REQUIRE(ReadFile(path) == large + tail);
	fs.RemoveFile(path);
}

TEST_CASE("AsyncFileWriter remote queued drain task covers newly registered tiny tail", "[async_file_writer]") {
	TestQueuedDrainTaskCoversNewTinyTail(false, "async_file_writer_remote_queued_large_then_tiny.tmp");
}

TEST_CASE("AsyncFileWriter does not treat sequential explicit-offset writes as positional", "[async_file_writer]") {
	DuckDB db(nullptr);
	auto con = CreateConnectionWithAsyncThreads(db, 2);
	SequentialExplicitOffsetWriteFileSystem fs;
	auto path = TestCreatePath("async_file_writer_sequential_explicit_offset.tmp");
	if (fs.FileExists(path)) {
		fs.RemoveFile(path);
	}

	string first(AsyncWriteConfig::DRAIN_TASK_BYTE_BUDGET + 1, 'a');
	string second(AsyncWriteConfig::DRAIN_TASK_BYTE_BUDGET + 1, 'b');

	AsyncFileWriter writer(*con->context, fs, path);
	{
		auto batch_guard = writer.StartBatch();
		writer.WriteData(make_uniq<StringAsyncWriteBuffer>(first));
		writer.WriteData(make_uniq<StringAsyncWriteBuffer>(second));
		batch_guard.Finish();
	}

	auto saw_first_blocked_write = fs.WaitForBlockedWrites(1);
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	auto max_active_writes = fs.MaxActiveWrites();
	fs.ReleaseWrites();
	writer.Close();

	REQUIRE(saw_first_blocked_write);
	REQUIRE(max_active_writes == 1);
	REQUIRE(ReadFile(path) == first + second);
	fs.RemoveFile(path);
}

TEST_CASE("AsyncFileWriter rethrows non-positional async write errors on close", "[async_file_writer]") {
	DuckDB db(nullptr);
	auto con = CreateConnectionWithAsyncThreads(db, 1);
	BlockingWriteFileSystem fs(FileWriteMode::SEQUENTIAL, false);
	auto path = TestCreatePath("async_file_writer_non_positional_error.tmp");
	if (fs.FileExists(path)) {
		fs.RemoveFile(path);
	}
	fs.fail_writes = true;

	string first(AsyncWriteConfig::REMOTE_COALESCE_THRESHOLD + 17, 'x');

	AsyncFileWriter writer(*con->context, fs, path);
	writer.WriteData(make_uniq<StringAsyncWriteBuffer>(first));
	REQUIRE(fs.WaitForBlockedWrites(1));
	writer.WriteData(make_uniq<StringAsyncWriteBuffer>("tail"));

	fs.ReleaseWrites();
	try {
		writer.Close();
		FAIL("Expected async write failure");
	} catch (const Exception &ex) {
		string error = ex.what();
		REQUIRE(error.find("Async write failed for range [offset=0, size=") != string::npos);
		REQUIRE(error.find("Injected async write failure") != string::npos);
	}

	if (fs.FileExists(path)) {
		fs.RemoveFile(path);
	}
}

TEST_CASE("AsyncFileWriter close drains an open batch", "[async_file_writer]") {
	DuckDB db(nullptr);
	auto con = CreateConnectionWithAsyncThreads(db);
	TrackingWriteFileSystem fs;
	auto path = TestCreatePath("async_file_writer_close_batch.tmp");
	if (fs.FileExists(path)) {
		fs.RemoveFile(path);
	}

	AsyncFileWriter writer(*con->context, fs, path);
	auto batch_guard = writer.StartBatch();
	writer.WriteData(make_uniq<StringAsyncWriteBuffer>("abcd"));
	writer.Close();
	batch_guard.Finish();

	REQUIRE(ReadFile(path) == "abcd");
	fs.RemoveFile(path);
}

TEST_CASE("AsyncFileWriter rethrows asynchronous write errors on close", "[async_file_writer]") {
	DuckDB db(nullptr);
	auto con = CreateConnectionWithAsyncThreads(db);
	TrackingWriteFileSystem fs;
	auto path = TestCreatePath("async_file_writer_error.tmp");
	if (fs.FileExists(path)) {
		fs.RemoveFile(path);
	}
	fs.fail_writes = true;

	AsyncFileWriter writer(*con->context, fs, path);
	try {
		auto batch_guard = writer.StartBatch();
		writer.WriteData(make_uniq<StringAsyncWriteBuffer>("abcd"));
		writer.Close();
		FAIL("Expected async write failure");
	} catch (const Exception &ex) {
		string error = ex.what();
		REQUIRE(error.find("Async write failed for range [offset=0, size=4]") != string::npos);
		REQUIRE(error.find("Injected async write failure") != string::npos);
	}

	if (fs.FileExists(path)) {
		fs.RemoveFile(path);
	}
}

TEST_CASE("AsyncFileWriter close drains scheduled tasks after async write error", "[async_file_writer]") {
	DuckDB db(nullptr);
	auto con = CreateConnectionWithAsyncThreads(db, 2);
	FailingBlockedWriteFileSystem fs;
	auto path = TestCreatePath("async_file_writer_error_close_drains.tmp");
	if (fs.FileExists(path)) {
		fs.RemoveFile(path);
	}

	string first(AsyncWriteConfig::DRAIN_TASK_BYTE_BUDGET + 1, 'a');
	string second(AsyncWriteConfig::DRAIN_TASK_BYTE_BUDGET + 1, 'b');

	AsyncFileWriter writer(*con->context, fs, path);
	{
		auto batch_guard = writer.StartBatch();
		writer.WriteData(make_uniq<StringAsyncWriteBuffer>(first));
		writer.WriteData(make_uniq<StringAsyncWriteBuffer>(second));
		batch_guard.Finish();
	}
	REQUIRE(fs.WaitForEnteredWrites(2));

	writer.WriteData(const_data_ptr_cast("x"), 1);
	fs.FailFirstWrite();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	std::atomic<bool> close_started(false);
	std::atomic<bool> close_finished(false);
	std::exception_ptr close_error;
	std::thread close_thread([&]() {
		close_started.store(true);
		try {
			writer.Close();
		} catch (...) {
			close_error = std::current_exception();
		}
		close_finished.store(true);
	});

	while (!close_started.load()) {
		std::this_thread::yield();
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	REQUIRE(!close_finished.load());

	fs.ReleaseSecondWrite();
	close_thread.join();
	REQUIRE(close_finished.load());
	REQUIRE(close_error != nullptr);

	if (fs.FileExists(path)) {
		fs.RemoveFile(path);
	}
}

TEST_CASE("AsyncFileWriter close discards unscheduled writes after async write error", "[async_file_writer]") {
	DuckDB db(nullptr);
	auto con = CreateConnectionWithAsyncThreads(db, 1);
	FailingBlockedWriteFileSystem fs;
	auto path = TestCreatePath("async_file_writer_error_unscheduled_tail.tmp");
	if (fs.FileExists(path)) {
		fs.RemoveFile(path);
	}

	string first(AsyncWriteConfig::DRAIN_TASK_BYTE_BUDGET + 1, 'a');

	AsyncFileWriter writer(*con->context, fs, path);
	writer.WriteData(make_uniq<StringAsyncWriteBuffer>(first));
	REQUIRE(fs.WaitForEnteredWrites(1));
	writer.WriteData(make_uniq<StringAsyncWriteBuffer>("tail"));

	fs.FailFirstWrite();
	try {
		writer.Close();
		FAIL("Expected async write failure");
	} catch (const Exception &ex) {
		string error = ex.what();
		REQUIRE(error.find("Async write failed for range [offset=0, size=") != string::npos);
		REQUIRE(error.find("Injected async write failure") != string::npos);
	}

	if (fs.FileExists(path)) {
		fs.RemoveFile(path);
	}
}

TEST_CASE("AsyncFileWriter publishes only on successful explicit close", "[async_file_writer]") {
	DuckDB db(nullptr);
	auto con = CreateConnectionWithNoAsyncThreads(db);
	TrackingWriteFileSystem fs;
	auto path = TestCreatePath("async_file_writer_explicit_close.tmp");
	fs.TryRemoveFile(path);

	{
		auto flags = AsyncFileWriter::DEFAULT_OPEN_FLAGS | FileFlags::FILE_FLAGS_EXCLUSIVE_CREATE;
		AsyncFileWriter writer(*con->context, fs, path, flags);
		writer.WriteData(const_data_ptr_cast("published"), 9);
		writer.Close();
	}

	REQUIRE(fs.AbortCount() == 0);
	REQUIRE(ReadFile(path) == "published");
	fs.RemoveFile(path);
}

TEST_CASE("AsyncFileWriter rejects incorrect final file sizes", "[async_file_writer]") {
	for (auto async_threads : {0, 2}) {
		for (auto size_difference : {-32768, 32768}) {
			for (auto exclusive_create : {false, true}) {
				CAPTURE(async_threads, size_difference, exclusive_create);
				DuckDB db(nullptr);
				auto con = CreateConnectionWithAsyncThreads(db, async_threads);
				SizeMismatchWriteFileSystem fs(size_difference);
				auto path = TestCreatePath("async_file_writer_size_mismatch.tmp");
				fs.TryRemoveFile(path);

				auto flags = AsyncFileWriter::DEFAULT_OPEN_FLAGS;
				if (exclusive_create) {
					flags |= FileFlags::FILE_FLAGS_EXCLUSIVE_CREATE;
				}
				AsyncFileWriter writer(*con->context, fs, path, flags);
				writer.WriteData(make_uniq<StringAsyncWriteBuffer>(string(65536, 'x')));
				auto error = CaptureException([&]() { writer.Close(); });
				REQUIRE(error.find("File size mismatch") != string::npos);
				REQUIRE(error.find("expected 65536 bytes") != string::npos);
				REQUIRE(error.find("found " + to_string(65536 + size_difference) + " bytes") != string::npos);
				REQUIRE(CaptureException([&]() { writer.Close(); }) == error);
				REQUIRE(fs.AbortCount() == 1);
				REQUIRE(fs.FileExists(path) == !exclusive_create);
				fs.TryRemoveFile(path);
			}
		}
	}
}

TEST_CASE("AsyncFileWriter exclusive creation preserves existing files", "[async_file_writer]") {
	DuckDB db(nullptr);
	auto con = CreateConnectionWithNoAsyncThreads(db);
	TrackingWriteFileSystem fs;
	auto path = TestCreatePath("async_file_writer_exclusive_existing.tmp");
	fs.TryRemoveFile(path);
	{
		auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW);
		string contents = "existing";
		handle->Write(data_ptr_cast(contents.data()), contents.size());
		handle->Close();
	}

	auto flags = AsyncFileWriter::DEFAULT_OPEN_FLAGS | FileFlags::FILE_FLAGS_EXCLUSIVE_CREATE;
	REQUIRE_THROWS(AsyncFileWriter(*con->context, fs, path, flags));
	REQUIRE(ReadFile(path) == "existing");
	fs.RemoveFile(path);
}

TEST_CASE("AsyncFileWriter destruction preserves non-exclusive paths and removes exclusive paths",
          "[async_file_writer]") {
	DuckDB db(nullptr);
	auto con = CreateConnectionWithNoAsyncThreads(db);
	TrackingWriteFileSystem fs;
	auto existing_path = TestCreatePath("async_file_writer_preserved_abort.tmp");
	auto exclusive_path = TestCreatePath("async_file_writer_exclusive_abort.tmp");
	fs.TryRemoveFile(existing_path);
	fs.TryRemoveFile(exclusive_path);
	{
		auto handle = fs.OpenFile(existing_path, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW);
		handle->Close();
	}

	{
		AsyncFileWriter writer(*con->context, fs, existing_path);
		writer.WriteData(const_data_ptr_cast("pending"), 7);
	}
	REQUIRE(fs.FileExists(existing_path));

	{
		auto flags = AsyncFileWriter::DEFAULT_OPEN_FLAGS | FileFlags::FILE_FLAGS_EXCLUSIVE_CREATE;
		AsyncFileWriter writer(*con->context, fs, exclusive_path, flags);
		writer.WriteData(const_data_ptr_cast("pending"), 7);
	}
	REQUIRE(!fs.FileExists(exclusive_path));
	REQUIRE(fs.AbortCount() == 2);
	fs.RemoveFile(existing_path);
}

TEST_CASE("AsyncFileWriter preserves ambiguous publications", "[async_file_writer]") {
	DuckDB db(nullptr);
	auto con = CreateConnectionWithNoAsyncThreads(db);
	PublishingWriteFileSystem fs;
	fs.fail_close_after_publish = true;

	{
		AsyncFileWriter writer(*con->context, fs, "ambiguous-publication");
		writer.WriteData(const_data_ptr_cast("published"), 9);
		auto error = CaptureException([&]() { writer.Close(); });
		REQUIRE(error.find("Injected ambiguous publish failure") != string::npos);
	}

	REQUIRE(fs.written_bytes == 9);
	REQUIRE(fs.publish_count == 1);
	REQUIRE(fs.abort_count == 0);
}

TEST_CASE("AsyncFileWriter preserves the first write error over abort cleanup", "[async_file_writer]") {
	DuckDB db(nullptr);
	auto con = CreateConnectionWithNoAsyncThreads(db);
	TrackingWriteFileSystem fs;
	fs.fail_writes = true;
	fs.fail_abort = true;
	auto path = TestCreatePath("async_file_writer_primary_error.tmp");
	fs.TryRemoveFile(path);
	auto flags = AsyncFileWriter::DEFAULT_OPEN_FLAGS | FileFlags::FILE_FLAGS_EXCLUSIVE_CREATE;
	AsyncFileWriter writer(*con->context, fs, path, flags);
	auto payload = string(2 * AsyncWriteConfig::COPIED_BUFFER_CAPACITY, 'x');

	auto write_error =
	    CaptureException([&]() { writer.WriteData(make_uniq<StringAsyncWriteBuffer>(std::move(payload))); });
	auto close_error = CaptureException([&]() { writer.Close(); });
	REQUIRE(write_error.find("Injected async write failure") != string::npos);
	REQUIRE(close_error.find("Injected async write failure") != string::npos);
	REQUIRE(close_error.find("Injected abort failure") == string::npos);
	REQUIRE(fs.AbortCount() == 1);
	REQUIRE(!fs.FileExists(path));
}

TEST_CASE("AsyncFileWriter construction aborts an acquired handle", "[async_file_writer]") {
	DuckDB db(nullptr);
	auto con = CreateConnectionWithNoAsyncThreads(db);
	TrackingWriteFileSystem fs;
	fs.fail_get_write_mode = true;
	fs.fail_abort = true;
	auto path = TestCreatePath("async_file_writer_constructor_abort.tmp");
	fs.TryRemoveFile(path);
	auto flags = AsyncFileWriter::DEFAULT_OPEN_FLAGS | FileFlags::FILE_FLAGS_EXCLUSIVE_CREATE;

	auto error = CaptureException([&]() { AsyncFileWriter writer(*con->context, fs, path, flags); });
	REQUIRE(error.find("Injected write mode failure") != string::npos);
	REQUIRE(error.find("Injected abort failure") == string::npos);
	REQUIRE(fs.AbortCount() == 1);
	REQUIRE(!fs.FileExists(path));
}

TEST_CASE("AsyncFileWriter completes and accounts partial sequential writes", "[async_file_writer]") {
	DuckDB db(nullptr);
	auto con = CreateConnectionWithNoAsyncThreads(db);
	PartialSequentialWriteFileSystem fs;
	auto path = TestCreatePath("async_file_writer_partial_sequential.tmp");
	fs.TryRemoveFile(path);
	auto initial_bytes_written = QueryProfiler::Get(*con->context).GetBytesWritten();

	AsyncFileWriter writer(*con->context, fs, path);
	writer.WriteData(const_data_ptr_cast("abcdef"), 6);
	writer.Close();

	REQUIRE(fs.write_sizes == vector<idx_t> {2, 2, 2});
	REQUIRE(QueryProfiler::Get(*con->context).GetBytesWritten() - initial_bytes_written == 6);
	REQUIRE(ReadFile(path) == "abcdef");
	fs.RemoveFile(path);
}

TEST_CASE("AsyncFileWriter rejects negative sequential writes without accounting", "[async_file_writer]") {
	DuckDB db(nullptr);
	auto con = CreateConnectionWithNoAsyncThreads(db);
	PartialSequentialWriteFileSystem fs;
	fs.return_negative = true;
	auto path = TestCreatePath("async_file_writer_negative_sequential.tmp");
	fs.TryRemoveFile(path);
	auto initial_bytes_written = QueryProfiler::Get(*con->context).GetBytesWritten();

	{
		AsyncFileWriter writer(*con->context, fs, path);
		writer.WriteData(const_data_ptr_cast("abcdef"), 6);
		auto error = CaptureException([&]() { writer.Close(); });
		REQUIRE(error.find("Failed to write to file") != string::npos);
		REQUIRE(QueryProfiler::Get(*con->context).GetBytesWritten() == initial_bytes_written);
	}
	fs.TryRemoveFile(path);
}

TEST_CASE("Compressed initialization failure aborts the acquired child", "[async_file_writer]") {
	TrackingWriteFileSystem child_fs;
	FailingInitializeCompressedFileSystem compressed_fs;
	auto path = TestCreatePath("compressed_file_initialize_abort.tmp");
	child_fs.TryRemoveFile(path);

	{
		auto child = child_fs.OpenFile(path, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW |
		                                         FileFlags::FILE_FLAGS_EXCLUSIVE_CREATE);
		auto file = make_uniq<CompressedFile>(compressed_fs, std::move(child), path);
		REQUIRE_THROWS(file->Initialize(QueryContext(), true));
	}

	REQUIRE(child_fs.AbortCount() == 1);
	REQUIRE(!child_fs.FileExists(path));
}

TEST_CASE("Compressed AsyncFileWriter abort removes an exclusively created output", "[async_file_writer]") {
	DuckDB db(nullptr);
	auto con = CreateConnectionWithNoAsyncThreads(db);
	auto &fs = FileSystem::GetFileSystem(*con->context);
	auto path = TestCreatePath("async_file_writer_compressed_abort.gz");
	fs.TryRemoveFile(path);

	{
		auto flags =
		    AsyncFileWriter::DEFAULT_OPEN_FLAGS | FileFlags::FILE_FLAGS_EXCLUSIVE_CREATE | FileCompressionType::GZIP;
		AsyncFileWriter writer(*con->context, fs, path, flags);
		writer.WriteData(const_data_ptr_cast("pending"), 7);
	}

	REQUIRE(!fs.FileExists(path));
}

TEST_CASE("AsyncFileWriter abort drains an accepted active write", "[async_file_writer]") {
	DuckDB db(nullptr);
	auto con = CreateConnectionWithAsyncThreads(db);
	BlockingWriteFileSystem fs(FileWriteMode::POSITIONAL, false);
	auto path = TestCreatePath("async_file_writer_running_abort.tmp");
	fs.TryRemoveFile(path);
	AsyncFileWriter writer(*con->context, fs, path);
	writer.WriteData(make_uniq<StringAsyncWriteBuffer>(string(AsyncWriteConfig::DRAIN_TASK_BYTE_BUDGET, 'a')));
	REQUIRE(fs.WaitForBlockedWrites(1));

	atomic<bool> abort_finished {false};
	std::exception_ptr abort_error;
	std::thread abort_thread([&]() {
		try {
			writer.AbortWrite();
		} catch (...) {
			abort_error = std::current_exception();
		}
		abort_finished = true;
	});
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	REQUIRE(!abort_finished.load());

	fs.ReleaseWrites();
	abort_thread.join();
	REQUIRE(abort_error == nullptr);
	REQUIRE(abort_finished.load());
	REQUIRE(fs.AbortCount() == 1);
	REQUIRE(fs.write_sizes.size() == 1);
	fs.RemoveFile(path);
}
