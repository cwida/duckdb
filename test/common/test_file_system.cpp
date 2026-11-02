#include "catch.hpp"
#include "duckdb/common/compressed_file_system.hpp"
#include "duckdb/common/multi_file/multi_file_list.hpp"
#include "duckdb/common/file_buffer.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/fstream.hpp"
#include "duckdb/common/local_file_system.hpp"
#include "duckdb/common/opener_file_system.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/virtual_file_system.hpp"
#include "duckdb/main/client_context_file_opener.hpp"
#include "duckdb/main/database_manager.hpp"
#include "test_helpers.hpp"
#if (!defined(_WIN32) && !defined(WIN32))
#define TEST_HAVE_SYMLINK
#include <unistd.h>
#else
#include "duckdb/common/windows.hpp"
#endif

#include <thread>

using namespace duckdb;

namespace {

class ExistingDirectoryFileSystem : public FileSystem {
public:
	string GetName() const override {
		return "ExistingDirectoryFileSystem";
	}

	bool DirectoryExists(const string &, optional_ptr<FileOpener>) override {
		return true;
	}

	void CreateDirectory(const string &, optional_ptr<FileOpener>) override {
		create_called = true;
	}

public:
	bool create_called = false;
};

class TestOpenerFileSystem : public OpenerFileSystem {
public:
	TestOpenerFileSystem(FileSystem &file_system_p, FileOpener &opener_p)
	    : file_system(file_system_p), opener(opener_p) {
	}

public:
	FileSystem &GetFileSystem() const override {
		return file_system;
	}

	optional_ptr<FileOpener> GetOpener() const override {
		return opener;
	}

private:
	FileSystem &file_system;
	FileOpener &opener;
};

void CreateDummyFile(string fname) {
	string normalized_string;
	if (StringUtil::StartsWith(fname, "file:///")) {
#ifdef _WIN32
		normalized_string = fname.substr(8);
#else
		normalized_string = fname.substr(7);
#endif

	} else if (StringUtil::StartsWith(fname, "file://localhost/")) {
#ifdef _WIN32
		normalized_string = fname.substr(18);
#else
		normalized_string = fname.substr(18);
#endif
	} else {
		normalized_string = fname;
	}

	ofstream outfile(normalized_string);
	outfile << "I_AM_A_DUMMY" << endl;
	outfile.close();
}

// Fake compress file handle and filesystem implementation, which doesn't contain any (de)compression functionality but
// just the interface compatibility.
class FakeCompressFileHandle : public FileHandle {
public:
	FakeCompressFileHandle(FileSystem &internal_file_system, duckdb::unique_ptr<FileHandle> internal_file_handle_p)
	    : FileHandle(internal_file_system, internal_file_handle_p->GetPath(), internal_file_handle_p->GetFlags()),
	      internal_file_handle(std::move(internal_file_handle_p)) {
	}

	void Close() override {
		internal_file_handle->Close();
	}

	duckdb::unique_ptr<FileHandle> internal_file_handle;
};
class FakeCompressFileSystem : public CompressedFileSystem {
public:
	duckdb::unique_ptr<FileHandle> OpenCompressedFile(QueryContext context, duckdb::unique_ptr<FileHandle> handle,
	                                                  bool write) override {
		(void)context; // Suppress compilation warning.
		(void)write;
		return make_uniq<FakeCompressFileHandle>(*this, std::move(handle));
	}

	FileCompressionType GetCompressionType() override {
		return FileCompressionType("fake_compress");
	}
	bool CanHandleFile(const string &fpath) override {
		return StringUtil::EndsWith(fpath, ".compress");
	}
	duckdb::unique_ptr<StreamWrapper> CreateStream() override {
		return nullptr;
	}
	idx_t InBufferSize() override {
		return 0;
	}
	idx_t OutBufferSize() override {
		return 0;
	}
	string GetName() const override {
		return "FakeCompress";
	}
};

} // namespace

TEST_CASE("Make sure the file:// protocol works as expected", "[file_system]") {
	duckdb::unique_ptr<FileSystem> fs = FileSystem::CreateLocal();
	auto dname = TestMakeAbsolute(TestCreatePath("TEST_DIR"), fs->GetWorkingDirectory());
	auto dname_converted_slashes = StringUtil::Replace(dname, "\\", "/");

	// handle differences between windows and linux
	if (StringUtil::StartsWith(dname_converted_slashes, "/")) {
		dname_converted_slashes = dname_converted_slashes.substr(1);
	}

	// Path of format file:///bla/bla on 'nix and file:///X:/bla/bla on Windows
	auto dname_triple_slash = string("file:///") + dname_converted_slashes;
	// Path of format file://localhost/bla/bla on 'nix and file://localhost/X:/bla/bla on Windows
	auto dname_localhost = string("file://localhost/") + dname_converted_slashes;
	auto dname_no_host = string("file:/") + dname_converted_slashes;

	string fname = "TEST_FILE";
	string fname2 = "TEST_FILE_TWO";

	if (fs->DirectoryExists(dname_triple_slash)) {
		fs->RemoveDirectory(dname_triple_slash);
	}

	fs->CreateDirectory(dname_triple_slash);
	REQUIRE(fs->DirectoryExists(dname_triple_slash));
	REQUIRE(!fs->FileExists(dname_triple_slash));

	// we can call this again and nothing happens
	fs->CreateDirectory(dname_triple_slash);

	auto fname_in_dir = fs->JoinPath(dname_triple_slash, fname);
	auto fname_in_dir2 = fs->JoinPath(dname_localhost, fname2);
	auto fname_in_dir3 = fs->JoinPath(dname_no_host, fname2);

	CreateDummyFile(fname_in_dir);
	REQUIRE(fs->FileExists(fname_in_dir));
	REQUIRE(!fs->DirectoryExists(fname_in_dir));

	size_t n_files = 0;
	REQUIRE(fs->ListFiles(dname_triple_slash, [&n_files](const string &path, bool) { n_files++; }));

	REQUIRE(n_files == 1);

	REQUIRE(fs->FileExists(fname_in_dir));
	REQUIRE(!fs->FileExists(fname_in_dir2));

	auto file_listing = fs->Glob(fs->JoinPath(dname_triple_slash, "*"));

	fs->MoveFile(fname_in_dir, fname_in_dir2);

	REQUIRE(!fs->FileExists(fname_in_dir));
	REQUIRE(fs->FileExists(fname_in_dir2));

	auto file_listing_after_move = fs->Glob(fs->JoinPath(dname_no_host, "*"));

	fs->RemoveDirectory(dname_triple_slash);

	REQUIRE(!fs->DirectoryExists(dname_triple_slash));
	REQUIRE(!fs->FileExists(fname_in_dir));
	REQUIRE(!fs->FileExists(fname_in_dir2));
}

TEST_CASE("Make sure file system operators work as advertised", "[file_system]") {
	duckdb::unique_ptr<FileSystem> fs = FileSystem::CreateLocal();
	auto dname = TestCreatePath("TEST_DIR");
	string fname = "TEST_FILE";
	string fname2 = "TEST_FILE_TWO";

	if (fs->DirectoryExists(dname)) {
		fs->RemoveDirectory(dname);
	}

	fs->CreateDirectory(dname);
	REQUIRE(fs->DirectoryExists(dname));
	REQUIRE(!fs->FileExists(dname));

	// we can call this again and nothing happens
	fs->CreateDirectory(dname);

	auto fname_in_dir = fs->JoinPath(dname, fname);
	auto fname_in_dir2 = fs->JoinPath(dname, fname2);

	CreateDummyFile(fname_in_dir);
	REQUIRE(fs->FileExists(fname_in_dir));
	REQUIRE(!fs->DirectoryExists(fname_in_dir));

	size_t n_files = 0;
	REQUIRE(fs->ListFiles(dname, [&n_files](const string &path, bool) { n_files++; }));

	REQUIRE(n_files == 1);

	REQUIRE(fs->FileExists(fname_in_dir));
	REQUIRE(!fs->FileExists(fname_in_dir2));

	fs->MoveFile(fname_in_dir, fname_in_dir2);

	REQUIRE(!fs->FileExists(fname_in_dir));
	REQUIRE(fs->FileExists(fname_in_dir2));

	fs->RemoveDirectory(dname);

	REQUIRE(!fs->DirectoryExists(dname));
	REQUIRE(!fs->FileExists(fname_in_dir));
	REQUIRE(!fs->FileExists(fname_in_dir2));
}

#ifndef _WIN32
TEST_CASE("Character devices use streaming file semantics", "[file_system]") {
	auto fs = FileSystem::CreateLocal();
	REQUIRE(fs->IsPipe("/dev/null"));
	auto handle = fs->OpenFile("/dev/null", FileFlags::FILE_FLAGS_WRITE);
	REQUIRE(handle->GetWriteMode() == FileWriteMode::SEQUENTIAL);
}
#endif

TEST_CASE("RemoveDirectoryExtended never removes directory contents in single mode", "[file_system]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &fs = FileSystem::GetFileSystem(*con.context);
	auto directory = TestCreatePath("try_remove_empty_directory");
	if (fs.DirectoryExists(directory)) {
		fs.RemoveDirectory(directory);
	}
	fs.CreateDirectory(directory);

	auto file = fs.JoinPath(directory, "keep");
	CreateDummyFile(file);
	REQUIRE(!fs.RemoveDirectoryExtended(directory, {RemoveDirectoryMode::SINGLE}));
	REQUIRE(fs.DirectoryExists(directory));
	REQUIRE(fs.FileExists(file));

	fs.RemoveFile(file);
	REQUIRE(fs.RemoveDirectoryExtended(directory, {RemoveDirectoryMode::SINGLE}));
	REQUIRE(!fs.DirectoryExists(directory));
	REQUIRE(!fs.RemoveDirectoryExtended(directory, {RemoveDirectoryMode::SINGLE}));
}

TEST_CASE("Test directory cleanup preserves a concurrently created sibling", "[file_system]") {
	class ConcurrentDirectoryFileSystem : public LocalFileSystem {
	public:
		explicit ConcurrentDirectoryFileSystem(string root_p) : root(std::move(root_p)) {
		}

		bool ListFilesExtended(const string &directory, const std::function<void(OpenFileInfo &)> &callback,
		                       optional_ptr<FileOpener> opener) override {
			auto result = LocalFileSystem::ListFilesExtended(directory, callback, opener);
			if (directory == root && !created_sibling) {
				created_sibling = true;
				auto sibling = JoinPath(root, "active_run");
				CreateDirectory(sibling);
				CreateDummyFile(JoinPath(sibling, "database.wal"));
			}
			return result;
		}

		string root;
		bool created_sibling = false;
	};

	auto root = TestCreatePath("cleanup_ancestor");
	ConcurrentDirectoryFileSystem fs(root);
	auto leaf = fs.JoinPath(root, "finished_run");
	fs.CreateDirectoriesRecursive(leaf);
	ReclaimTestDirectoryLevels(fs, leaf, {root, leaf});
	REQUIRE(fs.created_sibling);
	REQUIRE(!fs.DirectoryExists(leaf));
	REQUIRE(fs.FileExists(fs.JoinPath(root, "active_run/database.wal")));
	fs.RemoveDirectory(root);
}

TEST_CASE("Extended directory operations report target ownership", "[file_system]") {
	ExistingDirectoryFileSystem existing_directory_fs;
	REQUIRE(!existing_directory_fs.CreateDirectoryExtended("virtual_directory", {CreateDirectoryMode::SINGLE}));
	REQUIRE(!existing_directory_fs.create_called);
	REQUIRE(!existing_directory_fs.RemoveDirectoryExtended("virtual_directory", {RemoveDirectoryMode::SINGLE}));

	auto fs = FileSystem::CreateLocal();
	auto directory = TestCreatePath("try_create_directory");
	if (fs->DirectoryExists(directory)) {
		fs->RemoveDirectory(directory);
	}

	atomic<bool> start {false};
	atomic<idx_t> created {0};
	atomic<idx_t> errors {0};
	vector<std::thread> threads;
	for (idx_t i = 0; i < 8; i++) {
		threads.emplace_back([&]() {
			while (!start) {
				std::this_thread::yield();
			}
			try {
				if (fs->CreateDirectoryExtended(directory, {CreateDirectoryMode::SINGLE})) {
					created++;
				}
			} catch (...) {
				errors++;
			}
		});
	}
	start = true;
	for (auto &thread : threads) {
		thread.join();
	}

	REQUIRE(errors == 0);
	REQUIRE(created == 1);
	REQUIRE(!fs->CreateDirectoryExtended(directory, {CreateDirectoryMode::SINGLE}));
	REQUIRE(fs->RemoveDirectoryExtended(directory, {RemoveDirectoryMode::SINGLE}));

	auto recursive_parent = TestCreatePath("try_create_directory_recursive");
	if (fs->DirectoryExists(recursive_parent)) {
		fs->RemoveDirectory(recursive_parent);
	}
	auto recursive_leaf = fs->JoinPath(fs->JoinPath(recursive_parent, "nested"), "leaf");
	REQUIRE(fs->CreateDirectoryExtended(recursive_leaf, {CreateDirectoryMode::RECURSIVE}));
	REQUIRE(fs->DirectoryExists(recursive_leaf));
	REQUIRE(!fs->CreateDirectoryExtended(recursive_leaf, {CreateDirectoryMode::RECURSIVE}));
	REQUIRE(fs->RemoveDirectoryExtended(recursive_parent, {RemoveDirectoryMode::RECURSIVE}));
	REQUIRE(!fs->DirectoryExists(recursive_parent));

	auto file_path = TestCreatePath("try_create_directory_file");
	fs->TryRemoveFile(file_path);
	{ auto handle = fs->OpenFile(file_path, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW); }
	REQUIRE_THROWS(fs->CreateDirectoryExtended(file_path, {CreateDirectoryMode::SINGLE}));
	REQUIRE_THROWS(fs->RemoveDirectoryExtended(file_path, {RemoveDirectoryMode::SINGLE}));
	fs->RemoveFile(file_path);
}

TEST_CASE("Extended directory operations forward the file opener", "[file_system]") {
	DuckDB db(nullptr);
	Connection con(db);
	ClientContextFileOpener opener(*con.context);
	auto fs = FileSystem::CreateLocal();
	auto home_directory = TestCreatePath("extended_directory_opener");
	if (fs->DirectoryExists(home_directory)) {
		fs->RemoveDirectory(home_directory);
	}
	fs->CreateDirectory(home_directory);
	REQUIRE_NO_FAIL(con.Query(StringUtil::Format("SET home_directory = '%s'", home_directory)));

	auto directory = "~/parent/child";
	REQUIRE(fs->CreateDirectoryExtended(directory, {CreateDirectoryMode::RECURSIVE}, &opener));
	auto absolute_parent = fs->JoinPath(home_directory, "parent");
	auto file = fs->JoinPath(fs->JoinPath(absolute_parent, "child"), "file");
	CreateDummyFile(file);
	REQUIRE(fs->RemoveDirectoryExtended("~/parent", {RemoveDirectoryMode::RECURSIVE}, &opener));
	REQUIRE(!fs->DirectoryExists(absolute_parent));
	fs->RemoveDirectory(home_directory);
}

TEST_CASE("Recursive directory creation checks access for every ancestor", "[file_system]") {
	DuckDB db(nullptr);
	Connection con(db);
	ClientContextFileOpener opener(*con.context);
	auto fs = FileSystem::CreateLocal();
	TestOpenerFileSystem opener_fs(*fs, opener);
	auto base = TestCreatePath("extended_directory_permissions");
	if (fs->DirectoryExists(base)) {
		fs->RemoveDirectory(base);
	}
	auto allowed_root = fs->JoinPath(fs->JoinPath(base, "parent"), "allowed");
	REQUIRE_NO_FAIL(con.Query(StringUtil::Format("SET allowed_directories = ['%s']", allowed_root)));
	REQUIRE_NO_FAIL(con.Query("SET enable_external_access = false"));

	auto target = fs->JoinPath(allowed_root, "child");
	REQUIRE_THROWS(opener_fs.CreateDirectoryExtended(target, {CreateDirectoryMode::RECURSIVE}));
	REQUIRE(!fs->DirectoryExists(base));
}

// note: the integer count is chosen as 512 so that we write 512*8=4096 bytes to the file
// this is required for the Direct-IO as on Windows Direct-IO can only write multiples of sector sizes
// sector sizes are typically one of [512/1024/2048/4096] bytes, hence a 4096 bytes write succeeds.
#define INTEGER_COUNT 512

TEST_CASE("Test file operations", "[file_system]") {
	duckdb::unique_ptr<FileSystem> fs = FileSystem::CreateLocal();
	duckdb::unique_ptr<FileHandle> handle, handle2;
	int64_t test_data[INTEGER_COUNT];
	for (int i = 0; i < INTEGER_COUNT; i++) {
		test_data[i] = i;
	}

	auto fname = TestCreatePath("test_file");

	// standard reading/writing test

	// open file for writing
	REQUIRE_NOTHROW(handle = fs->OpenFile(fname, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE));
	// write 10 integers
	REQUIRE_NOTHROW(handle->Write(QueryContext(), (void *)test_data, sizeof(int64_t) * INTEGER_COUNT, 0));
	// close the file
	handle.reset();

	for (int i = 0; i < INTEGER_COUNT; i++) {
		test_data[i] = 0;
	}
	// now open the file for reading
	REQUIRE_NOTHROW(handle = fs->OpenFile(fname, FileFlags::FILE_FLAGS_READ));
	// Check file stats.
	const auto file_metadata = fs->Stats(*handle);
	REQUIRE(file_metadata.file_size == 4096);
	REQUIRE(file_metadata.file_type == FileType::FILE_TYPE_REGULAR);
	REQUIRE(file_metadata.last_modification_time > timestamp_t {-1});

	// read the 10 integers back
	REQUIRE_NOTHROW(handle->Read(QueryContext(), (void *)test_data, sizeof(int64_t) * INTEGER_COUNT, 0));
	// check the values of the integers
	for (int i = 0; i < 10; i++) {
		REQUIRE(test_data[i] == i);
	}
	handle.reset();
	fs->RemoveFile(fname);
}

TEST_CASE("GetStatsIfExists returns path metadata", "[file_system]") {
	auto fs = FileSystem::CreateLocal();
	auto fname = TestCreatePath("stats_if_exists");
	fs->TryRemoveFile(fname);

	vector<char> payload {'s', 't', 'a', 't', 's'};
	{
		auto handle = fs->OpenFile(fname, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW);
		handle->Write(payload.data(), payload.size());
		handle->Sync();
	}

	auto metadata = fs->GetStatsIfExists(fname);
	REQUIRE(metadata.has_value());
	REQUIRE(metadata->file_size == NumericCast<int64_t>(payload.size()));
	REQUIRE(metadata->file_type == FileType::FILE_TYPE_REGULAR);
	REQUIRE(metadata->last_modification_time > timestamp_t {-1});
	REQUIRE(!metadata->version_tag.empty());
	auto handle = fs->OpenFile(fname, FileFlags::FILE_FLAGS_READ);
	auto handle_metadata = handle->Stats();
	REQUIRE(metadata->file_id == handle_metadata.file_id);
	REQUIRE(metadata->version_tag == handle_metadata.version_tag);
	REQUIRE(!fs->GetStatsIfExists(fname + ".missing").has_value());

	fs->RemoveFile(fname);
}

TEST_CASE("absolute paths", "[file_system]") {
	duckdb::LocalFileSystem fs;

#ifndef _WIN32
	REQUIRE(fs.IsPathAbsolute("/home/me"));
	REQUIRE(!fs.IsPathAbsolute("./me"));
	REQUIRE(!fs.IsPathAbsolute("me"));
#else
	const std::string long_path = "\\\\?\\D:\\very long network\\";
	REQUIRE(fs.IsPathAbsolute(long_path));
	const std::string network = "\\\\network_drive\\filename.csv";
	REQUIRE(fs.IsPathAbsolute(network));
	REQUIRE(fs.IsPathAbsolute("C:\\folder\\filename.csv"));
	REQUIRE(fs.IsPathAbsolute("C:/folder\\filename.csv"));
#endif
}

TEST_CASE("Test RemoveFiles", "[file_system]") {
	duckdb::unique_ptr<FileSystem> fs = FileSystem::CreateLocal();
	auto dname = TestCreatePath("test_remove_files");

	if (fs->DirectoryExists(dname)) {
		fs->RemoveDirectory(dname);
	}
	fs->CreateDirectory(dname);

	auto file1 = fs->JoinPath(dname, "file1.txt");
	auto file2 = fs->JoinPath(dname, "file2.txt");
	auto file3 = fs->JoinPath(dname, "file3.txt");
	auto file4 = fs->JoinPath(dname, "file4.txt");
	CreateDummyFile(file1);
	CreateDummyFile(file2);
	CreateDummyFile(file3);

	REQUIRE(fs->FileExists(file1));
	REQUIRE(fs->FileExists(file2));
	REQUIRE(fs->FileExists(file3));
	REQUIRE(!fs->FileExists(file4));

	fs->RemoveFiles({file1, file2});
	REQUIRE(!fs->FileExists(file1));
	REQUIRE(!fs->FileExists(file2));
	REQUIRE(fs->FileExists(file3));
	REQUIRE(!fs->FileExists(file4));

	fs->RemoveDirectory(dname);
}

TEST_CASE("extract subsystem", "[file_system]") {
	duckdb::VirtualFileSystem vfs;
	auto local_filesystem = FileSystem::CreateLocal();
	auto *local_filesystem_ptr = local_filesystem.get();
	vfs.RegisterSubSystem(std::move(local_filesystem));

	// Extract a non-existent filesystem gets nullptr.
	REQUIRE(vfs.ExtractSubSystem("non-existent") == nullptr);

	// Extract an existing filesystem.
	auto extracted_filesystem = vfs.ExtractSubSystem(local_filesystem_ptr->GetName());
	REQUIRE(extracted_filesystem.get() == local_filesystem_ptr);

	// Re-extraction gets nullptr.
	REQUIRE(vfs.ExtractSubSystem("non-existent") == nullptr);

	// Register a subfilesystem and disable, which is not allowed to extract.
	const ::duckdb::string target_fs = extracted_filesystem->GetName();
	const ::duckdb::vector<string> disabled_subfilesystems {target_fs};
	vfs.RegisterSubSystem(std::move(extracted_filesystem));
	vfs.SetDisabledFileSystems(disabled_subfilesystems);
	REQUIRE(vfs.ExtractSubSystem(target_fs) == nullptr);
}

TEST_CASE("re-register subsystem", "[file_system]") {
	duckdb::VirtualFileSystem vfs;

	// First time registration should succeed.
	auto local_filesystem = FileSystem::CreateLocal();
	vfs.RegisterSubSystem(std::move(local_filesystem));

	// Re-register an already registered subfilesystem should throw.
	auto second_local_filesystem = FileSystem::CreateLocal();
	REQUIRE_THROWS(vfs.RegisterSubSystem(std::move(second_local_filesystem)));
}

struct CanonicalizationTest {
	CanonicalizationTest(string a_p, string b_p, string description_p)
	    : a(std::move(a_p)), b(std::move(b_p)), description(std::move(description_p)) {
	}

	string a;
	string b;
	string description;

	static void Test(const std::vector<CanonicalizationTest> &test_cases);
};

void CanonicalizationTest::Test(const std::vector<CanonicalizationTest> &test_cases) {
	auto fs = FileSystem::CreateLocal();
	std::vector<string> failures;
	for (auto &test : test_cases) {
		// test canonicalization
		auto canonical_a = fs->CanonicalizePath(test.a);
		auto canonical_b = fs->CanonicalizePath(test.b);

		if (canonical_a != canonical_b) {
			failures.emplace_back(StringUtil::Format("Canonical path mismatch %s <> %s (original: %s - %s)",
			                                         canonical_a, canonical_b, test.a, test.b));
		}
		if (!fs->IsPathAbsolute(test.a)) {
			// verify absolute and relative paths resolve to the same path
			auto abs_a = fs->JoinPath(fs->GetWorkingDirectory(), test.a);
			auto abs_b = fs->JoinPath(fs->GetWorkingDirectory(), test.b);
			auto canonical_abs_a = fs->CanonicalizePath(abs_a);
			auto canonical_abs_b = fs->CanonicalizePath(abs_b);
			if (canonical_abs_a != canonical_abs_b) {
				failures.emplace_back(StringUtil::Format("Canonical abs path mismatch %s <> %s (original: %s - %s)",
				                                         canonical_abs_a, canonical_abs_b, test.a, test.b));
			}
			// absolute path matches relative path
			if (canonical_abs_a != canonical_a) {
				failures.emplace_back(
				    StringUtil::Format("Canonical abs - relative path mismatch %s <> %s (original: %s)",
				                       canonical_abs_a, canonical_a, test.a));
			}
			REQUIRE(1 == 1);
		}
		REQUIRE(1 == 1);
	}
	if (!failures.empty()) {
		Printer::PrintF("------------- FAILURE -------------");
		for (auto &failure : failures) {
			Printer::PrintF("%s", failure);
		}
		FAIL();
	}
}

TEST_CASE("Test path canonicalization", "[file_system]") {
	std::vector<CanonicalizationTest> test_cases;
	test_cases.emplace_back("src/./common", "src/common", "single dot removal");
	test_cases.emplace_back("src/../common", "common", "parent directory removal");
	test_cases.emplace_back("src/common/..", "src", "trailing parent removal");
	test_cases.emplace_back("./src", "src", "leading dot removal");
	test_cases.emplace_back("src/.", "src", "trailing dot removal");
	test_cases.emplace_back("src/common/../../CMakeLists.txt", "CMakeLists.txt", "multiple parent traversal");
	test_cases.emplace_back("src/./common/./CMakeLists.txt", "src/common/CMakeLists.txt", "multiple dot removal");
	test_cases.emplace_back("./././src", "src", "repeated leading dots");
	test_cases.emplace_back("src/common/../../../CMakeLists.txt", "../CMakeLists.txt", "parent beyond root (relative)");
	test_cases.emplace_back("src//common", "src/common", "double slash");
	test_cases.emplace_back("src///common", "src/common", "triple slash");
	test_cases.emplace_back("src/common/", "src/common", "trailing slash");
	test_cases.emplace_back("src/common//", "src/common", "multiple trailing slashes");
	test_cases.emplace_back(".", ".", "single dot");
	test_cases.emplace_back("..", "..", "single parent");
	test_cases.emplace_back("", "", "empty path");
	test_cases.emplace_back("/", "/", "root only");
	test_cases.emplace_back("src", "src", "simple filename");
	test_cases.emplace_back("src/./common/../CMakeLists.txt/./qux/..", "src/CMakeLists.txt", "mixed operations");
	test_cases.emplace_back("./src/../common/./CMakeLists.txt", "common/CMakeLists.txt", "leading dot with parent");
	test_cases.emplace_back("a/b/c/../../d/e/../f", "a/d/f", "interleaved parents");

#ifdef _WIN32
	auto fs = FileSystem::CreateLocal();
	auto current_drive = fs->GetWorkingDirectory().substr(0, 1);

	// Backslash handling
	test_cases.emplace_back("src\\common", "src/common", "backslash separator equivalant to forward slash");
	test_cases.emplace_back("src\\..\\common", "common", "parent with backslash");
	test_cases.emplace_back("src/common\\CMakeLists.txt", "src\\common\\CMakeLists.txt", "mixed separators");

	// Drive letters
	test_cases.emplace_back("C:\\src\\common", "C:\\src\\common", "absolute with drive");
	test_cases.emplace_back("C:\\src\\..\\common", "C:\\common", "parent with drive");
	test_cases.emplace_back("C:src", "src", "drive-relative path");
	test_cases.emplace_back("C:src\\..\\file", "file", "drive-relative with dots");
	test_cases.emplace_back("C:", ".", "drive-relative root");
	test_cases.emplace_back("C:\\", "\\\\?\\C:\\", "drive root");
	test_cases.emplace_back("C:\\..\\common", "C:\\common", ".. in root of known drive");
	test_cases.emplace_back("Z:file", "Z:\\file", "drive-relative in unknown drive");
	test_cases.emplace_back("Z:\\src\\..\\common", "Z:\\common", "parent with drive in non-existent drive");
	test_cases.emplace_back("Z:\\..\\common", "Z:\\common", ".. in root of unknown drive");

	// UNC paths
	test_cases.emplace_back("\\\\server\\share", "\\\\server\\share", "UNC path");
	test_cases.emplace_back("\\\\server\\share\\src\\..\\common", "\\\\server\\share\\common", "UNC with parent");

	// if the current drive is not C:
	for (auto &test_case : test_cases) {
		test_case.a = StringUtil::Replace(test_case.a, "C:", current_drive + ":");
		test_case.b = StringUtil::Replace(test_case.b, "C:", current_drive + ":");
	}
#else
	test_cases.emplace_back("//src/common", "/src/common", "leading double slash");
#endif

	CanonicalizationTest::Test(test_cases);
}

void TestCreateSymlink(const string &source, const string &target, bool directory_link) {
#ifdef TEST_HAVE_SYMLINK
	if (symlink(target.c_str(), source.c_str()) != 0) {
		perror("symlink");
		FAIL();
	}
#else
	// windows - use native APIs
	if (!CreateSymbolicLinkA(source.c_str(), target.c_str(),
	                         (directory_link ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0) |
	                             SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
		std::cerr << "Error: " << GetLastError() << std::endl;
		FAIL();
	}
#endif
}

TEST_CASE("Test path canonicalization with symlinks", "[file_system]") {
	auto fs = FileSystem::CreateLocal();

	TestCreateDirectory("real_dir");
	auto real_dir = TestCreatePath("real_dir");
	auto test_dir = TestDirectoryPath();
	auto fake_dir = TestCreatePath("fake_dir");
	auto real_file = fs->JoinPath(real_dir, "real_file");

	TestCreateDirectory(real_dir);
	// create a real file
	{
		DuckDB db(real_file);
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE tbl AS SELECT 42 i"));
	}
	// create symlink from fake_dir -> real_dir
	TestCreateSymlink(fake_dir, fs->JoinPath(fs->GetWorkingDirectory(), real_dir), true);
	// create symlink from fake_file -> real_file
	TestCreateSymlink(fs->JoinPath(fake_dir, "fake_file"), fs->JoinPath(fs->GetWorkingDirectory(), real_file), false);

	// all of these tests are in the test dir
	std::vector<CanonicalizationTest> test_cases;
	test_cases.emplace_back("fake_dir", "real_dir", "base directory");
	test_cases.emplace_back("fake_dir/file", "real_dir/file", "follow directory symlink");
	test_cases.emplace_back("fake_dir/real_file", "real_dir/real_file", "follow directory symlink for real file");
	test_cases.emplace_back("./real_dir/.././fake_dir/.//file", "real_dir/file", "follow directory symlink with dots");
	test_cases.emplace_back("fake_dir/fake_file", "real_dir/real_file", "follow file symlink");
	test_cases.emplace_back("real_dir/.././fake_dir/././fake_file", "././real_dir/././real_file",
	                        "follow file symlink with dots");

#ifndef _WIN32
	// dangling symlinks resolve to the (non-existent) file they point to
	TestCreateSymlink(fs->JoinPath(real_dir, "dangling_link"),
	                  fs->JoinPath(fs->JoinPath(fs->GetWorkingDirectory(), real_dir), "missing_file"), false);
	TestCreateSymlink(fs->JoinPath(real_dir, "dangling_chain"),
	                  fs->JoinPath(fs->JoinPath(fs->GetWorkingDirectory(), real_dir), "dangling_link"), false);
	test_cases.emplace_back("real_dir/dangling_link", "real_dir/missing_file", "follow dangling symlink");
	test_cases.emplace_back("fake_dir/dangling_link", "real_dir/missing_file",
	                        "follow dangling symlink in symlink dir");
	test_cases.emplace_back("real_dir/dangling_chain", "real_dir/missing_file", "follow chain of dangling symlinks");
#endif

	// prepend everything with the test dir
	for (auto &test_case : test_cases) {
		test_case.a = fs->JoinPath(test_dir, test_case.a);
		test_case.b = fs->JoinPath(test_dir, test_case.b);
	}

	CanonicalizationTest::Test(test_cases);
}

TEST_CASE("filesystem concurrent access and deletion", "[file_system]") {
	auto fs = FileSystem::CreateLocal();

	auto fname = TestCreatePath("concurrent_delete_test_file");
	const string payload = "DELETE_WHILE_OPEN_CONTENT";

	// Open first handle to create/write, then close it.
	auto write_handle = fs->OpenFile(fname, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE);
	write_handle->Write(QueryContext(), static_cast<void *>(const_cast<char *>(payload.c_str())), payload.size(),
	                    /*location=*/0);
	write_handle->Sync();
	write_handle.reset();
	REQUIRE(fs->FileExists(fname));

	// Open second handle for reading.
	auto read_handle = fs->OpenFile(fname, FileFlags::FILE_FLAGS_READ);

	// Delete the file while the read handle is still open.
	fs->RemoveFile(fname);
	REQUIRE(!fs->FileExists(fname));

	// Reading from the already-open read handle should still return the content.
	string read_back(payload.size(), '\0');
	read_handle->Read(QueryContext(), static_cast<void *>(const_cast<char *>(read_back.data())), payload.size(),
	                  /*location=*/0);
	REQUIRE(read_back == payload);

	// Close the remaining handle; the file should not exist.
	read_handle.reset();
	REQUIRE(!fs->FileExists(fname));
}

TEST_CASE("Moving over an open file preserves existing handles", "[file_system]") {
	auto fs = FileSystem::CreateLocal();
	auto source = TestCreatePath("move_over_open_file_source");
	auto target = TestCreatePath("move_over_open_file_target");
	const vector<char> source_data {'s', 'o', 'u', 'r', 'c', 'e'};
	const vector<char> target_data {'t', 'a', 'r', 'g', 'e', 't'};

	auto write = [&](const string &path, vector<char> data) {
		auto handle = fs->OpenFile(path, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE);
		handle->Write(data.data(), data.size());
	};
	auto read = [](FileHandle &handle, idx_t size) {
		vector<char> data(size);
		handle.Read(data.data(), data.size());
		return data;
	};

	fs->TryRemoveFile(source);
	fs->TryRemoveFile(target);
	write(source, source_data);
	write(target, target_data);

	auto open_target = fs->OpenFile(target, FileFlags::FILE_FLAGS_READ);
	fs->MoveFile(source, target);

	REQUIRE(!fs->FileExists(source));
	REQUIRE(fs->FileExists(target));
	REQUIRE(read(*open_target, target_data.size()) == target_data);
	REQUIRE(read(*fs->OpenFile(target, FileFlags::FILE_FLAGS_READ), source_data.size()) == source_data);

	open_target.reset();
	fs->RemoveFile(target);
}

TEST_CASE("exclusive create fails when the file exists", "[file_system]") {
	auto fs = FileSystem::CreateLocal();
	auto fname = TestCreatePath("exclusive_create_test_file");
	for (auto create_flag : {FileFlags::FILE_FLAGS_FILE_CREATE, FileFlags::FILE_FLAGS_FILE_CREATE_NEW}) {
		for (auto null_if_exists : {false, true}) {
			CAPTURE(create_flag.GetFlagsInternal(), null_if_exists);
			fs->TryRemoveFile(fname);
			auto flags = FileFlags::FILE_FLAGS_WRITE | create_flag | FileFlags::FILE_FLAGS_EXCLUSIVE_CREATE;
			if (null_if_exists) {
				flags |= FileFlags::FILE_FLAGS_NULL_IF_EXISTS;
			}

			// Both creation modes must succeed on a new path, including with NULL_IF_EXISTS.
			string payload = "EXCLUSIVE_CREATE_ORIGINAL_CONTENT";
			auto handle = fs->OpenFile(fname, flags);
			REQUIRE(handle != nullptr);
			handle->Write(QueryContext(), payload.data(), payload.size(), 0);
			handle->Sync();
			handle.reset();
			REQUIRE(fs->FileExists(fname));

			if (null_if_exists) {
				REQUIRE(fs->OpenFile(fname, flags) == nullptr);
			} else {
				REQUIRE_THROWS_AS(fs->OpenFile(fname, flags), IOException);
			}

			// A collision must not truncate or alter the existing file.
			string read_back(payload.size(), '\0');
			auto read_handle = fs->OpenFile(fname, FileFlags::FILE_FLAGS_READ);
			REQUIRE(read_handle->GetFileSize() == payload.size());
			read_handle->Read(QueryContext(), read_back.data(), read_back.size(), 0);
			REQUIRE(read_back == payload);
			read_handle.reset();

			// A regular file cannot be a parent directory; NULL_IF_EXISTS must not hide this error.
			REQUIRE_THROWS_AS(fs->OpenFile(fs->JoinPath(fname, "child"), flags), IOException);

			auto reopen = fs->OpenFile(fname, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE);
			REQUIRE(reopen != nullptr);
			reopen.reset();

			fs->RemoveFile(fname);
			REQUIRE(!fs->FileExists(fname));
		}
	}
}

// ------------------------------------------------------------------------------------------------
// Path struct tests
// ------------------------------------------------------------------------------------------------

TEST_CASE("Path parses and correctly structures fields", "[file_system]") {
	Path output;

	SECTION("local files") {
		output = Path::FromString("a/b");
		CHECK(output.GetScheme() == "");
		CHECK(output.GetAuthority() == "");
		CHECK(output.GetAnchor() == "");
		CHECK(output.IsAbsolute() == false);
		CHECK(output.GetPath() == "a/b");

		output = Path::FromString("/..////a/./b/../c");
		CHECK(output.GetScheme() == "");
		CHECK(output.GetAuthority() == "");
		CHECK(output.GetAnchor() == "/");
		CHECK(output.GetPath() == "a/c");
		CHECK(output.IsAbsolute() == true);
	}

	SECTION("file schemes") {
		output = Path::FromString("file:/a/b");
		CHECK(output.GetScheme() == "file:");
		CHECK(output.GetAuthority() == "");
		CHECK(output.GetAnchor() == "/");
		CHECK(output.IsAbsolute() == true);
		CHECK(output.GetPath() == "a/b");

		output = Path::FromString("file://localhost/a/b");
		CHECK(output.GetScheme() == "file://");
		CHECK(output.GetAuthority() == "localhost");
		CHECK(output.GetAnchor() == "/");
		CHECK(output.IsAbsolute() == true);
		CHECK(output.GetPath() == "a/b");

		output = Path::FromString("file:///a/b");
		CHECK(output.GetScheme() == "file://");
		CHECK(output.GetAuthority() == "");
		CHECK(output.GetAnchor() == "/");
		CHECK(output.IsAbsolute() == true);
		CHECK(output.GetPath() == "a/b");

		output = Path::FromString("file://LOCALHOST/a/b");
		CHECK(output.GetScheme() == "file://");
		CHECK(output.GetAuthority() == "LOCALHOST");
		CHECK(output.GetAnchor() == "/");
		CHECK(output.GetPath() == "a/b");

#if defined(_WIN32)
		output = Path::FromString(R"(c:..\foo)");
		CHECK(output.GetScheme() == "");
		CHECK(output.GetAuthority() == "");
		CHECK(output.GetAnchor() == "C:");
		CHECK(output.IsAbsolute() == false);
		CHECK(output.GetPath() == R"(..\foo)");

		output = Path::FromString(R"(c:\..\foo)");
		CHECK(output.GetScheme() == "");
		CHECK(output.GetAuthority() == "");
		CHECK(output.GetAnchor() == R"(C:\)");
		CHECK(output.IsAbsolute() == true);
		CHECK(output.GetPath() == "foo");
#endif
	}

	SECTION("URI schemes") {
		output = Path::FromString("az://container/b/c");
		CHECK(output.GetScheme() == "az://");
		CHECK(output.GetAuthority() == "container");
		CHECK(output.GetAnchor() == "/");
		CHECK(output.IsAbsolute());
		CHECK(output.GetPath() == "b/c");
	}

#if defined(_WIN32)
	SECTION("UNC schemes") {
		output = Path::FromString(R"(\\foo\bar)");
		CHECK(output.GetScheme() == R"(\\)");
		CHECK(output.GetAuthority() == R"(foo\bar)");
		CHECK(output.GetAnchor() == R"(\)");
		CHECK(output.GetPath().empty());

		CHECK_THROWS(Path::FromString(R"(\\\\ab)"));
		CHECK_THROWS(Path::FromString(R"(\\foo\)"));
		CHECK_THROWS(Path::FromString(R"(\\foo\\)"));
	}
#endif
}

TEST_CASE("Path::FromString/ToString round-trips", "[file_system]") {
	using std::make_tuple;

	enum ResultType { ERR = false, OK_ = true };

	ResultType return_exp;
	std::string input;
	std::string output_exp;

	// L() converts / to the local separator (mirrors playground L helper)
	auto L = [](const string &path) {
		string out(path);
#ifdef _WIN32
		for (size_t pos = 0; (pos = out.find('/', pos)) != string::npos; pos++) {
			out[pos] = '\\';
		}
#endif
		return out;
	};

	// clang-format off
	SECTION("local files") {
		std::tie(return_exp, input, output_exp) = GENERATE(table<ResultType, std::string, std::string>({
		    make_tuple(OK_, "",         "."),
		    make_tuple(OK_, "a",        "a"),
		    make_tuple(OK_, "a/b",      "a/b"),
		    make_tuple(OK_, ".",        "."),
		    make_tuple(OK_, "..",       ".."),
		    make_tuple(OK_, "/",        "/"),
		    make_tuple(OK_, "/a",       "/a"),
		    make_tuple(OK_, "/a/b",     "/a/b"),
		    make_tuple(OK_, "/a/b/",    "/a/b/"),

		    // backslash ok separator on all platforms; confirm output sep = first sep in input
		    make_tuple(OK_, R"(a\b)",             "a/b"),
		    make_tuple(OK_, R"(/a\b\c)",          "/a/b/c"),
		    make_tuple(OK_, R"(/data\csv\*.csv)", "/data/csv/*.csv"),

		    make_tuple(OK_, "foo/bar://baz",  "foo/bar:/baz"),
		    make_tuple(OK_, "/foo/bar://baz", "/foo/bar:/baz"),
		}));
	}

#if defined(_WIN32)
	SECTION("local files (windows drive)") {
		std::tie(return_exp, input, output_exp) = GENERATE(table<ResultType, std::string, std::string>({
		    make_tuple(OK_, "C:",   "C:"),
		    make_tuple(OK_, "c:b",  "C:b"),
		    make_tuple(OK_, "c:/b", "C:/b"),
		}));
	}
#endif

	SECTION("file schemes") {
		std::tie(return_exp, input, output_exp) = GENERATE(table<ResultType, std::string, std::string>({
		    make_tuple(OK_, "file:/",       "file:/"),
		    make_tuple(OK_, "file:/a",      "file:/a"),
		    make_tuple(OK_, "file:/a/b",    "file:/a/b"),

		    make_tuple(OK_, "file://localhost",     "file://localhost/"),
		    make_tuple(OK_, "file://localhost/",    "file://localhost/"),
		    make_tuple(OK_, "file://localhost/a",   "file://localhost/a"),
		    make_tuple(OK_, "file://localhost/a/b", "file://localhost/a/b"),

		    make_tuple(ERR, "file://otherhost/a/b", ""),
		    make_tuple(ERR, "file://otherhost",     ""),
		    make_tuple(ERR, "file://",              ""),

		    make_tuple(OK_, "file:///",     "file:///"),
		    make_tuple(OK_, "file:///a",    "file:///a"),
		    make_tuple(OK_, "file:///a/b",  "file:///a/b"),
		}));
	}

#if defined(_WIN32)
	SECTION("file schemes (windows drive)") {
		std::tie(return_exp, input, output_exp) = GENERATE(table<ResultType, std::string, std::string>({
		    make_tuple(OK_, "file:/c:/b",               "file:/C:/b"),
		    make_tuple(OK_, "file://localhost/c://b",   "file://localhost/C:/b"),
		    make_tuple(OK_, "file:///c:///b",           "file:///C:/b"),
		    make_tuple(OK_, "file://localhost//c://b",  "file://localhost/c:/b"),
		    make_tuple(OK_, "file://localhost/c://localhost/b", "file://localhost/C:/localhost/b"),
		}));
	}
#endif

	SECTION("URI schemes") {
		std::tie(return_exp, input, output_exp) = GENERATE(table<ResultType, std::string, std::string>({
		    make_tuple(OK_, "S3://bucket/bar/baz",     "s3://bucket/bar/baz"),
		    make_tuple(OK_, "s3://bucket/bar/baz",     "s3://bucket/bar/baz"),
		    make_tuple(OK_, "s3://bucket/bar/../baz",  "s3://bucket/baz"),
		    make_tuple(OK_, "s3://bucket/..",          "s3://bucket/"),
		    make_tuple(OK_, "s3://bucket/../..",       "s3://bucket/"),
		    make_tuple(OK_, "s3://bucket/c:/B/",       "s3://bucket/c:/B/"),
		}));
	}
	// clang-format on

	CAPTURE(input);
	if (return_exp == OK_) {
		CHECK(L(Path::FromString(input).ToString()) == L(output_exp));
	} else {
		CHECK_THROWS(Path::FromString(input).ToString());
	}
}

TEST_CASE("Path::JoinPath table-based tests", "[file_system]") {
	using std::make_tuple;

	enum ResultType { ERR = false, OK_ = true };

	ResultType return_exp;
	std::string lhs, rhs, joined_exp;

	auto L = [](const string &path) {
		string out(path);
#ifdef _WIN32
		for (size_t pos = 0; (pos = out.find('/', pos)) != string::npos; pos++) {
			out[pos] = '\\';
		}
#endif
		return out;
	};

	auto do_join = [](const string &a, const string &b) {
		return Path::FromString(a).Join(b).ToString();
	};

	// clang-format off
	SECTION("local files") {
		std::tie(return_exp, lhs, rhs, joined_exp) = GENERATE(table<ResultType, std::string, std::string, std::string>({
		    make_tuple(OK_, "",   "",  "."),
		    make_tuple(OK_, "",   ".", "."),
		    make_tuple(OK_, ".",  "",  "."),
		    make_tuple(OK_, ".",  ".", "."),
		    make_tuple(OK_, "",   "a", "a"),
		    make_tuple(OK_, "a",  "",  "a"),
		    make_tuple(OK_, "a",  "b", "a/b"),
		    make_tuple(OK_, "/.", ".", "/"),
		    make_tuple(OK_, "/",  "a", "/a"),
		    make_tuple(OK_, "/a", "",  "/a"),
		    make_tuple(OK_, "/a", "b", "/a/b"),

		    // abs path sub-path helper
		    make_tuple(OK_, "/",  "/a",     "/a"),
		    make_tuple(OK_, "/",  "/a/b/c", "/a/b/c"),
		    make_tuple(OK_, "/a", "/a",     "/a"),
		    make_tuple(OK_, "/a", "/a/b",   "/a/b"),

		    // abs path fails
		    make_tuple(ERR, "/a", "/",  ""),
		    make_tuple(ERR, "/a", "/b", ""),

		    // extra slashes
		    make_tuple(OK_, "dir//sub/", "./file",      "dir/sub/file"),
		    make_tuple(OK_, "dir/sub", "../sibling",    "dir/sibling"),
		    make_tuple(OK_, "dir///", "nested///child", "dir/nested/child"),

		    // single & double dots
		    make_tuple(OK_, "/", "./..",         "/"),
		    make_tuple(OK_, "", "./..",          ".."),
		    make_tuple(OK_, "..", "a/..",        ".."),
		    make_tuple(OK_, "..", "../a",        "../../a"),
		    make_tuple(OK_, "./..", "./..",      "../.."),
		    make_tuple(OK_, "a/bar", "..",       "a"),
		    make_tuple(OK_, "a/bar", "../..",    "."),
		    make_tuple(OK_, "a/bar", "../../..", ".."),

		    make_tuple(OK_, "./a/././b/./", "././c/././", "a/b/c/"),

		    make_tuple(OK_, "dir/sub/..", "sibling",        "dir/sibling"),
		    make_tuple(OK_, "./dir/sub/..", "sibling",      "dir/sibling"),
		    make_tuple(OK_, "./dir/sub/./..", "sibling",    "dir/sibling"),
		    make_tuple(OK_, "dir/..", "../..",              "../.."),
		    make_tuple(OK_, "/usr/local", "../..",          "/"),
		    make_tuple(OK_, "/", "usr/local",               "/usr/local"),
		    make_tuple(OK_, "/usr/local", "../../..",       "/"),

		    make_tuple(ERR, "dir", "/abs/path", ""),
		    make_tuple(ERR, "/fo",  "/foobar",  ""),

		    // scheme-like token embedded after leading slash is treated as path, not scheme
		    make_tuple(OK_, "/foo/proto://bar", "a", "/foo/proto:/bar/a"),
		}));
	}

	SECTION("file and URI schemes") {
		std::tie(return_exp, lhs, rhs, joined_exp) = GENERATE(table<ResultType, std::string, std::string, std::string>({
		    make_tuple(OK_, "file:/", "",                   "file:/"),
		    make_tuple(OK_, "file:/", "../..",              "file:/"),
		    make_tuple(OK_, "file:/", "bin",                "file:/bin"),
		    make_tuple(OK_, "file:/usr", "",                "file:/usr"),
		    make_tuple(OK_, "file:/usr", "bin",             "file:/usr/bin"),
		    make_tuple(OK_, "file:/usr/local", "../bin",    "file:/usr/bin"),

		    make_tuple(OK_, "file://localhost/usr", "../bin",       "file://localhost/bin"),
		    make_tuple(OK_, "file://localhost/usr/local", "../bin", "file://localhost/usr/bin"),
		    make_tuple(OK_, "file:///usr/local", "../bin",          "file:///usr/bin"),

		    make_tuple(OK_, "s3://host", "bar/baz", "s3://host/bar/baz"),
		    make_tuple(OK_, "s3://host", "..",      "s3://host/"),
		    make_tuple(OK_, "s3://host", "../..",   "s3://host/"),

		    make_tuple(ERR, "/usr/local", "/var/log",   ""),
		    make_tuple(ERR, "s3://foo", "/foo/bar/baz", ""),
		    make_tuple(ERR, "s3://foo", "az://foo",     ""),

		    make_tuple(ERR, "s3://b/foo",      "s3://b/foobar",       ""),
		    make_tuple(ERR, "s3://BUCKET/foo", "s3://bucket/foo/bar", ""),
		    make_tuple(ERR, "s3://bucket/FOO", "s3://bucket/foo/bar", ""),

		    // sub-path joins
		    make_tuple(OK_, "file:/usr",             "file:/usr/local",            "file:/usr/local"),
		    make_tuple(OK_, "file://localhost/usr",  "file://localhost/usr/local", "file://localhost/usr/local"),
		    make_tuple(OK_, "file:///usr",           "file:///usr/local",          "file:///usr/local"),
		    make_tuple(OK_, "s3://bucket/a",         "s3://bucket/a/b",            "s3://bucket/a/b"),
		    make_tuple(OK_, "s3://bucket/a",         "s3://bucket/a",              "s3://bucket/a"),
		    // scheme normalized to lowercase (RFC 3986)
		    make_tuple(OK_, "S3://bucket/a",         "s3://bucket/a/b",            "s3://bucket/a/b"),
		}));
	}

#if defined(_WIN32)
	SECTION("windows files and schemes") {
		std::tie(return_exp, lhs, rhs, joined_exp) = GENERATE(table<ResultType, std::string, std::string, std::string>({
		    make_tuple(OK_, R"(C:\)", R"(..)",              R"(C:\)"),
		    make_tuple(OK_, R"(C:\)", R"(..\..)",           R"(C:\)"),
		    make_tuple(OK_, R"(C:\)", "system32",           R"(C:\system32)"),

		    make_tuple(OK_, "C:", "system32",               "C:system32"),
		    make_tuple(OK_, "C:relpath", "sub",             R"(C:relpath\sub)"),
		    make_tuple(OK_, "C:relpath", R"(..\sib)",       "C:sib"),

		    make_tuple(ERR, R"(C:\foo)", R"(D:\bar)",       ""),
		    make_tuple(ERR, R"(C:\foo)", R"(D:bar)",        ""),
		    make_tuple(ERR, R"(C:foo)", R"(D:bar)",         ""),
		    make_tuple(ERR, R"(C:\foo)", R"(D:\foo\bar)",   ""),

		    // extended UNC variants
		    make_tuple(OK_, R"(\\server\share)", R"(sub)",              R"(\\server\share\sub)"),
		    make_tuple(OK_, R"(\\?\UNC\server\share)", R"(sub\.\dir)",  R"(\\?\UNC\server\share\sub\dir)"),
		    make_tuple(OK_, R"(\\?\c:\sub)", R"(dir)",                  R"(\\?\C:\sub\dir)"),

		    // '..' dropping at front of abs path
		    make_tuple(OK_, R"(\\server\share)", R"(..\sibling)", R"(\\server\share\sibling)"),

		    // case-insensitive path prefix
		    make_tuple(OK_, R"(C:\FOO)",  R"(C:\foo\bar)",  R"(C:\foo\bar)"),
		    make_tuple(OK_, "C:/FOO",     "C:/foo/bar",     "C:/foo/bar"),

		    // case-insensitive UNC authority
		    make_tuple(OK_, R"(\\Server\Share\foo)",  R"(\\server\share\foo\bar)",  R"(\\Server\Share\foo\bar)"),
		}));
	}
#endif
	// clang-format on

	CAPTURE(lhs, rhs);
	if (return_exp == OK_) {
		CHECK(L(do_join(lhs, rhs)) == L(joined_exp));
	} else {
		CHECK_THROWS(do_join(lhs, rhs));
	}
}

TEST_CASE("Path attributes", "[file_system]") {
	using std::make_tuple;

	auto L = [](const string &path) {
		string out(path);
#ifdef _WIN32
		for (size_t pos = 0; (pos = out.find('/', pos)) != string::npos; pos++) {
			out[pos] = '\\';
		}
#endif
		return out;
	};

	std::string input;

	SECTION("IsAbsolute + IsLocal") {
		bool is_absolute_exp = false;
		bool is_local_exp = false;
		enum AbsType { REL = false, ABS = true };
		enum LocalTypeType { REMOTE = false, LOCAL_ = true };

		// clang-format off
		SECTION("posix and URI") {
			std::tie(input, is_absolute_exp, is_local_exp) = GENERATE(table<std::string, bool, bool>({
			    make_tuple("",                    REL, LOCAL_),
			    make_tuple("a/b",                 REL, LOCAL_),
			    make_tuple("/",                   ABS, LOCAL_),
			    make_tuple("/a/b",                ABS, LOCAL_),
			    make_tuple("file:/a/b",           ABS, LOCAL_),
			    make_tuple("file:///a/b",         ABS, LOCAL_),
			    make_tuple("s3://bucket/foo",     ABS, REMOTE),
			    make_tuple("az://container/foo",  ABS, REMOTE),
			    make_tuple("http://host/path",    ABS, REMOTE),
			}));
		}
#if defined(_WIN32)
		SECTION("windows") {
			std::tie(input, is_absolute_exp, is_local_exp) = GENERATE(table<std::string, bool, bool>({
			    make_tuple(R"(C:\foo)",          ABS, LOCAL_),
			    make_tuple(R"(C:foo)",           REL, LOCAL_),
			    make_tuple(R"(\\server\share)",  ABS, REMOTE),
			    make_tuple(R"(\\?\C:\foo)",      ABS, LOCAL_),
			}));
		}
#endif
		// clang-format on

		CAPTURE(input);
		auto p = Path::FromString(input);
		CHECK(p.IsAbsolute() == is_absolute_exp);
		CHECK(p.IsLocal() == is_local_exp);
	}

	SECTION("HasTrailingSeparator") {
		bool exp = false;

		// clang-format off
		SECTION("posix and URI") {
			std::tie(input, exp) = GENERATE(table<std::string, bool>({
			    make_tuple("",           false),
			    make_tuple("foo/bar",    false),
			    make_tuple("foo/bar/",   true),
			    make_tuple("/",          true),
			    make_tuple("/a/b",       false),
			    make_tuple("/a/b/",      true),
			    make_tuple("s3://b/p",   false),
			    make_tuple("s3://b/p/",  true),
			}));
		}
#if defined(_WIN32)
		SECTION("windows") {
			std::tie(input, exp) = GENERATE(table<std::string, bool>({
			    make_tuple("C:/",        true),
			    make_tuple("C:/foo",     false),
			    make_tuple("C:/foo/",    true),
			    make_tuple(R"(C:\)",     true),
			    make_tuple(R"(C:\foo)",  false),
			    make_tuple(R"(C:\foo\)", true),
			}));
		}
#endif

		CAPTURE(input);
		CHECK(Path::FromString(input).HasTrailingSeparator() == exp);
	}

	SECTION("trailing separator ToString") {
		// bare anchor: anchor already ends with sep, no double-emit
		CHECK(L(Path::FromString("/").ToString()) 				== L("/"));
		CHECK(L(Path::FromString("/foo/bar/").ToString()) 		== L("/foo/bar/"));
		CHECK(L(Path::FromString("/foo/bar").ToString()) 		== L("/foo/bar"));
		CHECK(Path::FromString("s3://bucket/a/b/").ToString() 	== "s3://bucket/a/b/");
#if defined(_WIN32)
		CHECK(Path::FromString("C:/").ToString() 				== "C:/");
		CHECK(L(Path::FromString("C:/foo/").ToString()) 		== L("C:/foo/"));
#endif
	}

	SECTION("trailing separator Join") {
		CHECK(Path::FromString("a/b").Join("c/").HasTrailingSeparator() == true);
		CHECK(Path::FromString("a/b").Join("c").HasTrailingSeparator()  == false);
		CHECK(L(Path::FromString("a/b/").Join("c/").ToString()) == L("a/b/c/"));
		CHECK(L(Path::FromString("a/b/").Join("c").ToString())  == L("a/b/c"));
	}

	SECTION("GetTrailingSeparator") {
		// has segments + trailing sep → string(1, separator)
		CHECK(Path::FromString("/foo/bar/").GetTrailingSeparator()  == "/");
		CHECK(Path::FromString("foo/bar/").GetTrailingSeparator()   == "/");
		CHECK(Path::FromString("s3://b/p/").GetTrailingSeparator()  == "/");
		// has segments, no trailing sep → ""
		CHECK(Path::FromString("/foo/bar").GetTrailingSeparator()   == "");
		CHECK(Path::FromString("foo/bar").GetTrailingSeparator()    == "");
		CHECK(Path::FromString("s3://b/p").GetTrailingSeparator()   == "");
		// no segments (bare anchors): anchor already ends with sep, no double-emit
		CHECK(Path::FromString("/").GetTrailingSeparator()          == "");
		CHECK(Path::FromString("").GetTrailingSeparator()           == "");
		CHECK(Path::FromString("s3://b/").GetTrailingSeparator()    == "");
#if defined(_WIN32)
		CHECK(L(Path::FromString(R"(C:\foo\)").GetTrailingSeparator()) == L("/"));
		CHECK(Path::FromString(R"(C:\)").GetTrailingSeparator()        == "");  // no segments
#endif
	}
	// clang-format on
}

TEST_CASE("Path one-off tests", "[file_system]") {
	// confirm that Join("..") and Parent() behaviors are identical in corner case
	auto dotdot = Path::FromString("..");
	CHECK(dotdot.Parent().ToString() == "../..");
	CHECK(dotdot.Join("..").ToString() == "../..");

	auto dotdot_a = Path::FromString("../a");
	CHECK(dotdot_a.Parent().ToString() == "..");
	CHECK(dotdot_a.Join("..").ToString() == "..");

	CHECK(Path::FromString("a").Join("b").ToString() == "a/b");
	CHECK(Path::FromString("a").Join("b", "c").ToString() == "a/b/c");
	CHECK(Path::FromString("a").Join("b", "c", "d").ToString() == "a/b/c/d");

	CHECK(Path::FromString("a").Join(std::vector<string>()).ToString() == "a");
	CHECK(Path::FromString("a").Join(std::vector<string>({"b"})).ToString() == "a/b");
	CHECK(Path::FromString("a").Join(std::vector<string>({"b", "c"})).ToString() == "a/b/c");
}

#ifdef _WIN32
TEST_CASE("Check path canonicalization on Windows", "[file_system]") {
	auto fs = FileSystem::CreateLocal();
	auto canonical_work_dir = fs->CanonicalizePath(fs->GetWorkingDirectory());
	// check that long path prefix "\\?\" or "\\?\UNC\" is not present
	REQUIRE(!StringUtil::StartsWith(canonical_work_dir, "\\\\"));
}

char GenChar(idx_t i) {
	char idx = static_cast<char>(i % 26);
	return idx + 'a';
}

string GenPath(idx_t len) {
	string res;
	idx_t i = 0;
	while (res.length() < len) {
		if (i > 0 && i % 16 == 0) {
			res.push_back('\\');
		} else {
			res.push_back(GenChar(i));
		}
		i++;
	}
	REQUIRE(res.length() == len);
	return res;
}

string GenString(idx_t len) {
	string res;
	idx_t i = 0;
	while (res.length() < len) {
		res.push_back(GenChar(i));
		i++;
	}
	REQUIRE(res.length() == len);
	return res;
}

TEST_CASE("Test long paths on Windows", "[file_system]") {
	idx_t max_dir_path_len = 247;
	idx_t work_dir_path_len = 240;

	duckdb::unique_ptr<FileSystem> fs = FileSystem::CreateLocal();
	string cwd = fs->GetWorkingDirectory();
	string test_dir = TestDirectoryPath();
	test_dir = fs->JoinPath(test_dir, "test_long_path");
	test_dir = fs->JoinPath(cwd, test_dir);
	REQUIRE(test_dir.length() < max_dir_path_len - 1);
	fs->CreateDirectory(test_dir);

	string work_dir = fs->JoinPath(test_dir, GenPath(work_dir_path_len - test_dir.length() - 1));
	REQUIRE(work_dir.length() == work_dir_path_len);
	fs->CreateDirectoriesRecursive(work_dir);
	REQUIRE(fs->DirectoryExists(work_dir));

	std::vector<char> test_buf = {'f', 'o', 'o'};

	// create/remove files
	for (idx_t i = 1; i < 32; i++) {
		string name = "f_" + GenString(i);
		string path = fs->JoinPath(work_dir, name);
		auto fd = fs->OpenFile(path, FileFlags::FILE_FLAGS_FILE_CREATE_NEW | FileFlags::FILE_FLAGS_WRITE);
		fd->Write(test_buf.data(), test_buf.size());
		fd->Close();
		REQUIRE(fs->FileExists(path));
		fs->RemoveFile(path);
		REQUIRE(!fs->FileExists(path));
	}

	// create/remove directories
	for (idx_t i = 1; i < 32; i++) {
		string name = "d_" + GenString(i);
		string path = fs->JoinPath(work_dir, name);
		fs->CreateDirectory(path);
		REQUIRE(fs->DirectoryExists(path));
		fs->RemoveDirectory(path);
		REQUIRE(!fs->DirectoryExists(path));
	}

	// move file
	string src_path = fs->JoinPath(work_dir, "move_src_" + GenString(32));
	{
		auto fd = fs->OpenFile(src_path, FileFlags::FILE_FLAGS_FILE_CREATE_NEW | FileFlags::FILE_FLAGS_WRITE);
		fd->Write(test_buf.data(), test_buf.size());
		fd->Close();
	}
	REQUIRE(fs->FileExists(src_path));
	string dest_path = fs->JoinPath(work_dir, "move_dest_" + GenString(32));
	fs->MoveFile(src_path, dest_path);
	REQUIRE(!fs->FileExists(src_path));
	REQUIRE(fs->FileExists(dest_path));
	{
		auto fd = fs->OpenFile(dest_path, FileFlags::FILE_FLAGS_READ);
		std::vector<char> buf;
		buf.resize(3);
		auto read = fd->Read(buf.data(), buf.size());
		fd->Close();
		REQUIRE(read == 3);
		REQUIRE(std::string(buf.data(), buf.size()) == "foo");
	}

	// non-clean paths
	{
		string long_dir = fs->JoinPath(work_dir, "long_dir_" + GenString(32));
		fs->CreateDirectory(long_dir);
		REQUIRE(fs->DirectoryExists(long_dir));
		string dir1 = long_dir + "/dir1";
		string dir2 = long_dir + "/dir1/.//..\\dir2";
		string dir2_clean = fs->JoinPath(long_dir, "dir2");
		string file1 = dir2 + "/file1";
		fs->CreateDirectory(dir1);
		REQUIRE(fs->DirectoryExists(dir1));
		fs->CreateDirectory(dir2);
		REQUIRE(fs->DirectoryExists(dir2));
		REQUIRE(fs->DirectoryExists(dir2_clean));
		auto fd = fs->OpenFile(file1, FileFlags::FILE_FLAGS_FILE_CREATE_NEW | FileFlags::FILE_FLAGS_WRITE);
		fd->Write(test_buf.data(), test_buf.size());
		fd->Close();
		REQUIRE(fs->FileExists(file1));
	}

	// pre-existing prefix
	{
		string name = "prefix_dir_" + GenString(32);
		string path = fs->JoinPath(work_dir, name);
		string prefixed = "\\\\?\\" + path;
		fs->CreateDirectory(prefixed);
		REQUIRE(fs->DirectoryExists(prefixed));
		REQUIRE(fs->DirectoryExists(path));
	}

	// cleaning up, as not all tools can remove long dirs
	fs->RemoveDirectory(work_dir);
}
#endif // _WIN32

TEST_CASE("compression filesystem registration and lookup", "[file_system]") {
	// Create a local file which pretends to be fake-compressed.
	auto filepath = TestCreatePath("fake_compression_file.compress");
	auto fs = FileSystem::CreateLocal();
	{
		const string payload = "CREATE_COMPRESSED_FILE";
		auto write_handle = fs->OpenFile(filepath, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE);
		write_handle->Write(QueryContext(), static_cast<void *>(const_cast<char *>(payload.c_str())), payload.size(),
		                    0);
		write_handle->Sync();
	}

	VirtualFileSystem vfs;
	vfs.RegisterCompressionFilesystem(make_uniq<FakeCompressFileSystem>());

	// Auto-detection matches the fake compression filesystem based on the file name.
	FileOpenFlags flags = FileOpenFlags::FILE_FLAGS_READ;
	flags.SetCompression(FileCompressionType::AUTO_DETECT);
	auto file_handle = vfs.OpenFile(filepath, flags, /*opener=*/nullptr);

	// Downcast to make sure compressed file handle is created.
	REQUIRE(file_handle != nullptr);
	auto &compressed_file_handle = file_handle->Cast<FakeCompressFileHandle>();
	REQUIRE(compressed_file_handle.internal_file_handle != nullptr);
	file_handle.reset();

	// Explicitly requesting the registered compression type works as well.
	flags.SetCompression(FileCompressionType("fake_compress"));
	file_handle = vfs.OpenFile(filepath, flags, /*opener=*/nullptr);
	REQUIRE(file_handle != nullptr);
	REQUIRE(file_handle->Cast<FakeCompressFileHandle>().internal_file_handle != nullptr);
	file_handle.reset();

	// Explicitly requesting a compression type that has not been registered throws.
	flags.SetCompression(FileCompressionType("nonexistent_compress"));
	REQUIRE_THROWS(vfs.OpenFile(filepath, flags, /*opener=*/nullptr));

	fs->RemoveFile(filepath);
}

TEST_CASE("JoinPath keeps a remote URL's query string at the end of the path", "[file_system]") {
	LocalFileSystem fs;
	// A query string rides at the end, not buried before the appended segment.
	REQUIRE(fs.JoinPath("s3://bucket/dir?s3_sse_c_key=abc", "file.parquet") ==
	        "s3://bucket/dir/file.parquet?s3_sse_c_key=abc");
	// Nested appends (a hive directory, then the file) keep it at the end -- the partitioned COPY case.
	auto hive_dir = fs.JoinPath("s3://bucket/dir?s3_sse_c_key=abc", "part=7");
	REQUIRE(hive_dir == "s3://bucket/dir/part=7?s3_sse_c_key=abc");
	REQUIRE(fs.JoinPath(hive_dir, "data_0.parquet") == "s3://bucket/dir/part=7/data_0.parquet?s3_sse_c_key=abc");
	// A URL without a query is unchanged.
	REQUIRE(fs.JoinPath("s3://bucket/dir", "file.parquet") == "s3://bucket/dir/file.parquet");
	// A '?' in a plain (non-URL) path is a literal character, not a query delimiter.
	REQUIRE(fs.JoinPath("/tmp/weird?name", "file.parquet") == "/tmp/weird?name/file.parquet");
}
