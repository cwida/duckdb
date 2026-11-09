#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/main/context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/transaction/transaction.hpp"

namespace duckdb {

CatalogTransaction::CatalogTransaction(Catalog &catalog, ClientContext &context)
    : db(DatabaseInstance::GetDatabase(context)), context(&context), transaction(&Transaction::Get(context, catalog)),
      view(transaction->GetSnapshotView()) {
}

CatalogTransaction::CatalogTransaction(DatabaseInstance &db, transaction_t transaction_id_p,
                                       VisibilityBound visibility_bound_p)
    : db(db), context(nullptr), transaction(nullptr), view(transaction_id_p, visibility_bound_p) {
}

ClientContext &CatalogTransaction::GetContext() {
	if (!context) {
		throw InternalException("Attempting to get a context in a CatalogTransaction without a context");
	}
	return *context;
}

CatalogTransaction CatalogTransaction::GetSystemCatalogTransaction(ClientContext &context) {
	return CatalogTransaction(Catalog::GetSystemCatalog(context), context);
}

CatalogTransaction CatalogTransaction::GetSystemCatalogTransaction(Context &context) {
	auto client_context = context.TryGetClientContext();
	if (client_context) {
		return GetSystemCatalogTransaction(*client_context);
	}
	return GetSystemTransaction(context.GetDatabase());
}

CatalogTransaction CatalogTransaction::GetSystemTransaction(DatabaseInstance &db) {
	return CatalogTransaction(db, SYSTEM_TRANSACTION_TIMESTAMP, VisibilityBound::Through(SYSTEM_TRANSACTION_TIMESTAMP));
}

} // namespace duckdb
