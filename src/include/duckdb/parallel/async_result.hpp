//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parallel/async_result.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/enums/operator_result_type.hpp"
#include "duckdb/common/enums/task_scheduler_type.hpp"

namespace duckdb {

class ClientContext;
class DataChunk;
class InterruptState;
class TaskExecutor;
class Executor;

enum class AsyncResultsExecutionMode : uint8_t {
	SYNCHRONOUS,  //! BLOCKED should not bubble up, and they should be executed synchronously
	TASK_EXECUTOR //! BLOCKED is allowed
};

class AsyncTask {
public:
	virtual ~AsyncTask() {};
	virtual void Execute() = 0;
	//! The number of bytes this task reads, when known (used to budget I/O scheduled ahead by the read-ahead)
	virtual idx_t GetIOSize() const {
		return 0;
	}
};

class AsyncResult {
	explicit AsyncResult(AsyncResultType t);

public:
	AsyncResult() = default;
	AsyncResult(AsyncResult &&) = default;
	AsyncResult(SourceResultType t); // NOLINT
	explicit AsyncResult(vector<unique_ptr<AsyncTask>> &&task,
	                     TaskSchedulerType pool_type = TaskSchedulerType::REGULAR);
	AsyncResult &operator=(SourceResultType t);
	AsyncResult &operator=(AsyncResultType t);
	AsyncResult &operator=(AsyncResult &&) noexcept;
	//! Schedule held async_tasks into the Executor, eventually unblocking InterruptState
	//! needs to be called with non-empty async_tasks and from BLOCKED state, will empty the async_tasks and transform
	//! into INVALID
	void ScheduleTasks(InterruptState &interrupt_state, Executor &executor);
	//! Execute tasks synchronously at callsite, one after the other
	//! needs to be called with non-empty async_tasks and from BLOCKED state, will empty the async_tasks and transform
	//! into HAVE_MORE_OUTPUT
	void ExecuteTasksSynchronously();
	//! Execute tasks synchronously at callsite, side by side on the result's pool: the calling thread works on them
	//! too and returns when all of them are done. Same preconditions and result as the overload above.
	void ExecuteTasksSynchronously(ClientContext &context);

	static AsyncResultType GetAsyncResultType(SourceResultType s);
	//! Wraps tasks into a BLOCKED result, or HAVE_MORE_OUTPUT when there are none
	static AsyncResult FromTasks(vector<unique_ptr<AsyncTask>> &&tasks,
	                             TaskSchedulerType pool_type = TaskSchedulerType::REGULAR);
	//! FINISHED when the chunk is empty, HAVE_MORE_OUTPUT otherwise
	static AsyncResult FromChunk(const DataChunk &chunk);
	//! Maps a non-BLOCKED result to a SourceResultType, IMPLICIT resolves through the chunk size
	static SourceResultType GetSourceResultType(AsyncResultType type, idx_t chunk_size);

	//! Check whether there are tasks associated
	bool HasTasks() const;
	AsyncResultType GetResultType() const;
	//! Extract associated tasks, moving them away, will empty async_tasks and transform to INVALID
	vector<unique_ptr<AsyncTask>> &&ExtractAsyncTasks();

#ifdef DUCKDB_DEBUG_ASYNC_SINK_SOURCE
	//! Randomly generate a BLOCKED test result, returns false if no test tasks were generated
	static bool TryGenerateTestResult(AsyncResult &result);
#endif

	static AsyncResultsExecutionMode
	ConvertToAsyncResultExecutionMode(const PhysicalTableScanExecutionStrategy &execution_mode);

private:
	AsyncResultType result_type {AsyncResultType::INVALID};
	vector<unique_ptr<AsyncTask>> async_tasks {};
	//! The thread pool that the async_tasks are scheduled onto when BLOCKED
	TaskSchedulerType pool_type {TaskSchedulerType::REGULAR};
};
} // namespace duckdb
