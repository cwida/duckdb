//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/mutex.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#ifdef __MVS__
#include <time.h>
#endif
#include "duckdb/common/thread_annotation.hpp"
#include "duckdb/common/thread_annotation/mutex.hpp"

#include <mutex>

namespace duckdb {
// Annotated mutex implementation.
class DUCKDB_CAPABILITY("mutex") annotated_mutex : public internal::mutex_impl_t<annotated_mutex> {
private:
	using Impl = internal::mutex_impl_t<annotated_mutex>;

public:
	void lock() DUCKDB_ACQUIRE() {
		Impl::lock();
	}
	void unlock() DUCKDB_RELEASE() {
		Impl::unlock();
	}
	bool try_lock() DUCKDB_TRY_ACQUIRE(true) {
		return Impl::try_lock();
	}
};

// Annotated shared_mutex implementation.
class DUCKDB_CAPABILITY("mutex") annotated_shared_mutex : public internal::mutex_impl_t<annotated_shared_mutex> {
private:
	using Impl = internal::mutex_impl_t<annotated_shared_mutex>;

public:
	void lock() DUCKDB_ACQUIRE() {
		Impl::lock();
	}
	void unlock() DUCKDB_RELEASE() {
		Impl::unlock();
	}
	bool try_lock() DUCKDB_TRY_ACQUIRE(true) {
		return Impl::try_lock();
	}
	void lock_shared() DUCKDB_ACQUIRE_SHARED() {
		Impl::lock_shared();
	}
	void unlock_shared() DUCKDB_RELEASE_SHARED() {
		Impl::unlock_shared();
	}
	bool try_lock_shared() DUCKDB_TRY_ACQUIRE_SHARED(true) {
		return Impl::try_lock_shared();
	}
};

// Annotated lock_guard implementation.
template <typename M>
class DUCKDB_SCOPED_CAPABILITY annotated_lock_guard : public internal::lock_impl_t<annotated_lock_guard<M>> {
private:
	using Impl = internal::lock_impl_t<annotated_lock_guard<M>>;

public:
	explicit annotated_lock_guard(M &m) DUCKDB_ACQUIRE(m) : Impl(m) {
	}
	annotated_lock_guard(M &m, std::adopt_lock_t t) DUCKDB_REQUIRES(m) : Impl(m, t) {
	}

	// Disable copy and enable move.
	annotated_lock_guard(const annotated_lock_guard &) = delete;
	annotated_lock_guard &operator=(const annotated_lock_guard &) = delete;
	annotated_lock_guard(annotated_lock_guard &&) = default;
	annotated_lock_guard &operator=(annotated_lock_guard &&) = default;

	~annotated_lock_guard() DUCKDB_RELEASE() = default;
};

// Annotated unique_lock implementation.
template <typename M>
class DUCKDB_SCOPED_CAPABILITY annotated_unique_lock : public internal::lock_impl_t<annotated_unique_lock<M>> {
private:
	using Impl = internal::lock_impl_t<annotated_unique_lock<M>>;

public:
	annotated_unique_lock() = default;
	explicit annotated_unique_lock(M &m) DUCKDB_ACQUIRE(m) : Impl(m) {
	}
	annotated_unique_lock(M &m, std::defer_lock_t t) noexcept DUCKDB_EXCLUDES(m) : Impl(m, t) {
	}
	annotated_unique_lock(M &m, std::try_to_lock_t t) DUCKDB_TRY_ACQUIRE(true, m) : Impl(m, t) {
	}
	annotated_unique_lock(M &m, std::adopt_lock_t t) DUCKDB_REQUIRES(m) : Impl(m, t) {
	}

	// Disable copy and enable move.
	annotated_unique_lock(const annotated_unique_lock &) = delete;
	annotated_unique_lock &operator=(const annotated_unique_lock &) = delete;
	annotated_unique_lock(annotated_unique_lock &&) = default;
	annotated_unique_lock &operator=(annotated_unique_lock &&) = default;

	~annotated_unique_lock() DUCKDB_RELEASE() = default;

	void lock() DUCKDB_ACQUIRE() {
		Impl::lock();
	}
	bool try_lock() DUCKDB_TRY_ACQUIRE(true) {
		return Impl::try_lock();
	}
	template <typename R, typename P>
	bool try_lock_for(const std::chrono::duration<R, P> &timeout) DUCKDB_TRY_ACQUIRE(true) {
		return Impl::try_lock_for(timeout);
	}
	template <typename C, typename D>
	bool try_lock_until(const std::chrono::time_point<C, D> &timeout) DUCKDB_TRY_ACQUIRE(true) {
		return Impl::try_lock_until(timeout);
	}
	void unlock() DUCKDB_RELEASE() {
		Impl::unlock();
	}
};

// Annotated shared_lock implementation.
template <typename M>
class DUCKDB_SCOPED_CAPABILITY annotated_shared_lock : public internal::lock_impl_t<annotated_shared_lock<M>> {
private:
	using Impl = internal::lock_impl_t<annotated_shared_lock<M>>;

public:
	annotated_shared_lock() = default;
	explicit annotated_shared_lock(M &m) DUCKDB_ACQUIRE_SHARED(m) : Impl(m) {
	}
	annotated_shared_lock(M &m, std::defer_lock_t t) noexcept DUCKDB_EXCLUDES(m) : Impl(m, t) {
	}
	annotated_shared_lock(M &m, std::try_to_lock_t t) DUCKDB_TRY_ACQUIRE_SHARED(true, m) : Impl(m, t) {
	}
	annotated_shared_lock(M &m, std::adopt_lock_t t) DUCKDB_REQUIRES_SHARED(m) : Impl(m, t) {
	}

	// Disable copy and enable move.
	annotated_shared_lock(const annotated_shared_lock &) = delete;
	annotated_shared_lock &operator=(const annotated_shared_lock &) = delete;
	annotated_shared_lock(annotated_shared_lock &&) = default;
	annotated_shared_lock &operator=(annotated_shared_lock &&) = default;

	~annotated_shared_lock() DUCKDB_RELEASE() = default;

	void lock() DUCKDB_ACQUIRE_SHARED() {
		Impl::lock();
	}
	bool try_lock() DUCKDB_TRY_ACQUIRE_SHARED(true) {
		return Impl::try_lock();
	}
	template <typename R, typename P>
	bool try_lock_for(const std::chrono::duration<R, P> &timeout) DUCKDB_TRY_ACQUIRE_SHARED(true) {
		return Impl::try_lock_for(timeout);
	}
	template <typename C, typename D>
	bool try_lock_until(const std::chrono::time_point<C, D> &timeout) DUCKDB_TRY_ACQUIRE_SHARED(true) {
		return Impl::try_lock_until(timeout);
	}
	void unlock() DUCKDB_RELEASE() {
		Impl::unlock();
	}
};

// Unannotated mutex type, which is alias for STL ones.
using mutex = std::mutex;
template <typename M = std::mutex>
using lock_guard = std::lock_guard<M>;
template <typename M = std::mutex>
using unique_lock = std::unique_lock<M>;

} // namespace duckdb
