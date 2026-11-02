#include "duckdb/common/tree_renderer.hpp"
#include "duckdb/common/tree_renderer/text_tree_renderer.hpp"
#include "duckdb/common/tree_renderer/json_tree_renderer.hpp"
#include "duckdb/common/tree_renderer/html_tree_renderer.hpp"
#include "duckdb/common/tree_renderer/graphviz_tree_renderer.hpp"
#include "duckdb/common/tree_renderer/yaml_tree_renderer.hpp"
#include "duckdb/common/tree_renderer/mermaid_tree_renderer.hpp"
#include "duckdb/common/tree_renderer/compact_tree_renderer.hpp"
#include "duckdb/main/profiler/profiler_print_format.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/query_profiler.hpp"
#include "duckdb/main/client_config.hpp"
#include "duckdb/main/profiler_extension.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Profiler output (base implementations)
//===--------------------------------------------------------------------===//
void TreeRenderer::RenderProfiler(const QueryProfiler &profiler, BaseTreeRenderer &ss) {
	// by default, render the profiling node tree using this renderer (covers HTML/GraphViz/Mermaid)
	profiler.RenderProfilingNodeTree(*this, ss);
}

unique_ptr<BaseTreeRenderer> TreeRenderer::GetPrintRenderer() {
	return make_uniq<PrinterTreeRenderer>();
}

string TreeRenderer::RenderProfilerDisabled() {
	return "Query profiling is disabled. Use 'PRAGMA enable_profiling;' to enable profiling!";
}

//===--------------------------------------------------------------------===//
// Built-in print format registry
//===--------------------------------------------------------------------===//
template <class T>
static unique_ptr<TreeRenderer> MakeRenderer() {
	return make_uniq<T>();
}

static unique_ptr<TreeRenderer> MakeNoOutputRenderer() {
	// the "no_output" format renders nothing and so has no renderer
	return nullptr;
}

struct ProfilerPrintFormatEntry {
	const char *name;
	unique_ptr<TreeRenderer> (*create_renderer)();
};

static const ProfilerPrintFormatEntry PRINT_FORMATS[] = {
    {"default", MakeRenderer<TextTreeRenderer>},
    {"text", MakeRenderer<TextTreeRenderer>},
    // aliases of "text" used by the profiler (e.g. PRAGMA enable_profiling = 'query_tree')
    {"query_tree", MakeRenderer<TextTreeRenderer>},
    {"query_tree_optimizer", MakeRenderer<TextTreeRenderer>},
    // format that renders no output at all
    {"no_output", MakeNoOutputRenderer},
    {"json", MakeRenderer<JSONTreeRenderer>},
    {"html", MakeRenderer<HTMLTreeRenderer>},
    {"graphviz", MakeRenderer<GRAPHVIZTreeRenderer>},
    {"yaml", MakeRenderer<YAMLTreeRenderer>},
    {"mermaid", MakeRenderer<MermaidTreeRenderer>},
    {"compact", MakeRenderer<CompactTreeRenderer>},
};

//! Look up the registry entry for a format name, throwing InvalidInputException (listing valid names) when unknown.
static const ProfilerPrintFormatEntry &LookupProfilerPrintFormat(const string &name) {
	auto lower = StringUtil::Lower(name);
	for (auto &entry : PRINT_FORMATS) {
		if (lower == entry.name) {
			return entry;
		}
	}
	vector<string> options;
	for (auto &entry : PRINT_FORMATS) {
		options.push_back(entry.name);
	}
	throw InvalidInputException("\"%s\" is not a valid FORMAT argument, valid options are: %s", name,
	                            StringUtil::Join(options, ", "));
}

unique_ptr<TreeRenderer> TreeRenderer::CreateRenderer(const string &name) {
	return LookupProfilerPrintFormat(name).create_renderer();
}

unique_ptr<TreeRenderer> TreeRenderer::CreateRenderer(const ProfilerPrintFormat &format) {
	return CreateRenderer(format.ToString());
}

unique_ptr<TreeRenderer> TreeRenderer::CreateRenderer(ClientContext &context, const string &name) {
	// registered renderers take precedence over the built-in formats
	auto extension = ProfilerExtension::Find(context, name);
	if (extension && extension->create_renderer) {
		return extension->create_renderer(context);
	}
	auto renderer = CreateRenderer(name);
	if (renderer) {
		renderer->Configure(ClientConfig::GetConfig(context).profiling_renderer_settings);
	}
	return renderer;
}

unique_ptr<TreeRenderer> TreeRenderer::CreateRenderer(ClientContext &context, const ProfilerPrintFormat &format) {
	return CreateRenderer(context, format.ToString());
}

} // namespace duckdb
