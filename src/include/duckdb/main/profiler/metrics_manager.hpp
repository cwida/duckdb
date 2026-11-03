//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/profiler/metrics_manager.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/main/profiler/metric_info.hpp"

namespace duckdb {

class ClientContext;
class DatabaseInstance;

//! MetricsManager holds descriptors for every metric known to the system:
//! the statically compiled set (from src/common/metrics.json), optimizer
//! metrics generated at runtime from the OptimizerType enum, and any
//! additional metrics registered by extensions at runtime.
class MetricsManager {
public:
	MetricsManager() = default;

	DUCKDB_API static MetricsManager &Get(ClientContext &context);
	DUCKDB_API static MetricsManager &Get(DatabaseInstance &db);

	//! Returns the total number of known metrics (static + optimizer-generated + registered).
	DUCKDB_API idx_t GetMetricCount() const;

	//! Returns a snapshot of all known metrics.
	DUCKDB_API vector<MetricInfo> GetAllMetrics() const;

	//! Register an extension-defined metric. Silently ignores duplicates (same name) and the profiler's own metrics.
	DUCKDB_API void RegisterMetric(MetricInfo info);

	//! Whether an extension registered the metric `name`. The profiler's own metrics cannot be registered.
	DUCKDB_API bool IsRegisteredMetric(const string &name) const;

private:
	mutable mutex registered_metrics_lock;
	vector<MetricInfo> registered_metrics;
};

} // namespace duckdb
