#include "catch.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/local_file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parallel/task_executor.hpp"
#include "test_helpers.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>

using namespace duckdb;

namespace {

static std::atomic<idx_t> total_reads {0};
static std::atomic<idx_t> data_reads {0};

//! Serves read-ahead-fault://<name> from a local file and fails every read of row-group data.
//! Footer reads pass so files open, the first I/O task then fails, and the opens queued behind it are retired.
class ReadAheadFaultFileSystem : public LocalFileSystem {
public:
	//! Resolve the test directory once, on the test's own thread: the query runs on other threads, where the
	//! test helpers cannot tell which test is active and would resolve a different directory
	ReadAheadFaultFileSystem() : local_prefix(TestCreatePath("read_ahead_fault_")) {
	}

	static const string &Prefix() {
		static const string prefix = "read-ahead-fault://";
		return prefix;
	}

	string MapPath(const string &path) const {
		if (!StringUtil::StartsWith(path, Prefix())) {
			return path;
		}
		return local_prefix + path.substr(Prefix().size());
	}

	string GetName() const override {
		return "ReadAheadFaultFileSystem";
	}

	bool CanHandleFile(const string &path) override {
		return StringUtil::StartsWith(path, Prefix());
	}

	//! Pretend to be remote, so the scan prefetches through read-ahead I/O tasks instead of reading inline
	bool IsLocalFileSystem() const override {
		return false;
	}

	bool FileExists(const string &path, optional_ptr<FileOpener> opener) override {
		return LocalFileSystem::FileExists(MapPath(path), opener);
	}

	vector<OpenFileInfo> Glob(const string &path, FileOpener *opener) override {
		if (!CanHandleFile(path)) {
			return LocalFileSystem::Glob(path, opener);
		}
		vector<OpenFileInfo> result;
		if (LocalFileSystem::FileExists(MapPath(path), opener)) {
			result.emplace_back(path);
		}
		return result;
	}

	unique_ptr<FileHandle> OpenFile(const string &path, FileOpenFlags flags, optional_ptr<FileOpener> opener) override {
		return LocalFileSystem::OpenFile(MapPath(path), flags, opener);
	}

	void Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override {
		++total_reads;
		if (IsDataRead(handle, nr_bytes, location)) {
			++data_reads;
			throw IOException("injected read-ahead I/O failure");
		}
		LocalFileSystem::Read(handle, buffer, nr_bytes, location);
	}

private:
	bool IsDataRead(FileHandle &handle, int64_t nr_bytes, idx_t location) {
		if (!StringUtil::StartsWith(handle.GetPath(), local_prefix)) {
			return false;
		}
		// the footer sits at the end of the file, row-group data well before it
		auto file_size = NumericCast<idx_t>(LocalFileSystem::GetFileSize(handle));
		return location + NumericCast<idx_t>(nr_bytes) < file_size / 2;
	}

	const string local_prefix;
};

class AsyncPoolBlockerState {
public:
	bool WaitForStarted() {
		unique_lock<mutex> guard(lock);
		return cv.wait_for(guard, std::chrono::seconds(5), [&]() { return started; });
	}

	void Enter() {
		unique_lock<mutex> guard(lock);
		started = true;
		cv.notify_all();
		cv.wait(guard, [&]() { return released; });
	}

	void Release() {
		{
			lock_guard<mutex> guard(lock);
			released = true;
		}
		cv.notify_all();
	}

private:
	mutex lock;
	std::condition_variable cv;
	bool started = false;
	bool released = false;
};

class AsyncPoolBlockerTask : public BaseExecutorTask {
public:
	AsyncPoolBlockerTask(TaskExecutor &executor, AsyncPoolBlockerState &state_p)
	    : BaseExecutorTask(executor), state(state_p) {
	}

	void ExecuteTask() override {
		state.Enter();
	}

private:
	AsyncPoolBlockerState &state;
};

//! Parks the only async worker, so every read-ahead task runs inline on the scan thread in queue order
class AsyncPoolBlocker {
public:
	explicit AsyncPoolBlocker(ClientContext &context) : executor(context, TaskSchedulerType::ASYNC) {
		executor.ScheduleTask(make_uniq<AsyncPoolBlockerTask>(executor, state));
	}

	~AsyncPoolBlocker() {
		Release();
	}

	bool WaitForStarted() {
		return state.WaitForStarted();
	}

	void Release() {
		if (released) {
			return;
		}
		state.Release();
		executor.WorkOnTasks();
		released = true;
	}

private:
	AsyncPoolBlockerState state;
	TaskExecutor executor;
	bool released = false;
};

} // namespace

TEST_CASE("Read-ahead reports an async I/O failure instead of hanging or ending the scan early", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	REQUIRE_NO_FAIL(con.Query("SET async_threads=1"));
	// a fixed depth has no memory governor, so the scan produces every job before it claims one and never parks
	// on I/O that only the parked worker could run
	REQUIRE_NO_FAIL(con.Query("SET read_ahead_depth=64"));

	// a few MB per file, so row-group reads sit well away from the footer and cannot ride along with it
	auto fault_fs = make_uniq<ReadAheadFaultFileSystem>();
	const vector<string> names {"a.parquet", "b.parquet", "c.parquet"};
	string file_list;
	for (auto &name : names) {
		auto path = fault_fs->MapPath(ReadAheadFaultFileSystem::Prefix() + name);
		REQUIRE_NO_FAIL(con.Query(StringUtil::Format(
		    "COPY (SELECT hash(i) AS h, i FROM range(400000) t(i)) TO '%s' (FORMAT parquet)", path)));
		file_list +=
		    (file_list.empty() ? "" : ", ") + StringUtil::Format("'%s%s'", ReadAheadFaultFileSystem::Prefix(), name);
	}
	FileSystem::GetFileSystem(*con.context).RegisterSubSystem(std::move(fault_fs));

	// with the worker parked the scan thread drains the queue itself, in order: the first file's I/O fails and
	// records the error, and the file open queued behind it is retired instead of run, leaving that reader in
	// OPENING - exactly the state the scan then waits on
	AsyncPoolBlocker blocker(*con.context);
	REQUIRE(blocker.WaitForStarted());

	auto pending = std::async(std::launch::async, [&]() {
		// read a real column: count(*) is answered from the footer and never touches row-group data
		return con.Query("SELECT sum(h) FROM read_parquet([" + file_list + "])");
	});
	// a hang trips the timeout, a short read produces rows instead of an error, only a thrown error passes both
	const bool finished = pending.wait_for(std::chrono::seconds(60)) == std::future_status::ready;
	if (!finished) {
		// unwind a hung scan so the failure is an assertion, not a process that never exits
		con.Interrupt();
	}
	auto result = pending.get();
	INFO("reads seen by the fault file system: " << total_reads.load() << ", data reads: " << data_reads.load()
	                                             << ", result: " << (result->HasError() ? result->GetError() : "ok"));
	REQUIRE(finished);
	REQUIRE(result->HasError());
	REQUIRE(StringUtil::Contains(result->GetError(), "injected read-ahead I/O failure"));
	blocker.Release();
}
