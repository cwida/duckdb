#include "duckdb/common/tree_renderer/html_tree_renderer.hpp"

#include "duckdb/main/query_profiler.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/common/json_document.hpp"
#include "duckdb/common/tree_renderer/html_template.hpp"
#include "duckdb/parser/parser.hpp"

#include <cstdlib>

namespace duckdb {

string HTMLTreeRenderer::ToString(const LogicalOperator &op) {
	StringTreeRenderer ss;
	Render(op, ss);
	return ss.str();
}

string HTMLTreeRenderer::ToString(const PhysicalOperator &op) {
	StringTreeRenderer ss;
	Render(op, ss);
	return ss.str();
}

string HTMLTreeRenderer::ToString(const ProfilingNode &op) {
	StringTreeRenderer ss;
	Render(op, ss);
	return ss.str();
}

string HTMLTreeRenderer::ToString(const Pipeline &op) {
	StringTreeRenderer ss;
	Render(op, ss);
	return ss.str();
}

void HTMLTreeRenderer::Render(const LogicalOperator &op, BaseTreeRenderer &ss) {
	auto tree = RenderTree::CreateRenderTree(op);
	ToStream(*tree, ss);
}

void HTMLTreeRenderer::Render(const PhysicalOperator &op, BaseTreeRenderer &ss) {
	auto tree = RenderTree::CreateRenderTree(op);
	ToStream(*tree, ss);
}

void HTMLTreeRenderer::Render(const ProfilingNode &op, BaseTreeRenderer &ss) {
	auto tree = RenderTree::CreateRenderTree(op);
	ToStream(*tree, ss);
}

void HTMLTreeRenderer::Render(const Pipeline &op, BaseTreeRenderer &ss) {
	auto tree = RenderTree::CreateRenderTree(op);
	ToStream(*tree, ss);
}

//! Map an operator name (and, for leaves, its details) to a coarse "kind" used for colour-coding in the UI. Mirrors
//! the classification used by the text renderer.
static const char *ClassifyKind(const string &name, bool is_leaf, const InsertionOrderPreservingMap<string> &extra) {
	// CTE first: CTE_SCAN contains "SCAN" but should be classified as a CTE operator
	if (StringUtil::Contains(name, "CTE")) {
		return "cte";
	}
	if (StringUtil::Contains(name, "SCAN") || StringUtil::Contains(name, "GET")) {
		return "scan";
	}
	if (StringUtil::Contains(name, "JOIN") || name == "CROSS_PRODUCT") {
		return "join";
	}
	if (StringUtil::Contains(name, "WINDOW")) {
		return "window";
	}
	if (StringUtil::Contains(name, "AGGREGATE") || StringUtil::Contains(name, "GROUP_BY") ||
	    StringUtil::Contains(name, "DISTINCT")) {
		return "aggregate";
	}
	if (StringUtil::Contains(name, "ORDER_BY") || StringUtil::Contains(name, "TOP_N")) {
		return "order";
	}
	if (StringUtil::Contains(name, "PROJECTION")) {
		return "projection";
	}
	if (StringUtil::Contains(name, "UNION")) {
		return "union";
	}
	if (StringUtil::Contains(name, "FILTER")) {
		return "filter";
	}
	if (is_leaf) {
		for (auto &entry : extra) {
			if (entry.first == "Table" || entry.first == "Function") {
				return "scan";
			}
		}
	}
	return "generic";
}

//! Prettify a leftover internal key (e.g. "__some_metric__" -> "Some Metric"). Regular keys are returned unchanged.
static string PrettifyKey(const string &key) {
	if (!StringUtil::StartsWith(key, "__")) {
		return key;
	}
	auto result = StringUtil::Replace(key, "__", "");
	result = StringUtil::Replace(result, "_", " ");
	return StringUtil::Title(result);
}

static JSONMutableValue BuildNodeJSON(JSONWriter &writer, RenderTree &tree, idx_t x, idx_t y, double &total_time,
                                      bool &has_timing) {
	auto node_p = tree.GetNode(x, y);
	D_ASSERT(node_p);
	auto &node = *node_p;

	auto obj = writer.CreateObject();
	obj.AddString("name", node.name);

	bool is_leaf = node.child_positions.empty();
	obj.AddString("kind", ClassifyKind(node.name, is_leaf, node.extra_text));

	auto details = writer.CreateArray();
	for (auto &entry : node.extra_text) {
		auto &key = entry.first;
		auto &value = entry.second;
		if (key == RenderTreeNode::CARDINALITY) {
			if (!value.empty()) {
				obj.Add("cardinality", writer.CreateSignedInteger(std::strtoll(value.c_str(), nullptr, 10)));
			}
			continue;
		}
		if (key == RenderTreeNode::ESTIMATED_CARDINALITY) {
			if (!value.empty()) {
				obj.Add("estimated_cardinality", writer.CreateSignedInteger(std::strtoll(value.c_str(), nullptr, 10)));
			}
			continue;
		}
		if (key == RenderTreeNode::TIMING) {
			double seconds = std::strtod(value.c_str(), nullptr);
			obj.Add("timing", writer.CreateDouble(seconds));
			total_time += seconds;
			has_timing = true;
			continue;
		}
		if (value.empty()) {
			continue;
		}
		auto detail = writer.CreateObject();
		detail.AddString("key", PrettifyKey(key));
		auto values = writer.CreateArray();
		for (auto &split : StringUtil::Split(value, "\n")) {
			values.AppendString(split);
		}
		detail.Add("values", values);
		details.Append(detail);
	}
	obj.Add("details", details);

	auto children = writer.CreateArray();
	for (auto &child_pos : node.child_positions) {
		children.Append(BuildNodeJSON(writer, tree, child_pos.x, child_pos.y, total_time, has_timing));
	}
	obj.Add("children", children);
	return obj;
}

void HTMLTreeRenderer::ToStreamInternal(RenderTree &root, BaseTreeRenderer &ss) {
	JSONWriter writer;
	double total_time = 0;
	bool has_timing = false;
	auto root_node = BuildNodeJSON(writer, root, 0, 0, total_time, has_timing);

	auto doc = writer.CreateObject();
	doc.Add("analyze", writer.CreateBoolean(has_timing));
	doc.Add("total_time", writer.CreateDouble(total_time));
	if (has_query_metrics) {
		auto query = writer.CreateObject();
		query.Add("real_time", writer.CreateDouble(query_real_time));
		query.Add("cpu_time", writer.CreateDouble(query_cpu_time));
		query.Add("bytes_read", writer.CreateUnsignedInteger(query_bytes_read));
		query.Add("bytes_written", writer.CreateUnsignedInteger(query_bytes_written));
		query.AddString("sql", query_sql);
		auto timings = writer.CreateArray();
		for (auto &entry : query_timings) {
			auto t = writer.CreateObject();
			t.AddString("key", entry.first);
			t.Add("seconds", writer.CreateDouble(entry.second));
			timings.Append(t);
		}
		query.Add("timings", timings);
		doc.Add("query", query);
	}
	doc.Add("root", root_node);
	writer.SetRoot(doc);

	auto json = writer.ToString(JSONWriteFlags::ALLOW_INF_AND_NAN);
	// escape angle brackets so the JSON cannot break out of the <script> tag it is embedded in
	json = StringUtil::Replace(json, "<", "\\u003c");
	json = StringUtil::Replace(json, ">", "\\u003e");

	string html(reinterpret_cast<const char *>(HTML_TEMPLATE));
	html = StringUtil::Replace(html, "__PLAN_JSON__", json);
	ss << html;
}

// HTML-escape text for safe embedding in the generated page (matches the viewer's escHtml: & < >).
static void AppendEscapedHTML(string &out, const char *data, idx_t len) {
	for (idx_t i = 0; i < len; i++) {
		char c = data[i];
		switch (c) {
		case '&':
			out += "&amp;";
			break;
		case '<':
			out += "&lt;";
			break;
		case '>':
			out += "&gt;";
			break;
		default:
			out += c;
		}
	}
}

// Render the query SQL as highlighted HTML using the DuckDB tokenizer (keyword/constant/comment spans). Highlighting is
// done here, at generation time, so the viewer does not need its own SQL highlighter.
static string HighlightSQL(const string &sql) {
	if (sql.empty()) {
		return string();
	}
	vector<SimplifiedToken> tokens;
	try {
		tokens = Parser::Tokenize(sql);
	} catch (...) {
		// tokenization can throw on invalid SQL - fall back to escaped plain text
		string fallback;
		AppendEscapedHTML(fallback, sql.c_str(), sql.size());
		return fallback;
	}
	string out;
	idx_t pos = 0;
	for (idx_t i = 0; i < tokens.size(); i++) {
		idx_t start = tokens[i].start;
		if (start > sql.size()) {
			break;
		}
		idx_t end = i + 1 == tokens.size() ? sql.size() : tokens[i + 1].start;
		if (end > sql.size()) {
			end = sql.size();
		}
		// emit any text before this token (e.g. leading whitespace) unhighlighted
		if (start > pos) {
			AppendEscapedHTML(out, sql.c_str() + pos, start - pos);
		}
		if (end <= start) {
			pos = start;
			continue;
		}
		const char *cls = nullptr;
		switch (tokens[i].type) {
		case SimplifiedTokenType::SIMPLIFIED_TOKEN_KEYWORD:
			cls = "sql-kw";
			break;
		case SimplifiedTokenType::SIMPLIFIED_TOKEN_NUMERIC_CONSTANT:
			cls = "sql-num";
			break;
		case SimplifiedTokenType::SIMPLIFIED_TOKEN_STRING_CONSTANT:
			cls = "sql-str";
			break;
		case SimplifiedTokenType::SIMPLIFIED_TOKEN_COMMENT:
			cls = "sql-comment";
			break;
		default:
			break;
		}
		if (cls) {
			out += "<span class=\"";
			out += cls;
			out += "\">";
			AppendEscapedHTML(out, sql.c_str() + start, end - start);
			out += "</span>";
		} else {
			AppendEscapedHTML(out, sql.c_str() + start, end - start);
		}
		pos = end;
	}
	if (pos < sql.size()) {
		AppendEscapedHTML(out, sql.c_str() + pos, sql.size() - pos);
	}
	return out;
}

void HTMLTreeRenderer::RenderProfiler(const QueryProfiler &profiler, BaseTreeRenderer &ss) {
	auto &qm = profiler.GetQueryMetrics();
	query_real_time = qm.GetStringMetricInSeconds("query.total_time");
	query_cpu_time = qm.GetStringMetricInSeconds("query.cpu_time");
	query_bytes_read = qm.GetBytesRead();
	query_bytes_written = qm.GetBytesWritten();
	query_sql = HighlightSQL(FormatSQL(profiler.GetQuerySQL()));
	query_timings.clear();
	for (auto &entry : qm.GetMetricTimings()) {
		query_timings.emplace_back(entry.first, static_cast<double>(entry.second.duration) / 1e9);
	}
	has_query_metrics = true;
	profiler.RenderProfilingNodeTree(*this, ss);
}

string HTMLTreeRenderer::RenderProfilerDisabled() {
	return R"(<!DOCTYPE html>
<html lang="en"><head><meta charset="UTF-8"><title>DuckDB Query Profile</title></head>
<body style="font-family: sans-serif; padding: 2rem; color: #1c2127;">
  Query profiling is disabled. Use <code>PRAGMA enable_profiling;</code> to enable profiling.
</body></html>)";
}

} // namespace duckdb
