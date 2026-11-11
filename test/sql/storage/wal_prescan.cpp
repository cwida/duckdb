#include "catch.hpp"
#include "duckdb/common/checksum.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/common/enums/wal_type.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/local_file_system.hpp"
#include "duckdb/common/serializer/binary_deserializer.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

class WALPayloadTestHelper {
public:
	explicit WALPayloadTestHelper(const string &database_path_p)
	    : database_path(database_path_p), wal_path(database_path + ".wal") {
		DeleteDatabase(database_path);
	}

	~WALPayloadTestHelper() {
		DeleteDatabase(database_path);
	}

	const string &GetDatabasePath() const {
		return database_path;
	}

	void CorruptPayload(WALType target_type) {
		auto target = FindPayload(target_type);
		auto handle = fs.OpenFile(wal_path, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_READ);
		auto wal_size = handle->GetFileSize();
		auto wal_contents = make_uniq_array<data_t>(wal_size);
		handle->Read(QueryContext(), wal_contents.get(), wal_size, 0);

		if (target.payload_offset + sizeof(field_id_t) > target.entry_offset + target.entry_size) {
			throw InternalException("Invalid WAL payload offset");
		}
		Store<field_id_t>(0, wal_contents.get() + target.payload_offset);

		auto entry = make_uniq_array<data_t>(target.entry_size);
		memcpy(entry.get(), wal_contents.get() + target.entry_offset, target.entry_size);
		auto checksum = Checksum(entry.get(), target.entry_size);
		Store<uint64_t>(checksum, wal_contents.get() + target.checksum_offset);

		handle->Write(QueryContext(), wal_contents.get(), wal_size, 0);
		handle->Sync();
	}

private:
	struct Entry {
		idx_t checksum_offset;
		idx_t entry_offset;
		idx_t entry_size;
		idx_t payload_offset;
		WALType type;
	};

	vector<Entry> ReadEntries() {
		auto handle = fs.OpenFile(wal_path, FileFlags::FILE_FLAGS_READ);
		auto wal_size = handle->GetFileSize();
		auto wal_contents = make_uniq_array<data_t>(wal_size);
		handle->Read(QueryContext(), wal_contents.get(), wal_size, 0);

		for (idx_t start = 0; start + 2 * sizeof(uint64_t) <= wal_size; start++) {
			idx_t offset = start;
			vector<Entry> entries;
			while (offset + 2 * sizeof(uint64_t) <= wal_size) {
				auto entry_size = Load<uint64_t>(wal_contents.get() + offset);
				auto stored_checksum = Load<uint64_t>(wal_contents.get() + offset + sizeof(uint64_t));
				auto entry_offset = offset + 2 * sizeof(uint64_t);
				if (entry_size > wal_size - entry_offset) {
					break;
				}

				// Checksum requires aligned input, but this scan tries every possible byte offset.
				auto entry = make_uniq_array<data_t>(entry_size);
				memcpy(entry.get(), wal_contents.get() + entry_offset, entry_size);
				if (Checksum(entry.get(), entry_size) != stored_checksum) {
					break;
				}

				MemoryStream stream(entry.get(), entry_size);
				BinaryDeserializer deserializer(stream);
				WALType entry_type;
				try {
					deserializer.Begin();
					entry_type = deserializer.ReadProperty<WALType>(100, "wal_type");
				} catch (std::exception &) {
					break;
				}

				entries.push_back({offset + sizeof(uint64_t), entry_offset, entry_size,
				                   entry_offset + stream.GetPosition(), entry_type});
				offset = entry_offset + entry_size;
			}
			if (offset == wal_size && !entries.empty()) {
				return entries;
			}
		}
		throw InternalException("Could not find framed WAL entries");
	}

	Entry FindPayload(WALType target_type) {
		auto entries = ReadEntries();
		optional<Entry> target;
		for (const auto &entry : entries) {
			if (!target && entry.type == target_type) {
				target = entry;
			} else if (target && entry.type == WALType::CHECKPOINT) {
				return *target;
			}
		}
		throw InternalException("Could not find %s entry followed by a checkpoint in WAL",
		                        EnumUtil::ToString(target_type));
	}

private:
	LocalFileSystem fs;
	string database_path;
	string wal_path;
};

TEST_CASE("WAL checkpoint prescan skips unnecessary framed payloads", "[storage][wal]") {
	struct TestCase {
		const char *name;
		WALType wal_type;
		const char *statement;
		const char *query;
		vector<Value> expected;
	};
	vector<TestCase> test_cases {
	    {"create_table", WALType::CREATE_TABLE, "CREATE TABLE created(i INTEGER)", "SELECT count(*) FROM created", {0}},
	    {"insert",
	     WALType::INSERT_TUPLE,
	     "INSERT INTO integers VALUES (4)",
	     "SELECT i FROM integers ORDER BY i",
	     {1, 2, 3, 4}},
	    {"delete",
	     WALType::DELETE_TUPLE,
	     "DELETE FROM integers WHERE i = 2",
	     "SELECT i FROM integers ORDER BY i",
	     {1, 3}},
	    {"update",
	     WALType::UPDATE_TUPLE,
	     "UPDATE integers SET i = 20 WHERE i = 2",
	     "SELECT i FROM integers ORDER BY i",
	     {1, 3, 20}},
	};

	for (const auto &test : test_cases) {
		WALPayloadTestHelper helper(TestCreatePath("wal_prescan_skip_" + string(test.name)));
		{
			auto config = GetTestConfig();
			config->options.checkpoint_wal_size = idx_t(-1);
			config->SetOption(CheckpointOnShutdownSetting::SettingIndex, Value::BOOLEAN(false));
			DuckDB db(helper.GetDatabasePath(), config.get());
			Connection con(db);
			REQUIRE_NO_FAIL(con.Query("CREATE TABLE integers(i INTEGER)"));
			REQUIRE_NO_FAIL(con.Query("INSERT INTO integers VALUES (1), (2), (3)"));
			REQUIRE_NO_FAIL(con.Query("CHECKPOINT"));
			REQUIRE_NO_FAIL(con.Query(test.statement));
			REQUIRE_NO_FAIL(con.Query("SET debug_checkpoint_abort = 'before_truncate'"));
			REQUIRE_FAIL(con.Query("CHECKPOINT"));
		}

		// Keep the entry framing and checksum valid while making the payload unreadable.
		helper.CorruptPayload(test.wal_type);

		auto config = GetTestConfig();
		config->SetOption(DebugAbortOnWalFailureSetting::SettingIndex, Value::BOOLEAN(true));
		DuckDB db(helper.GetDatabasePath(), config.get());
		Connection con(db);
		auto result = con.Query(test.query);
		REQUIRE_NO_FAIL(*result);
		REQUIRE(CHECK_COLUMN(result, 0, test.expected));
	}
}

TEST_CASE("WAL replay validates DML payloads skipped during prescan", "[storage][wal]") {
	WALPayloadTestHelper helper(TestCreatePath("wal_prescan_validate_dml"));
	{
		auto config = GetTestConfig();
		config->options.checkpoint_wal_size = idx_t(-1);
		config->SetOption(CheckpointOnShutdownSetting::SettingIndex, Value::BOOLEAN(false));
		DuckDB db(helper.GetDatabasePath(), config.get());
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE integers(i INTEGER)"));
		REQUIRE_NO_FAIL(con.Query("CHECKPOINT"));
		REQUIRE_NO_FAIL(con.Query("INSERT INTO integers VALUES (1)"));
		REQUIRE_NO_FAIL(con.Query("SET debug_checkpoint_abort = 'before_header'"));
		REQUIRE_FAIL(con.Query("CHECKPOINT"));
	}

	helper.CorruptPayload(WALType::INSERT_TUPLE);

	auto config = GetTestConfig();
	config->SetOption(DebugAbortOnWalFailureSetting::SettingIndex, Value::BOOLEAN(true));
	try {
		DuckDB db(helper.GetDatabasePath(), config.get());
		FAIL("Expected WAL replay to fail");
	} catch (Exception &ex) {
		ErrorData error(ex);
		REQUIRE(error.Type() == ExceptionType::SERIALIZATION);
		REQUIRE(error.RawMessage().find("field id mismatch, expected: 101, got: 0") != string::npos);
	}
}
