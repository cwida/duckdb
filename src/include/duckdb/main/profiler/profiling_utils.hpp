//===----------------------------------------------------------------------===//
//
//                         DuckDB
//
// duckdb/main/profiler/profiling_utils.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enums/metric_type.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/main/profiler/gathered_metrics.hpp"
#include "duckdb/main/profiler/profiling_node.hpp"
#include "duckdb/common/profiler.hpp"
#include "duckdb/common/types/timestamp.hpp"

namespace duckdb_yyjson {
struct yyjson_mut_doc;
struct yyjson_mut_val;
} // namespace duckdb_yyjson

namespace duckdb {

struct MetricsTimer;

// Top level query metrics
struct QueryMetrics {
public:
	QueryMetrics();
	~QueryMetrics();

	idx_t system_peak_buffer_memory;
	idx_t system_peak_streaming_buffer_size;
	idx_t system_peak_temp_dir_size;
	double blocked_thread_time;

	std::string query_sql;

	// Always-tracked byte counters (used by progress bar even when profiling is disabled)
	atomic<idx_t> bytes_read;
	atomic<idx_t> read_operations;
	atomic<idx_t> read_time_us;
	atomic<idx_t> bytes_written;
	atomic<idx_t> write_operations;
	atomic<idx_t> write_time_us;
	// Cumulative bytes written to the temporary (spill) directory
	atomic<idx_t> bytes_spilled;
	// Always tracked as well, so it can be read while the query runs: the compressed size of the column data scanned.
	// Not reported - query.total_bytes_scanned is summed over the operator tree, so that secure views hide theirs
	atomic<idx_t> bytes_scanned;
	// Thread-safe memory allocation counter (updated from allocator callbacks on any thread)
	atomic<idx_t> total_memory_allocated;

public:
	void UpdateMetric(const string &key, idx_t addition) {
		annotated_lock_guard<annotated_mutex> guard(string_metrics_lock);
		string_timings[key].Add(addition);
	}

	void UpdateMetric(const string &key, idx_t addition, timestamp_t start_time, timestamp_t end_time) {
		annotated_lock_guard<annotated_mutex> guard(string_metrics_lock);
		string_timings[key].Add(addition, start_time, end_time);
	}

	void UpdateMetricCounter(const string &key, idx_t addition) {
		annotated_lock_guard<annotated_mutex> guard(string_metrics_lock);
		string_counters[key] += addition;
	}

	void SetMetricValue(const string &key, Value value) {
		annotated_lock_guard<annotated_mutex> guard(string_metrics_lock);
		string_values[key] = std::move(value);
	}

	void UpdateBytesRead(idx_t n, idx_t elapsed_us) {
		bytes_read += n;
		read_operations++;
		read_time_us += elapsed_us;
	}

	void UpdateBytesWritten(idx_t n, idx_t elapsed_us) {
		bytes_written += n;
		write_operations++;
		write_time_us += elapsed_us;
	}

	void UpdateBytesSpilled(idx_t n) {
		bytes_spilled += n;
	}
	void UpdateBytesScanned(idx_t n) {
		bytes_scanned += n;
	}

	void UpdateTotalMemoryAllocated(idx_t n) {
		total_memory_allocated += n;
	}

	double GetStringMetricInSeconds(const string &key) const {
		annotated_lock_guard<annotated_mutex> guard(string_metrics_lock);
		auto it = string_timings.find(key);
		if (it == string_timings.end()) {
			return 0.0;
		}
		return static_cast<double>(it->second.duration) / 1e9;
	}

	idx_t GetStringCounter(const string &key) const {
		annotated_lock_guard<annotated_mutex> guard(string_metrics_lock);
		auto it = string_counters.find(key);
		if (it == string_counters.end()) {
			return 0;
		}
		return it->second;
	}

	idx_t GetBytesRead() const {
		return bytes_read.load();
	}

	idx_t GetReadOperations() const {
		return read_operations.load();
	}

	double GetReadTime() const {
		return static_cast<double>(read_time_us.load()) / 1e6;
	}

	idx_t GetBytesWritten() const {
		return bytes_written.load();
	}
	idx_t GetBytesScanned() const {
		return bytes_scanned.load();
	}

	idx_t GetWriteOperations() const {
		return write_operations.load();
	}

	double GetWriteTime() const {
		return static_cast<double>(write_time_us.load()) / 1e6;
	}

	idx_t GetBytesSpilled() const {
		return bytes_spilled.load();
	}

	idx_t GetTotalMemoryAllocated() const {
		return total_memory_allocated.load();
	}

	//! A copy, as timers and counters may still be updated from other threads
	unordered_map<string, MetricTiming> GetMetricTimings() const {
		annotated_lock_guard<annotated_mutex> guard(string_metrics_lock);
		return string_timings;
	}

	unordered_map<string, idx_t> GetMetricCounters() const {
		annotated_lock_guard<annotated_mutex> guard(string_metrics_lock);
		return string_counters;
	}

	//! Reset the counters that are tracked without profiling; Reset keeps them, so they outlive their query.
	void ResetLiveMetrics() {
		bytes_read = 0;
		read_operations = 0;
		read_time_us = 0;
		bytes_written = 0;
		write_operations = 0;
		write_time_us = 0;
		bytes_spilled = 0;
		// bytes_scanned is not reset here, so it can be read after a failed query; StartQuery resets it
		total_memory_allocated = 0;
	}

	void Reset() {
		// ended before taking the lock: the timer adds its time to string_timings
		latency_timer.reset();
		{
			annotated_lock_guard<annotated_mutex> guard(string_metrics_lock);
			string_timings.clear();
			string_counters.clear();
			string_values.clear();
		}

		query_sql = "";
		system_peak_buffer_memory = 0;
		system_peak_streaming_buffer_size = 0;
		system_peak_temp_dir_size = 0;
		blocked_thread_time = 0;
	}

	//! Write the values set with QueryProfiler::SetMetric into the given GatheredMetrics, before the metrics the
	//! profiler computes itself, which take precedence.
	void FinalizeMetricValues(GatheredMetrics &info);
	//! Write all query-level metrics into the given GatheredMetrics.
	void FinalizeMetrics(GatheredMetrics &info);
	//! The metrics backed by atomic counters, which can be read from any thread while the query runs
	profiler_metrics_t GetLiveMetrics() const;

	void Merge(const QueryMetrics &other) {
		auto other_timings = other.GetMetricTimings();
		auto other_counters = other.GetMetricCounters();
		{
			annotated_lock_guard<annotated_mutex> guard(string_metrics_lock);
			for (const auto &entry : other_timings) {
				string_timings[entry.first].Merge(entry.second);
			}
			for (const auto &entry : other_counters) {
				string_counters[entry.first] += entry.second;
			}
		}
		bytes_read += other.bytes_read.load();
		read_operations += other.read_operations.load();
		read_time_us += other.read_time_us.load();
		bytes_written += other.bytes_written.load();
		write_operations += other.write_operations.load();
		write_time_us += other.write_time_us.load();
		bytes_spilled += other.bytes_spilled.load();
		bytes_scanned += other.bytes_scanned.load();
		total_memory_allocated += other.total_memory_allocated.load();
	}

private:
	// Guards the string-keyed metrics below, which operators, timers and extensions update from any thread
	mutable annotated_mutex string_metrics_lock;
	// String-keyed timings for optimizer, storage and phase timing metrics
	unordered_map<string, MetricTiming> string_timings DUCKDB_GUARDED_BY(string_metrics_lock);
	// String-keyed counters for storage counter metrics (e.g. "storage.wal_replay_entry_count")
	unordered_map<string, idx_t> string_counters DUCKDB_GUARDED_BY(string_metrics_lock);
	// Values set by QueryProfiler::SetMetric, kept until the query's metrics are gathered
	profiler_metrics_t string_values DUCKDB_GUARDED_BY(string_metrics_lock);

public:
	// Declared after string_timings and string_metrics_lock so it is destroyed first; its destructor writes to
	// string_timings.
	unique_ptr<MetricsTimer> latency_timer;
};

struct MetricsTimer {
public:
	MetricsTimer() : metric_name(""), is_active(false) {
	}
	MetricsTimer(QueryMetrics &query_metrics, string key, const bool is_active = true)
	    : query_metrics(query_metrics), metric_name(std::move(key)), is_active(is_active) {
		if (!is_active) {
			return;
		}
		start_time = Timestamp::GetCurrentTimestamp();
		profiler.Start();
	}
	~MetricsTimer() {
		if (is_active && !Exception::UncaughtException()) {
			EndTimer();
		}
	}
	// disable copy constructors
	MetricsTimer(const MetricsTimer &other) = delete;
	MetricsTimer &operator=(const MetricsTimer &) = delete;
	//! enable move constructors
	MetricsTimer(MetricsTimer &&other) noexcept : is_active(false) {
		std::swap(query_metrics, other.query_metrics);
		std::swap(metric_name, other.metric_name);
		std::swap(profiler, other.profiler);
		std::swap(start_time, other.start_time);
		std::swap(is_active, other.is_active);
	}
	MetricsTimer &operator=(MetricsTimer &&other) noexcept {
		std::swap(query_metrics, other.query_metrics);
		std::swap(metric_name, other.metric_name);
		std::swap(profiler, other.profiler);
		std::swap(start_time, other.start_time);
		std::swap(is_active, other.is_active);
		return *this;
	}

	// Automatically called in the destructor.
	void EndTimer() {
		if (!is_active) {
			return;
		}
		is_active = false;
		profiler.End();
		query_metrics->UpdateMetric(metric_name, profiler.ElapsedNanos(), start_time, Timestamp::GetCurrentTimestamp());
	}

	void Reset() {
		if (!is_active) {
			return;
		}
		profiler.Reset();
		is_active = false;
	}

private:
	optional_ptr<QueryMetrics> query_metrics;
	string metric_name;
	Profiler profiler;
	timestamp_t start_time = timestamp_t::epoch();
	bool is_active;
};

} // namespace duckdb
