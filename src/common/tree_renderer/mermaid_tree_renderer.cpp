#include "duckdb/common/tree_renderer/mermaid_tree_renderer.hpp"

#include "duckdb/common/pair.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/execution/operator/aggregate/physical_hash_aggregate.hpp"
#include "duckdb/execution/operator/join/physical_delim_join.hpp"
#include "duckdb/execution/operator/scan/physical_positional_scan.hpp"
#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/parallel/pipeline.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/main/query_profiler.hpp"
#include "utf8proc_wrapper.hpp"

#include <sstream>

namespace duckdb {

string MermaidTreeRenderer::ToString(const LogicalOperator &op) {
	StringTreeRenderer ss;
	Render(op, ss);
	return ss.str();
}

string MermaidTreeRenderer::ToString(const PhysicalOperator &op) {
	StringTreeRenderer ss;
	Render(op, ss);
	return ss.str();
}

string MermaidTreeRenderer::ToString(const ProfilingNode &op) {
	StringTreeRenderer ss;
	Render(op, ss);
	return ss.str();
}

string MermaidTreeRenderer::ToString(const Pipeline &op) {
	StringTreeRenderer ss;
	Render(op, ss);
	return ss.str();
}

void MermaidTreeRenderer::Render(const LogicalOperator &op, BaseTreeRenderer &ss) {
	auto tree = RenderTree::CreateRenderTree(op);
	ToStream(*tree, ss);
}

void MermaidTreeRenderer::Render(const PhysicalOperator &op, BaseTreeRenderer &ss) {
	auto tree = RenderTree::CreateRenderTree(op);
	ToStream(*tree, ss);
}

void MermaidTreeRenderer::Render(const ProfilingNode &op, BaseTreeRenderer &ss) {
	auto tree = RenderTree::CreateRenderTree(op);
	ToStream(*tree, ss);
}

void MermaidTreeRenderer::Render(const Pipeline &op, BaseTreeRenderer &ss) {
	auto tree = RenderTree::CreateRenderTree(op);
	ToStream(*tree, ss);
}

static string SanitizeMermaidLabel(const string &text) {
	string result;
	result.reserve(text.size() * 2); // Reserve more space for potential escape sequences
	for (size_t i = 0; i < text.size(); i++) {
		char c = text[i];
		// Escape backticks and quotes
		if (c == '`') {
			result += "\\`";
		} else if (c == '"') {
			result += "\\\"";
		} else if (c == '\\' && i + 1 < text.size() && text[i + 1] == 'n') {
			// Replace literal "\n" with actual newline for Mermaid markdown
			result += "\n\t";
			i++; // Skip the 'n'
		} else {
			result += c;
		}
	}
	return result;
}

static string MermaidNodeName(const string &prefix, idx_t x, idx_t y) {
	if (prefix.empty()) {
		return StringUtil::Format("node_%d_%d", x, y);
	}
	return StringUtil::Format("node_%s_%d_%d", prefix, x, y);
}

static void RenderMermaidGraph(RenderTree &tree, const string &prefix, vector<string> &nodes, vector<string> &edges,
                               idx_t &next_sub_plan) {
	for (idx_t y = 0; y < tree.height; y++) {
		for (idx_t x = 0; x < tree.width; x++) {
			auto node = tree.GetNode(x, y);
			if (!node) {
				continue;
			}

			string extra_info;
			for (auto &item : node->extra_text) {
				auto value = QueryProfiler::JSONSanitize(item.second);
				extra_info += StringUtil::Format("\n\t%s: %s", item.first, SanitizeMermaidLabel(value));
			}
			auto trimmed_name = node->name;
			StringUtil::Trim(trimmed_name);
			auto node_name = MermaidNodeName(prefix, x, y);
			nodes.push_back(StringUtil::Format("    %s[\"`**%s**%s`\"]", node_name, SanitizeMermaidLabel(trimmed_name),
			                                   extra_info));

			for (auto &coord : node->child_positions) {
				edges.push_back(
				    StringUtil::Format("    %s --> %s", node_name, MermaidNodeName(prefix, coord.x, coord.y)));
			}
			for (auto &sub_plan : node->sub_plans) {
				if (!sub_plan.tree) {
					continue;
				}
				auto sub_plan_name = StringUtil::Format("subplan_%d", next_sub_plan++);
				vector<string> sub_plan_nodes;
				vector<string> sub_plan_edges;
				RenderMermaidGraph(*sub_plan.tree, sub_plan_name, sub_plan_nodes, sub_plan_edges, next_sub_plan);
				nodes.push_back(StringUtil::Format(
				    "    subgraph %s [\"%s\"]\n%s\n%s\n    end", sub_plan_name, SanitizeMermaidLabel(sub_plan.label),
				    StringUtil::Join(sub_plan_nodes, "\n\n"), StringUtil::Join(sub_plan_edges, "\n")));
				edges.push_back(StringUtil::Format("    %s -.-> %s", node_name, MermaidNodeName(sub_plan_name, 0, 0)));
			}
		}
	}
}

void MermaidTreeRenderer::ToStreamInternal(RenderTree &root, BaseTreeRenderer &ss) {
	vector<string> nodes;
	vector<string> edges;
	idx_t next_sub_plan = 0;
	RenderMermaidGraph(root, string(), nodes, edges, next_sub_plan);

	// Output Mermaid flowchart
	ss << "flowchart TD\n";

	// Output nodes
	for (auto &node : nodes) {
		ss << node << "\n\n";
	}

	// Output edges
	for (auto &edge : edges) {
		ss << edge << "\n";
	}
}

string MermaidTreeRenderer::RenderProfilerDisabled() {
	return R"(flowchart TD
    node_0_0["`**DISABLED**
Query profiling is disabled.
Use 'PRAGMA enable_profiling;' to enable profiling!`"]
)";
}

} // namespace duckdb
