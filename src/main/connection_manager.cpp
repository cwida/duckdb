#include "duckdb/main/connection_manager.hpp"
#include "duckdb/common/exception/transaction_exception.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/planner/extension_callback.hpp"

namespace duckdb {

ConnectionManager::ConnectionManager() : connection_count(0), current_connection_id(0) {
}

void ConnectionManager::AddConnection(ClientContext &context) {
	lock_guard<mutex> lock(connections_lock);
	for (auto &callback : ExtensionCallback::Iterate(context)) {
		callback->OnConnectionOpened(context);
	}
	connections[context] = weak_ptr<ClientContext>(context.shared_from_this());
	connection_count = connections.size();
}

void ConnectionManager::RemoveConnection(ClientContext &context) {
	unique_lock<mutex> lock(connections_lock);
	auto callbacks = ExtensionCallback::Iterate(context);
	for (auto &callback : callbacks) {
		callback->OnConnectionClosed(context);
	}
	connections.erase(context);
	connection_count = connections.size();
	lock.unlock();
	for (auto &callback : callbacks) {
		callback->OnConnectionUnregistered(context);
	}
}

idx_t ConnectionManager::GetConnectionCount() const {
	return connection_count;
}

void ConnectionManager::AssignConnectionId(Connection &connection) {
	connection.context->connection_id = current_connection_id.fetch_add(1, std::memory_order_relaxed) + 1;
}

vector<shared_ptr<ClientContext>> ConnectionManager::GetConnectionList() {
	lock_guard<mutex> lock(connections_lock);
	vector<shared_ptr<ClientContext>> result;
	for (auto it = connections.begin(); it != connections.end();) {
		auto connection = it->second.lock();
		if (!connection) {
			it = connections.erase(it);
		} else {
			result.push_back(std::move(connection));
			++it;
		}
	}
	connection_count = connections.size();
	return result;
}

} // namespace duckdb
