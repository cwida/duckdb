//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/render_tree.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/insertion_order_preserving_map.hpp"

namespace duckdb {
class LogicalOperator;
class PhysicalOperator;
class Pipeline;
class ProfilingNode;
struct RenderTree;
struct PipelineRenderNode;

struct ExplainSubPlan {
	//! Caption shown above the sub-plan.
	string label;
	//! A render tree owned independently of the enclosing operator tree.
	unique_ptr<RenderTree> tree;
};

struct RenderTreeNode {
public:
	static constexpr const char *CARDINALITY = "__cardinality__";
	static constexpr const char *ESTIMATED_CARDINALITY = "__estimated_cardinality__";
	static constexpr const char *TIMING = "__timing__";

public:
	struct Coordinate {
	public:
		Coordinate(idx_t x, idx_t y) : x(x), y(y) {
		}

	public:
		idx_t x;
		idx_t y;
	};
	RenderTreeNode(const string &name, InsertionOrderPreservingMap<string> extra_text)
	    : name(name), extra_text(std::move(extra_text)) {
	}

public:
	void AddChildPosition(idx_t x, idx_t y) {
		child_positions.emplace_back(x, y);
	}

public:
	string name;
	InsertionOrderPreservingMap<string> extra_text;
	vector<Coordinate> child_positions;
	vector<ExplainSubPlan> sub_plans;
};

struct RenderTree {
	RenderTree(idx_t width, idx_t height);

	unique_array<unique_ptr<RenderTreeNode>> nodes;
	idx_t width;
	idx_t height;

public:
	static unique_ptr<RenderTree> CreateRenderTree(const LogicalOperator &op);
	static unique_ptr<RenderTree> CreateRenderTree(const PhysicalOperator &op);
	static unique_ptr<RenderTree> CreateRenderTree(const ProfilingNode &op);
	static unique_ptr<RenderTree> CreateRenderTree(const Pipeline &op);

public:
	optional_ptr<RenderTreeNode> GetNode(idx_t x, idx_t y);
	void SetNode(idx_t x, idx_t y, unique_ptr<RenderTreeNode> node);
	bool HasNode(idx_t x, idx_t y);
	void SanitizeKeyNames();

private:
	idx_t GetPosition(idx_t x, idx_t y);
};

} // namespace duckdb
