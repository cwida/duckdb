#include "test_capi_v2.hpp"

#include <cstring>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// V2 statement collection tests: bind a column data collection to a parsed
// statement under a name with statement_add_collection, and execute it.
// ---------------------------------------------------------------------------

namespace test_capi_v2 {

namespace {

// Appends the values to a single-column BIGINT collection.
void CollAppend(duckdb_v2_factory_handle factory, duckdb_v2_column_data_collection_handle cdc,
                const std::vector<int64_t> &values) {
	auto bigint = MakeType(factory, DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT);
	duckdb_v2_data_chunk_handle chunk = nullptr;
	auto chunk_rc = duckdb_v2_data_chunk_create(factory, &bigint, 1, &chunk, nullptr);
	duckdb_v2_logical_type_destroy(&bigint);
	REQUIRE(chunk_rc == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_vector_handle vec = nullptr;
	REQUIRE(duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_vector_set_size(vec, values.size(), nullptr) == DUCKDB_V2_ERROR_NONE);
	if (!values.empty()) {
		void *raw = nullptr;
		REQUIRE(duckdb_v2_vector_get_data_mutable(vec, &raw, nullptr) == DUCKDB_V2_ERROR_NONE);
		std::memcpy(raw, values.data(), values.size() * sizeof(int64_t));
	}

	duckdb_v2_column_data_collection_append_state_handle state = nullptr;
	REQUIRE(duckdb_v2_column_data_collection_append_state_create(cdc, &state, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto append_rc = duckdb_v2_column_data_collection_append(cdc, state, chunk, nullptr);
	duckdb_v2_column_data_collection_append_state_destroy(&state);
	duckdb_v2_data_chunk_destroy(&chunk);
	REQUIRE(append_rc == DUCKDB_V2_ERROR_NONE);
}

// A single-column BIGINT collection holding the values.
duckdb_v2_column_data_collection_handle CollMake(duckdb_v2_factory_handle factory, const std::vector<int64_t> &values) {
	auto bigint = MakeType(factory, DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT);
	duckdb_v2_column_data_collection_handle cdc = nullptr;
	auto rc = duckdb_v2_column_data_collection_create(factory, &bigint, 1, &cdc, nullptr);
	duckdb_v2_logical_type_destroy(&bigint);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);
	CollAppend(factory, cdc, values);
	return cdc;
}

duckdb_v2_sql_statement_handle CollParse(duckdb_v2_connection_handle conn, const char *sql) {
	duckdb_v2_statement_iterator_handle iter = nullptr;
	REQUIRE(duckdb_v2_parse_sql(conn, sql, true, &iter, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_sql_statement_handle stmt = nullptr;
	auto rc = duckdb_v2_statement_iterator_next(iter, &stmt, nullptr);
	duckdb_v2_statement_iterator_destroy(&iter);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);
	REQUIRE(stmt);
	return stmt;
}

DUCKDB_V2_ERROR CollBind(duckdb_v2_sql_statement_handle stmt, const char *name,
                         duckdb_v2_column_data_collection_handle cdc,
                         const std::vector<const char *> &column_names = {}) {
	auto name_id = Convert(name);
	std::vector<duckdb_v2_identifier_t> names;
	for (auto column_name : column_names) {
		names.push_back(Convert(column_name));
	}
	return duckdb_v2_statement_add_collection(stmt, &name_id, cdc, names.empty() ? nullptr : names.data(), names.size(),
	                                          nullptr);
}

// The BIGINT values of the first column of a result, which is destroyed.
std::vector<int64_t> CollDrainI64(duckdb_v2_result_handle result) {
	std::vector<int64_t> out;
	while (auto chunk = StepChunk(result)) {
		idx_t size = 0;
		duckdb_v2_data_chunk_get_size(chunk, &size, nullptr);
		duckdb_v2_vector_handle vec = nullptr;
		duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr);
		duckdb_v2_vector_view view {};
		duckdb_v2_vector_get_view(vec, &view, nullptr);
		for (idx_t i = 0; i < size; i++) {
			out.push_back(static_cast<const int64_t *>(view.data)[SelAt(view.sel, i)]);
		}
		duckdb_v2_data_chunk_destroy(&chunk);
	}
	duckdb_v2_result_destroy(&result);
	return out;
}

std::vector<int64_t> CollExecute(duckdb_v2_connection_handle conn, duckdb_v2_sql_statement_handle stmt) {
	duckdb_v2_result_handle result = nullptr;
	REQUIRE(duckdb_v2_statement_execute(conn, stmt, nullptr, nullptr, 0, &result, nullptr) == DUCKDB_V2_ERROR_NONE);
	return CollDrainI64(result);
}

std::vector<int64_t> CollQuery(duckdb_v2_connection_handle conn, const char *sql) {
	duckdb_v2_result_handle result = nullptr;
	REQUIRE(Query(conn, sql, &result) == DUCKDB_V2_ERROR_NONE);
	return CollDrainI64(result);
}

} // namespace

TEST_CASE("V2 statement collection: INSERT and SELECT over a bound collection", "[capi_v2][statement_collection]") {
	EnvFixture fx;
	REQUIRE(RunSql(fx.conn, "CREATE TABLE t (v BIGINT, tag VARCHAR DEFAULT 'seen')") == DUCKDB_V2_ERROR_NONE);
	auto cdc = CollMake(fx.factory, {3, 1, 2});

	// The columns are named col1..colN by default.
	auto insert = CollParse(fx.conn, "INSERT INTO t (v) SELECT col1 FROM buf");
	REQUIRE(CollBind(insert, "buf", cdc) == DUCKDB_V2_ERROR_NONE);
	CollExecute(fx.conn, insert);
	REQUIRE(CollQuery(fx.conn, "SELECT v FROM t ORDER BY v") == std::vector<int64_t> {1, 2, 3});
	REQUIRE(CollQuery(fx.conn, "SELECT count(*)::BIGINT FROM t WHERE tag = 'seen'") == std::vector<int64_t> {3});

	// Or as the caller names them, and several collections bind under distinct names.
	auto other = CollMake(fx.factory, {2, 3, 4});
	auto join = CollParse(fx.conn, "SELECT a.x FROM a JOIN b ON a.x = b.y ORDER BY a.x");
	REQUIRE(CollBind(join, "a", cdc, {"x"}) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(CollBind(join, "b", other, {"y"}) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(CollExecute(fx.conn, join) == std::vector<int64_t> {2, 3});

	// The collection drives UPDATE, DELETE and MERGE INTO too.
	auto del = CollParse(fx.conn, "DELETE FROM t WHERE v IN (SELECT col1 FROM gone)");
	auto gone = CollMake(fx.factory, {1});
	REQUIRE(CollBind(del, "gone", gone) == DUCKDB_V2_ERROR_NONE);
	CollExecute(fx.conn, del);
	auto update = CollParse(fx.conn, "UPDATE t SET tag = 'updated' FROM upd WHERE t.v = upd.col1");
	auto upd = CollMake(fx.factory, {2});
	REQUIRE(CollBind(update, "upd", upd) == DUCKDB_V2_ERROR_NONE);
	CollExecute(fx.conn, update);
	auto merge = CollParse(fx.conn, "MERGE INTO t USING src ON t.v = src.col1 WHEN NOT MATCHED THEN INSERT (v) "
	                                "VALUES (src.col1)");
	auto src = CollMake(fx.factory, {3, 9});
	REQUIRE(CollBind(merge, "src", src) == DUCKDB_V2_ERROR_NONE);
	CollExecute(fx.conn, merge);
	REQUIRE(CollQuery(fx.conn, "SELECT v FROM t ORDER BY v") == std::vector<int64_t> {2, 3, 9});
	REQUIRE(CollQuery(fx.conn, "SELECT v FROM t WHERE tag = 'updated'") == std::vector<int64_t> {2});

	// The binding is the statement's own: other statements do not see it.
	REQUIRE(RunSql(fx.conn, "SELECT * FROM buf") != DUCKDB_V2_ERROR_NONE);

	for (auto stmt : {insert, join, del, update, merge}) {
		duckdb_v2_sql_statement_destroy(&stmt);
	}
	for (auto collection : {cdc, other, gone, upd, src}) {
		duckdb_v2_column_data_collection_destroy(&collection);
	}
}

TEST_CASE("V2 statement collection: each execution reads the current contents", "[capi_v2][statement_collection]") {
	EnvFixture fx;
	REQUIRE(RunSql(fx.conn, "CREATE TABLE t (v BIGINT)") == DUCKDB_V2_ERROR_NONE);
	auto cdc = CollMake(fx.factory, {1, 2});
	auto stmt = CollParse(fx.conn, "INSERT INTO t SELECT * FROM buf");
	REQUIRE(CollBind(stmt, "buf", cdc) == DUCKDB_V2_ERROR_NONE);

	CollExecute(fx.conn, stmt);
	REQUIRE(duckdb_v2_column_data_collection_clear(cdc, nullptr) == DUCKDB_V2_ERROR_NONE);
	CollAppend(fx.factory, cdc, {3});
	CollExecute(fx.conn, stmt);
	REQUIRE(CollQuery(fx.conn, "SELECT v FROM t ORDER BY v") == std::vector<int64_t> {1, 2, 3});

	// A prepared statement reuses its plan and still reads what the collection holds when it runs.
	duckdb_v2_prepared_statement_handle prepared = nullptr;
	REQUIRE(duckdb_v2_prepared_statement_create(fx.conn, stmt, false, &prepared, nullptr) == DUCKDB_V2_ERROR_NONE);
	bool reuses = false;
	REQUIRE(duckdb_v2_prepared_statement_reuses_plan(prepared, &reuses, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(reuses);
	for (int64_t batch = 10; batch < 13; batch++) {
		REQUIRE(duckdb_v2_column_data_collection_clear(cdc, nullptr) == DUCKDB_V2_ERROR_NONE);
		CollAppend(fx.factory, cdc, {batch});
		duckdb_v2_result_handle result = nullptr;
		REQUIRE(duckdb_v2_prepared_statement_execute(prepared, nullptr, nullptr, 0, &result, nullptr) ==
		        DUCKDB_V2_ERROR_NONE);
		CollDrainI64(result);
	}
	REQUIRE(CollQuery(fx.conn, "SELECT v FROM t WHERE v >= 10 ORDER BY v") == std::vector<int64_t> {10, 11, 12});

	// An empty collection is a valid binding.
	REQUIRE(duckdb_v2_column_data_collection_clear(cdc, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_result_handle result = nullptr;
	REQUIRE(duckdb_v2_prepared_statement_execute(prepared, nullptr, nullptr, 0, &result, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(DrainChangedRows(result) == 0);
	duckdb_v2_result_destroy(&result);

	duckdb_v2_prepared_statement_destroy(&prepared);
	duckdb_v2_sql_statement_destroy(&stmt);
	duckdb_v2_column_data_collection_destroy(&cdc);
}

TEST_CASE("V2 statement collection: shadows a table and binds into the statement schema",
          "[capi_v2][statement_collection]") {
	EnvFixture fx;
	REQUIRE(RunSql(fx.conn, "CREATE TABLE buf (v BIGINT)") == DUCKDB_V2_ERROR_NONE);
	REQUIRE(RunSql(fx.conn, "INSERT INTO buf VALUES (100)") == DUCKDB_V2_ERROR_NONE);
	auto cdc = CollMake(fx.factory, {7});
	auto stmt = CollParse(fx.conn, "SELECT * FROM buf");
	REQUIRE(CollBind(stmt, "buf", cdc, {"amount"}) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_schema_handle schema = nullptr;
	REQUIRE(duckdb_v2_statement_bind(fx.conn, stmt, &schema, nullptr, nullptr) == DUCKDB_V2_ERROR_NONE);
	idx_t count = 0;
	REQUIRE(duckdb_v2_schema_get_count(schema, &count, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(count == 1);
	duckdb_v2_identifier_t name = {nullptr, 0};
	duckdb_v2_logical_type_handle type = nullptr;
	REQUIRE(duckdb_v2_schema_get_field(schema, 0, &name, &type, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(std::string(name.ptr, name.len) == "amount");
	duckdb_v2_schema_destroy(&schema);

	REQUIRE(CollExecute(fx.conn, stmt) == std::vector<int64_t> {7});
	duckdb_v2_sql_statement_destroy(&stmt);
	duckdb_v2_column_data_collection_destroy(&cdc);
}

TEST_CASE("V2 statement collection: refusals", "[capi_v2][statement_collection]") {
	EnvFixture fx;
	auto cdc = CollMake(fx.factory, {1});

	// Only statements with common table expressions accept a collection.
	auto create = CollParse(fx.conn, "CREATE TABLE c AS SELECT * FROM buf");
	REQUIRE(CollBind(create, "buf", cdc) == DUCKDB_V2_ERROR_INPUT_INVALID);
	duckdb_v2_sql_statement_destroy(&create);

	// A name the statement already defines, in its text or by a prior call.
	auto with = CollParse(fx.conn, "WITH buf AS (SELECT 1) SELECT * FROM buf");
	REQUIRE(CollBind(with, "BUF", cdc) == DUCKDB_V2_ERROR_INPUT_INVALID);
	duckdb_v2_sql_statement_destroy(&with);
	auto stmt = CollParse(fx.conn, "SELECT * FROM buf");
	REQUIRE(CollBind(stmt, "buf", cdc) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(CollBind(stmt, "buf", cdc) == DUCKDB_V2_ERROR_INPUT_INVALID);

	// Bad names.
	REQUIRE(CollBind(stmt, "", cdc) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(CollBind(stmt, "other", cdc, {"a", "b"}) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(CollBind(stmt, "other", cdc, {""}) == DUCKDB_V2_ERROR_INPUT_INVALID);
	auto name = Convert("other");
	REQUIRE(duckdb_v2_statement_add_collection(stmt, &name, cdc, nullptr, 1, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	// Null arguments.
	REQUIRE(duckdb_v2_statement_add_collection(nullptr, &name, cdc, nullptr, 0, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_statement_add_collection(stmt, nullptr, cdc, nullptr, 0, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_statement_add_collection(stmt, &name, nullptr, nullptr, 0, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);

	// The refusals left the statement as it was.
	REQUIRE(CollExecute(fx.conn, stmt) == std::vector<int64_t> {1});
	duckdb_v2_sql_statement_destroy(&stmt);
	duckdb_v2_column_data_collection_destroy(&cdc);
}

} // namespace test_capi_v2
