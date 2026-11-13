//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/profiler_extension.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/main/extension_callback_manager.hpp"

#include <functional>

namespace duckdb {
struct DBConfig;
class ClientContext;
class TreeRenderer;

//! Factory that creates a TreeRenderer for a given client context; it is configured from the renderer settings after
using create_tree_renderer_t = std::function<unique_ptr<TreeRenderer>(ClientContext &context)>;

//! A ProfilerExtension registers a pluggable tree renderer for a profiler / EXPLAIN output format
class ProfilerExtension {
public:
	//! Creates the renderer for the registered format name
	create_tree_renderer_t create_renderer = nullptr;

	static void Register(DBConfig &config, const string &format_name, shared_ptr<ProfilerExtension> extension);
	static optional_ptr<ProfilerExtension> Find(const ClientContext &context, const string &format_name);
};

} // namespace duckdb
