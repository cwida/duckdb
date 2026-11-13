#include "duckdb/common/tree_renderer/graphviz_tree_renderer.hpp"

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

string GRAPHVIZTreeRenderer::ToString(const LogicalOperator &op) {
	StringTreeRenderer ss;
	Render(op, ss);
	return ss.str();
}

string GRAPHVIZTreeRenderer::ToString(const PhysicalOperator &op) {
	StringTreeRenderer ss;
	Render(op, ss);
	return ss.str();
}

string GRAPHVIZTreeRenderer::ToString(const ProfilingNode &op) {
	StringTreeRenderer ss;
	Render(op, ss);
	return ss.str();
}

string GRAPHVIZTreeRenderer::ToString(const Pipeline &op) {
	StringTreeRenderer ss;
	Render(op, ss);
	return ss.str();
}

void GRAPHVIZTreeRenderer::Render(const LogicalOperator &op, BaseTreeRenderer &ss) {
	auto tree = RenderTree::CreateRenderTree(op);
	ToStream(*tree, ss);
}

void GRAPHVIZTreeRenderer::Render(const PhysicalOperator &op, BaseTreeRenderer &ss) {
	auto tree = RenderTree::CreateRenderTree(op);
	ToStream(*tree, ss);
}

void GRAPHVIZTreeRenderer::Render(const ProfilingNode &op, BaseTreeRenderer &ss) {
	auto tree = RenderTree::CreateRenderTree(op);
	ToStream(*tree, ss);
}

void GRAPHVIZTreeRenderer::Render(const Pipeline &op, BaseTreeRenderer &ss) {
	auto tree = RenderTree::CreateRenderTree(op);
	ToStream(*tree, ss);
}

static string GraphvizNodeName(const string &prefix, idx_t x, idx_t y) {
	if (prefix.empty()) {
		return StringUtil::Format("node_%d_%d", x, y);
	}
	return StringUtil::Format("node_%s_%d_%d", prefix, x, y);
}

static void RenderGraph(RenderTree &tree, const string &prefix, vector<string> &nodes, vector<string> &edges,
                        idx_t &next_sub_plan) {
	for (idx_t y = 0; y < tree.height; y++) {
		for (idx_t x = 0; x < tree.width; x++) {
			auto node = tree.GetNode(x, y);
			if (!node) {
				continue;
			}

			vector<string> body;
			body.push_back(node->name);
			for (auto &item : node->extra_text) {
				auto value = QueryProfiler::JSONSanitize(item.second);
				body.push_back(StringUtil::Format("%s:\\n%s", item.first, value));
			}
			auto node_name = GraphvizNodeName(prefix, x, y);
			nodes.push_back(
			    StringUtil::Format("    %s [label=\"%s\"];", node_name, StringUtil::Join(body, "\\n───\\n")));

			for (auto &coord : node->child_positions) {
				edges.push_back(
				    StringUtil::Format("    %s -> %s;", node_name, GraphvizNodeName(prefix, coord.x, coord.y)));
			}
			for (auto &sub_plan : node->sub_plans) {
				if (!sub_plan.tree) {
					continue;
				}
				auto sub_plan_name = StringUtil::Format("subplan_%d", next_sub_plan++);
				vector<string> sub_plan_nodes;
				vector<string> sub_plan_edges;
				RenderGraph(*sub_plan.tree, sub_plan_name, sub_plan_nodes, sub_plan_edges, next_sub_plan);
				nodes.push_back(StringUtil::Format("    subgraph cluster_%s {\n        label=\"%s\";\n%s\n%s\n    }",
				                                   sub_plan_name, QueryProfiler::JSONSanitize(sub_plan.label),
				                                   StringUtil::Join(sub_plan_nodes, "\n"),
				                                   StringUtil::Join(sub_plan_edges, "\n")));
				edges.push_back(StringUtil::Format("    %s -> %s [style=dashed];", node_name,
				                                   GraphvizNodeName(sub_plan_name, 0, 0)));
			}
		}
	}
}

void GRAPHVIZTreeRenderer::ToStreamInternal(RenderTree &root, BaseTreeRenderer &ss) {
	const string digraph_format = R"(
digraph G {
    node [shape=box, style=rounded, fontname="Courier New", fontsize=10];
%s
%s
}
	)";

	vector<string> nodes;
	vector<string> edges;
	idx_t next_sub_plan = 0;
	RenderGraph(root, string(), nodes, edges, next_sub_plan);
	auto node_lines = StringUtil::Join(nodes, "\n");
	auto edge_lines = StringUtil::Join(edges, "\n");

	string result = StringUtil::Format(digraph_format, node_lines, edge_lines);
	ss << result;
}

string GRAPHVIZTreeRenderer::RenderProfilerDisabled() {
	return R"(
				digraph G {
				    node [shape=box, style=rounded, fontname="Courier New", fontsize=10];
				    node_0_0 [label="Query profiling is disabled. Use 'PRAGMA enable_profiling;' to enable profiling!"];
				}
			)";
}

} // namespace duckdb
