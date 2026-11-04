#include "duckdb/main/capi_v2/capi_v2_internal.hpp"

#include "duckdb/common/enum_util.hpp"
#include "duckdb/main/profiler/gathered_metrics.hpp"
#include "duckdb/main/profiler/profiling_node.hpp"
#include "duckdb/main/query_profiler.hpp"

namespace duckdb::capiv2 {

//! One node of a snapshot: the query as a whole, or one of its operators
struct CV2QueryMetricsNode {
	string name;
	//! Sorted by name
	vector<pair<string, Value>> metrics;
	vector<unique_ptr<CV2QueryMetricsNode>> children;
};

//! An owned snapshot of the metrics of a query, independent of the profiler it was taken from
struct CV2QueryMetrics {
	CV2QueryMetricsNode root;
};

static auto Convert(duckdb_v2_query_metrics_handle metrics) -> CV2QueryMetrics * {
	return reinterpret_cast<CV2QueryMetrics *>(metrics);
}
static auto Convert(CV2QueryMetrics *metrics) -> duckdb_v2_query_metrics_handle {
	return reinterpret_cast<duckdb_v2_query_metrics_handle>(metrics);
}
static auto Convert(duckdb_v2_query_metrics_node_handle node) -> CV2QueryMetricsNode * {
	return reinterpret_cast<CV2QueryMetricsNode *>(node);
}
static auto Convert(CV2QueryMetricsNode *node) -> duckdb_v2_query_metrics_node_handle {
	return reinterpret_cast<duckdb_v2_query_metrics_node_handle>(node);
}

//! Adds the numeric metrics, sorted by name: the others, e.g. the SQL text, are not measurements
static void AddQueryMetrics(CV2QueryMetricsNode &node, const profiler_metrics_t &metrics, const string &prefix) {
	for (auto &entry : metrics) {
		if (!entry.second.IsNull() && entry.second.type().IsNumeric()) {
			node.metrics.emplace_back(prefix + entry.first, entry.second);
		}
	}
	std::sort(node.metrics.begin(), node.metrics.end(),
	          [](const pair<string, Value> &a, const pair<string, Value> &b) { return a.first < b.first; });
}

static unique_ptr<CV2QueryMetricsNode> SnapshotQueryMetricsOperator(const GatheredMetrics &gathered,
                                                                    const ProfilingNode &node) {
	auto result = make_uniq<CV2QueryMetricsNode>();
	auto &info = node.GetOperatorMetrics();
	result->name = EnumUtil::ToString(info.operator_type);
	// operator metrics are keyed without their group
	AddQueryMetrics(*result, info.GetMetrics(gathered), "operator.");
	for (auto &child : node.children) {
		result->children.push_back(SnapshotQueryMetricsOperator(gathered, *child));
	}
	return result;
}

static CV2QueryMetricsNode &GetCheckedQueryMetricsChild(CV2QueryMetricsNode &node, idx_t index, const char *func) {
	if (index >= node.children.size()) {
		throw Exception(ExceptionType::OUT_OF_RANGE,
		                StringUtil::Format("Child index %llu is out of range for a node with %llu children in %s.",
		                                   static_cast<uint64_t>(index), static_cast<uint64_t>(node.children.size()),
		                                   func));
	}
	return *node.children[index];
}

static pair<string, Value> &GetCheckedQueryMetric(CV2QueryMetricsNode &node, idx_t index, const char *func) {
	if (index >= node.metrics.size()) {
		throw Exception(ExceptionType::OUT_OF_RANGE,
		                StringUtil::Format("Metric index %llu is out of range for a node with %llu metrics in %s.",
		                                   static_cast<uint64_t>(index), static_cast<uint64_t>(node.metrics.size()),
		                                   func));
	}
	return node.metrics[index];
}

} // namespace duckdb::capiv2

//----------------------------------------------------------------------------------------------------------------------
// Public Functions
//----------------------------------------------------------------------------------------------------------------------

using namespace duckdb::capiv2;

DUCKDB_V2_ERROR duckdb_v2_connection_get_query_metrics(duckdb_v2_connection_handle conn,
                                                       duckdb_v2_query_metrics_handle *metrics,
                                                       duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(conn);
	DUCKDB_CHECK_ARG(metrics);
	*metrics = nullptr;
	return WithErrorHandler(err, [&]() {
		auto &context = *Convert(conn)->context;
		context.VerifyNoOpenResult();
		auto snapshot = duckdb::make_uniq<CV2QueryMetrics>();
		snapshot->root.name = "QUERY";
		duckdb::QueryProfiler::Get(context).GetFinalizedProfileUnderLock(
		    [&](duckdb::optional_ptr<const duckdb::GatheredMetrics> gathered,
		        duckdb::optional_ptr<const duckdb::ProfilingNode> root) {
			    if (!gathered) {
				    return;
			    }
			    AddQueryMetrics(snapshot->root, gathered->GetMetrics(), "");
			    if (root) {
				    snapshot->root.children.push_back(SnapshotQueryMetricsOperator(*gathered, *root));
			    }
		    });
		*metrics = Convert(snapshot.release());
	});
}

DUCKDB_V2_ERROR duckdb_v2_connection_enable_profiling(duckdb_v2_connection_handle conn, bool enabled,
                                                      duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(conn);
	return WithErrorHandler(err, [&]() {
		auto &connection = *Convert(conn);
		if (enabled) {
			connection.EnableProfiling();
		} else {
			connection.DisableProfiling();
		}
	});
}

DUCKDB_V2_ERROR duckdb_v2_query_metrics_get_root(duckdb_v2_query_metrics_handle metrics,
                                                 duckdb_v2_query_metrics_node_handle *node,
                                                 duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(metrics);
	DUCKDB_CHECK_ARG(node);
	*node = nullptr;
	return WithErrorHandler(err, [&]() { *node = Convert(&Convert(metrics)->root); });
}

DUCKDB_V2_ERROR duckdb_v2_query_metrics_node_get_name(duckdb_v2_query_metrics_node_handle node, duckdb_v2_str *name,
                                                      duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(node);
	DUCKDB_CHECK_ARG(name);
	*name = duckdb_v2_str {nullptr, 0};
	return WithErrorHandler(err, [&]() { *name = Convert(Convert(node)->name); });
}

DUCKDB_V2_ERROR duckdb_v2_query_metrics_node_get_child_count(duckdb_v2_query_metrics_node_handle node, idx_t *count,
                                                             duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(node);
	DUCKDB_CHECK_ARG(count);
	return WithErrorHandler(err, [&]() { *count = Convert(node)->children.size(); });
}

DUCKDB_V2_ERROR duckdb_v2_query_metrics_node_get_child(duckdb_v2_query_metrics_node_handle node, idx_t index,
                                                       duckdb_v2_query_metrics_node_handle *child,
                                                       duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(node);
	DUCKDB_CHECK_ARG(child);
	*child = nullptr;
	return WithErrorHandler(err,
	                        [&]() { *child = Convert(&GetCheckedQueryMetricsChild(*Convert(node), index, __func__)); });
}

DUCKDB_V2_ERROR duckdb_v2_query_metrics_node_get_metric_count(duckdb_v2_query_metrics_node_handle node, idx_t *count,
                                                              duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(node);
	DUCKDB_CHECK_ARG(count);
	return WithErrorHandler(err, [&]() { *count = Convert(node)->metrics.size(); });
}

DUCKDB_V2_ERROR duckdb_v2_query_metrics_node_get_metric_name(duckdb_v2_query_metrics_node_handle node, idx_t index,
                                                             duckdb_v2_str *name, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(node);
	DUCKDB_CHECK_ARG(name);
	*name = duckdb_v2_str {nullptr, 0};
	return WithErrorHandler(err,
	                        [&]() { *name = Convert(GetCheckedQueryMetric(*Convert(node), index, __func__).first); });
}

DUCKDB_V2_ERROR duckdb_v2_query_metrics_node_get_metric_value(duckdb_v2_query_metrics_node_handle node, idx_t index,
                                                              duckdb_v2_value_handle *value,
                                                              duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(node);
	DUCKDB_CHECK_ARG(value);
	*value = nullptr;
	return WithErrorHandler(err, [&]() {
		auto &metric = GetCheckedQueryMetric(*Convert(node), index, __func__);
		*value = Convert(new duckdb::Value(metric.second));
	});
}

DUCKDB_V2_ERROR duckdb_v2_query_metrics_node_get_metric(duckdb_v2_query_metrics_node_handle node,
                                                        const duckdb_v2_str *name, duckdb_v2_value_handle *value,
                                                        bool *present, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(node);
	DUCKDB_CHECK_ARG(name);
	DUCKDB_CHECK_ARG(value);
	DUCKDB_CHECK_ARG(present);
	*value = nullptr;
	*present = false;
	return WithErrorHandler(err, [&]() {
		std::string_view key(name->ptr, name->len);
		for (auto &metric : Convert(node)->metrics) {
			if (metric.first == key) {
				*value = Convert(new duckdb::Value(metric.second));
				*present = true;
				return;
			}
		}
	});
}

DUCKDB_V2_ERROR duckdb_v2_query_metrics_destroy(duckdb_v2_query_metrics_handle *metrics) {
	return WithErrorHandler(nullptr, [&]() {
		if (!metrics) {
			return;
		}
		if (*metrics) {
			delete Convert(*metrics);
			*metrics = nullptr;
		}
	});
}
