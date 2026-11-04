#include "duckdb/common/tree_renderer/compact_tree_renderer.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/main/profiler/profiling_node.hpp"
#include "duckdb/main/query_profiler.hpp"
#include "duckdb/parallel/pipeline.hpp"
#include "duckdb/planner/logical_operator.hpp"

namespace duckdb {

string CompactTreeRenderer::ToString(const LogicalOperator &op) {
	StringTreeRenderer ss;
	Render(op, ss);
	return ss.str();
}

string CompactTreeRenderer::ToString(const PhysicalOperator &op) {
	StringTreeRenderer ss;
	Render(op, ss);
	return ss.str();
}

string CompactTreeRenderer::ToString(const ProfilingNode &op) {
	StringTreeRenderer ss;
	Render(op, ss);
	return ss.str();
}

string CompactTreeRenderer::ToString(const Pipeline &op) {
	StringTreeRenderer ss;
	Render(op, ss);
	return ss.str();
}

void CompactTreeRenderer::Render(const LogicalOperator &op, BaseTreeRenderer &ss) {
	auto tree = RenderTree::CreateRenderTree(op);
	ToStream(*tree, ss);
}

void CompactTreeRenderer::Render(const PhysicalOperator &op, BaseTreeRenderer &ss) {
	auto tree = RenderTree::CreateRenderTree(op);
	ToStream(*tree, ss);
}

void CompactTreeRenderer::Render(const ProfilingNode &op, BaseTreeRenderer &ss) {
	auto tree = RenderTree::CreateRenderTree(op);
	ToStream(*tree, ss);
}

void CompactTreeRenderer::Render(const Pipeline &op, BaseTreeRenderer &ss) {
	auto tree = RenderTree::CreateRenderTree(op);
	ToStream(*tree, ss);
}

void CompactTreeRenderer::RenderProfiler(const QueryProfiler &profiler, BaseTreeRenderer &ss) {
	auto &metrics = profiler.GetQueryMetrics();
	vector<string> stats;
	stats.push_back(StringUtil::Format("time=%.4fs", metrics.GetStringMetricInSeconds("query.total_time")));
	auto bytes_read = metrics.GetBytesRead();
	if (bytes_read > 0) {
		stats.push_back("read=" + StringUtil::BytesToHumanReadableString(bytes_read, 1000));
	}
	auto bytes_written = metrics.GetBytesWritten();
	if (bytes_written > 0) {
		stats.push_back("written=" + StringUtil::BytesToHumanReadableString(bytes_written, 1000));
	}
	ss << "QUERY (" << StringUtil::Join(stats, ", ") << ")\n";
	profiler.RenderProfilingNodeTree(*this, ss);
}

void CompactTreeRenderer::ToStreamInternal(RenderTree &root, BaseTreeRenderer &ss) {
	RenderRecursive(root, ss, 0, 0, 0);
}

//! Collapse a (possibly multi-line) property value onto one line
static string CompactValue(const string &value) {
	auto lines = StringUtil::Split(value, "\n");
	vector<string> parts;
	for (auto &line : lines) {
		auto trimmed = line;
		StringUtil::Trim(trimmed);
		if (!trimmed.empty()) {
			parts.push_back(std::move(trimmed));
		}
	}
	return StringUtil::Join(parts, ", ");
}

void CompactTreeRenderer::RenderRecursive(RenderTree &tree, BaseTreeRenderer &ss, idx_t depth, idx_t x, idx_t y) {
	auto node_p = tree.GetNode(x, y);
	D_ASSERT(node_p);
	auto &node = *node_p;

	// the row counts and timing go in parentheses right after the name, the remaining properties follow
	vector<string> stats;
	vector<string> properties;
	for (auto &entry : node.extra_text) {
		auto &key = entry.first;
		auto &value = entry.second;
		if (value.empty()) {
			continue;
		}
		if (key == RenderTreeNode::CARDINALITY) {
			stats.push_back("rows=" + value);
		} else if (key == RenderTreeNode::ESTIMATED_CARDINALITY) {
			stats.push_back("est=" + value);
		} else if (key == RenderTreeNode::TIMING) {
			stats.push_back("time=" + value);
		} else {
			auto display_key = key;
			if (StringUtil::StartsWith(display_key, "__")) {
				display_key =
				    StringUtil::Title(StringUtil::Replace(StringUtil::Replace(display_key, "__", ""), "_", " "));
			}
			properties.push_back(display_key + ": " + CompactValue(value));
		}
	}

	ss << string(depth * 2, ' ') << node.name;
	if (!stats.empty()) {
		ss << " (" << StringUtil::Join(stats, ", ") << ")";
	}
	if (!properties.empty()) {
		ss << " " << StringUtil::Join(properties, "; ");
	}
	ss << '\n';

	for (auto &child_pos : node.child_positions) {
		RenderRecursive(tree, ss, depth + 1, child_pos.x, child_pos.y);
	}
}

} // namespace duckdb
