#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/common/set.hpp"
#include "duckdb/common/virtual_file_system.hpp"
#include "duckdb/storage/temporary_file_manager.hpp"

using namespace duckdb;

TEST_CASE("Test temporary file owners", "[api]") {
	SECTION("a name round-trips through its owner") {
		set<string> prefixes;
		for (int64_t pid = 1; pid < 64; pid++) {
			for (idx_t instance = 0; instance < 64; instance++) {
				TemporaryFileOwner written {pid, instance};
				auto prefix = TemporaryFilePrefix(written);
				prefixes.insert(prefix);

				TemporaryFileOwner parsed;
				REQUIRE(TryParseTemporaryFileOwner(prefix + "storage_DEFAULT-0.tmp", parsed));
				REQUIRE(parsed == written);
				REQUIRE(TryParseTemporaryFileOwner(prefix + "block-7.block", parsed));
				REQUIRE(parsed == written);
				// the marker is swept like any other file of a dead owner, so it must parse too
				REQUIRE(TryParseTemporaryFileOwner(TemporaryOwnerMarkerName(written), parsed));
				REQUIRE(parsed == written);
			}
		}
		// no two owners share a prefix, so no instance can reach another's files
		REQUIRE(prefixes.size() == 63 * 64);
	}

	SECTION("names that do not carry an owner are not claimed") {
		// a version that named its files differently may still be running and using them
		TemporaryFileOwner parsed;
		REQUIRE(!TryParseTemporaryFileOwner("duckdb_temp_5ee2fed7-f0e1-4e7f-9399-d813d74cee53_storage_DEFAULT-0.tmp",
		                                    parsed));
		REQUIRE(!TryParseTemporaryFileOwner("duckdb_temp_storage_DEFAULT-0.tmp", parsed));
		REQUIRE(!TryParseTemporaryFileOwner("duckdb_temp_block-1.block", parsed));
		REQUIRE(!TryParseTemporaryFileOwner("duckdb_temp_-1_0_storage_DEFAULT-0.tmp", parsed));
		REQUIRE(!TryParseTemporaryFileOwner("duckdb_temp_1_ _storage_DEFAULT-0.tmp", parsed));
		REQUIRE(!TryParseTemporaryFileOwner("something_else.tmp", parsed));
	}
}

#ifndef _WIN32
namespace {

//! Makes closing the first owner markers fail
class FailingMarkerCloseFileSystem : public VirtualFileSystem {
public:
	atomic<idx_t> marker_opens {0};

protected:
	unique_ptr<FileHandle> OpenFileExtended(const OpenFileInfo &file, FileOpenFlags flags,
	                                        optional_ptr<FileOpener> opener) override {
		if (!StringUtil::EndsWith(file.path, "owner") || marker_opens++ >= 3) {
			return VirtualFileSystem::OpenFileExtended(file, flags, opener);
		}
		unique_ptr<FileHandle> handle;
		auto open_file = [&]() {
			handle = VirtualFileSystem::OpenFileExtended(file, flags, opener);
		};
		if (!TestOpenWithFailingClose(file.path, open_file)) {
			throw InternalException("Could not make closing \"%s\" fail", file.path);
		}
		return handle;
	}
};

} // namespace

TEST_CASE("A failed close of a temporary directory marker is reported", "[api]") {
	auto fs = make_uniq<FailingMarkerCloseFileSystem>();
	auto &marker_fs = *fs;
	DBConfig config;
	config.file_system = std::move(fs);
	DuckDB db(nullptr, &config);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	REQUIRE_NO_FAIL(con.Query("SET temp_directory='" + TestCreatePath("failed_marker_close.tmp") + "'"));
	REQUIRE_NO_FAIL(con.Query("CREATE TEMP TABLE t AS SELECT i, repeat('y', 80000) AS s FROM range(400) r(i)"));
	auto result = con.Query("SET memory_limit='10MB'");
	REQUIRE(result->HasError());
	REQUIRE(StringUtil::Contains(result->GetError(), "Could not close file"));
	REQUIRE(marker_fs.marker_opens == 1);
}
#endif
