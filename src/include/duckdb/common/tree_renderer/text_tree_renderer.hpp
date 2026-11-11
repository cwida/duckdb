//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/tree_renderer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/constants.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/tree_renderer.hpp"
#include "duckdb/common/render_tree.hpp"

namespace duckdb {
class LogicalOperator;
class PhysicalOperator;
class Pipeline;
struct PipelineRenderNode;

struct TextTreeRendererConfig {
	idx_t maximum_render_width = 240;
	idx_t node_render_width = 29;
	idx_t minimum_render_width = 15;
	idx_t max_extra_lines = 30;

	// Formatting options
	char thousand_separator = ',';
	char decimal_separator = '.';

	//! When set, render every operator as a full box (disable the timing-based folding/merging of operators).
	//! Defaults to the full plan; only the interactive CLI opts into folding (where ".last" can re-expand it).
	bool expand_all = true;
	//! When set, operator names are rendered raw/upper-case (e.g. HASH_JOIN) instead of title-cased (Hash Join)
	bool upper_case_operators = false;

#ifndef DUCKDB_ASCII_TREE_RENDERER
	const char *LTCORNER = "\342\224\214"; // NOLINT "┌";
	const char *RTCORNER = "\342\224\220"; // NOLINT "┐";
	const char *LDCORNER = "\342\224\224"; // NOLINT "└";
	const char *RDCORNER = "\342\224\230"; // NOLINT "┘";

	const char *MIDDLE = "\342\224\274";  // NOLINT "┼";
	const char *TMIDDLE = "\342\224\254"; // NOLINT "┬";
	const char *LMIDDLE = "\342\224\234"; // NOLINT "├";
	const char *RMIDDLE = "\342\224\244"; // NOLINT "┤";
	const char *DMIDDLE = "\342\224\264"; // NOLINT "┴";

	const char *VERTICAL = "\342\224\202";   // NOLINT "│";
	const char *HORIZONTAL = "\342\224\200"; // NOLINT "─";
#else
	// ASCII version
	const char *LTCORNER = "<";
	const char *RTCORNER = ">";
	const char *LDCORNER = "<";
	const char *RDCORNER = ">";

	const char *MIDDLE = "+";
	const char *TMIDDLE = "+";
	const char *LMIDDLE = "+";
	const char *RMIDDLE = "+";
	const char *DMIDDLE = "+";

	const char *VERTICAL = "|";
	const char *HORIZONTAL = "-";
#endif
};

class TextTreeRenderer : public TreeRenderer {
public:
	explicit TextTreeRenderer(TextTreeRendererConfig config_p = TextTreeRendererConfig()) : config(config_p) {
	}
	~TextTreeRenderer() override {
	}

public:
	string ToString(const LogicalOperator &op);
	string ToString(const PhysicalOperator &op);
	string ToString(const ProfilingNode &op);
	string ToString(const Pipeline &op);

	void Render(const LogicalOperator &op, BaseTreeRenderer &ss);
	void Render(const PhysicalOperator &op, BaseTreeRenderer &ss);
	void Render(const ProfilingNode &op, BaseTreeRenderer &ss) override;
	void Render(const Pipeline &op, BaseTreeRenderer &ss);

	void ToStreamInternal(RenderTree &root, BaseTreeRenderer &ss) override;

	//! Profiler text output: the framed query tree (with phase timings, total time, etc.)
	void RenderProfiler(const QueryProfiler &profiler, BaseTreeRenderer &ss) override;

	void Configure(const unordered_map<string, Value> &settings) override;

	bool UsesRawKeyNames() override {
		return true;
	}

private:
	//! The configuration used for rendering
	TextTreeRendererConfig config;
};

} // namespace duckdb
