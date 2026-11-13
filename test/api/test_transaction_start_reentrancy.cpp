#include "catch.hpp"
#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/common/thread.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"
#include "duckdb/transaction/meta_transaction.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

// A transaction manager that performs a secret lookup through the system catalog while starting a transaction.
// This mirrors an extension that issues an HTTP request (through httpfs) from its StartTransaction override.
class SecretLookupTransactionManager : public DuckTransactionManager {
public:
	explicit SecretLookupTransactionManager(AttachedDatabase &db) : DuckTransactionManager(db) {
	}

	Transaction &StartTransaction(ClientContext &context) override {
		auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
		auto match = SecretManager::Get(context).LookupSecret(transaction, "https://example.com/file.csv", "http");
		lookup_count++;
		if (match.HasMatch()) {
			matched_count++;
		}
		return DuckTransactionManager::StartTransaction(context);
	}

	atomic<idx_t> lookup_count {0};
	atomic<idx_t> matched_count {0};
};

// A transaction manager that requests a transaction for its own database while starting one.
class SelfRequestingTransactionManager : public DuckTransactionManager {
public:
	explicit SelfRequestingTransactionManager(AttachedDatabase &db) : DuckTransactionManager(db) {
	}

	Transaction &StartTransaction(ClientContext &context) override {
		return Transaction::Get(context, db);
	}
};

// A transaction manager that takes a while to start a transaction, so that other threads can race it.
class SlowTransactionManager : public DuckTransactionManager {
public:
	explicit SlowTransactionManager(AttachedDatabase &db) : DuckTransactionManager(db) {
	}

	Transaction &StartTransaction(ClientContext &context) override {
		start_count++;
		ThreadUtil::SleepMs(50);
		return DuckTransactionManager::StartTransaction(context);
	}

	atomic<idx_t> start_count {0};
};

template <class T>
struct TestStorageExtension : StorageExtension {
	TestStorageExtension() {
		attach = [](optional_ptr<StorageExtensionInfo>, ClientContext &, AttachedDatabase &db, const string &,
		            AttachInfo &, AttachOptions &) -> unique_ptr<Catalog> {
			return make_uniq_base<Catalog, DuckCatalog>(db);
		};
		create_transaction_manager = [](optional_ptr<StorageExtensionInfo>, AttachedDatabase &db,
		                                Catalog &) -> unique_ptr<TransactionManager> {
			return make_uniq<T>(db);
		};
	}
};

TEST_CASE("Secret lookup from StartTransaction does not deadlock", "[api][transaction]") {
	DBConfig config;
	StorageExtension::Register(config, "secret_lookup_ext",
	                           make_shared_ptr<TestStorageExtension<SecretLookupTransactionManager>>());

	DuckDB db(nullptr, &config);
	Connection con(db);

	REQUIRE_NO_FAIL(con.Query("ATTACH ':memory:' AS ext (TYPE secret_lookup_ext)"));
	auto attached = DatabaseManager::Get(*con.context).GetDatabase("ext");
	REQUIRE(attached);
	auto &manager = attached->GetTransactionManager().Cast<SecretLookupTransactionManager>();

	// the first query that touches "ext" starts its transaction, which looks up a secret in the system catalog
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE ext.tbl AS SELECT 42 AS i"));
	REQUIRE(manager.lookup_count == 1);
	auto result = con.Query("SELECT i FROM ext.tbl");
	REQUIRE(CHECK_COLUMN(result, 0, {42}));
	REQUIRE(manager.lookup_count == 2);

	// also works when the attached database is the first database touched in an explicit transaction
	REQUIRE_NO_FAIL(con.Query("BEGIN"));
	result = con.Query("SELECT i FROM ext.tbl");
	REQUIRE(CHECK_COLUMN(result, 0, {42}));
	REQUIRE_NO_FAIL(con.Query("COMMIT"));

	// and with a secret present that actually matches the lookup
	REQUIRE_NO_FAIL(con.Query("CREATE SECRET http_secret (TYPE http, BEARER_TOKEN 'token', SCOPE 'https://')"));
	REQUIRE(manager.matched_count == 0);
	result = con.Query("SELECT i FROM ext.tbl");
	REQUIRE(CHECK_COLUMN(result, 0, {42}));
	REQUIRE(manager.matched_count == 1);

	REQUIRE_NO_FAIL(con.Query("DETACH ext"));
}

TEST_CASE("Requesting a transaction for the same database from StartTransaction throws", "[api][transaction]") {
	DBConfig config;
	StorageExtension::Register(config, "self_requesting_ext",
	                           make_shared_ptr<TestStorageExtension<SelfRequestingTransactionManager>>());

	DuckDB db(nullptr, &config);
	Connection con(db);

	REQUIRE_NO_FAIL(con.Query("ATTACH ':memory:' AS ext (TYPE self_requesting_ext)"));
	auto result = con.Query("SELECT * FROM ext.information_schema.tables");
	REQUIRE(result->HasError());
	REQUIRE(result->GetErrorType() == ExceptionType::TRANSACTION);
	REQUIRE_NO_FAIL(con.Query("DETACH ext"));
}

#ifndef DUCKDB_NO_THREADS
TEST_CASE("Concurrent requests for a transaction start it exactly once", "[api][transaction]") {
	DBConfig config;
	StorageExtension::Register(config, "slow_ext", make_shared_ptr<TestStorageExtension<SlowTransactionManager>>());

	DuckDB db(nullptr, &config);
	Connection con(db);

	REQUIRE_NO_FAIL(con.Query("ATTACH ':memory:' AS ext (TYPE slow_ext)"));
	auto &context = *con.context;
	auto attached = DatabaseManager::Get(context).GetDatabase("ext");
	REQUIRE(attached);
	auto &manager = attached->GetTransactionManager().Cast<SlowTransactionManager>();

	for (idx_t iteration = 0; iteration < 3; iteration++) {
		manager.start_count = 0;
		con.BeginTransaction();

		constexpr idx_t THREAD_COUNT = 8;
		vector<Transaction *> transactions(THREAD_COUNT, nullptr);
		vector<thread> threads;
		for (idx_t i = 0; i < THREAD_COUNT; i++) {
			threads.emplace_back([&, i]() { transactions[i] = &Transaction::Get(context, *attached); });
		}
		for (auto &t : threads) {
			t.join();
		}
		REQUIRE(manager.start_count == 1);
		for (idx_t i = 0; i < THREAD_COUNT; i++) {
			REQUIRE(transactions[i] == transactions[0]);
		}
		con.Commit();
	}
	REQUIRE_NO_FAIL(con.Query("DETACH ext"));
}
#endif
