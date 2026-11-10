#include "catch.hpp"
#include "duckdb/common/checksum.hpp"
#include "duckdb/common/local_file_system.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/common/enums/wal_type.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"
#include "duckdb/parser/parsed_data/transaction_info.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

//! Append a framed [size][checksum][payload] entry to the WAL
static void AppendWALEntry(FileSystem &fs, const string &path, MemoryStream &payload) {
	auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_APPEND);
	uint64_t size = payload.GetPosition();
	uint64_t checksum = Checksum(payload.GetData(), size);
	handle->Write(QueryContext(), &size, sizeof(uint64_t));
	handle->Write(QueryContext(), &checksum, sizeof(uint64_t));
	handle->Write(QueryContext(), payload.GetData(), size);
}

//! Replay a WAL whose only new entry is an ALTER_INFO with the given info, expecting the given error
static void TestReplayInvalidAlterInfo(const string &name, unique_ptr<ParseInfo> info, const string &expected_error) {
	auto config = GetTestConfig();
	auto storage_database = TestCreatePath(name);
	auto storage_wal = storage_database + ".wal";

	LocalFileSystem lfs;
	config->options.checkpoint_wal_size = idx_t(-1);
	config->options.checkpoint_on_shutdown = false;
	DeleteDatabase(storage_database);
	{
		DuckDB db(storage_database, config.get());
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE t (i INTEGER);"));
	}
	REQUIRE(lfs.FileExists(storage_wal));

	// the ALTER_INFO entry, followed by a flush
	{
		MemoryStream alter_entry;
		BinarySerializer serializer(alter_entry);
		serializer.Begin();
		serializer.WriteProperty(100, "wal_type", WALType::ALTER_INFO);
		serializer.WriteProperty(101, "info", info);
		serializer.End();
		AppendWALEntry(lfs, storage_wal, alter_entry);
	}
	{
		MemoryStream flush_entry;
		BinarySerializer serializer(flush_entry);
		serializer.Begin();
		serializer.WriteProperty(100, "wal_type", WALType::WAL_FLUSH);
		serializer.End();
		AppendWALEntry(lfs, storage_wal, flush_entry);
	}
	REQUIRE_THROWS_WITH(DuckDB(storage_database, config.get()), Catch::Contains(expected_error));
	DeleteDatabase(storage_database);
}

TEST_CASE("Test WAL alter entry carrying non-alter information", "[storage][.]") {
	TestReplayInvalidAlterInfo("wal_alter_wrong_info", make_uniq<TransactionInfo>(TransactionType::BEGIN_TRANSACTION),
	                           "alter entry does not carry alter information");
}

TEST_CASE("Test WAL add constraint entry without a constraint", "[storage][.]") {
	AlterEntryData data(QualifiedName(Identifier(""), Identifier("main"), Identifier("t")),
	                    OnEntryNotFound::THROW_EXCEPTION);
	TestReplayInvalidAlterInfo("wal_add_constraint_without_constraint", make_uniq<AddConstraintInfo>(data, nullptr),
	                           "add constraint entry is missing its constraint");
}
