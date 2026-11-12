//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/valid_checker.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/constants.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/mutex.hpp"

namespace duckdb {
class AttachedDatabase;
class ErrorData;
class DatabaseInstance;
class MetaTransaction;

class ValidChecker {
public:
	explicit ValidChecker(DatabaseInstance &db);

	DUCKDB_API static ValidChecker &Get(DatabaseInstance &db);
	DUCKDB_API static ValidChecker &Get(AttachedDatabase &db);
	DUCKDB_API static ValidChecker &Get(MetaTransaction &transaction);

	DUCKDB_API void Invalidate(string error);
	//! Invalidate unless already invalidated, keeping the original error
	DUCKDB_API void InvalidateIfValid(string error);
	DUCKDB_API bool IsInvalidated();
	DUCKDB_API string InvalidatedMessage();

	template <class T>
	static bool IsInvalidated(T &o) {
		return Get(o).IsInvalidated();
	}
	//! Invalidate the instance after a fatal error, unless an attached database already handled it (ScopeFatalError)
	DUCKDB_API static void Invalidate(DatabaseInstance &db, const ErrorData &error);
	template <class T>
	static void Invalidate(T &o, string error) {
		Get(o).Invalidate(std::move(error));
	}

	template <class T>
	static string InvalidatedMessage(T &o) {
		return Get(o).InvalidatedMessage();
	}

private:
	mutex invalidate_lock;
	//! Set to true when encountering a fatal exception.
	atomic<bool> is_invalidated;
	//! The message invalidating the database instance.
	string invalidated_msg;
	//! The database instance.
	DatabaseInstance &db;
};

} // namespace duckdb
