#include "capi_tester.hpp"

using namespace duckdb;
using namespace std;

TEST_CASE("Test C API GEOMETRY type support", "[capi]") {
	CAPITester tester;
	duckdb::unique_ptr<CAPIResult> result;

	REQUIRE(tester.OpenDatabase(nullptr));
	REQUIRE_NO_FAIL(tester.Query("CREATE TABLE t1 (data GEOMETRY('OGC:CRS84'))"));
	REQUIRE_NO_FAIL(tester.Query("INSERT INTO t1 VALUES ('POINT(42 1337)')"));

	result = tester.Query("SELECT data FROM t1");
	REQUIRE_NO_FAIL(*result);

	REQUIRE(result->ColumnType(0) == DUCKDB_TYPE_GEOMETRY);

	auto chunk = result->FetchChunk(0);
	REQUIRE(chunk);

	auto vec = duckdb_data_chunk_get_vector(chunk->GetChunk(), 0);

	auto type = duckdb_vector_get_column_type(vec);

	REQUIRE(duckdb_get_type_id(type) == DUCKDB_TYPE_GEOMETRY);
	auto crs = duckdb_geometry_type_get_crs(type);
	REQUIRE(crs);
	REQUIRE(strcmp(crs, "OGC:CRS84") == 0);
	duckdb_free(crs);

	duckdb_destroy_logical_type(&type);

	auto blob = *static_cast<duckdb_string_t *>(duckdb_vector_get_data(vec));

	REQUIRE(duckdb_string_t_length(blob) == 21);

	auto data = duckdb_string_t_data(&blob);

	uint8_t le = 0;
	uint32_t meta = 0;
	double x;
	double y;

	memcpy(&le, data, 1);
	memcpy(&meta, data + 1, 4);
	memcpy(&x, data + 5, 8);
	memcpy(&y, data + 13, 8);

	REQUIRE(le == 1);
	REQUIRE(meta == 0x00000001); // WKB for POINT
	REQUIRE(x == 42);
	REQUIRE(y == 1337);
}

static const uint8_t POINT_WKB[] = {0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                    0x45, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0xE4, 0x94, 0x40}; // POINT (42 1337)

TEST_CASE("Test C API GEOMETRY value cast to and from BLOB", "[capi]") {
	auto geom_type = duckdb_create_logical_type(DUCKDB_TYPE_GEOMETRY);

	// Creating a GEOMETRY list from a BLOB value casts the WKB to GEOMETRY
	auto blob_val = duckdb_create_blob(POINT_WKB, sizeof(POINT_WKB));
	auto list_val = duckdb_create_list_value(geom_type, &blob_val, 1);
	REQUIRE(list_val);
	auto geom_val = duckdb_get_list_child(list_val, 0);
	REQUIRE(geom_val);

	auto val_type = duckdb_get_value_type(geom_val);
	REQUIRE(duckdb_get_type_id(val_type) == DUCKDB_TYPE_GEOMETRY);

	auto wkt = duckdb_get_varchar(geom_val);
	REQUIRE(string(wkt) == "POINT (42 1337)");
	duckdb_free(wkt);

	// Reading a GEOMETRY value as a BLOB casts it to WKB
	auto wkb = duckdb_get_blob(geom_val);
	REQUIRE(wkb.size == sizeof(POINT_WKB));
	REQUIRE(memcmp(wkb.data, POINT_WKB, sizeof(POINT_WKB)) == 0);
	duckdb_free(wkb.data);

	// Invalid WKB cannot be cast to GEOMETRY
	auto invalid_blob_val = duckdb_create_blob(POINT_WKB, 5);
	auto invalid_list_val = duckdb_create_list_value(geom_type, &invalid_blob_val, 1);
	REQUIRE(!invalid_list_val);

	duckdb_destroy_value(&invalid_blob_val);
	duckdb_destroy_value(&geom_val);
	duckdb_destroy_value(&list_val);
	duckdb_destroy_value(&blob_val);
	duckdb_destroy_logical_type(&geom_type);
}

TEST_CASE("Test C API GEOMETRY vector cast to and from BLOB", "[capi]") {
	CAPITester tester;
	duckdb::unique_ptr<CAPIResult> result;

	REQUIRE(tester.OpenDatabase(nullptr));
	REQUIRE_NO_FAIL(tester.Query("CREATE TABLE geoms (g GEOMETRY)"));
	REQUIRE_NO_FAIL(tester.Query("INSERT INTO geoms VALUES ('POINT(42 1337)'), (NULL), ('LINESTRING(0 0, 1 1)')"));
	REQUIRE_NO_FAIL(tester.Query("CREATE TABLE blobs (b BLOB)"));

	// Append a GEOMETRY vector to a BLOB column: the appender casts it to WKB
	result = tester.Query("SELECT g FROM geoms");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->ChunkCount() > 0);

	duckdb_appender appender;
	REQUIRE(duckdb_appender_create(tester.connection, nullptr, "blobs", &appender) == DuckDBSuccess);
	for (idx_t chunk_idx = 0; chunk_idx < result->ChunkCount(); chunk_idx++) {
		auto chunk = result->FetchChunk(chunk_idx);
		REQUIRE(chunk);
		REQUIRE(duckdb_append_data_chunk(appender, chunk->GetChunk()) == DuckDBSuccess);
	}
	REQUIRE(duckdb_appender_close(appender) == DuckDBSuccess);
	duckdb_appender_destroy(&appender);

	result = tester.Query("SELECT hex(b), b IS NOT DISTINCT FROM ST_AsWKB(g) FROM blobs POSITIONAL JOIN geoms");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->row_count() == 3);
	REQUIRE(result->Fetch<string>(0, 0) == "010100000000000000000045400000000000E49440");
	REQUIRE(result->IsNull(0, 1));
	REQUIRE(result->Fetch<bool>(1, 0));
	REQUIRE(result->Fetch<bool>(1, 1));
	REQUIRE(result->Fetch<bool>(1, 2));

	// Append a BLOB vector of WKB to a GEOMETRY column: the appender casts it to GEOMETRY
	REQUIRE_NO_FAIL(tester.Query("CREATE TABLE geoms_from_wkb (g GEOMETRY)"));

	auto blob_type = duckdb_create_logical_type(DUCKDB_TYPE_BLOB);
	auto blob_chunk = duckdb_create_data_chunk(&blob_type, 1);
	auto blob_vec = duckdb_data_chunk_get_vector(blob_chunk, 0);
	duckdb_vector_assign_string_element_len(blob_vec, 0, reinterpret_cast<const char *>(POINT_WKB), sizeof(POINT_WKB));
	duckdb_data_chunk_set_size(blob_chunk, 1);

	REQUIRE(duckdb_appender_create(tester.connection, nullptr, "geoms_from_wkb", &appender) == DuckDBSuccess);
	REQUIRE(duckdb_append_data_chunk(appender, blob_chunk) == DuckDBSuccess);
	REQUIRE(duckdb_appender_close(appender) == DuckDBSuccess);
	duckdb_appender_destroy(&appender);

	result = tester.Query("SELECT ST_AsText(g) FROM geoms_from_wkb");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->row_count() == 1);
	REQUIRE(result->Fetch<string>(0, 0) == "POINT (42 1337)");

	// Invalid WKB fails to cast
	duckdb_vector_assign_string_element_len(blob_vec, 0, reinterpret_cast<const char *>(POINT_WKB), 5);
	REQUIRE(duckdb_appender_create(tester.connection, nullptr, "geoms_from_wkb", &appender) == DuckDBSuccess);
	REQUIRE(duckdb_append_data_chunk(appender, blob_chunk) == DuckDBError);
	duckdb_appender_destroy(&appender);

	duckdb_destroy_data_chunk(&blob_chunk);
	duckdb_destroy_logical_type(&blob_type);
}

TEST_CASE("Test C API GEOGRAPHY type support", "[capi]") {
	CAPITester tester;
	duckdb::unique_ptr<CAPIResult> result;

	REQUIRE(tester.OpenDatabase(nullptr));
	REQUIRE_NO_FAIL(tester.Query("CREATE TABLE t1 (data GEOGRAPHY('OGC:CRS84'))"));
	REQUIRE_NO_FAIL(tester.Query("INSERT INTO t1 VALUES ('POINT(42 13)')"));

	result = tester.Query("SELECT data FROM t1");
	REQUIRE_NO_FAIL(*result);

	REQUIRE(result->ColumnType(0) == DUCKDB_TYPE_GEOGRAPHY);

	auto chunk = result->FetchChunk(0);
	REQUIRE(chunk);

	auto vec = duckdb_data_chunk_get_vector(chunk->GetChunk(), 0);

	auto type = duckdb_vector_get_column_type(vec);

	REQUIRE(duckdb_get_type_id(type) == DUCKDB_TYPE_GEOGRAPHY);
	auto crs = duckdb_geometry_type_get_crs(type);
	REQUIRE(crs);
	REQUIRE(strcmp(crs, "OGC:CRS84") == 0);
	duckdb_free(crs);

	duckdb_destroy_logical_type(&type);

	auto blob = *static_cast<duckdb_string_t *>(duckdb_vector_get_data(vec));

	REQUIRE(duckdb_string_t_length(blob) == 21);

	auto data = duckdb_string_t_data(&blob);

	uint8_t le = 0;
	uint32_t meta = 0;
	double x;
	double y;

	memcpy(&le, data, 1);
	memcpy(&meta, data + 1, 4);
	memcpy(&x, data + 5, 8);
	memcpy(&y, data + 13, 8);

	REQUIRE(le == 1);
	REQUIRE(meta == 0x00000001); // WKB for POINT
	REQUIRE(x == 42);
	REQUIRE(y == 13);
}

static const uint8_t GEOG_POINT_WKB[] = {0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                         0x45, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2A, 0x40}; // POINT (42 13)

TEST_CASE("Test C API GEOGRAPHY value cast to and from BLOB", "[capi]") {
	auto geog_type = duckdb_create_logical_type(DUCKDB_TYPE_GEOGRAPHY);

	// Creating a GEOGRAPHY list from a BLOB value casts the WKB to GEOGRAPHY
	auto blob_val = duckdb_create_blob(GEOG_POINT_WKB, sizeof(GEOG_POINT_WKB));
	auto list_val = duckdb_create_list_value(geog_type, &blob_val, 1);
	REQUIRE(list_val);
	auto geog_val = duckdb_get_list_child(list_val, 0);
	REQUIRE(geog_val);
	REQUIRE(duckdb_get_type_id(duckdb_get_value_type(geog_val)) == DUCKDB_TYPE_GEOGRAPHY);

	auto wkt = duckdb_get_varchar(geog_val);
	REQUIRE(string(wkt) == "POINT (42 13)");
	duckdb_free(wkt);

	// Reading a GEOGRAPHY value as a BLOB casts it to WKB
	auto wkb = duckdb_get_blob(geog_val);
	REQUIRE(wkb.size == sizeof(GEOG_POINT_WKB));
	REQUIRE(memcmp(wkb.data, GEOG_POINT_WKB, sizeof(GEOG_POINT_WKB)) == 0);
	duckdb_free(wkb.data);

	// Valid WKB outside the canonical ranges (POINT (42 1337)) cannot be cast to GEOGRAPHY
	auto out_of_range_val = duckdb_create_blob(POINT_WKB, sizeof(POINT_WKB));
	auto out_of_range_list = duckdb_create_list_value(geog_type, &out_of_range_val, 1);
	REQUIRE(!out_of_range_list);

	duckdb_destroy_value(&out_of_range_val);
	duckdb_destroy_value(&geog_val);
	duckdb_destroy_value(&list_val);
	duckdb_destroy_value(&blob_val);
	duckdb_destroy_logical_type(&geog_type);
}
