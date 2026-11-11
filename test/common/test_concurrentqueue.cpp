#include "catch.hpp"

#ifndef DUCKDB_NO_THREADS
#include "blockingconcurrentqueue.h"
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

} // namespace

TEST_CASE("Concurrent queue padding preserves custom allocation and producer lifetime", "[concurrentqueue]") {
	CheckQueueTraits<std::size_t>();
	CheckQueueTraits<uint8_t>();
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
