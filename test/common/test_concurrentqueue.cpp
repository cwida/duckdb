#include "catch.hpp"

#ifndef DUCKDB_NO_THREADS
#include "blockingconcurrentqueue.h"
#include "duckdb/storage/sharded_allocator_queue.hpp"
#include <thread>
#include <vector>

namespace {

template <class Size>
struct SmallQueueTraits : duckdb_moodycamel::ConcurrentQueueDefaultTraits {
	using size_t = Size;
	using index_t = uint64_t;
	static constexpr size_t BLOCK_SIZE = 2;
	static constexpr size_t EXPLICIT_INITIAL_INDEX_SIZE = 2;
	static constexpr size_t IMPLICIT_INITIAL_INDEX_SIZE = 2;
	static constexpr size_t INITIAL_IMPLICIT_PRODUCER_HASH_SIZE = 4;

	static void *malloc(size_t bytes) {
		return std::malloc(bytes);
	}

	static void free(void *ptr) {
		std::free(ptr);
	}
};

template <class Queue>
void CheckQueueAllocations() {
	Queue queue(0);
	duckdb_moodycamel::ProducerToken producer(queue);
	REQUIRE(producer.valid());

	uint64_t input[] = {0, 1, 2, 3, 4, 5, 6, 7};
	uint64_t output[8];
	for (uint32_t round = 0; round < 32; round++) {
		REQUIRE(queue.enqueue_bulk(producer, input, 8));
		REQUIRE(queue.try_dequeue_bulk(output, 8) == 8);
		for (uint32_t i = 0; i < 8; i++) {
			REQUIRE(output[i] == input[i]);
		}
		REQUIRE(queue.enqueue_bulk(input, 8));
		REQUIRE(queue.try_dequeue_bulk(output, 8) == 8);
		for (uint32_t i = 0; i < 8; i++) {
			REQUIRE(output[i] == input[i]);
		}
	}

	REQUIRE(queue.enqueue(producer, 42));
	Queue moved(std::move(queue));
	Queue swapped(0);
	swapped.swap(moved);
	REQUIRE(swapped.try_dequeue(output[0]));
	REQUIRE(output[0] == 42);
	REQUIRE_FALSE(swapped.try_dequeue(output[0]));
}

template <class Size>
void CheckQueueTraits() {
	using Traits = SmallQueueTraits<Size>;
	CheckQueueAllocations<duckdb_moodycamel::ConcurrentQueue<uint64_t, Traits>>();
	CheckQueueAllocations<duckdb_moodycamel::BlockingConcurrentQueue<uint64_t, Traits>>();
}

struct EnqueueOnThreadExit {
	duckdb_moodycamel::ConcurrentQueue<uint64_t> &queue;
	std::atomic<bool> &success;

	~EnqueueOnThreadExit() {
		uint64_t value = 2;
		if (!queue.enqueue_bulk(&value, 1)) {
			success.store(false);
		}
	}
};

struct FailingQueueTraits : SmallQueueTraits<std::size_t> {
	static uint32_t allocations_until_failure;
	static uint32_t allocation_count;

	static void *malloc(size_t bytes) {
		allocation_count++;
		if (allocations_until_failure > 0 && --allocations_until_failure == 0) {
			return nullptr;
		}
		return std::malloc(bytes);
	}
};

uint32_t FailingQueueTraits::allocations_until_failure = 0;
uint32_t FailingQueueTraits::allocation_count = 0;

struct AllocatorOverflowQueueTraits : SmallQueueTraits<std::size_t> {
	static std::atomic<bool> reject_allocations;
	static std::atomic<uint32_t> rejected_allocations;

	static void *malloc(size_t bytes) {
		if (reject_allocations.load()) {
			rejected_allocations.fetch_add(1);
			return nullptr;
		}
		return std::malloc(bytes);
	}
};

std::atomic<bool> AllocatorOverflowQueueTraits::reject_allocations {false};
std::atomic<uint32_t> AllocatorOverflowQueueTraits::rejected_allocations {0};

class AllocatorOverflowQueue : public duckdb_moodycamel::ConcurrentQueue<uint32_t, AllocatorOverflowQueueTraits> {
public:
	AllocatorOverflowQueue() : ConcurrentQueue(0) {
	}
};

struct RejectAllocatorQueueAllocations {
	RejectAllocatorQueueAllocations() {
		AllocatorOverflowQueueTraits::reject_allocations = true;
	}
	~RejectAllocatorQueueAllocations() {
		AllocatorOverflowQueueTraits::reject_allocations = false;
	}
};

} // namespace

TEST_CASE("Allocator queue preserves fixed-pool returns when metadata allocation fails",
          "[concurrentqueue][allocator_locality]") {
	const bool track_count = GENERATE(false, true);
	duckdb::ShardedAllocatorQueue<uint32_t, AllocatorOverflowQueue> queues(2, track_count);
	auto &first = queues.Get(0);
	auto &second = queues.Get(1);
	first.ReserveOverflow(4);
	second.ReserveOverflow(1);
	uint32_t input[] = {1, 2, 3, 4};
	const auto rejected = AllocatorOverflowQueueTraits::rejected_allocations.load();
	{
		RejectAllocatorQueueAllocations failures;
		REQUIRE(first.EnqueueBulk(input, 4));
		REQUIRE(second.Enqueue(5));
		CHECK(queues.SizeApprox() == 5);
		REQUIRE(AllocatorOverflowQueueTraits::rejected_allocations.load() > rejected);
	}
	// Both the recovered queue and overflow storage own live entries.
	REQUIRE(first.Enqueue(6));
	CHECK(queues.SizeApprox() == 6);
	bool seen[7] = {};
	uint32_t output[4];
	duckdb::idx_t total = 0;
	for (duckdb::idx_t shard = 0; shard < queues.Count(); shard++) {
		duckdb::idx_t obtained;
		while ((obtained = queues.Get(shard).TryDequeueBulk(output, 4)) != 0) {
			for (duckdb::idx_t i = 0; i < obtained; i++) {
				REQUIRE(output[i] >= 1);
				REQUIRE(output[i] <= 6);
				CHECK_FALSE(seen[output[i]]);
				seen[output[i]] = true;
			}
			total += obtained;
		}
	}
	CHECK(total == 6);
	CHECK(queues.SizeApprox() == 0);
	// A cache without a fixed-pool reserve remains fallible.
	duckdb::ShardedAllocatorQueue<uint32_t, AllocatorOverflowQueue> unreserved(1, track_count);
	RejectAllocatorQueueAllocations failures;
	CHECK_FALSE(unreserved.Get(0).Enqueue(1));
	CHECK(unreserved.SizeApprox() == 0);
}

TEST_CASE("Allocator queue overflow supports concurrent producers and consumers",
          "[concurrentqueue][allocator_locality]") {
	constexpr uint32_t COUNT = 128;
	duckdb::ShardedAllocatorQueue<uint32_t, AllocatorOverflowQueue> queues(1, true);
	auto &queue = queues.Get(0);
	queue.ReserveOverflow(COUNT);
	std::atomic<uint32_t> seen[COUNT] {};
	std::atomic<uint32_t> remaining {4};
	std::atomic<uint32_t> returned {0};
	std::atomic<bool> success {true};
	std::vector<std::thread> threads;
	RejectAllocatorQueueAllocations failures;
	for (uint32_t worker = 0; worker < 4; worker++) {
		threads.emplace_back([&, worker] {
			for (uint32_t value = worker; value < COUNT; value += 4) {
				if (!queue.Enqueue(value)) {
					success = false;
				}
				returned.fetch_add(1);
			}
			remaining.fetch_sub(1);
		});
		threads.emplace_back([&] {
			uint32_t value;
			do {
				if (queue.TryDequeue(value)) {
					if (value >= COUNT || seen[value].fetch_add(1) != 0) {
						success = false;
					}
				} else {
					std::this_thread::yield();
				}
			} while (remaining.load() != 0 || queue.SizeApprox() != 0);
		});
	}
	for (auto &thread : threads) {
		thread.join();
	}
	CHECK(success.load());
	CHECK(returned.load() == COUNT);
	for (auto &entry : seen) {
		CHECK(entry.load() == 1);
	}
	CHECK(queues.SizeApprox() == 0);
}

TEST_CASE("Concurrent queue padding preserves custom allocation and producer lifetime", "[concurrentqueue]") {
	CheckQueueTraits<std::size_t>();
	CheckQueueTraits<uint8_t>();
}

TEST_CASE("Concurrent queue retries a failed initial implicit index allocation", "[concurrentqueue]") {
	duckdb_moodycamel::ConcurrentQueue<uint64_t, FailingQueueTraits> queue(0);
	uint64_t input[] = {42, 84};
	uint64_t output[2];
	// Create the producer, but fail its initial index allocation.
	FailingQueueTraits::allocations_until_failure = 2;
	CHECK_FALSE(queue.try_enqueue_bulk(input, 2));
	CHECK(FailingQueueTraits::allocations_until_failure == 0);
	const auto allocations = FailingQueueTraits::allocation_count;
	CHECK_FALSE(queue.try_enqueue_bulk(input, 2));
	CHECK(FailingQueueTraits::allocation_count == allocations);
	REQUIRE(queue.enqueue_bulk(input, 2));
	REQUIRE(queue.try_dequeue_bulk(output, 2) == 2);
	CHECK(output[0] == input[0]);
	CHECK(output[1] == input[1]);
	CHECK_FALSE(queue.try_dequeue(output[0]));
}

TEST_CASE("Concurrent queue accepts enqueue from thread-local destructors", "[concurrentqueue]") {
	duckdb_moodycamel::ConcurrentQueue<uint64_t> queue;
	std::atomic<uint32_t> ready {0};
	std::atomic<bool> release {false};
	std::atomic<bool> success {true};
	std::vector<std::thread> workers;
	constexpr uint32_t thread_count = 64;
	for (uint32_t i = 0; i < thread_count; i++) {
		workers.emplace_back([&] {
			thread_local EnqueueOnThreadExit flush {queue, success};
			if (!queue.enqueue(1)) {
				success.store(false);
			}
			ready.fetch_add(1);
			while (!release.load()) {
				std::this_thread::yield();
			}
		});
	}
	while (ready.load() != thread_count) {
		std::this_thread::yield();
	}
	release.store(true);
	for (auto &worker : workers) {
		worker.join();
	}
	REQUIRE(success.load());
	uint32_t initial_count = 0;
	uint32_t exit_count = 0;
	uint64_t value;
	while (queue.try_dequeue(value)) {
		REQUIRE((value == 1 || value == 2));
		initial_count += value == 1;
		exit_count += value == 2;
	}
	REQUIRE(initial_count == thread_count);
	REQUIRE(exit_count == thread_count);
}
#endif
