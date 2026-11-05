#include "catch.hpp"
#include "duckdb/common/lru_cache.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/storage/object_cache.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/buffer/buffer_pool.hpp"
#include "test_helpers.hpp"

using namespace duckdb; // NOLINT

namespace {

struct TestObject : public ObjectCacheEntry {
	int value;
	TestObject(int value) : value(value) {
	}
	~TestObject() override = default;
	string GetObjectType() override {
		return ObjectType();
	}
	static string ObjectType() {
		return "TestObject";
	}
	optional_idx GetEstimatedCacheMemory() const override {
		return optional_idx {};
	}
};

struct AnotherTestObject : public ObjectCacheEntry {
	int value;
	AnotherTestObject(int value) : value(value) {
	}
	~AnotherTestObject() override = default;
	string GetObjectType() override {
		return ObjectType();
	}
	static string ObjectType() {
		return "AnotherTestObject";
	}
	optional_idx GetEstimatedCacheMemory() const override {
		return optional_idx {};
	}
};

struct EvictableTestObject : public ObjectCacheEntry {
	int value;
	idx_t size;
	EvictableTestObject(int value, idx_t size) : value(value), size(size) {
	}
	~EvictableTestObject() override = default;
	string GetObjectType() override {
		return ObjectType();
	}
	static string ObjectType() {
		return "EvictableTestObject";
	}
	optional_idx GetEstimatedCacheMemory() const override {
		return optional_idx(size);
	}
};

} // namespace

TEST_CASE("Test ObjectCache", "[api][object_cache]") {
	DuckDB db;
	Connection con(db);
	auto &context = *con.context;

	auto &cache = ObjectCache::GetObjectCache(context);

	REQUIRE(cache.GetObject("test") == nullptr);
	cache.Put("test", make_shared_ptr<TestObject>(42));

	REQUIRE(cache.GetObject("test") != nullptr);

	cache.Delete("test");
	REQUIRE(cache.GetObject("test") == nullptr);

	REQUIRE(cache.GetOrCreate<TestObject>("test", 42) != nullptr);
	REQUIRE(cache.Get<TestObject>("test") != nullptr);
	REQUIRE(cache.GetOrCreate<TestObject>("test", 1337)->value == 42);
	REQUIRE(cache.Get<TestObject>("test")->value == 42);

	REQUIRE(cache.GetOrCreate<AnotherTestObject>("test", 13) == nullptr);
}

TEST_CASE("Test ObjectCache memory accounting", "[api][object_cache]") {
	DuckDB db;
	Connection con(db);
	auto &context = *con.context;
	auto &cache = ObjectCache::GetObjectCache(context);
	auto &buffer_pool = DatabaseInstance::GetDatabase(context).GetBufferPool();
	const idx_t initial_memory = buffer_pool.GetUsedMemory();

	// Put and check accountable memory for buffer pool.
	constexpr idx_t obj_size = 1024 * 1024;
	cache.Put("evictable1", make_shared_ptr<EvictableTestObject>(1, obj_size));
	const idx_t after_put_memory = buffer_pool.GetUsedMemory();
	REQUIRE(after_put_memory == initial_memory + obj_size);

	// Delete and check accountable memory for buffer pool.
	cache.Delete("evictable1");
	const idx_t after_delete_memory = buffer_pool.GetUsedMemory();
	REQUIRE(after_delete_memory == initial_memory);
}

TEST_CASE("Test ObjectCache Manual Eviction", "[api][object_cache]") {
	DuckDB db;
	Connection con(db);
	auto &context = *con.context;
	auto &cache = ObjectCache::GetObjectCache(context);
	auto &buffer_pool = DatabaseInstance::GetDatabase(context).GetBufferPool();
	const idx_t initial_memory = buffer_pool.GetUsedMemory();
	REQUIRE(cache.IsEmpty());

	// Put and check accountable memory for buffer pool.
	constexpr idx_t obj_size = 1024 * 1024;
	constexpr idx_t obj_count = 10;
	for (idx_t idx = 0; idx < obj_count; ++idx) {
		cache.Put(StringUtil::Format("evictable%llu", idx), make_shared_ptr<EvictableTestObject>(idx, obj_size));
	}
	REQUIRE(cache.GetEntryCount() == 10);
	const idx_t after_put_memory = buffer_pool.GetUsedMemory();
	REQUIRE(after_put_memory == initial_memory + obj_size * obj_count);

	// Evict 5 objects, leaving 5 objects in cache
	const idx_t bytes_to_free = 5 * obj_size;
	idx_t freed = cache.EvictToReduceMemory(bytes_to_free);
	REQUIRE(freed >= bytes_to_free); // Should free at least the requested amount
	REQUIRE(cache.GetCurrentMemory() == 5 * obj_size);
	REQUIRE(cache.GetEntryCount() == 5);

	// First five items should be evicted.
	for (idx_t idx = 0; idx < 5; ++idx) {
		auto value = cache.GetObject(StringUtil::Format("evictable%llu", idx));
		REQUIRE(value == nullptr);
	}

	// Later five items should be kept.
	for (idx_t idx = 5; idx < 10; ++idx) {
		auto value = cache.GetObject(StringUtil::Format("evictable%llu", idx));
		REQUIRE(value != nullptr);
	}
	REQUIRE(!cache.IsEmpty());
}

TEST_CASE("Test ObjectCache Eviction To Memory Limit", "[api][object_cache]") {
	DuckDB db;
	auto &cache = db.instance->GetObjectCache();
	auto &buffer_pool = db.instance->GetBufferPool();
	const auto initial_memory = buffer_pool.GetUsedMemory();
	REQUIRE(cache.EvictToMemoryLimit(initial_memory));

	cache.Put("non-evictable", make_shared_ptr<TestObject>(1));
	cache.Put("zero-weight", make_shared_ptr<EvictableTestObject>(2, 0));
	constexpr idx_t OBJECT_COUNT = 40;
	constexpr idx_t OBJECT_SIZE = 1024;
	for (idx_t i = 0; i < OBJECT_COUNT; i++) {
		cache.Put(StringUtil::Format("entry%llu", i), make_shared_ptr<EvictableTestObject>(3, OBJECT_SIZE));
	}
	REQUIRE(buffer_pool.GetUsedMemory() == initial_memory + OBJECT_COUNT * OBJECT_SIZE);

	// Cross multiple batches, then stop partway through a batch.
	constexpr idx_t KEEP_COUNT = 7;
	const auto limit = initial_memory + KEEP_COUNT * OBJECT_SIZE;
	REQUIRE(cache.EvictToMemoryLimit(limit));
	REQUIRE(buffer_pool.GetUsedMemory() == limit);
	REQUIRE(cache.GetEntryCount() == KEEP_COUNT + 1);
	REQUIRE(cache.GetObject("zero-weight") == nullptr);
	REQUIRE(cache.GetObject("non-evictable") != nullptr);
	for (idx_t i = 0; i < OBJECT_COUNT; i++) {
		auto entry = cache.GetObject(StringUtil::Format("entry%llu", i));
		REQUIRE(bool(entry) == (i >= OBJECT_COUNT - KEEP_COUNT));
	}

	REQUIRE(cache.EvictToMemoryLimit(initial_memory));
	REQUIRE(buffer_pool.GetUsedMemory() == initial_memory);
	REQUIRE(!cache.IsEmpty());
	{
		TempBufferPoolReservation retained(MemoryTag::EXTENSION, buffer_pool, 64);
		REQUIRE(!cache.EvictToMemoryLimit(initial_memory));
		REQUIRE(buffer_pool.GetUsedMemory() == initial_memory + 64);
	}
	REQUIRE(buffer_pool.GetUsedMemory() == initial_memory);
}
