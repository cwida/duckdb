//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/client_context_state.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/atomic.hpp"
#include "duckdb/common/enums/prepared_statement_mode.hpp"
#include "duckdb/common/exception/transaction_exception.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/main/valid_checker.hpp"
#include "duckdb/planner/expression/bound_parameter_data.hpp"
#include <mutex>

namespace duckdb {
class ClientContext;
class ErrorData;
class MetaTransaction;
class PreparedStatementData;
class SQLStatement;
class RegisteredStateManager;

enum class RebindQueryInfo { DO_NOT_REBIND, ATTEMPT_TO_REBIND };

struct BindPreparedStatementCallbackInfo {
	PreparedStatementData &prepared_statement;
	optional_ptr<identifier_map_t<BoundParameterData>> parameters;
};

//! ClientContextState is virtual base class for ClientContext-local (or Query-Local, using QueryEnd callback) state
//! e.g. caches that need to live as long as a ClientContext or Query.
class ClientContextState {
public:
	virtual ~ClientContextState() = default;
	virtual void QueryBegin(ClientContext &context) {
	}
	virtual void QueryEnd() {
	}
	virtual void QueryEnd(ClientContext &context) {
		QueryEnd();
	}
	virtual void QueryEnd(ClientContext &context, optional_ptr<ErrorData> error) {
		QueryEnd(context);
	}
	virtual void TransactionBegin(MetaTransaction &transaction, ClientContext &context) {
	}
	virtual void TransactionCommit(MetaTransaction &transaction, ClientContext &context) {
	}
	virtual void TransactionRollback(MetaTransaction &transaction, ClientContext &context) {
	}
	virtual void TransactionRollback(MetaTransaction &transaction, ClientContext &context,
	                                 optional_ptr<ErrorData> error) {
		TransactionRollback(transaction, context);
	}
	virtual bool CanRequestRebind() {
		return false;
	}
	virtual RebindQueryInfo OnPlanningError(ClientContext &context, SQLStatement &statement, ErrorData &error) {
		return RebindQueryInfo::DO_NOT_REBIND;
	}
	virtual RebindQueryInfo OnFinalizePrepare(ClientContext &context, PreparedStatementData &prepared_statement,
	                                          PreparedStatementMode mode) {
		return RebindQueryInfo::DO_NOT_REBIND;
	}
	virtual RebindQueryInfo OnRebindPreparedStatement(ClientContext &context, BindPreparedStatementCallbackInfo &info,
	                                                  RebindQueryInfo current_rebind) {
		return RebindQueryInfo::DO_NOT_REBIND;
	}
	virtual void WriteProfilingInformation(std::ostream &ss) {
	}
	virtual void OnTaskStart(ClientContext &context) {
	}
	virtual void OnTaskStop(ClientContext &context) {
	}

public:
	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}
	template <class TARGET>
	const TARGET &Cast() const {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<const TARGET &>(*this);
	}
};

class RegisteredStateManager {
public:
	template <class T, typename... ARGS>
	shared_ptr<T> GetOrCreate(const string &key, ARGS &&... args) {
		lock_guard<mutex> l(lock);
		auto lookup = registered_state.find(key);
		if (lookup != registered_state.end()) {
			return shared_ptr_cast<ClientContextState, T>(lookup->second);
		}
		auto cache = make_shared_ptr<T>(std::forward<ARGS>(args)...);
		registered_state[key] = cache;
		InvalidateSnapshot();
		return cache;
	}

	template <class T>
	shared_ptr<T> Get(const string &key) {
		lock_guard<mutex> l(lock);
		auto lookup = registered_state.find(key);
		if (lookup == registered_state.end()) {
			return nullptr;
		}
		return shared_ptr_cast<ClientContextState, T>(lookup->second);
	}

	void Insert(const string &key, shared_ptr<ClientContextState> state_p) {
		lock_guard<mutex> l(lock);
		if (registered_state.insert(make_pair(key, std::move(state_p))).second) {
			InvalidateSnapshot();
		}
	}

	void Remove(const string &key) {
		lock_guard<mutex> l(lock);
		if (registered_state.erase(key)) {
			InvalidateSnapshot();
		}
	}

	vector<shared_ptr<ClientContextState>> States() {
		lock_guard<mutex> l(lock);
		return StatesInternal();
	}

	//! Invokes callbacks outside the registry lock, retaining snapshot ownership throughout dispatch.
	template <class STATE_CALLBACK>
	void ForEachState(STATE_CALLBACK &&callback) {
		auto states = StatesSnapshot();
		if (!states) {
			return;
		}
		for (auto &state : *states) {
			callback(*state);
		}
	}

private:
	shared_ptr<const vector<shared_ptr<ClientContextState>>> StatesSnapshot() {
		if (!has_states.load(std::memory_order_acquire)) {
			return nullptr;
		}
		auto snapshot = state_snapshot.atomic_load();
		if (snapshot) {
			return snapshot;
		}
		lock_guard<mutex> l(lock);
		snapshot = state_snapshot;
		if (!snapshot && !registered_state.empty()) {
			snapshot = make_shared_ptr<vector<shared_ptr<ClientContextState>>>(StatesInternal());
			state_snapshot.atomic_store(snapshot);
		}
		return snapshot;
	}

	vector<shared_ptr<ClientContextState>> StatesInternal() {
		vector<shared_ptr<ClientContextState>> states;
		states.reserve(registered_state.size());
		for (auto &entry : registered_state) {
			states.push_back(entry.second);
		}
		return states;
	}

	//! Called with the registry lock held after a successful mutation.
	void InvalidateSnapshot() {
		state_snapshot.atomic_store(nullptr);
		has_states.store(!registered_state.empty(), std::memory_order_release);
	}

private:
	mutex lock;
	unordered_map<string, shared_ptr<ClientContextState>> registered_state;
	shared_ptr<const vector<shared_ptr<ClientContextState>>> state_snapshot;
	//! Avoid atomic shared_ptr loads for empty registries.
	atomic<bool> has_states {false};
};

} // namespace duckdb
