#include "catch.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/extension/linked_extension_registry.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "test_helpers.hpp"

#include <atomic>

using namespace duckdb;

static std::atomic<idx_t> database_callback_calls {0};

static void CountDatabaseCallback(DatabaseInstance &) {
	database_callback_calls++;
}

static void NoopCppEntry(void *) {
}

extern "C" {
static int32_t DescribeDatabaseCallback(duckdb_extension_descriptor *descriptor) {
	descriptor->version = 2;
	descriptor->name = "test_database_callback";
	descriptor->database_callback = reinterpret_cast<void (*)(void)>(CountDatabaseCallback);
	return 0;
}

static int32_t DescribeCallbackAndEntry(duckdb_extension_descriptor *descriptor) {
	descriptor->version = 2;
	descriptor->name = "test_callback_and_entry";
	descriptor->database_callback = reinterpret_cast<void (*)(void)>(CountDatabaseCallback);
	descriptor->entry_cpp = reinterpret_cast<void (*)(void)>(NoopCppEntry);
	return 0;
}

static int32_t DescribeCallbackInLayoutOne(duckdb_extension_descriptor *descriptor) {
	descriptor->version = 1;
	descriptor->name = "test_callback_in_layout_one";
	descriptor->database_callback = reinterpret_cast<void (*)(void)>(CountDatabaseCallback);
	return 0;
}
}

TEST_CASE("Static registrations that are not extensions are called back per database", "[api]") {
	REQUIRE(duckdb_register_static_extension(DescribeDatabaseCallback) == 0);

	auto calls_before = database_callback_calls.load();
	DuckDB db(nullptr);
	CHECK(database_callback_calls.load() == calls_before + 1);
	DuckDB other(nullptr);
	CHECK(database_callback_calls.load() == calls_before + 2);

	Connection con(db);
	auto result = con.Query("SELECT count(*) FROM duckdb_extensions() WHERE extension_name = 'test_database_callback'");
	REQUIRE_NO_FAIL(*result);
	CHECK(CHECK_COLUMN(result, 0, {0}));
	CHECK(ExtensionHelper::LoadExtension(db, "test_database_callback") == ExtensionLoadResult::NOT_LOADED);
}

TEST_CASE("Static registrations reject a database callback they cannot use", "[api]") {
	StaticExtensionDescription description;
	auto error = LinkedExtensionRegistry::Describe(DescribeCallbackAndEntry, description);
	CHECK(StringUtil::Contains(error, "set both an entry point and a database callback"));

	error = LinkedExtensionRegistry::Describe(DescribeCallbackInLayoutOne, description);
	CHECK(StringUtil::Contains(error, "did not set an entry point"));
}
