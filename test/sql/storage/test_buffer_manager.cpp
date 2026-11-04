#include <duckdb/main/settings.hpp>

#include "catch.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/storage/buffer/block_handle.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/storage_info.hpp"
#include "duckdb/common/enums/memory_tag.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

TEST_CASE("Test storing a big string that exceeds buffer manager size", "[storage][.]") {
	duckdb::unique_ptr<QueryResult> result;
	auto storage_database = TestCreatePath("storage_test");
	auto config = GetTestConfig();
	config->SetOptionByName("default_block_size", Value::UBIGINT(DEFAULT_BLOCK_ALLOC_SIZE));
	config->options.maximum_threads = 1;
	// ZSTD can store this in a smaller way, force uncompressed so the 5mb max test correctly fails
	config->SetOptionByName("force_compression", "uncompressed");

	uint64_t string_length = 64;
	uint64_t desired_size = 10000000; // desired size is 10MB
	uint64_t iteration = 2;
	// make sure the database does not exist
	DeleteDatabase(storage_database);
	{
		// create a database and insert the big string
		DuckDB db(storage_database, config.get());
		Connection con(db);
		string big_string = string(string_length, 'a');
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE test (a VARCHAR, j BIGINT);"));
		REQUIRE_NO_FAIL(con.Query("INSERT INTO test VALUES ('" + big_string + "', 1)"));
		while (string_length < desired_size) {
			REQUIRE_NO_FAIL(con.Query("INSERT INTO test SELECT repeat(a, 10), " + to_string(iteration) + " FROM test"));
			REQUIRE_NO_FAIL(con.Query("DELETE FROM test WHERE j=" + to_string(iteration - 1)));
			iteration++;
			string_length *= 10;
		}

		// check the length
		result = con.Query("SELECT LENGTH(a) FROM test");
		REQUIRE(CHECK_COLUMN(result, 0, {Value::BIGINT(string_length)}));
		result = con.Query("SELECT j FROM test");
		REQUIRE(CHECK_COLUMN(result, 0, {Value::BIGINT(iteration - 1)}));
	}
	{
		DuckDB db(storage_database, config.get());
		Connection con(db);
		result = con.Query("SELECT LENGTH(a) FROM test");
		REQUIRE(CHECK_COLUMN(result, 0, {Value::BIGINT(string_length)}));
		result = con.Query("SELECT j FROM test");
		REQUIRE(CHECK_COLUMN(result, 0, {Value::BIGINT(iteration - 1)}));
	}
	// now reload the database, but this time with a max memory of 5MB
	{
		config->options.maximum_memory = 5000000;
		DuckDB db(storage_database, config.get());
		Connection con(db);
		// we can still select the integer
		result = con.Query("SELECT j FROM test");
		REQUIRE(CHECK_COLUMN(result, 0, {Value::BIGINT(iteration - 1)}));
		// however the string is too big to fit in our buffer manager
		REQUIRE_FAIL(con.Query("SELECT LENGTH(a) FROM test"));
	}
	{
		// reloading with a bigger limit again makes it work
		config->options.maximum_memory = (idx_t)-1;
		DuckDB db(storage_database, config.get());
		Connection con(db);
		result = con.Query("SELECT LENGTH(a) FROM test");
		REQUIRE(CHECK_COLUMN(result, 0, {Value::BIGINT(string_length)}));
		result = con.Query("SELECT j FROM test");
		REQUIRE(CHECK_COLUMN(result, 0, {Value::BIGINT(iteration - 1)}));
	}
	DeleteDatabase(storage_database);
}

TEST_CASE("Modifying the buffer manager limit at runtime for an in-memory database", "[storage][.]") {
	duckdb::unique_ptr<QueryResult> result;

	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("PRAGMA threads=1"));
	REQUIRE_NO_FAIL(con.Query("PRAGMA force_compression='uncompressed'"));
	REQUIRE_NO_FAIL(con.Query("PRAGMA temp_directory=''"));

	// initialize an in-memory database of size 10MB
	uint64_t table_size = (1000 * 1000) / sizeof(int);

	REQUIRE_NO_FAIL(con.Query("CREATE TABLE test (a INTEGER);"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO test VALUES (1), (2), (3), (NULL)"));

	idx_t not_null_size = 3;
	idx_t size = 4;
	idx_t sum = 6;
	for (; size < table_size; size *= 2) {
		REQUIRE_NO_FAIL(con.Query("INSERT INTO test SELECT * FROM test"));
		not_null_size *= 2;
		sum *= 2;
	}

	result = con.Query("SELECT COUNT(*), COUNT(a), SUM(a) FROM test");
	REQUIRE(CHECK_COLUMN(result, 0, {Value::BIGINT(size)}));
	REQUIRE(CHECK_COLUMN(result, 1, {Value::BIGINT(not_null_size)}));
	REQUIRE(CHECK_COLUMN(result, 2, {Value::BIGINT(sum)}));

	// we can set the memory limit to 1GB
	REQUIRE_NO_FAIL(con.Query("PRAGMA memory_limit='1GB'"));
	// but we cannot set it below 10MB
	REQUIRE_FAIL(con.Query("PRAGMA memory_limit='1MB'"));

	// if we make room by dropping the table, we can set it to 1MB though
	REQUIRE_NO_FAIL(con.Query("DROP TABLE test"));
	REQUIRE_NO_FAIL(con.Query("PRAGMA memory_limit='1MB'"));

	// also test that large strings are properly deleted
	// reset the memory limit
	REQUIRE_NO_FAIL(con.Query("PRAGMA memory_limit=-1"));

	// create a table with a large string (10MB)
	uint64_t string_length = 64;
	uint64_t desired_size = 10000000; // desired size is 10MB
	uint64_t iteration = 2;

	string big_string = string(string_length, 'a');
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE test (a VARCHAR, j BIGINT);"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO test VALUES ('" + big_string + "', 1)"));
	while (string_length < desired_size) {
		REQUIRE_NO_FAIL(
		    con.Query("INSERT INTO test SELECT a||a||a||a||a||a||a||a||a||a, " + to_string(iteration) + " FROM test"));
		REQUIRE_NO_FAIL(con.Query("DELETE FROM test WHERE j=" + to_string(iteration - 1)));
		iteration++;
		string_length *= 10;
	}

	// now we cannot set the memory limit to 1MB again
	REQUIRE_FAIL(con.Query("PRAGMA memory_limit='1MB'"));
	// but dropping the table allows us to set the memory limit to 1MB again
	REQUIRE_NO_FAIL(con.Query("DROP TABLE test"));
	REQUIRE_NO_FAIL(con.Query("PRAGMA memory_limit='1MB'"));
}

TEST_CASE("Test buffer manager variable size allocations", "[storage][.]") {
	auto storage_database = TestCreatePath("storage_test");
	auto config = GetTestConfig();
	config->SetOptionByName("default_block_size", Value::UBIGINT(DEFAULT_BLOCK_ALLOC_SIZE));

	// make sure the database does not exist
	DeleteDatabase(storage_database);
	DuckDB db(storage_database, config.get());
	Connection con(db);

	auto &buffer_manager = BufferManager::GetBufferManager(*con.context);
	CHECK(buffer_manager.GetUsedMemory() == 0);

	idx_t requested_size = 424242;
	auto pin = buffer_manager.Allocate(MemoryTag::EXTENSION, requested_size, false);
	auto block = pin.GetBlockHandle();
	CHECK(buffer_manager.GetUsedMemory() >= requested_size + block->GetBlockHeaderSize());

	pin.Destroy();
	block.reset();
	CHECK(buffer_manager.GetUsedMemory() == 0);
}

TEST_CASE("Test buffer manager buffer re-use", "[storage][.]") {
	auto storage_database = TestCreatePath("storage_test");
	auto config = GetTestConfig();
	config->SetOptionByName("default_block_size", Value::UBIGINT(DEFAULT_BLOCK_ALLOC_SIZE));

	// make sure the database does not exist
	DeleteDatabase(storage_database);
	DuckDB db(storage_database, config.get());
	Connection con(db);

	auto &buffer_manager = BufferManager::GetBufferManager(*con.context);
	CHECK(buffer_manager.GetUsedMemory() == 0);

	// Set memory limit to hold exactly 10 blocks
	idx_t pin_count = 10;
	auto block_alloc_size = Settings::Get<DefaultBlockSizeSetting>(*config);
	auto block_size = block_alloc_size - Storage::DEFAULT_BLOCK_HEADER_SIZE;
	REQUIRE_NO_FAIL(con.Query(StringUtil::Format("PRAGMA memory_limit='%lldB'", block_alloc_size * pin_count)));

	// Create 40 blocks, but don't hold the pin
	// They will be added to the eviction queue and the buffers will be re-used
	idx_t block_count = 40;
	duckdb::vector<duckdb::shared_ptr<BlockHandle>> blocks;
	blocks.reserve(block_count);
	for (idx_t i = 0; i < block_count; i++) {
		auto pin = buffer_manager.Allocate(MemoryTag::EXTENSION, block_size, false);
		blocks.push_back(pin.GetBlockHandle());
		// used memory should increment by exactly one block at a time, up to 10
		CHECK(buffer_manager.GetUsedMemory() == MinValue<idx_t>(pin_count, i + 1) * block_alloc_size);
	}

	// now pin them one by one - cycling through should trigger more buffer re-use
	for (idx_t i = 0; i < block_count; i++) {
		auto pin = buffer_manager.Pin(blocks[i]);
		CHECK(buffer_manager.GetUsedMemory() == pin_count * block_alloc_size);
	}

	// Clear all blocks and verify we go back down to 0 used memory
	blocks.clear();
	CHECK(buffer_manager.GetUsedMemory() == 0);

	// now we do exactly the same, but with variable-sized blocks
	idx_t variable_block_size = 424242;
	auto alloc_size = BufferManager::GetAllocSize(variable_block_size + Storage::DEFAULT_BLOCK_HEADER_SIZE);
	REQUIRE_NO_FAIL(con.Query(StringUtil::Format("PRAGMA memory_limit='%lldB'", alloc_size * pin_count)));
	for (idx_t i = 0; i < block_count; i++) {
		auto pin = buffer_manager.Allocate(MemoryTag::EXTENSION, variable_block_size, false);
		blocks.push_back(pin.GetBlockHandle());
		CHECK(buffer_manager.GetUsedMemory() == MinValue<idx_t>(pin_count, i + 1) * alloc_size);
	}
	for (idx_t i = 0; i < block_count; i++) {
		auto pin = buffer_manager.Pin(blocks[i]);
		CHECK(buffer_manager.GetUsedMemory() == pin_count * alloc_size);
	}
	blocks.clear();
	CHECK(buffer_manager.GetUsedMemory() == 0);

	// again, the same but incrementing variable_block_size by 1 for every block (has same alloc_size)
	for (idx_t i = 0; i < block_count; i++) {
		auto pin = buffer_manager.Allocate(MemoryTag::EXTENSION, variable_block_size, false);
		blocks.push_back(pin.GetBlockHandle());
		CHECK(buffer_manager.GetUsedMemory() == MinValue<idx_t>(pin_count, i + 1) * alloc_size);
		// increment variable_block_size
		variable_block_size++;
		CHECK(BufferManager::GetAllocSize(variable_block_size + pin.GetBlockHandle()->GetBlockHeaderSize()) ==
		      alloc_size);
	}
	for (idx_t i = 0; i < block_count; i++) {
		auto pin = buffer_manager.Pin(blocks[i]);
		CHECK(buffer_manager.GetUsedMemory() == pin_count * alloc_size);
	}
	blocks.clear();
	CHECK(buffer_manager.GetUsedMemory() == 0);

	// reset block size and do the same but decrement by 1 for every block (still same alloc_size)
	variable_block_size = 424242;
	for (idx_t i = 0; i < block_count; i++) {
		auto pin = buffer_manager.Allocate(MemoryTag::EXTENSION, variable_block_size, false);
		blocks.push_back(pin.GetBlockHandle());
		CHECK(buffer_manager.GetUsedMemory() == MinValue<idx_t>(pin_count, i + 1) * alloc_size);
		// increment variable_block_size
		variable_block_size--;
		CHECK(BufferManager::GetAllocSize(variable_block_size + pin.GetBlockHandle()->GetBlockHeaderSize()) ==
		      alloc_size);
	}
	for (idx_t i = 0; i < block_count; i++) {
		auto pin = buffer_manager.Pin(blocks[i]);
		CHECK(buffer_manager.GetUsedMemory() == pin_count * alloc_size);
	}
	blocks.clear();
	CHECK(buffer_manager.GetUsedMemory() == 0);
}

TEST_CASE("Test evicted_data not double-decremented for variable-sized blocks", "[storage][.]") {
	auto storage_database = TestCreatePath("storage_test");
	auto config = GetTestConfig();
	config->SetOptionByName("default_block_size", Value::UBIGINT(DEFAULT_BLOCK_ALLOC_SIZE));
	config->options.maximum_threads = 1;
	DeleteDatabase(storage_database);
	DuckDB db(storage_database, config.get());
	Connection con(db);
	auto &buffer_manager = BufferManager::GetBufferManager(*con.context);

	idx_t variable_block_size = 424242;
	auto alloc_size = BufferManager::GetAllocSize(variable_block_size + Storage::DEFAULT_BLOCK_HEADER_SIZE);
	REQUIRE_NO_FAIL(con.Query(StringUtil::Format("PRAGMA memory_limit='%lldB'", alloc_size)));
	REQUIRE_NO_FAIL(con.Query("PRAGMA temp_directory='" + TestCreatePath("eviction_tracking_temp") + "'"));

	shared_ptr<BlockHandle> block_a = nullptr;
	shared_ptr<BlockHandle> block_b = nullptr;
	{
		auto pin = buffer_manager.Allocate(MemoryTag::EXTENSION, variable_block_size, false);
		block_a = pin.GetBlockHandle();
	}
	{
		auto pin = buffer_manager.Allocate(MemoryTag::EXTENSION, variable_block_size, false);
		block_b = pin.GetBlockHandle();
	}

	// Read block_a back from disk.
	{ auto pin = buffer_manager.Pin(block_a); }

	// Destroy both handles, so remaining temp files are cleaned up.
	block_a.reset();
	block_b.reset();

	// For now there should be no blocks on disk, and evicted_data must be 0.
	for (auto &entry : buffer_manager.GetMemoryUsageInfo()) {
		if (entry.tag == MemoryTag::EXTENSION) {
			CHECK(entry.evicted_data == 0);
		}
	}
}

TEST_CASE("Test buffer allocator", "[storage][.]") {
	auto storage_database = TestCreatePath("storage_test");
	auto config = GetTestConfig();
	config->SetOptionByName("default_block_size", Value::UBIGINT(DEFAULT_BLOCK_ALLOC_SIZE));

	// make sure the database does not exist
	DeleteDatabase(storage_database);
	DuckDB db(storage_database, config.get());
	Connection con(db);

	auto &buffer_manager = BufferManager::GetBufferManager(*con.context);
	CHECK(buffer_manager.GetUsedMemory() == 0);

	const idx_t limit = 1000000000;
	REQUIRE_NO_FAIL(con.Query(StringUtil::Format("PRAGMA memory_limit='%lldB'", limit)));

	auto &allocator = buffer_manager.GetBufferAllocator();
	auto block_size = Settings::Get<DefaultBlockSizeSetting>(*config) - Storage::DEFAULT_BLOCK_HEADER_SIZE;
	idx_t requested_size = block_size;
	auto pointer = allocator.AllocateData(requested_size);
	idx_t current_size = requested_size;
	CHECK(buffer_manager.GetUsedMemory() == requested_size);

	// increase
	for (; requested_size < limit; requested_size *= 2) {
		pointer = allocator.ReallocateData(pointer, current_size, requested_size);
		current_size = requested_size;
		CHECK(buffer_manager.GetUsedMemory() == requested_size);
	}

	// decrease
	for (; requested_size >= block_size; requested_size /= 2) {
		pointer = allocator.ReallocateData(pointer, current_size, requested_size);
		current_size = requested_size;
		CHECK(buffer_manager.GetUsedMemory() == requested_size);
	}

	allocator.FreeData(pointer, current_size);
}
