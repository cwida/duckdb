#include "duckdb/common/render_tree.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/main/profiler/profiling_node.hpp"
#include "duckdb/execution/operator/aggregate/physical_hash_aggregate.hpp"
#include "duckdb/execution/operator/join/physical_delim_join.hpp"
#include "duckdb/execution/operator/scan/physical_positional_scan.hpp"
#include "duckdb/planner/logical_operator.hpp"

namespace duckdb {

struct PipelineRenderNode {
	explicit PipelineRenderNode(const PhysicalOperator &op) : op(op) {
	}

	const PhysicalOperator &op;
	unique_ptr<PipelineRenderNode> child;
};

} // namespace duckdb

namespace {

using duckdb::LogicalOperator;
using duckdb::LogicalOperatorType;
using duckdb::MaxValue;
using duckdb::PhysicalDelimJoin;
using duckdb::PhysicalOperator;
using duckdb::PhysicalOperatorType;
using duckdb::PhysicalPositionalScan;
using duckdb::PipelineRenderNode;
using duckdb::RenderTreeNode;

//! Secure views must not expose anything about their contents - the operators inside the view are never rendered,
//! regardless of the output format. For the profiling tree the subtree has already been folded into the boundary
//! node by the query profiler, so there is nothing left to hide there.
template <class T>
static bool HidesChildren(const T &op) {
	return false;
}

template <>
bool HidesChildren(const LogicalOperator &op) {
	return op.type == LogicalOperatorType::LOGICAL_SECURE_VIEW;
}

template <>
bool HidesChildren(const PhysicalOperator &op) {
	return op.type == PhysicalOperatorType::SECURE_VIEW;
}

template <>
bool HidesChildren(const PipelineRenderNode &op) {
	return HidesChildren(op.op);
}

class TreeChildrenIterator {
public:
	template <class T>
	static bool HasChildren(const T &op) {
		return !HidesChildren(op) && !op.children.empty();
	}
	template <class T>
	static void Iterate(const T &op, const std::function<void(const T &child)> &callback) {
		if (HidesChildren(op)) {
			return;
		}
		for (auto &child : op.children) {
			callback(*child);
		}
	}
};

template <>
bool TreeChildrenIterator::HasChildren(const PhysicalOperator &op) {
	return !HidesChildren(op) && !op.GetChildren().empty();
}
template <>
void TreeChildrenIterator::Iterate(const PhysicalOperator &op,
                                   const std::function<void(const PhysicalOperator &child)> &callback) {
	if (HidesChildren(op)) {
		return;
	}
	for (auto &child : op.GetChildren()) {
		callback(child);
	}
}

template <>
bool TreeChildrenIterator::HasChildren(const PipelineRenderNode &op) {
	return !HidesChildren(op) && op.child.get();
}

template <>
void TreeChildrenIterator::Iterate(const PipelineRenderNode &op,
                                   const std::function<void(const PipelineRenderNode &child)> &callback) {
	if (op.child && !HidesChildren(op)) {
		callback(*op.child);
	}
}

} // namespace

namespace duckdb {

template <class T>
static void GetTreeWidthHeight(const T &op, idx_t &width, idx_t &height) {
	if (!TreeChildrenIterator::HasChildren(op)) {
		width = 1;
		height = 1;
		return;
	}
	width = 0;
	height = 0;

	TreeChildrenIterator::Iterate<T>(op, [&](const T &child) {
		idx_t child_width, child_height;
		GetTreeWidthHeight<T>(child, child_width, child_height);
		width += child_width;
		height = MaxValue<idx_t>(height, child_height);
	});
	height++;
}

static unique_ptr<RenderTreeNode> CreateNode(const LogicalOperator &op) {
	return make_uniq<RenderTreeNode>(op.GetName(), op.ParamsToString());
}

static unique_ptr<RenderTreeNode> CreateNode(const PhysicalOperator &op) {
	return make_uniq<RenderTreeNode>(op.GetName(), op.ParamsToString());
}

static unique_ptr<RenderTreeNode> CreateNode(const PipelineRenderNode &op) {
	return CreateNode(op.op);
}

static unique_ptr<RenderTreeNode> CreateNode(const ProfilingNode &op) {
	auto &info = op.GetOperatorMetrics();

	auto node_name = EnumUtil::ToString(info.operator_type);
	auto result = make_uniq<RenderTreeNode>(node_name, info.GetExtraInfo());
	if (info.total_row_groups_to_scan > 0) {
		result->extra_text["Row Groups Scanned"] =
		    to_string(info.row_groups_scanned) + " / " + to_string(info.total_row_groups_to_scan);
	}
	result->extra_text[RenderTreeNode::CARDINALITY] = to_string(info.elements_returned);
	string timing = StringUtil::Format("%.2f", info.time);
	result->extra_text[RenderTreeNode::TIMING] = timing + "s";
	return result;
}

template <class T>
static idx_t CreateTreeRecursive(RenderTree &result, const T &op, idx_t x, idx_t y) {
	auto node = CreateNode(op);

	if (!TreeChildrenIterator::HasChildren(op)) {
		result.SetNode(x, y, std::move(node));
		return 1;
	}
	idx_t width = 0;
	// render the children of this node
	TreeChildrenIterator::Iterate<T>(op, [&](const T &child) {
		auto child_x = x + width;
		auto child_y = y + 1;
		node->AddChildPosition(child_x, child_y);
		width += CreateTreeRecursive<T>(result, child, child_x, child_y);
	});
	result.SetNode(x, y, std::move(node));
	return width;
}

template <class T>
static unique_ptr<RenderTree> CreateTree(const T &op) {
	idx_t width, height;
	GetTreeWidthHeight<T>(op, width, height);

	auto result = make_uniq<RenderTree>(width, height);

	// now fill in the tree
	CreateTreeRecursive<T>(*result, op, 0, 0);
	return result;
}

RenderTree::RenderTree(idx_t width_p, idx_t height_p) : width(width_p), height(height_p) {
	nodes = make_uniq_array<unique_ptr<RenderTreeNode>>((width + 1) * (height + 1));
}

optional_ptr<RenderTreeNode> RenderTree::GetNode(idx_t x, idx_t y) {
	if (x >= width || y >= height) {
		return nullptr;
	}
	return nodes[GetPosition(x, y)].get();
}

bool RenderTree::HasNode(idx_t x, idx_t y) {
	if (x >= width || y >= height) {
		return false;
	}
	return nodes[GetPosition(x, y)].get() != nullptr;
}

idx_t RenderTree::GetPosition(idx_t x, idx_t y) {
	return y * width + x;
}

void RenderTree::SetNode(idx_t x, idx_t y, unique_ptr<RenderTreeNode> node) {
	nodes[GetPosition(x, y)] = std::move(node);
}

unique_ptr<RenderTree> RenderTree::CreateRenderTree(const LogicalOperator &op) {
	return CreateTree<LogicalOperator>(op);
}

unique_ptr<RenderTree> RenderTree::CreateRenderTree(const PhysicalOperator &op) {
	return CreateTree<PhysicalOperator>(op);
}

unique_ptr<RenderTree> RenderTree::CreateRenderTree(const ProfilingNode &op) {
	return CreateTree<ProfilingNode>(op);
}

void RenderTree::SanitizeKeyNames() {
	for (idx_t i = 0; i < width * height; i++) {
		if (!nodes[i]) {
			continue;
		}
		InsertionOrderPreservingMap<string> new_map;
		for (auto &entry : nodes[i]->extra_text) {
			auto key = entry.first;
			if (StringUtil::StartsWith(key, "__")) {
				key = StringUtil::Replace(key, "__", "");
				key = StringUtil::Replace(key, "_", " ");
				key = StringUtil::Title(key);
			}
			auto &value = entry.second;
			new_map.insert(make_pair(key, value));
		}
		nodes[i]->extra_text = std::move(new_map);
	}
}

unique_ptr<RenderTree> RenderTree::CreateRenderTree(const Pipeline &pipeline) {
	auto operators = pipeline.GetOperators();
	D_ASSERT(!operators.empty());
	unique_ptr<PipelineRenderNode> node;
	for (auto &op : operators) {
		auto new_node = make_uniq<PipelineRenderNode>(op.get());
		new_node->child = std::move(node);
		node = std::move(new_node);
	}
	return CreateTree<PipelineRenderNode>(*node);
}

} // namespace duckdb
