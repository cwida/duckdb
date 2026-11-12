//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/cow_ptr.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/unique_ptr.hpp"

namespace duckdb {

//! A copy-on-write pointer: copies of a cow_ptr share the object, reads are const and writes go through write().
//! T must provide a Copy() method that returns a unique_ptr to (a base class of) T.
template <class T>
class cow_ptr { // NOLINT: mimic std casing
public:
	cow_ptr() noexcept {
	}
	cow_ptr(std::nullptr_t) noexcept { // NOLINT: allow implicit creation from nullptr
	}
	//! Takes sole ownership of the object, so no mutable alias exists outside of the cow_ptr
	template <class U, class DELETER, bool SAFE_P,
	          typename std::enable_if<std::is_convertible<U *, T *>::value, int>::type = 0>
	cow_ptr(unique_ptr<U, DELETER, SAFE_P> &&other) // NOLINT: allow implicit creation from unique pointer
	    : ptr(std::move(other)) {
	}

	//! Constructs the object in place - U is the (derived) type to construct
	template <class U = T, class... ARGS>
	static cow_ptr make(ARGS &&... args) { // NOLINT: mimic std casing
		cow_ptr result;
		result.ptr = make_shared_ptr<U>(std::forward<ARGS>(args)...);
		return result;
	}

	//! Adopts the object owned by a unique_ptr
	template <class U, class DELETER, bool SAFE_P>
	static cow_ptr from_unique(unique_ptr<U, DELETER, SAFE_P> &&other) { // NOLINT: mimic std casing
		return cow_ptr(std::move(other));
	}
	//! Shares the object of a shared_ptr - the object is copied on write for as long as the shared_ptr holds on to it
	template <class U>
	static cow_ptr from_shared(shared_ptr<U> other) { // NOLINT: mimic std casing
		cow_ptr result;
		result.ptr = std::move(other);
		return result;
	}

	// copying shares the underlying object
	cow_ptr(const cow_ptr &other) = default;
	cow_ptr &operator=(const cow_ptr &other) = default;
	cow_ptr(cow_ptr &&other) noexcept = default;
	cow_ptr &operator=(cow_ptr &&other) noexcept = default;

public:
	const T *operator->() const {
		return ptr.operator->();
	}
	const T &operator*() const {
		return *ptr;
	}
	//! Read access never copies
	const T &read() const { // NOLINT: mimic std casing
		return *ptr;
	}
	const T *get() const { // NOLINT: mimic std casing
		return ptr.get();
	}
	operator optional_ptr<const T>() const { // NOLINT: allow implicit conversion to a read-only optional_ptr
		return optional_ptr<const T>(ptr.get());
	}
	explicit operator bool() const {
		return ptr.operator bool();
	}
	bool operator==(std::nullptr_t) const {
		return !ptr;
	}
	bool operator!=(std::nullptr_t) const {
		return ptr.operator bool();
	}
	void reset() { // NOLINT: mimic std casing
		ptr.reset();
	}

	//! Write access: returns a mutable reference to the object, copying it first if it is shared with another cow_ptr.
	//! Requires exclusive access to this cow_ptr: no other thread may use it or hold a reference obtained from it.
	T &write() { // NOLINT: mimic std casing
		if (!ptr) {
			throw InternalException("Attempting to write to a cow_ptr that is not set");
		}
		detach();
		return *ptr;
	}
	//! Mutable counterpart of get(): same as write(), but returns a pointer that is null if the cow_ptr is not set
	T *get_mut() { // NOLINT: mimic std casing
		detach();
		return ptr.get();
	}

	//! Whether both pointers refer to the same object
	bool shares_with(const cow_ptr &other) const { // NOLINT: mimic std casing
		return ptr == other.ptr;
	}

private:
	//! Stop sharing the object with other cow_ptrs by replacing it with a copy
	void detach() { // NOLINT: mimic std casing
		if (ptr && ptr.use_count() > 1) {
			ptr = FromCopy(ptr->Copy());
		}
	}
	template <class SRC>
	static shared_ptr<T> FromCopy(unique_ptr<SRC> copy) {
		return shared_ptr<T>(unique_ptr_cast<SRC, T>(std::move(copy)));
	}

private:
	shared_ptr<T> ptr;
};

} // namespace duckdb
