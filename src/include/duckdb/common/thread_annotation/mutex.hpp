//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/thread_annotation/mutex.hpp
//
//
//===----------------------------------------------------------------------===//

// Bind annotated mutex and lock type with standard implementation, so that
// duckdb::annotated_unique_lock<duckdb::annotated_mutex> could inherit std::unique_lock<std::mutex>.

#pragma once

#include <mutex>
#include <shared_mutex>

namespace duckdb {

// Forward declaration for annotated mutex types.
class annotated_mutex;
class annotated_shared_mutex;

// Forward declaration for annotated lock types.
template <typename M>
class annotated_unique_lock;
template <typename M>
class annotated_lock_guard;
template <typename M>
class annotated_shared_lock;

namespace internal {

// Type alias for mutex types.
template <typename T>
struct standard_impl {
	using type = T;
};
template <typename T>
using standard_impl_t = typename standard_impl<T>::type;

// Specialization for `std::mutex`.
template <>
struct standard_impl<::duckdb::annotated_mutex> {
	using type = std::mutex;
};

// Specialization for `std::shared_mutex`.
template <>
struct standard_impl<::duckdb::annotated_shared_mutex> {
	using type = std::shared_mutex;
};

// Type alias for lock types.
template <typename M>
using mutex_impl_t = standard_impl_t<M>;

// Specialization for `std::unique_lock`.
template <typename M>
struct standard_impl<::duckdb::annotated_unique_lock<M>> {
	using type = std::unique_lock<mutex_impl_t<M>>;
};

// Specialization for `std::lock_guard`.
template <typename M>
struct standard_impl<::duckdb::annotated_lock_guard<M>> {
	using type = std::lock_guard<mutex_impl_t<M>>;
};

// Specialization for `std::shared_lock`.
template <typename M>
struct standard_impl<::duckdb::annotated_shared_lock<M>> {
	using type = std::shared_lock<mutex_impl_t<M>>;
};

template <typename L>
using lock_impl_t = standard_impl_t<L>;

} // namespace internal
} // namespace duckdb
