#include "duckdb/main/query_profiler.hpp"

#include "duckdb/common/enums/metric_type.hpp"
#include "duckdb/common/fstream.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/tree_renderer/base_tree_renderer.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/tree_renderer.hpp"
#include "duckdb/common/tree_renderer/text_tree_renderer.hpp"
#include "duckdb/execution/operator/scan/physical_column_data_scan.hpp"
#include "duckdb/execution/operator/scan/physical_table_scan.hpp"
#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/main/client_config.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/profiler/profiling_utils.hpp"
#include "duckdb/main/profiler/gathered_metrics.hpp"
#include "duckdb/main/profiler/metrics_manager.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/storage/buffer/buffer_pool.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/common/json_document.hpp"

#include <utility>

namespace duckdb {

void QueryProfileResult::AddValue(const string &k, Value val) {
	D_ASSERT(kind == QueryProfileResultKind::OBJECT);
	auto child = make_uniq<QueryProfileResult>();
	child->kind = QueryProfileResultKind::VALUE;
	child->key = k;
	child->value = std::move(val);
	children.push_back(std::move(child));
}

QueryProfileResult &QueryProfileResult::AddObject(const string &k) {
	D_ASSERT(kind == QueryProfileResultKind::OBJECT);
	auto child = make_uniq<QueryProfileResult>();
	child->kind = QueryProfileResultKind::OBJECT;
	child->key = k;
	auto &ref = *child;
	children.push_back(std::move(child));
	return ref;
}

QueryProfileResult &QueryProfileResult::AddList(const string &k) {
	D_ASSERT(kind == QueryProfileResultKind::OBJECT);
	auto child = make_uniq<QueryProfileResult>();
	child->kind = QueryProfileResultKind::LIST;
	child->key = k;
	auto &ref = *child;
	children.push_back(std::move(child));
	return ref;
}

QueryProfileResult &QueryProfileResult::AppendObject() {
	D_ASSERT(kind == QueryProfileResultKind::LIST);
	auto child = make_uniq<QueryProfileResult>();
	child->kind = QueryProfileResultKind::OBJECT;
	auto &ref = *child;
	children.push_back(std::move(child));
	return ref;
}

QueryProfileResult &QueryProfileResult::AppendList() {
	auto child = make_uniq<QueryProfileResult>();
	child->kind = QueryProfileResultKind::LIST;
	auto &ref = *child;
	children.push_back(std::move(child));
	return ref;
}

QueryProfiler::QueryProfiler(ClientContext &context_p)
    : context(context_p), running(false), query_started(false), query_requires_profiling(false),
      is_explain_analyze(false), metrics_finalized(false) {
}

bool QueryProfiler::IsEnabled() const {
	return is_explain_analyze || ClientConfig::GetConfig(context).enable_profiler;
}

unique_ptr<TreeRenderer> QueryProfiler::CreateProfiler(const string &name) const {
	return TreeRenderer::CreateRenderer(context, name);
}

unique_ptr<TreeRenderer> QueryProfiler::GetRenderer(const ProfilerPrintFormat &format) const {
	if (format == ProfilerPrintFormat::Default()) {
		// use the configured default profiler format; "no_output" still renders as a query tree when explicitly asked
		// for output (e.g. EXPLAIN ANALYZE), so fall back to it here
		auto name = ClientConfig::GetConfig(context).profiler_print_format;
		return CreateProfiler(name == "no_output" ? "query_tree" : name);
	}
	// resolve the explain format name (text/json/html/...) and create the matching renderer
	return CreateProfiler(format.ToString());
}

bool QueryProfiler::PrintOptimizerOutput() const {
	if (metrics) {
		return metrics->MetricIsTracked("optimizer.join_order");
	}
	// Fall back to checking tracked_metrics patterns directly
	auto &config = ClientConfig::GetConfig(context);
	for (const auto &pattern : config.tracked_metrics) {
		if (pattern == "*" || StringUtil::StartsWith(pattern, "optimizer")) {
			return true;
		}
	}
	return false;
}

string QueryProfiler::GetSaveLocation() const {
	return is_explain_analyze ? string() : ClientConfig::GetConfig(context).profiler_save_location;
}

QueryProfiler &QueryProfiler::Get(ClientContext &context) {
	return *ClientData::Get(context).profiler;
}

void QueryProfiler::Start(const string &query) {
	Reset();
	tracked_metric_patterns = make_uniq<GatheredMetrics>(ClientConfig::GetConfig(context).tracked_metrics);
	running = true;
	query_metrics.query_sql = query;
	query_metrics.latency_timer = make_uniq<MetricsTimer>(StartTimer<MetricQueryTotalTime>());
}

void QueryProfiler::Reset() {
	tree_map.clear();
	root = nullptr;
	metrics.reset();
	tracked_metric_patterns.reset();
	running = false;
	query_metrics.Reset();
	result_tree.reset();
	metrics_finalized = false;
}

void QueryProfiler::StartQuery(const string &query, bool is_explain_analyze_p, bool start_at_optimizer) {
	lock_guard<std::mutex> guard(lock);
	// the counters tracked without profiling hold per-query values: reset them once, as StartQuery is called
	// several times for one query
	if (!query_started) {
		query_started = true;
		query_metrics.ResetLiveMetrics();
		query_metrics.bytes_scanned = 0;
	}
	if (is_explain_analyze_p) {
		StartExplainAnalyze();
	}
	if (!IsEnabled()) {
		return;
	}
	if (start_at_optimizer && !PrintOptimizerOutput()) {
		// This is the StartQuery call before the optimizer, but we don't have to print optimizer output
		return;
	}
	if (running) {
		// Called while already running: this happens when statement setup follows parser timing,
		// or when we print optimizer output.
		// D_ASSERT(PrintOptimizerOutput());
		query_metrics.query_sql = query;
		return;
	}
	Start(query);
}
void QueryProfiler::AddParserTime(const Profiler &parser_timer, timestamp_t start_time, timestamp_t end_time) {
	if (!running || !IsEnabled()) {
		return;
	}
	auto parser_time_ns = parser_timer.ElapsedNanos();
	if (!parser_time_ns) {
		return;
	}
	query_metrics.UpdateMetric(MetricParserTotalTime::Name, parser_time_ns, start_time, end_time);
}

bool QueryProfiler::OperatorRequiresProfiling(const PhysicalOperatorType op_type) {
	const auto &config = ClientConfig::GetConfig(context);
	if (config.profiling_coverage == ProfilingCoverage::ALL) {
		return true;
	}

	switch (op_type) {
	case PhysicalOperatorType::ORDER_BY:
	case PhysicalOperatorType::RESERVOIR_SAMPLE:
	case PhysicalOperatorType::STREAMING_SAMPLE:
	case PhysicalOperatorType::LIMIT:
	case PhysicalOperatorType::LIMIT_PERCENT:
	case PhysicalOperatorType::STREAMING_LIMIT:
	case PhysicalOperatorType::TOP_N:
	case PhysicalOperatorType::WINDOW:
	case PhysicalOperatorType::UNNEST:
	case PhysicalOperatorType::UNGROUPED_AGGREGATE:
	case PhysicalOperatorType::HASH_GROUP_BY:
	case PhysicalOperatorType::FILTER:
	case PhysicalOperatorType::PROJECTION:
	case PhysicalOperatorType::COPY_TO_FILE:
	case PhysicalOperatorType::TABLE_SCAN:
	case PhysicalOperatorType::CHUNK_SCAN:
	case PhysicalOperatorType::DELIM_SCAN:
	case PhysicalOperatorType::EXPRESSION_SCAN:
	case PhysicalOperatorType::BLOCKWISE_NL_JOIN:
	case PhysicalOperatorType::NESTED_LOOP_JOIN:
	case PhysicalOperatorType::HASH_JOIN:
	case PhysicalOperatorType::CROSS_PRODUCT:
	case PhysicalOperatorType::PIECEWISE_MERGE_JOIN:
	case PhysicalOperatorType::IE_JOIN:
	case PhysicalOperatorType::LEFT_DELIM_JOIN:
	case PhysicalOperatorType::RIGHT_DELIM_JOIN:
	case PhysicalOperatorType::UNION:
	case PhysicalOperatorType::CTE:
	case PhysicalOperatorType::CTE_SCAN:
	case PhysicalOperatorType::RECURSIVE_CTE:
	case PhysicalOperatorType::RECURSIVE_KEY_CTE:
	case PhysicalOperatorType::RECURSIVE_KEY_JOIN:
	case PhysicalOperatorType::EMPTY_RESULT:
	case PhysicalOperatorType::EXTENSION:
		return true;
	default:
		return false;
	}
}

void QueryProfiler::ResetQueryStart() {
	lock_guard<std::mutex> guard(lock);
	query_started = false;
}

void QueryProfiler::StartExplainAnalyze() {
	is_explain_analyze = true;
}

void QueryProfiler::EndQuery() {
	unique_lock<std::mutex> guard(lock);
	query_started = false;
	if (!running) {
		return;
	}
	if (!IsEnabled()) {
		Reset();
		return;
	}

	FinalizeMetricsInternal();
	running = false;
	bool emit_output = false;

	// Print or output the query profiling after query termination.
	// EXPLAIN ANALYZE output is not written by the profiler, and the "no_output" format emits no output.
	if (!is_explain_analyze && ClientConfig::GetConfig(context).profiler_print_format != "no_output") {
		emit_output = true;
	}

	is_explain_analyze = false;
	// a query that failed before it was planned has no tree to print; its profile still replaces the saved one
	const bool has_plan = root != nullptr;

	// To log is inexpensive, whether to log or not depends on whether logging is active
	ToLogInternal();

	guard.unlock();

	if (emit_output) {
		auto save_location = GetSaveLocation();
		if (!save_location.empty()) {
			string tree = ToString();
			WriteToFile(save_location.c_str(), tree);
		} else if (has_plan) {
			// print directly through the renderer's print sink, the blank line that separates consecutive profiles
			// included: a pluggable renderer (ProfilerExtension) whose sink keeps the output elsewhere prints nothing
			auto renderer = GetRenderer();
			PrintProfilerOutput(renderer.get(), "\n\n");
		}
	}
}

void QueryProfiler::FinalizeMetrics() {
	lock_guard<std::mutex> guard(lock);
	FinalizeMetricsInternal();
}

profiler_metrics_t QueryProfiler::GetLiveMetrics() const {
	// StartQuery resets the counters under the lock, so a snapshot never mixes two queries
	lock_guard<std::mutex> guard(lock);
	return query_metrics.GetLiveMetrics();
}

void QueryProfiler::TrackBytesRead(const idx_t amount, const idx_t elapsed_us) {
	query_metrics.UpdateBytesRead(amount, elapsed_us);
}

void QueryProfiler::TrackBytesWritten(const idx_t amount, const idx_t elapsed_us) {
	query_metrics.UpdateBytesWritten(amount, elapsed_us);
}

void QueryProfiler::TrackBytesSpilled(const idx_t amount) {
	query_metrics.UpdateBytesSpilled(amount);
}

void QueryProfiler::TrackBytesScanned(const idx_t amount) {
	query_metrics.UpdateBytesScanned(amount);
}

void QueryProfiler::ResetBytesScanned() {
	query_metrics.bytes_scanned = 0;
}

void QueryProfiler::TrackTotalMemoryAllocated(const idx_t amount) {
	query_metrics.UpdateTotalMemoryAllocated(amount);
}

void QueryProfiler::AddToMetricCounter(const string &key, const idx_t amount) {
	if (IsEnabled() && running) {
		query_metrics.UpdateMetricCounter(key, amount);
	}
}

void QueryProfiler::AddToMetricTiming(const string &key, const idx_t elapsed_ns) {
	if (IsEnabled() && running) {
		query_metrics.UpdateMetric(key, elapsed_ns);
	}
}

void QueryProfiler::SetMetric(const string &key, Value new_value) {
	if (!IsEnabled() || !running) {
		return;
	}
	// only the metrics extensions registered: the profiler sets its own, and leaves some out on purpose
	if (!MetricsManager::Get(context).IsRegisteredMetric(key)) {
		return;
	}
	// the query's metrics are only gathered once its plan is built, so the value is kept with the query until then:
	// an extension may set it while the query binds
	query_metrics.SetMetricValue(key, std::move(new_value));
}

bool QueryProfiler::MetricIsTracked(const string &key) const {
	if (!IsEnabled() || !running) {
		return false;
	}
	if (metrics) {
		return metrics->MetricIsTracked(key);
	}
	// before the plan is built there are no gathered metrics to ask
	return tracked_metric_patterns && tracked_metric_patterns->MetricIsTracked(key);
}

idx_t QueryProfiler::GetBytesRead() const {
	return query_metrics.GetBytesRead();
}

idx_t QueryProfiler::GetBytesWritten() const {
	return query_metrics.GetBytesWritten();
}

idx_t QueryProfiler::GetBytesScanned() const {
	return query_metrics.GetBytesScanned();
}

MetricsTimer QueryProfiler::StartTimerInternal(const string &key) {
	return MetricsTimer(query_metrics, key, IsEnabled());
}

string QueryProfiler::ToString(const ProfilerPrintFormat &format) const {
	auto renderer = GetRenderer(format);
	return RenderProfilerOutput(renderer.get());
}

string QueryProfiler::ToString(const string &profiler_format_name) const {
	auto renderer = CreateProfiler(profiler_format_name);
	return RenderProfilerOutput(renderer.get());
}

//! Strip a leading EXPLAIN [ANALYZE] [(...)] wrapper so only the underlying query remains.
static string StripExplainPrefix(const string &sql) {
	auto is_space = [](char c) {
		return c == ' ' || c == '\t' || c == '\n' || c == '\r';
	};
	idx_t start = 0;
	while (start < sql.size() && is_space(sql[start])) {
		start++;
	}
	string rest = sql.substr(start);
	if (!StringUtil::CIStartsWith(rest, "EXPLAIN")) {
		return sql;
	}
	idx_t pos = 7; // past "EXPLAIN"
	while (pos < rest.size() && is_space(rest[pos])) {
		pos++;
	}
	if (StringUtil::CIStartsWith(std::string_view(rest).substr(pos), "ANALYZE")) {
		pos += 7;
		while (pos < rest.size() && is_space(rest[pos])) {
			pos++;
		}
	}
	if (pos < rest.size() && rest[pos] == '(') {
		idx_t depth = 0;
		while (pos < rest.size()) {
			char c = rest[pos++];
			if (c == '(') {
				depth++;
			} else if (c == ')' && --depth == 0) {
				break;
			}
		}
		while (pos < rest.size() && is_space(rest[pos])) {
			pos++;
		}
	}
	return rest.substr(pos);
}

string QueryProfiler::GetQuerySQL() const {
	auto sql = StripExplainPrefix(query_metrics.query_sql);
	return sql.empty() ? query_metrics.query_sql : sql;
}

string QueryProfiler::RenderProfile(const string &format) const {
	auto renderer = CreateProfiler(format);
	if (!renderer) {
		return string();
	}
	StringTreeRenderer ss;
	renderer->RenderProfiler(*this, ss);
	renderer->Finish();
	return ss.str();
}

string QueryProfiler::RenderProfilerOutput(optional_ptr<TreeRenderer> renderer) const {
	if (!renderer) {
		// "no_output" format: nothing is rendered, enabled or not
		return "";
	}
	if (!IsEnabled()) {
		return renderer->RenderProfilerDisabled();
	}
	StringTreeRenderer ss;
	renderer->RenderProfiler(*this, ss);
	renderer->Finish();
	return ss.str();
}

void QueryProfiler::PrintProfilerOutput(optional_ptr<TreeRenderer> renderer, const string &trailer) const {
	if (!renderer) {
		// "no_output" format: nothing is rendered, enabled or not
		return;
	}
	// only created now that we are actually printing
	auto sink = renderer->GetPrintRenderer();
	if (!IsEnabled()) {
		*sink << renderer->RenderProfilerDisabled();
		return;
	}
	renderer->RenderProfiler(*this, *sink);
	renderer->Finish();
	*sink << trailer;
}

void QueryProfiler::RenderProfilingNodeTree(TreeRenderer &renderer, BaseTreeRenderer &ss) const {
	lock_guard<std::mutex> guard(lock);
	// checking the tree to ensure the query is really empty
	// the query string is empty when a logical plan is deserialized
	if (query_metrics.query_sql.empty() || !root) {
		return;
	}
	renderer.Render(*root, ss);
}

OperatorProfiler::OperatorProfiler(ClientContext &context) : context(context) {
	enabled = QueryProfiler::Get(context).IsEnabled();
}

void OperatorProfiler::StartOperator(optional_ptr<const PhysicalOperator> phys_op) {
	if (!enabled) {
		return;
	}
	if (active_operator) {
		throw InternalException("OperatorProfiler: Attempting to call StartOperator while another operator is active");
	}
	active_operator = phys_op;

	if (!OperatorMetricsIsInitialized(*active_operator)) {
		// first time calling into this operator - fetch the info
		auto &info = GetOperatorMetrics(*active_operator);
		info.SetExtraInfo(active_operator->ParamsToString());
	}

	// Start the timing of the current operator.
	op.Start();
}

void OperatorMetrics::GatherMetrics(ClientContext &context, double elapsed_time, optional_ptr<DataChunk> chunk) {
	time += elapsed_time;
	if (chunk) {
		elements_returned += chunk->size();
		intermediate_size_bytes += LossyNumericCast<idx_t>(chunk->GetDataSize());
	}
	auto &buffer_manager = BufferManager::GetBufferManager(context);
	auto used_memory = buffer_manager.GetBufferPool().GetUsedMemory(false);
	if (used_memory > system_peak_buffer_manager_memory) {
		system_peak_buffer_manager_memory = used_memory;
	}
	auto used_swap = buffer_manager.GetUsedSwap();
	if (used_swap > system_peak_temp_directory_size) {
		system_peak_temp_directory_size = used_swap;
	}
}

void OperatorMetrics::MergeInternal(const OperatorMetrics &other) {
	time += other.time;
	elements_returned += other.elements_returned;
	intermediate_size_bytes += other.intermediate_size_bytes;
	rows_scanned += other.rows_scanned;
	row_groups_scanned += other.row_groups_scanned;
	bytes_scanned += other.bytes_scanned;
	bytes_scanned_reported = bytes_scanned_reported || other.bytes_scanned_reported;
	if (other.system_peak_buffer_manager_memory > system_peak_buffer_manager_memory) {
		system_peak_buffer_manager_memory = other.system_peak_buffer_manager_memory;
	}
	if (other.system_peak_temp_directory_size > system_peak_temp_directory_size) {
		system_peak_temp_directory_size = other.system_peak_temp_directory_size;
	}
}

void OperatorMetrics::Accumulate(const OperatorMetrics &other) {
	MergeInternal(other);
	total_row_groups_to_scan += other.total_row_groups_to_scan;
}

void OperatorMetrics::Merge(const OperatorMetrics &other) {
	MergeInternal(other);
	total_row_groups_to_scan = MaxValue<idx_t>(total_row_groups_to_scan, other.total_row_groups_to_scan);
}

void OperatorProfiler::EndOperator(optional_ptr<DataChunk> chunk) {
	if (!enabled) {
		return;
	}
	if (!active_operator) {
		throw InternalException("OperatorProfiler: Attempting to call EndOperator while no operator is active");
	}

	auto &info = GetOperatorMetrics(*active_operator);
	op.End();
	info.GatherMetrics(context, op.Elapsed(), chunk);
	active_operator = nullptr;
}

void OperatorProfiler::FinishSource(GlobalSourceState &gstate, LocalSourceState &lstate) {
	if (!enabled) {
		return;
	}
	if (!active_operator) {
		throw InternalException("OperatorProfiler: Attempting to call FinishSource while no operator is active");
	}
	FinishSource(*active_operator, gstate, lstate);
}

void OperatorProfiler::FinishSource(const PhysicalOperator &phys_op, GlobalSourceState &gstate,
                                    LocalSourceState &lstate) {
	if (phys_op.type == PhysicalOperatorType::TABLE_SCAN) {
		const auto &table_scan = phys_op.Cast<PhysicalTableScan>();
		auto &scan_metrics = GetOperatorMetrics(phys_op);
		table_scan.GetMetrics(context, gstate, lstate, scan_metrics);
	}
}

bool OperatorProfiler::OperatorMetricsIsInitialized(const PhysicalOperator &phys_op) {
	auto entry = operator_metrics.find(phys_op);
	return entry != operator_metrics.end();
}

OperatorMetrics &OperatorProfiler::GetOperatorMetrics(const PhysicalOperator &phys_op) {
	auto entry = operator_metrics.find(phys_op);
	if (entry != operator_metrics.end()) {
		return entry->second;
	}

	// Add a new entry.
	operator_metrics[phys_op] = OperatorMetrics();
	return operator_metrics[phys_op];
}

void OperatorProfiler::Flush(const PhysicalOperator &phys_op) {
	auto entry = operator_metrics.find(phys_op);
	if (entry == operator_metrics.end()) {
		return;
	}

	auto &info = entry->second;
	if (info.name.empty()) {
		info.name = phys_op.GetName();
	}
}

void QueryProfiler::Flush(OperatorProfiler &profiler) {
	if (!running.load(std::memory_order_relaxed)) {
		return;
	}
	lock_guard<std::mutex> guard(lock);
	if (!IsEnabled() || !running) {
		return;
	}
	for (auto &node : profiler.operator_metrics) {
		auto &op = node.first.get();
		auto entry = tree_map.find(op);
		// all profiled operators should be registered in the tree
		D_ASSERT(entry != tree_map.end());
		if (entry == tree_map.end()) {
			continue;
		}
		auto &tree_node = entry->second.get();
		auto &info = tree_node.GetOperatorMetrics();
		info.Merge(node.second);
		// Update extra_info from the per-thread metrics: these are set during execution (StartOperator),
		// so they capture runtime values like dynamic filters that aren't known at plan-creation time.
		if (!node.second.GetExtraInfo().empty()) {
			info.SetExtraInfo(node.second.GetExtraInfo());
		}

		if (node.second.system_peak_buffer_manager_memory > query_metrics.system_peak_buffer_memory) {
			query_metrics.system_peak_buffer_memory = node.second.system_peak_buffer_manager_memory;
		}
		if (node.second.system_peak_temp_directory_size > query_metrics.system_peak_temp_dir_size) {
			query_metrics.system_peak_temp_dir_size = node.second.system_peak_temp_directory_size;
		}
		node.second.ResetMetrics();
	}
}

void QueryProfiler::SetBlockedTime(const double &blocked_thread_time) {
	if (!running.load(std::memory_order_relaxed)) {
		return;
	}
	lock_guard<std::mutex> guard(lock);
	if (!IsEnabled() || !running) {
		return;
	}

	query_metrics.blocked_thread_time = blocked_thread_time;
}

void QueryProfiler::SetStreamingPeakBufferSize(idx_t peak_bytes) {
	lock_guard<std::mutex> guard(lock);
	if (!IsEnabled() || !running) {
		return;
	}
	query_metrics.system_peak_streaming_buffer_size = peak_bytes;
}

string QueryProfiler::DrawPadded(const string &str, idx_t width) {
	if (str.size() > width) {
		return str.substr(0, width);
	} else {
		width -= str.size();
		auto half_spaces = width / 2;
		auto extra_left_space = NumericCast<idx_t>(width % 2 != 0 ? 1 : 0);
		return string(half_spaces + extra_left_space, ' ') + str + string(half_spaces, ' ');
	}
}

static string RenderTiming(double timing) {
	string timing_s;
	if (timing >= 1) {
		timing_s = StringUtil::Format("%.2f", timing);
	} else if (timing >= 0.1) {
		timing_s = StringUtil::Format("%.3f", timing);
	} else {
		timing_s = StringUtil::Format("%.4f", timing);
	}
	return timing_s + "s";
}

string QueryProfiler::QueryTreeToString() const {
	StringTreeRenderer ss;
	RenderQueryTree(ss);
	return ss.str();
}

void QueryProfiler::QueryTreeToStream(std::ostream &ss) const {
	StringTreeRenderer renderer;
	RenderQueryTree(renderer);
	ss << renderer.str();
}

void QueryProfiler::RenderQueryTree(BaseTreeRenderer &ss) const {
	lock_guard<std::mutex> guard(lock);

	// the query string is empty when a logical plan is deserialized
	if (query_metrics.query_sql.empty() && !root) {
		return;
	}

	// the registered states write profiling info through an ostream - capture it and emit as layout text
	duckdb::stringstream state_info;
	for (auto &state : context.registered_state->States()) {
		state->WriteProfilingInformation(state_info);
	}
	ss << state_info.str();

	// summary box, styled to match the operator boxes (rounded corners, title in the top border)
	const string title = "Summary";
	vector<pair<string, string>> rows;
	rows.emplace_back("Total Time: ", RenderTiming(query_metrics.GetStringMetricInSeconds("query.total_time")));
	auto bytes_read = query_metrics.GetBytesRead();
	if (bytes_read > 0) {
		rows.emplace_back("Data Read: ", StringUtil::BytesToHumanReadableString(bytes_read, 1000));
	}
	auto bytes_written = query_metrics.GetBytesWritten();
	if (bytes_written > 0) {
		rows.emplace_back("Data Written: ", StringUtil::BytesToHumanReadableString(bytes_written, 1000));
	}
	idx_t content_width = title.size() + 2;
	for (auto &row : rows) {
		content_width = MaxValue<idx_t>(content_width, row.first.size() + row.second.size());
	}
	idx_t box_width = content_width + 4;
	// top border: ╭─ Summary ─╮
	ss << "╭─ ";
	ss.Render(title, TreeRenderType::HEADER);
	ss << " " + StringUtil::Repeat("─", box_width - 5 - title.size()) + "╮\n";
	// content rows with the values right-aligned (pad between the key and the value)
	for (auto &row : rows) {
		idx_t row_width = row.first.size() + row.second.size();
		ss << "│ ";
		ss.Render(row.first, TreeRenderType::KEY);
		ss << string(content_width - row_width, ' ');
		ss.Render(row.second, TreeRenderType::VALUE);
		ss << " │\n";
	}
	// bottom border
	ss << "╰" + StringUtil::Repeat("─", box_width - 2) + "╯\n";
	// render the main operator tree
	if (root) {
		Render(*root, ss);
	}
}

Value QueryProfiler::JSONSanitize(const Value &input) {
	D_ASSERT(input.type().id() == LogicalTypeId::MAP);

	InsertionOrderPreservingMap<string> result;
	auto children = MapValue::GetChildren(input);
	for (auto &child : children) {
		auto struct_children = StructValue::GetChildren(child);
		auto key = struct_children[0].GetValue<string>();
		auto value = struct_children[1].GetValue<string>();

		if (StringUtil::StartsWith(key, "__")) {
			key = StringUtil::Replace(key, "__", "");
			key = StringUtil::Replace(key, "_", " ");
			key = StringUtil::Title(key);
		}
		result[key] = value;
	}
	return Value::MAP(result);
}

string QueryProfiler::JSONSanitize(const std::string &text) {
	string result;
	result.reserve(text.size());
	for (char i : text) {
		switch (i) {
		case '\b':
			result += "\\b";
			break;
		case '\f':
			result += "\\f";
			break;
		case '\n':
			result += "\\n";
			break;
		case '\r':
			result += "\\r";
			break;
		case '\t':
			result += "\\t";
			break;
		case '"':
			result += "\\\"";
			break;
		case '\\':
			result += "\\\\";
			break;
		default:
			result += i;
			break;
		}
	}
	return result;
}

profiler_metrics_t OperatorMetrics::GetMetrics(const GatheredMetrics &info) const {
	profiler_metrics_t result;
	if (info.MetricIsTracked<MetricOperatorType>()) {
		result["type"] = Value(EnumUtil::ToString(operator_type));
	}
	if (info.MetricIsTracked<MetricOperatorName>()) {
		result["name"] = Value(name);
	}
	if (info.MetricIsTracked<MetricOperatorTiming>()) {
		result["timing"] = Value::DOUBLE(time);
	}
	if (info.MetricIsTracked<MetricOperatorIntermediateRows>()) {
		result["intermediate_rows"] = Value::UBIGINT(elements_returned);
	}
	if (info.MetricIsTracked<MetricOperatorIntermediateSizeBytes>()) {
		result["intermediate_size_bytes"] = Value::UBIGINT(intermediate_size_bytes);
	}
	if (info.MetricIsTracked<MetricOperatorRowsScanned>() && operator_type == PhysicalOperatorType::TABLE_SCAN) {
		result["rows_scanned"] = Value::UBIGINT(rows_scanned);
	}
	if (info.MetricIsTracked<MetricOperatorRowGroupsScanned>() && operator_type == PhysicalOperatorType::TABLE_SCAN) {
		result["row_groups_scanned"] = Value::UBIGINT(row_groups_scanned);
	}
	if (info.MetricIsTracked<MetricOperatorBytesScanned>() && operator_type == PhysicalOperatorType::TABLE_SCAN &&
	    bytes_scanned_reported) {
		result["bytes_scanned"] = Value::UBIGINT(bytes_scanned);
	}
	if (info.MetricIsTracked<MetricOperatorTotalRowGroupsToScan>() &&
	    operator_type == PhysicalOperatorType::TABLE_SCAN) {
		result["total_row_groups_to_scan"] = Value::UBIGINT(total_row_groups_to_scan);
	}
	if (info.MetricIsTracked<MetricOperatorExtraInfo>()) {
		result["extra_info"] = QueryProfiler::JSONSanitize(Value::MAP(extra_info));
	}
	return result;
}

static JSONMutableValue ValueToJSON(JSONWriter &writer, const Value &val) {
	if (val.IsNull()) {
		return writer.CreateNull();
	}
	auto &type = val.type();
	if (type.id() == LogicalTypeId::MAP) {
		// MAP values (e.g. extra_info) become JSON objects; multiline string values become arrays
		auto obj = writer.CreateObject();
		for (auto &child : MapValue::GetChildren(val)) {
			auto kv = StructValue::GetChildren(child);
			auto k = kv[0].GetValue<string>();
			auto v = kv[1].GetValue<string>();
			auto splits = StringUtil::Split(v, "\n");
			if (splits.size() > 1) {
				auto arr = writer.CreateArray();
				for (auto &s : splits) {
					arr.AppendString(s);
				}
				obj.Add(k, arr);
			} else {
				obj.AddString(k, v);
			}
		}
		return obj;
	}
	if (type.IsIntegral()) {
		return writer.CreateUnsignedInteger(val.GetValue<uint64_t>());
	}
	if (type.IsNumeric()) {
		return writer.CreateDouble(val.GetValue<double>());
	}
	return writer.CreateString(val.GetValue<string>());
}

static JSONMutableValue QueryProfileResultToJSON(JSONWriter &writer, const QueryProfileResult &node) {
	switch (node.kind) {
	case QueryProfileResultKind::VALUE:
		return ValueToJSON(writer, node.value);
	case QueryProfileResultKind::LIST: {
		auto arr = writer.CreateArray();
		for (auto &child : node.children) {
			arr.Append(QueryProfileResultToJSON(writer, *child));
		}
		return arr;
	}
	case QueryProfileResultKind::OBJECT: {
		auto obj = writer.CreateObject();
		// Sort children alphabetically by key for deterministic output
		vector<reference<const QueryProfileResult>> sorted_children;
		sorted_children.reserve(node.children.size());
		for (auto &child : node.children) {
			sorted_children.push_back(*child);
		}
		std::sort(sorted_children.begin(), sorted_children.end(),
		          [](const QueryProfileResult &a, const QueryProfileResult &b) {
			          if (a.IsNested() != b.IsNested()) {
				          return !a.IsNested();
			          }
			          return a.key < b.key;
		          });
		for (const QueryProfileResult &child : sorted_children) {
			D_ASSERT(!child.key.empty());
			obj.Add(child.key, QueryProfileResultToJSON(writer, child));
		}
		return obj;
	}
	default:
		throw InternalException("Unknown QueryProfileResultKind");
	}
}

void QueryProfiler::ToLogInternal() const {
	if (!root) {
		return;
	}
	metrics->WriteMetricsToLog(context, query_metrics.GetMetricTimings());
}

void QueryProfiler::ToLog() const {
	lock_guard<std::mutex> guard(lock);
	ToLogInternal();
}

static void OperatorToResultTree(const GatheredMetrics &settings, ProfilingNode &node, QueryProfileResult &result) {
	auto operator_metrics = node.GetOperatorMetrics().GetMetrics(settings);
	for (auto &entry : operator_metrics) {
		result.AddValue(entry.first, std::move(entry.second));
	}
	if (node.GetChildCount() > 0) {
		auto &children_list = result.AddList("children");
		for (idx_t i = 0; i < node.GetChildCount(); i++) {
			auto &child_result = children_list.AppendObject();
			OperatorToResultTree(settings, *node.GetChild(i), child_result);
		}
	}
}

struct LegacyCumulative {
	double timing = 0;
	uint64_t cardinality = 0;
	uint64_t rows_scanned = 0;
};

static LegacyCumulative LegacyOperatorToResultTree(const GatheredMetrics &info, ProfilingNode &node,
                                                   QueryProfileResult &result) {
	auto operator_metrics = node.GetOperatorMetrics().GetMetrics(info);

	auto emit_as = [&](const string &old_key, const string &new_key) {
		auto it = operator_metrics.find(old_key);
		if (it != operator_metrics.end()) {
			result.AddValue(new_key, it->second);
		}
	};

	emit_as("type", "operator_type");
	emit_as("name", "operator_name");
	emit_as("timing", "operator_timing");
	emit_as("rows_scanned", "operator_rows_scanned");
	emit_as("intermediate_rows", "operator_cardinality");
	emit_as("intermediate_size_bytes", "result_set_size");

	auto it_extra = operator_metrics.find("extra_info");
	if (it_extra != operator_metrics.end()) {
		result.AddValue("extra_info", it_extra->second);
	}
	result.AddValue("system_peak_buffer_memory", Value::UBIGINT(0));
	result.AddValue("system_peak_streaming_buffer_size", Value::UBIGINT(0));
	result.AddValue("system_peak_temp_dir_size", Value::UBIGINT(0));

	LegacyCumulative cumulative;
	auto timing_it = operator_metrics.find("timing");
	if (timing_it != operator_metrics.end()) {
		cumulative.timing = timing_it->second.GetValue<double>();
	}
	auto card_it = operator_metrics.find("intermediate_rows");
	if (card_it != operator_metrics.end()) {
		cumulative.cardinality = card_it->second.GetValue<uint64_t>();
	}
	auto rows_it = operator_metrics.find("rows_scanned");
	if (rows_it != operator_metrics.end()) {
		cumulative.rows_scanned = rows_it->second.GetValue<uint64_t>();
	}

	if (node.GetChildCount() > 0) {
		auto &children_list = result.AddList("children");
		for (idx_t i = 0; i < node.GetChildCount(); i++) {
			auto &child_result = children_list.AppendObject();
			auto child_cum = LegacyOperatorToResultTree(info, *node.GetChild(i), child_result);
			cumulative.timing += child_cum.timing;
			cumulative.cardinality += child_cum.cardinality;
			cumulative.rows_scanned += child_cum.rows_scanned;
		}
	}

	result.AddValue("cpu_time", Value::DOUBLE(cumulative.timing));
	result.AddValue("cumulative_cardinality", Value::UBIGINT(cumulative.cardinality));
	result.AddValue("cumulative_rows_scanned", Value::UBIGINT(cumulative.rows_scanned));
	return cumulative;
}

unique_ptr<QueryProfileResult> QueryProfiler::ToLegacyResultTree() const {
	auto result = make_uniq<QueryProfileResult>();
	if (!root) {
		result->AddValue("result", Value(query_metrics.query_sql.empty() ? "empty" : "error"));
		return result;
	}

	const auto &gathered = metrics->GetMetrics();

	auto emit = [&](const string &new_key, const string &old_key) {
		auto it = gathered.find(old_key);
		if (it != gathered.end()) {
			result->AddValue(new_key, it->second);
		}
	};

	emit("total_memory_allocated", "system.total_memory_allocated");
	emit("total_bytes_written", "io.total_bytes_written");
	emit("total_write_operations", "io.total_write_operations");
	emit("total_bytes_read", "io.total_bytes_read");
	emit("total_read_operations", "io.total_read_operations");
	emit("total_write_time", "io.total_write_time");
	emit("total_read_time", "io.total_read_time");
	emit("system_peak_temp_dir_size", "system.peak_temp_dir_size");
	emit("total_bytes_spilled", "system.total_bytes_spilled");
	emit("system_peak_buffer_memory", "system.peak_buffer_memory");
	emit("system_peak_streaming_buffer_size", "system.peak_streaming_buffer_size");

	// rows_returned = root operator's elements_returned (rows sent to client)
	{
		auto root_op_metrics = root->GetOperatorMetrics().GetMetrics(*metrics);
		auto it = root_op_metrics.find("intermediate_rows");
		if (it != root_op_metrics.end()) {
			result->AddValue("rows_returned", it->second);
		}
	}

	emit("result_set_size", "query.total_intermediate_size_bytes");
	emit("latency", "query.total_time");
	emit("wal_replay_entry_count", "storage.wal_replay_entry_count");
	result->AddValue("extra_info", Value::MAP(InsertionOrderPreservingMap<string>()));
	emit("commit_local_storage_latency", "storage.commit_local_storage_latency");
	emit("attach_load_storage_latency", "storage.attach_load_storage_latency");
	emit("query_name", "query.sql");
	emit("cpu_time", "query.cpu_time");
	emit("checkpoint_latency", "storage.checkpoint_latency");
	emit("cumulative_cardinality", "query.total_intermediate_rows");
	emit("waiting_to_attach_latency", "storage.waiting_to_attach_latency");
	emit("write_to_wal_latency", "storage.write_to_wal_latency");
	emit("attach_replay_wal_latency", "storage.attach_replay_wal_latency");
	emit("blocked_thread_time", "system.blocked_thread_time");
	emit("cumulative_rows_scanned", "query.total_rows_scanned");
	emit("total_vacuum_time", "storage.total_vacuum_time");

	auto &children_list = result->AddList("children");
	auto &root_node = children_list.AppendObject();
	LegacyOperatorToResultTree(*metrics, *root, root_node);
	return result;
}

unique_ptr<QueryProfileResult> QueryProfiler::ToResultTree() const {
	if (Settings::Get<LegacyMetricsFormatSetting>(context)) {
		return ToLegacyResultTree();
	}
	auto result = make_uniq<QueryProfileResult>();
	if (!root) {
		result->AddValue("result", Value(query_metrics.query_sql.empty() ? "empty" : "error"));
		return result;
	}
	metrics->MetricsToProfileResult(*result);
	if (metrics->AnyOperatorMetricTracked()) {
		auto &op_list = result->AddList("operator");
		auto &op_node = op_list.AppendObject();
		OperatorToResultTree(*metrics, *root, op_node);
	}
	return result;
}

QueryProfileResult &QueryProfiler::GetResult() {
	lock_guard<std::mutex> guard(lock);
	if (!result_tree) {
		result_tree = ToResultTree();
	}
	return *result_tree;
}

void QueryProfiler::GetFinalizedProfileUnderLock(
    const std::function<void(optional_ptr<const GatheredMetrics>, optional_ptr<const ProfilingNode>)> &callback) const {
	lock_guard<std::mutex> guard(lock);
	// a profile left over from before profiling was disabled is stale, and a running one is not finalized yet
	if (!IsEnabled() || running || !metrics_finalized) {
		callback(nullptr, nullptr);
		return;
	}
	callback(metrics.get(), root.get());
}

bool QueryProfiler::HasRoot() const {
	return root != nullptr;
}

string QueryProfiler::ToJSON() const {
	lock_guard<std::mutex> guard(lock);
	JSONWriter writer;
	auto result = ToResultTree();
	writer.SetRoot(QueryProfileResultToJSON(writer, *result));
	return writer.ToString(JSONWriteFlags::ALLOW_INF_AND_NAN | JSONWriteFlags::PRETTY);
}

void QueryProfiler::WriteToFile(const char *path, string &info) const {
	auto &fs = FileSystem::GetFileSystem(context);
	auto flags = FileOpenFlags::FILE_FLAGS_WRITE | FileOpenFlags::FILE_FLAGS_FILE_CREATE_NEW;
	auto file = fs.OpenFile(path, flags);
	if (!file) {
		throw IOException("Could not open the profiling output file \"%s\" for writing", path);
	}
	file->Write((void *)info.c_str(), info.size());
	file->Close();
}

unique_ptr<ProfilingNode> QueryProfiler::CreateTree(const PhysicalOperator &root_p, const idx_t depth) {
	if (OperatorRequiresProfiling(root_p.type)) {
		query_requires_profiling = true;
	}

	auto node = make_uniq<ProfilingNode>();
	auto &info = node->GetOperatorMetrics();
	node->depth = depth;

	info.name = root_p.GetName();
	info.operator_type = root_p.type;
	auto params = root_p.ParamsToString();
	info.SetExtraInfo(std::move(params));

	tree_map.insert(make_pair(reference<const PhysicalOperator>(root_p), reference<ProfilingNode>(*node)));
	if (root_p.type == PhysicalOperatorType::CTE_SCAN) {
		auto &cte_scan = root_p.Cast<PhysicalColumnDataScan>();
		if (cte_scan.cte_source) {
			tree_map.insert(
			    make_pair(reference<const PhysicalOperator>(*cte_scan.cte_source), reference<ProfilingNode>(*node)));
		}
	}
	auto children = root_p.GetChildren();
	for (auto &child : children) {
		auto child_node = CreateTree(child.get(), depth + 1);
		node->AddChild(std::move(child_node));
	}
	return node;
}

void QueryProfiler::Initialize(const PhysicalOperator &root_op) {
	lock_guard<std::mutex> guard(lock);
	if (!IsEnabled() || !running) {
		return;
	}
	query_requires_profiling = false;
	root = CreateTree(root_op, 0);
	if (!query_requires_profiling) {
		// query does not require profiling: disable profiling for this query
		running = false;
		tree_map.clear();
		root = nullptr;
	} else {
		auto &client_config = ClientConfig::GetConfig(context);
		metrics = make_uniq<GatheredMetrics>(client_config.tracked_metrics);
	}
}

void QueryProfiler::Render(const ProfilingNode &node, BaseTreeRenderer &ss) const {
	TextTreeRenderer renderer;
	renderer.Configure(ClientConfig::GetConfig(context).profiling_renderer_settings);
	renderer.Render(node, ss);
}

void QueryProfiler::Print() {
	// print the framed text query tree directly through the renderer's print sink
	auto renderer = CreateProfiler("query_tree");
	PrintProfilerOutput(renderer.get());
}

static bool HasScanWithoutBytesScanned(ProfilingNode &node) {
	auto &metrics = node.GetOperatorMetrics();
	if (metrics.operator_type == PhysicalOperatorType::TABLE_SCAN && !metrics.bytes_scanned_reported) {
		return true;
	}
	for (idx_t i = 0; i < node.GetChildCount(); i++) {
		if (HasScanWithoutBytesScanned(*node.GetChild(i))) {
			return true;
		}
	}
	return false;
}

static void MergeOperatorMeasurements(ProfilingNode &root, OperatorMetrics &result) {
	// merge in this layer
	result.Accumulate(root.GetOperatorMetrics());
	// recurse into children
	for (idx_t i = 0; i < root.GetChildCount(); i++) {
		auto child = root.GetChild(i);
		MergeOperatorMeasurements(*child, result);
	}
}

static double SumSubtreeTime(ProfilingNode &node) {
	auto result = node.GetOperatorMetrics().time;
	for (idx_t i = 0; i < node.GetChildCount(); i++) {
		result += SumSubtreeTime(*node.GetChild(i));
	}
	return result;
}

//! Drop the operators of a secure view, keeping only the metrics that describe the view as a whole: the time spent
//! in it (accumulated over its operators) and the rows it returned, which are observable regardless. The remaining
//! metrics of the operators inside the view are discarded rather than accumulated, because their sum describes the
//! contents of the view - how many rows it processed internally, how much of the table it scanned.
//! This runs before any profiling output is produced, so every consumer (EXPLAIN ANALYZE in any format, the
//! profiling output, the query profile result) sees the collapsed tree.
static void CollapseSecureViews(ProfilingNode &node) {
	if (node.GetOperatorMetrics().operator_type == PhysicalOperatorType::SECURE_VIEW) {
		node.GetOperatorMetrics().time = SumSubtreeTime(node);
		node.children.clear();
		return;
	}
	for (idx_t i = 0; i < node.GetChildCount(); i++) {
		CollapseSecureViews(*node.GetChild(i));
	}
}

void QueryProfiler::FinalizeMetricsInternal() {
	if (metrics_finalized || !IsEnabled() || !metrics) {
		return;
	}
	if (query_metrics.latency_timer) {
		query_metrics.latency_timer->EndTimer();
	}
	query_metrics.FinalizeMetricValues(*metrics);
	if (root) {
		// collapse secure views first - the query-wide totals are sums over the operator tree, so leaving the
		// operators of the view in would expose how much of the table behind it was scanned. The time of the view
		// is accumulated onto its boundary node, so the total CPU time still covers the whole query.
		CollapseSecureViews(*root);
		OperatorMetrics cumulative_metrics;
		MergeOperatorMeasurements(*root, cumulative_metrics);
		metrics->SetMetric<MetricQueryCPUTime>(cumulative_metrics.time);
		metrics->SetMetric<MetricQueryTotalIntermediateRows>(cumulative_metrics.elements_returned);
		metrics->SetMetric<MetricQueryTotalRowsScanned>(cumulative_metrics.rows_scanned);
		metrics->SetMetric<MetricQueryTotalIntermediateSizeBytes>(cumulative_metrics.intermediate_size_bytes);
		metrics->SetMetric<MetricQueryTotalRowGroupsScanned>(cumulative_metrics.row_groups_scanned);
		metrics->SetMetric<MetricQueryTotalRowGroupsToScan>(cumulative_metrics.total_row_groups_to_scan);
		// left out if a scan doesn't count the bytes it scans, as the total would leave that scan out
		if (!HasScanWithoutBytesScanned(*root)) {
			metrics->SetMetric<MetricQueryTotalBytesScanned>(cumulative_metrics.bytes_scanned);
		}
	}
	query_metrics.FinalizeMetrics(*metrics);
	metrics_finalized = true;
}

} // namespace duckdb
