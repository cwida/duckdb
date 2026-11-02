#include "catch.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/identifier.hpp"
#include "duckdb/parser/qualified_name.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "test_helpers.hpp"

using namespace duckdb; // NOLINT

namespace {

//! Scan a table through the internal DataTable API, the way extensions (e.g. DuckLake's server-side commit)
//! read their staged tables: DataTable::InitializeScan followed by a DataTable::Scan loop.
idx_t ScanTableThroughStorage(Connection &con, const Identifier &table_name) {
	auto &context = *con.context;
	auto &table =
	    Catalog::GetEntry<TableCatalogEntry>(context, QualifiedName(INVALID_CATALOG, DEFAULT_SCHEMA, table_name));
	auto &storage = table.GetStorage();
	auto &transaction = DuckTransaction::Get(context, table.ParentCatalog());

	vector<StorageIndex> column_ids;
	for (idx_t i = 0; i < table.GetColumns().PhysicalColumnCount(); i++) {
		column_ids.emplace_back(i);
	}

	TableScanState state;
	storage.InitializeScan(context, transaction, state, column_ids);

	DataChunk chunk;
	chunk.Initialize(Allocator::Get(context), storage.GetTypes());

	idx_t count = 0;
	while (true) {
		chunk.Reset();
		storage.Scan(transaction, chunk, state);
		if (chunk.size() == 0) {
			break;
		}
		count += chunk.size();
	}
	return count;
}

} // namespace

TEST_CASE("Test scanning a table that has no committed row groups", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);

	SECTION("table created and filled inside the scanning transaction") {
		// all rows live in LocalStorage, the row group collection is empty
		con.BeginTransaction();
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(i INTEGER)"));
		REQUIRE_NO_FAIL(con.Query("INSERT INTO t VALUES (1), (2), (3)"));

		idx_t count = 0;
		REQUIRE_NOTHROW(count = ScanTableThroughStorage(con, "t"));
		REQUIRE(count == 3);

		con.Rollback();
	}

	SECTION("committed table that never had any rows") {
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE e(i INTEGER)"));

		con.BeginTransaction();
		idx_t count = 1;
		REQUIRE_NOTHROW(count = ScanTableThroughStorage(con, "e"));
		REQUIRE(count == 0);
		con.Commit();
	}

	SECTION("committed rows plus transaction-local rows") {
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE m(i INTEGER)"));
		REQUIRE_NO_FAIL(con.Query("INSERT INTO m VALUES (1), (2)"));

		con.BeginTransaction();
		REQUIRE_NO_FAIL(con.Query("INSERT INTO m VALUES (3), (4), (5)"));

		idx_t count = 0;
		REQUIRE_NOTHROW(count = ScanTableThroughStorage(con, "m"));
		REQUIRE(count == 5);

		con.Rollback();
	}
}
