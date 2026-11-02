#include "catch.hpp"
#include "duckdb_cpp.hpp"

#include "test_cpp_api.hpp"
#include "test_helpers.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

// ---------------------------------------------------------------------------
// Stable C++ API tests: environment, database options, filesystem, logging,
// exceptions, replacement scans.
// ---------------------------------------------------------------------------

TEST_CASE("Stable C++API: Instance GetOption by name and option target scope", "[cpp_api]") {
	using namespace duckdb::cxx;

	auto instance = Instance(":memory:");

	// Options describe the scopes they may be written at.
	auto option = instance.GetConfig().DescribeOption("allow_community_extensions");
	REQUIRE(option.GetName() == "allow_community_extensions");
	REQUIRE(option.SupportsScope(SettingScope::GLOBAL));
	REQUIRE_FALSE(option.SupportsScope(SettingScope::SESSION));
	REQUIRE(option.GetDefaultScope() == SettingScope::GLOBAL);

	// An alias resolves to its canonical option.
	REQUIRE(instance.GetConfig().DescribeOption("memory_limit").GetName() == "max_memory");

	REQUIRE_THROWS_MATCHES(instance.GetConfig().DescribeOption("no_such_option"), Exception,
	                       HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
}
TEST_CASE("Stable C++API: instances can live in an explicit environment", "[cpp_api]") {
	using namespace duckdb::cxx;

	const auto default_before = Environment::Default().GetInstanceCount();
	{
		Environment env;
		REQUIRE(env.GetInstanceCount() == 0);
		{
			auto empty = Instance(env);
			auto opened = Instance(env, ":memory:", StartupOptions().EnableUnsignedExtensions(false));
			REQUIRE(env.GetInstanceCount() == 2);
			// They are not counted in the default environment.
			REQUIRE(Environment::Default().GetInstanceCount() == default_before);

			auto conn = opened.Connect();
			auto result = conn.Execute("SELECT 42");
			auto chunk = result.FetchChunk();
			REQUIRE(chunk.GetRowCount() == 1);
		}
		REQUIRE(env.GetInstanceCount() == 0);
	}
	// An instance opened without an environment lands in the default one.
	{
		auto instance = Instance(":memory:");
		REQUIRE(Environment::Default().GetInstanceCount() == default_before + 1);
	}
	REQUIRE(Environment::Default().GetInstanceCount() == default_before);
}

TEST_CASE("Stable C++API: options can be enumerated with their metadata", "[cpp_api]") {
	using namespace duckdb::cxx;

	// The count is shared with every other test: compare against what was alive before.
	const auto alive_before = Environment::Default().GetInstanceCount();
	{
		auto counted = Instance(":memory:");
		REQUIRE(Environment::Default().GetInstanceCount() == alive_before + 1);
	}
	REQUIRE(Environment::Default().GetInstanceCount() == alive_before);
	auto instance = Instance(":memory:");

	REQUIRE(instance.GetConfig().GetOptionCount() > 0);
	bool found_memory_limit = false;
	for (size_t i = 0; i < instance.GetConfig().GetOptionCount(); i++) {
		auto option = instance.GetConfig().DescribeOption(i);
		if (option.GetName() != "max_memory") {
			continue;
		}
		found_memory_limit = true;
		REQUIRE_FALSE(option.GetDescription().empty());
		REQUIRE(option.GetAliasCount() > 0);
		bool found_alias = false;
		for (size_t alias_idx = 0; alias_idx < option.GetAliasCount(); alias_idx++) {
			found_alias |= option.GetAliasByIndex(alias_idx) == "memory_limit";
		}
		REQUIRE(found_alias);
	}
	REQUIRE(found_memory_limit);
	REQUIRE_FALSE(instance.GetConfig().DescribeOption("allow_community_extensions").GetDefaultValue().IsNull());

	auto conn = instance.Connect();
	REQUIRE(conn.GetConfig().GetOptionCount() == instance.GetConfig().GetOptionCount());
	REQUIRE_FALSE(conn.GetConfig().DescribeOption(0).GetName().empty());
}

TEST_CASE("Stable C++API: Instance SetDefault selects the database for new connections", "[cpp_api]") {
	using namespace duckdb::cxx;

	auto first_path = duckdb::TestCreatePath("cpp_api_default_first.duckdb");
	auto second_path = duckdb::TestCreatePath("cpp_api_default_second.duckdb");
	duckdb::DeleteDatabase(first_path);
	duckdb::DeleteDatabase(second_path);

	{
		auto instance = Instance();
		instance.Attach(first_path, "first", {}, true);
		instance.Attach(second_path, "second", {});
		instance.SetDefault("second");

		auto conn = instance.Connect();
		auto result = conn.Execute("SELECT current_database()");
		REQUIRE(result.FetchChunk().GetVector(0).GetValue(0).Get<varchar_t>().view() == "second");
	}
	duckdb::DeleteDatabase(first_path);
	duckdb::DeleteDatabase(second_path);
}
TEST_CASE("Stable C++API: RenderQuotedIdentifier quotes only when required", "[cpp_api]") {
	using duckdb::cxx::RenderQuotedIdentifier;
	REQUIRE(RenderQuotedIdentifier("col") == "col");
	REQUIRE(RenderQuotedIdentifier("MyCol") == "MyCol");
	REQUIRE(RenderQuotedIdentifier("select") == "\"select\"");
	REQUIRE(RenderQuotedIdentifier("my col") == "\"my col\"");
	REQUIRE(RenderQuotedIdentifier("a\"b") == "\"a\"\"b\"");
}

TEST_CASE("Stable C++API: LibraryVersion reports the engine version", "[cpp_api]") {
	const auto version = duckdb::cxx::LibraryVersion();
	REQUIRE_FALSE(version.empty());

	// The engine agrees; the C entry point reports the same text as a borrowed view.
	duckdb_v2_str raw = {nullptr, 0};
	REQUIRE(duckdb_v2_library_version(&raw, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(version == std::string(raw.ptr, raw.len));
}

TEST_CASE("Stable C++API: Exception carries the code and message body", "[cpp_api]") {
	using namespace duckdb::cxx;

	auto instance = Instance(":memory:");
	auto conn = instance.Connect();

	// Binder error: GetCode() is the identity, GetRawMessage() the unprefixed body.
	try {
		conn.Execute("SELECT * FROM no_such_table");
		FAIL("expected a Catalog error");
	} catch (const Exception &ex) {
		REQUIRE(ex.GetCode() == DUCKDB_V2_ERROR_DATABASE_CATALOG);
		REQUIRE(std::string(ex.GetRawMessage()).find("no_such_table") != std::string::npos);
		REQUIRE(std::string(ex.GetRawMessage()).rfind("Catalog Error:", 0) != 0);
		// what() is the full prefixed message and contains the body.
		REQUIRE(std::string(ex.what()).rfind("Catalog Error:", 0) == 0);
		REQUIRE(std::string(ex.what()).find(ex.GetRawMessage()) != std::string::npos);
	}

	// Parse error surfaces lazily: ParseSQL only sets up the iterator, the first
	// Next() yields "SELECT 1", and the parse error for "SELEKT 2" surfaces from the
	// Next() that reaches it. Same shape: Parser code, unprefixed body.
	try {
		auto iter = conn.ParseSQL("SELECT 1; SELEKT 2");
		REQUIRE(iter.Next());
		iter.Next();
		FAIL("expected a Parser error");
	} catch (const Exception &ex) {
		REQUIRE(ex.GetCode() == DUCKDB_V2_ERROR_QUERY_PARSER);
		REQUIRE(std::string(ex.GetRawMessage()).rfind("Parser Error:", 0) != 0);
	}
}
TEST_CASE("Stable C++API: Connection::SetOption scope split is visible correctly across connections", "[cpp_api]") {
	using namespace duckdb::cxx;

	auto instance = Instance(":memory:");
	auto conn_a = instance.Connect();
	auto conn_b = instance.Connect();

	// A SESSION write on conn_a stays invisible to conn_b, and reads back as SESSION.
	conn_a.GetConfig().SetOption("max_execution_time", "5000", SettingScope::SESSION);
	auto seen = conn_a.GetConfig().GetOption("max_execution_time");
	REQUIRE(seen.value.ToText() == "5000");
	REQUIRE(seen.scope == SettingScope::SESSION);
	REQUIRE(conn_b.GetConfig().GetOption("max_execution_time").value.ToText() != "5000");

	// A GLOBAL write on conn_a is visible identically on conn_b, and on the instance.
	conn_a.GetConfig().SetOption("memory_limit", "987MB", SettingScope::GLOBAL);
	auto seen_a = conn_a.GetConfig().GetOption("memory_limit");
	auto seen_b = conn_b.GetConfig().GetOption("memory_limit");
	REQUIRE(seen_a.value.ToText() == seen_b.value.ToText());
	auto seen_instance = instance.GetConfig().GetOption("memory_limit");
	REQUIRE(seen_instance.value.ToText() == seen_a.value.ToText());
	REQUIRE(seen_instance.scope == SettingScope::GLOBAL);

	// A GLOBAL-only option rejects a SESSION write.
	REQUIRE_THROWS_MATCHES(conn_a.GetConfig().SetOption("allow_community_extensions", "false", SettingScope::SESSION),
	                       Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
}
TEST_CASE("Stable C++API: Instance and Connection write to the log", "[cpp_api]") {
	using namespace duckdb::cxx;

	auto instance = Instance(":memory:");
	auto conn = instance.Connect();
	conn.Execute("SET enable_logging = true").Drain();
	instance.Log(LogLevel::LOG_INFO, "cpp instance log");
	conn.Log(LogLevel::LOG_INFO, "cpp connection log");
	auto result = conn.Execute("SELECT count(*) FROM duckdb_logs WHERE (message = 'cpp instance log' AND scope = "
	                           "'DATABASE') OR (message = 'cpp connection log' AND scope = 'CONNECTION')");
	REQUIRE(result.FetchChunk().GetVector(0).GetValue(0).Get<int64_t>() == 2);
}

TEST_CASE("Stable C++API: Connection options and the scopeless SetOption default", "[cpp_api]") {
	using namespace duckdb::cxx;

	auto instance = Instance(":memory:");
	auto conn = instance.Connect();

	auto option = conn.GetConfig().DescribeOption("allow_community_extensions");
	REQUIRE(option.GetName() == "allow_community_extensions");

	// The scopeless overload writes the option's default scope (SQL `SET` semantics): the session here.
	conn.GetConfig().SetOption("max_execution_time", "4242");
	auto seen = conn.GetConfig().GetOption("max_execution_time");
	REQUIRE(seen.value.ToText() == "4242");
	REQUIRE(seen.scope == SettingScope::SESSION);

	// A context writes too, at any scope.
	conn.GetConfig().SetOption("max_execution_time", "77", SettingScope::GLOBAL);
	REQUIRE(instance.GetConfig().GetOption("max_execution_time").value.ToText() == "77");

	REQUIRE_THROWS_MATCHES(conn.GetConfig().DescribeOption("no_such_option_xyz"), Exception,
	                       HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
}
TEST_CASE("Stable C++API: Instance::Attach with a name and options", "[cpp_api]") {
	using namespace duckdb::cxx;

	auto path = duckdb::TestCreatePath("cpp_api_attach_options.duckdb");
	duckdb::DeleteDatabase(path);

	auto instance = Instance();
	instance.Attach(":memory:", true);
	instance.Attach(path, "named", {{"BLOCK_SIZE", "16384"}});
	auto conn = instance.Connect();
	conn.Execute("CREATE TABLE named.t(i INTEGER)").Drain();
	{
		auto result = conn.Execute("SELECT block_size FROM pragma_database_size() WHERE database_name = 'named'");
		REQUIRE(result.FetchChunk().GetVector(0).GetValue(0).Get<int64_t>() == 16384);
	}
	instance.Detach("named");

	// Re-attaching read-only, and as the default for later sessions.
	instance.Attach(path, "named", {{"READ_ONLY", "true"}}, true);
	auto later = instance.Connect();
	REQUIRE_THROWS_AS(later.Execute("INSERT INTO t VALUES (1)"), Exception);
	later.Execute("SELECT * FROM t").Drain();

	// An option the engine rejects fails the attach, not the option.
	REQUIRE_THROWS_AS(instance.Attach(":memory:", "other", {{"no_such_attach_option", "1"}}), Exception);

	duckdb::DeleteDatabase(path);
}

TEST_CASE("Stable C++API: a database attached read-only refuses writes", "[cpp_api]") {
	using namespace duckdb::cxx;

	auto path = duckdb::TestCreatePath("cpp_api_readonly.duckdb");
	duckdb::DeleteDatabase(path);

	{
		// Seed the database, then close (scope exit) to free the exclusive-open
		// slot for the read-only reopen.
		auto instance = Instance(path);
		auto conn = instance.Connect();
		conn.Execute("CREATE TABLE t(i INTEGER)").Drain();
		conn.Execute("INSERT INTO t VALUES (1), (2)").Drain();
	}

	{
		auto ro_instance = Instance();
		ro_instance.Attach(path, "", {{"READ_ONLY", "true"}}, true);
		auto ro_conn = ro_instance.Connect();

		// Reads see the seeded data. Scoped so the live result is released
		// before the write attempts below.
		{
			auto result = ro_conn.Execute("SELECT count(*) FROM t");
			auto chunk = result.FetchChunk();
			REQUIRE(chunk.GetVector(0).GetValue(0).Get<int64_t>() == 2);
		}

		// Writes are rejected: both DML and DDL.
		REQUIRE_THROWS_AS(ro_conn.Execute("INSERT INTO t VALUES (3)"), Exception);
		REQUIRE_THROWS_AS(ro_conn.Execute("CREATE TABLE u(i INTEGER)"), Exception);

		// The data is unchanged after the rejected write attempts.
		auto after = ro_conn.Execute("SELECT count(*) FROM t");
		REQUIRE(after.FetchChunk().GetVector(0).GetValue(0).Get<int64_t>() == 2);
	}

	duckdb::DeleteDatabase(path);
}
TEST_CASE("Stable C++API: typed exceptions carry their error code", "[cpp_api]") {
	using namespace duckdb::cxx;

	// Each typed exception fixes its code in the implementation; throwing one
	// is how a callback names its error class without any code vocabulary.
	REQUIRE(InvalidInputException("boom").GetCode() == static_cast<uint32_t>(DUCKDB_V2_ERROR_INPUT_INVALID));
	REQUIRE(InterruptException("stop").GetCode() == static_cast<uint32_t>(DUCKDB_V2_ERROR_RUNTIME_INTERRUPT));

	// They are catchable through the Exception base, preserving the code.
	try {
		throw InvalidInputException("bad arg");
	} catch (const Exception &caught) {
		REQUIRE(caught.GetCode() == static_cast<uint32_t>(DUCKDB_V2_ERROR_INPUT_INVALID));
	}

	// The base Exception with a raw code still works.
	Exception raw(static_cast<uint32_t>(DUCKDB_V2_ERROR_QUERY_BINDER), "parse boom");
	REQUIRE(raw.GetCode() == static_cast<uint32_t>(DUCKDB_V2_ERROR_QUERY_BINDER));

	// A thrown-and-caught engine error classifies back correctly end to end.
	auto instance = Instance(":memory:");
	auto conn = instance.Connect();
	try {
		conn.Execute("SELECT * FROM no_such_table_xyz");
		FAIL("expected a Catalog error");
	} catch (const Exception &caught) {
		REQUIRE(caught.GetCode() == static_cast<uint32_t>(DUCKDB_V2_ERROR_DATABASE_CATALOG));
	}
}

TEST_CASE("Stable C++API: Config reads, writes and describes settings", "[cpp_api]") {
	using namespace duckdb::cxx;

	auto instance = Instance(":memory:");
	auto conn = instance.Connect();
	auto &config = conn.GetConfig();

	// A value destructures into the value and the scope it came from.
	config.SetOption("max_execution_time", "5000", SettingScope::SESSION);
	auto [value, scope] = config.GetOption("max_execution_time");
	REQUIRE(value.ToText() == "5000");
	REQUIRE(scope == SettingScope::SESSION);

	// operator[] is GetOption.
	REQUIRE(config["max_execution_time"].value.ToText() == "5000");

	// A typed value writes as well as text does.
	config.SetOption("max_execution_time", conn.GetFactory().CreateValue(int64_t(77)), SettingScope::SESSION);
	REQUIRE(config["max_execution_time"].value.ToText() == "77");

	// TryGetOption reports an unknown name rather than throwing.
	REQUIRE(config.TryGetOption("no_such_option_xyz") == std::nullopt);
	REQUIRE(config.TryGetOption("max_execution_time").has_value());
	REQUIRE_THROWS_MATCHES(config.GetOption("no_such_option_xyz"), Exception,
	                       HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));

	// The three configs of one database describe the same settings.
	REQUIRE(instance.GetConfig().GetOptionCount() == config.GetOptionCount());
	REQUIRE(conn.GetConfig().GetOptionCount() == config.GetOptionCount());
	REQUIRE(instance.GetConfig().DescribeOption("max_memory").GetName() == "max_memory");

	// An instance's config is GLOBAL alone.
	REQUIRE_THROWS_MATCHES(instance.GetConfig().SetOption("max_execution_time", "1", SettingScope::SESSION), Exception,
	                       HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));

	// A const source hands out a config that reads but does not write.
	const auto &const_conn = conn;
	REQUIRE(const_conn.GetConfig()["max_execution_time"].value.ToText() == "77");
}

TEST_CASE("Stable C++API: StartupOptions reach the instance's settings", "[cpp_api]") {
	using namespace duckdb::cxx;

	auto instance = Instance(":memory:", StartupOptions()
	                                         .EnableCommunityExtensions(false)
	                                         .EnableUnsignedExtensions(true)
	                                         .EnableDatabaseInvalidation(false)
	                                         .EnableUnredactedSecrets(true)
	                                         .SetCustomUserAgent("cpp_api_test"));
	auto &config = instance.GetConfig();
	REQUIRE(config.GetOption("allow_community_extensions").value.ToText() == "false");
	REQUIRE(config.GetOption("allow_unsigned_extensions").value.ToText() == "true");
	REQUIRE(config.GetOption("disable_database_invalidation").value.ToText() == "true");
	REQUIRE(config.GetOption("allow_unredacted_secrets").value.ToText() == "true");
	REQUIRE(config.GetOption("custom_user_agent").value.ToText() == "cpp_api_test");

	// Without a path, nothing is attached.
	auto empty = Instance(StartupOptions().EnableUnsignedExtensions(true));
	REQUIRE(empty.GetConfig().GetOption("allow_unsigned_extensions").value.ToText() == "true");
	REQUIRE_THROWS_AS(empty.Connect().Execute("CREATE TABLE t(i INTEGER)"), Exception);

	// Unset options keep their defaults.
	auto defaults = Instance(":memory:");
	REQUIRE(defaults.GetConfig().GetOption("allow_unsigned_extensions").value.ToText() == "false");
	REQUIRE(defaults.GetConfig().GetOption("disable_database_invalidation").value.ToText() == "false");
}
