#include "catch.hpp"
#include "duckdb/common/checksum.hpp"
#include "duckdb/common/file_system.hpp"
#include "test_helpers.hpp"
#include "duckdb/common/local_file_system.hpp"

using namespace duckdb;

static idx_t GetWALFileSize(FileSystem &fs, const string &path) {
	auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ);
	return fs.GetFileSize(*handle);
}

static void TruncateWAL(FileSystem &fs, const string &path, idx_t new_size) {
	auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_WRITE);
	fs.Truncate(*handle, new_size);
}

static void CopyFile(FileSystem &fs, const string &source, const string &target) {
	auto source_handle = fs.OpenFile(source, FileFlags::FILE_FLAGS_READ);
	auto file_size = source_handle->GetFileSize();
	auto contents = duckdb::unique_ptr<data_t[]>(new data_t[file_size]);
	source_handle->Read(QueryContext(), contents.get(), file_size, 0);

	auto target_handle = fs.OpenFile(target, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW);
	target_handle->Write(QueryContext(), contents.get(), file_size, 0);
}

static pair<idx_t, idx_t> GetWALCommitBoundaries(FileSystem &fs, const string &path) {
	auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ);
	auto wal_size = handle->GetFileSize();
	auto wal_contents = duckdb::unique_ptr<data_t[]>(new data_t[wal_size]);
	handle->Read(QueryContext(), wal_contents.get(), wal_size, 0);

	for (idx_t start = 0; start + 2 * sizeof(uint64_t) <= wal_size; start++) {
		idx_t offset = start;
		vector<idx_t> entry_ends;
		while (offset + 2 * sizeof(uint64_t) <= wal_size) {
			uint64_t entry_size;
			uint64_t stored_checksum;
			memcpy(&entry_size, wal_contents.get() + offset, sizeof(uint64_t));
			memcpy(&stored_checksum, wal_contents.get() + offset + sizeof(uint64_t), sizeof(uint64_t));
			offset += 2 * sizeof(uint64_t);
			if (entry_size > wal_size - offset) {
				break;
			}
			// Checksum requires aligned input, but this scan tries every possible byte offset.
			auto entry = duckdb::unique_ptr<data_t[]>(new data_t[entry_size]);
			memcpy(entry.get(), wal_contents.get() + offset, entry_size);
			if (Checksum(entry.get(), entry_size) != stored_checksum) {
				break;
			}
			offset += entry_size;
			entry_ends.push_back(offset);
		}
		if (entry_ends.size() >= 4) {
			// CREATE_TABLE A, WAL_FLUSH, CREATE_TABLE B, WAL_FLUSH
			return {entry_ends[1], entry_ends[3]};
		}
	}
	throw InternalException("Could not find expected WAL entries in torn WAL write test");
}

TEST_CASE("Test torn WAL writes", "[storage][.]") {
	auto config = GetTestConfig();
	duckdb::unique_ptr<QueryResult> result;
	auto storage_database = TestCreatePath("storage_test");
	auto storage_wal = storage_database + ".wal";
	auto source_database = TestCreatePath("storage_test_source");
	auto source_wal = source_database + ".wal";

	LocalFileSystem lfs;
	config->options.checkpoint_wal_size = idx_t(-1);
	config->options.checkpoint_on_shutdown = false;
	config->options.abort_on_wal_failure = false;
	idx_t wal_size_one_table;
	idx_t wal_size_two_table;
	// obtain the size of the WAL when writing one table, and then when writing two tables
	DeleteDatabase(storage_database);
	DeleteDatabase(source_database);
	{
		DuckDB db(source_database, config.get());
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE A (a INTEGER);"));
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE B (a INTEGER);"));
	}
	auto commit_boundaries = GetWALCommitBoundaries(lfs, source_wal);
	wal_size_one_table = commit_boundaries.first;
	wal_size_two_table = commit_boundaries.second;

	// now for all sizes in between these two sizes we have a torn write
	// try all of the possible sizes and truncate the WAL
	for (idx_t i = wal_size_one_table + 1; i + 1 < wal_size_two_table; i++) {
		DeleteDatabase(storage_database);
		CopyFile(lfs, source_database, storage_database);
		CopyFile(lfs, source_wal, storage_wal);
		TruncateWAL(lfs, storage_wal, i);
		{
			// reload and make sure table A is there, and table B is not there
			DuckDB db(storage_database, config.get());
			Connection con(db);
			REQUIRE_NO_FAIL(con.Query("FROM A"));
			REQUIRE_FAIL(con.Query("FROM B"));
		}
	}
	DeleteDatabase(storage_database);
	DeleteDatabase(source_database);
}

TEST_CASE("Test torn WAL writes followed by successful commits", "[storage][.]") {
	auto config = GetTestConfig();
	duckdb::unique_ptr<QueryResult> result;
	auto storage_database = TestCreatePath("storage_test");
	auto storage_wal = storage_database + ".wal";

	LocalFileSystem lfs;
	config->options.checkpoint_wal_size = idx_t(-1);
	config->options.checkpoint_on_shutdown = false;
	config->options.abort_on_wal_failure = false;
	idx_t wal_size_one_table;
	// obtain the size of the WAL when writing one table, and then when writing two tables
	DeleteDatabase(storage_database);
	{
		DuckDB db(storage_database, config.get());
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE A (a INTEGER);"));
		wal_size_one_table = GetWALFileSize(lfs, storage_wal);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE B (a INTEGER);"));
	}
	DeleteDatabase(storage_database);

	DeleteDatabase(storage_database);
	{
		DuckDB db(storage_database, config.get());
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE A (a INTEGER);"));
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE B (a INTEGER);"));
	}
	TruncateWAL(lfs, storage_wal, wal_size_one_table + 17);
	{
		// reload and make sure table A is there, and table B is not there
		DuckDB db(storage_database, config.get());
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("FROM A"));
		REQUIRE_FAIL(con.Query("FROM B"));

		// create table C
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE C (a INTEGER);"));
	}
	{
		// reload and make sure table A and C are there, and table B is not
		DuckDB db(storage_database, config.get());
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("FROM A"));
		REQUIRE_FAIL(con.Query("FROM B"));
		REQUIRE_NO_FAIL(con.Query("FROM C"));
	}
	DeleteDatabase(storage_database);
}

static void FlipWALByte(FileSystem &fs, const string &path, idx_t byte_pos) {
	auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_READ);
	auto wal_size = handle->GetFileSize();
	auto wal_contents = duckdb::unique_ptr<data_t[]>(new data_t[wal_size]);

	handle->Read(QueryContext(), wal_contents.get(), wal_size, 0);
	wal_contents[byte_pos]++;

	handle->Write(QueryContext(), wal_contents.get(), wal_size, 0);
}

TEST_CASE("Test WAL checksums", "[storage][.]") {
	auto config = GetTestConfig();
	duckdb::unique_ptr<QueryResult> result;
	auto storage_database = TestCreatePath("wal_checksum");
	auto storage_wal = storage_database + ".wal";
	auto source_database = TestCreatePath("wal_checksum_source");
	auto source_wal = source_database + ".wal";

	LocalFileSystem lfs;
	config->options.checkpoint_wal_size = idx_t(-1);
	config->options.checkpoint_on_shutdown = false;
	config->options.abort_on_wal_failure = false;
	idx_t wal_size_one_table;
	idx_t wal_size_two_table;
	// obtain the size of the WAL when writing one table, and then when writing two tables
	DeleteDatabase(storage_database);
	DeleteDatabase(source_database);
	{
		DuckDB db(source_database, config.get());
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE A (a INTEGER);"));
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE B (a INTEGER);"));
	}
	auto commit_boundaries = GetWALCommitBoundaries(lfs, source_wal);
	wal_size_one_table = commit_boundaries.first;
	wal_size_two_table = commit_boundaries.second;

	// now for all sizes in between these two sizes we have a torn write
	// try all of the possible sizes and truncate the WAL
	for (idx_t i = wal_size_one_table + 1; i + 1 < wal_size_two_table; i++) {
		DeleteDatabase(storage_database);
		CopyFile(lfs, source_database, storage_database);
		CopyFile(lfs, source_wal, storage_wal);
		FlipWALByte(lfs, storage_wal, i);
		{
			// flipping a byte in the checksum leads to a DataCorruptionException
			// flipping a byte in the size of a WAL entry leads to a torn write
			// we succeed on either of these cases here
			try {
				DuckDB db(storage_database, config.get());
				Connection con(db);
				REQUIRE_NO_FAIL(con.Query("FROM A"));
				REQUIRE_FAIL(con.Query("FROM B"));
			} catch (std::exception &ex) {
				ErrorData error(ex);
				if (error.Type() == ExceptionType::DATA_CORRUPTION) {
					REQUIRE(1 == 1);
				} else {
					throw;
				}
			}
		}
	}
	DeleteDatabase(storage_database);
	DeleteDatabase(source_database);
}

TEST_CASE("Test WAL entry size that overflows the bound check", "[storage][.]") {
	auto config = GetTestConfig();
	auto storage_database = TestCreatePath("wal_size_overflow");
	auto storage_wal = storage_database + ".wal";
	auto source_database = TestCreatePath("wal_size_overflow_source");
	auto source_wal = source_database + ".wal";

	LocalFileSystem lfs;
	config->options.checkpoint_wal_size = idx_t(-1);
	config->options.checkpoint_on_shutdown = false;
	config->options.abort_on_wal_failure = false;
	DeleteDatabase(storage_database);
	DeleteDatabase(source_database);
	{
		DuckDB db(source_database, config.get());
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE A (a INTEGER);"));
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE B (a INTEGER);"));
	}
	auto commit_boundaries = GetWALCommitBoundaries(lfs, source_wal);
	CopyFile(lfs, source_database, storage_database);
	CopyFile(lfs, source_wal, storage_wal);
	TruncateWAL(lfs, storage_wal, commit_boundaries.first);
	{
		// append an entry header with an entry size of 2^64 - 1: the size check must not overflow
		auto handle = lfs.OpenFile(storage_wal, FileFlags::FILE_FLAGS_WRITE);
		data_t header[2 * sizeof(uint64_t)];
		memset(header, 0xFF, sizeof(header));
		handle->Write(QueryContext(), header, sizeof(header), commit_boundaries.first);
	}
	{
		// the oversized entry is treated as a torn write: table A is replayed, table B is not
		DuckDB db(storage_database, config.get());
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("FROM A"));
		REQUIRE_FAIL(con.Query("FROM B"));
	}
	DeleteDatabase(storage_database);
	DeleteDatabase(source_database);
}
