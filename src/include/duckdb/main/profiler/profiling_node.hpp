//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/profiler/profiling_node.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/main/profiler/gathered_metrics.hpp"

namespace duckdb {

struct OperatorMetrics {
	explicit OperatorMetrics() {
		ResetMetrics();
	}

	string name;
	PhysicalOperatorType operator_type;

	double time;
	idx_t elements_returned;
	idx_t intermediate_size_bytes;
	idx_t system_peak_buffer_manager_memory;
	idx_t system_peak_temp_directory_size;
	idx_t rows_scanned;
	idx_t row_groups_scanned;
	idx_t total_row_groups_to_scan;
	idx_t bytes_scanned;
	//! Whether the scan reports bytes_scanned at all - otherwise the metric is left out rather than reported as 0
	bool bytes_scanned_reported;

	profiler_metrics_t GetMetrics(const GatheredMetrics &info) const;
	void ResetMetrics() {
		time = 0;
		elements_returned = 0;
		intermediate_size_bytes = 0;
		system_peak_buffer_manager_memory = 0;
		system_peak_temp_directory_size = 0;
		rows_scanned = 0;
		row_groups_scanned = 0;
		total_row_groups_to_scan = 0;
		bytes_scanned = 0;
		bytes_scanned_reported = false;
		operator_type = PhysicalOperatorType::INVALID;
		extra_info.clear();
	}
	void AddExtraInfo(string key, string value) {
		extra_info.insert(make_pair(std::move(key), std::move(value)));
	}
	void SetExtraInfo(InsertionOrderPreservingMap<string> info) {
		extra_info = std::move(info);
	}
	const InsertionOrderPreservingMap<string> &GetExtraInfo() const {
		return extra_info;
	}
	void GatherMetrics(ClientContext &context, double elapsed_time, optional_ptr<DataChunk> chunk);
	void Merge(const OperatorMetrics &other);
	void Accumulate(const OperatorMetrics &other);

private:
	InsertionOrderPreservingMap<string> extra_info;
	void MergeInternal(const OperatorMetrics &other);
};

//! The OperatorProfiler measures timings of individual operators
//! This class exists once for all operators and collects `OperatorInfo` for each operator
class OperatorProfiler {
	friend class QueryProfiler;

public:
	DUCKDB_API explicit OperatorProfiler(ClientContext &context);
	~OperatorProfiler() {
	}

public:
	DUCKDB_API void StartOperator(optional_ptr<const PhysicalOperator> phys_op);
	DUCKDB_API void EndOperator(optional_ptr<DataChunk> chunk);
	DUCKDB_API void FinishSource(GlobalSourceState &gstate, LocalSourceState &lstate);
	DUCKDB_API void FinishSource(const PhysicalOperator &phys_op, GlobalSourceState &gstate, LocalSourceState &lstate);

	//! Adds the timings in the OperatorProfiler (tree) to the QueryProfiler (tree).
	DUCKDB_API void Flush(const PhysicalOperator &phys_op);
	DUCKDB_API OperatorMetrics &GetOperatorMetrics(const PhysicalOperator &phys_op);
	DUCKDB_API bool OperatorMetricsIsInitialized(const PhysicalOperator &phys_op);

public:
	ClientContext &context;

private:
	//! Whether or not the profiler is enabled
	bool enabled;
	//! The timer used to time the execution time of the individual Physical Operators
	Profiler op;
	//! The stack of Physical Operators that are currently active
	optional_ptr<const PhysicalOperator> active_operator;
	//! A mapping of physical operators to profiled operator information.
	reference_map_t<const PhysicalOperator, OperatorMetrics> operator_metrics;
};

//! Recursive tree mirroring the operator tree.
class ProfilingNode {
public:
	explicit ProfilingNode() {
	}
	virtual ~ProfilingNode() {};

private:
	OperatorMetrics operator_info;

public:
	idx_t depth = 0;
	vector<unique_ptr<ProfilingNode>> children;

public:
	idx_t GetChildCount() {
		return children.size();
	}
	OperatorMetrics &GetOperatorMetrics() {
		return operator_info;
	}
	const OperatorMetrics &GetOperatorMetrics() const {
		return operator_info;
	}
	optional_ptr<ProfilingNode> GetChild(idx_t idx) {
		return children[idx].get();
	}
	optional_ptr<ProfilingNode> AddChild(unique_ptr<ProfilingNode> child) {
		children.push_back(std::move(child));
		return children.back().get();
	}
};

} // namespace duckdb
