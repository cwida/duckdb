//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/extension_callback.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/main/extension_callback_manager.hpp"

namespace duckdb {
struct DBConfig;
class ClientContext;
class DatabaseInstance;
class ErrorData;
class SQLStatement;

//! The client operation, not the SQL statement type. Generated PREPARE/EXECUTE wrappers use SUBMISSION.
enum class StatementCheckType : uint8_t { SUBMISSION, PREPARATION, INSPECTION };

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
	//! Throw to reject a parsed statement before preprocessing or connected routing, and reached children before
	//! planning. Covers SQL submission, explicit preparation, BindStatement, GetTableNames and ExtractPlan.
	//! Parsing alone and relation construction do not invoke this callback. Checks may repeat, including generated
	//! PREPARE/EXECUTE wrappers. Extensions must inspect nested
	//! ASTs themselves; retained prepared targets remain the responsibility of
	//! ClientContextState::OnRebindPreparedStatement. Called with the connection lock held, without assuming a
	//! transaction. Must be idempotent and not execute SQL. Different connections can invoke the same callback
	//! concurrently.
	virtual void OnStatementCheck(ClientContext &context, const SQLStatement &statement, StatementCheckType type) {
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
