//===----------------------------------------------------------------------===//
//                         DuckDB
//
// main/connection_manager.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "main/connection.hpp"

namespace duckdb {

class DuckDB;

class ConnectionManager {
public:
	~ConnectionManager();

	void AddConnection(Connection *conn);
	void RemoveConnection(Connection *conn);

	template <class T> void Scan(T &&callback) {
		// lock the catalog set
		std::lock_guard<std::mutex> lock(connections_lock);
		for (auto &conn : connections) {
			callback(conn);
		}
	}

private:
	std::mutex connections_lock;
	unordered_set<Connection *> connections;
};

} // namespace duckdb
