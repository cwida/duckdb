//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/catalog_transaction.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/transaction/transaction_data.hpp"

namespace duckdb {
class Catalog;
class ClientContext;
class DatabaseInstance;
class Transaction;

struct CatalogTransaction {
	CatalogTransaction(Catalog &catalog, ClientContext &context);
	CatalogTransaction(DatabaseInstance &db, transaction_t transaction_id_p, VisibilityBound visibility_bound_p);

	DatabaseInstance &db;
	optional_ptr<ClientContext> context;
	optional_ptr<Transaction> transaction;
	SnapshotView view;

	transaction_t GetTransactionId() const {
		return view.transaction_id;
	}
	bool HasContext() const {
		return context;
	}
	ClientContext &GetContext();

	static CatalogTransaction GetSystemCatalogTransaction(ClientContext &context);
	static CatalogTransaction GetSystemTransaction(DatabaseInstance &db);
};

} // namespace duckdb
