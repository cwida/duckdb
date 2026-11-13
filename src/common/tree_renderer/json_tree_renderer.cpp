#include "duckdb/common/tree_renderer/json_tree_renderer.hpp"

#include "duckdb/common/pair.hpp"
#include "duckdb/main/query_profiler.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/execution/operator/aggregate/physical_hash_aggregate.hpp"
#include "duckdb/execution/operator/join/physical_delim_join.hpp"
#include "duckdb/execution/operator/scan/physical_positional_scan.hpp"
#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/parallel/pipeline.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "utf8proc_wrapper.hpp"

#include "duckdb/common/json_document.hpp"

#include <sstream>

namespace duckdb {

string JSONTreeRenderer::ToString(const LogicalOperator &op) {
	StringTreeRenderer ss;
	Render(op, ss);
	return ss.str();
}

string JSONTreeRenderer::ToString(const PhysicalOperator &op) {
	StringTreeRenderer ss;
	Render(op, ss);
	return ss.str();
}

string JSONTreeRenderer::ToString(const ProfilingNode &op) {
	StringTreeRenderer ss;
	Render(op, ss);
	return ss.str();
}

string JSONTreeRenderer::ToString(const Pipeline &op) {
	StringTreeRenderer ss;
	Render(op, ss);
	return ss.str();
}

void JSONTreeRenderer::Render(const LogicalOperator &op, BaseTreeRenderer &ss) {
	auto tree = RenderTree::CreateRenderTree(op);
	ToStream(*tree, ss);
}

void JSONTreeRenderer::Render(const PhysicalOperator &op, BaseTreeRenderer &ss) {
	auto tree = RenderTree::CreateRenderTree(op);
	ToStream(*tree, ss);
}

void JSONTreeRenderer::Render(const ProfilingNode &op, BaseTreeRenderer &ss) {
	auto tree = RenderTree::CreateRenderTree(op);
	ToStream(*tree, ss);
}

void JSONTreeRenderer::Render(const Pipeline &op, BaseTreeRenderer &ss) {
	auto tree = RenderTree::CreateRenderTree(op);
	ToStream(*tree, ss);
}

static JSONMutableValue RenderRecursive(JSONWriter &writer, RenderTree &tree, idx_t x, idx_t y) {
	auto node_p = tree.GetNode(x, y);
	D_ASSERT(node_p);
	auto &node = *node_p;

	auto object = writer.CreateObject();
	auto children = writer.CreateArray();
	for (auto &child_pos : node.child_positions) {
		children.Append(RenderRecursive(writer, tree, child_pos.x, child_pos.y));
	}
	object.AddString("name", node.name);
	object.Add("children", children);
	auto extra_info = writer.CreateObject();
	for (auto &it : node.extra_text) {
		auto &key = it.first;
		auto &value = it.second;
		auto splits = StringUtil::Split(value, "\n");
		if (splits.size() > 1) {
			auto list_items = writer.CreateArray();
			for (auto &split : splits) {
				list_items.AppendString(split);
			}
			extra_info.Add(key, list_items);
		} else {
			extra_info.AddString(key, value);
		}
	}
	object.Add("extra_info", extra_info);
	auto sub_plans = writer.CreateArray();
	bool has_sub_plans = false;
	for (auto &sub_plan : node.sub_plans) {
		if (!sub_plan.tree) {
			continue;
		}
		auto sub_plan_object = writer.CreateObject();
		sub_plan_object.AddString("label", sub_plan.label);
		sub_plan_object.Add("tree", RenderRecursive(writer, *sub_plan.tree, 0, 0));
		sub_plans.Append(sub_plan_object);
		has_sub_plans = true;
	}
	if (has_sub_plans) {
		object.Add("sub_plans", sub_plans);
	}
	return object;
}

void JSONTreeRenderer::ToStreamInternal(RenderTree &root, BaseTreeRenderer &ss) {
	JSONWriter writer;
	auto result_obj = writer.CreateArray();
	result_obj.Append(RenderRecursive(writer, root, 0, 0));
	writer.SetRoot(result_obj);
	ss << writer.ToString(JSONWriteFlags::ALLOW_INF_AND_NAN | JSONWriteFlags::PRETTY);
}

void JSONTreeRenderer::RenderProfiler(const QueryProfiler &profiler, BaseTreeRenderer &ss) {
	// the JSON profiler output is the full query profile result tree (including query-level metrics)
	ss << profiler.ToJSON();
}

string JSONTreeRenderer::RenderProfilerDisabled() {
	return R"({
    "result": "disabled"
})";
}

} // namespace duckdb
