//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/task_error_manager.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/error_data.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/vector.hpp"

#include <new>

namespace duckdb {

class TaskErrorManager {
public:
	TaskErrorManager() : has_error(false), first_error_lost(false) {
		// allocate the error before memory can run out
		OutOfMemoryError();
	}

	void PushError(ErrorData error) {
		lock_guard<mutex> elock(error_lock);
		this->exceptions.push_back(std::move(error));
		has_error = true;
	}

	//! Push the error of a task that create_error returns. An exception that leaves a worker thread ends the process,
	//! so when memory runs out while creating or recording the error, OutOfMemoryError takes its place
	template <class CREATE_ERROR>
	void PushTaskError(CREATE_ERROR &&create_error) {
		try {
			PushError(create_error());
		} catch (const std::bad_alloc &) {
			PushOutOfMemoryError();
		}
	}

	ErrorData GetError() {
		lock_guard<mutex> elock(error_lock);
		if (first_error_lost) {
			return OutOfMemoryError();
		}
		D_ASSERT(!exceptions.empty());

		// FIXME: Should we try to get the biggest priority error?
		// In case the first exception is a StandardException but a regular Exception or a FatalException occurred
		// Maybe we should throw the more critical exception instead, as that changes behavior.
		auto &entry = exceptions[0];
		return entry;
	}

	bool HasError() {
		return has_error;
	}

	void ThrowException() {
		lock_guard<mutex> elock(error_lock);
		if (first_error_lost) {
			OutOfMemoryError().Throw();
		}
		D_ASSERT(!exceptions.empty());
		auto &entry = exceptions[0];
		entry.Throw();
	}

	void Reset() {
		lock_guard<mutex> elock(error_lock);
		exceptions.clear();
		first_error_lost = false;
		has_error = false;
	}

private:
	//! Allocated once, when the first error manager is created
	static const ErrorData &OutOfMemoryError() {
		static const ErrorData error(ExceptionType::OUT_OF_MEMORY,
		                             "Failed to allocate memory to report the error of a task");
		return error;
	}

	void PushOutOfMemoryError() {
		lock_guard<mutex> elock(error_lock);
		if (exceptions.empty()) {
			first_error_lost = true;
		}
		has_error = true;
	}

private:
	mutex error_lock;
	//! Exceptions that occurred during the execution of the current query
	vector<ErrorData> exceptions;
	//! Lock-free error flag
	atomic<bool> has_error;
	//! Whether the first error could not be recorded, so that OutOfMemoryError is reported in its place
	bool first_error_lost;
};
} // namespace duckdb
