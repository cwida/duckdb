//===----------------------------------------------------------------------===//
//						 DuckDB
//
// duckdb/parallel/concurrentqueue.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#ifndef DUCKDB_NO_THREADS
#include "concurrentqueue.h"
#else

#include <cstddef>
#include <deque>
#include <queue>

namespace duckdb_moodycamel {

template <typename T>
class ConcurrentQueue;
template <typename T>
class BlockingConcurrentQueue;

struct ProducerToken {
	//! Constructor
	template <typename T>
	explicit ProducerToken(ConcurrentQueue<T> &) {
	}
	//! Constructor
	template <typename T>
	explicit ProducerToken(BlockingConcurrentQueue<T> &) {
	}
	//! Move constructor
	ProducerToken(ProducerToken &&) {
	}
	//! Is valid token?
	inline bool valid() const {
		return true;
	}
};

struct ConsumerToken {
	//! Constructor
	template <typename T>
	explicit ConsumerToken(ConcurrentQueue<T> &) {
	}
	//! Constructor
	template <typename T>
	explicit ConsumerToken(BlockingConcurrentQueue<T> &) {
	}
	//! Move constructor
	ConsumerToken(ConsumerToken &&) {
	}
};

template <typename T>
class ConcurrentQueue {
private:
	//! The standard library queue.
	std::queue<T, std::deque<T>> q;

public:
	//! Default constructor.
	ConcurrentQueue() = default;
	//! Constructor reserving capacity.
	explicit ConcurrentQueue(size_t capacity) {
		q.reserve(capacity);
	}

	//! Enqueue an item.
	template <typename U>
	bool enqueue(U &&item) {
		q.push(std::forward<U>(item));
		return true;
	}
	//! Try to dequeue an item.
	bool try_dequeue(T &item) {
		if (q.empty()) {
			return false;
		}
		item = std::move(q.front());
		q.pop();
		return true;
	}
	//! The single-threaded queue has no separate producer queues.
	bool try_dequeue_from_producer(ProducerToken const &, T &) {
		return false;
	}
	//! Get the size of the queue.
	size_t size_approx() const {
		return q.size();
	}
	//! Dequeues several elements from the queue.
	//! Returns the number of elements dequeued.
	template <typename It>
	size_t try_dequeue_bulk(It itemFirst, size_t max) {
		for (size_t i = 0; i < max; i++) {
			if (!try_dequeue(*itemFirst)) {
				return i;
			}
			itemFirst++;
		}
		return max;
	}
	//! Dequeues several elements from the queue using a consumer token.
	template <typename It>
	size_t try_dequeue_bulk(ConsumerToken &, It itemFirst, size_t max) {
		return try_dequeue_bulk(itemFirst, max);
	}

	template <typename It>
	bool enqueue_bulk(It itemFirst, size_t count) {
		for (size_t i = 0; i < count; i++) {
			q.push(std::move(*itemFirst++));
		}
		return true;
	}
	//! Enqueues several elements using a producer token.
	template <typename It>
	bool enqueue_bulk(ProducerToken const &, It itemFirst, size_t count) {
		return enqueue_bulk(itemFirst, count);
	}
};

} // namespace duckdb_moodycamel

#endif
