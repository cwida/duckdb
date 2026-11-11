//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/sharded_allocator_queue.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/atomic.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/parallel/concurrentqueue.hpp"

#include <new>
#include <type_traits>

namespace duckdb {

template <class T, class QUEUE = duckdb_moodycamel::ConcurrentQueue<T>>
class ShardedAllocatorQueue {
	static_assert(std::is_trivially_copyable<T>::value, "Allocator queue entries must be trivially copyable");

public:
	struct alignas(64) Shard {
		bool Enqueue(T value) {
			return EnqueueInternal(
			    1, [&] { return q.enqueue(value); }, [&] { return EnqueueOverflow(&value, 1); });
		}
		template <class ITERATOR>
		bool EnqueueBulk(ITERATOR begin, idx_t size) {
#ifdef DUCKDB_NO_THREADS
			// The single-threaded queue can throw after publishing a prefix of a batch.
			for (idx_t i = 0; i < size; i++) {
				if (!Enqueue(begin[i])) {
					return false;
				}
			}
			return true;
#else
			return EnqueueInternal(
			    size, [&] { return q.enqueue_bulk(begin, size); }, [&] { return EnqueueOverflow(begin, size); });
#endif
		}
		bool TryDequeue(T &value) {
			return TryDequeueInternal([&] { return idx_t(q.try_dequeue(value)); },
			                          [&] { return DequeueOverflow(&value, 1); }) != 0;
		}
		template <class ITERATOR>
		idx_t TryDequeueBulk(ITERATOR begin, idx_t size) {
			return TryDequeueInternal([&] { return q.try_dequeue_bulk(begin, size); },
			                          [&] { return DequeueOverflow(begin, size); });
		}
		idx_t SizeApprox() const {
			return track_count ? count.load(std::memory_order_relaxed)
			                   : q.size_approx() + overflow_count.load(std::memory_order_relaxed);
		}
		//! Reserve before publication: fixed-pool returns must survive queue allocation failure.
		void ReserveOverflow(idx_t capacity) {
			if (capacity == 0) {
				overflow.reset();
			} else {
				overflow = make_uniq<vector<T>>(capacity);
			}
		}

		bool track_count = false;

	private:
		template <class ENQUEUE, class ENQUEUE_OVERFLOW>
		bool EnqueueInternal(idx_t size, ENQUEUE enqueue, ENQUEUE_OVERFLOW enqueue_overflow) {
			if (track_count) {
				count.fetch_add(size, std::memory_order_relaxed);
			}
			try {
				bool queued = false;
				try {
					queued = enqueue();
				} catch (const std::bad_alloc &) {
					// The single-threaded queue reports allocation failure by throwing.
				}
				if (queued || enqueue_overflow()) {
					return true;
				}
			} catch (...) {
				if (track_count) {
					count.fetch_sub(size, std::memory_order_relaxed);
				}
				throw;
			}
			if (track_count) {
				count.fetch_sub(size, std::memory_order_relaxed);
			}
			return false;
		}
		template <class DEQUEUE, class DEQUEUE_OVERFLOW>
		idx_t TryDequeueInternal(DEQUEUE dequeue, DEQUEUE_OVERFLOW dequeue_overflow) {
			if (track_count && count.load(std::memory_order_relaxed) == 0) {
				return 0;
			}
			auto obtained = dequeue();
			if (obtained == 0) {
				obtained = dequeue_overflow();
			}
			if (track_count && obtained > 0) {
				count.fetch_sub(obtained, std::memory_order_relaxed);
			}
			return obtained;
		}
		template <class ITERATOR>
		bool EnqueueOverflow(ITERATOR begin, idx_t size) {
			if (!overflow) {
				return false;
			}
			lock_guard<mutex> guard(overflow_lock);
			const auto current = overflow_count.load(std::memory_order_relaxed);
			if (size > overflow->size() - current) {
				return false;
			}
			for (idx_t i = 0; i < size; i++) {
				(*overflow)[current + i] = begin[i];
			}
			overflow_count.store(current + size, std::memory_order_relaxed);
			return true;
		}
		template <class ITERATOR>
		idx_t DequeueOverflow(ITERATOR begin, idx_t size) {
			if (overflow_count.load(std::memory_order_relaxed) == 0) {
				return 0;
			}
			lock_guard<mutex> guard(overflow_lock);
			const auto current = overflow_count.load(std::memory_order_relaxed);
			const auto obtained = MinValue(size, current);
			for (idx_t i = 0; i < obtained; i++) {
				begin[i] = (*overflow)[current - obtained + i];
			}
			overflow_count.store(current - obtained, std::memory_order_relaxed);
			return obtained;
		}

		QUEUE q;
		mutex overflow_lock;
		unique_ptr<vector<T>> overflow;
		atomic<idx_t> overflow_count {0};
		alignas(64) atomic<idx_t> count {0};
	};

public:
	explicit ShardedAllocatorQueue(idx_t count, bool track_count = false) : queues(count) {
		D_ASSERT(count > 0);
		for (auto &queue : queues) {
			queue.track_count = track_count;
		}
	}
	idx_t Count() const {
		return queues.size();
	}
	Shard &Get(idx_t shard) {
		return queues[shard];
	}
	idx_t SizeApprox() const {
		idx_t count = 0;
		for (auto &queue : queues) {
			count += queue.SizeApprox();
		}
		return count;
	}

private:
	vector<Shard> queues;
};

} // namespace duckdb
