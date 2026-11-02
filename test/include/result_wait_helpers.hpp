//===----------------------------------------------------------------------===//
//                         DuckDB
//
// result_wait_helpers.hpp
//
// Bounded waiting for the result tests: a query result test that hangs must fail its own test
// instead of hanging the suite.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"
#include "duckdb/main/client_context.hpp"

#include <atomic>
#include <chrono>
#include <thread>

namespace duckdb {

//! Bounds a polling loop. Check Passed() on every iteration
struct Deadline {
	std::chrono::steady_clock::time_point expiry = std::chrono::steady_clock::now() + std::chrono::seconds(60);

	bool Passed() const {
		return std::chrono::steady_clock::now() >= expiry;
	}
};

//! Waits until the connection's query has no execution left. False when the deadline passed first
inline bool WaitForExecution(Connection &con) {
	Deadline deadline;
	while (!con.context->ExecutionIsFinished()) {
		if (deadline.Passed()) {
			return false;
		}
		std::this_thread::sleep_for(std::chrono::microseconds(100));
	}
	return true;
}

//! Interrupts the connection when the guarded scope outlives the deadline, so a hung blocking call
//! ends with an error instead of hanging the suite
class DrainWatchdog {
public:
	explicit DrainWatchdog(Connection &con)
	    : watcher([this, &con]() {
		      Deadline deadline;
		      while (!done.load() && !deadline.Passed()) {
			      std::this_thread::sleep_for(std::chrono::milliseconds(100));
		      }
		      if (!done.load()) {
			      con.Interrupt();
		      }
	      }) {
	}
	~DrainWatchdog() {
		done = true;
		watcher.join();
	}
	DrainWatchdog(const DrainWatchdog &) = delete;
	DrainWatchdog &operator=(const DrainWatchdog &) = delete;

private:
	std::atomic<bool> done {false};
	std::thread watcher;
};

} // namespace duckdb
