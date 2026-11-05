//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/profiler/gathered_metrics.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/enums/output_type.hpp"
#include "duckdb/common/progress_bar/progress_bar.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/common/constants.hpp"
#include "duckdb/common/enums/metric_type.hpp"
#include "duckdb/common/set.hpp"
#include "duckdb/main/profiler/metrics.hpp"

namespace duckdb_yyjson {
struct yyjson_mut_doc;
struct yyjson_mut_val;
} // namespace duckdb_yyjson

namespace duckdb {

struct QueryProfileResult;
enum class ProfilingParameterNames : uint8_t { FORMAT, COVERAGE, SAVE_LOCATION, MODE, METRICS };

struct MetricTiming {
	idx_t duration = 0;
	timestamp_t start_time = timestamp_t::epoch();
	timestamp_t end_time = timestamp_t::epoch();
	bool has_bounds = false;

	void Add(idx_t addition) {
		duration += addition;
	}

	void Add(idx_t addition, timestamp_t start, timestamp_t end) {
		Add(addition);
		UpdateBounds(start, end);
	}

	void Merge(const MetricTiming &other) {
		Add(other.duration);
		if (other.has_bounds) {
			UpdateBounds(other.start_time, other.end_time);
		}
	}

private:
	void UpdateBounds(timestamp_t start, timestamp_t end) {
		if (!has_bounds) {
			start_time = start;
			end_time = end;
			has_bounds = true;
			return;
		}
		start_time = MinValue(start_time, start);
		end_time = MaxValue(end_time, end);
	}
};

class GatheredMetrics {
public:
	GatheredMetrics() = default;
	explicit GatheredMetrics(const vector<string> &tracked_metrics);
	GatheredMetrics(GatheredMetrics &) = default;
	GatheredMetrics &operator=(GatheredMetrics const &) = default;

public:
	void ResetMetrics();
	//! Returns true if this metric is enabled (and should therefore be collected and output).
	bool MetricIsTracked(const string &key) const;
	void SetMetric(const string &key, Value new_value);
	void SetMetric(const string &key, idx_t value);
	void SetMetric(const string &key, double value);
	void SetMetric(const string &key, const string &value);

	template <class T>
	bool MetricIsTracked() const {
		return MetricIsTracked(T::Name);
	}

	bool AnyOperatorMetricTracked() const {
		string operator_prefix = "operator.";
		if (MetricIsTracked(operator_prefix)) {
			return true;
		}
		for (const auto &key : tracked_exact) {
			if (key.size() > operator_prefix.size() && key.compare(0, operator_prefix.size(), operator_prefix) == 0) {
				return true;
			}
		}
		return false;
	}

	template <class T>
	void SetMetric(const typename T::METRIC_TYPE &value) {
		SetMetric(T::Name, value);
	}

	const profiler_metrics_t &GetMetrics() const {
		return metrics;
	}

public:
	void WriteMetricsToLog(ClientContext &context, const unordered_map<string, MetricTiming> &timings) const;
	//! Copy all enabled metrics into a QueryProfileResult node using lowercase string keys
	void MetricsToProfileResult(QueryProfileResult &result) const;

private:
	void InitTrackedMetrics(const vector<string> &patterns);

private:
	//! When tracked_metrics contains "*", all metrics are enabled and pattern matching is skipped.
	bool track_all = false;
	//! Exact metric names from tracked_metrics globs with no wildcard characters.
	unordered_set<string> tracked_exact;
	//! Prefix strings from tracked_metrics patterns of the form "prefix*" (stored without the trailing '*').
	set<string> tracked_prefixes;
	//! Arbitrary glob patterns from tracked_metrics that don't fit the prefix category.
	vector<string> tracked_globs;
	//! Contains all enabled metrics.
	profiler_metrics_t metrics;
};

} // namespace duckdb
