//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/query_profiler.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/atomic.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/enums/profiling_coverage.hpp"
#include "duckdb/common/deque.hpp"
#include "duckdb/common/enums/metric_type.hpp"
#include "duckdb/main/profiler/profiler_print_format.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/pair.hpp"
#include "duckdb/common/profiler.hpp"
#include "duckdb/common/reference_map.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/winapi.hpp"
#include "duckdb/execution/expression_executor_state.hpp"
#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/main/profiler/profiling_node.hpp"
#include "duckdb/main/profiler/profiling_utils.hpp"

namespace duckdb {

class BaseTreeRenderer;
class ClientContext;
class ExpressionExecutor;
class ProfilingNode;
class PhysicalOperator;
class TreeRenderer;
class SQLStatement;
struct MetricsTimer;
class OperatorProfiler;

//! A JSON-like recursive profiling value.
//! FIXME: this should at some point be replaced by a "Value" - but that's not easily possible until our VARIANT Value
//! is extended to be able to easily hold arbitrary values
enum class QueryProfileResultKind {
	VALUE,
	LIST,
	OBJECT,
};

struct QueryProfileResult {
	QueryProfileResultKind kind = QueryProfileResultKind::OBJECT;
	//! Non-empty for children of an OBJECT node; empty for LIST items and the root
	string key;
	//! Valid when kind == VALUE
	Value value;
	//! Ordered children; valid when kind == LIST or OBJECT
	vector<unique_ptr<QueryProfileResult>> children;

	//! Add a named VALUE leaf to this OBJECT node
	void AddValue(const string &k, Value val);
	//! Add a named OBJECT child to this OBJECT node; returns the new child
	QueryProfileResult &AddObject(const string &k);
	//! Add a named LIST child to this OBJECT node; returns the new child
	QueryProfileResult &AddList(const string &k);
	//! Append an anonymous OBJECT item to this LIST node; returns the new item
	QueryProfileResult &AppendObject();
	//! Append an anonymous LIST item to this node; returns the new item
	QueryProfileResult &AppendList();
	//! Returns true if this node is a nested type (OBJECT or LIST)
	bool IsNested() const {
		return kind == QueryProfileResultKind::OBJECT || kind == QueryProfileResultKind::LIST;
	}
};

//! QueryProfiler collects the profiling metrics of a query.
class QueryProfiler {
public:
	using TreeMap = reference_map_t<const PhysicalOperator, reference<ProfilingNode>>;

public:
	DUCKDB_API explicit QueryProfiler(ClientContext &context);

public:
	DUCKDB_API bool IsEnabled() const;
	//! Create the TreeRenderer for the given profiler format name (e.g. "json", "query_tree"). Returns nullptr for the
	//! "no_output" format, and throws if the format name is not recognized.
	DUCKDB_API unique_ptr<TreeRenderer> CreateProfiler(const string &name) const;
	//! Returns the renderer to use for the given ProfilerPrintFormat, taking the configured default format into
	//! account.
	DUCKDB_API unique_ptr<TreeRenderer>
	GetRenderer(const ProfilerPrintFormat &format = ProfilerPrintFormat::Default()) const;
	DUCKDB_API bool PrintOptimizerOutput() const;
	DUCKDB_API string GetSaveLocation() const;

	DUCKDB_API static QueryProfiler &Get(ClientContext &context);

	DUCKDB_API void Start(const string &query);
	DUCKDB_API void Reset();
	DUCKDB_API void StartQuery(const string &query, bool is_explain_analyze = false, bool start_at_optimizer = false);
	DUCKDB_API void EndQuery();
	//! Let the next StartQuery reset the live counters, also when the previous call never ended its query.
	DUCKDB_API void ResetQueryStart();
	//! Finalize query metrics for output; safe to call multiple times.
	DUCKDB_API void FinalizeMetrics();
	//! Snapshot the query-level metrics that are kept up to date while the query runs, keyed by metric name.
	DUCKDB_API profiler_metrics_t GetLiveMetrics() const;

	//! Track bytes read and the time spent reading.
	DUCKDB_API void TrackBytesRead(idx_t amount, idx_t elapsed_us);
	//! Track bytes written and the time spent writing.
	DUCKDB_API void TrackBytesWritten(idx_t amount, idx_t elapsed_us);
	//! Track bytes spilled to the temporary directory (always tracked, even when profiling disabled).
	DUCKDB_API void TrackBytesSpilled(idx_t amount);
	//! Track the compressed size of column data scanned (always tracked, even when profiling disabled). Unlike
	//! query.total_bytes_scanned, the running total includes scans inside secure views
	DUCKDB_API void TrackBytesScanned(idx_t amount);
	//! Zero the running total of bytes scanned, at the start of every statement
	void ResetBytesScanned();
	//! Track memory allocated (thread-safe; always tracked).
	DUCKDB_API void TrackTotalMemoryAllocated(idx_t amount);
	// The metrics an extension reports for the running query, declared with ExtensionLoader::RegisterMetric. These
	// calls are thread-safe and take a lock while profiling is enabled, so add up per-row figures locally first. Make
	// them on threads the query waits for: an update that arrives after the query ends may be lost, or counted for the
	// next query.

	//! Add to a metric counter (profiling-only, thread-safe).
	DUCKDB_API void AddToMetricCounter(const string &key, idx_t amount);
	//! Add time measured elsewhere, in nanoseconds, to a timing metric, which is reported in seconds (profiling-only,
	//! thread-safe). Unlike a timer it records no start and end time. For time a timer cannot measure, e.g. requests
	//! that were made on other threads.
	DUCKDB_API void AddToMetricTiming(const string &key, idx_t elapsed_ns);
	//! Add parser time measured before the per-statement profiler was started.
	void AddParserTime(const Profiler &parser_timer, timestamp_t start_time, timestamp_t end_time);

	//! Set the value of a metric an extension registered with ExtensionLoader::RegisterMetric, for the running query
	//! (profiling-only). Any other name, including the profiler's own metrics, is ignored. Thread-safe, and may be
	//! called while the statement binds or executes; a value set while a prepared statement binds is not kept for its
	//! executions.
	DUCKDB_API void SetMetric(const string &key, Value new_value);
	//! Returns true if the given metric is being tracked for the running query, also while the statement binds.
	//! Always returns false when profiling is disabled or no query is being profiled.
	DUCKDB_API bool MetricIsTracked(const string &key) const;

	//! Start a timer for a metric identified by its struct type.
	template <class METRIC>
	MetricsTimer StartTimer() {
		return StartTimerInternal(METRIC::Name);
	}
	//! Start a timer for a string-keyed metric (use the template overload when possible).
	DUCKDB_API MetricsTimer StartTimerInternal(const string &key);

	DUCKDB_API void StartExplainAnalyze();

	//! Adds the timings gathered by an OperatorProfiler to this query profiler
	DUCKDB_API void Flush(OperatorProfiler &profiler);
	//! Adds the top level query information to the global profiler.
	DUCKDB_API void SetBlockedTime(const double &blocked_thread_time);
	//! Record the peak bytes a streaming result buffered. Called just before the query ends
	DUCKDB_API void SetStreamingPeakBufferSize(idx_t peak_bytes);

	DUCKDB_API void Initialize(const PhysicalOperator &root);

	DUCKDB_API string QueryTreeToString() const;
	DUCKDB_API void QueryTreeToStream(std::ostream &str) const;
	//! Render the framed query tree (total time + operator tree) into the given sink. Folding/expansion of the tree is
	//! controlled by the renderer (configured via the profiling renderer settings), not here.
	DUCKDB_API void RenderQueryTree(BaseTreeRenderer &ss) const;
	DUCKDB_API void Print();

	//! Render the profiler output as a string, formatted based on the given ProfilerPrintFormat (or the configured
	//! default profiler format when ProfilerPrintFormat::Default is passed).
	DUCKDB_API string ToString(const ProfilerPrintFormat &format = ProfilerPrintFormat::Default()) const;
	//! Render the profiler output for the given profiler format name (e.g. "json", "query_tree"), handling the
	//! profiling-disabled and no-output cases.
	DUCKDB_API string ToString(const string &profiler_format_name) const;
	//! Render the profiling node tree using the given renderer into the sink (renders nothing when there is no tree).
	//! Called by TreeRenderer::RenderProfiler for the formats that render the node tree directly.
	DUCKDB_API void RenderProfilingNodeTree(TreeRenderer &renderer, BaseTreeRenderer &ss) const;

	// Sanitize a Value::MAP
	static Value JSONSanitize(const Value &input);
	static string JSONSanitize(const string &text);
	static string DrawPadded(const string &str, idx_t width);
	DUCKDB_API void ToLog() const;
	DUCKDB_API string ToJSON() const;
	DUCKDB_API void WriteToFile(const char *path, string &info) const;
	DUCKDB_API idx_t GetBytesRead() const;
	DUCKDB_API idx_t GetBytesWritten() const;
	DUCKDB_API idx_t GetBytesScanned() const;

	//! Return the result tree (generating it if it does not yet exist)
	QueryProfileResult &GetResult();
	//! Returns true if the last query produced a profiling tree (i.e. profiling was enabled and the query succeeded)
	bool HasRoot() const;

private:
	unique_ptr<ProfilingNode> CreateTree(const PhysicalOperator &root, const idx_t depth = 0);
	void Render(const ProfilingNode &node, BaseTreeRenderer &str) const;
	//! Render the profiler output to a string via the given renderer (nullptr renders nothing), handling the disabled
	//! case. Used for the programmatic / string paths.
	string RenderProfilerOutput(optional_ptr<TreeRenderer> renderer) const;
	//! Print the profiler output directly via the renderer's print sink (nullptr prints nothing), handling the
	//! disabled case, followed by `trailer`. Only used on the terminal-print paths; everything they print goes
	//! through the sink, so a renderer whose sink keeps or discards the output decides what is printed.
	void PrintProfilerOutput(optional_ptr<TreeRenderer> renderer, const string &trailer = "") const;

public:
	//! Provides access to the root of the query tree, but ensures there are no concurrent modifications
	//! This can be useful when implementing continuous profiling or making customizations
	DUCKDB_API void GetRootUnderLock(const std::function<void(optional_ptr<ProfilingNode>)> &callback) {
		lock_guard<std::mutex> guard(lock);
		callback(root.get());
	}
	//! Provides access to the finalized metrics and query tree of the last profiled query, both null while a query is
	//! running or when there are none, and ensures there are no concurrent modifications
	DUCKDB_API void GetFinalizedProfileUnderLock(
	    const std::function<void(optional_ptr<const GatheredMetrics>, optional_ptr<const ProfilingNode>)> &callback)
	    const;

private:
	ClientContext &context;

	//! Atomic so inactive flushes can skip acquiring the profiler lock.
	atomic<bool> running;
	//! Whether StartQuery was called for the current query; only the first call resets the live counters
	bool query_started;
	//! The lock used for accessing the global query profiler or flushing information to it from a thread
	mutable std::mutex lock;

	//! Whether or not the query requires profiling
	bool query_requires_profiling;

	//! The root of the query tree
	unique_ptr<ProfilingNode> root;

	unique_ptr<GatheredMetrics> metrics;
	//! The tracked_metrics patterns of the running query, for MetricIsTracked before metrics exists
	unique_ptr<GatheredMetrics> tracked_metric_patterns;

	//! Top level query information.
	QueryMetrics query_metrics;

	unique_ptr<QueryProfileResult> result_tree;

	//! A map of a Physical Operator pointer to a tree node
	TreeMap tree_map;
	//! Whether or not we are running as part of a explain_analyze query
	bool is_explain_analyze;
	//! Whether root metrics have been finalized for output
	bool metrics_finalized;

public:
	const TreeMap &GetTreeMap() const {
		return tree_map;
	}
	//! Top-level query metrics (total/CPU time, bytes read/written, ...), for renderers that surface a query summary.
	const QueryMetrics &GetQueryMetrics() const {
		return query_metrics;
	}
	//! The query text with any EXPLAIN [ANALYZE] wrapper stripped. Used by renderers that show the query;
	//! pretty-printing and highlighting are left to the renderer (e.g. HTMLTreeRenderer) so this stays catalog-free.
	DUCKDB_API string GetQuerySQL() const;
	//! Render the current profiling tree in the given format (text/json/html/...) to a string. Unlike ToString this
	//! only requires a tree to exist (HasRoot()), not that profiling is currently enabled - so it can re-render the
	//! last profile after the query has finished (e.g. the shell's ".web" command).
	DUCKDB_API string RenderProfile(const string &format) const;

private:
	void FinalizeMetricsInternal();
	//! Write metrics to log without acquiring the lock (must be called with lock held).
	void ToLogInternal() const;

	unique_ptr<QueryProfileResult> ToResultTree() const;
	unique_ptr<QueryProfileResult> ToLegacyResultTree() const;

	//! Check whether or not an operator type requires query profiling. If none of the ops in a query require profiling
	//! no profiling information is output.
	bool OperatorRequiresProfiling(const PhysicalOperatorType op_type);
};

} // namespace duckdb
