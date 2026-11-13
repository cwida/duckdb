#include "catch.hpp"
#include "duckdb/main/os_util.hpp"
#include "test_helpers.hpp"

#include <cstdlib>

using namespace duckdb;
using namespace std;

#ifndef _WIN32
TEST_CASE("Environment variables are read through OSUtil", "[api]") {
	setenv("DUCKDB_TEST_OS_UTIL", "quack", 1);
	{
		DuckDB db(nullptr);
		auto &os_util = OSUtil::Get(*db.instance);
		string value;
		REQUIRE(os_util.GetEnv("DUCKDB_TEST_OS_UTIL", value));
		REQUIRE(value == "quack");
		REQUIRE(os_util.GetEnv("DUCKDB_TEST_OS_UTIL") == "quack");
		REQUIRE(!os_util.GetEnv("DUCKDB_TEST_OS_UTIL_UNSET", value));
		REQUIRE(os_util.GetEnv("DUCKDB_TEST_OS_UTIL_UNSET").empty());

		// the engine's own configuration variables need no database
		REQUIRE(OSUtil::IsConfigurationEnv("HOME"));
		REQUIRE(!OSUtil::IsConfigurationEnv("DUCKDB_TEST_OS_UTIL"));
		REQUIRE(!OSUtil::GetConfigurationEnv("HOME").empty());
		REQUIRE(os_util.GetEnvUnrestricted("HOME", value));
#ifndef DUCKDB_CRASH_ON_ASSERT
		// reading a variable that is not configuration is a programmer error
		REQUIRE_THROWS(OSUtil::GetConfigurationEnv("DUCKDB_TEST_OS_UTIL"));
		REQUIRE_THROWS(os_util.GetEnvUnrestricted("DUCKDB_TEST_OS_UTIL", value));
#endif
	}
	{
		// without external access only configuration variables are readable
		DBConfig config;
		config.SetOptionByName("enable_external_access", Value::BOOLEAN(false));
		// the embedder declares its own configuration variables before the database exists
		config.options.configuration_env.insert("DUCKDB_TEST_OS_UTIL");
		DuckDB db(nullptr, &config);
		auto &os_util = OSUtil::Get(*db.instance);
		string value;
		REQUIRE_THROWS_AS(os_util.GetEnv("DUCKDB_TEST_OS_UTIL", value), PermissionException);
		REQUIRE_THROWS(os_util.GetEnv("DUCKDB_TEST_OS_UTIL"));
		REQUIRE(os_util.GetEnvUnrestricted("DUCKDB_TEST_OS_UTIL", value));
		REQUIRE(value == "quack");
		REQUIRE(os_util.GetEnvUnrestricted("HOME", value));
#ifndef DUCKDB_CRASH_ON_ASSERT
		REQUIRE_THROWS(os_util.GetEnvUnrestricted("DUCKDB_TEST_OS_UTIL_OTHER", value));
#endif
	}
	unsetenv("DUCKDB_TEST_OS_UTIL");
}
#endif
