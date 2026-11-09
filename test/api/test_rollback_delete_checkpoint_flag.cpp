#include "catch.hpp"
#include "duckdb.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/parser/qualified_name.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/row_group.hpp"
#include "duckdb/storage/table/row_group_collection.hpp"
#include "duckdb/storage/table/row_version_manager.hpp"
#include "test_helpers.hpp"

using namespace duckdb; // NOLINT

TEST_CASE("Rolled-back DELETE must not stick uncheckpointed_delete_commit flag", "[storage][rollback][checkpoint]") {
	auto path = TestCreatePath("rollback_delete_checkpoint_flag.db");
	DeleteDatabase(path);
	DuckDB db(path);
	Connection con(db);

	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(k INTEGER)"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO t SELECT range FROM range(1000)"));
	REQUIRE_NO_FAIL(con.Query("DELETE FROM t WHERE k < 500"));
	REQUIRE_NO_FAIL(con.Query("CHECKPOINT"));

	auto has_unserialized_changes = [&]() {
		bool result = false;
		con.context->RunFunctionInTransaction([&]() {
			auto &table = Catalog::GetEntry<TableCatalogEntry>(*con.context, QualifiedName::Parse("t"));
			auto &storage = table.Cast<DuckTableEntry>().GetStorage();
			auto row_group = storage.GetRowGroupCollection()->GetRowGroup(0);
			REQUIRE(row_group);
			result =
			    row_group->GetOrCreateVersionInfo().HasUnserializedChanges(VisibilityBound::IncludingUncommitted());
		});
		return result;
	};

	// After a real DELETE + CHECKPOINT the flag is cleared.
	REQUIRE(!has_unserialized_changes());

	// Rolled-back DELETE followed by CHECKPOINT: the flag must still be clear.
	REQUIRE_NO_FAIL(con.Query("BEGIN"));
	REQUIRE_NO_FAIL(con.Query("DELETE FROM t WHERE k = 999"));
	REQUIRE_NO_FAIL(con.Query("ROLLBACK"));
	REQUIRE_NO_FAIL(con.Query("CHECKPOINT"));

	REQUIRE(!has_unserialized_changes());
}
