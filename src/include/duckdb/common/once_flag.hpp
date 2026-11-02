//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/once_flag.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/atomic.hpp"
#include "duckdb/common/mutex.hpp"

#include <utility>

namespace duckdb {

//! One-time initialization flag that stays usable when the callable throws.
//! std::call_once is built on pthread_once with libstdc++, which never resets the flag after an exception on
//! MinGW and under ThreadSanitizer, so every other caller then waits forever.
class OnceFlag {
public:
	//! Runs func unless an earlier call completed; a throwing func leaves the flag unset so the next caller retries
	template <class FUNC>
	void Call(FUNC &&func) {
		if (done.load(std::memory_order_acquire)) {
			return;
		}
		lock_guard<mutex> guard(lock);
		if (done.load(std::memory_order_relaxed)) {
			return;
		}
		func();
		done.store(true, std::memory_order_release);
	}

private:
	atomic<bool> done {false};
	mutex lock;
};

//! Drop-in replacement for std::call_once
template <class FUNC>
void CallOnce(OnceFlag &flag, FUNC &&func) {
	flag.Call(std::forward<FUNC>(func));
}

} // namespace duckdb
