#include "catch.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/map.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/storage/block_manager.hpp"
#include "duckdb/storage/buffer/block_handle.hpp"
#include "duckdb/storage/buffer/buffer_handle.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/metadata/metadata_manager.hpp"
#include "duckdb/storage/storage_manager.hpp"
#include "test_helpers.hpp"

#include <atomic>
#include <chrono>
#include <map>
#include <string>
#include <thread>

using namespace duckdb;

namespace {

struct BlockManagerRefs {
	BlockManager *block_manager;
	MetadataManager *metadata_manager;
	BufferManager *buffer_manager;
};

BlockManagerRefs GetBlockManagerRefs(DuckDB &db, Connection &con) {
	BlockManagerRefs refs;
	con.BeginTransaction();
	auto &catalog = Catalog::GetCatalog(*con.context, "");
	refs.block_manager = &StorageManager::Get(catalog).GetBlockManager();
	refs.metadata_manager = &refs.block_manager->GetMetadataManager();
	refs.buffer_manager = &refs.block_manager->GetBufferManager();
	con.Commit();
	return refs;
}

map<block_id_t, shared_ptr<BlockHandle>> SnapshotMetadataHandles(MetadataManager &metadata_manager) {
	map<block_id_t, shared_ptr<BlockHandle>> result;
	for (auto &handle : metadata_manager.GetBlocks()) {
		if (handle->BlockId() < MAXIMUM_BLOCK) {
			result[handle->BlockId()] = handle;
		}
	}
	return result;
}

} // namespace

// When the metadata manager writes to a disk-backed metadata block, ConvertToTransient replaces the
// registered handle and the next checkpoint rewrites the same disk block in place. Loading the
// replaced handle from disk could race that rewrite, so it must not read from disk anymore.
TEST_CASE("Pinning a replaced metadata block handle does not read from disk", "[storage]") {
	auto path = TestCreatePath("metadata_stale_handle.db");
	DeleteDatabase(path);
	{
		DuckDB db(path);
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(i INTEGER, s VARCHAR)"));
		REQUIRE_NO_FAIL(con.Query("INSERT INTO t SELECT i, repeat('x', 64) FROM range(10000) tbl(i)"));
		REQUIRE_NO_FAIL(con.Query("CHECKPOINT"));
		auto refs = GetBlockManagerRefs(db, con);

		// run checkpoints until a metadata block handle is replaced in place (same id, new handle)
		shared_ptr<BlockHandle> stale;
		for (idx_t round = 0; round < 200 && !stale; round++) {
			auto before = SnapshotMetadataHandles(*refs.metadata_manager);
			REQUIRE_NO_FAIL(con.Query("INSERT INTO t SELECT i, repeat('y', 64) FROM range(1000) tbl(i)"));
			REQUIRE_NO_FAIL(con.Query("CHECKPOINT"));
			auto after = SnapshotMetadataHandles(*refs.metadata_manager);
			for (auto &entry : before) {
				auto it = after.find(entry.first);
				if (it != after.end() && it->second.get() != entry.second.get()) {
					stale = entry.second;
					break;
				}
			}
		}
		REQUIRE(stale);

		// unload the stale handle so that pinning it would have to read from disk
		{
			auto lock = stale->GetMemory().GetLock();
			if (stale->GetMemory().CanUnload() == CanUnloadResult::CAN_UNLOAD) {
				stale->GetMemory().Unload(lock);
			}
		}
		REQUIRE(stale->GetMemory().GetState() == BlockState::BLOCK_UNLOADED);

		// the stale handle must not load its (possibly rewritten) disk block
		auto pin = refs.buffer_manager->Pin(stale);
		REQUIRE(!pin.IsValid());
	}
	DeleteDatabase(path);
}

// A handle that was replaced (ConvertToTransient unregisters the id, a later checkpoint registers a
// new handle for it) must not unregister the newer handle when it is destroyed.
TEST_CASE("A dying stale block handle does not unregister a newer handle", "[storage]") {
	auto path = TestCreatePath("stale_handle_unregister.db");
	DeleteDatabase(path);
	{
		DuckDB db(path);
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(i INTEGER)"));
		REQUIRE_NO_FAIL(con.Query("CHECKPOINT"));
		auto refs = GetBlockManagerRefs(db, con);
		auto &block_manager = *refs.block_manager;

		// take a fresh block id so that retiring it goes through the newly-used path
		auto test_id = block_manager.GetFreeBlockId();
		auto stale = block_manager.RegisterBlock(test_id);
		// orphan the handle, like MetadataManager::ConvertToTransient does
		block_manager.UnregisterBlock(test_id);
		// register a new handle for the same block id, like the next checkpoint does
		auto current = block_manager.RegisterBlock(test_id);
		REQUIRE(current.get() != stale.get());
		// retire the block id - the live handle moves it to free_blocks_in_use instead of the
		// free list, so the block cannot be reused while the handle exists
		block_manager.MarkBlockAsModified(test_id);
		auto free_blocks_before = block_manager.FreeBlocks();

		// destroying the stale handle must leave the newer handle registered and must not
		// release the block id to the free list
		stale.reset();
		REQUIRE(block_manager.FreeBlocks() == free_blocks_before);
		auto resolved = block_manager.RegisterBlock(test_id);
		REQUIRE(resolved.get() == current.get());

		// once the last handle dies, the block id must become available again
		resolved.reset();
		current.reset();
		REQUIRE(block_manager.FreeBlocks() == free_blocks_before + 1);
	}
	DeleteDatabase(path);
}

// A dying handle's registration check and free-list transition must be atomic: destroying expired
// handles for a block id while another thread re-registers the same id must never drop the live
// handle's registration (or release the id to the free list).
TEST_CASE("Concurrent handle destruction does not drop a newer registration", "[storage]") {
	auto path = TestCreatePath("stale_handle_unregister_race.db");
	DeleteDatabase(path);
	{
		DuckDB db(path);
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(i INTEGER)"));
		REQUIRE_NO_FAIL(con.Query("CHECKPOINT"));
		auto refs = GetBlockManagerRefs(db, con);
		auto &block_manager = *refs.block_manager;

		const block_id_t test_id = 2000000;
		std::atomic<bool> done {false};
		std::atomic<bool> registration_lost {false};

		// churn thread: repeatedly creates and destroys handles for the id - each destruction
		// runs the expired-registration cleanup that must not touch a newer live handle
		std::thread churn([&]() {
			while (!done) {
				auto handle = block_manager.RegisterBlock(test_id);
				handle.reset();
			}
		});

		for (idx_t i = 0; i < 100000; i++) {
			auto live = block_manager.RegisterBlock(test_id);
			// while 'live' exists, resolving the id must always return the same handle
			auto resolved = block_manager.RegisterBlock(test_id);
			if (resolved.get() != live.get()) {
				registration_lost = true;
				break;
			}
		}
		done = true;
		churn.join();
		REQUIRE(!registration_lost.load());
	}
	DeleteDatabase(path);
}

// Stress test: readers pin metadata block handles that have been replaced by
// MetadataManager::ConvertToTransient while a checkpoint rewrites the same disk block in place.
// Without the fix a stale, unloaded handle reads the block from disk; that read can overlap the
// checkpoint's write of the same block. On filesystems that do not serialize buffered reads
// against writes (e.g. ext4, tmpfs - unlike XFS) this produces a torn read and a spurious
// checksum failure that invalidates the database.
TEST_CASE("Stress stale metadata block handle pin against checkpoint rewrite", "[storage][.]") {
	auto path = TestCreatePath("metadata_pin_race.db");
	if (const char *dir = std::getenv("METADATA_PIN_RACE_DIR")) {
		// allow redirecting the database to a filesystem on which reads can tear (e.g. tmpfs)
		path = std::string(dir) + "/metadata_pin_race.db";
	}
	DeleteDatabase(path);
	{
		DuckDB db(path);
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(i INTEGER, s VARCHAR)"));
		REQUIRE_NO_FAIL(con.Query("INSERT INTO t SELECT i, repeat('x', 64) FROM range(50000) tbl(i)"));
		REQUIRE_NO_FAIL(con.Query("CHECKPOINT"));
		auto refs = GetBlockManagerRefs(db, con);
		auto &metadata_manager = *refs.metadata_manager;
		auto &buffer_manager = *refs.buffer_manager;

		std::atomic<bool> done {false};
		std::atomic<bool> checkpoint_active {false};
		std::atomic<idx_t> corruption_count {0};
		std::atomic<idx_t> other_error_count {0};
		std::atomic<idx_t> stale_pin_count {0};
		std::atomic<idx_t> stale_detected {0};
		std::string first_error;
		mutex error_lock;

		// recently replaced (stale) metadata block handles, newest at the back
		mutex stale_lock;
		std::vector<shared_ptr<BlockHandle>> recent_stale;

		std::thread writer([&]() {
			Connection wcon(db);
			for (idx_t i = 0; i < 5000 && !done; i++) {
				wcon.Query("DELETE FROM t WHERE i % 97 = " + std::to_string(i % 97));
				wcon.Query("INSERT INTO t SELECT i, repeat('y', 64) FROM range(500) tbl(i)");
				checkpoint_active = true;
				wcon.Query("CHECKPOINT");
				checkpoint_active = false;
			}
			done = true;
		});

		// detects metadata block handles that were replaced in place (same block id, new handle)
		std::thread collector([&]() {
			map<block_id_t, shared_ptr<BlockHandle>> previous;
			while (!done) {
				auto current = SnapshotMetadataHandles(metadata_manager);
				{
					lock_guard<mutex> guard(stale_lock);
					for (auto &entry : previous) {
						auto it = current.find(entry.first);
						// the handle is stale if its block id now maps to a different handle, or if
						// the id is gone from the snapshot (converted to a transient block - whose
						// disk block the next flush rewrites in place - or fully freed)
						if (it == current.end() || it->second.get() != entry.second.get()) {
							stale_detected++;
							recent_stale.push_back(entry.second);
						}
					}
					while (recent_stale.size() > 64) {
						recent_stale.erase(recent_stale.begin());
					}
				}
				previous = std::move(current);
				std::this_thread::sleep_for(std::chrono::microseconds(200));
			}
		});

		std::vector<std::thread> readers;
		for (idx_t r = 0; r < 8; r++) {
			readers.emplace_back([&]() {
				while (!done) {
					if (!checkpoint_active) {
						std::this_thread::sleep_for(std::chrono::microseconds(50));
						continue;
					}
					std::vector<shared_ptr<BlockHandle>> targets;
					{
						lock_guard<mutex> guard(stale_lock);
						targets = recent_stale;
					}
					for (auto &handle : targets) {
						if (done) {
							break;
						}
						// unload the handle (if possible) so that pinning would read from disk
						{
							auto lock = handle->GetMemory().GetLock();
							if (handle->GetMemory().CanUnload() == CanUnloadResult::CAN_UNLOAD) {
								handle->GetMemory().Unload(lock);
							}
						}
						try {
							auto pin = buffer_manager.Pin(handle);
							stale_pin_count++;
						} catch (std::exception &ex) {
							ErrorData error(ex);
							{
								lock_guard<mutex> guard(error_lock);
								if (first_error.empty()) {
									first_error = error.Message();
								}
							}
							if (error.Type() == ExceptionType::DATA_CORRUPTION) {
								corruption_count++;
								done = true;
							} else {
								other_error_count++;
							}
						}
					}
				}
			});
		}
		writer.join();
		collector.join();
		for (auto &t : readers) {
			t.join();
		}
		fprintf(stderr, "stale_detected=%llu stale_pins=%llu corruption=%llu other=%llu\nfirst error: %s\n",
		        (unsigned long long)stale_detected.load(), (unsigned long long)stale_pin_count.load(),
		        (unsigned long long)corruption_count.load(), (unsigned long long)other_error_count.load(),
		        first_error.c_str());
		// no pin may ever observe a checksum failure
		REQUIRE(corruption_count.load() == 0);
		REQUIRE(other_error_count.load() == 0);
	}
	DeleteDatabase(path);
}
