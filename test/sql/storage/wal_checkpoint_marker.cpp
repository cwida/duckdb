#include "catch.hpp"
#include "duckdb/common/enums/wal_type.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/local_file_system.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/common/serializer/buffered_file_writer.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/storage/storage_manager.hpp"
#include "duckdb/storage/write_ahead_log.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

static void RemoveCheckpointRecoveryFiles(FileSystem &fs, const string &database_path) {
	fs.TryRemoveFile(database_path + ".wal.checkpoint");
	fs.TryRemoveFile(database_path + ".wal.recovery");
}

static duckdb::vector<data_t> ReadFile(FileSystem &fs, const string &path) {
	auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ);
	auto size = handle->GetFileSize();
	duckdb::vector<data_t> result(size);
	if (size > 0) {
		handle->Read(QueryContext(), result.data(), size, 0);
	}
	return result;
}

static void WriteFile(FileSystem &fs, const string &path, const duckdb::vector<data_t> &contents) {
	BufferedFileWriter writer(fs, path, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW);
	if (!contents.empty()) {
		writer.WriteData(contents.data(), contents.size());
	}
	writer.Sync();
}

static void WriteTornLegacyCheckpointMarker(FileSystem &fs, const string &path) {
	BufferedFileWriter writer(fs, path, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW);
	BinarySerializer serializer(writer);
	serializer.Begin();
	serializer.WriteProperty(100, "wal_type", WALType::CHECKPOINT);
	serializer.WriteProperty(101, "meta_block", MetaBlockPointer(1000000, 0));
	// Deliberately omit serializer.End() to model a torn WAL v1 checkpoint entry.
	writer.Sync();
}

static idx_t WriteCheckpointMarkers(DuckDB &db, Connection &con, idx_t count) {
	auto database_name = DatabaseManager::GetDefaultDatabase(*con.context);
	auto attached_database = db.instance->GetDatabaseManager().GetDatabase(database_name);
	REQUIRE(attached_database);
	auto wal = attached_database->GetStorageManager().GetWAL();
	REQUIRE(wal);
	REQUIRE(count > 0);

	idx_t marker_end = 0;
	for (idx_t marker_idx = 0; marker_idx < count; marker_idx++) {
		// These deliberately do not match the current database root, which models an unsuccessful checkpoint.
		wal->WriteCheckpoint(MetaBlockPointer(1000000 + marker_idx, 0));
		marker_end = wal->Initialize().GetFileSize();
		wal->Flush();
	}
	return marker_end;
}

TEST_CASE("Truncate a failed checkpoint marker before making its WAL writable", "[storage][wal]") {
	auto config = GetTestConfig();
	config->options.checkpoint_wal_size = idx_t(-1);
	config->SetOption(CheckpointOnShutdownSetting::SettingIndex, Value::BOOLEAN(false));

	auto database_path = TestCreatePath("failed_checkpoint_marker");
	auto wal_path = database_path + ".wal";
	LocalFileSystem fs;
	DeleteDatabase(database_path);
	RemoveCheckpointRecoveryFiles(fs, database_path);

	{
		DuckDB db(database_path, config.get());
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE integers(i INTEGER)"));
		REQUIRE_NO_FAIL(con.Query("CHECKPOINT"));
		REQUIRE_NO_FAIL(con.Query("INSERT INTO integers VALUES (42)"));
		WriteCheckpointMarkers(db, con, 1);
	}

	REQUIRE(fs.FileExists(wal_path));
	REQUIRE_FALSE(fs.FileExists(database_path + ".wal.checkpoint"));
	auto wal_size_with_marker = fs.GetFileSize(*fs.OpenFile(wal_path, FileFlags::FILE_FLAGS_READ));

	{
		DuckDB db(database_path, config.get());
		Connection con(db);
		auto result = con.Query("SELECT * FROM integers ORDER BY i");
		REQUIRE_FALSE(result->HasError());
		REQUIRE(CHECK_COLUMN(result, 0, {42}));
	}

	auto wal_size_without_marker = fs.GetFileSize(*fs.OpenFile(wal_path, FileFlags::FILE_FLAGS_READ));
	REQUIRE(wal_size_without_marker < wal_size_with_marker);
	REQUIRE_FALSE(fs.FileExists(database_path + ".wal.recovery"));

	DeleteDatabase(database_path);
	RemoveCheckpointRecoveryFiles(fs, database_path);
}

TEST_CASE("Reject a checkpoint marker that is not at the end of the WAL", "[storage][wal]") {
	auto config = GetTestConfig();
	config->options.checkpoint_wal_size = idx_t(-1);
	config->SetOption(CheckpointOnShutdownSetting::SettingIndex, Value::BOOLEAN(false));

	auto database_path = TestCreatePath("checkpoint_marker_not_at_end");
	auto wal_path = database_path + ".wal";
	LocalFileSystem fs;
	DeleteDatabase(database_path);
	RemoveCheckpointRecoveryFiles(fs, database_path);

	{
		DuckDB db(database_path, config.get());
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE integers(i INTEGER)"));
		REQUIRE_NO_FAIL(con.Query("CHECKPOINT"));
		REQUIRE_NO_FAIL(con.Query("INSERT INTO integers VALUES (42)"));
		WriteCheckpointMarkers(db, con, 1);
		REQUIRE_NO_FAIL(con.Query("INSERT INTO integers VALUES (84)"));
	}
	auto wal_contents = ReadFile(fs, wal_path);

	bool threw = false;
	try {
		DuckDB db(database_path, config.get());
	} catch (std::exception &ex) {
		threw = true;
		REQUIRE(ErrorData(ex).Type() == ExceptionType::DATA_CORRUPTION);
		REQUIRE(StringUtil::Contains(ex.what(), "WAL checkpoint marker must be at the end of the WAL"));
	}
	REQUIRE(threw);

	REQUIRE(ReadFile(fs, wal_path) == wal_contents);
	REQUIRE_FALSE(fs.FileExists(database_path + ".wal.recovery"));

	DeleteDatabase(database_path);
	RemoveCheckpointRecoveryFiles(fs, database_path);
}

TEST_CASE("Recover a missing or torn WAL flush following a checkpoint marker", "[storage][wal]") {
	auto config = GetTestConfig();
	config->options.checkpoint_wal_size = idx_t(-1);
	config->SetOption(CheckpointOnShutdownSetting::SettingIndex, Value::BOOLEAN(false));
	config->SetOption(DebugAbortOnWalFailureSetting::SettingIndex, Value::BOOLEAN(false));

	LocalFileSystem fs;
	for (idx_t flush_bytes = 0; flush_bytes <= 1; flush_bytes++) {
		auto database_path = TestCreatePath("torn_checkpoint_flush_" + to_string(flush_bytes));
		auto wal_path = database_path + ".wal";
		DeleteDatabase(database_path);
		RemoveCheckpointRecoveryFiles(fs, database_path);

		idx_t marker_end;
		{
			DuckDB db(database_path, config.get());
			Connection con(db);
			REQUIRE_NO_FAIL(con.Query("CREATE TABLE integers(i INTEGER)"));
			REQUIRE_NO_FAIL(con.Query("CHECKPOINT"));
			REQUIRE_NO_FAIL(con.Query("INSERT INTO integers VALUES (42)"));
			marker_end = WriteCheckpointMarkers(db, con, 1);
		}

		auto wal_handle = fs.OpenFile(wal_path, FileFlags::FILE_FLAGS_WRITE);
		REQUIRE(marker_end + flush_bytes < wal_handle->GetFileSize());
		fs.Truncate(*wal_handle, marker_end + flush_bytes);
		wal_handle->Sync();
		wal_handle.reset();

		{
			DuckDB db(database_path, config.get());
			Connection con(db);
			auto result = con.Query("SELECT * FROM integers ORDER BY i");
			REQUIRE_FALSE(result->HasError());
			REQUIRE(CHECK_COLUMN(result, 0, {42}));
		}

		REQUIRE(fs.GetFileSize(*fs.OpenFile(wal_path, FileFlags::FILE_FLAGS_READ)) < marker_end);
		REQUIRE_FALSE(fs.FileExists(database_path + ".wal.recovery"));

		DeleteDatabase(database_path);
		RemoveCheckpointRecoveryFiles(fs, database_path);
	}
}

TEST_CASE("Treat an incomplete checkpoint marker as a torn WAL entry", "[storage][wal]") {
	auto config = GetTestConfig();
	config->options.checkpoint_wal_size = idx_t(-1);
	config->SetOption(CheckpointOnShutdownSetting::SettingIndex, Value::BOOLEAN(false));
	config->SetOption(DebugAbortOnWalFailureSetting::SettingIndex, Value::BOOLEAN(false));

	auto database_path = TestCreatePath("torn_checkpoint_marker");
	auto wal_path = database_path + ".wal";
	LocalFileSystem fs;
	DeleteDatabase(database_path);
	RemoveCheckpointRecoveryFiles(fs, database_path);

	{
		DuckDB db(database_path, config.get());
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE integers(i INTEGER)"));
		REQUIRE_NO_FAIL(con.Query("CHECKPOINT"));
	}
	WriteTornLegacyCheckpointMarker(fs, wal_path);
	auto torn_wal_contents = ReadFile(fs, wal_path);

	{
		DuckDB db(database_path, config.get());
		Connection con(db);
		auto result = con.Query("SELECT COUNT(*) FROM integers");
		REQUIRE_FALSE(result->HasError());
		REQUIRE(CHECK_COLUMN(result, 0, {0}));
	}

	REQUIRE(ReadFile(fs, wal_path) == torn_wal_contents);
	REQUIRE_FALSE(fs.FileExists(database_path + ".wal.recovery"));

	DeleteDatabase(database_path);
	RemoveCheckpointRecoveryFiles(fs, database_path);
}

TEST_CASE("Reject a mismatched WAL generation before truncating its checkpoint marker", "[storage][wal]") {
	auto config = GetTestConfig();
	config->options.checkpoint_wal_size = idx_t(-1);
	config->SetOption(CheckpointOnShutdownSetting::SettingIndex, Value::BOOLEAN(false));
	config->options.storage_compatibility = StorageCompatibility::FromString("v1.4.0");

	auto database_path = TestCreatePath("mismatched_checkpoint_marker");
	auto wal_path = database_path + ".wal";
	LocalFileSystem fs;
	DeleteDatabase(database_path);
	RemoveCheckpointRecoveryFiles(fs, database_path);

	{
		DuckDB db(database_path, config.get());
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE integers(i INTEGER)"));
		REQUIRE_NO_FAIL(con.Query("CHECKPOINT"));
		REQUIRE_NO_FAIL(con.Query("INSERT INTO integers VALUES (42)"));
		WriteCheckpointMarkers(db, con, 1);
	}
	auto old_wal_contents = ReadFile(fs, wal_path);

	// Recover this WAL and advance the database to the next checkpoint generation.
	{
		DuckDB db(database_path, config.get());
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CHECKPOINT"));
	}
	WriteFile(fs, wal_path, old_wal_contents);

	bool threw = false;
	try {
		DuckDB db(database_path, config.get());
	} catch (std::exception &ex) {
		threw = true;
		REQUIRE(StringUtil::Contains(ex.what(), "older version"));
	}
	REQUIRE(threw);

	REQUIRE(ReadFile(fs, wal_path) == old_wal_contents);
	REQUIRE_FALSE(fs.FileExists(database_path + ".wal.recovery"));

	DeleteDatabase(database_path);
	RemoveCheckpointRecoveryFiles(fs, database_path);
}

TEST_CASE("Reject multiple checkpoint markers before changing WAL files", "[storage][wal]") {
	auto config = GetTestConfig();
	config->options.checkpoint_wal_size = idx_t(-1);
	config->SetOption(CheckpointOnShutdownSetting::SettingIndex, Value::BOOLEAN(false));

	auto database_path = TestCreatePath("multiple_checkpoint_markers");
	auto wal_path = database_path + ".wal";
	auto checkpoint_wal_path = database_path + ".wal.checkpoint";
	auto recovery_wal_path = database_path + ".wal.recovery";
	LocalFileSystem fs;
	DeleteDatabase(database_path);
	RemoveCheckpointRecoveryFiles(fs, database_path);

	{
		DuckDB db(database_path, config.get());
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE integers(i INTEGER)"));
		REQUIRE_NO_FAIL(con.Query("CHECKPOINT"));
		REQUIRE_NO_FAIL(con.Query("INSERT INTO integers VALUES (42)"));
		WriteCheckpointMarkers(db, con, 2);
	}

	auto main_wal_contents = ReadFile(fs, wal_path);
	{
		auto checkpoint_handle =
		    fs.OpenFile(checkpoint_wal_path, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW);
		checkpoint_handle->Sync();
	}

	bool threw = false;
	try {
		DuckDB db(database_path, config.get());
	} catch (std::exception &ex) {
		threw = true;
		REQUIRE(ErrorData(ex).Type() == ExceptionType::DATA_CORRUPTION);
		REQUIRE(StringUtil::Contains(ex.what(), "WAL cannot contain more than one checkpoint marker"));
	}
	REQUIRE(threw);

	REQUIRE(ReadFile(fs, wal_path) == main_wal_contents);
	REQUIRE(fs.FileExists(checkpoint_wal_path));
	REQUIRE(fs.GetFileSize(*fs.OpenFile(checkpoint_wal_path, FileFlags::FILE_FLAGS_READ)) == 0);
	REQUIRE_FALSE(fs.FileExists(recovery_wal_path));

	DeleteDatabase(database_path);
	RemoveCheckpointRecoveryFiles(fs, database_path);
}
