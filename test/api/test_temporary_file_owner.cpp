#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/common/set.hpp"
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
