#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb.hpp"
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

#include <fstream>
#include <functional>
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

//! Registers TEST_TYPE and OTHER_TYPE, each created with a KEY_ID and a REGION
void RegisterTestSecretTypes(DatabaseInstance &instance) {
	ExtensionInfo extension_info {};
	ExtensionActiveLoad load_info {instance, extension_info, Identifier("file_credentials_test"), Identifier()};
	ExtensionLoader loader {load_info};
	for (auto type : {TEST_TYPE, OTHER_TYPE}) {
		SecretType secret_type;
		secret_type.name = Identifier(type);
		secret_type.deserializer = KeyValueSecret::Deserialize<KeyValueSecret>;
		secret_type.default_provider = "config";
		loader.RegisterSecretType(secret_type);
		CreateSecretFunction function = {type, "config", CreateTestSecret};
		function.named_parameters["key_id"] = LogicalType::VARCHAR;
		function.named_parameters["region"] = LogicalType::VARCHAR;
		loader.RegisterFunction(function);
	}
	auto &config = DBConfig::GetConfig(instance);
	config.AddExtensionOption("file_credentials_test_region", "region a read uses when no secret sets one",
	                          LogicalType::VARCHAR);
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
//! secret reader chose for it. A glob lists the files under the pattern's prefix, each naming the secret that
//! `listing_secrets` gives its pattern, as a catalog lists the files of a table
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
		FileOpenerInfo info(pattern);
		string key_id;
		string region;
		ReadSecret(opener, info, key_id, region);
		Record("glob", pattern, key_id, region);
		string secret_name;
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

//! A database with the test secret types, mem:// files, and the secret that reads of them by path use. Its scope,
//! mem://bucket/table/, is longer than that of the secrets the tests name, so a read that loses the name of its
//! secret reads with it, and the tests see that
struct CredentialsFixture {
	CredentialsFixture() : db(nullptr), con(db) {
		RegisterTestSecretTypes(*db.instance);
		auto fs_p = make_uniq<MemoryFileSystem>();
		fs = fs_p.get();
		db.instance->GetFileSystem().RegisterSubSystem(std::move(fs_p));
		fs->files["mem://bucket/table/a.bin"] = "abc";
		fs->files["mem://bucket/table/b.bin"] = "def";
		REQUIRE_NO_FAIL(con.Query("CREATE SECRET by_path (TYPE file_credentials_test, SCOPE 'mem://bucket/table/', "
		                          "KEY_ID 'path-key', REGION 'path-region')"));
	}
	ClientContext &Context() {
		return *con.context;
	}
	//! Creates the secret `name`, whose key id is its name too, for the paths under `scope`; returns the name
	string NamedSecret(const string &name, const string &scope = "mem://bucket/", const char *type = TEST_TYPE) {
		REQUIRE_NO_FAIL(con.Query("CREATE SECRET \"" + name + "\" (TYPE " + type + ", SCOPE '" + scope + "', KEY_ID '" +
		                          name + "')"));
		return name;
	}
	//! `path`, naming the secret `name` in its options
	static OpenFileInfo File(const string &path, const string &name) {
		OpenFileInfo file(path);
		file.extended_info = make_shared_ptr<ExtendedOpenFileInfo>();
		file.extended_info->options[ExtendedOpenFileInfo::SECRET_OPTION] = Value(name);
		return file;
	}
	//! Opens `file` through the file system of `c`, which must be in a transaction, and returns the key id the open
	//! used
	static string OpenKey(Connection &c, const OpenFileInfo &file) {
		auto handle = FileSystem::GetFileSystem(*c.context).OpenFile(file, FileFlags::FILE_FLAGS_READ);
		return handle->Cast<MemoryFileHandle>().key_id;
	}
	string OpenKey(const OpenFileInfo &file) {
		return OpenKey(con, file);
	}
	//! Runs `scan` with every listed file naming the secret `name`; returns the row count
	idx_t ScanWith(const string &scan, const string &name) {
		fs->listing_secrets = {{"", name}};
		fs->ClearAccesses();
		auto result = con.Query(scan);
		fs->listing_secrets.clear();
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
	//! Runs the scan of `glob` on `c` `runs` times, its files naming the secret `name`; returns how often the scans
	//! opened and read them
	std::pair<idx_t, idx_t> ScanAlone(Connection &c, const string &format, const string &glob, const string &name,
	                                  idx_t runs = 1) {
		fs->listing_secrets = {{glob, name}};
		fs->ClearAccesses();
		for (idx_t run = 0; run < runs; run++) {
			auto result = c.Query(Scan(format, glob));
			REQUIRE_NO_FAIL(*result);
			REQUIRE(result->Collection().GetValue(0, 0).ToString() == SAME_FILES_SUM);
		}
		fs->listing_secrets.clear();
		return {fs->CountKey("open", name), fs->CountKey("read", name)};
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

TEST_CASE("A file that names a secret reads with it instead of the secret matched to its path", "[file_credentials]") {
	CredentialsFixture f;
	REQUIRE_NO_FAIL(f.con.Query("SET file_credentials_test_region = 'setting-region'"));
	auto named = f.NamedSecret("named-key");
	const string path = "mem://bucket/table/a.bin";
	f.con.BeginTransaction();
	REQUIRE(f.OpenKey(OpenFileInfo(path)) == "path-key");
	REQUIRE(f.OpenKey(CredentialsFixture::File(path, named)) == "named-key");
	// the named secret sets no region: the setting fills it, as for a secret matched to the path
	REQUIRE(f.fs->Accesses("open")[1].region == "setting-region");
	f.con.Rollback();
	// an opener without a client finds the secret too
	DatabaseFileOpener database_opener(*f.db.instance);
	FileOpenerInfo info(CredentialsFixture::File(path, named));
	KeyValueSecretReader reader(database_opener, &info, TEST_TYPE);
	REQUIRE(reader.GetSecretKey("key_id").ToString() == "named-key");
	// the name is an option of the file, so it also travels in a file struct, and a statement can name a secret
	string name;
	REQUIRE(OpenFileInfo::FromValue(CredentialsFixture::File(path, named).ToValue(), Identifier("test"))
	            .extended_info->TryGetSecretName(name));
	REQUIRE(name == named);
	f.fs->files["mem://bucket/table/t1.csv"] = "i\n1\n";
	f.fs->ClearAccesses();
	auto result =
	    f.con.Query("SELECT i FROM read_csv([{'filename': 'mem://bucket/table/t1.csv', 'secret': '" + named + "'}])");
	REQUIRE(CHECK_COLUMN(result, 0, {1}));
	REQUIRE(f.fs->Accesses("open").size() > 0);
	REQUIRE(f.fs->CountKey("open", named) == f.fs->Accesses("open").size());
}

TEST_CASE("A named secret of another type fails the read, and a reader of several types takes any of them",
          "[file_credentials]") {
	CredentialsFixture f;
	auto other = f.NamedSecret("other-key", "mem://bucket/", OTHER_TYPE);
	f.con.BeginTransaction();
	RequireThrowsWith([&]() { f.OpenKey(CredentialsFixture::File("mem://bucket/table/a.bin", other)); },
	                  "is of type \"file_credentials_test_other\", which this read does not take");
	ClientContextFileOpener opener(f.Context());
	FileOpenerInfo info(CredentialsFixture::File("mem://bucket/table/a.bin", other));
	const char *with_other[] = {TEST_TYPE, "FILE_CREDENTIALS_TEST_OTHER"};
	KeyValueSecretReader reader(opener, &info, with_other, 2);
	REQUIRE(reader.GetSecretKey("key_id").ToString() == "other-key");
	f.con.Rollback();
}

TEST_CASE("A file that names a secret which does not exist, or whose scope does not cover it, fails rather than "
          "reading with the secret matched to its path",
          "[file_credentials]") {
	CredentialsFixture f;
	Connection other(f.db);
	const string path = "mem://bucket/table/a.bin";
	auto elsewhere = f.NamedSecret("elsewhere-key", "mem://bucket/other/");
	f.con.BeginTransaction();
	RequireThrowsWith([&]() { f.OpenKey(CredentialsFixture::File(path, "missing")); },
	                  "Secret 'missing' named by 'mem://bucket/table/a.bin' not found");
	RequireThrowsWith([&]() { f.OpenKey(CredentialsFixture::File(path, elsewhere)); },
	                  "Secret 'elsewhere-key' named by 'mem://bucket/table/a.bin' does not apply to it");
	// a secret of a transaction, which only C++ creates, serves only that transaction
	CreateSecretInput input;
	input.type = TEST_TYPE;
	input.provider = "config";
	input.name = Identifier("txn-key");
	input.scope = {"mem://bucket/"};
	input.options["key_id"] = Value("txn-key");
	input.on_conflict = OnCreateConflict::ERROR_ON_CONFLICT;
	input.persist_type = SecretPersistType::TRANSACTION;
	REQUIRE(SecretManager::Get(f.Context()).CreateSecret(f.Context(), input));
	REQUIRE(f.OpenKey(CredentialsFixture::File(path, "txn-key")) == "txn-key");
	other.BeginTransaction();
	RequireThrowsWith([&]() { CredentialsFixture::OpenKey(other, CredentialsFixture::File(path, "txn-key")); },
	                  "Secret 'txn-key' named by 'mem://bucket/table/a.bin' not found");
	other.Rollback();
	f.con.Rollback();
	f.con.BeginTransaction();
	RequireThrowsWith([&]() { f.OpenKey(CredentialsFixture::File(path, "txn-key")); },
	                  "Secret 'txn-key' named by 'mem://bucket/table/a.bin' not found");
	f.con.Rollback();
	// as when a statement names it in a file struct
	f.fs->files["mem://bucket/table/t1.csv"] = "i\n1\n";
	for (auto &name : {string("missing"), elsewhere, string("txn-key")}) {
		INFO(name);
		auto result = f.con.Query("SELECT i FROM read_csv([{'filename': 'mem://bucket/table/t1.csv', 'secret': '" +
		                          name + "'}])");
		REQUIRE(result->HasError());
		REQUIRE(StringUtil::Contains(result->GetError(), "Secret '" + name + "' named by"));
	}
	REQUIRE(f.fs->CountKey("open", "path-key") == 0);
}

TEST_CASE("Opens of one path that name two secrets each use their own, also between opens by path",
          "[file_credentials]") {
	CredentialsFixture f;
	auto a = f.NamedSecret("key-a");
	auto b = f.NamedSecret("key-b");
	const string path = "mem://bucket/table/a.bin";
	f.con.BeginTransaction();
	REQUIRE(f.OpenKey(CredentialsFixture::File(path, a)) == "key-a");
	REQUIRE(f.OpenKey(CredentialsFixture::File(path, b)) == "key-b");
	REQUIRE(f.OpenKey(OpenFileInfo(path)) == "path-key");
	REQUIRE(f.OpenKey(CredentialsFixture::File(path, a)) == "key-a");
	REQUIRE(f.OpenKey(CredentialsFixture::File(path, b)) == "key-b");
	f.con.Rollback();
}

TEST_CASE("read_csv, read_json and read_parquet open and read every listed file with the secret it names",
          "[file_credentials]") {
	CredentialsFixture f;
	auto named = f.NamedSecret("table-key");
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
		auto rows = f.ScanWith(scan.first, named);
		if (scan.second) {
			REQUIRE(rows == scan.second);
		}
		REQUIRE(f.fs->Accesses("open").size() > 0);
		REQUIRE(f.fs->CountKey("open", named) == f.fs->Accesses("open").size());
		REQUIRE(f.fs->CountKey("read", named) == f.fs->Accesses("read").size());
		if (StringUtil::Contains(scan.first, "part = 2")) {
			// a pruned file is not read; the first is, for the schema
			for (auto &read : f.fs->Accesses("read")) {
				REQUIRE(read.path != "mem://bucket/table/part=3/c.parquet");
			}
		}
	}
}

TEST_CASE("Two scans of the same files in one query each read them with the secret they name", "[file_credentials]") {
	CredentialsFixture f;
	auto a = f.NamedSecret("key-a");
	auto b = f.NamedSecret("key-b");
	for (auto &format : f.Formats()) {
		INFO(format);
		f.WriteFiles(format);
		auto glob_a = CredentialsFixture::Glob(format, "");
		auto glob_b = CredentialsFixture::Glob(format, "t");
		auto alone_a = f.ScanAlone(f.con, format, glob_a, a);
		auto alone_b = f.ScanAlone(f.con, format, glob_b, b);
		REQUIRE(alone_a.second > 0);
		f.fs->listing_secrets = {{glob_a, a}, {glob_b, b}};
		f.fs->ClearAccesses();
		auto result = f.con.Query("SELECT (" + CredentialsFixture::Scan(format, glob_a) + "), (" +
		                          CredentialsFixture::Scan(format, glob_b) + ")");
		f.fs->listing_secrets.clear();
		REQUIRE_NO_FAIL(*result);
		REQUIRE(result->Collection().GetValue(0, 0).ToString() == SAME_FILES_SUM);
		REQUIRE(result->Collection().GetValue(1, 0).ToString() == SAME_FILES_SUM);
		// each scan opens and reads every file itself, as often as alone, and nothing reads by path
		f.RequireUses(a, alone_a.first, alone_a.second);
		f.RequireUses(b, alone_b.first, alone_b.second);
		REQUIRE(f.fs->Accesses("open").size() == alone_a.first + alone_b.first);
		REQUIRE(f.fs->Accesses("read").size() == alone_a.second + alone_b.second);
	}
}

TEST_CASE("Two connections reading the same files at once each read them with the secret they name",
          "[file_credentials]") {
	CredentialsFixture f;
	Connection other(f.db);
	auto a = f.NamedSecret("key-a");
	auto b = f.NamedSecret("key-b");
	constexpr idx_t RUNS = 5;
	for (auto &format : f.Formats()) {
		INFO(format);
		f.WriteFiles(format);
		auto glob_a = CredentialsFixture::Glob(format, "");
		auto glob_b = CredentialsFixture::Glob(format, "t");
		auto alone_a = f.ScanAlone(f.con, format, glob_a, a, RUNS);
		auto alone_b = f.ScanAlone(other, format, glob_b, b, RUNS);
		f.fs->listing_secrets = {{glob_a, a}, {glob_b, b}};
		f.fs->ClearAccesses();
		duckdb::vector<duckdb::unique_ptr<QueryResult>> results_a;
		duckdb::vector<duckdb::unique_ptr<QueryResult>> results_b;
		std::thread thread_a([&]() {
			for (idx_t i = 0; i < RUNS; i++) {
				results_a.push_back(f.con.Query(CredentialsFixture::Scan(format, glob_a)));
			}
		});
		std::thread thread_b([&]() {
			for (idx_t i = 0; i < RUNS; i++) {
				results_b.push_back(other.Query(CredentialsFixture::Scan(format, glob_b)));
			}
		});
		thread_a.join();
		thread_b.join();
		f.fs->listing_secrets.clear();
		for (auto results : {&results_a, &results_b}) {
			for (auto &result : *results) {
				REQUIRE_NO_FAIL(*result);
				REQUIRE(result->Collection().GetValue(0, 0).ToString() == SAME_FILES_SUM);
			}
		}
		// a read that took the other scan's secret, or lost its own and read by path, would change the counts
		f.RequireUses(a, alone_a.first, alone_a.second);
		f.RequireUses(b, alone_b.first, alone_b.second);
		f.RequireUses("path-key", 0, 0);
	}
}

TEST_CASE("A scan of files that name a secret and a read by path of the same files both succeed, each with its own key",
          "[file_credentials]") {
	CredentialsFixture f;
	auto named = f.NamedSecret("key-a");
	// the scan of the files that name the secret ("secret") and the read by path ("path"), in this order
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
		auto alone = f.ScanAlone(f.con, format, secret_glob, named);
		vector<string> scans;
		for (auto &read : order) {
			scans.push_back(CredentialsFixture::Scan(format, read == "secret" ? secret_glob : path_glob));
		}
		vector<string> statements = scans;
		if (one_query) {
			statements = {"SELECT (" + scans[0] + "), (" + scans[1] + ")"};
		}
		f.con.BeginTransaction();
		f.fs->listing_secrets = {{secret_glob, named}};
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
		f.RequireUses(named, alone.first, alone.second);
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
