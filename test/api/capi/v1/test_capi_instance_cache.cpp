#include "capi_tester.hpp"
#include "duckdb.h"
#include "test_helpers.hpp"

#include <chrono>
#include <thread>

using namespace duckdb;

static void background_thread_connect(duckdb_instance_cache instance_cache, const char *path) {
	try {
		duckdb_database out_database;
		auto state = duckdb_get_or_create_from_cache(instance_cache, path, &out_database, nullptr, nullptr);
		REQUIRE(state == DuckDBSuccess);
		duckdb_close(&out_database);
	} catch (std::exception &ex) {
		FAIL(ex.what());
	}
}

TEST_CASE("Test the database instance cache in the C API", "[api][.]") {
	auto instance_cache = duckdb_create_instance_cache();

	for (idx_t i = 0; i < 30; i++) {
		auto path = TestCreatePath("shared_db.db");

		duckdb_database shared_out_database;
		auto state =
		    duckdb_get_or_create_from_cache(instance_cache, path.c_str(), &shared_out_database, nullptr, nullptr);
		REQUIRE(state == DuckDBSuccess);

		std::thread background_thread(background_thread_connect, instance_cache, path.c_str());
		duckdb_close(&shared_out_database);
		background_thread.join();
		TestDeleteFile(path);
		REQUIRE(1);
	}

	duckdb_destroy_instance_cache(&instance_cache);
}

TEST_CASE("Test the database instance cache in the C API with a connection that outlives the database", "[capi]") {
	auto instance_cache = duckdb_create_instance_cache();
	auto path = TestCreatePath("instance_cache_in_use.db");
	duckdb_database db;
	REQUIRE(duckdb_get_or_create_from_cache(instance_cache, path.c_str(), &db, nullptr, nullptr) == DuckDBSuccess);
	duckdb_connection con;
	REQUIRE(duckdb_connect(db, &con) == DuckDBSuccess);
	REQUIRE(duckdb_query(con, "CREATE TABLE t AS SELECT 42 AS i", nullptr) == DuckDBSuccess);
	duckdb_close(&db);

	duckdb_database reopened;
	REQUIRE(duckdb_get_or_create_from_cache(instance_cache, path.c_str(), &reopened, nullptr, nullptr) ==
	        DuckDBSuccess);
	duckdb_connection reopened_con;
	REQUIRE(duckdb_connect(reopened, &reopened_con) == DuckDBSuccess);
	duckdb_result result;
	REQUIRE(duckdb_query(reopened_con, "SELECT i FROM t", &result) == DuckDBSuccess);
	REQUIRE(duckdb_value_int32(&result, 0, 0) == 42);
	duckdb_destroy_result(&result);
	duckdb_disconnect(&reopened_con);
	duckdb_close(&reopened);

	// the first connection outlives both handles
	REQUIRE(duckdb_query(con, "INSERT INTO t VALUES (43)", nullptr) == DuckDBSuccess);
	duckdb_disconnect(&con);
	duckdb_destroy_instance_cache(&instance_cache);
	TestDeleteFile(path);
}

TEST_CASE("Test the database instance cache in the C API with a null path", "[capi]") {
	auto instance_cache = duckdb_create_instance_cache();
	duckdb_database db;
	auto state = duckdb_get_or_create_from_cache(instance_cache, nullptr, &db, nullptr, nullptr);
	REQUIRE(state == DuckDBSuccess);
	duckdb_close(&db);
	duckdb_destroy_instance_cache(&instance_cache);
}

TEST_CASE("Test the database instance cache in the C API with an empty path", "[capi]") {
	auto instance_cache = duckdb_create_instance_cache();
	duckdb_database db;
	auto state = duckdb_get_or_create_from_cache(instance_cache, "", &db, nullptr, nullptr);
	REQUIRE(state == DuckDBSuccess);
	duckdb_close(&db);
	duckdb_destroy_instance_cache(&instance_cache);
}

TEST_CASE("Test the database instance cache in the C API with a memory path", "[capi]") {
	auto instance_cache = duckdb_create_instance_cache();
	duckdb_database db;
	auto state = duckdb_get_or_create_from_cache(instance_cache, ":memory:", &db, nullptr, nullptr);
	REQUIRE(state == DuckDBSuccess);
	duckdb_close(&db);
	duckdb_destroy_instance_cache(&instance_cache);
}

TEST_CASE("Test the database instance cache with case-insensitive FS", "[capi]") {
	// Check that file on disk is created in user-specified case
	// independently whether FS is case-sensitive or not.
	// FS access can be eventually changed to CAPI FS.
	duckdb::unique_ptr<FileSystem> fs = FileSystem::CreateLocal();
	string work_dir = TestDirectoryPath();
	auto instance_cache = duckdb_create_instance_cache();

	std::vector<string> db_file_list = {"Test1.db", "TEST2.db", "TeSt3.db"};
	for (const string &db_file : db_file_list) {
		string db_path = TestJoinPath(work_dir, db_file);
		string attached_db_path = TestJoinPath(work_dir, "attached_" + db_file);
		fs->TryRemoveFile(db_path);
		fs->TryRemoveFile(attached_db_path);

		duckdb_database db1 = nullptr;
		REQUIRE(duckdb_get_or_create_from_cache(instance_cache, db_path.c_str(), &db1, nullptr, nullptr) ==
		        DuckDBSuccess);
		duckdb_connection conn1 = nullptr;
		REQUIRE(duckdb_connect(db1, &conn1) == DuckDBSuccess);
		REQUIRE(duckdb_query(conn1, (string("ATTACH '") + attached_db_path + "' AS attached").c_str(), nullptr) ==
		        DuckDBSuccess);
		REQUIRE(duckdb_query(conn1, "CREATE TABLE attached.tab1 (col1 INT)", nullptr) == DuckDBSuccess);

		// Check the file with originally specified case exists in FS
		bool found = false;
		fs->ListFiles(work_dir, [&db_file, &found](const string &name, bool) {
			if (name == db_file) {
				found = true;
			}
		});
		REQUIRE(found);

		// Get the DB from cache using different case and check that instance is the same
		// (when the FS and instance cache are case-insensitive) and different (when case-sensitive)
		string db_file_lower = db_file;
		std::transform(db_file_lower.begin(), db_file_lower.end(), db_file_lower.begin(),
		               [](unsigned char c) { return tolower(c); });
		string db_path_lower = TestJoinPath(work_dir, db_file_lower);
		fs->TryRemoveFile(db_path_lower);
		duckdb_database db2 = nullptr;
		REQUIRE(duckdb_get_or_create_from_cache(instance_cache, db_path_lower.c_str(), &db2, nullptr, nullptr) ==
		        DuckDBSuccess);
		duckdb_connection conn2 = nullptr;
		REQUIRE(duckdb_connect(db2, &conn2) == DuckDBSuccess);
		duckdb_result result;
		memset(&result, '\0', sizeof(result));
		REQUIRE(duckdb_query(conn2, "SELECT COUNT(*) FROM information_schema.tables WHERE table_name = 'tab1'",
		                     &result) == DuckDBSuccess);
		duckdb_data_chunk chunk = duckdb_fetch_chunk(result);
		REQUIRE(chunk != nullptr);
		duckdb_vector vec = duckdb_data_chunk_get_vector(chunk, 0);
		REQUIRE(vec != nullptr);
		int32_t *data = reinterpret_cast<int32_t *>(duckdb_vector_get_data(vec));
		int32_t count = data[0];
#if defined(_WIN32) || defined(__APPLE__) // case insensitive, attached
		REQUIRE(count == 1);
#else  // !(_WIN32 or __APPLE__): case sensitive, not attached
		REQUIRE(count == 0);
#endif // _WIN32 or __APPLE__
       // Cleanup
		duckdb_destroy_data_chunk(&chunk);
		duckdb_destroy_result(&result);
		duckdb_disconnect(&conn2);
		duckdb_close(&db2);
		duckdb_disconnect(&conn1);
		duckdb_close(&db1);
		if (count == 0) {
			fs->TryRemoveFile(db_path_lower);
		}
		fs->TryRemoveFile(db_path);
		fs->TryRemoveFile(attached_db_path);
	}
	duckdb_destroy_instance_cache(&instance_cache);
}
