#pragma once

#include "duckdb/common/shared_ptr_ipp.hpp"

namespace duckdb {

template <typename T, bool SAFE>
class weak_ptr { // NOLINT: invalid case style
public:
	using original = std::weak_ptr<T>;
	using element_type = typename original::element_type;
#if defined(_LIBCPP_VERSION)
	// libc++ relocates elements of this type with memcpy when the typedef names the class itself
	using __trivially_relocatable = weak_ptr; // NOLINT: libc++ extension point
#endif

private:
	template <class U, bool SAFE_P>
	friend class shared_ptr;

private:
	original internal;

public:
	// Constructors
	weak_ptr() : internal() {
	}

	// NOLINTBEGIN
	template <class U, typename std::enable_if<compatible_with_t<U, T>::value, int>::type = 0>
	weak_ptr(shared_ptr<U, SAFE> const &ptr) noexcept : internal(ptr.internal) {
	}
	weak_ptr(weak_ptr const &other) noexcept : internal(other.internal) {
	}
	template <class U, typename std::enable_if<compatible_with_t<U, T>::value, int>::type = 0>
	weak_ptr(weak_ptr<U> const &ptr) noexcept : internal(ptr.internal) {
	}
#ifdef DUCKDB_CLANG_TIDY
	[[clang::reinitializes]]
#endif
	weak_ptr(weak_ptr &&ptr) noexcept
	    : internal(std::move(ptr.internal)) {
	}
	template <class U, typename std::enable_if<compatible_with_t<U, T>::value, int>::type = 0>
#ifdef DUCKDB_CLANG_TIDY
	[[clang::reinitializes]]
#endif
	weak_ptr(weak_ptr<U> &&ptr) noexcept
	    : internal(std::move(ptr.internal)) {
	}
	// NOLINTEND
	// Destructor
	~weak_ptr() = default;

	// Assignment operators
	weak_ptr &operator=(const weak_ptr &other) {
		if (this == &other) {
			return *this;
		}
		internal = other.internal;
		return *this;
	}

	template <class U, typename std::enable_if<compatible_with_t<U, T>::value, int>::type = 0>
	weak_ptr &operator=(const shared_ptr<U, SAFE> &ptr) {
		internal = ptr.internal;
		return *this;
	}

	// Modifiers
#ifdef DUCKDB_CLANG_TIDY
	// This is necessary to tell clang-tidy that it reinitializes the variable after a move
	[[clang::reinitializes]]
#endif
	void
	reset() { // NOLINT: invalid case style
		internal.reset();
	}

	// Observers
	long use_count() const { // NOLINT: invalid case style
		return internal.use_count();
	}

	bool expired() const { // NOLINT: invalid case style
		return internal.expired();
	}

	shared_ptr<T, SAFE> lock() const { // NOLINT: invalid case style
		return shared_ptr<T, SAFE>(internal.lock());
	}

	// Relational operators
	template <typename U>
	bool operator==(const weak_ptr<U> &other) const noexcept {
		return internal == other.internal;
	}

	template <typename U>
	bool operator!=(const weak_ptr<U> &other) const noexcept {
		return internal != other.internal;
	}

	template <typename U>
	bool operator<(const weak_ptr<U> &other) const noexcept {
		return internal < other.internal;
	}

	template <typename U>
	bool operator<=(const weak_ptr<U> &other) const noexcept {
		return internal <= other.internal;
	}

	template <typename U>
	bool operator>(const weak_ptr<U> &other) const noexcept {
		return internal > other.internal;
	}

	template <typename U>
	bool operator>=(const weak_ptr<U> &other) const noexcept {
		return internal >= other.internal;
	}
};

template <typename T>
using unsafe_weak_ptr = weak_ptr<T, false>;

} // namespace duckdb
