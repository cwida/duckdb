#include "catch.hpp"
#include "duckdb/common/file_system.hpp"
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

	// once forbidden, it cannot be re-enabled or reset to 'hosts' while the database is running
	REQUIRE_FAIL(con.Query("SET allow_extension_repositories='allowed'"));
	REQUIRE_FAIL(con.Query("SET allow_extension_repositories='hosts'"));
}

TEST_CASE("Test that adding trusted extension repositories must be opted into", "[api]") {
	auto repository_directory = TestCreatePath("extension_repositories_hosts");
	auto local_prefix = TestCreatePath("extension_repositories_hosts_prefix");

	// the default is 'hosts': adding a repository by prefix fails until the setting is explicitly moved to 'allowed',
	// which can be done at startup or at runtime. Once moved, the setting cannot be reverted to 'hosts'
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
		REQUIRE_NO_FAIL(
		    con.Query(StringUtil::Format("CREATE EXTENSION REPOSITORY local WITH PREFIX '%s' USING PUBLIC KEY '%s'",
		                                 local_prefix, TEST_PUBLIC_KEY)));

		// the decision cannot be reverted
		REQUIRE_FAIL(con.Query("SET allow_extension_repositories='hosts'"));
	}

	// the 'hosts' default keeps trusting repositories that were added earlier, but blocks adding new ones
	{
		DBConfig config;
		config.SetOptionByName("extension_repository_directory", repository_directory);
		DuckDB db(nullptr, &config);
		Connection con(db);

		REQUIRE(UserRepositoryCount(con) == 2);
		REQUIRE(KeyIsTrusted(*db.instance, ExtensionRepositoryType::USER_PROVIDED, "acme", TEST_PUBLIC_KEY));
		REQUIRE_FAIL(con.Query(
		    StringUtil::Format("CREATE EXTENSION REPOSITORY other WITH PREFIX 'http://other.com' USING PUBLIC KEY '%s'",
		                       OTHER_PUBLIC_KEY)));

		// installing from a trusted repository is not gated by the opt-in: the (empty) repository is consulted
		auto install = con.Query("INSTALL some_extension FROM local");
		REQUIRE(install->HasError());
		REQUIRE(install->GetErrorObject().Type() == ExceptionType::IO);
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

static void WriteTextFile(const string &path, const string &contents) {
	auto fs = FileSystem::CreateLocal();
	auto handle = fs->OpenFile(path, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW);
	handle->Write(QueryContext(), (void *)contents.data(), contents.size(), 0);
	handle->Sync();
}

TEST_CASE("Test that a stored repository file is only believed about a host it is named after", "[api]") {
	auto repository_directory = TestCreatePath("extension_repositories_host_mismatch");
	auto prefix = TestCreatePath("extension_repositories_host_mismatch_prefix");
	auto fs = FileSystem::CreateLocal();
	fs->CreateDirectory(repository_directory);
	fs->CreateDirectory(prefix);
	// a file that claims a host it is not named after was not written by DuckDB: under 'hosts_only' a stored host is
	// what gets a repository trusted, so the claim is refused rather than believed
	WriteTextFile(fs->JoinPath(repository_directory, "mismatch.example.duckdb_extension.repo.json"),
	              StringUtil::Format("{\"version\": 1, \"name\": \"mismatch.example\", \"url\": \"%s\", \"host\": "
	                                 "\"other.example\", \"public_keys\": [\"%s\"]}",
	                                 prefix, TEST_PUBLIC_KEY));

	DBConfig config;
	config.SetOptionByName("extension_repository_directory", repository_directory);
	config.SetOptionByName("autoload_known_extensions", false);
	DuckDB db(nullptr, &config);
	Connection con(db);

	auto result = con.Query("SELECT repository_name FROM duckdb_extension_repositories() WHERE type='USER_PROVIDED'");
	REQUIRE(result->HasError());
	REQUIRE(StringUtil::Contains(result->GetError(), "names host 'other.example'"));
	// reading the file fails, so the key is not trusted either way
	bool trusted = false;
	try {
		trusted =
		    KeyIsTrusted(*db.instance, ExtensionRepositoryType::USER_PROVIDED, "mismatch.example", TEST_PUBLIC_KEY);
	} catch (std::exception &) {
	}
	REQUIRE(!trusted);
}

TEST_CASE("Test that a repository resolved from its host does not need the opt-in", "[api]") {
	auto repository_directory = TestCreatePath("extension_repositories_from_host");
	auto prefix = TestCreatePath("extension_repositories_from_host_prefix");
	auto fs = FileSystem::CreateLocal();
	fs->CreateDirectory(repository_directory);
	fs->CreateDirectory(prefix);
	// a repository as the host form stores it: resolving one needs httpfs and a live host, which this test has not,
	// so the stored file stands in for the resolution.
	// TODO: resolve one for real once duckdb.org serves a .well-known/duckdb-extension-repo.json
	WriteTextFile(fs->JoinPath(repository_directory, "listed.example.duckdb_extension.repo.json"),
	              StringUtil::Format("{\"version\": 1, \"name\": \"listed.example\", \"url\": \"%s\", \"host\": "
	                                 "\"listed.example\", \"public_keys\": [\"%s\"]}",
	                                 prefix, TEST_PUBLIC_KEY));

	DBConfig config;
	config.SetOptionByName("extension_repository_directory", repository_directory);
	config.SetOptionByName("autoload_known_extensions", false);
	DuckDB db(nullptr, &config);
	Connection con(db);

	// 'hosts' (the default): a repository by prefix is refused, the one resolved from its host is trusted
	REQUIRE_FAIL(con.Query(StringUtil::Format("CREATE EXTENSION REPOSITORY by_prefix WITH PREFIX '%s'", prefix)));
	REQUIRE(KeyIsTrusted(*db.instance, ExtensionRepositoryType::USER_PROVIDED, "listed.example", TEST_PUBLIC_KEY));
	REQUIRE(UserRepositoryCount(con) == 1);

	// the conflict is decided before the host is contacted: IF NOT EXISTS returns the stored definition, a plain CREATE
	// reports the duplicate; OR REPLACE does resolve, fails here (no httpfs), and keeps the old definition
	REQUIRE_NO_FAIL(con.Query("CREATE EXTENSION REPOSITORY IF NOT EXISTS listed.example"));
	REQUIRE_FAIL(con.Query("CREATE EXTENSION REPOSITORY listed.example"));
	REQUIRE_FAIL(con.Query("CREATE OR REPLACE EXTENSION REPOSITORY listed.example"));
	REQUIRE_FAIL(con.Query("CREATE EXTENSION REPOSITORY unlisted.example"));
	REQUIRE(KeyIsTrusted(*db.instance, ExtensionRepositoryType::USER_PROVIDED, "listed.example", TEST_PUBLIC_KEY));

	// an empty prefix does not reopen the host form with keys of the caller's choosing: refused at parse, and refused
	// by the manager when handed in through the API; the pinned key stays
	REQUIRE_FAIL(con.Query(
	    StringUtil::Format("CREATE OR REPLACE EXTENSION REPOSITORY listed.example WITH PREFIX '' USING PUBLIC KEY '%s'",
	                       OTHER_PUBLIC_KEY)));
	{
		ExtensionRepository requested("listed.example", "", {OTHER_PUBLIC_KEY});
		REQUIRE_THROWS(ExtensionRepositoryManager::CreateRepository(*db.instance, FileSystem::GetLocal(*db.instance),
		                                                            con.context.get(), requested,
		                                                            OnCreateConflict::REPLACE_ON_CONFLICT));
	}
	REQUIRE(KeyIsTrusted(*db.instance, ExtensionRepositoryType::USER_PROVIDED, "listed.example", TEST_PUBLIC_KEY));
	REQUIRE(!KeyIsTrusted(*db.instance, ExtensionRepositoryType::USER_PROVIDED, "listed.example", OTHER_PUBLIC_KEY));

	// 'hosts_only' keeps trusting the repository resolved from its host, 'forbidden' forbids the form as well
	REQUIRE_NO_FAIL(con.Query("SET allow_extension_repositories='hosts_only'"));
	REQUIRE(KeyIsTrusted(*db.instance, ExtensionRepositoryType::USER_PROVIDED, "listed.example", TEST_PUBLIC_KEY));
	REQUIRE_NO_FAIL(con.Query("DROP EXTENSION REPOSITORY listed.example"));
	REQUIRE_NO_FAIL(con.Query("SET allow_extension_repositories='forbidden'"));
	REQUIRE_FAIL(con.Query("CREATE EXTENSION REPOSITORY listed.example"));
}

TEST_CASE("Test that 'hosts_only' distrusts the repositories added by prefix", "[api]") {
	auto repository_directory = TestCreatePath("extension_repositories_hosts_only");

	DBConfig config;
	config.SetOptionByName("extension_repository_directory", repository_directory);
	config.SetOptionByName("allow_extension_repositories", "allowed");
	DuckDB db(nullptr, &config);
	Connection con(db);

	REQUIRE_NO_FAIL(con.Query(StringUtil::Format(
	    "CREATE EXTENSION REPOSITORY acme WITH PREFIX 'http://acme.com' USING PUBLIC KEY '%s'", TEST_PUBLIC_KEY)));
	REQUIRE(KeyIsTrusted(*db.instance, ExtensionRepositoryType::USER_PROVIDED, "acme", TEST_PUBLIC_KEY));

	// a repository added by prefix is distrusted under 'hosts_only', like under 'forbidden'; it stays listed
	REQUIRE_NO_FAIL(con.Query("SET allow_extension_repositories='hosts_only'"));
	REQUIRE(!KeyIsTrusted(*db.instance, ExtensionRepositoryType::USER_PROVIDED, "acme", TEST_PUBLIC_KEY));
	REQUIRE(UserRepositoryCount(con) == 1);

	// the ratchet: 'hosts_only' cannot go back to 'allowed' or 'hosts', only on to 'forbidden'
	REQUIRE_FAIL(con.Query("SET allow_extension_repositories='allowed'"));
	REQUIRE_FAIL(con.Query("SET allow_extension_repositories='hosts'"));
	REQUIRE_NO_FAIL(con.Query("SET allow_extension_repositories='forbidden'"));
	REQUIRE_FAIL(con.Query("SET allow_extension_repositories='hosts_only'"));
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

// Stage a stub extension in the per-repository subfolder that INSTALL ... FROM a user-provided repository writes to
static void StageUserRepoExtension(DatabaseInstance &db, const string &name, const string &repository_name) {
	auto fs = FileSystem::CreateLocal();
	string dir = fs->JoinPath(ExtensionHelper::ExtensionDirectory(db, *fs), "repositories");
	dir = fs->JoinPath(dir, repository_name);
	fs->CreateDirectoriesRecursive(dir);

	vector<data_t> stub(ParsedExtensionMetaData::FOOTER_SIZE, 0);
	WriteExtensionFile(*fs, fs->JoinPath(dir, name + ".duckdb_extension"), stub.data(), stub.size());
}

TEST_CASE("Test that a bare load names the user repository an extension is installed from", "[api]") {
	auto repository_directory = TestCreatePath("ext_repo_load_hint");
	auto extension_directory = TestCreatePath("ext_repo_load_hint_ext");

	DBConfig config;
	config.SetOptionByName("extension_repository_directory", repository_directory);
	config.SetOptionByName("extension_directory", extension_directory);
	config.SetOptionByName("allow_extension_repositories", "allowed");
	DuckDB db(nullptr, &config);
	Connection con(db);

	REQUIRE_NO_FAIL(con.Query(StringUtil::Format(
	    "CREATE EXTENSION REPOSITORY acme WITH PREFIX 'http://acme.com' USING PUBLIC KEY '%s'", TEST_PUBLIC_KEY)));
	REQUIRE_NO_FAIL(con.Query(StringUtil::Format(
	    "CREATE EXTENSION REPOSITORY other WITH PREFIX 'http://other.com' USING PUBLIC KEY '%s'", OTHER_PUBLIC_KEY)));
	StageUserRepoExtension(*db.instance, "some_ext", "acme");
	StageUserRepoExtension(*db.instance, "shared_ext", "acme");
	StageUserRepoExtension(*db.instance, "shared_ext", "other");
	StageUserRepoExtension(*db.instance, "stray_ext", "not_a_repository");

	// a bare load does not reach a user-provided repository, but the error names the repository to load from
	auto bare = con.Query("LOAD some_ext");
	REQUIRE(bare->HasError());
	REQUIRE(bare->GetErrorObject().Type() == ExceptionType::IO);
	REQUIRE(StringUtil::Contains(bare->GetError(),
	                             "Note: extension 'some_ext' exists in one or more user-provided repositories.\n"
	                             "Consider `LOAD some_ext FROM <repo_name>`\nCandidate repositories: \"acme\""));
	REQUIRE(!StringUtil::Contains(bare->GetError(), "other"));

	// attaching through the extension prefix performs a bare load as well
	auto attach = con.Query("ATTACH 'some_ext:file.db'");
	REQUIRE(attach->HasError());
	REQUIRE(StringUtil::Contains(attach->GetError(), "Candidate repositories: \"acme\""));

	// the same extension name can be installed in several repositories: each of them is a candidate
	auto shared = con.Query("LOAD shared_ext");
	REQUIRE(shared->HasError());
	REQUIRE(StringUtil::Contains(shared->GetError(), "Candidate repositories: \"acme\", \"other\""));

	// an extension that is not installed, or that sits in a folder no repository backs, gets the plain error
	auto missing = con.Query("LOAD missing_ext");
	REQUIRE(missing->HasError());
	REQUIRE(!StringUtil::Contains(missing->GetError(), "Candidate repositories"));
	auto stray = con.Query("LOAD stray_ext");
	REQUIRE(stray->HasError());
	REQUIRE(!StringUtil::Contains(stray->GetError(), "Candidate repositories"));

	// once repositories are forbidden they are distrusted, so loading from them is no longer suggested
	REQUIRE_NO_FAIL(con.Query("SET allow_extension_repositories='forbidden'"));
	auto forbidden = con.Query("LOAD some_ext");
	REQUIRE(forbidden->HasError());
	REQUIRE(!StringUtil::Contains(forbidden->GetError(), "Candidate repositories"));
}

// Stage an extension plus the .info an install writes, in the flat folder (empty folder_repository) or in the
// per-repository subfolder of folder_repository. The binary is a stub: duckdb_extensions only reads the .info
static void StageInstalledExtension(DatabaseInstance &db, const string &name, const string &folder_repository,
                                    const ExtensionInstallInfo &info) {
	auto fs = FileSystem::CreateLocal();
	string dir = ExtensionHelper::ExtensionDirectory(db, *fs);
	if (!folder_repository.empty()) {
		dir = fs->JoinPath(fs->JoinPath(dir, "repositories"), folder_repository);
	}
	fs->CreateDirectoriesRecursive(dir);

	string ext_path = fs->JoinPath(dir, name + ".duckdb_extension");
	vector<data_t> stub(ParsedExtensionMetaData::FOOTER_SIZE, 0);
	WriteExtensionFile(*fs, ext_path, stub.data(), stub.size());

	MemoryStream stream;
	BinarySerializer::Serialize(info, stream);
	WriteExtensionFile(*fs, ext_path + ".info", stream.GetData(), stream.GetPosition());
}

static ExtensionInstallInfo UserRepositoryInstallInfo(const string &repository_name, const string &version) {
	ExtensionInstallInfo info;
	info.mode = ExtensionInstallMode::REPOSITORY;
	info.repository_type = ExtensionRepositoryType::USER_PROVIDED;
	info.repository_name = repository_name;
	info.repository_url = "http://" + repository_name + ".com";
	info.version = version;
	info.signature_key_fingerprint = "sha256:" + repository_name;
	return info;
}

// One "installed|loaded|install_mode|installed_from|version|fingerprint|path suffix" string per row of the extension
static vector<string> ExtensionRows(Connection &con, const string &name) {
	auto result = con.Query("SELECT installed, loaded, install_mode, installed_from, extension_version, "
	                        "coalesce(signature_key_fingerprint, 'NULL'), "
	                        "regexp_extract(replace(install_path, '\\', '/'), '(repositories/[^/]+/)?[^/]*$') "
	                        "FROM duckdb_extensions() WHERE extension_name = $1",
	                        name);
	REQUIRE_NO_FAIL(*result);
	vector<string> rows;
	for (auto &row : result->Collection().Rows()) {
		vector<string> values;
		for (idx_t c = 0; c < result->ColumnCount(); c++) {
			values.push_back(row.GetValue(c).ToString());
		}
		rows.push_back(StringUtil::Join(values, "|"));
	}
	return rows;
}

TEST_CASE("Test that duckdb_extensions lists extensions installed from user-provided repositories", "[api]") {
	auto repository_directory = TestCreatePath("ext_repo_duckdb_extensions");
	auto extension_directory = TestCreatePath("ext_repo_duckdb_extensions_ext");

	DBConfig config;
	config.SetOptionByName("extension_repository_directory", repository_directory);
	config.SetOptionByName("extension_directory", extension_directory);
	config.SetOptionByName("allow_extension_repositories", "allowed");
	DuckDB db(nullptr, &config);
	Connection con(db);

	REQUIRE_NO_FAIL(con.Query(
	    StringUtil::Format("CREATE EXTENSION REPOSITORY my_repo WITH PREFIX 'http://my_repo.com' USING PUBLIC KEY '%s'",
	                       TEST_PUBLIC_KEY)));
	REQUIRE_NO_FAIL(con.Query(StringUtil::Format(
	    "CREATE EXTENSION REPOSITORY other WITH PREFIX 'http://other.com' USING PUBLIC KEY '%s'", OTHER_PUBLIC_KEY)));
	REQUIRE(ExtensionRows(con, "dns").empty());

	// INSTALL dns FROM my_repo writes to the per-repository subfolder: the row reports the repository by name
	StageInstalledExtension(*db.instance, "dns", "my_repo", UserRepositoryInstallInfo("my_repo", "v1"));
	REQUIRE(
	    ExtensionRows(con, "dns") ==
	    vector<string> {"true|false|REPOSITORY|my_repo|v1|sha256:my_repo|repositories/my_repo/dns.duckdb_extension"});

	// rows are keyed by name and repository: a copy in the flat folder and in another repository each get a row
	ExtensionInstallInfo community_info;
	community_info.mode = ExtensionInstallMode::REPOSITORY;
	community_info.repository_type = ExtensionRepositoryType::COMMUNITY;
	community_info.repository_url = ExtensionRepository::TryGetRepositoryUrl("community");
	community_info.version = "v0";
	StageInstalledExtension(*db.instance, "dns", "", community_info);
	StageInstalledExtension(*db.instance, "dns", "other", UserRepositoryInstallInfo("other", "v2"));
	REQUIRE(ExtensionRows(con, "dns") ==
	        vector<string> {"true|false|REPOSITORY|community|v0|NULL|dns.duckdb_extension",
	                        "true|false|REPOSITORY|my_repo|v1|sha256:my_repo|repositories/my_repo/dns.duckdb_extension",
	                        "true|false|REPOSITORY|other|v2|sha256:other|repositories/other/dns.duckdb_extension"});

	// a known extension keeps its flat row next to the copy from a user-provided repository
	StageInstalledExtension(*db.instance, "json", "my_repo", UserRepositoryInstallInfo("my_repo", "v1"));
	auto json_rows = ExtensionRows(con, "json");
	REQUIRE(json_rows.size() == 2);
	REQUIRE(json_rows[1] ==
	        "true|false|REPOSITORY|my_repo|v1|sha256:my_repo|repositories/my_repo/json.duckdb_extension");

	// moving the files to the flat folder, as the reproducer does, keeps the origin recorded in the .info
	StageInstalledExtension(*db.instance, "moved_ext", "", UserRepositoryInstallInfo("my_repo", "v1"));
	REQUIRE(ExtensionRows(con, "moved_ext") ==
	        vector<string> {"true|false|REPOSITORY|my_repo|v1|sha256:my_repo|moved_ext.duckdb_extension"});

	// install folders are listed from disk, also when no repository file backs them
	StageInstalledExtension(*db.instance, "stray_ext", "not_a_repository",
	                        UserRepositoryInstallInfo("not_a_repository", "v1"));
	REQUIRE(ExtensionRows(con, "stray_ext") ==
	        vector<string> {"true|false|REPOSITORY|not_a_repository|v1|sha256:not_a_repository|"
	                        "repositories/not_a_repository/stray_ext.duckdb_extension"});

	// folders that cannot be repository names are not listed
	StageInstalledExtension(*db.instance, "invalid_ext", "Not_Normalized", UserRepositoryInstallInfo("x", "v1"));
	StageInstalledExtension(*db.instance, "invalid_ext", "core", UserRepositoryInstallInfo("x", "v1"));
	REQUIRE(ExtensionRows(con, "invalid_ext").empty());

	// an unreadable repository file does not fail the listing, nor hide any installed extension
	auto fs = FileSystem::CreateLocal();
	auto broken_path = fs->JoinPath(repository_directory, "broken.duckdb_extension.repo.json");
	{
		auto handle = fs->OpenFile(broken_path, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW);
		string garbage = "not json";
		handle->Write((void *)garbage.data(), garbage.size());
	}
	REQUIRE(ExtensionRows(con, "dns").size() == 3);
	fs->RemoveFile(broken_path);

	// dropping a repository leaves its installed files on disk, so they are still listed
	REQUIRE_NO_FAIL(con.Query("DROP EXTENSION REPOSITORY other"));
	REQUIRE(ExtensionRows(con, "dns").size() == 3);

	// forbidding repositories only affects loading: the installed files are still listed
	REQUIRE_NO_FAIL(con.Query("SET allow_extension_repositories='forbidden'"));
	REQUIRE(ExtensionRows(con, "dns").size() == 3);
	REQUIRE(ExtensionRows(con, "json").size() == 2);
}

// Copy a loadable extension into the flat folder (empty folder_repository) or a per-repository subfolder, with the
// .info an install from repository_name writes
static string CopyInstalledExtension(DatabaseInstance &db, const string &source, const string &name,
                                     const string &folder_repository, const string &repository_name,
                                     const string &version) {
	auto fs = FileSystem::CreateLocal();
	string dir = ExtensionHelper::ExtensionDirectory(db, *fs);
	if (!folder_repository.empty()) {
		dir = fs->JoinPath(fs->JoinPath(dir, "repositories"), folder_repository);
	}
	fs->CreateDirectoriesRecursive(dir);

	auto source_handle = fs->OpenFile(source, FileFlags::FILE_FLAGS_READ);
	auto size = source_handle->GetFileSize();
	auto buffer = make_unsafe_uniq_array<data_t>(size);
	source_handle->Read(buffer.get(), size);
	string ext_path = fs->JoinPath(dir, name + ".duckdb_extension");
	WriteExtensionFile(*fs, ext_path, buffer.get(), size);

	MemoryStream stream;
	BinarySerializer::Serialize(UserRepositoryInstallInfo(repository_name, version), stream);
	WriteExtensionFile(*fs, ext_path + ".info", stream.GetData(), stream.GetPosition());
	return ext_path;
}

// "in_repository_folder|loaded" per row, the flat row first
static vector<string> LoadedRows(Connection &con, const string &name) {
	auto result = con.Query("SELECT loaded, "
	                        "replace(install_path, '\\', '/') LIKE '%/repositories/%' AS in_repository "
	                        "FROM duckdb_extensions() WHERE extension_name = $1 ORDER BY in_repository",
	                        name);
	REQUIRE_NO_FAIL(*result);
	vector<string> rows;
	for (auto &row : result->Collection().Rows()) {
		rows.push_back(row.GetValue(1).ToString() + "|" + row.GetValue(0).ToString());
	}
	return rows;
}

TEST_CASE("Test that duckdb_extensions marks the loaded copy", "[api]") {
	// short names: the test name is part of the path, and LoadLibrary rejects paths over MAX_PATH on Windows
	auto repository_directory = TestCreatePath("lc_repo");
	auto extension_directory = TestCreatePath("lc_ext");
	auto fs = FileSystem::CreateLocal();
	auto demo = fs->JoinPath(TestBuildDirectory(), "test/extension/loadable_extension_demo.duckdb_extension");
	if (!fs->FileExists(demo)) {
		return;
	}
	const string name = "loadable_extension_demo";

	auto make_db = [&]() {
		DBConfig config;
		config.SetOptionByName("extension_repository_directory", repository_directory);
		config.SetOptionByName("extension_directory", extension_directory);
		config.SetOptionByName("allow_extension_repositories", "allowed");
		config.SetOptionByName("allow_unsigned_extensions", true);
		return make_uniq<DuckDB>(nullptr, &config);
	};

	// the .info version has to match the binary
	string version;
	string repository_path;
	{
		auto db = make_db();
		Connection con(*db);
		REQUIRE_NO_FAIL(con.Query(StringUtil::Format(
		    "CREATE EXTENSION REPOSITORY my_repo WITH PREFIX 'http://my_repo.com' USING PUBLIC KEY '%s'",
		    TEST_PUBLIC_KEY)));
		REQUIRE_NO_FAIL(con.Query(StringUtil::Format("LOAD '%s'", demo)));
		auto result = con.Query("SELECT extension_version FROM duckdb_extensions() WHERE extension_name = $1", name);
		REQUIRE_NO_FAIL(*result);
		version = result->Collection().GetValue(0, 0).ToString();

		// the same extension installed from my_repo, and a copy of it in the flat folder
		repository_path = CopyInstalledExtension(*db->instance, demo, name, "my_repo", "my_repo", version);
		CopyInstalledExtension(*db->instance, demo, name, "", "my_repo", version);
	}

	// a bare load resolves the flat copy, even though both .info files record my_repo
	{
		auto db = make_db();
		Connection con(*db);
		REQUIRE_NO_FAIL(con.Query("LOAD " + name));
		REQUIRE(LoadedRows(con, name) == vector<string> {"false|true", "true|false"});
	}

	// loading from the repository resolves the copy in its folder
	{
		auto db = make_db();
		Connection con(*db);
		REQUIRE_NO_FAIL(con.Query("LOAD " + name + " FROM my_repo"));
		REQUIRE(LoadedRows(con, name) == vector<string> {"false|false", "true|true"});
	}

	// loading the repository copy by its path marks that copy
	{
		auto db = make_db();
		Connection con(*db);
		REQUIRE_NO_FAIL(con.Query(StringUtil::Format("LOAD '%s'", repository_path)));
		REQUIRE(LoadedRows(con, name) == vector<string> {"false|false", "true|true"});
	}
}
