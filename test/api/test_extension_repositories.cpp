#include "catch.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/main/extension.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/main/extension_install_info.hpp"
#include "duckdb/main/extension_repository_manager.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

static constexpr const char *TEST_PUBLIC_KEY =
    "MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAt9+KeDkWJa7SiBb7AMdXAPV2qpRunztoC2c7DtRmN9+Lq2YQm4rMQmmAQcwyxTxcK8vwPK"
    "OiE4mLMwmQMvTty9tL+buREfvg5KJ+FY3Bv05cZYsf7VuYgUDcw41zSEE5W1thh5PiafcrMn7CPF0jN0oFfVvAHB/fAM0yaZGPKs3WYqnHAHa6v/cV"
    "MeiWZ/lQORCBqqL2hii1fEiuIqw6AOEoZVsN0/lqm7GfnD899zrzLOPRuP4YFg46drL1UyNdg+4TquJ2OV9kR4CAfdGRexqHFWLmoxkTfnUxJ4yL+g"
    "kV57K1H+smEdKrDhDX68pEFM/34E2eVKZoRU/0paxDZQIDAQAB";

static constexpr const char *OTHER_PUBLIC_KEY =
    "MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAvTX0K9ihHb3oHcfV+hKS+lcXhWGjRtxGHUkSvMnuTOExsH78a7FcafIF8YcPwRw7cxIK8o"
    "aHIZ0c/DLTR6s9O2qvUzvIU4N6F/rc/ruFJIMSIlN3+CWKxKl/x/unWnnJoza/30zI69n4fuvVHcblGtxLB41tiF3un2Nfs0BrHMVBocEb+vBeozVy"
    "NnZxyKIa9SLy9ZM2/CwsEt/l0uQKwF6dJ+jT11gsitJQuQ4wP18pAx8qamV3Pg6MS3lktd7BlC/Fdm/4Sp68ik971CLYWhkBe3sIzCPbkpwzjeJu52"
    "hE7EOyuB5KaWYpknapwxv5B/MG0y75kRtOnK+L+4PsvQIDAQAB";

static bool KeyIsTrusted(DatabaseInstance &db, ExtensionRepositoryType type, const string &repository_name,
                         const string &compact_key) {
	auto pem_key = ExtensionRepositoryManager::ToPEMPublicKey(compact_key);
	for (auto &key : ExtensionHelper::GetTrustedPublicKeys(db, type, repository_name)) {
		if (key == pem_key) {
			return true;
		}
	}
	return false;
}

TEST_CASE("Test that the public key of a trusted extension repository is trusted", "[api]") {
	auto repository_directory = TestCreatePath("extension_repositories");

	DBConfig config;
	config.SetOptionByName("extension_repository_directory", repository_directory);
	config.SetOptionByName("allow_extension_repositories", "allowed");
	DuckDB db(nullptr, &config);
	Connection con(db);

	auto &instance = *db.instance;
	REQUIRE(!KeyIsTrusted(instance, ExtensionRepositoryType::USER_PROVIDED, "acme", TEST_PUBLIC_KEY));

	REQUIRE_NO_FAIL(con.Query(StringUtil::Format(
	    "CREATE EXTENSION REPOSITORY acme WITH PREFIX 'http://acme.com' USING PUBLIC KEY '%s'", TEST_PUBLIC_KEY)));
	REQUIRE(KeyIsTrusted(instance, ExtensionRepositoryType::USER_PROVIDED, "acme", TEST_PUBLIC_KEY));

	// the key of a user provided repository is never trusted for extensions that come from anywhere else
	REQUIRE(!KeyIsTrusted(instance, ExtensionRepositoryType::CORE, "acme", TEST_PUBLIC_KEY));
	REQUIRE(!KeyIsTrusted(instance, ExtensionRepositoryType::COMMUNITY, "acme", TEST_PUBLIC_KEY));

	// ... nor for extensions that come from a different user provided repository
	REQUIRE_NO_FAIL(con.Query(StringUtil::Format(
	    "CREATE EXTENSION REPOSITORY other WITH PREFIX 'http://other.com' USING PUBLIC KEY '%s'", OTHER_PUBLIC_KEY)));
	REQUIRE(!KeyIsTrusted(instance, ExtensionRepositoryType::USER_PROVIDED, "other", TEST_PUBLIC_KEY));
	REQUIRE(KeyIsTrusted(instance, ExtensionRepositoryType::USER_PROVIDED, "other", OTHER_PUBLIC_KEY));
	REQUIRE(!KeyIsTrusted(instance, ExtensionRepositoryType::USER_PROVIDED, "acme", OTHER_PUBLIC_KEY));

	// an unknown repository name has no trusted keys at all
	REQUIRE(ExtensionHelper::GetTrustedPublicKeys(instance, ExtensionRepositoryType::USER_PROVIDED, "unknown").empty());

	REQUIRE_NO_FAIL(con.Query("DROP EXTENSION REPOSITORY acme"));
	REQUIRE(!KeyIsTrusted(instance, ExtensionRepositoryType::USER_PROVIDED, "acme", TEST_PUBLIC_KEY));
}

TEST_CASE("Test that a repository can trust multiple keys", "[api]") {
	auto repository_directory = TestCreatePath("extension_repositories_multi");

	DBConfig config;
	config.SetOptionByName("extension_repository_directory", repository_directory);
	config.SetOptionByName("allow_extension_repositories", "allowed");
	DuckDB db(nullptr, &config);
	Connection con(db);

	auto &instance = *db.instance;
	// both keys can be provided as a comma separated list, and both of them are trusted
	REQUIRE_NO_FAIL(con.Query(StringUtil::Format(
	    "CREATE EXTENSION REPOSITORY acme WITH PREFIX 'http://acme.com' USING PUBLIC KEYS '%s', '%s'", TEST_PUBLIC_KEY,
	    OTHER_PUBLIC_KEY)));
	REQUIRE(KeyIsTrusted(instance, ExtensionRepositoryType::USER_PROVIDED, "acme", TEST_PUBLIC_KEY));
	REQUIRE(KeyIsTrusted(instance, ExtensionRepositoryType::USER_PROVIDED, "acme", OTHER_PUBLIC_KEY));
}

TEST_CASE("Test that the core and community signing keys are separate", "[api]") {
	DuckDB db(nullptr);

	auto core_keys = ExtensionHelper::GetTrustedPublicKeys(*db.instance, ExtensionRepositoryType::CORE, "core");
	auto community_keys =
	    ExtensionHelper::GetTrustedPublicKeys(*db.instance, ExtensionRepositoryType::COMMUNITY, "community");
	REQUIRE(!core_keys.empty());
	REQUIRE(!community_keys.empty());

	for (auto &community_key : community_keys) {
		for (auto &core_key : core_keys) {
			REQUIRE(core_key != community_key);
		}
	}
}

static idx_t UserRepositoryCount(Connection &con) {
	auto result = con.Query("SELECT count(*) FROM duckdb_extension_repositories() WHERE type='USER_PROVIDED'");
	REQUIRE_NO_FAIL(*result);
	return result->Collection().GetValue(0, 0).GetValue<idx_t>();
}

TEST_CASE("Test that forbidding repositories distrusts their keys", "[api]") {
	auto repository_directory = TestCreatePath("extension_repositories_forbidden");

	DBConfig config;
	config.SetOptionByName("extension_repository_directory", repository_directory);
	config.SetOptionByName("allow_extension_repositories", "allowed");
	DuckDB db(nullptr, &config);
	Connection con(db);

	REQUIRE_NO_FAIL(con.Query(StringUtil::Format(
	    "CREATE EXTENSION REPOSITORY acme WITH PREFIX 'http://acme.com' USING PUBLIC KEY '%s'", TEST_PUBLIC_KEY)));
	REQUIRE(KeyIsTrusted(*db.instance, ExtensionRepositoryType::USER_PROVIDED, "acme", TEST_PUBLIC_KEY));

	// 'forbidden' distrusts the repository entirely: its key is no longer trusted, so loading an extension that was
	// installed from it fails
	REQUIRE_NO_FAIL(con.Query("SET allow_extension_repositories='forbidden'"));
	REQUIRE(!KeyIsTrusted(*db.instance, ExtensionRepositoryType::USER_PROVIDED, "acme", TEST_PUBLIC_KEY));

	// the repository can still be listed and dropped - only its keys are distrusted
	REQUIRE(UserRepositoryCount(con) == 1);
	REQUIRE_NO_FAIL(con.Query("DROP EXTENSION REPOSITORY acme"));
	REQUIRE(UserRepositoryCount(con) == 0);

	// adding a new repository is blocked
	REQUIRE_FAIL(con.Query(StringUtil::Format(
	    "CREATE EXTENSION REPOSITORY other WITH PREFIX 'http://other.com' USING PUBLIC KEY '%s'", OTHER_PUBLIC_KEY)));

	// once forbidden, it cannot be re-enabled or reset to 'undecided' while the database is running
	REQUIRE_FAIL(con.Query("SET allow_extension_repositories='allowed'"));
	REQUIRE_FAIL(con.Query("SET allow_extension_repositories='undecided'"));
}

TEST_CASE("Test that adding trusted extension repositories must be opted into", "[api]") {
	auto repository_directory = TestCreatePath("extension_repositories_undecided");

	// the default is 'undecided': adding a repository fails until the setting is explicitly moved to 'allowed', which
	// can be done at startup or at runtime. Once decided, the setting cannot be reverted to 'undecided'
	{
		DBConfig config;
		config.SetOptionByName("extension_repository_directory", repository_directory);
		DuckDB db(nullptr, &config);
		Connection con(db);

		REQUIRE_FAIL(con.Query(StringUtil::Format(
		    "CREATE EXTENSION REPOSITORY acme WITH PREFIX 'http://acme.com' USING PUBLIC KEY '%s'", TEST_PUBLIC_KEY)));

		// opting in at runtime
		REQUIRE_NO_FAIL(con.Query("SET allow_extension_repositories='allowed'"));
		REQUIRE_NO_FAIL(con.Query(StringUtil::Format(
		    "CREATE EXTENSION REPOSITORY acme WITH PREFIX 'http://acme.com' USING PUBLIC KEY '%s'", TEST_PUBLIC_KEY)));
		REQUIRE(KeyIsTrusted(*db.instance, ExtensionRepositoryType::USER_PROVIDED, "acme", TEST_PUBLIC_KEY));

		// the decision cannot be reverted
		REQUIRE_FAIL(con.Query("SET allow_extension_repositories='undecided'"));
	}

	// the 'undecided' default keeps trusting repositories that were added earlier, but blocks adding new ones
	{
		DBConfig config;
		config.SetOptionByName("extension_repository_directory", repository_directory);
		DuckDB db(nullptr, &config);
		Connection con(db);

		REQUIRE(UserRepositoryCount(con) == 1);
		REQUIRE(KeyIsTrusted(*db.instance, ExtensionRepositoryType::USER_PROVIDED, "acme", TEST_PUBLIC_KEY));
		REQUIRE_FAIL(con.Query(
		    StringUtil::Format("CREATE EXTENSION REPOSITORY other WITH PREFIX 'http://other.com' USING PUBLIC KEY '%s'",
		                       OTHER_PUBLIC_KEY)));
	}

	// opting in at startup through the config also allows adding repositories
	{
		DBConfig config;
		config.SetOptionByName("extension_repository_directory", repository_directory);
		config.SetOptionByName("allow_extension_repositories", "allowed");
		DuckDB db(nullptr, &config);
		Connection con(db);

		REQUIRE_NO_FAIL(con.Query(
		    StringUtil::Format("CREATE EXTENSION REPOSITORY other WITH PREFIX 'http://other.com' USING PUBLIC KEY '%s'",
		                       OTHER_PUBLIC_KEY)));
		REQUIRE(KeyIsTrusted(*db.instance, ExtensionRepositoryType::USER_PROVIDED, "other", OTHER_PUBLIC_KEY));
	}
}

TEST_CASE("Test that the extension repository directory is a startup-only trust anchor", "[api]") {
	auto repository_directory = TestCreatePath("extension_repositories_anchor");

	// with signature checking enabled (the default), the repository directory determines which keys are trusted, so it
	// cannot be repointed at runtime - this stops a connection from pointing it at a directory of attacker keys
	{
		DBConfig config;
		config.SetOptionByName("extension_repository_directory", repository_directory);
		DuckDB db(nullptr, &config);
		Connection con(db);
		REQUIRE_FAIL(con.Query("SET extension_repository_directory='/tmp/some_other_directory'"));
	}

	// when unsigned extensions are allowed the signature-trust model is off, so the directory can be changed freely
	{
		DBConfig config;
		config.SetOptionByName("allow_unsigned_extensions", true);
		DuckDB db(nullptr, &config);
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("SET extension_repository_directory='" + repository_directory + "'"));
	}
}

TEST_CASE("Test which signing keys a load trusts", "[api]") {
	using T = ExtensionRepositoryType;
	// signature (has_from_clause, core_only, recorded_origin)

	// with an explicit FROM, the named origin's keys are the ones trusted
	REQUIRE(ExtensionHelper::ResolveTrustedSignatureOrigin(true, false, T::CORE) == T::CORE);
	REQUIRE(ExtensionHelper::ResolveTrustedSignatureOrigin(true, false, T::COMMUNITY) == T::COMMUNITY);
	REQUIRE(ExtensionHelper::ResolveTrustedSignatureOrigin(true, false, T::USER_PROVIDED) == T::USER_PROVIDED);

	// a plain bare LOAD keeps the community keys for a community extension, but verifies a user-provided origin
	// against the core keys - so a repointed extension_directory cannot load code under a user-provided key
	REQUIRE(ExtensionHelper::ResolveTrustedSignatureOrigin(false, false, T::CORE) == T::CORE);
	REQUIRE(ExtensionHelper::ResolveTrustedSignatureOrigin(false, false, T::COMMUNITY) == T::COMMUNITY);
	REQUIRE(ExtensionHelper::ResolveTrustedSignatureOrigin(false, false, T::USER_PROVIDED) == T::CORE);

	// an autoload targets only core extensions, so nothing but the core keys is trusted - not even community
	REQUIRE(ExtensionHelper::ResolveTrustedSignatureOrigin(false, true, T::CORE) == T::CORE);
	REQUIRE(ExtensionHelper::ResolveTrustedSignatureOrigin(false, true, T::COMMUNITY) == T::CORE);
	REQUIRE(ExtensionHelper::ResolveTrustedSignatureOrigin(false, true, T::USER_PROVIDED) == T::CORE);
}

// Stages the on-disk state an install would produce, writing to the reserved '.duckdb_extension' domain like INSTALL
static void WriteExtensionFile(FileSystem &fs, const string &path, void *data, idx_t size) {
	auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW |
	                                    FileFlags::FILE_FLAGS_ENABLE_EXTENSION_INSTALL);
	handle->Write(data, size);
}

// Stage an extension plus a .info recording a user-provided origin, in the flat layout a bare load resolves. The
// binary is a stub: these tests assert the origin handling, not a real load
static void StageUserRepoExtensionFlat(DatabaseInstance &db, const string &name, const string &repository_name) {
	auto fs = FileSystem::CreateLocal();
	string dir = ExtensionHelper::ExtensionDirectory(db, *fs);
	fs->CreateDirectoriesRecursive(dir);

	string ext_path = fs->JoinPath(dir, name + ".duckdb_extension");
	vector<data_t> stub(ParsedExtensionMetaData::FOOTER_SIZE, 0);
	WriteExtensionFile(*fs, ext_path, stub.data(), stub.size());

	ExtensionInstallInfo info;
	info.mode = ExtensionInstallMode::REPOSITORY;
	info.repository_type = ExtensionRepositoryType::USER_PROVIDED;
	info.repository_name = repository_name;
	info.version = "";
	MemoryStream stream;
	BinarySerializer::Serialize(info, stream);
	WriteExtensionFile(*fs, ext_path + ".info", stream.GetData(), stream.GetPosition());
}

TEST_CASE("Test that a user repository does not block an unsigned load", "[api]") {
	auto repository_directory = TestCreatePath("ext_repo_devflow");
	auto extension_directory = TestCreatePath("ext_repo_devflow_ext");

	// pointing at a build folder to test autoloading is a real workflow: with allow_unsigned_extensions the trust
	// model is off, so a user-provided origin must not impose an extra load barrier
	DBConfig config;
	config.SetOptionByName("extension_repository_directory", repository_directory);
	config.SetOptionByName("extension_directory", extension_directory);
	config.SetOptionByName("allow_extension_repositories", "allowed");
	config.SetOptionByName("allow_unsigned_extensions", true);
	config.SetOptionByName("allow_extensions_metadata_mismatch", true);
	DuckDB db(nullptr, &config);
	Connection con(db);

	REQUIRE_NO_FAIL(con.Query(StringUtil::Format(
	    "CREATE EXTENSION REPOSITORY quackquack WITH PREFIX 'http://quackquack.com' USING PUBLIC KEY '%s'",
	    TEST_PUBLIC_KEY)));
	StageUserRepoExtensionFlat(*db.instance, "some_ext", "quackquack");

	auto bare = con.Query("LOAD some_ext");
	REQUIRE(bare->HasError());
	// it fails on the stub binary, not on a user-provided-origin rule: the dev flow is not blocked
	REQUIRE(!StringUtil::Contains(bare->GetError(), "installed from a user-provided repository"));
}
