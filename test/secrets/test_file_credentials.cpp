#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb.hpp"
#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/common/file_opener.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/multi_file/multi_file_list.hpp"
#include "duckdb/main/client_context_file_opener.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/database_file_opener.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/extension_manager.hpp"
#include "duckdb/main/secret/secret.hpp"
#include "duckdb/main/secret/secret_manager.hpp"

#include <atomic>
#include <chrono>
#include <fstream>
#include <sstream>
#include <thread>

using namespace duckdb;

namespace {

//! The secret type the test file system reads, and one it does not
constexpr const char *TEST_TYPE = "file_credentials_test";
constexpr const char *OTHER_TYPE = "file_credentials_test_other";
//! What a read records when no secret and no setting give a key
constexpr const char *NO_KEY = "<none>";
//! The sum of column i over the two files CredentialsFixture::WriteFiles writes
constexpr const char *SAME_FILES_SUM = "999000";

duckdb::unique_ptr<BaseSecret> CreateTestSecret(ClientContext &context, CreateSecretInput &input) {
	auto secret = make_uniq<KeyValueSecret>(input.scope, input.type, input.provider, input.name);
	for (auto &option : input.options) {
		secret->secret_map[Identifier(option.first)] = Value(option.second.ToString());
	}
	return std::move(secret);
}

void RegisterTestSecretType(DatabaseInstance &instance) {
	ExtensionInfo extension_info {};
	ExtensionActiveLoad load_info {instance, extension_info, Identifier("file_credentials_test"), Identifier()};
	ExtensionLoader loader {load_info};
	SecretType secret_type;
	secret_type.name = Identifier(TEST_TYPE);
	secret_type.deserializer = KeyValueSecret::Deserialize<KeyValueSecret>;
	secret_type.default_provider = "config";
	loader.RegisterSecretType(secret_type);
	CreateSecretFunction function = {TEST_TYPE, "config", CreateTestSecret};
	function.named_parameters["key_id"] = LogicalType::VARCHAR;
	function.named_parameters["region"] = LogicalType::VARCHAR;
	loader.RegisterFunction(function);
	auto &config = DBConfig::GetConfig(instance);
	config.AddExtensionOption("file_credentials_test_region", "region a read uses when no secret sets one",
	                          LogicalType::VARCHAR);
}

//! A secret of `type` with key id `key_id`
shared_ptr<const KeyValueSecret> TestSecret(const string &key_id, const char *type = TEST_TYPE) {
	auto secret = make_shared_ptr<KeyValueSecret>(vector<string>(), Identifier(type), Identifier("test"), Identifier());
	secret->secret_map[Identifier("key_id")] = Value(key_id);
	return std::move(secret);
}

//! One open, read or glob the test file system served, with the key id and region the secret reader gave it
struct Access {
	string kind;
	string path;
	string key_id;
	string region;
};

struct MemoryFileHandle : public FileHandle {
	MemoryFileHandle(FileSystem &fs, const string &path, FileOpenFlags flags, string key_id_p)
	    : FileHandle(fs, path, flags), key_id(std::move(key_id_p)) {
	}
	void Close() override {
	}
	string key_id;
	idx_t position = 0;
};

//! Serves files from memory under mem://, and records every open, read and glob with the key id of the secret the
//! secret reader chose for it. A glob lists the files under the pattern's prefix, each with the glob's ephemeral
//! secret, or with the one `listing_secrets` gives its pattern, as a catalog lists a table
class MemoryFileSystem : public FileSystem {
public:
	duckdb::map<string, string> files;
	//! By glob pattern; "" for every pattern
	duckdb::map<string, string> listing_secrets;
	duckdb::mutex lock;
	duckdb::vector<Access> accesses;

	duckdb::vector<Access> Accesses(const string &kind) {
		duckdb::lock_guard<duckdb::mutex> guard(lock);
		duckdb::vector<Access> result;
		for (auto &access : accesses) {
			if (access.kind == kind) {
				result.push_back(access);
			}
		}
		return result;
	}
	idx_t CountKey(const string &kind, const string &key) {
		idx_t count = 0;
		for (auto &access : Accesses(kind)) {
			count += access.key_id == key;
		}
		return count;
	}
	void ClearAccesses() {
		duckdb::lock_guard<duckdb::mutex> guard(lock);
		accesses.clear();
	}
	string GetName() const override {
		return "MemoryFileSystem";
	}
	using FileSystem::OpenFile;
	unique_ptr<FileHandle> OpenFile(const string &path, FileOpenFlags flags,
	                                optional_ptr<FileOpener> opener = nullptr) override {
		return OpenFileExtended(OpenFileInfo(path), flags, opener);
	}
	bool CanHandleFile(const string &path) override {
		return StringUtil::StartsWith(path, "mem://");
	}
	bool SupportsOpenFileExtended() const override {
		return true;
	}
	bool SupportsGlobExtended() const override {
		return true;
	}
	void Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override {
		auto &memory_handle = handle.Cast<MemoryFileHandle>();
		auto &data = files.at(handle.path);
		if (location + nr_bytes > data.size()) {
			throw IOException("Read past the end of \"%s\"", handle.path);
		}
		memcpy(buffer, data.data() + location, nr_bytes);
		Record("read", handle.path, memory_handle.key_id, "");
	}
	int64_t Read(FileHandle &handle, void *buffer, int64_t nr_bytes) override {
		auto &memory_handle = handle.Cast<MemoryFileHandle>();
		auto &data = files.at(handle.path);
		auto count = MinValue<int64_t>(nr_bytes, data.size() - memory_handle.position);
		Read(handle, buffer, count, memory_handle.position);
		memory_handle.position += count;
		return count;
	}
	int64_t GetFileSize(FileHandle &handle) override {
		return files.at(handle.path).size();
	}
	timestamp_t GetLastModifiedTime(FileHandle &handle) override {
		return Timestamp::FromEpochSeconds(1000);
	}
	string GetVersionTag(FileHandle &handle) override {
		return "v1";
	}
	FileType GetFileType(FileHandle &handle) override {
		return FileType::FILE_TYPE_REGULAR;
	}
	bool CanSeek() override {
		return true;
	}
	bool OnDiskFile(FileHandle &handle) override {
		return false;
	}
	void Seek(FileHandle &handle, idx_t location) override {
		handle.Cast<MemoryFileHandle>().position = location;
	}
	idx_t SeekPosition(FileHandle &handle) override {
		return handle.Cast<MemoryFileHandle>().position;
	}
	bool FileExists(const string &filename, optional_ptr<FileOpener> opener = nullptr) override {
		return files.count(filename) > 0;
	}

protected:
	unique_ptr<FileHandle> OpenFileExtended(const OpenFileInfo &file, FileOpenFlags flags,
	                                        optional_ptr<FileOpener> opener) override {
		FileOpenerInfo info(file);
		string key_id;
		string region;
		ReadSecret(opener, info, key_id, region);
		Record("open", file.path, key_id, region);
		if (!files.count(file.path) && flags.ReturnNullIfNotExists()) {
			return nullptr;
		}
		if (!files.count(file.path)) {
			throw IOException("No file \"%s\"", file.path);
		}
		return make_uniq<MemoryFileHandle>(*this, file.path, flags, key_id);
	}
	unique_ptr<MultiFileList> GlobFilesExtended(const string &pattern, const FileGlobInput &input,
	                                            optional_ptr<FileOpener> opener) override {
		FileOpenerInfo info(OpenFileInfo(pattern).WithSecretName(input.secret_name));
		string key_id;
		string region;
		ReadSecret(opener, info, key_id, region);
		Record("glob", pattern, key_id, region);
		auto secret_name = input.secret_name;
		auto listed_with = listing_secrets.find(listing_secrets.count(pattern) ? pattern : "");
		if (listed_with != listing_secrets.end()) {
			secret_name = listed_with->second;
		}
		auto star = pattern.find('*');
		auto listed_prefix = pattern.substr(0, star);
		auto suffix = star == string::npos ? string() : pattern.substr(pattern.rfind('*') + 1);
		vector<OpenFileInfo> result;
		for (auto &entry : files) {
			auto &path = entry.first;
			bool matches = star == string::npos
			                   ? path == pattern
			                   : StringUtil::StartsWith(path, listed_prefix) && StringUtil::EndsWith(path, suffix);
			if (!matches) {
				continue;
			}
			OpenFileInfo listed(path);
			listed.extended_info = make_shared_ptr<ExtendedOpenFileInfo>();
			if (!secret_name.empty()) {
				listed.extended_info->options[ExtendedOpenFileInfo::SECRET_OPTION] = Value(secret_name);
			}
			result.push_back(std::move(listed));
		}
		return make_uniq<SimpleMultiFileList>(std::move(result));
	}

private:
	void ReadSecret(optional_ptr<FileOpener> opener, FileOpenerInfo &info, string &key_id, string &region) {
		key_id = NO_KEY;
		region = NO_KEY;
		if (!opener) {
			return;
		}
		KeyValueSecretReader reader(*opener, &info, TEST_TYPE);
		Value value;
		if (reader.TryGetSecretKeyOrSetting("key_id", "file_credentials_test_key_id", value) && !value.IsNull()) {
			key_id = value.ToString();
		}
		if (reader.TryGetSecretKeyOrSetting("region", "file_credentials_test_region", value) && !value.IsNull()) {
			region = value.ToString();
		}
	}
	void Record(const string &kind, const string &path, const string &key_id, const string &region) {
		duckdb::lock_guard<duckdb::mutex> guard(lock);
		accesses.push_back(Access {kind, path, key_id, region});
	}
};

string ReadLocalFile(const string &path) {
	std::ifstream input(path, std::ios::binary);
	std::stringstream buffer;
	buffer << input.rdbuf();
	return buffer.str();
}

//! A database with the test secret type, mem:// files, and a secret matched to every mem:// path, which a read that
//! loses a file's ephemeral secret would use
struct CredentialsFixture {
	CredentialsFixture() : db(nullptr), con(db) {
		RegisterTestSecretType(*db.instance);
		auto fs_p = make_uniq<MemoryFileSystem>();
		fs = fs_p.get();
		db.instance->GetFileSystem().RegisterSubSystem(std::move(fs_p));
		fs->files["mem://bucket/table/a.bin"] = "abc";
		fs->files["mem://bucket/table/b.bin"] = "def";
		REQUIRE_NO_FAIL(con.Query("CREATE SECRET by_path (TYPE file_credentials_test, SCOPE 'mem://bucket/', "
		                          "KEY_ID 'path-key', REGION 'path-region')"));
		// names of ephemeral secrets take random bits from the crypto module that writes encrypted files; without
		// httpfs, that is mbedtls, which generates them only when forced
		REQUIRE_NO_FAIL(con.Query("SET force_mbedtls_unsafe = true"));
	}
	ClientContext &Context() {
		return *con.context;
	}
	//! Registers an ephemeral secret for mem://bucket/table/ in the transaction of `context`, whose vends return
	//! `key_id` and count in `vends`; returns its name
	string EphemeralSecret(ClientContext &context, const string &key_id, const char *type = TEST_TYPE) {
		return SecretManager::Get(context).RegisterEphemeralSecret(context, "mem://bucket/table/",
		                                                           [this, key_id, type](ClientContext &) {
			                                                           vends++;
			                                                           return TestSecret(key_id, type);
		                                                           });
	}
	string EphemeralSecret(const string &key_id, const char *type = TEST_TYPE) {
		return EphemeralSecret(Context(), key_id, type);
	}
	static shared_ptr<SecretEntry> GetEphemeralSecret(ClientContext &context, const string &name, const string &path) {
		return SecretManager::Get(context).GetEphemeralSecret(context, name, path);
	}
	static string KeyOf(const shared_ptr<SecretEntry> &entry) {
		return entry->secret->Cast<KeyValueSecret>().TryGetValue("key_id").ToString();
	}
	static OpenFileInfo File(const string &path, const string &name) {
		return OpenFileInfo(path).WithSecretName(name);
	}
	//! Open `file` through the client's file system and return the key id the open used
	string OpenKey(const OpenFileInfo &file) {
		auto handle = FileSystem::GetFileSystem(Context()).OpenFile(file, FileFlags::FILE_FLAGS_READ);
		return handle->Cast<MemoryFileHandle>().key_id;
	}
	//! Runs `scan` in a transaction with an ephemeral secret of key `key_id` on every listed file; returns the row
	//! count
	idx_t ScanWith(const string &scan, const string &key_id = "table-key") {
		con.BeginTransaction();
		fs->listing_secrets = {{"", EphemeralSecret(key_id)}};
		fs->ClearAccesses();
		auto result = con.Query(scan);
		fs->listing_secrets.clear();
		con.Rollback();
		REQUIRE_NO_FAIL(*result);
		return result->RowCount();
	}
	//! csv, and json and parquet when they are loaded
	duckdb::vector<string> Formats() {
		duckdb::vector<string> result {"csv"};
		for (auto format : {"json", "parquet"}) {
			if (db.ExtensionIsLoaded(format)) {
				result.push_back(format);
			}
		}
		return result;
	}
	//! Writes mem://bucket/table/t1.<format> and t2.<format>, each with a column i of 0 to 999
	void WriteFiles(const string &format) {
		string data = format == "csv" ? "i\n" : "";
		if (format == "parquet") {
			auto local = TestCreatePath("file_credentials_same_files.parquet");
			REQUIRE_NO_FAIL(con.Query("COPY (SELECT range AS i FROM range(1000)) TO '" + local + "' (FORMAT parquet)"));
			data = ReadLocalFile(local);
		}
		for (idx_t i = 0; format != "parquet" && i < 1000; i++) {
			data += format == "csv" ? to_string(i) + "\n" : "{\"i\": " + to_string(i) + "}\n";
		}
		fs->files["mem://bucket/table/t1." + format] = data;
		fs->files["mem://bucket/table/t2." + format] = data;
	}
	//! mem://bucket/table/<prefix>*.<format>: with "" and with "t" it lists the files WriteFiles wrote
	static string Glob(const string &format, const string &prefix) {
		return "mem://bucket/table/" + prefix + "*." + format;
	}
	static string Scan(const string &format, const string &glob) {
		return "SELECT sum(i) FROM read_" + format + "('" + glob + "')";
	}
	//! Runs the scan of `glob` `runs` times in one transaction of `c`, its files listed with one new ephemeral secret
	//! of `c` for `key_id`; returns how often the scans opened and read them
	std::pair<idx_t, idx_t> ScanAlone(Connection &c, const string &format, const string &glob, const string &key_id,
	                                  idx_t runs = 1) {
		c.BeginTransaction();
		fs->listing_secrets = {{glob, EphemeralSecret(*c.context, key_id)}};
		fs->ClearAccesses();
		duckdb::vector<duckdb::unique_ptr<QueryResult>> results;
		for (idx_t run = 0; run < runs; run++) {
			results.push_back(c.Query(Scan(format, glob)));
		}
		fs->listing_secrets.clear();
		c.Rollback();
		for (auto &result : results) {
			REQUIRE_NO_FAIL(*result);
			REQUIRE(result->Collection().GetValue(0, 0).ToString() == SAME_FILES_SUM);
		}
		return {fs->CountKey("open", key_id), fs->CountKey("read", key_id)};
	}
	//! Requires `key_id` to have opened files `opens` times and read them `reads` times since the accesses were cleared
	void RequireUses(const string &key_id, idx_t opens, idx_t reads) {
		INFO(key_id);
		REQUIRE(fs->CountKey("open", key_id) == opens);
		REQUIRE(fs->CountKey("read", key_id) == reads);
	}

	DuckDB db;
	Connection con;
	MemoryFileSystem *fs;
	std::atomic<idx_t> vends {0};
};

void RequireThrowsWith(const std::function<void()> &call, const string &message) {
	try {
		call();
	} catch (std::exception &ex) {
		ErrorData error(ex);
		INFO(error.Message());
		REQUIRE(StringUtil::Contains(error.Message(), message));
		return;
	}
	FAIL("expected an error containing: " << message);
}

} // namespace

TEST_CASE("An open with an ephemeral secret uses it, and no secret or setting chosen by path", "[file_credentials]") {
	CredentialsFixture f;
	REQUIRE_NO_FAIL(f.con.Query("SET file_credentials_test_region = 'setting-region'"));
	f.con.BeginTransaction();
	auto name = f.EphemeralSecret("table-key");
	REQUIRE(f.OpenKey(CredentialsFixture::File("mem://bucket/table/a.bin", name)) == "table-key");
	// the ephemeral secret sets no region: neither the secret matched to the path nor the setting fills it
	REQUIRE(f.fs->Accesses("open")[0].region == NO_KEY);
	// the same path without an ephemeral secret reads with the secret matched to it
	REQUIRE(f.OpenKey(OpenFileInfo("mem://bucket/table/a.bin")) == "path-key");
	REQUIRE(f.fs->Accesses("open")[1].region == "path-region");
	f.con.Rollback();
}

TEST_CASE("An ephemeral secret of another type fails the read, and a reader of several types takes any of them",
          "[file_credentials]") {
	CredentialsFixture f;
	f.con.BeginTransaction();
	auto name = f.EphemeralSecret("other-key", OTHER_TYPE);
	RequireThrowsWith([&]() { f.OpenKey(CredentialsFixture::File("mem://bucket/table/a.bin", name)); },
	                  "is a secret of type \"file_credentials_test_other\", which this read does not take");
	ClientContextFileOpener opener(f.Context());
	FileOpenerInfo info(CredentialsFixture::File("mem://bucket/table/a.bin", name));
	const char *with_other[] = {TEST_TYPE, "FILE_CREDENTIALS_TEST_OTHER"};
	KeyValueSecretReader reader(opener, &info, with_other, 2);
	REQUIRE(reader.GetSecretKey("key_id").ToString() == "other-key");
	f.con.Rollback();
}

TEST_CASE("A glob with an ephemeral secret lists with it, and each file it lists carries it", "[file_credentials]") {
	CredentialsFixture f;
	f.fs->files["mem://bucket/table/dir/x.parquet"] = "x";
	f.con.BeginTransaction();
	auto name = f.EphemeralSecret("table-key");
	auto &client_fs = FileSystem::GetFileSystem(f.Context());
	FileGlobInput input(FileGlobOptions::DISALLOW_EMPTY);
	input.secret_name = name;
	auto files = client_fs.GlobFiles("mem://bucket/table/*.bin", input);
	REQUIRE(files.size() == 2);
	REQUIRE(f.fs->Accesses("glob")[0].key_id == "table-key");
	for (auto &file : files) {
		REQUIRE(file.GetSecretName() == name);
		REQUIRE(f.OpenKey(file) == "table-key");
	}
	// WithSecretName copies the extended information, and keeps the ephemeral secret a file has
	auto own = f.EphemeralSecret("own-key");
	OpenFileInfo given("mem://bucket/table/a.bin");
	given.extended_info = make_shared_ptr<ExtendedOpenFileInfo>();
	given.extended_info->options["file_size"] = Value::UBIGINT(3);
	auto with = given.WithSecretName(name);
	REQUIRE(with.extended_info->options.count("file_size") == 1);
	REQUIRE(given.GetSecretName().empty());
	REQUIRE(with.WithSecretName(own).GetSecretName() == name);
	REQUIRE_FALSE(OpenFileInfo("mem://bucket/table/a.bin").WithSecretName("").extended_info);
	// the name is an option of the file, so it also travels in a file struct
	REQUIRE(OpenFileInfo::FromValue(with.ToValue(), Identifier("test")).GetSecretName() == name);
	f.con.Rollback();
}

TEST_CASE("read_csv, read_json and read_parquet open and read every listed file with its ephemeral secret",
          "[file_credentials]") {
	CredentialsFixture f;
	string csv = "i\n";
	string json;
	for (idx_t i = 0; i < 2000; i++) {
		csv += to_string(i) + "\n";
		json += "{\"i\": " + to_string(i) + "}\n";
	}
	f.fs->files["mem://bucket/table/t1.csv"] = csv;
	f.fs->files["mem://bucket/table/t2.csv"] = "i\n1\n2\n";
	f.fs->files["mem://bucket/table/t.json"] = json;
	duckdb::vector<std::pair<string, idx_t>> scans {{"SELECT i FROM read_csv('mem://bucket/table/t1.csv')", 2000},
	                                                {"SELECT i FROM read_csv('mem://bucket/table/*.csv')", 2002}};
	if (f.db.ExtensionIsLoaded("json")) {
		scans.emplace_back("SELECT i FROM read_json('mem://bucket/table/*.json')", 2000);
	}
	if (f.db.ExtensionIsLoaded("parquet")) {
		auto local = TestCreatePath("file_credentials_scan.parquet");
		REQUIRE_NO_FAIL(f.con.Query("COPY (SELECT range AS i FROM range(1000)) TO '" + local + "' (FORMAT parquet)"));
		auto data = ReadLocalFile(local);
		f.fs->files["mem://bucket/table/part=1/a.parquet"] = data;
		f.fs->files["mem://bucket/table/part=2/b.parquet"] = data;
		f.fs->files["mem://bucket/table/part=3/c.parquet"] = data;
		scans.emplace_back("SELECT i FROM read_parquet('mem://bucket/table/*/*.parquet')", 3000);
		// a filter on a partition prunes the listed files
		scans.emplace_back(
		    "SELECT i FROM read_parquet('mem://bucket/table/*/*.parquet', hive_partitioning = true) WHERE part = 2",
		    1000);
		for (auto function : {"parquet_metadata", "parquet_schema", "parquet_file_metadata"}) {
			scans.emplace_back(string("SELECT * FROM ") + function + "('mem://bucket/table/*/*.parquet')", 0);
		}
	}
	for (auto &scan : scans) {
		INFO(scan.first);
		auto rows = f.ScanWith(scan.first);
		if (scan.second) {
			REQUIRE(rows == scan.second);
		}
		REQUIRE(f.fs->Accesses("open").size() > 0);
		REQUIRE(f.fs->CountKey("open", "table-key") == f.fs->Accesses("open").size());
		REQUIRE(f.fs->CountKey("read", "table-key") == f.fs->Accesses("read").size());
		if (StringUtil::Contains(scan.first, "part = 2")) {
			// a pruned file is not read; the first is, for the schema
			for (auto &read : f.fs->Accesses("read")) {
				REQUIRE(read.path != "mem://bucket/table/part=3/c.parquet");
			}
		}
	}
}

TEST_CASE("An ephemeral secret serves only its client and its scope, until its transaction ends",
          "[file_credentials]") {
	CredentialsFixture f;
	Connection other(f.db);
	string outcome;
	SECTION("commit") {
		outcome = "COMMIT";
	}
	SECTION("rollback") {
		outcome = "ROLLBACK";
	}
	SECTION("failed statement") {
		outcome = "SELECT error('fails')";
	}
	const string path = "mem://bucket/table/a.bin";
	auto &context = f.Context();
	REQUIRE_NO_FAIL(f.con.Query("BEGIN"));
	auto name = f.EphemeralSecret("table-key");
	REQUIRE(CredentialsFixture::GetEphemeralSecret(context, name, path));
	for (auto &outside : {"mem://bucket/other/c.bin", "mem://bucket/table", "mem://bucket/table_archive/a.bin",
	                      "mem://bucket/table/../other/c.bin", "mem://bucket/table/x/..", "mem://bucket/table/..\\c"}) {
		INFO(outside);
		RequireThrowsWith([&]() { CredentialsFixture::GetEphemeralSecret(context, name, outside); },
		                  "cannot be used for");
	}
	// another connection of the database, outside a transaction and in one with ephemeral secrets of its own
	auto open_on_other = [&]() {
		FileSystem::GetFileSystem(*other.context)
		    .OpenFile(CredentialsFixture::File(path, name), FileFlags::FILE_FLAGS_READ);
	};
	RequireThrowsWith(open_on_other, "is not available");
	other.BeginTransaction();
	f.EphemeralSecret(*other.context, "other-key");
	RequireThrowsWith(open_on_other, "is not available");
	other.Rollback();
	// an opener without a client
	DatabaseFileOpener database_opener(*f.db.instance);
	FileOpenerInfo info(CredentialsFixture::File(path, name));
	RequireThrowsWith([&]() { KeyValueSecretReader reader(database_opener, &info, TEST_TYPE); }, "without a client");
	f.con.Query(outcome);
	if (context.transaction.HasActiveTransaction()) {
		REQUIRE_NO_FAIL(f.con.Query("ROLLBACK"));
	}
	RequireThrowsWith([&]() { CredentialsFixture::GetEphemeralSecret(context, name, path); }, "is not available");
	REQUIRE_FALSE(SecretManager::Get(context).RefreshEphemeralSecret(context, name));
	// a later transaction's ephemeral secrets take other names, and an open that names it fails rather than falling
	// back to the secret matched to the path
	f.con.BeginTransaction();
	REQUIRE(f.EphemeralSecret("later-key") != name);
	RequireThrowsWith([&]() { f.OpenKey(CredentialsFixture::File(path, name)); }, "is not available");
	f.con.Rollback();
	// ephemeral secrets for a transaction are registered only in one
	RequireThrowsWith([&]() { f.EphemeralSecret("table-key"); }, "an active transaction");
}

TEST_CASE("An ephemeral secret for the connection serves it in and outside transactions, and no other connection",
          "[file_credentials]") {
	CredentialsFixture f;
	Connection other(f.db);
	auto &context = f.Context();
	const string path = "mem://bucket/table/a.bin";
	auto for_connection = [&](const string &key_id) {
		return SecretManager::Get(context).RegisterEphemeralSecret(
		    context, "mem://bucket/table/",
		    [&f, key_id](ClientContext &) {
			    f.vends++;
			    return TestSecret(key_id);
		    },
		    EphemeralSecretLifetime::CONNECTION);
	};
	auto held = make_shared_ptr<idx_t>(0);
	// registered outside a transaction, and in one that rolls back
	auto name = for_connection("connection-key");
	f.con.BeginTransaction();
	auto in_transaction = for_connection("rolled-back-key");
	SecretManager::Get(context).RegisterEphemeralSecret(context, "mem://bucket/table/", [held](ClientContext &) {
		return TestSecret("table-key#" + to_string(*held));
	});
	REQUIRE(f.OpenKey(CredentialsFixture::File(path, name)) == "connection-key");
	f.con.Rollback();
	REQUIRE(f.OpenKey(CredentialsFixture::File(path, name)) == "connection-key");
	REQUIRE(f.OpenKey(CredentialsFixture::File(path, in_transaction)) == "rolled-back-key");
	// the next registration drops the ephemeral secrets of the transaction that ended, and keeps those of the
	// connection
	REQUIRE(held.use_count() == 2);
	f.con.BeginTransaction();
	f.EphemeralSecret("later-key");
	REQUIRE(held.use_count() == 1);
	REQUIRE(f.OpenKey(CredentialsFixture::File(path, name)) == "connection-key");
	f.con.Commit();
	// each vended once; a refresh needs no transaction
	REQUIRE(f.vends == 2);
	REQUIRE(SecretManager::Get(context).RefreshEphemeralSecret(context, name));
	REQUIRE(f.vends == 3);
	// it still serves only its scope, and only its connection
	RequireThrowsWith([&]() { CredentialsFixture::GetEphemeralSecret(context, name, "mem://bucket/other/c.bin"); },
	                  "cannot be used for");
	RequireThrowsWith(
	    [&]() {
		    FileSystem::GetFileSystem(*other.context)
		        .OpenFile(CredentialsFixture::File(path, name), FileFlags::FILE_FLAGS_READ);
	    },
	    "is not available");
}

TEST_CASE("Only a statement given the name of an ephemeral secret reads with it, and none can match, list or drop it",
          "[file_credentials]") {
	CredentialsFixture f;
	f.fs->files["mem://bucket/table/t1.csv"] = "i\n1\n";
	auto &context = f.Context();
	auto read_with = [&](const string &secret) {
		f.fs->ClearAccesses();
		return f.con.Query("SELECT i FROM read_csv([{'filename': 'mem://bucket/table/t1.csv', 'secret': '" + secret +
		                   "'}])");
	};
	f.con.BeginTransaction();
	auto name = f.EphemeralSecret("table-key");
	// 128 random bits, as hex digits
	REQUIRE(name.size() == 32);
	REQUIRE(name.find_first_not_of("0123456789abcdef") == string::npos);
	REQUIRE(f.EphemeralSecret("table-key") != name);
	auto result = f.con.Query("SELECT name FROM which_secret('mem://bucket/table/t1.csv', 'file_credentials_test')");
	REQUIRE(CHECK_COLUMN(result, 0, {"by_path"}));
	result = f.con.Query("SELECT string_agg(name, ',') FROM duckdb_secrets()");
	REQUIRE(CHECK_COLUMN(result, 0, {"by_path"}));
	REQUIRE_FALSE(
	    SecretManager::Get(context).GetSecretByName(CatalogTransaction::GetSystemCatalogTransaction(context), name));
	// a file struct that names the ephemeral secret reads the file with it
	result = read_with(name);
	REQUIRE(CHECK_COLUMN(result, 0, {1}));
	REQUIRE(f.fs->Accesses("open").size() > 0);
	REQUIRE(f.fs->CountKey("open", "table-key") == f.fs->Accesses("open").size());
	// last: the failure aborts the transaction
	RequireThrowsWith([&]() { f.con.Query("DROP SECRET \"" + name + "\" FROM transaction")->ThrowError(); },
	                  "non-existent transaction secret");
	f.con.Rollback();
	// one that names no ephemeral secret of its transaction fails, rather than reading with the secret matched to the
	// path, and the error does not repeat the name
	for (auto &other : {name, string(32, '0')}) {
		INFO(other);
		result = read_with(other);
		REQUIRE(result->HasError());
		REQUIRE(StringUtil::Contains(result->GetError(), "is not available"));
		REQUIRE_FALSE(StringUtil::Contains(result->GetError(), other));
		REQUIRE(f.fs->CountKey("open", "path-key") == 0);
	}
}

TEST_CASE("Opens of one path with two ephemeral secrets each use their own, also between opens by path",
          "[file_credentials]") {
	CredentialsFixture f;
	f.con.BeginTransaction();
	auto a = f.EphemeralSecret("key-a");
	auto b = f.EphemeralSecret("key-b");
	const string path = "mem://bucket/table/a.bin";
	REQUIRE(f.OpenKey(CredentialsFixture::File(path, a)) == "key-a");
	REQUIRE(f.OpenKey(CredentialsFixture::File(path, b)) == "key-b");
	REQUIRE(f.OpenKey(OpenFileInfo(path)) == "path-key");
	REQUIRE(f.OpenKey(CredentialsFixture::File(path, a)) == "key-a");
	REQUIRE(f.OpenKey(CredentialsFixture::File(path, b)) == "key-b");
	f.con.Rollback();
}

TEST_CASE("Two scans of the same files in one query each read them with their own ephemeral secret",
          "[file_credentials]") {
	CredentialsFixture f;
	for (auto &format : f.Formats()) {
		INFO(format);
		f.WriteFiles(format);
		auto glob_a = CredentialsFixture::Glob(format, "");
		auto glob_b = CredentialsFixture::Glob(format, "t");
		auto alone_a = f.ScanAlone(f.con, format, glob_a, "key-a");
		auto alone_b = f.ScanAlone(f.con, format, glob_b, "key-b");
		REQUIRE(alone_a.second > 0);
		f.con.BeginTransaction();
		f.fs->listing_secrets = {{glob_a, f.EphemeralSecret("key-a")}, {glob_b, f.EphemeralSecret("key-b")}};
		f.fs->ClearAccesses();
		auto result = f.con.Query("SELECT (" + CredentialsFixture::Scan(format, glob_a) + "), (" +
		                          CredentialsFixture::Scan(format, glob_b) + ")");
		f.fs->listing_secrets.clear();
		f.con.Rollback();
		REQUIRE_NO_FAIL(*result);
		REQUIRE(result->Collection().GetValue(0, 0).ToString() == SAME_FILES_SUM);
		REQUIRE(result->Collection().GetValue(1, 0).ToString() == SAME_FILES_SUM);
		// each scan opens and reads every file itself, as often as alone, and nothing reads by path
		f.RequireUses("key-a", alone_a.first, alone_a.second);
		f.RequireUses("key-b", alone_b.first, alone_b.second);
		REQUIRE(f.fs->Accesses("open").size() == alone_a.first + alone_b.first);
		REQUIRE(f.fs->Accesses("read").size() == alone_a.second + alone_b.second);
	}
}

TEST_CASE("Two connections reading the same files at once each read them with their own ephemeral secret",
          "[file_credentials]") {
	CredentialsFixture f;
	Connection other(f.db);
	constexpr idx_t RUNS = 5;
	for (auto &format : f.Formats()) {
		INFO(format);
		f.WriteFiles(format);
		auto scan_a = CredentialsFixture::Scan(format, CredentialsFixture::Glob(format, ""));
		auto scan_b = CredentialsFixture::Scan(format, CredentialsFixture::Glob(format, "t"));
		auto alone_a = f.ScanAlone(f.con, format, CredentialsFixture::Glob(format, ""), "key-a", RUNS);
		auto alone_b = f.ScanAlone(other, format, CredentialsFixture::Glob(format, "t"), "key-b", RUNS);
		f.con.BeginTransaction();
		other.BeginTransaction();
		f.fs->listing_secrets = {{CredentialsFixture::Glob(format, ""), f.EphemeralSecret("key-a")},
		                         {CredentialsFixture::Glob(format, "t"), f.EphemeralSecret(*other.context, "key-b")}};
		f.fs->ClearAccesses();
		duckdb::vector<duckdb::unique_ptr<QueryResult>> results_a;
		duckdb::vector<duckdb::unique_ptr<QueryResult>> results_b;
		std::thread thread_a([&]() {
			for (idx_t i = 0; i < RUNS; i++) {
				results_a.push_back(f.con.Query(scan_a));
			}
		});
		std::thread thread_b([&]() {
			for (idx_t i = 0; i < RUNS; i++) {
				results_b.push_back(other.Query(scan_b));
			}
		});
		thread_a.join();
		thread_b.join();
		f.fs->listing_secrets.clear();
		f.con.Rollback();
		other.Rollback();
		for (auto results : {&results_a, &results_b}) {
			for (auto &result : *results) {
				REQUIRE_NO_FAIL(*result);
				REQUIRE(result->Collection().GetValue(0, 0).ToString() == SAME_FILES_SUM);
			}
		}
		// a read that took the other connection's ephemeral secret would fail; one that lost its own would read by
		// path
		f.RequireUses("key-a", alone_a.first, alone_a.second);
		f.RequireUses("key-b", alone_b.first, alone_b.second);
		f.RequireUses("path-key", 0, 0);
	}
}

TEST_CASE("A scan with an ephemeral secret and a read by path of the same files both succeed, each with its own key",
          "[file_credentials]") {
	CredentialsFixture f;
	// the scan with the ephemeral secret ("secret") and the read by path ("path"), in this order
	vector<string> order;
	bool one_query = true;
	SECTION("one query, the scan first") {
		order = {"secret", "path"};
	}
	SECTION("one query, the read by path first") {
		order = {"path", "secret"};
	}
	SECTION("one transaction, the scan first") {
		order = {"secret", "path"};
		one_query = false;
	}
	SECTION("one transaction, the read by path first") {
		order = {"path", "secret"};
		one_query = false;
	}
	for (auto &format : f.Formats()) {
		INFO(format);
		f.WriteFiles(format);
		auto secret_glob = CredentialsFixture::Glob(format, "");
		auto path_glob = CredentialsFixture::Glob(format, "t");
		auto alone = f.ScanAlone(f.con, format, secret_glob, "key-a");
		vector<string> scans;
		for (auto &read : order) {
			scans.push_back(CredentialsFixture::Scan(format, read == "secret" ? secret_glob : path_glob));
		}
		vector<string> statements = scans;
		if (one_query) {
			statements = {"SELECT (" + scans[0] + "), (" + scans[1] + ")"};
		}
		f.con.BeginTransaction();
		f.fs->listing_secrets = {{secret_glob, f.EphemeralSecret("key-a")}};
		f.fs->ClearAccesses();
		for (auto &statement : statements) {
			INFO(statement);
			auto result = f.con.Query(statement);
			REQUIRE_NO_FAIL(*result);
			for (idx_t column = 0; column < result->ColumnCount(); column++) {
				REQUIRE(result->Collection().GetValue(column, 0).ToString() == SAME_FILES_SUM);
			}
		}
		f.fs->listing_secrets.clear();
		f.con.Rollback();
		// the scan opened and read every file itself, as often as alone
		f.RequireUses("key-a", alone.first, alone.second);
		// every other open and read was the read by path's, with the secret matched to the path
		REQUIRE(f.fs->Accesses("open").size() == alone.first + f.fs->CountKey("open", "path-key"));
		REQUIRE(f.fs->Accesses("read").size() == alone.second + f.fs->CountKey("read", "path-key"));
		// and the read by path read every file itself
		for (auto &file : {"t1.", "t2."}) {
			auto path = "mem://bucket/table/" + string(file) + format;
			INFO(path);
			bool read_by_path = false;
			for (auto &access : f.fs->Accesses("read")) {
				read_by_path = read_by_path || (access.path == path && access.key_id == "path-key");
			}
			REQUIRE(read_by_path);
		}
	}
}

TEST_CASE("An ephemeral secret vends on first use, once, also when several threads use it first",
          "[file_credentials]") {
	CredentialsFixture f;
	auto &context = f.Context();
	f.con.BeginTransaction();
	auto name =
	    SecretManager::Get(context).RegisterEphemeralSecret(context, "mem://bucket/table/", [&](ClientContext &) {
		    f.vends++;
		    std::this_thread::sleep_for(std::chrono::milliseconds(50));
		    return TestSecret("table-key");
	    });
	REQUIRE(f.vends == 0);
	vector<std::thread> threads;
	std::atomic<idx_t> served {0};
	for (idx_t i = 0; i < 8; i++) {
		threads.emplace_back([&]() {
			if (CredentialsFixture::KeyOf(
			        CredentialsFixture::GetEphemeralSecret(context, name, "mem://bucket/table/a.bin")) == "table-key") {
				served++;
			}
		});
	}
	for (auto &thread : threads) {
		thread.join();
	}
	REQUIRE(served == 8);
	REQUIRE(f.vends == 1);
	f.con.Rollback();
}

TEST_CASE("A failed vend throws and the next use vends again; a refresh replaces the secret, a failed one keeps it",
          "[file_credentials]") {
	CredentialsFixture f;
	auto &context = f.Context();
	f.con.BeginTransaction();
	bool fail = true;
	auto name =
	    SecretManager::Get(context).RegisterEphemeralSecret(context, "mem://bucket/table/", [&](ClientContext &) {
		    auto vend = ++f.vends;
		    if (fail) {
			    throw IOException("the vend failed");
		    }
		    return TestSecret("table-key#" + to_string(vend));
	    });
	auto key = [&]() {
		return CredentialsFixture::KeyOf(
		    CredentialsFixture::GetEphemeralSecret(context, name, "mem://bucket/table/a.bin"));
	};
	auto refresh = [&]() {
		return SecretManager::Get(context).RefreshEphemeralSecret(context, name);
	};
	REQUIRE_THROWS_AS(key(), IOException);
	fail = false;
	REQUIRE(key() == "table-key#2");
	auto before = CredentialsFixture::GetEphemeralSecret(context, name, "mem://bucket/table/a.bin");
	REQUIRE(refresh());
	REQUIRE(key() == "table-key#3");
	// a reader of the secret a refresh replaced keeps it
	REQUIRE(CredentialsFixture::KeyOf(before) == "table-key#2");
	fail = true;
	REQUIRE_THROWS_AS(refresh(), IOException);
	REQUIRE(key() == "table-key#3");
	f.con.Rollback();
}
