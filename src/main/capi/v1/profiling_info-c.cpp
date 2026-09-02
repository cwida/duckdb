#include "duckdb/main/capi/capi_internal.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/query_profiler.hpp"

using duckdb::Connection;
using duckdb::QueryProfileResult;
using duckdb::QueryProfileResultKind;

// Returns the total number of navigable children of a node:
// items across all LIST children (flattened) followed by direct OBJECT children.
static idx_t CountChildren(const QueryProfileResult &node) {
	idx_t count = 0;
	for (auto &child : node.children) {
		if (child->kind == QueryProfileResultKind::LIST) {
			count += child->children.size();
		} else if (child->kind == QueryProfileResultKind::OBJECT) {
			count++;
		}
	}
	return count;
}

// Returns the i-th navigable child of a node.
// LIST items come first (flattened across all LIST children), then direct OBJECT children.
static QueryProfileResult *GetChild(const QueryProfileResult &node, idx_t index) {
	// LIST items first (preserves backward-compatible indices for operator nodes)
	for (auto &child : node.children) {
		if (child->kind != QueryProfileResultKind::LIST) {
			continue;
		}
		if (index < child->children.size()) {
			return child->children[index].get();
		}
		index -= child->children.size();
	}
	// Then direct OBJECT children (e.g. metric-group sub-objects like "query", "system")
	for (auto &child : node.children) {
		if (child->kind != QueryProfileResultKind::OBJECT) {
			continue;
		}
		if (index == 0) {
			return child.get();
		}
		index--;
	}
	return nullptr;
}

duckdb_profiling_info duckdb_get_profiling_info(duckdb_connection connection) {
	if (!connection) {
		return nullptr;
	}
	auto *conn = reinterpret_cast<Connection *>(connection);
	try {
		auto &profiler = duckdb::QueryProfiler::Get(*conn->context);
		if (!profiler.IsEnabled() || !profiler.HasRoot()) {
			return nullptr;
		}
		return reinterpret_cast<duckdb_profiling_info>(&profiler.GetResult());
	} catch (...) {
		return nullptr;
	}
}

extern "C" DUCKDB_C_API duckdb_profiling_info duckdb_get_profiling_info_snapshot(duckdb_connection connection) {
	if (!connection) {
		return nullptr;
	}
	auto *conn = reinterpret_cast<Connection *>(connection);
	try {
		auto &profiler = duckdb::QueryProfiler::Get(*conn->context);
		auto snapshot = profiler.CopyResult();
		if (!snapshot) {
			return nullptr;
		}
		return reinterpret_cast<duckdb_profiling_info>(snapshot.release());
	} catch (...) {
		return nullptr;
	}
}

extern "C" DUCKDB_C_API void duckdb_destroy_profiling_info_snapshot(duckdb_profiling_info *info) {
	if (!info || !*info) {
		return;
	}
	delete reinterpret_cast<QueryProfileResult *>(*info);
	*info = nullptr;
}

duckdb_value duckdb_profiling_info_get_value(duckdb_profiling_info info, const char *key) {
	if (!info || !key) {
		return nullptr;
	}
	auto &node = *reinterpret_cast<QueryProfileResult *>(info);
	duckdb::string key_str(key);
	for (auto &child : node.children) {
		if (child->kind != QueryProfileResultKind::VALUE) {
			continue;
		}
		if (duckdb::StringUtil::CIEquals(child->key, key_str)) {
			return reinterpret_cast<duckdb_value>(new duckdb::Value(child->value));
		}
	}
	return nullptr;
}

duckdb_value duckdb_profiling_info_get_metrics(duckdb_profiling_info info) {
	if (!info) {
		return nullptr;
	}
	auto &node = *reinterpret_cast<QueryProfileResult *>(info);
	duckdb::InsertionOrderPreservingMap<duckdb::string> metrics_map;
	for (auto &child : node.children) {
		if (child->kind == QueryProfileResultKind::VALUE) {
			metrics_map.insert(child->key, child->value.ToString());
		}
	}
	auto map = duckdb::Value::MAP(metrics_map);
	return reinterpret_cast<duckdb_value>(new duckdb::Value(map));
}

idx_t duckdb_profiling_info_get_child_count(duckdb_profiling_info info) {
	if (!info) {
		return 0;
	}
	auto &node = *reinterpret_cast<QueryProfileResult *>(info);
	return CountChildren(node);
}

duckdb_profiling_info duckdb_profiling_info_get_child(duckdb_profiling_info info, idx_t index) {
	if (!info) {
		return nullptr;
	}
	auto &node = *reinterpret_cast<QueryProfileResult *>(info);
	return reinterpret_cast<duckdb_profiling_info>(GetChild(node, index));
}
