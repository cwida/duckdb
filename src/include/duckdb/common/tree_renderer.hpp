//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/tree_renderer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/constants.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/render_tree.hpp"
#include "duckdb/common/tree_renderer/base_tree_renderer.hpp"

namespace duckdb {

class ClientContext;
class QueryProfiler;
class ProfilingNode;
struct ProfilerPrintFormat;

//! TreeRenderer renders a plan/operator tree (for EXPLAIN) or a query profiler's output in a particular format.
//! It is the single renderer abstraction: each format (text, JSON, HTML, GraphViz, Mermaid, YAML) is one subclass
//! that handles both EXPLAIN rendering and profiler output. New formats are added in CreateRenderer.
class TreeRenderer {
public:
	explicit TreeRenderer() {
	}
	virtual ~TreeRenderer() {
	}

public:
	//! Render the tree into a BaseTreeRenderer (e.g. a StringTreeRenderer, or a highlighting-aware sink)
	void ToStream(RenderTree &root, BaseTreeRenderer &ss);
	virtual void ToStreamInternal(RenderTree &root, BaseTreeRenderer &ss) = 0;

	//! Called once after this renderer has produced its output (end of ToStream, and after RenderProfiler). Lets a
	//! renderer perform a one-shot side effect for the render (e.g. open the rendered result in a browser).
	virtual void Finish() {
	}

	//! Whether this format produces a single standalone artifact (e.g. one HTML page opened in a browser). For such
	//! formats EXPLAIN renders only the final plan instead of every stage, so a single artifact is produced.
	virtual bool RendersSinglePlan() {
		return false;
	}

	//! Returns the sink to render into when printing this format's output directly. Only invoked when we are about
	//! to print (the default renderer writes straight to the output stream), so it is never created for the
	//! string-producing paths. Formats can override this to provide a highlighting-aware sink.
	virtual unique_ptr<BaseTreeRenderer> GetPrintRenderer();
	//! Create a renderer for the given format, consulting the pluggable registry and configuring built-ins from the
	//! client's "profiling_renderer_settings". Matched case-insensitively; throws if unknown, nullptr for "no_output".
	static unique_ptr<TreeRenderer> CreateRenderer(ClientContext &context, const string &name);
	static unique_ptr<TreeRenderer> CreateRenderer(ClientContext &context, const ProfilerPrintFormat &format);

	//! Create a built-in renderer without configuring it or consulting the registry (no ClientContext available)
	static unique_ptr<TreeRenderer> CreateRenderer(const string &name);
	static unique_ptr<TreeRenderer> CreateRenderer(const ProfilerPrintFormat &format);

	//! Generic configuration of the renderer: passes renderer settings (e.g. from the "profiling_renderer_settings"
	//! setting) to the renderer. Renderers override this to handle the settings they support, and throw on invalid
	//! setting values. Unrecognized settings are ignored - they may be intended for a different renderer.
	virtual void Configure(const unordered_map<string, Value> &settings) {
	}

	virtual bool UsesRawKeyNames() {
		return false;
	}
	virtual void Render(const ProfilingNode &op, BaseTreeRenderer &ss) {
	}

	//! Render the profiler's output into the given sink. Only called when profiling is enabled. The base
	//! implementation renders the profiling node tree; formats with richer output (text, JSON) override this.
	virtual void RenderProfiler(const QueryProfiler &profiler, BaseTreeRenderer &ss);
	//! The message shown (in this format) when profiling is disabled.
	virtual string RenderProfilerDisabled();
};

} // namespace duckdb
