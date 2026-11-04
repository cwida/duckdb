#include "catch.hpp"
#include "duckdb.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/local_file_system.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/virtual_file_system.hpp"
#include "test_config.hpp"
#include "test_helpers.hpp"

#include <iostream>
#include <shared_mutex>

using namespace duckdb;

bool g_enable_verbose_output = false;
bool g_enable_info_output = true;

#define PRINT_VERBOSE(x)                                                                                               \
	do {                                                                                                               \
		if (g_enable_verbose_output)                                                                                   \
			std::cout << x << std::endl;                                                                               \
	} while (0)

#define PRINT_INFO(x)                                                                                                  \
	do {                                                                                                               \
		if (g_enable_info_output)                                                                                      \
			std::cout << x << std::endl;                                                                               \
	} while (0)

bool ends_with(const std::string &str, const std::string &suffix) {
	// Ensure str is at least as long as suffix
	if (str.length() < suffix.length()) {
		return false;
	}

	// Compare the ending characters
	return str.compare(str.length() - suffix.length(), suffix.length(), suffix) == 0;
}

class FaultInjectionFileSystem : public duckdb::LocalFileSystem {
public:
	enum FaultInjectionSite {
		WRITE = 0,
		FSYNC = 1,
	};

	void Write(duckdb::FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override {
		PRINT_VERBOSE("FS write offset=" << location << " bytes=" << nr_bytes);
		if (is_db_file(handle)) {
			ThrowInjectedFaultIfSet(FaultInjectionSite::WRITE);
		}
		return duckdb::LocalFileSystem::Write(handle, buffer, nr_bytes, location);
	}

	void FileSync(duckdb::FileHandle &handle) override {
		PRINT_VERBOSE("FS fsync " << handle.GetPath() << " file_size=" << handle.GetFileSize());
		if (is_db_file(handle)) {
			ThrowInjectedFaultIfSet(FaultInjectionSite::FSYNC);
		}
		return duckdb::LocalFileSystem::FileSync(handle);
	}

	void RemoveFile(const duckdb::string &filename,
	                duckdb::optional_ptr<duckdb::FileOpener> opener = nullptr) override {
		PRINT_VERBOSE("FS remove " << filename);
		return duckdb::LocalFileSystem::RemoveFile(filename, opener);
	}

	void Truncate(duckdb::FileHandle &handle, int64_t new_size) override {
		PRINT_VERBOSE("FS truncate " << handle.GetPath() << " from " << handle.GetFileSize() << " to " << new_size);
		return duckdb::LocalFileSystem::Truncate(handle, new_size);
	}

	// In linux - trim() is equivalent to zeroing out a range (albeit in a much more efficient manner). Let's
	// reproduce this behavior regardless of whether the current environment supports it.
	bool Trim(duckdb::FileHandle &handle, idx_t offset_bytes, idx_t length_bytes) override {
		PRINT_VERBOSE("FS trim " << handle.GetPath() << " offset=" << offset_bytes << " bytes=" << length_bytes);

		std::string nulls(length_bytes, '\0');
		duckdb::LocalFileSystem::Write(handle, (void *)nulls.data(), length_bytes, offset_bytes);
		return true;
	}

	// Will inject a single occurrence of a fault
	void InjectFault(FaultInjectionSite site) {
		std::lock_guard<std::mutex> l(fault_m_);
		// Make sure this is not called twice - as we will drop a fault
		REQUIRE(faults.insert(site).second);
	}

protected:
	void ThrowInjectedFaultIfSet(FaultInjectionSite site) {
		std::lock_guard<std::mutex> l(fault_m_);
		auto it = faults.find(site);
		if (it != faults.end()) {
			faults.erase(it);
			PRINT_VERBOSE("Injecting fault");
			throw duckdb::IOException("Injected fault");
		}
	}

	bool is_wal_file(const duckdb::FileHandle &handle) {
		return ends_with(handle.GetPath(), ".db.wal");
	}

	bool is_db_file(const duckdb::FileHandle &handle) {
		return ends_with(handle.GetPath(), ".db");
	}

	bool is_wal_or_db_file(const duckdb::FileHandle &handle) {
		return is_db_file(handle) || is_wal_file(handle);
	}

private:
	std::mutex fault_m_;
	std::unordered_set<FaultInjectionSite> faults;
};

// This implementation of duckdb::FileSystem will cache writes to the database file in memory until fsync is called.
// It expects all read ranges to be perfectly aligned with previous writes.
class LazyFlushFileSystem : public FaultInjectionFileSystem {
public:
	~LazyFlushFileSystem() {
		if (!unflushed_chunks.empty()) {
			PRINT_INFO("Unflushed chunks on shutdown for db file");
		}
	}

	void Write(duckdb::FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override {
		PRINT_VERBOSE("FS write offset=" << location << " bytes=" << nr_bytes);

		// We only perform positional writes for the db file
		REQUIRE(is_db_file(handle));

		std::unique_lock<std::mutex> l(m_);

		ThrowInjectedFaultIfSet(FaultInjectionSite::WRITE);

		// Store the data in memory until fsync occurs
		PRINT_VERBOSE("Caching chunk " << location << " bytes " << nr_bytes);

		// TODO: be lazy - don't handle partial overwrites
		REQUIRE(!partially_overlaps_existing_chunk(unflushed_chunks, location, nr_bytes));

		auto it = unflushed_chunks.find(location);
		if (it != unflushed_chunks.end()) {
			// Check that if there is an existing chunk - it's size matches exactly
			REQUIRE(it->second.size() == NumericCast<size_t>(nr_bytes));
			it->second = std::string((char *)buffer, nr_bytes);
		} else {
			unflushed_chunks.emplace(location, std::string((char *)buffer, nr_bytes));
		}
	}

	int64_t Write(duckdb::FileHandle &handle, void *buffer, int64_t nr_bytes) override {
		// Check appends only occur on the WAL
		REQUIRE(is_wal_file(handle));

		return duckdb::LocalFileSystem::Write(handle, buffer, nr_bytes);
	}

	void Read(duckdb::FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override {
		REQUIRE(is_db_file(handle));

		{
			// TODO: shared_lock
			std::unique_lock<std::mutex> l(m_);

			// We don't handle partial overlaps for now.
			REQUIRE(!partially_overlaps_existing_chunk(unflushed_chunks, location, nr_bytes));

			auto it = unflushed_chunks.find(location);
			if (it != unflushed_chunks.end()) {
				PRINT_VERBOSE("FS read cached chunk at offset=" << location << " bytes=" << nr_bytes);
				const auto &data = it->second;
				// Assume block-aligned reads
				REQUIRE(data.size() == NumericCast<size_t>(nr_bytes));
				memcpy(buffer, data.data(), nr_bytes);
				return;
			}
		}

		PRINT_VERBOSE("FS read disk chunk at offset=" << location << " bytes=" << nr_bytes);
		return duckdb::LocalFileSystem::Read(handle, buffer, nr_bytes, location);
	}

	int64_t Read(duckdb::FileHandle &handle, void *buffer, int64_t nr_bytes) override {
		PRINT_VERBOSE("FS read at end of file, bytes=" << nr_bytes);
		REQUIRE(is_wal_or_db_file(handle));

		if (is_db_file(handle)) {
			// Just make sure we don't miss the unflushed chunks
			REQUIRE(unflushed_chunks.empty());
		}
		return duckdb::LocalFileSystem::Read(handle, buffer, nr_bytes);
	}

	void FileSync(duckdb::FileHandle &handle) override {
		PRINT_VERBOSE("FS fsync " << handle.GetPath() << " file_size=" << handle.GetFileSize());

		REQUIRE(is_wal_or_db_file(handle));

		if (!is_db_file(handle)) {
			return duckdb::LocalFileSystem::FileSync(handle);
		}

		std::unique_lock<std::mutex> l(m_);

		ThrowInjectedFaultIfSet(FaultInjectionSite::FSYNC);

		for (const auto &location_and_data : unflushed_chunks) {
			auto location = location_and_data.first;
			const auto &data = location_and_data.second;
			PRINT_VERBOSE("Flushing chunk " << location << " size=" << data.size());
			duckdb::LocalFileSystem::Write(handle, (void *)data.data(), data.size(), location);
		}
		unflushed_chunks.clear();

		duckdb::LocalFileSystem::FileSync(handle);
	}

	bool Trim(duckdb::FileHandle &handle, idx_t offset_bytes, idx_t length_bytes) override {
		REQUIRE(is_db_file(handle));

		std::unique_lock<std::mutex> l(m_);

		// This is just simpler to implement
		REQUIRE(unflushed_chunks.count(offset_bytes) == 0);

		return FaultInjectionFileSystem::Trim(handle, offset_bytes, length_bytes);
	}

	void Truncate(duckdb::FileHandle &handle, int64_t new_size) override {
		std::unique_lock<std::mutex> l(m_);

		if (is_db_file(handle)) {
			REQUIRE(unflushed_chunks.empty());
		}

		return duckdb::LocalFileSystem::Truncate(handle, new_size);
	}

private:
	// Lock for modifying unflushed_chunks:
	// 1. Adding to unflushed_chunks on write
	// 2. Flushing unflushed_chunks on fsync
	// 3. Reading from unflushed_chunks
	std::mutex m_;
	std::map<idx_t, std::string> unflushed_chunks;

	bool partially_overlaps_existing_chunk(const std::map<idx_t, std::string> &chunks, idx_t offset, size_t length) {
		idx_t end = offset + length;
		for (const auto &off_data : chunks) {
			auto off = off_data.first;
			const auto &data = off_data.second;
			idx_t chunk_end = off + data.size();

			// Check for any overlap
			bool overlap = offset < chunk_end && off < end;

			// Exclude full containment and exact match
			bool exact_match = (offset == off && length == data.size());

			if (overlap && !exact_match)
				return true;
		}
		return false;
	}
};

template <class ResultT>
void validate(ResultT &r, std::string expected_err_message = "") {
	// For debugging
	bool expected_err = !expected_err_message.empty();
	if (expected_err != r.HasError() && r.HasError()) {
		PRINT_INFO("Unexpected: query failed with " << r.GetError());
	}
	REQUIRE(expected_err == r.HasError());
	if (r.HasError()) {
		REQUIRE(r.GetError().find(expected_err_message) != std::string::npos);
	}
}

void cleanup_db_file(const std::string &filename) {
	bool removed_or_missing = std::remove(filename.c_str()) == 0 || errno == ENOENT;
	REQUIRE(removed_or_missing);
}

TEST_CASE("simple fault injection storage test", "[storage][.]") {
	if (!TestConfiguration::TestRunStorageFuzzer()) {
		SKIP_TEST("storage-fuzzer not enabled");
		return;
	}
	duckdb::DBConfig config;

	LazyFlushFileSystem *raw_fs = new LazyFlushFileSystem();
	config.file_system = duckdb::make_uniq<duckdb::VirtualFileSystem>(duckdb::unique_ptr<LazyFlushFileSystem>(raw_fs));

	std::string file_path = TestCreatePath("pig.db");

	cleanup_db_file(file_path);

	{
		duckdb::DuckDB db(file_path, &config);

		duckdb::Connection con(db);

		validate(*con.Query("CREATE TABLE IF NOT EXISTS t(i INTEGER)"));
		validate(*con.Query("INSERT INTO t SELECT * FROM RANGE(0, 1000)"));
		validate(*con.Query("INSERT INTO t SELECT * FROM RANGE(0, 1000000)"));

		auto res = con.Query("SELECT count(*) FROM t");
		validate(*res);
		REQUIRE(res->Collection().GetValue(0, 0).ToString() == "1001000");

		// Writes are ok - fsync are not ok
		raw_fs->InjectFault(LazyFlushFileSystem::FaultInjectionSite::FSYNC);

		validate(*con.Query("INSERT INTO t SELECT * FROM RANGE(0, 1000000)"),
		         "TransactionContext Error: Failed to commit: Injected fault");

		// Check that the tx was rolled back
		auto res2 = con.Query("SELECT count(*) FROM t");
		validate(*res2);
		REQUIRE(res2->Collection().GetValue(0, 0).ToString() == "1001000");
	}
	{
		duckdb::DuckDB db(file_path, &config);
		duckdb::Connection con(db);

		auto res = con.Query("SELECT count(*) FROM t");
		validate(*res);
		REQUIRE(res->Collection().GetValue(0, 0).ToString() == "1001000");
	}
}

enum ActionType {
	// This action will simply flip the setting true -> false or false -> true
	TOGGLE_SKIP_CHECKPOINTS_ON_COMMIT = 0,
	SMALL_WRITE = 2,
	LARGE_WRITE = 3,
	LARGE_WRITE_WITH_FAULT = 4,
	UPDATE = 5,
	DELETE_ACTION = 6,
	RESET_TABLE = 7,
};

TEST_CASE("fuzzed storage test", "[storage][.]") {
	if (!TestConfiguration::TestRunStorageFuzzer()) {
		SKIP_TEST("storage-fuzzer not enabled");
		return;
	}
	// DuckDB Configurations
	duckdb::DBConfig config;
	config.SetOptionByName("debug_skip_checkpoint_on_commit", true);
	config.options.trim_free_blocks = true;
	config.options.checkpoint_on_shutdown = false;

	std::string file_path = TestCreatePath("pig.db");

	cleanup_db_file(file_path);

	{
		duckdb::DuckDB db(file_path, &config);
		duckdb::Connection con(db);
		validate(*con.Query("CREATE TABLE IF NOT EXISTS t(i INTEGER)"));
	}

	std::map<double, ActionType> pct_to_action = {{0.1, ActionType::TOGGLE_SKIP_CHECKPOINTS_ON_COMMIT},
	                                              {0.3, ActionType::LARGE_WRITE},
	                                              {0.5, ActionType::SMALL_WRITE},
	                                              {0.7, ActionType::UPDATE},
	                                              {0.85, ActionType::DELETE_ACTION},
	                                              {1.0, ActionType::LARGE_WRITE_WITH_FAULT}};

	// Randomly generated sequence of actions
	std::vector<ActionType> actions = {};

	int NUM_ACTIONS = 30;
	for (int i = 0; i < NUM_ACTIONS; i++) {
		double selection = (rand() % 100) / 100.0;
		for (const auto &prob_type : pct_to_action) {
			auto prob = prob_type.first;
			auto type = prob_type.second;
			if (selection > prob) {
				continue;
			}
			actions.push_back(type);
			break;
		}
	}
	actions.push_back(RESET_TABLE);
	for (int i = 0; i < NUM_ACTIONS; i++) {
		double selection = (rand() % 100) / 100.0;
		for (const auto &prob_type : pct_to_action) {
			auto prob = prob_type.first;
			auto type = prob_type.second;
			if (selection > prob) {
				continue;
			}
			actions.push_back(type);
			break;
		}
	}

	uint64_t offset = 0;
	bool skip_checkpoint_on_commit = true;
	std::string expected_checksum = "";
	duckdb::unique_ptr<QueryResult> previous_result;
	for (const auto &action : actions) {
		// Note: the injected file system has to be reset each time. DuckDB construction seems to be std::move'ing them

		/*
		LazyFlushFileSystem *raw_fs = new LazyFlushFileSystem();
		config.file_system =
		    duckdb::make_uniq<duckdb::VirtualFileSystem>(duckdb::unique_ptr<LazyFlushFileSystem>(raw_fs));
		 */

		FaultInjectionFileSystem *raw_fs = new FaultInjectionFileSystem();
		config.file_system =
		    duckdb::make_uniq<duckdb::VirtualFileSystem>(duckdb::unique_ptr<FaultInjectionFileSystem>(raw_fs));

		duckdb::DuckDB db(file_path, &config);
		duckdb::Connection con(db);

		// Compute a checksum
		if (!expected_checksum.empty()) {
			auto checksum = con.Query("SELECT bit_xor(hash(i)) FROM t");
			validate(*checksum);
			auto computed_checksum = checksum->Collection().GetValue(0, 0).ToString();
			PRINT_INFO("Verifying checksum computed=" << computed_checksum << ", actual=" << expected_checksum);
			if (computed_checksum != expected_checksum) {
				auto result = con.Query("SELECT * FROM t ORDER BY ALL");
				string error;
				ColumnDataCollection::ResultEquals(previous_result->Collection(), result->Collection(), error);
				Printer::PrintF("Checksum failure\nResult comparison:\n%s", error);
				REQUIRE(computed_checksum == expected_checksum);
			}
		}
		previous_result = con.Query("SELECT * FROM t ORDER BY ALL");

		switch (action) {
		case ActionType::TOGGLE_SKIP_CHECKPOINTS_ON_COMMIT:
			skip_checkpoint_on_commit = !skip_checkpoint_on_commit;
			PRINT_INFO("Setting skip commit=" << skip_checkpoint_on_commit);
			config.SetOptionByName("debug_skip_checkpoint_on_commit", skip_checkpoint_on_commit);
			break;
		case ActionType::SMALL_WRITE: {
			std::string small_insert = "INSERT INTO t SELECT * FROM RANGE(" + std::to_string(offset) + ", " +
			                           std::to_string(offset + 100) + ")";
			PRINT_INFO("RUN: " << small_insert);
			validate(*con.Query(small_insert));
			offset += 100;
			break;
		}
		case ActionType::LARGE_WRITE: {
			std::string large_insert = "INSERT INTO t SELECT * FROM RANGE(" + std::to_string(offset) + ", " +
			                           std::to_string(offset + 1000 * 1000) + ")";
			PRINT_INFO("RUN: " << large_insert);
			validate(*con.Query(large_insert));
			offset += 1000 * 1000;
			break;
		}
		case ActionType::UPDATE: {
			if (offset != 0) {
				uint64_t begin = rand() % offset;
				uint64_t length = rand() % (offset - begin);
				std::string update_query = "UPDATE t SET i = i * 2 WHERE i > " + std::to_string(begin) + " and i <" +
				                           std::to_string(begin + length);

				PRINT_INFO("RUN: " << update_query);
				validate(*con.Query(update_query));
			}
			break;
		}
		case ActionType::DELETE_ACTION: {
			if (offset != 0) {
				uint64_t begin = rand() % offset;
				uint64_t length = rand() % (offset - begin);
				std::string delete_query =
				    "DELETE FROM t WHERE i > " + std::to_string(begin) + " and i <" + std::to_string(begin + length);

				PRINT_INFO("RUN: " << delete_query);
				validate(*con.Query(delete_query));
				break;
			}
		}
		case ActionType::LARGE_WRITE_WITH_FAULT: {
			raw_fs->InjectFault(LazyFlushFileSystem::FaultInjectionSite::FSYNC);
			std::string large_insert = "INSERT INTO t SELECT * FROM RANGE(" + std::to_string(offset) + ", " +
			                           std::to_string(offset + 1000 * 1000) + ")";

			PRINT_INFO("RUN with fault: " << large_insert);
			validate(*con.Query(large_insert), "Injected fault");
			break;
		}
		case ActionType::RESET_TABLE: {
			std::string replace_query = "CREATE OR REPLACE TABLE t(i INTEGER)";
			PRINT_INFO("RUN with fault: " << replace_query);
			validate(*con.Query(replace_query));
			break;
		}
		}

		// Compute a checksum (unless we injected a fault - which will invalidate the database)
		if (action != ActionType::LARGE_WRITE_WITH_FAULT) {
			auto checksum = con.Query("SELECT bit_xor(hash(i)) FROM t");
			validate(*checksum);
			expected_checksum = checksum->Collection().GetValue(0, 0).ToString();

			PRINT_INFO("Computed new checksum: " << expected_checksum);
		} else {
			PRINT_INFO("Keeping old checksum due to faults: " << expected_checksum);
		}
	}
}
