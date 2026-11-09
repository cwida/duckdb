#include "catch.hpp"
#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/common/local_file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/virtual_file_system.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/secret/secret.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "test_helpers.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <thread>

using namespace duckdb;

namespace {

//! Observes the operations on secret files and fails the sync, the rename or the removal while enabled
class SecretFileSystem : public LocalFileSystem {
public:
	std::atomic<bool> fail_secret_sync {false};
	std::atomic<bool> fail_secret_move {false};
	std::atomic<bool> fail_secret_remove {false};
	//! Runs when a secret file is synced, with the file complete and nothing replaced yet
	std::function<void()> on_secret_sync;
	//! Runs when a secret file was renamed into place, before the catalog changes
	std::function<void()> on_secret_moved;
	//! Renames of a secret file over an existing file
	std::atomic<idx_t> moves_over_existing_file {0};
	//! Removals of the file of a stored secret, as opposed to a temporary file
	std::atomic<idx_t> stored_secret_files_removed {0};

public:
	void FileSync(FileHandle &handle) override {
		if (IsSecretFile(handle.GetPath())) {
			if (on_secret_sync) {
				on_secret_sync();
			}
			if (fail_secret_sync) {
				throw IOException("Could not fsync file \"%s\": No space left on device", handle.GetPath());
			}
		}
		LocalFileSystem::FileSync(handle);
	}

	void MoveFile(const string &source, const string &target, optional_ptr<FileOpener> opener) override {
		if (IsSecretFile(target)) {
			if (LocalFileSystem::FileExists(target, opener)) {
				moves_over_existing_file++;
			}
			if (fail_secret_move) {
				throw IOException("Could not rename file \"%s\" to \"%s\": Permission denied", source, target);
			}
		}
		LocalFileSystem::MoveFile(source, target, opener);
		if (IsSecretFile(target) && on_secret_moved) {
			on_secret_moved();
		}
	}

	bool TryRemoveFile(const string &filename, optional_ptr<FileOpener> opener) override {
		if (fail_secret_remove && IsSecretFile(filename) && LocalFileSystem::FileExists(filename, opener)) {
			throw IOException("Could not remove file \"%s\": Permission denied", filename);
		}
		return LocalFileSystem::TryRemoveFile(filename, opener);
	}

	void RemoveFile(const string &filename, optional_ptr<FileOpener> opener) override {
		if (IsStoredSecretFile(filename)) {
			stored_secret_files_removed++;
		}
		LocalFileSystem::RemoveFile(filename, opener);
	}

private:
	static bool IsSecretFile(const string &path) {
		return StringUtil::Contains(path, ".duckdb_secret");
	}
	static bool IsStoredSecretFile(const string &path) {
		return StringUtil::EndsWith(path, ".duckdb_secret");
	}
};

//! The tokens are checked, which only a database started with this option shows
unique_ptr<DBConfig> UnredactedConfig() {
	auto config = GetTestConfig();
	config->SetOptionByName("allow_unredacted_secrets", Value::BOOLEAN(true));
	return config;
}

//! A database on the directory that observes the secret files
unique_ptr<DBConfig> ObservedConfig(unique_ptr<SecretFileSystem> fs) {
	auto config = UnredactedConfig();
	config->file_system = make_uniq<VirtualFileSystem>(std::move(fs));
	return config;
}

vector<string> SecretFiles(const string &secret_dir) {
	LocalFileSystem local;
	vector<string> result;
	if (!local.DirectoryExists(secret_dir)) {
		return result;
	}
	local.ListFiles(secret_dir, [&](const string &name, bool is_dir) { result.push_back(name); });
	std::sort(result.begin(), result.end());
	return result;
}

//! A fresh database on the directory reads the secret with this token
void CheckFreshDatabaseReadsToken(const string &secret_dir, const string &name, const string &token) {
	auto config = UnredactedConfig();
	DuckDB db(nullptr, config.get());
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET secret_directory = '" + secret_dir + "'"));
	auto result = con.Query("SELECT persistent, secret_string LIKE '%bearer_token=" + token +
	                        "%' FROM duckdb_secrets(redact := false) WHERE name = '" + name + "'");
	REQUIRE(result->RowCount() == 1);
	REQUIRE(CHECK_COLUMN(result, 0, {true}));
	REQUIRE(CHECK_COLUMN(result, 1, {true}));
}

} // namespace

TEST_CASE("A persistent secret whose file cannot be synced is not created", "[api][secrets]") {
	auto observing_fs = make_uniq<SecretFileSystem>();
	auto &fs = *observing_fs;
	auto config = ObservedConfig(std::move(observing_fs));
	DuckDB db(nullptr, config.get());
	Connection con(db);
	auto secret_dir = TestCreatePath("persistent_secret_write_failure");
	REQUIRE_NO_FAIL(con.Query("SET secret_directory = '" + secret_dir + "'"));

	// the write fails when the file is synced, so the statement fails and nothing stays behind
	fs.fail_secret_sync = true;
	auto result = con.Query("CREATE PERSISTENT SECRET s1 (TYPE HTTP, BEARER_TOKEN 'abc')");
	REQUIRE(result->HasError());
	REQUIRE(StringUtil::Contains(result->GetError(), "Could not fsync"));
	result = con.Query("SELECT count(*) FROM duckdb_secrets()");
	REQUIRE(CHECK_COLUMN(result, 0, {0}));
	REQUIRE(SecretFiles(secret_dir).empty());

	// the file system recovers
	fs.fail_secret_sync = false;
	REQUIRE_NO_FAIL(con.Query("CREATE PERSISTENT SECRET s1 (TYPE HTTP, BEARER_TOKEN 'abc')"));
	REQUIRE(SecretFiles(secret_dir) == vector<string> {"s1.duckdb_secret"});

	// a replacement whose write fails keeps the old secret in its file
	fs.fail_secret_sync = true;
	result = con.Query("CREATE OR REPLACE PERSISTENT SECRET s1 (TYPE HTTP, BEARER_TOKEN 'xyz')");
	REQUIRE(result->HasError());
	REQUIRE(StringUtil::Contains(result->GetError(), "Could not fsync"));
	REQUIRE(SecretFiles(secret_dir) == vector<string> {"s1.duckdb_secret"});
	result = con.Query("SELECT name, secret_string LIKE '%bearer_token=abc%' FROM duckdb_secrets(redact := false)");
	REQUIRE(result->RowCount() == 1);
	REQUIRE(CHECK_COLUMN(result, 0, {"s1"}));
	REQUIRE(CHECK_COLUMN(result, 1, {true}));

	// the old secret is the one on disk
	fs.fail_secret_sync = false;
	CheckFreshDatabaseReadsToken(secret_dir, "s1", "abc");

	// when the half written file cannot be removed either, the write's error is the one reported
	fs.fail_secret_sync = true;
	fs.fail_secret_remove = true;
	result = con.Query("CREATE OR REPLACE PERSISTENT SECRET s1 (TYPE HTTP, BEARER_TOKEN 'xyz')");
	REQUIRE(result->HasError());
	REQUIRE(StringUtil::Contains(result->GetError(), "Could not fsync"));
	fs.fail_secret_sync = false;
	fs.fail_secret_remove = false;
	auto files = SecretFiles(secret_dir);
	REQUIRE(files.size() == 2);
	REQUIRE(files[0] == "s1.duckdb_secret");
	REQUIRE(StringUtil::StartsWith(files[1], "s1.duckdb_secret.tmp-"));
	CheckFreshDatabaseReadsToken(secret_dir, "s1", "abc");
	LocalFileSystem local;
	local.RemoveFile(local.JoinPath(secret_dir, files[1]));

	// a replacement that succeeds swaps the file
	REQUIRE_NO_FAIL(con.Query("CREATE OR REPLACE PERSISTENT SECRET s1 (TYPE HTTP, BEARER_TOKEN 'xyz')"));
	REQUIRE(SecretFiles(secret_dir) == vector<string> {"s1.duckdb_secret"});
	result = con.Query("SELECT secret_string LIKE '%bearer_token=xyz%' FROM duckdb_secrets(redact := false)");
	REQUIRE(CHECK_COLUMN(result, 0, {true}));
	CheckFreshDatabaseReadsToken(secret_dir, "s1", "xyz");

	// a creation of an existing name is refused or ignored before anything is written
	fs.fail_secret_sync = true;
	result = con.Query("CREATE PERSISTENT SECRET s1 (TYPE HTTP, BEARER_TOKEN 'other')");
	REQUIRE(result->HasError());
	REQUIRE(StringUtil::Contains(result->GetError(), "already exists"));
	REQUIRE_NO_FAIL(con.Query("CREATE PERSISTENT SECRET IF NOT EXISTS s1 (TYPE HTTP, BEARER_TOKEN 'other')"));
	fs.fail_secret_sync = false;
	REQUIRE(SecretFiles(secret_dir) == vector<string> {"s1.duckdb_secret"});
	CheckFreshDatabaseReadsToken(secret_dir, "s1", "xyz");
}

TEST_CASE("A replacement renames the new file over the old one and removes no file", "[api][secrets]") {
	auto observing_fs = make_uniq<SecretFileSystem>();
	auto &fs = *observing_fs;
	auto config = ObservedConfig(std::move(observing_fs));
	DuckDB db(nullptr, config.get());
	Connection con(db);
	auto secret_dir = TestCreatePath("persistent_secret_replace_rename");
	REQUIRE_NO_FAIL(con.Query("SET secret_directory = '" + secret_dir + "'"));
	REQUIRE_NO_FAIL(con.Query("CREATE PERSISTENT SECRET s1 (TYPE HTTP, BEARER_TOKEN 'abc')"));
	REQUIRE(fs.moves_over_existing_file == 0);

	// the old file stays in place until the complete new file is renamed over it
	REQUIRE_NO_FAIL(con.Query("CREATE OR REPLACE PERSISTENT SECRET s1 (TYPE HTTP, BEARER_TOKEN 'xyz')"));
	REQUIRE(fs.moves_over_existing_file == 1);
	REQUIRE(fs.stored_secret_files_removed == 0);
	REQUIRE(SecretFiles(secret_dir) == vector<string> {"s1.duckdb_secret"});
	CheckFreshDatabaseReadsToken(secret_dir, "s1", "xyz");

	// a rename that fails leaves the old file, removes the new one and keeps the old secret in the catalog
	fs.fail_secret_move = true;
	auto result = con.Query("CREATE OR REPLACE PERSISTENT SECRET s1 (TYPE HTTP, BEARER_TOKEN 'lost')");
	REQUIRE(result->HasError());
	REQUIRE(StringUtil::Contains(result->GetError(), "Could not rename"));
	fs.fail_secret_move = false;
	REQUIRE(fs.stored_secret_files_removed == 0);
	REQUIRE(SecretFiles(secret_dir) == vector<string> {"s1.duckdb_secret"});
	result = con.Query("SELECT secret_string LIKE '%bearer_token=xyz%' FROM duckdb_secrets(redact := false)");
	REQUIRE(CHECK_COLUMN(result, 0, {true}));
	CheckFreshDatabaseReadsToken(secret_dir, "s1", "xyz");
}

TEST_CASE("A replaced secret keeps the file it was loaded from", "[api][secrets]") {
	auto secret_dir = TestCreatePath("persistent_secret_legacy_file");
	{
		auto config = UnredactedConfig();
		DuckDB db(nullptr, config.get());
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("SET secret_directory = '" + secret_dir + "'"));
		REQUIRE_NO_FAIL(con.Query("CREATE PERSISTENT SECRET \"legacy%name\" (TYPE HTTP, BEARER_TOKEN 'abc')"));
		REQUIRE(SecretFiles(secret_dir) == vector<string> {"legacy%25name.duckdb_secret"});
	}

	// a release before names were escaped in file names wrote the file under the plain name
	LocalFileSystem local;
	local.MoveFile(local.JoinPath(secret_dir, "legacy%25name.duckdb_secret"),
	               local.JoinPath(secret_dir, "legacy%name.duckdb_secret"));

	auto observing_fs = make_uniq<SecretFileSystem>();
	auto &fs = *observing_fs;
	auto config = ObservedConfig(std::move(observing_fs));
	DuckDB db(nullptr, config.get());
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET secret_directory = '" + secret_dir + "'"));
	auto result = con.Query("SELECT name FROM duckdb_secrets()");
	REQUIRE(result->RowCount() == 1);
	REQUIRE(CHECK_COLUMN(result, 0, {"legacy%name"}));

	// the replacement is renamed over that file, so no step leaves the secret without a file
	REQUIRE_NO_FAIL(con.Query("CREATE OR REPLACE PERSISTENT SECRET \"legacy%name\" (TYPE HTTP, BEARER_TOKEN 'xyz')"));
	REQUIRE(fs.moves_over_existing_file == 1);
	REQUIRE(fs.stored_secret_files_removed == 0);
	REQUIRE(SecretFiles(secret_dir) == vector<string> {"legacy%name.duckdb_secret"});
	CheckFreshDatabaseReadsToken(secret_dir, "legacy%name", "xyz");

	REQUIRE_NO_FAIL(con.Query("DROP PERSISTENT SECRET \"legacy%name\""));
	REQUIRE(SecretFiles(secret_dir).empty());
}

TEST_CASE("A write the catalog rejects leaves its file, and the catalog the committed secret", "[api][secrets]") {
	auto config = UnredactedConfig();
	DuckDB db(nullptr, config.get());
	Connection con(db);
	auto secret_dir = TestCreatePath("persistent_secret_rejected_write");
	REQUIRE_NO_FAIL(con.Query("SET secret_directory = '" + secret_dir + "'"));
	REQUIRE_NO_FAIL(con.Query("CREATE PERSISTENT SECRET s1 (TYPE HTTP, BEARER_TOKEN 'first')"));

	// a transaction reads the secret, another connection replaces it, and the transaction's own replacement conflicts
	Connection reader(db);
	REQUIRE_NO_FAIL(reader.Query("BEGIN"));
	auto result = reader.Query("SELECT name FROM duckdb_secrets()");
	REQUIRE(CHECK_COLUMN(result, 0, {"s1"}));
	REQUIRE_NO_FAIL(con.Query("CREATE OR REPLACE PERSISTENT SECRET s1 (TYPE HTTP, BEARER_TOKEN 'winner')"));
	result = reader.Query("CREATE OR REPLACE PERSISTENT SECRET s1 (TYPE HTTP, BEARER_TOKEN 'loser')");
	REQUIRE(result->HasError());
	REQUIRE(StringUtil::Contains(result->GetError(), "conflict"));
	REQUIRE_NO_FAIL(reader.Query("ROLLBACK"));

	// the catalog keeps the committed secret, the file holds the rejected one until the next replacement
	result = con.Query("SELECT secret_string LIKE '%bearer_token=winner%' FROM duckdb_secrets(redact := false)");
	REQUIRE(result->RowCount() == 1);
	REQUIRE(CHECK_COLUMN(result, 0, {true}));
	REQUIRE(SecretFiles(secret_dir) == vector<string> {"s1.duckdb_secret"});
	CheckFreshDatabaseReadsToken(secret_dir, "s1", "loser");
	REQUIRE_NO_FAIL(con.Query("CREATE OR REPLACE PERSISTENT SECRET s1 (TYPE HTTP, BEARER_TOKEN 'third')"));
	CheckFreshDatabaseReadsToken(secret_dir, "s1", "third");

	// a creation conflicts the same way when another connection created the secret after the transaction's snapshot
	REQUIRE_NO_FAIL(reader.Query("BEGIN"));
	result = reader.Query("SELECT count(*) FROM duckdb_secrets() WHERE name = 's2'");
	REQUIRE(CHECK_COLUMN(result, 0, {0}));
	REQUIRE_NO_FAIL(con.Query("CREATE PERSISTENT SECRET s2 (TYPE HTTP, BEARER_TOKEN 'first')"));
	result = reader.Query("CREATE PERSISTENT SECRET s2 (TYPE HTTP, BEARER_TOKEN 'second')");
	REQUIRE(result->HasError());
	REQUIRE(StringUtil::Contains(result->GetError(), "conflict"));
	REQUIRE_NO_FAIL(reader.Query("ROLLBACK"));
	result = con.Query("SELECT secret_string LIKE '%bearer_token=first%' FROM duckdb_secrets(redact := false) WHERE "
	                   "name = 's2'");
	REQUIRE(result->RowCount() == 1);
	REQUIRE(CHECK_COLUMN(result, 0, {true}));
	REQUIRE(SecretFiles(secret_dir) == vector<string> {"s1.duckdb_secret", "s2.duckdb_secret"});
	CheckFreshDatabaseReadsToken(secret_dir, "s2", "second");
	REQUIRE_NO_FAIL(con.Query("DROP PERSISTENT SECRET s2"));
	REQUIRE(SecretFiles(secret_dir) == vector<string> {"s1.duckdb_secret"});
}

TEST_CASE("A failed replacement through the system transaction keeps the replaced secret", "[api][secrets]") {
	auto observing_fs = make_uniq<SecretFileSystem>();
	auto &fs = *observing_fs;
	auto config = ObservedConfig(std::move(observing_fs));
	DuckDB db(nullptr, config.get());
	Connection con(db);
	auto secret_dir = TestCreatePath("persistent_secret_system_transaction");
	REQUIRE_NO_FAIL(con.Query("SET secret_directory = '" + secret_dir + "'"));

	// extensions use the system transaction, which has no rollback, so the catalog changes after the file is in place
	auto &secret_manager = SecretManager::Get(*db.instance);
	auto transaction = CatalogTransaction::GetSystemTransaction(*db.instance);
	auto register_secret = [&](const string &name, const string &token) {
		auto secret = make_uniq<KeyValueSecret>(vector<string> {"http://"}, "http", "config", Identifier(name));
		secret->secret_map["bearer_token"] = Value(token);
		string error;
		try {
			secret_manager.RegisterSecret(transaction, std::move(secret), OnCreateConflict::REPLACE_ON_CONFLICT,
			                              SecretPersistType::PERSISTENT);
		} catch (std::exception &ex) {
			error = ex.what();
		}
		return error;
	};
	REQUIRE(register_secret("s1", "abc").empty());
	REQUIRE(SecretFiles(secret_dir) == vector<string> {"s1.duckdb_secret"});
	auto check_replaced_secret_kept = [&]() {
		auto result =
		    con.Query("SELECT name, persistent, secret_string LIKE '%bearer_token=abc%' FROM duckdb_secrets(redact "
		              ":= false)");
		REQUIRE(result->RowCount() == 1);
		REQUIRE(CHECK_COLUMN(result, 0, {"s1"}));
		REQUIRE(CHECK_COLUMN(result, 1, {true}));
		REQUIRE(CHECK_COLUMN(result, 2, {true}));
		REQUIRE(SecretFiles(secret_dir) == vector<string> {"s1.duckdb_secret"});
		CheckFreshDatabaseReadsToken(secret_dir, "s1", "abc");
	};

	// while the new file is written, the replaced secret is still in the catalog with its token
	unique_ptr<QueryResult> secret_during_write;
	fs.on_secret_sync = [&]() {
		secret_during_write = con.Query("SELECT secret_string LIKE '%bearer_token=abc%' FROM duckdb_secrets(redact := "
		                                "false) WHERE name = 's1'");
	};

	// the replacement's write fails, so the replaced secret stays in the catalog and in its file
	fs.fail_secret_sync = true;
	auto error = register_secret("s1", "xyz");
	fs.fail_secret_sync = false;
	fs.on_secret_sync = nullptr;
	REQUIRE(StringUtil::Contains(error, "Could not fsync"));
	REQUIRE(secret_during_write);
	REQUIRE(CHECK_COLUMN(secret_during_write, 0, {true}));
	check_replaced_secret_kept();

	// a rename that fails changes nothing either, as the catalog changes only after the file is in place
	fs.fail_secret_move = true;
	error = register_secret("s1", "xyz");
	fs.fail_secret_move = false;
	REQUIRE(StringUtil::Contains(error, "Could not rename"));
	check_replaced_secret_kept();

	// a drop that arrives during the store waits for it, so it cannot leave the new entry without its file
	string drop_error;
	std::atomic<bool> drop_started {false};
	std::atomic<bool> drop_finished {false};
	bool drop_finished_during_store = false;
	std::thread drop_thread;
	fs.on_secret_moved = [&]() {
		drop_thread = std::thread([&]() {
			drop_started = true;
			try {
				secret_manager.DropSecretByName(transaction, Identifier("s1"), OnEntryNotFound::THROW_EXCEPTION,
				                                SecretPersistType::PERSISTENT);
			} catch (std::exception &ex) {
				drop_error = ex.what();
			}
			drop_finished = true;
		});
		while (!drop_started) {
			std::this_thread::yield();
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
		drop_finished_during_store = drop_finished;
	};
	error = register_secret("s1", "xyz");
	fs.on_secret_moved = nullptr;
	if (drop_thread.joinable()) {
		drop_thread.join();
	}
	REQUIRE(error == "");
	REQUIRE(drop_error == "");
	REQUIRE(!drop_finished_during_store);
	auto result = con.Query("SELECT count(*) FROM duckdb_secrets()");
	REQUIRE(CHECK_COLUMN(result, 0, {0}));
	REQUIRE(SecretFiles(secret_dir).empty());
}
