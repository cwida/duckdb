#include "catch.hpp"
#include "duckdb.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/parser/constraints/foreign_key_constraint.hpp"
#include "duckdb/parser/qualified_name.hpp"
#include "test_helpers.hpp"

using namespace duckdb; // NOLINT

namespace {

idx_t CountPrimaryKeySideConstraints(ClientContext &context, const string &table_name, const string &referencing_table) {
	idx_t count = 0;
	context.RunFunctionInTransaction([&]() {
		auto &table = Catalog::GetEntry<TableCatalogEntry>(context, QualifiedName::Parse(table_name));
		for (auto &constraint : table.GetConstraints()) {
			if (constraint->type != ConstraintType::FOREIGN_KEY) {
				continue;
			}
			auto &fk = constraint->Cast<ForeignKeyConstraint>();
			if (fk.info.type != ForeignKeyType::FK_TYPE_PRIMARY_KEY_TABLE) {
				continue;
			}
			if (fk.info.table == Identifier(referencing_table)) {
				count++;
			}
		}
	});
	return count;
}

} // namespace

TEST_CASE("Reopening a database does not duplicate the primary-key side foreign keys", "[api][foreign_key][checkpoint]") {
	auto path = TestCreatePath("foreign_key_checkpoint_metadata.db");
	DeleteDatabase(path);

	{
		DuckDB db(path);
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE p(id INTEGER PRIMARY KEY)"));
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE c_1(pid INTEGER REFERENCES p(id))"));
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE c_2(pid INTEGER REFERENCES p(id))"));
	}

	// Every open -> write -> close cycle checkpoints the catalog again. The referenced table must keep exactly
	// one primary-key side constraint per referencing table instead of accumulating a copy on every load.
	for (idx_t i = 0; i < 3; i++) {
		DuckDB db(path);
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("INSERT INTO p VALUES (" + to_string(i) + ")"));
		REQUIRE(CountPrimaryKeySideConstraints(*con.context, "p", "c_1") == 1);
		REQUIRE(CountPrimaryKeySideConstraints(*con.context, "p", "c_2") == 1);
	}

	DuckDB db(path);
	Connection con(db);
	REQUIRE(CountPrimaryKeySideConstraints(*con.context, "p", "c_1") == 1);
	REQUIRE(CountPrimaryKeySideConstraints(*con.context, "p", "c_2") == 1);
	// The foreign keys are still enforced.
	REQUIRE_FAIL(con.Query("INSERT INTO c_1 VALUES (100)"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO c_1 VALUES (2)"));
	REQUIRE_FAIL(con.Query("DELETE FROM p WHERE id = 2"));
}
