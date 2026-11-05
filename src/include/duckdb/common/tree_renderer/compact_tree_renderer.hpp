//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/tree_renderer/compact_tree_renderer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/tree_renderer.hpp"
#include "duckdb/common/render_tree.hpp"

namespace duckdb {
class LogicalOperator;
class PhysicalOperator;
class Pipeline;

//! Renders a plan as indented text with one operator per line: the name, the cardinality/timing in parentheses and
//! the operator's properties separated by semicolons. Meant for machine consumers (e.g. AI coding agents) that read
//! the plan as text, where the box-drawing tree is expensive and the JSON/YAML formats are verbose.
class CompactTreeRenderer : public TreeRenderer {
public:
	explicit CompactTreeRenderer() {
	}
	~CompactTreeRenderer() override {
	}

public:
	string ToString(const LogicalOperator &op);
	string ToString(const PhysicalOperator &op);
	string ToString(const ProfilingNode &op);
	string ToString(const Pipeline &op);

	//! A summary line (total time, bytes read/written) before the tree - also when there is no tree, e.g. a
	//! count(*) answered from metadata
	void RenderProfiler(const QueryProfiler &profiler, BaseTreeRenderer &ss) override;

	void Render(const LogicalOperator &op, BaseTreeRenderer &ss);
	void Render(const PhysicalOperator &op, BaseTreeRenderer &ss);
	void Render(const ProfilingNode &op, BaseTreeRenderer &ss) override;
	void Render(const Pipeline &op, BaseTreeRenderer &ss);

	void ToStreamInternal(RenderTree &root, BaseTreeRenderer &ss) override;
	bool UsesRawKeyNames() override {
		return true;
	}

private:
	void RenderRecursive(RenderTree &node, BaseTreeRenderer &ss, idx_t depth, idx_t x, idx_t y);
};

} // namespace duckdb
