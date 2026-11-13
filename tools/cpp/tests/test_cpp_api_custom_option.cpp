#include "catch.hpp"
#include "duckdb_cpp.hpp"
#include "duckdb_v2.h"
#include "test_cpp_api.hpp"

#include <string>

// ---------------------------------------------------------------------------
// Stable C++ API tests: CustomOption. Options of our own, read and written
// through a Config and from SQL, with an update callback that validates and
// normalizes the values written.
// ---------------------------------------------------------------------------

namespace {

using namespace duckdb::cxx;

// What the update callback saw, carried in the option's user data.
struct UpdateLog {
	size_t calls = 0;
	SettingScope last_scope = SettingScope::DEFAULT;
	bool last_reset = false;
};

// The log of the registered option, latched so the test can read it after the user data moved into the option.
UpdateLog *registered_log = nullptr;

// Refuses negative values and clamps values above 100.
void ClampUpdate(CustomOption::UpdateInput &input) {
	auto &log = input.GetUserData<UpdateLog>();
	registered_log = &log;
	log.calls++;
	log.last_scope = input.GetScope();
	log.last_reset = input.IsReset();
	auto value = input.GetValue().Get<int64_t>();
	if (value < 0) {
		throw InvalidInputException("cpp_budget cannot be negative");
	}
	if (value > 100) {
		input.SetValue(Value::Create(input.GetContext().GetFactory(), int64_t {100}));
	}
}

auto ReadBudget(Config &config, const std::string &name) -> int64_t {
	return config.GetOption(name).value.Get<int64_t>();
}

} // namespace

TEST_CASE("Stable C++API: custom option is read and written like any other", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();

	auto option = CustomOption::Create(factory);
	option.SetName("cpp_setting")
	    .SetType(factory.ParseType("BIGINT"))
	    .SetDefaultValue(Value::Create(factory, int64_t {5}))
	    .SetDescription("A setting of our own");
	conn.Register(option);

	REQUIRE(ReadBudget(conn.GetConfig(), "cpp_setting") == 5);
	conn.Execute("SET cpp_setting = 7").Drain();
	REQUIRE(ReadBudget(conn.GetConfig(), "cpp_setting") == 7);
	REQUIRE(conn.GetConfig().GetOption("cpp_setting").scope == SettingScope::SESSION);

	auto description = conn.GetConfig().DescribeOption("cpp_setting");
	REQUIRE(description.GetDescription() == "A setting of our own");
	REQUIRE(description.GetDefaultScope() == SettingScope::SESSION);
}

TEST_CASE("Stable C++API: custom option scopes and update callback", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();

	auto option = CustomOption::Create(factory);
	option.SetName("cpp_budget")
	    .SetType(factory.ParseType("BIGINT"))
	    .SetDefaultValue(Value::Create(factory, int64_t {10}))
	    .SetSupportsScope(SettingScope::SESSION, false)
	    .SetUpdateCallback(ClampUpdate)
	    .SetUserData<UpdateLog>();
	conn.Register(option);

	REQUIRE(conn.GetConfig().DescribeOption("cpp_budget").GetDefaultScope() == SettingScope::GLOBAL);

	registered_log = nullptr;
	conn.Execute("SET cpp_budget = 500").Drain();
	REQUIRE(registered_log != nullptr);
	REQUIRE(registered_log->calls == 1);
	REQUIRE(registered_log->last_scope == SettingScope::GLOBAL);
	REQUIRE(ReadBudget(db.GetConfig(), "cpp_budget") == 100);

	REQUIRE_THROWS_MATCHES(conn.Execute("SET cpp_budget = -1").Drain(), Exception,
	                       HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	REQUIRE_THROWS_MATCHES(conn.Execute("SET SESSION cpp_budget = 1").Drain(), Exception,
	                       HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	REQUIRE(ReadBudget(db.GetConfig(), "cpp_budget") == 100);

	conn.Execute("RESET cpp_budget").Drain();
	REQUIRE(registered_log->last_reset);
	REQUIRE(ReadBudget(db.GetConfig(), "cpp_budget") == 10);
}

TEST_CASE("Stable C++API: custom option registration errors", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();

	// No type, and no default value to take one from.
	{
		auto option = CustomOption::Create(factory);
		option.SetName("cpp_untyped");
		REQUIRE_THROWS_MATCHES(conn.Register(option), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	}
	// A built-in name.
	{
		auto option = CustomOption::Create(factory);
		option.SetName("threads").SetType(factory.ParseType("BIGINT"));
		REQUIRE_THROWS_MATCHES(conn.Register(option), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	}
	// DEFAULT is not a scope to configure.
	{
		auto option = CustomOption::Create(factory);
		REQUIRE_THROWS_MATCHES(option.SetDefaultScope(SettingScope::DEFAULT), Exception,
		                       HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	}
}
