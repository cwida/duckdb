//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/extension_callback.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/main/extension_callback_manager.hpp"

namespace duckdb {
struct DBConfig;
class ClientContext;
class DatabaseInstance;
class ErrorData;

struct ParserErrorAnnotation {
	//! Appended to the native message. Empty means no additional text.
	string hint;
	//! Keys must start with "extension."; use "extension.<name>." to avoid collisions. Existing keys are preserved.
	unordered_map<string, string> details;
};

class ExtensionCallback {
public:
	virtual ~ExtensionCallback() {
	}

	//! Called when a new connection is opened
	virtual void OnConnectionOpened(ClientContext &context) {
	}
	//! Called when a connection is closed
	virtual void OnConnectionClosed(ClientContext &context) {
	}
	//! Called before an extension starts loading
	virtual void OnBeginExtensionLoad(DatabaseInstance &db, const string &name) {
	}
	//! Called after an extension is finished loading
	virtual void OnExtensionLoaded(DatabaseInstance &db, const string &name) {
	}
	//! Called after an extension fails to load loading
	virtual void OnExtensionLoadFail(DatabaseInstance &db, const string &name, const ErrorData &error) {
	}
	//! Called after removal from the connection manager, without holding its lock.
	//! Destruction waits for this callback. The context and database are still alive.
	//! Must not throw. Calls for different connections can overlap.
	virtual void OnConnectionUnregistered(ClientContext &context) {
	}
	//! Annotate a final ParseIterator parser error, before rendering. Return {} to leave it unchanged.
	//! All callbacks see the original error; annotations are appended in registration order.
	//! query is parser input, possibly whitespace-normalized with preserved byte offsets, or invalid UTF-8.
	//! The context lock is held. Read cached state only; do not execute or recursively parse SQL.
	//! Different connections may invoke the same callback concurrently. Standalone Parser calls are excluded.
	//! Callback failures discard that annotation, except fatal/internal errors, which propagate normally.
	virtual ParserErrorAnnotation OnParserError(const ClientContext &context, const string &query,
	                                            const ErrorData &error) {
		return {};
	}

	static void Register(DBConfig &config, shared_ptr<ExtensionCallback> extension);
	static ExtensionCallbackIteratorHelper<shared_ptr<ExtensionCallback>> Iterate(ClientContext &context) {
		return ExtensionCallbackManager::Get(context).ExtensionCallbacks();
	}
	static ExtensionCallbackIteratorHelper<shared_ptr<ExtensionCallback>> Iterate(DatabaseInstance &db) {
		return ExtensionCallbackManager::Get(db).ExtensionCallbacks();
	}
};

} // namespace duckdb
