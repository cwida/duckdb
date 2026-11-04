//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/transaction/transaction_data.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/optional_ptr.hpp"

namespace duckdb {
class DuckTransaction;
class Transaction;

inline bool IsCommitted(transaction_t timestamp) {
	return timestamp <= MAX_COMMIT_ID;
}

//! What a transaction sees: everything before its exclusive bound, plus its own writes.
struct SnapshotView {
	SnapshotView(transaction_t transaction_id_p, VisibilityBound visibility_bound_p)
	    : transaction_id(transaction_id_p), visibility_bound(visibility_bound_p) {
	}

	//! The view of what was committed before the bound, without any uncommitted changes
	static SnapshotView CommittedBefore(VisibilityBound visibility_bound) {
		return SnapshotView(MAX_TRANSACTION_ID, visibility_bound);
	}

	//! The reading transaction, so that its own writes are visible to it
	transaction_t transaction_id;
	//! Exclusive: timestamps below it are visible
	VisibilityBound visibility_bound;

	//! Below the bound, or written by this transaction
	bool Sees(transaction_t timestamp) const {
		return timestamp < visibility_bound || timestamp == transaction_id;
	}
};

struct TransactionData {
	TransactionData(DuckTransaction &transaction_p); // NOLINT: allow implicit conversion
	TransactionData(transaction_t transaction_id_p, VisibilityBound visibility_bound_p);

	optional_ptr<DuckTransaction> transaction;
	SnapshotView view;

	transaction_t GetTransactionId() const {
		return view.transaction_id;
	}

	//! For writes into a collection private to one transaction
	static TransactionData Unversioned() {
		return TransactionData(0, VisibilityBound::Before(0));
	}
};

} // namespace duckdb
