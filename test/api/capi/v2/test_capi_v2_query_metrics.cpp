#include "test_capi_v2.hpp"

#include <string>

// ---------------------------------------------------------------------------
// V2 query metrics tests: snapshot the metrics of the last query a
// connection finished, and walk the snapshot's tree of nodes.
// ---------------------------------------------------------------------------

namespace test_capi_v2 {

namespace {

constexpr const char *METRICS_PROFILED_QUERY = "SELECT sum(i) FROM range(1000) t(i)";

duckdb_v2_query_metrics_handle TakeMetricsSnapshot(duckdb_v2_connection_handle conn) {
	duckdb_v2_query_metrics_handle metrics = nullptr;
	REQUIRE(duckdb_v2_connection_get_query_metrics(conn, &metrics, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(metrics);
	return metrics;
}

duckdb_v2_query_metrics_node_handle GetMetricsRoot(duckdb_v2_query_metrics_handle metrics) {
	duckdb_v2_query_metrics_node_handle root = nullptr;
	REQUIRE(duckdb_v2_query_metrics_get_root(metrics, &root, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(root);
	return root;
}

std::string MetricsNodeName(duckdb_v2_query_metrics_node_handle node) {
	duckdb_v2_str name = {nullptr, 0};
	REQUIRE(duckdb_v2_query_metrics_node_get_name(node, &name, nullptr) == DUCKDB_V2_ERROR_NONE);
	return std::string(name.ptr, name.len);
}

idx_t MetricsNodeChildCount(duckdb_v2_query_metrics_node_handle node) {
	idx_t count = 0;
	REQUIRE(duckdb_v2_query_metrics_node_get_child_count(node, &count, nullptr) == DUCKDB_V2_ERROR_NONE);
	return count;
}

idx_t MetricsNodeMetricCount(duckdb_v2_query_metrics_node_handle node) {
	idx_t count = 0;
	REQUIRE(duckdb_v2_query_metrics_node_get_metric_count(node, &count, nullptr) == DUCKDB_V2_ERROR_NONE);
	return count;
}

// The first node of the given name, searching depth-first.
duckdb_v2_query_metrics_node_handle FindMetricsNode(duckdb_v2_query_metrics_node_handle node, const std::string &name) {
	if (MetricsNodeName(node) == name) {
		return node;
	}
	for (idx_t i = 0; i < MetricsNodeChildCount(node); i++) {
		duckdb_v2_query_metrics_node_handle child = nullptr;
		REQUIRE(duckdb_v2_query_metrics_node_get_child(node, i, &child, nullptr) == DUCKDB_V2_ERROR_NONE);
		auto found = FindMetricsNode(child, name);
		if (found) {
			return found;
		}
	}
	return nullptr;
}

// Looks a metric up by name, returning whether the node carries it and, if so, its value as a count.
bool GetMetricCount(duckdb_v2_query_metrics_node_handle node, const char *name, uint64_t &count) {
	auto name_str = Convert(name);
	duckdb_v2_value_handle value = nullptr;
	bool present = true;
	REQUIRE(duckdb_v2_query_metrics_node_get_metric(node, &name_str, &value, &present, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	if (!present) {
		REQUIRE(value == nullptr);
		return false;
	}
	REQUIRE(duckdb_v2_value_get_ubigint(value, &count, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_value_destroy(&value);
	return true;
}

bool NodeHasMetric(duckdb_v2_query_metrics_node_handle node, const char *name) {
	auto name_str = Convert(name);
	duckdb_v2_value_handle value = nullptr;
	bool present = false;
	REQUIRE(duckdb_v2_query_metrics_node_get_metric(node, &name_str, &value, &present, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	duckdb_v2_value_destroy(&value);
	return present;
}

} // namespace

TEST_CASE("V2 query metrics: without profiling the snapshot is empty", "[capi_v2][query_metrics]") {
	EnvFixture fx;
	// Before any query, and after one that was not profiled.
	for (int i = 0; i < 2; i++) {
		auto metrics = TakeMetricsSnapshot(fx.conn);
		auto root = GetMetricsRoot(metrics);
		REQUIRE(MetricsNodeName(root) == "QUERY");
		REQUIRE(MetricsNodeMetricCount(root) == 0);
		REQUIRE(MetricsNodeChildCount(root) == 0);
		REQUIRE(duckdb_v2_query_metrics_destroy(&metrics) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(metrics == nullptr);
		REQUIRE(RunSql(fx.conn, METRICS_PROFILED_QUERY) == DUCKDB_V2_ERROR_NONE);
	}
}

TEST_CASE("V2 query metrics: a profiled query", "[capi_v2][query_metrics]") {
	EnvFixture fx;
	REQUIRE(RunSql(fx.conn, "SET enable_profiling = 'no_output'") == DUCKDB_V2_ERROR_NONE);
	REQUIRE(RunSql(fx.conn, METRICS_PROFILED_QUERY) == DUCKDB_V2_ERROR_NONE);

	auto metrics = TakeMetricsSnapshot(fx.conn);
	auto root = GetMetricsRoot(metrics);
	REQUIRE(MetricsNodeName(root) == "QUERY");
	REQUIRE(NodeHasMetric(root, "query.total_time"));
	// Only numbers are metrics: the SQL text is not one.
	REQUIRE_FALSE(NodeHasMetric(root, "query.sql"));

	// Every metric reads by index, as a number, and its name looks it up again.
	auto count = MetricsNodeMetricCount(root);
	REQUIRE(count > 0);
	for (idx_t i = 0; i < count; i++) {
		duckdb_v2_str name = {nullptr, 0};
		REQUIRE(duckdb_v2_query_metrics_node_get_metric_name(root, i, &name, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(name.len > 0);
		REQUIRE(NodeHasMetric(root, std::string(name.ptr, name.len).c_str()));
		duckdb_v2_value_handle value = nullptr;
		REQUIRE(duckdb_v2_query_metrics_node_get_metric_value(root, i, &value, nullptr) == DUCKDB_V2_ERROR_NONE);
		double number = -1;
		REQUIRE(duckdb_v2_value_get_double(value, &number, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(number >= 0);
		duckdb_v2_value_destroy(&value);
	}

	// The operators that executed the query hang below the root.
	REQUIRE(MetricsNodeChildCount(root) == 1);
	auto aggregate = FindMetricsNode(root, "UNGROUPED_AGGREGATE");
	REQUIRE(aggregate);
	uint64_t rows = 0;
	REQUIRE(GetMetricCount(aggregate, "operator.intermediate_rows", rows));
	REQUIRE(rows == 1);
	auto scan = FindMetricsNode(root, "TABLE_SCAN");
	REQUIRE(scan);
	REQUIRE(GetMetricCount(scan, "operator.intermediate_rows", rows));
	REQUIRE(rows == 1000);
	REQUIRE(NodeHasMetric(scan, "operator.timing"));
	REQUIRE_FALSE(NodeHasMetric(scan, "operator.type"));
	REQUIRE_FALSE(NodeHasMetric(scan, "no.such_metric"));

	// The snapshot does not change with later queries, also when they are not profiled.
	REQUIRE(RunSql(fx.conn, "SELECT 42") == DUCKDB_V2_ERROR_NONE);
	REQUIRE(RunSql(fx.conn, "SET enable_profiling = 'no_output'") == DUCKDB_V2_ERROR_NONE);
	REQUIRE(MetricsNodeMetricCount(root) == count);
	REQUIRE(FindMetricsNode(root, "UNGROUPED_AGGREGATE") == aggregate);
	REQUIRE(GetMetricCount(scan, "operator.intermediate_rows", rows));
	REQUIRE(rows == 1000);

	// A new snapshot describes the last query.
	auto latest = TakeMetricsSnapshot(fx.conn);
	REQUIRE_FALSE(FindMetricsNode(GetMetricsRoot(latest), "UNGROUPED_AGGREGATE"));
	duckdb_v2_query_metrics_destroy(&latest);

	// Once profiling is disabled, there is nothing to report.
	REQUIRE(duckdb_v2_connection_enable_profiling(fx.conn, false, nullptr) == DUCKDB_V2_ERROR_NONE);
	latest = TakeMetricsSnapshot(fx.conn);
	REQUIRE(MetricsNodeMetricCount(GetMetricsRoot(latest)) == 0);
	REQUIRE(MetricsNodeChildCount(GetMetricsRoot(latest)) == 0);
	duckdb_v2_query_metrics_destroy(&latest);
	REQUIRE(RunSql(fx.conn, METRICS_PROFILED_QUERY) == DUCKDB_V2_ERROR_NONE);
	latest = TakeMetricsSnapshot(fx.conn);
	REQUIRE_FALSE(FindMetricsNode(GetMetricsRoot(latest), "UNGROUPED_AGGREGATE"));
	duckdb_v2_query_metrics_destroy(&latest);

	// Enabling it again keeps the output format: still no output.
	REQUIRE(duckdb_v2_connection_enable_profiling(fx.conn, true, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(RunSql(fx.conn, METRICS_PROFILED_QUERY) == DUCKDB_V2_ERROR_NONE);
	latest = TakeMetricsSnapshot(fx.conn);
	REQUIRE(FindMetricsNode(GetMetricsRoot(latest), "UNGROUPED_AGGREGATE"));
	duckdb_v2_query_metrics_destroy(&latest);
	REQUIRE(duckdb_v2_connection_enable_profiling(nullptr, true, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	duckdb_v2_query_metrics_destroy(&metrics);
}

TEST_CASE("V2 query metrics: not while a query runs", "[capi_v2][query_metrics]") {
	EnvFixture fx;
	REQUIRE(RunSql(fx.conn, "SET enable_profiling = 'no_output'") == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_result_handle result = nullptr;
	REQUIRE(Query(fx.conn, "SELECT * FROM range(100000)", &result) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_data_chunk_handle chunk = nullptr;
	auto status = DUCKDB_V2_RESULT_STEP_STATUS_WAITING;
	REQUIRE(duckdb_v2_result_step(result, &chunk, &status, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_data_chunk_destroy(&chunk);

	duckdb_v2_query_metrics_handle metrics = nullptr;
	REQUIRE(duckdb_v2_connection_get_query_metrics(fx.conn, &metrics, nullptr) == DUCKDB_V2_ERROR_RESOURCE_IN_USE);
	REQUIRE(metrics == nullptr);

	// Once the result is gone, the query is finished. The snapshot of an abandoned query need not be complete.
	duckdb_v2_result_destroy(&result);
	metrics = TakeMetricsSnapshot(fx.conn);
	duckdb_v2_query_metrics_destroy(&metrics);

	// Once the result is drained, so is its query.
	REQUIRE(Query(fx.conn, "SELECT * FROM range(100000)", &result) == DUCKDB_V2_ERROR_NONE);
	while (status != DUCKDB_V2_RESULT_STEP_STATUS_FINISHED) {
		REQUIRE(duckdb_v2_result_step(result, &chunk, &status, nullptr) == DUCKDB_V2_ERROR_NONE);
		duckdb_v2_data_chunk_destroy(&chunk);
	}
	metrics = TakeMetricsSnapshot(fx.conn);
	REQUIRE(FindMetricsNode(GetMetricsRoot(metrics), "TABLE_SCAN"));
	duckdb_v2_query_metrics_destroy(&metrics);
	duckdb_v2_result_destroy(&result);
}

TEST_CASE("V2 query metrics: argument checks", "[capi_v2][query_metrics]") {
	EnvFixture fx;
	auto metrics = TakeMetricsSnapshot(fx.conn);
	auto root = GetMetricsRoot(metrics);

	duckdb_v2_query_metrics_node_handle child = nullptr;
	REQUIRE(duckdb_v2_query_metrics_node_get_child(root, 0, &child, nullptr) == DUCKDB_V2_ERROR_INPUT_OUT_OF_RANGE);
	REQUIRE(child == nullptr);
	duckdb_v2_str name = {nullptr, 0};
	REQUIRE(duckdb_v2_query_metrics_node_get_metric_name(root, 0, &name, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_OUT_OF_RANGE);
	duckdb_v2_value_handle value = nullptr;
	REQUIRE(duckdb_v2_query_metrics_node_get_metric_value(root, 0, &value, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_OUT_OF_RANGE);
	REQUIRE(value == nullptr);
	bool present = false;
	REQUIRE(duckdb_v2_query_metrics_node_get_metric(root, nullptr, &value, &present, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_connection_get_query_metrics(nullptr, &metrics, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	duckdb_v2_query_metrics_destroy(&metrics);

	// Destroying is null-safe.
	REQUIRE(duckdb_v2_query_metrics_destroy(&metrics) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_query_metrics_destroy(nullptr) == DUCKDB_V2_ERROR_NONE);
}

} // namespace test_capi_v2
