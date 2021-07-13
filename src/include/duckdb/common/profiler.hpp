//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/profiler.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/chrono.hpp"
#include "duckdb/common/helper.hpp"

namespace duckdb {

//! The profiler can be used to measure elapsed time
template <typename T>
class Profiler {
public:
	//! Starts the timer
	void Start() {
		finished = false;
		start = Tick();
	}
	//! Finishes timing
	void End() {
		end = Tick();
		finished = true;
	}

	//! Returns the elapsed time in seconds. If End() has been called, returns
	//! the total elapsed time. Otherwise returns how far along the timer is
	//! right now.
	double Elapsed() const {
		auto _end = finished ? end : Tick();
		return std::chrono::duration_cast<std::chrono::duration<double>>(_end - start).count();
	}

private:
	time_point<T> Tick() const {
		return T::now();
	}
	time_point<T> start;
	time_point<T> end;
	bool finished = false;
};

} // namespace duckdb
