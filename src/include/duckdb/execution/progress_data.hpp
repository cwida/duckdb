//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/progress_data.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/assert.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/constants.hpp"

namespace duckdb {

struct ProgressData {
	double done = 0.0;
	double total = 0.0;
	bool invalid = false;

public:
	double ProgressDone() const {
		// ProgressDone requires a valid state
		D_ASSERT(IsValid());

		return done / total;
	}

	void Add(const ProgressData &other) {
		// Add is unchecked, propagating invalid
		done += other.done;
		total += other.total;
		invalid = invalid || other.invalid;
	}
	void Normalize(const double target = 1.0) {
		// Normalize checks only `target`, propagating invalid
		D_ASSERT(target > 0.0);
		if (IsValid()) {
			if (total > 0.0) {
				done /= total;
			}
			total = 1.0;
			done *= target;
			total *= target;
		} else {
			SetInvalid();
		}
	}
	void SetInvalid() {
		invalid = true;
		done = 0.0;
		total = 1.0;
	}
	bool IsValid() const {
		return (!invalid) && (done >= 0.0) && (done <= total) && (total >= 0.0);
	}
};

//! Keeps an estimated progress from decreasing, for operators whose total is only known once they finish.
//! The highest fraction reported so far is kept as fixed-point, and is only touched when progress is requested.
struct MonotonicProgress {
public:
	//! Returns the given progress, raised to the highest fraction that was returned before
	ProgressData Update(const ProgressData &progress) const {
		if (!progress.IsValid() || progress.total <= 0) {
			return progress;
		}
		auto fraction = static_cast<idx_t>(progress.done / progress.total * static_cast<double>(PRECISION));
		fraction = fraction > PRECISION ? PRECISION : fraction;
		auto previous = max_fraction.load(std::memory_order_relaxed);
		while (fraction > previous &&
		       !max_fraction.compare_exchange_weak(previous, fraction, std::memory_order_relaxed)) {
		}
		auto result_fraction = fraction > previous ? fraction : previous;
		ProgressData result;
		result.done = static_cast<double>(result_fraction) / static_cast<double>(PRECISION) * progress.total;
		result.total = progress.total;
		return result;
	}
	void Reset() {
		max_fraction.store(0, std::memory_order_relaxed);
	}

private:
	static constexpr idx_t PRECISION = 1000000000;
	//! Updated when progress is requested, which only has const access to the operator state
	mutable atomic<idx_t> max_fraction {0};
};

//! Progress of a source that executes a known number of tasks - every task contributes UNITS_PER_TASK units once it
//! is finished, and threads report the units of the task they are working on as it advances
struct TaskProgress {
public:
	static constexpr idx_t UNITS_PER_TASK = 1 << 20;

	void AddUnits(idx_t count) {
		units.fetch_add(count, std::memory_order_relaxed);
	}
	ProgressData GetProgress(idx_t task_count) const {
		ProgressData result;
		result.total = static_cast<double>(task_count);
		result.done = static_cast<double>(units.load(std::memory_order_relaxed)) / static_cast<double>(UNITS_PER_TASK);
		if (result.done > result.total) {
			result.done = result.total;
		}
		return result;
	}
	void Reset() {
		units.store(0, std::memory_order_relaxed);
	}

private:
	atomic<idx_t> units {0};
};

//! Reports the progress of the task a thread is working on to a TaskProgress
struct TaskProgressTracker {
public:
	//! Starts reporting a new task
	void Start() {
		reported_units = 0;
	}
	//! Reports that the current task has done "done" out of "total" work
	void Update(TaskProgress &progress, idx_t done, idx_t total) {
		idx_t task_units = TaskProgress::UNITS_PER_TASK;
		if (total > 0 && done < total) {
			task_units = static_cast<idx_t>(static_cast<double>(TaskProgress::UNITS_PER_TASK) *
			                                static_cast<double>(done) / static_cast<double>(total));
		}
		if (task_units > reported_units) {
			progress.AddUnits(task_units - reported_units);
			reported_units = task_units;
		}
	}
	//! Reports that the current task is finished
	void Finish(TaskProgress &progress) {
		Update(progress, 1, 1);
	}

private:
	idx_t reported_units = 0;
};

} // namespace duckdb
