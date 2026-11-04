#include "test_capi_v2.hpp"

#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// V2 table description: resolve one table name and snapshot where it
// resolved, its columns, and per-column catalog facts. These tests pin the
// resolution semantics (search path, two-part schema-versus-catalog reading,
// the error cases), the resolved-name reporting, the column getters, and the
// snapshot lifecycle, plus the column description handle.
// ---------------------------------------------------------------------------

namespace test_capi_v2 {

namespace {

// Owned qname built from parts; the caller destroys it.
duckdb_v2_qname_handle MakeQName(const std::vector<const char *> &parts) {
	std::vector<duckdb_v2_identifier_t> views;
	for (auto *part : parts) {
		views.push_back(Convert(part));
	}
	duckdb_v2_qname_handle qname = nullptr;
	REQUIRE(duckdb_v2_qname_create(views.data(), views.size(), &qname, nullptr) == DUCKDB_V2_ERROR_NONE);
	return qname;
}

// Owned description for a name given as parts; asserts success.
duckdb_v2_table_description_handle Describe(duckdb_v2_connection_handle conn, const std::vector<const char *> &parts) {
	auto qname = MakeQName(parts);
	duckdb_v2_table_description_handle desc = nullptr;
	REQUIRE(duckdb_v2_connection_describe_table(conn, qname, &desc, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(desc != nullptr);
	duckdb_v2_qname_destroy(&qname);
	return desc;
}

// The description's resolved name as rendered SQL text.
std::string ResolvedName(duckdb_v2_table_description_handle desc) {
	duckdb_v2_qname_handle qname = nullptr;
	REQUIRE(duckdb_v2_table_description_get_qname(desc, &qname, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto rc = DUCKDB_V2_ERROR_NONE;
	auto out = RenderText(
	    [&](char *buf, idx_t cap, idx_t *len) { return duckdb_v2_qname_render(qname, buf, cap, len, nullptr); }, rc);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_qname_destroy(&qname);
	return out;
}

bool IsReadOnly(duckdb_v2_table_description_handle desc) {
	bool readonly = false;
	REQUIRE(duckdb_v2_table_description_is_readonly(desc, &readonly, nullptr) == DUCKDB_V2_ERROR_NONE);
	return readonly;
}

// Fails to describe a name, handing back the error text.
std::string DescribeError(duckdb_v2_connection_handle conn, const std::vector<const char *> &parts) {
	auto qname = MakeQName(parts);
	duckdb_v2_table_description_handle desc = nullptr;
	duckdb_v2_error_info_handle err = nullptr;
	REQUIRE(duckdb_v2_connection_describe_table(conn, qname, &desc, &err) != DUCKDB_V2_ERROR_NONE);
	REQUIRE(desc == nullptr);
	REQUIRE(err != nullptr);
	duckdb_v2_str message = {nullptr, 0};
	REQUIRE(duckdb_v2_error_info_get_text(err, &message) == DUCKDB_V2_ERROR_NONE);
	auto text = Convert(message);
	duckdb_v2_error_info_destroy(&err);
	duckdb_v2_qname_destroy(&qname);
	return text;
}

} // namespace

TEST_CASE("V2 table description: resolution reports the resolved location", "[capi_v2][catalog]") {
	EnvFixture fx;
	ExecSQL(fx.conn, "CREATE TABLE t(i INTEGER)");
	ExecSQL(fx.conn, "CREATE TEMP TABLE tt(i INTEGER)");
	ExecSQL(fx.conn, "CREATE TABLE \"FooBar\"(i INTEGER)");

	// An unqualified name fills in the resolved catalog and schema.
	auto desc = Describe(fx.conn, {"t"});
	REQUIRE(ResolvedName(desc) == "memory.main.t");
	REQUIRE_FALSE(IsReadOnly(desc));
	duckdb_v2_table_description_destroy(&desc);

	// A temp table resolves through the temp catalog, which is writable. The
	// catalog renders quoted because temp is a parser keyword.
	desc = Describe(fx.conn, {"tt"});
	REQUIRE(ResolvedName(desc) == "\"temp\".main.tt");
	REQUIRE_FALSE(IsReadOnly(desc));
	duckdb_v2_table_description_destroy(&desc);

	// The name comes back with its DDL time casing, not the lookup casing.
	desc = Describe(fx.conn, {"foobar"});
	REQUIRE(ResolvedName(desc) == "memory.main.FooBar");
	duckdb_v2_table_description_destroy(&desc);

	// A fully qualified name resolves as written.
	desc = Describe(fx.conn, {"memory", "main", "t"});
	REQUIRE(ResolvedName(desc) == "memory.main.t");
	duckdb_v2_table_description_destroy(&desc);
}

TEST_CASE("V2 table description: two-part names read as SQL reads them", "[capi_v2][catalog]") {
	EnvFixture fx;
	ExecSQL(fx.conn, "CREATE SCHEMA s");
	ExecSQL(fx.conn, "CREATE TABLE s.t2(i INTEGER)");
	ExecSQL(fx.conn, "ATTACH ':memory:' AS other");
	ExecSQL(fx.conn, "CREATE TABLE other.t3(i INTEGER)");

	// schema.table resolves within the default catalog.
	auto desc = Describe(fx.conn, {"s", "t2"});
	REQUIRE(ResolvedName(desc) == "memory.s.t2");
	duckdb_v2_table_description_destroy(&desc);

	// catalog.table promotes the first part to an attached database.
	desc = Describe(fx.conn, {"other", "t3"});
	REQUIRE(ResolvedName(desc) == "other.main.t3");
	duckdb_v2_table_description_destroy(&desc);

	// A first part naming both a schema and an attached database is ambiguous.
	ExecSQL(fx.conn, "CREATE SCHEMA amb");
	ExecSQL(fx.conn, "ATTACH ':memory:' AS amb");
	REQUIRE(DescribeError(fx.conn, {"amb", "t2"}).find("Ambiguous") != std::string::npos);
}

TEST_CASE("V2 table description: missing tables and views are rejected", "[capi_v2][catalog]") {
	EnvFixture fx;
	ExecSQL(fx.conn, "CREATE VIEW v AS SELECT 42 AS i");

	// A name that resolves to nothing.
	REQUIRE(DescribeError(fx.conn, {"no_such_table"}).find("does not exist") != std::string::npos);

	// A name that resolves to a view: a description snapshots a base table.
	REQUIRE(DescribeError(fx.conn, {"v"}).find("does not exist") != std::string::npos);

	// Null arguments are rejected.
	auto qname = MakeQName({"v"});
	duckdb_v2_table_description_handle desc = nullptr;
	REQUIRE(duckdb_v2_connection_describe_table(nullptr, qname, &desc, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_connection_describe_table(fx.conn, nullptr, &desc, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_connection_describe_table(fx.conn, qname, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	duckdb_v2_qname_destroy(&qname);
}

TEST_CASE("V2 table description: describing a view keeps the open transaction", "[capi_v2][catalog]") {
	EnvFixture fx;
	ExecSQL(fx.conn, "CREATE TABLE u(k INTEGER)");
	ExecSQL(fx.conn, "CREATE VIEW v AS SELECT 42 AS i");

	REQUIRE(RunSql(fx.conn, "BEGIN TRANSACTION") == DUCKDB_V2_ERROR_NONE);
	REQUIRE(RunSql(fx.conn, "INSERT INTO u VALUES (1)") == DUCKDB_V2_ERROR_NONE);
	REQUIRE(DescribeError(fx.conn, {"v"}).find("does not exist") != std::string::npos);
	REQUIRE(RunSql(fx.conn, "INSERT INTO u VALUES (2)") == DUCKDB_V2_ERROR_NONE);
	REQUIRE(RunSql(fx.conn, "COMMIT") == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_result_handle result = nullptr;
	REQUIRE(Query(fx.conn, "SELECT k FROM u WHERE k IN (1, 2)", &result) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(DrainRowCount(result) == 2);
	duckdb_v2_result_destroy(&result);
}

TEST_CASE("V2 table description: column descriptions", "[capi_v2][catalog]") {
	EnvFixture fx;
	ExecSQL(fx.conn, "CREATE TABLE facts("
	                 "i INTEGER, "
	                 "\"J\" VARCHAR DEFAULT 'x', "
	                 "k INTEGER GENERATED ALWAYS AS (i + 1))");

	auto desc = Describe(fx.conn, {"facts"});

	// Every column in declared order, the generated one included.
	idx_t count = 0;
	REQUIRE(duckdb_v2_table_description_get_column_count(desc, &count, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(count == 3);

	const char *expected_names[] = {"i", "J", "k"};
	DUCKDB_V2_LOGICAL_TYPE_ID expected_types[] = {DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER, DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR,
	                                              DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER};
	const bool expected_generated[] = {false, false, true};
	const bool expected_default[] = {false, true, false};
	std::vector<duckdb_v2_column_description_handle> columns;
	for (idx_t i = 0; i < count; i++) {
		duckdb_v2_column_description_handle column = nullptr;
		REQUIRE(duckdb_v2_table_description_get_column(desc, i, &column, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(column != nullptr);
		columns.push_back(column);
	}

	// A column description is independent of the table description it came from.
	duckdb_v2_table_description_destroy(&desc);

	for (idx_t i = 0; i < count; i++) {
		auto column = columns[i];
		duckdb_v2_identifier_t name = {nullptr, 0};
		REQUIRE(duckdb_v2_column_description_get_name(column, &name, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(name == expected_names[i]);

		duckdb_v2_logical_type_handle type = nullptr; // borrowed; do not destroy
		REQUIRE(duckdb_v2_column_description_get_type(column, &type, nullptr) == DUCKDB_V2_ERROR_NONE);
		DUCKDB_V2_LOGICAL_TYPE_ID id = DUCKDB_V2_LOGICAL_TYPE_ID_INVALID;
		REQUIRE(duckdb_v2_logical_type_get_id(type, &id, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(id == expected_types[i]);

		bool has_generated = false;
		REQUIRE(duckdb_v2_column_description_has_generated(column, &has_generated, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(has_generated == expected_generated[i]);

		bool has_default = false;
		REQUIRE(duckdb_v2_column_description_has_default(column, &has_default, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(has_default == expected_default[i]);

		duckdb_v2_column_description_destroy(&column);
		REQUIRE(column == nullptr);
	}

	// An out-of-range index is rejected.
	desc = Describe(fx.conn, {"facts"});
	duckdb_v2_column_description_handle column = nullptr;
	REQUIRE(duckdb_v2_table_description_get_column(desc, 3, &column, nullptr) == DUCKDB_V2_ERROR_INPUT_OUT_OF_RANGE);
	REQUIRE(column == nullptr);
	duckdb_v2_table_description_destroy(&desc);

	// Destroy is null-safe and idempotent; null subjects are rejected.
	REQUIRE(duckdb_v2_column_description_destroy(nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_column_description_destroy(&column) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_identifier_t name = {nullptr, 0};
	REQUIRE(duckdb_v2_column_description_get_name(nullptr, &name, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	bool flag = false;
	REQUIRE(duckdb_v2_column_description_has_default(nullptr, &flag, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
}

TEST_CASE("V2 table description: a read only catalog reports readonly", "[capi_v2][catalog]") {
	EnvFixture fx;
	auto path = duckdb::TestCreatePath("v2_catalog_readonly.db");
	duckdb::DeleteDatabase(path);

	// Create the file backed table through a writable attach, then reattach read only.
	ExecSQL(fx.conn, ("ATTACH '" + path + "' AS rw").c_str());
	ExecSQL(fx.conn, "CREATE TABLE rw.rt(i INTEGER)");
	ExecSQL(fx.conn, "DETACH rw");
	ExecSQL(fx.conn, ("ATTACH '" + path + "' AS ro (READ_ONLY)").c_str());

	auto desc = Describe(fx.conn, {"ro", "rt"});
	REQUIRE(ResolvedName(desc) == "ro.main.rt");
	REQUIRE(IsReadOnly(desc));
	duckdb_v2_table_description_destroy(&desc);

	ExecSQL(fx.conn, "DETACH ro");
	duckdb::DeleteDatabase(path);
}

TEST_CASE("V2 table description: a description is a snapshot", "[capi_v2][catalog]") {
	EnvFixture fx;
	ExecSQL(fx.conn, "CREATE TABLE snap(i INTEGER)");

	auto desc = Describe(fx.conn, {"snap"});
	ExecSQL(fx.conn, "DROP TABLE snap");

	// The snapshot stays readable after the table is gone.
	REQUIRE(ResolvedName(desc) == "memory.main.snap");
	idx_t count = 0;
	REQUIRE(duckdb_v2_table_description_get_column_count(desc, &count, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(count == 1);
	duckdb_v2_table_description_destroy(&desc);
	REQUIRE(desc == nullptr);

	// Destroy is null-safe and idempotent.
	REQUIRE(duckdb_v2_table_description_destroy(nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_description_destroy(&desc) == DUCKDB_V2_ERROR_NONE);

	// Null getter subjects and out-slots are rejected.
	duckdb_v2_qname_handle qname = nullptr;
	REQUIRE(duckdb_v2_table_description_get_qname(nullptr, &qname, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_table_description_get_column_count(nullptr, &count, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	bool flag = false;
	REQUIRE(duckdb_v2_table_description_is_readonly(nullptr, &flag, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
}

} // namespace test_capi_v2
